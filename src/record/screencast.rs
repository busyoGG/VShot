// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Recording one window through the compositor's own screen-cast service:
//! `org.gnome.Mutter.ScreenCast`.
//!
//! The wlroots route ([`super::window`]) asks the compositor for a window's
//! pixels through `ext_image_copy_capture_v1` with a source built from the
//! window's `ext_foreign_toplevel_handle_v1`.  A compositor that implements
//! neither — niri, whose capture support stops at outputs — has no such
//! protocol, and the recording fails at the connection with nothing to fall
//! back on.  What niri *does* have is the screen-cast service GNOME's own
//! portal drives: `org.gnome.Mutter.ScreenCast`, a session-bus object that
//! takes a window id and hands back a PipeWire stream of that window.
//!
//! The route is aimed, not picked.  The portal's screen cast is the
//! compositor's choice — the user answers a picker and what comes back is
//! whatever they chose — but `RecordWindow` takes the window's own id, so a
//! recording here is of the window the command named, exactly as on the
//! wlroots route.  That is why this is a fallback that happens on its own
//! rather than a mode the user asks for: it is the same recording through
//! another door, not a different kind of one.
//!
//! Three things about the service shape the code:
//!
//! * **The window id comes from the toplevel list.**  `RecordWindow` takes a
//!   number, and the only number both sides agree on is the compositor's own
//!   window id.  `ext_foreign_toplevel_handle_v1`'s identifier is that id in
//!   decimal for a compositor that speaks both (niri documents the encoding as
//!   reversible for exactly this reason), so the window is resolved against
//!   the toplevel list and its identifier is the id.
//!
//! * **The frames come from the session's own PipeWire graph.**  Unlike the
//!   portal, there is no `OpenPipeWireRemote` to hand over a descriptor: the
//!   node the service names is on the session's PipeWire daemon, and the
//!   client connects to that daemon itself.
//!
//! * **There is no picker, so the stream is the only thing to wait for.**
//!   `Start` is answered at once and the node id arrives as a signal, which is
//!   why the listener for it is registered before the call is made — and why
//!   the wait for it is bounded, because a compositor that has refused says
//!   nothing at all.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::Arc;
use std::time::Duration;

use zbus::blocking::{Connection, MessageIterator};
use zbus::message::Type as MessageType;
use zbus::zvariant::{DynamicType, OwnedObjectPath, Value};
use zbus::{MatchRule, Message};

use crate::capture::window_copy::{Name, WindowCapture};
use crate::error::{Result, VshotError};

use super::avcodec::{EncoderBackend, Recorder};
use super::cast::{self, Shape};
use super::pipewire;
use super::{debug_enabled, prepare_output_path, RecordRequest, WindowTarget};

/// The service, the object every session hangs off, and the two interfaces.
const SERVICE: &str = "org.gnome.Mutter.ScreenCast";
const ROOT: &str = "/org/gnome/Mutter/ScreenCast";
const SESSION_INTERFACE: &str = "org.gnome.Mutter.ScreenCast.Session";
const STREAM_INTERFACE: &str = "org.gnome.Mutter.ScreenCast.Stream";

/// `cursor-mode`: whether the pointer is drawn into the stream.  `Metadata`
/// exists in the interface, but the two modes that put the cursor in the
/// frames (what `--cursor` means for a recording) are the ones asked for.
const CURSOR_HIDDEN: u32 = 0;
const CURSOR_EMBEDDED: u32 = 1;

/// How long the service may take to name the PipeWire node after `Start`.  The
/// node is made by the compositor's own event loop, so this is a round trip
/// through it rather than a call that can wait on a person; a compositor that
/// refused the window (it closed in the moment between being listed and being
/// asked for) never answers, and that is what this bound is for.
const STREAM_TIMEOUT: Duration = Duration::from_secs(10);

/// Whether this session has the compositor's own screen-cast service.
///
/// This is the test that decides whether a window recording with no wlroots
/// protocols has anywhere else to go, so it is a question about the bus rather
/// than about the recording: a session where the name is unowned is a session
/// this route cannot serve at all.
pub(super) fn available() -> bool {
    let Ok(connection) = Connection::session() else {
        return false;
    };
    connection
        .call_method(
            Some("org.freedesktop.DBus"),
            "/org/freedesktop/DBus",
            Some("org.freedesktop.DBus"),
            "NameHasOwner",
            &(SERVICE,),
        )
        .ok()
        .and_then(|reply| reply.body().deserialize::<bool>().ok())
        .unwrap_or(false)
}

/// Runs a window recording through the compositor's own screen-cast service.
pub(super) fn run_window(request: &RecordRequest, target: &WindowTarget) -> Result<PathBuf> {
    if !request.follow.is_empty() {
        return Err(VshotError::Recording(
            "`--follow` needs a capture session that can be pointed at another window while it \
             runs, and this session's window capture cannot be (it is the compositor's own \
             screen-cast service, which casts the window it was started on). Record one window \
             without `--follow`"
                .into(),
        ));
    }

    // --- the cast, and the stream it produces ------------------------------
    let backend = request.encoder_backend.resolve();
    let mut window = WindowCast::open(target, request.cursor, request.fps, allow_dmabuf(backend))?;
    let geometry = window.geometry;
    let fourcc = cast::fourcc_for(geometry.spa_format)?;
    let shape = Shape::of(&geometry);

    // --- the soundtrack ----------------------------------------------------
    // `--mic` and `--app-audio` are independent and may both be given: the
    // microphone is the room, the application audio is the window's own sound,
    // and a person wants to hear both at once, so they are summed into the one
    // track the file has.
    let app_audio = if request.app_audio {
        super::open_app_audio(&window.name)?
    } else {
        None
    };
    let mut mic = super::open_soundtrack_with(request.mic.as_ref(), app_audio)?;
    let mic_format = mic.format();

    // --- the encoder and muxer ---------------------------------------------
    let path = prepare_output_path(request)?;
    let mut recorder = match (shape, mic_format) {
        (Shape::Dmabuf, Some(format)) => Recorder::start_dmabuf_mic(
            &path,
            geometry.width,
            geometry.height,
            request.encoder,
            fourcc,
            format,
            backend,
        )?,
        (Shape::Dmabuf, None) => Recorder::start_dmabuf(
            &path,
            geometry.width,
            geometry.height,
            request.encoder,
            fourcc,
            backend,
        )?,
        (Shape::Software, Some(format)) => Recorder::start_mic(
            &path,
            geometry.width,
            geometry.height,
            request.encoder,
            format,
            backend,
        )?,
        (Shape::Software, None) => Recorder::start(
            &path,
            geometry.width,
            geometry.height,
            request.encoder,
            backend,
        )?,
    };
    if debug_enabled() {
        eprintln!(
            "vshot: recording {}x{} at up to {} fps with {} ({}) through libavcodec {} and \
             libavformat's MP4 muxer",
            geometry.width,
            geometry.height,
            request.fps,
            request.encoder.word(),
            backend.word(),
            super::avcodec::libavcodec_version()
        );
        if recorder.audio_channels() > 0 {
            eprintln!(
                "vshot: soundtrack: {} Hz, {} channel(s), AAC",
                recorder.audio_rate(),
                recorder.audio_channels()
            );
        }
    }

    let interrupted = super::install_stop_handler()?;
    super::write_pid_file()?;
    // The streams have been running through the setup; arming keeps the samples
    // from here on, so the soundtrack starts where the recording does.
    mic.arm();
    let ended = window.ended_flag();
    let outcome = cast::loop_over(
        &mut window.stream,
        &mut recorder,
        request.duration,
        shape,
        geometry,
        &mut mic,
        &interrupted,
        // A window that is resized keeps recording: its new frames are fitted
        // into the canvas the file was opened with, because the window was on
        // screen the whole time.
        true,
        // The one thing to serve at a frame boundary here: the compositor
        // saying the cast is over, which is what a window being closed looks
        // like from this side — no more frames, and no error either.
        move |_sink| {
            if ended.load(Ordering::Relaxed) {
                eprintln!(
                    "vshot: the recording ended: the compositor stopped casting the window; it \
                     may have been closed"
                );
                return Ok(true);
            }
            Ok(false)
        },
    );
    let _ = std::fs::remove_file(super::pid_file());
    window.stop();

    // The trailer goes in even when the loop failed: a file that exists is
    // finished properly, and the failure is reported next to it.
    let finish = recorder.finish();
    if let Err(error) = finish {
        if outcome.is_ok() {
            return Err(error);
        }
        eprintln!("vshot: the recording could not be finished: {error}");
    }
    super::report_outcome(
        outcome.map(|_| (recorder.frames() as usize, recorder.seconds())),
        &path,
    )
}

/// The id the service knows a toplevel by: its foreign-toplevel identifier,
/// which a compositor speaking both protocols sets to the decimal window id it
/// also uses over IPC.  A compositor that puts something else there is refused
/// by name rather than by a cast that silently records nothing.
fn window_id(name: &Name) -> Result<u64> {
    name.identifier.parse::<u64>().map_err(|_| {
        VshotError::Recording(format!(
            "the window `{}` is listed as `{}`, which is not the numeric id this session's \
             screen-cast service takes",
            name.label(),
            name.identifier
        ))
    })
}

/// Whether the cast should be asked for dma-buf frames.
///
/// A dma-buf cast only works where the encoder can import it: VAAPI can, NVENC
/// cannot, so on NVENC the cast is asked for memory frames from the start.
/// `VSHOT_PORTAL_SHM` is the manual form of the same request — the variable
/// keeps its historical name, and asks any screen-cast route for the copy
/// path.
pub(super) fn allow_dmabuf(backend: EncoderBackend) -> bool {
    backend != EncoderBackend::Nvenc && std::env::var_os("VSHOT_PORTAL_SHM").is_none()
}

/// A cast of one window: the service session driving it, the PipeWire stream
/// it produces, and the shape that stream settled on.
///
/// Shared by the recording and the replay, which differ only in what they do
/// with the frames — one encodes them into a file, the other into a ring.
pub(super) struct WindowCast {
    /// The frames.  The loops read from it; the buffer goes back to PipeWire
    /// on each `next`, so only one frame is ever in hand.
    ///
    /// Declared **before** `cast` on purpose: a value's fields are dropped in
    /// declaration order, and the stream has to go back before the session
    /// does.  See [`Self::stop`].
    pub(super) stream: pipewire::Stream,
    /// What the stream settled on, which is the canvas the file or the ring
    /// has to be opened for.
    pub(super) geometry: pipewire::Geometry,
    /// The window's own name, which a `--app-audio` session needs to find the
    /// application's pid.
    pub(super) name: Name,
    cast: ScreenCast,
}

impl WindowCast {
    /// Resolves the window, starts the cast, and connects to its stream.
    pub(super) fn open(
        target: &WindowTarget,
        cursor: bool,
        fps: u32,
        allow_dmabuf: bool,
    ) -> Result<Self> {
        let mut listing = WindowCapture::connect_listing()?;
        let toplevels = listing.toplevels()?;
        if toplevels.is_empty() {
            return Err(VshotError::Recording(
                "the compositor lists no windows to record".into(),
            ));
        }
        let names: Vec<&Name> = toplevels.iter().map(|toplevel| &toplevel.name).collect();
        let index = super::window::resolve_window(target, &names)?;
        let toplevel = &toplevels[index];
        let window_id = window_id(&toplevel.name)?;
        let name = toplevel.name.clone();
        if debug_enabled() {
            eprintln!(
                "vshot: the window `{}` (app_id `{}`, title `{}`) is cast as id {window_id}",
                toplevel.label(),
                name.app_id,
                name.title
            );
        }

        let mut cast = ScreenCast::connect()?;
        cast.record_window(window_id, cursor)?;
        let node = cast.start()?;
        // The node is on the session's PipeWire daemon, so the client connects
        // to that daemon itself rather than being handed a descriptor to it —
        // the negative descriptor is what asks the PipeWire client for that.
        let stream =
            pipewire::Stream::open(-1, node, allow_dmabuf, fps, cast::NEGOTIATION_TIMEOUT)?;
        let geometry = stream.geometry()?;
        if debug_enabled() {
            eprintln!(
                "vshot: the compositor is casting {}x{} (spa format {}, {} frames)",
                geometry.width,
                geometry.height,
                geometry.spa_format,
                Shape::of(&geometry).word()
            );
        }
        Ok(Self {
            cast,
            stream,
            geometry,
            name,
        })
    }

    /// Whether the compositor has said the cast is over — the window it was
    /// casting is gone.  Handed to the frame loop, which polls it at every
    /// frame boundary, because a cast whose window closed sends no more frames
    /// and no error either.
    pub(super) fn ended_flag(&self) -> Arc<AtomicBool> {
        self.cast.ended_flag()
    }

    /// Ends the cast: the stream goes back first, then the session is stopped.
    ///
    /// The order is the whole point, and getting it wrong is a segfault rather
    /// than a leak.  The client still holds the last frame when the recording
    /// ends, and returning that buffer to PipeWire is what closing the stream
    /// does — but only while the node it came from still exists.  Stopping the
    /// cast first destroys that node, and the buffer that goes back afterwards
    /// is memory the compositor has already freed: measured, `pw_stream_queue_buffer`
    /// on a stale `pw_buffer` in `vshot_pw_close`.
    ///
    /// The session is stopped explicitly rather than left to the connection's
    /// drop, because the compositor keeps casting until it is told to stop.
    pub(super) fn stop(self) {
        let WindowCast {
            stream,
            cast,
            geometry: _,
            name: _,
        } = self;
        drop(stream);
        cast.stop();
    }
}

/// One listener for a signal on an object, with its match rule registered on
/// the bus before the iterator is returned.  `None` is a rule or a
/// subscription that could not be made, already reported through `ready`.
fn listen(
    connection: &Connection,
    interface: &str,
    member: &str,
    path: &str,
    ready: &mpsc::Sender<std::result::Result<(), String>>,
) -> Option<MessageIterator> {
    let rule = match MatchRule::builder()
        .msg_type(MessageType::Signal)
        .interface(interface)
        .and_then(|rule| rule.member(member))
        .and_then(|rule| rule.path_namespace(path))
        .map(|rule| rule.build())
    {
        Ok(rule) => rule,
        Err(error) => {
            let _ = ready.send(Err(error.to_string()));
            return None;
        }
    };
    match MessageIterator::for_match_rule(rule, connection, Some(4)) {
        Ok(iterator) => Some(iterator),
        Err(error) => {
            let _ = ready.send(Err(error.to_string()));
            None
        }
    }
}

/// The compositor's screen-cast service, as this module drives it.
///
/// The protocol is `CreateSession`, then `RecordWindow` on the session, then
/// `Start`; the node the frames come from arrives as a `PipeWireStreamAdded`
/// signal on the stream object, which is why the listener for it is registered
/// before `Start` is called rather than after.  The cast's *end* arrives as a
/// `Closed` signal on the session object, and that one matters just as much:
/// when the window being cast is closed, the compositor destroys its PipeWire
/// stream and the frames simply stop — nothing in the stream says it is over,
/// so the signal is the only word the recording gets.
struct ScreenCast {
    connection: Connection,
    session: OwnedObjectPath,
    /// The object the node id will be announced on.  The listener itself is
    /// made in [`Self::start`], in the thread that waits on it.
    stream: OwnedObjectPath,
    /// Set when the compositor says the cast is over.  Shared with the frame
    /// loop, which checks it at every frame boundary.
    ended: Arc<AtomicBool>,
}

impl ScreenCast {
    /// `CreateSession`: the service's handle for one screen-cast session.
    fn connect() -> Result<Self> {
        let connection = Connection::session().map_err(|error| {
            VshotError::Recording(format!(
                "this session's window capture runs over the compositor's own screen-cast \
                 service, and the session bus could not be reached: {error}"
            ))
        })?;
        // An empty set of properties: a cast is not a remote desktop session,
        // and the service refuses one that claims to be.
        let properties: HashMap<&str, Value<'_>> = HashMap::new();
        let reply = connection
            .call_method(
                Some(SERVICE),
                ROOT,
                Some(SERVICE),
                "CreateSession",
                &(properties,),
            )
            .map_err(|error| {
                VshotError::Recording(format!(
                    "the compositor's screen-cast service could not start a session \
                     (`{SERVICE}.CreateSession`): {error}"
                ))
            })?;
        let session: OwnedObjectPath = reply.body().deserialize().map_err(|error| {
            VshotError::Recording(format!(
                "the compositor's screen-cast service started a session without naming it: \
                 {error}"
            ))
        })?;
        // A placeholder the first `RecordWindow` replaces; a session with no
        // stream is never started.
        let stream = session.clone();
        Ok(Self {
            connection,
            session,
            stream,
            ended: Arc::new(AtomicBool::new(false)),
        })
    }

    /// Whether the compositor has said the cast is over.  The frame loop polls
    /// it at every frame boundary: a cast whose window was closed sends no
    /// more frames and no error either, so this is what ends the recording
    /// there rather than leaving it to freeze on its last frame.
    fn ended_flag(&self) -> Arc<AtomicBool> {
        Arc::clone(&self.ended)
    }

    /// `RecordWindow`: one stream of one window, by the id the compositor's own
    /// window list gives it.
    fn record_window(&mut self, window_id: u64, cursor: bool) -> Result<()> {
        let mut properties: HashMap<&str, Value<'_>> = HashMap::new();
        properties.insert("window-id", Value::from(window_id));
        properties.insert(
            "cursor-mode",
            Value::from(if cursor {
                CURSOR_EMBEDDED
            } else {
                CURSOR_HIDDEN
            }),
        );
        let reply = self.call("RecordWindow", &(properties,))?;
        let stream: OwnedObjectPath = reply.body().deserialize().map_err(|error| {
            VshotError::Recording(format!(
                "the compositor's screen-cast service made a stream without naming it: {error}"
            ))
        })?;
        self.stream = stream;
        Ok(())
    }

    /// `Start`, and the PipeWire node id the compositor then announces.
    ///
    /// The listener has to be in place before the call: the signal is emitted
    /// from the compositor's event loop, which `Start` only hands the request
    /// to, so a listener made afterwards would be racing it.  The wait runs on
    /// its own thread because the bus client blocks on it, and it is bounded
    /// because a compositor that refused the window answers nothing at all.
    ///
    /// The same thread then waits for the cast to end, on the session object's
    /// `Closed` signal, and records it in [`Self::ended`].  That is the only
    /// notice a recording gets that the window it was casting is gone: the
    /// compositor destroys its PipeWire stream, the frames stop, and the
    /// stream itself says nothing.
    fn start(&mut self) -> Result<u32> {
        let stream_path = self.stream.to_string();
        let session_path = self.session.to_string();
        let connection = self.connection.clone();
        let ended = Arc::clone(&self.ended);
        let (ready_tx, ready_rx) = mpsc::channel::<std::result::Result<(), String>>();
        let (node_tx, node_rx) = mpsc::channel::<std::result::Result<u32, String>>();
        std::thread::spawn(move || {
            let Some(node_iterator) = listen(
                &connection,
                STREAM_INTERFACE,
                "PipeWireStreamAdded",
                &stream_path,
                &ready_tx,
            ) else {
                return;
            };
            let Some(closed_iterator) = listen(
                &connection,
                SESSION_INTERFACE,
                "Closed",
                &session_path,
                &ready_tx,
            ) else {
                return;
            };
            // Both listeners are on the bus now, so the caller may make the
            // call that makes the compositor emit.
            if ready_tx.send(Ok(())).is_err() {
                return;
            }
            // One signal is all there is to read here: the stream object
            // carries a node id once, when the compositor creates it.
            let answer = match node_iterator.into_iter().next() {
                Some(Ok(message)) => message
                    .body()
                    .deserialize::<(u32,)>()
                    .map(|(node,)| node)
                    .map_err(|error| format!("it named no PipeWire node: {error}")),
                Some(Err(error)) => Err(error.to_string()),
                None => Err("the stream went away without naming a node".to_owned()),
            };
            let _ = node_tx.send(answer);
            // And then the end of the cast, which may be a long way off.
            if closed_iterator.into_iter().next().is_some() {
                ended.store(true, Ordering::Relaxed);
            }
        });
        match ready_rx.recv() {
            Ok(Ok(())) => {}
            Ok(Err(error)) => {
                return Err(VshotError::Recording(format!(
                    "the compositor's screen-cast stream could not be waited for: {error}"
                )))
            }
            Err(_) => {
                return Err(VshotError::Recording(
                    "the compositor's screen-cast stream could not be waited for".into(),
                ))
            }
        }

        self.call("Start", &())?;
        match node_rx.recv_timeout(STREAM_TIMEOUT) {
            Ok(Ok(node)) => Ok(node),
            Ok(Err(error)) => Err(VshotError::Recording(format!(
                "the compositor's screen-cast stream could not be read: {error}"
            ))),
            Err(_) => Err(VshotError::Recording(format!(
                "the compositor's screen-cast service accepted the recording but named no \
                 PipeWire stream within {} seconds; the window may have closed",
                STREAM_TIMEOUT.as_secs()
            ))),
        }
    }

    /// `Stop` on the session, so the compositor stops casting as soon as the
    /// recording does.  Its failure is not worth reporting: the session goes
    /// away with the connection either way.
    fn stop(&self) {
        let _ = self.call("Stop", &());
    }

    /// One call on the session object, with the bus's own words in the error
    /// message.
    fn call(&self, method: &str, body: &(impl serde::Serialize + DynamicType)) -> Result<Message> {
        self.connection
            .call_method(
                Some(SERVICE),
                self.session.as_str(),
                Some(SESSION_INTERFACE),
                method,
                body,
            )
            .map_err(|error| {
                VshotError::Recording(format!(
                    "the compositor's screen-cast service could not `{SESSION_INTERFACE}.{method}`: \
                     {error}"
                ))
            })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn named(identifier: &str) -> Name {
        Name {
            app_id: "kitty".to_owned(),
            title: "~".to_owned(),
            identifier: identifier.to_owned(),
        }
    }

    /// The id the service takes is the toplevel's identifier, which a
    /// compositor speaking both protocols writes as the decimal window id —
    /// niri documents the encoding as reversible for exactly this reason.
    #[test]
    fn the_window_id_is_the_identifiers_number() {
        assert_eq!(window_id(&named("3")).unwrap(), 3);
        assert_eq!(window_id(&named("18446744073709551615")).unwrap(), u64::MAX);
    }

    /// Anything else is refused with both the window and the identifier
    /// named, rather than being cast as some other window's pixels.
    #[test]
    fn an_identifier_that_is_not_a_number_is_refused() {
        let error = window_id(&named("a-uuid")).unwrap_err().to_string();
        assert!(error.contains("a-uuid"), "{error}");
        assert!(error.contains("kitty"), "{error}");
        assert!(window_id(&named("")).is_err());
    }

    /// NVENC has no dma-buf import, so the cast is asked for memory frames
    /// there whatever else is set.
    #[test]
    fn nvenc_is_never_asked_for_dma_buf_frames() {
        assert!(!allow_dmabuf(EncoderBackend::Nvenc));
    }

    /// Every other backend takes the zero-copy shape, unless the escape hatch
    /// asks for memory frames by hand.
    #[test]
    fn the_other_backends_take_dma_buf_frames() {
        if std::env::var_os("VSHOT_PORTAL_SHM").is_some() {
            return;
        }
        assert!(allow_dmabuf(EncoderBackend::Vaapi));
        assert!(allow_dmabuf(EncoderBackend::Auto));
    }
}

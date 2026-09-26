// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Recording through the XDG desktop portal: `vshot record --portal`.
//!
//! The compositor's own protocols — wlr-screencopy, KWin's ScreenShot2,
//! `ext_image_copy_capture_v1` — are the fast route to a screen, but each of
//! them exists on some desktops and not on others.  What every desktop does
//! have is the XDG portal: `org.freedesktop.portal.ScreenCast`, a session-bus
//! service where the *compositor* does the capturing.  That is the point of
//! the portal, and it is also its cost: the compositor shows its own picker
//! when a session asks what to record, and what comes back is one stream of
//! that screen or window rather than anything vshot chose by name.
//! `--portal` is therefore a mode with its own semantics, not a fallback that
//! happens behind the user's back:
//!
//! * **The compositor picks.**  `vshot record monitor --portal` asks for a
//!   screen, `vshot record window --portal` for a window; both then show the
//!   compositor's picker, and whatever is chosen there is what gets recorded.
//!   A name or a `--pick` still decides which kind of source the picker
//!   offers — a window, or a screen — but not which one.
//!
//! * **One stream per session.**  The portal hands over one screen-cast
//!   stream, and the portal chooses its size, so `record all --portal` has no
//!   meaning and is refused: composing every output is the compositor's own
//!   protocols' job.
//!
//! * **The frames are damage-driven.**  A screen cast produces a frame when
//!   the screen it is casting changes and nothing at all when it does not, so
//!   this loop waits for frames rather than sampling them; a still screen
//!   becomes a long-duration frame, which is exactly what was on screen.
//!   `--fps` is the rate asked of the compositor — a limit on how often it
//!   samples, not a promise that frames arrive that often.
//!
//! * **There are two frame shapes.**  A dma-buf frame goes to the GPU encoder
//!   without a copy; a memory frame is converted on the CPU.  Which one the
//!   portal produces is part of the negotiation ([`pipewire`] is the client
//!   that reads it); `VSHOT_PORTAL_SHM=1` asks for the memory shape, which is
//!   the escape hatch when a compositor's dma-buf buffers cannot be imported.
//!
//! Everything around the frame source is shared with the other recordings:
//! the output path, the stop signal, the pid file, the trailer and the final
//! report (see [`super`]).

use std::collections::HashMap;
use std::os::fd::{AsFd, IntoRawFd};
use std::path::PathBuf;
use std::time::Duration;

use zbus::blocking::{Connection, MessageIterator};
use zbus::message::Type as MessageType;
use zbus::zvariant::{Array, DynamicType, ObjectPath, OwnedFd, OwnedValue, Structure, Value};
use zbus::{MatchRule, Message};

use crate::error::{Result, VshotError};

use super::avcodec::Recorder;
use super::cast::{self, Shape};
use super::pipewire;
use super::{debug_enabled, prepare_output_path, RecordRequest, RecordTarget};

const PORTAL_SERVICE: &str = "org.freedesktop.portal.Desktop";
const PORTAL_PATH: &str = "/org/freedesktop/portal/desktop";
const SCREEN_CAST: &str = "org.freedesktop.portal.ScreenCast";
const REQUEST_INTERFACE: &str = "org.freedesktop.portal.Request";
const SESSION_INTERFACE: &str = "org.freedesktop.portal.Session";
/// Where the portal puts the request objects it answers with a signal.  The
/// listener is registered on the whole namespace once, before the first call:
/// the answer is a signal, and a signal that arrives before a listener exists
/// is a signal nobody hears.
const REQUEST_NAMESPACE: &str = "/org/freedesktop/portal/desktop/request";

/// What the portal may offer the user to share.  Monitor and window are the
/// two a recording can use; a virtual source is a compositor-defined region
/// with no defined size, which an encoder opened for one size could not take.
const SOURCE_MONITOR: u32 = 1;
const SOURCE_WINDOW: u32 = 2;

/// `org.freedesktop.portal.ScreenCast` cursor modes.  `Metadata` exists in the
/// protocol but not in every portal, so the two modes that draw the cursor
/// into the frames (what `--cursor` means for a recording) are the ones asked
/// for.
const CURSOR_HIDDEN: u32 = 1;
const CURSOR_EMBEDDED: u32 = 2;

/// Runs a portal recording to completion.
pub(super) fn run(request: &RecordRequest) -> Result<PathBuf> {
    if let RecordTarget::All = request.target {
        return Err(VshotError::Recording(
            "`--portal` records one stream, and the portal chooses which screen it is; a \
             whole-desktop recording is the compositor's own protocols' job (`vshot record all` \
             without --portal)"
                .into(),
        ));
    }
    if !pipewire::available() {
        return Err(VshotError::Recording(format!(
            "`--portal` reads its frames from the compositor's screen cast, and {}",
            pipewire::load_error()
        )));
    }

    let mut session = ScreenCast::connect()?;
    let handle = session.create_session()?;
    // The picker belongs to the compositor, so it has to be announced before
    // it appears out of nowhere.
    eprintln!(
        "vshot: the portal asks the compositor to choose what {}. Answer its picker",
        match request.target {
            RecordTarget::Window(_) => "window to record",
            _ => "screen to record",
        }
    );
    let outcome = record(&mut session, &handle, request);
    session.close_session(&handle);
    outcome
}

/// The recording itself, from `SelectSources` to the finished file.
fn record(
    session: &mut ScreenCast,
    handle: &ObjectPath<'_>,
    request: &RecordRequest,
) -> Result<PathBuf> {
    session.select_sources(handle, request)?;
    let node = session.start(handle)?;
    // The portal's own connection to the compositor; the frames come from a
    // node on it.  It stays open for as long as the stream does.
    let remote = session.open_pipewire_remote(handle)?;

    // A dma-buf cast only works where the encoder can import it: VAAPI can,
    // NVENC cannot, so on NVENC the portal is asked for memory frames from the
    // start (`VSHOT_PORTAL_SHM` is the manual form of the same request).
    let backend = request.encoder_backend.resolve();
    let allow_dmabuf = backend != crate::record::avcodec::EncoderBackend::Nvenc
        && std::env::var_os("VSHOT_PORTAL_SHM").is_none();
    // The node the portal names is registered with PipeWire by the portal's
    // own process, and for a window it can take a moment longer than the
    // answer to `Start`: the stream is created when the compositor has
    // produced the first buffer, which for a window means the copy has
    // already happened.  Connecting before it exists fails with PipeWire's
    // "no target node available", so a refusal is retried for a moment
    // before it is believed.
    //
    // Each attempt needs its own descriptor: a successful connect hands the
    // descriptor to PipeWire (the connection owns it from then on), and an
    // attempt that got as far as the connection but failed on the node takes
    // the descriptor with it.  `try_clone` is what makes the retry possible
    // without ever re-using a descriptor that may already be closed.
    //
    // Ownership passes to PipeWire, so the descriptor is released to raw form
    // here rather than closed by Rust: a close on this side would take the
    // number out from under the connection the moment the call returned.
    let mut stream = None;
    let mut last_error = None;
    for attempt in 0..25 {
        let descriptor = match remote.as_fd().try_clone_to_owned() {
            Ok(descriptor) => descriptor,
            Err(error) => {
                return Err(VshotError::Recording(format!(
                    "the portal's PipeWire connection could not be duplicated for the stream: \
                     {error}"
                )))
            }
        };
        match pipewire::Stream::open(
            descriptor.into_raw_fd(),
            node,
            allow_dmabuf,
            request.fps,
            cast::NEGOTIATION_TIMEOUT,
        ) {
            Ok(opened) => {
                stream = Some(opened);
                break;
            }
            Err(error) => {
                let text = error.to_string();
                last_error = Some(error);
                if !text.contains("no target node available") {
                    break;
                }
                if attempt == 0 && debug_enabled() {
                    eprintln!(
                        "vshot: the portal's stream node ({node}) is not in the PipeWire graph \
                         yet; waiting for it"
                    );
                }
                std::thread::sleep(Duration::from_millis(200));
            }
        }
    }
    let mut stream = stream.ok_or_else(|| {
        last_error.unwrap_or_else(|| {
            VshotError::Recording("the portal's screen-cast stream could not be opened".into())
        })
    })?;
    let geometry = stream.geometry()?;
    let fourcc = cast::fourcc_for(geometry.spa_format)?;
    let shape = Shape::of(&geometry);
    let backend = request.encoder_backend.resolve();
    if debug_enabled() {
        eprintln!(
            "vshot: the portal is casting {}x{} (spa format {}, {} frames)",
            geometry.width,
            geometry.height,
            geometry.spa_format,
            shape.word()
        );
    }

    // The microphone, when one was asked for: opened before the encoder,
    // because the AAC encoder's rate and channel count come from the
    // negotiation and the MP4's audio stream is declared in its header.  The
    // portal records a whole stream, not a window, so it has no application to
    // attach `--app-audio` to — only the microphone.
    let mut mic = super::open_soundtrack(request.mic.as_ref())?;
    let mic_format = mic.format();
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
                "vshot: microphone soundtrack: {} Hz, {} channel(s), AAC",
                recorder.audio_rate(),
                recorder.audio_channels()
            );
        }
    }

    let interrupted = super::install_stop_handler()?;
    super::write_pid_file()?;
    // The microphone has been running through the negotiation and the
    // picker; arming keeps the samples from here on, so the soundtrack
    // starts where the recording does.
    mic.arm();
    let outcome = cast::loop_over(
        &mut stream,
        &mut recorder,
        request.duration,
        shape,
        geometry,
        &mut mic,
        &interrupted,
        // A portal cast is of the screen the portal picked, so a
        // reconfiguration is a different screen rather than a resize to
        // follow: the recording ends there.
        false,
        // A recording is not a replay: a still screen stays one long frame,
        // which is both what was on screen and the cheaper answer.  Its length
        // is carried by the last frame at the end of the loop.
        None,
        // A portal recording has nothing to serve at a frame boundary: the
        // control socket belongs to a replay, which does not take this route.
        |_sink| Ok(false),
    );
    let _ = std::fs::remove_file(super::pid_file());

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

/// The portal's screen cast, as this module drives it.
///
/// The protocol is a sequence of method calls whose answers arrive as signals
/// on a request object — `CreateSession`, then `SelectSources`, then `Start` —
/// followed by a call that hands over the file descriptor the frames travel
/// on.  Each call's answer is awaited before the next is made, which is what
/// the protocol expects and what keeps the code a straight line.
struct ScreenCast {
    connection: Connection,
    /// Our unique bus name, as the request paths spell it.
    sender: String,
    /// A counter for the tokens that name each request.
    counter: u32,
    /// The listener for the answer of the call in flight.  It is registered
    /// before the call is made, because the answer is a signal.
    pending: Option<MessageIterator>,
}

impl ScreenCast {
    /// Connects to the session bus.  Nothing is negotiated here: a desktop
    /// without a portal says so at the first call, with the bus's own words.
    fn connect() -> Result<Self> {
        let connection = Connection::session().map_err(|error| {
            VshotError::Recording(format!(
                "`--portal` drives the desktop portal over the session bus, and the bus could \
                 not be reached: {error}"
            ))
        })?;
        let unique = connection.unique_name().ok_or_else(|| {
            VshotError::Recording(
                "the session bus did not give this process a name, so the portal's answers \
                 could not be told apart from anybody else's"
                    .into(),
            )
        })?;
        // A request path is `/org/freedesktop/portal/desktop/request/<sender>/
        // <token>`, and the sender in it is our unique name with the leading
        // colon dropped and its dots turned into underscores.
        let sender = unique.as_str().trim_start_matches(':').replace('.', "_");
        Ok(Self {
            connection,
            sender,
            counter: 0,
            pending: None,
        })
    }

    /// Starts listening for the answer of the call about to be made, and
    /// returns the token that call has to carry as its `handle_token`.
    fn listen(&mut self) -> Result<String> {
        self.counter += 1;
        let token = format!("vshot{}x{}", std::process::id(), self.counter);
        let namespace = format!("{REQUEST_NAMESPACE}/{}/{}", self.sender, token);
        let rule = MatchRule::builder()
            .msg_type(MessageType::Signal)
            .interface(REQUEST_INTERFACE)
            .and_then(|rule| rule.member("Response"))
            .and_then(|rule| rule.path_namespace(namespace.as_str()))
            .map(|rule| rule.build())
            .map_err(|error| {
                VshotError::Recording(format!(
                    "the portal's answer could not be waited for ({namespace}): {error}"
                ))
            })?;
        let iterator =
            MessageIterator::for_match_rule(rule, &self.connection, Some(4)).map_err(|error| {
                VshotError::Recording(format!(
                    "the portal's answer could not be waited for: {error}"
                ))
            })?;
        self.pending = Some(iterator);
        Ok(token)
    }

    /// The answer to the request the portal just returned the handle for.
    ///
    /// This is where a portal recording waits on the user: the compositor's
    /// picker is answered somewhere between `SelectSources` and its response,
    /// and there is no timeout, because a picker that is up is a user who has
    /// not decided yet.
    fn response(&mut self, handle: &ObjectPath<'_>) -> Result<HashMap<String, OwnedValue>> {
        let Some(iterator) = self.pending.take() else {
            return Err(VshotError::Recording(
                "the portal's answer was expected before it was asked for".into(),
            ));
        };
        for message in iterator {
            let message = message.map_err(|error| {
                VshotError::Recording(format!("the portal's answer could not be read: {error}"))
            })?;
            if message.header().path().map(|path| path.as_str()) != Some(handle.as_str()) {
                continue;
            }
            let (code, results): (u32, HashMap<String, OwnedValue>) =
                message.body().deserialize().map_err(|error| {
                    VshotError::Recording(format!(
                        "the portal's answer was not a response and a set of results: {error}"
                    ))
                })?;
            return match code {
                0 => Ok(results),
                1 => Err(VshotError::Recording(
                    "the screen sharing request was cancelled at the compositor's picker".into(),
                )),
                other => Err(VshotError::Recording(format!(
                    "the portal refused the screen sharing request (response code {other})"
                ))),
            };
        }
        Err(VshotError::Recording(
            "the portal closed the request without answering it".into(),
        ))
    }

    /// One ScreenCast call, with the bus's own words in the error message.
    fn call<B>(&self, what: &str, method: &str, body: &B) -> Result<Message>
    where
        B: serde::Serialize + DynamicType,
    {
        self.connection
            .call_method(
                Some(PORTAL_SERVICE),
                PORTAL_PATH,
                Some(SCREEN_CAST),
                method,
                body,
            )
            .map_err(|error| {
                VshotError::Recording(format!(
                    "the desktop portal could not {what} ({SCREEN_CAST}.{method}): {error}"
                ))
            })
    }

    /// `CreateSession`: the portal's handle for one screen-cast session.
    fn create_session(&mut self) -> Result<ObjectPath<'static>> {
        let token = self.listen()?;
        let session_token = format!("{token}s");
        let mut options: HashMap<&str, Value<'_>> = HashMap::new();
        options.insert("handle_token", Value::from(token.as_str()));
        options.insert("session_handle_token", Value::from(session_token.as_str()));
        let reply = self.call("start a session", "CreateSession", &(options,))?;
        let handle: zbus::zvariant::OwnedObjectPath = reply
            .body()
            .deserialize()
            .map_err(|error| request_handle_error(&error))?;
        let results = self.response(&handle)?;
        let session = results.get("session_handle").ok_or_else(|| {
            VshotError::Recording(
                "the portal started a session without telling us how to address it".into(),
            )
        })?;
        // The protocol calls the session handle an object path, but the
        // portal's frontend hands it back as a string inside the results
        // dictionary — verified against xdg-desktop-portal 1.20.4, whose
        // `CreateSession` answer carries `session_handle` as a `Str`.  Both
        // shapes are accepted, because a portal that follows the spec
        // literally would send the path.
        let session = match session.downcast_ref::<&ObjectPath<'_>>() {
            Ok(path) => path.to_owned(),
            Err(_) => {
                let text = session.downcast_ref::<&str>().map_err(|_| {
                    VshotError::Recording(
                        "the portal's session handle is neither a path nor a string".into(),
                    )
                })?;
                ObjectPath::try_from(text.to_string()).map_err(|error| {
                    VshotError::Recording(format!(
                        "the portal's session handle is not a valid path: {error}"
                    ))
                })?
            }
        };
        Ok(session)
    }

    /// `SelectSources`: what kind of source the compositor should offer, and
    /// whether the pointer is drawn into the frames.  This is the call the
    /// picker answers.
    fn select_sources(&mut self, session: &ObjectPath<'_>, request: &RecordRequest) -> Result<()> {
        let token = self.listen()?;
        let types = match request.target {
            RecordTarget::Window(_) => SOURCE_WINDOW,
            _ => SOURCE_MONITOR,
        };
        let mut options: HashMap<&str, Value<'_>> = HashMap::new();
        options.insert("handle_token", Value::from(token.as_str()));
        options.insert("types", Value::from(types));
        // One stream: the portal's own shape for a recording, and what the
        // loop can encode.  A multi-source session would be a stream per
        // source and a composition vshot does not do.
        options.insert("multiple", Value::from(false));
        options.insert(
            "cursor_mode",
            Value::from(if request.cursor {
                CURSOR_EMBEDDED
            } else {
                CURSOR_HIDDEN
            }),
        );
        let reply = self.call("ask what to record", "SelectSources", &(session, options))?;
        let handle: zbus::zvariant::OwnedObjectPath = reply
            .body()
            .deserialize()
            .map_err(|error| request_handle_error(&error))?;
        self.response(&handle)?;
        Ok(())
    }

    /// `Start`: the compositor begins casting, and the answer names the
    /// PipeWire node the frames come from.
    fn start(&mut self, session: &ObjectPath<'_>) -> Result<u32> {
        let token = self.listen()?;
        let mut options: HashMap<&str, Value<'_>> = HashMap::new();
        options.insert("handle_token", Value::from(token.as_str()));
        // No parent window: vshot has no window of its own on screen, and an
        // empty string is what the protocol says to pass when there is none.
        let reply = self.call("start the screen cast", "Start", &(session, "", options))?;
        let handle: zbus::zvariant::OwnedObjectPath = reply
            .body()
            .deserialize()
            .map_err(|error| request_handle_error(&error))?;
        let results = self.response(&handle)?;
        stream_node(&results)
    }

    /// `OpenPipeWireRemote`: the file descriptor of the compositor's own
    /// PipeWire connection, which the frames are read from.
    fn open_pipewire_remote(&self, session: &ObjectPath<'_>) -> Result<OwnedFd> {
        let options: HashMap<&str, Value<'_>> = HashMap::new();
        let reply = self.call(
            "open the screen cast's connection",
            "OpenPipeWireRemote",
            &(session, options),
        )?;
        reply.body().deserialize().map_err(|error| {
            VshotError::Recording(format!(
                "the portal did not hand over a file descriptor for the screen cast: {error}"
            ))
        })
    }

    /// `Close` on the session, so the compositor stops casting as soon as the
    /// recording does.  Its failure is not worth reporting: the session goes
    /// away with the connection either way.
    fn close_session(&self, session: &ObjectPath<'_>) {
        let _ = self.connection.call_method(
            Some(PORTAL_SERVICE),
            session,
            Some(SESSION_INTERFACE),
            "Close",
            &(),
        );
    }
}

/// The node id out of a `Start` answer's `streams: a(ua{sv})`.
fn stream_node(results: &HashMap<String, OwnedValue>) -> Result<u32> {
    let streams = results
        .get("streams")
        .ok_or_else(|| {
            VshotError::Recording(
                "the portal started the screen cast without naming a stream".into(),
            )
        })?
        .downcast_ref::<&Array<'_>>()
        .map_err(|_| VshotError::Recording("the portal's `streams` is not a list".into()))?;
    let first = streams.inner().first().ok_or_else(|| {
        VshotError::Recording(
            "the portal's `streams` list is empty, so there is nothing to record".into(),
        )
    })?;
    let entry = first
        .downcast_ref::<&Structure<'_>>()
        .map_err(|_| VshotError::Recording("the portal's stream entry is not a stream".into()))?;
    entry
        .fields()
        .first()
        .ok_or_else(|| VshotError::Recording("the portal's stream has no node id".into()))?
        .downcast_ref::<u32>()
        .map_err(|_| VshotError::Recording("the portal's stream node id is not a number".into()))
}

/// The message for a call that answered without a request handle.
fn request_handle_error(error: &zbus::Error) -> VshotError {
    VshotError::Recording(format!(
        "the portal answered without a request handle, so its answer could not be waited for: \
         {error}"
    ))
}

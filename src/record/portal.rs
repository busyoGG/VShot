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
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use zbus::blocking::{Connection, MessageIterator};
use zbus::message::Type as MessageType;
use zbus::zvariant::{Array, DynamicType, ObjectPath, OwnedFd, OwnedValue, Structure, Value};
use zbus::{MatchRule, Message};

use crate::error::{Result, VshotError};
use crate::geometry::Size;
use crate::model::Frame;

use super::avcodec::Recorder;
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

/// The `SPA_VIDEO_FORMAT_*` values a screen cast produces, and what vshot's
/// encoder can take.  The SPA name is the byte order in memory; `BGRx` is the
/// opaque layout a screen comes out as, `BGRA` the one a window with alpha
/// does.
const SPA_RGBX: u32 = 7;
const SPA_BGRX: u32 = 8;
const SPA_RGBA: u32 = 11;
const SPA_BGRA: u32 = 12;

/// How long the format negotiation may take before the stream is given up on.
/// The negotiation is a handful of round trips over the local socket, so this
/// only fires when something is genuinely wrong.
const NEGOTIATION_TIMEOUT: Duration = Duration::from_secs(15);

/// How long the recording waits for its first frame.  The compositor queues
/// one as soon as the session starts, so a stream that produces nothing —
/// a window on an output that is off, disabled or disconnected — is reported
/// after a few seconds instead of waited on forever.
const FIRST_FRAME_TIMEOUT: Duration = Duration::from_secs(15);

/// How long one wait for a frame lasts before the loop looks around: at the
/// stop signal and at the `--duration` deadline.  A frame that arrives inside
/// the window is used immediately.
const POLL: Duration = Duration::from_millis(200);

/// The frame shape the encoder was opened for.  The portal's frames arrive in
/// one of these and cannot change without ending the recording: one MP4 holds
/// one pixel layout, exactly as it holds one frame size.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Shape {
    /// dma-buf frames the encoder imports directly (the zero-copy path).
    Dmabuf,
    /// Memory frames converted to RGBA on the CPU (the compatibility path).
    Software,
}

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
            NEGOTIATION_TIMEOUT,
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
    let fourcc = fourcc_for(geometry.spa_format)?;
    let shape = if geometry.dmabuf {
        Shape::Dmabuf
    } else {
        Shape::Software
    };
    let backend = request.encoder_backend.resolve();
    if debug_enabled() {
        eprintln!(
            "vshot: the portal is casting {}x{} (spa format {}, {} frames)",
            geometry.width,
            geometry.height,
            geometry.spa_format,
            match shape {
                Shape::Dmabuf => "dma-buf",
                Shape::Software => "memory",
            }
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
    let outcome = loop_over(
        &mut stream,
        &mut recorder,
        request,
        shape,
        fourcc,
        geometry,
        &mut mic,
        &interrupted,
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

/// The frame loop: wait for a frame, encode it with the time the previous one
/// was on screen, wait again.
#[allow(clippy::too_many_arguments)] // the frame source's whole shape
fn loop_over(
    stream: &mut pipewire::Stream,
    recorder: &mut Recorder,
    request: &RecordRequest,
    shape: Shape,
    fourcc: u32,
    geometry: pipewire::Geometry,
    mic: &mut super::pipewire_audio::Soundtrack,
    interrupted: &AtomicBool,
) -> Result<()> {
    // The clock the recording's own length is measured against: the first
    // frame's arrival.  Everything before it is the negotiation, which the
    // user may have spent answering a picker.
    let mut started: Option<Instant> = None;
    let mut first_frame_deadline = Instant::now() + FIRST_FRAME_TIMEOUT;
    let mut last_frame_at = Instant::now();
    let mut timeline_ms = 0u64;
    let mut covered_us = 0u64;
    // What the last frame was, so the time between it and the stop signal can
    // be written as its duration once the loop is over: a frame's length is
    // only known when the next one arrives, and for the last one that is the
    // end of the recording.
    let mut tail_dmabuf: Option<pipewire::Dmabuf> = None;
    let mut tail_software: Option<Frame> = None;

    loop {
        if interrupted.load(Ordering::Relaxed) {
            break;
        }
        let now = Instant::now();
        if let Some(started) = started {
            if let Some(seconds) = request.duration {
                if started.elapsed() >= Duration::from_secs(seconds) {
                    break;
                }
            }
        } else if now >= first_frame_deadline {
            return Err(VshotError::Recording(format!(
                "the compositor's screen cast sent no frame within {} seconds; the picker may \
                 have been dismissed, or the source it chose may be on an output that is off, \
                 disabled or disconnected",
                FIRST_FRAME_TIMEOUT.as_secs()
            )));
        }

        let frame = match stream.next(POLL) {
            Ok(Some(frame)) => frame,
            // A screen cast is damage-driven: no frame in the window is the
            // normal state of a screen nothing has happened on, not a failure.
            Ok(None) => continue,
            Err(error) => return Err(error),
        };
        let now = Instant::now();
        if started.is_none() {
            started = Some(now);
            last_frame_at = now;
            first_frame_deadline = now;
        }
        // A screen cast that renegotiates mid-recording — a window resized, a
        // monitor reconfigured — would have to change the layout of a file
        // that already has one, so the recording ends cleanly there instead.
        // The file up to that point is complete.
        let (frame_width, frame_height) = frame.size();
        if frame_width != geometry.width
            || frame_height != geometry.height
            || frame.spa_format() != geometry.spa_format
        {
            eprintln!(
                "vshot: the recording ended: the screen cast changed to {frame_width}x\
                 {frame_height} (spa format {}), and one MP4 holds one frame size and layout",
                frame.spa_format()
            );
            break;
        }

        // The frame's duration is the time the *previous* frame was on
        // screen, which is the interval between the two arrivals — the same
        // accounting the screen and window loops use, so a portal recording
        // plays back on the same clock as the others.
        covered_us += now.saturating_duration_since(last_frame_at).as_micros() as u64;
        last_frame_at = now;
        let due_ms = covered_us / 1000;
        let step = due_ms.saturating_sub(timeline_ms);
        timeline_ms = timeline_ms.max(due_ms);
        let duration_ms = u32::try_from(step).unwrap_or(1).max(1);

        let encode_started = Instant::now();
        match shape {
            Shape::Dmabuf => {
                let Some(dmabuf) = frame.dmabuf() else {
                    eprintln!(
                        "vshot: the recording ended: the screen cast stopped sending dma-buf \
                         frames and one MP4 holds one pixel layout"
                    );
                    break;
                };
                recorder.frame_dmabuf(
                    dmabuf.fd,
                    fourcc,
                    dmabuf.modifier,
                    dmabuf.offset,
                    dmabuf.stride,
                    duration_ms,
                )?;
                tail_dmabuf = Some(dmabuf);
            }
            Shape::Software => {
                let Some(memory) = frame.memory() else {
                    eprintln!(
                        "vshot: the recording ended: the screen cast stopped sending memory \
                         frames and one MP4 holds one pixel layout"
                    );
                    break;
                };
                let rgba = rgba_from_memory(
                    &memory,
                    geometry.width,
                    geometry.height,
                    geometry.spa_format,
                )?;
                recorder.frame(&rgba, duration_ms)?;
                tail_software = Some(rgba);
            }
        }
        // The soundtrack for the interval this frame covered.
        super::pump_soundtrack(mic, recorder)?;
        if debug_enabled() {
            eprintln!(
                "vshot: frame {}: muxed in {:.1}ms (on screen {duration_ms}ms, PipeWire \
                 sequence {})",
                recorder.frames(),
                encode_started.elapsed().as_secs_f64() * 1000.0,
                frame.sequence()
            );
        }
    }

    // The last interval has not been written yet, because a frame's length is
    // only known once the next frame arrives.  Sending the last frame a second
    // time with the time up to the end as its duration is what puts that time
    // in the file: without it a still screen's recording would be as long as
    // the moment of its last change, not as long as it was recorded.  The
    // client still holds the buffer, so its pixels are still there.
    // The soundtrack of the tail interval, queued before the last frame is
    // sent again: `finish` flushes the audio encoder after the video's, so
    // the samples have to be in by then.
    super::pump_soundtrack(mic, recorder)?;
    let now = Instant::now();
    covered_us += now.saturating_duration_since(last_frame_at).as_micros() as u64;
    let step = (covered_us / 1000).saturating_sub(timeline_ms);
    if let Ok(duration_ms) = u32::try_from(step) {
        if duration_ms > 0 {
            match (&tail_dmabuf, &tail_software) {
                (Some(dmabuf), _) => recorder.frame_dmabuf(
                    dmabuf.fd,
                    fourcc,
                    dmabuf.modifier,
                    dmabuf.offset,
                    dmabuf.stride,
                    duration_ms,
                )?,
                (None, Some(frame)) => recorder.frame(frame, duration_ms)?,
                (None, None) => {}
            }
        }
    }
    Ok(())
}

/// The DRM name for the byte order a `SPA_VIDEO_FORMAT_*` stands for, which is
/// what the encoder's dma-buf import takes.  Only the two layouts a screen
/// cast produces are known here; anything else is refused with the format
/// named rather than guessed at, because importing the wrong one gives a
/// file with the colours swapped and no error anywhere.
fn fourcc_for(spa_format: u32) -> Result<u32> {
    match spa_format {
        SPA_BGRX => Ok(crate::capture::dmabuf::DRM_FORMAT_XRGB8888),
        SPA_BGRA => Ok(crate::capture::dmabuf::DRM_FORMAT_ARGB8888),
        other => Err(VshotError::Recording(format!(
            "the portal is casting pixels in SPA format {other}, which the encoder cannot \
             import; set VSHOT_PORTAL_SHM=1 to take the copy path instead"
        ))),
    }
}

/// Converts one memory frame to the packed RGBA the encoder's software path
/// takes, dropping the row padding and swapping the byte order.
///
/// The SPA name says what the bytes are; vshot works in RGBA, which is the
/// same four bytes with the red and blue ends swapped — the conversion the
/// wlroots capture path does for the same reason.  Doing it as whole 32-bit
/// words rather than a byte at a time is what lets it keep up with a full
/// screen; a copy is unavoidable here, because the memory belongs to the
/// compositor's buffer and the encoder wants a tight buffer of its own.
fn rgba_from_memory(
    memory: &pipewire::Memory<'_>,
    width: u32,
    height: u32,
    spa_format: u32,
) -> Result<Frame> {
    let width = usize::try_from(width)
        .map_err(|_| VshotError::Recording("the frame width is too large".into()))?;
    let height = usize::try_from(height)
        .map_err(|_| VshotError::Recording("the frame height is too large".into()))?;
    let row_bytes = width
        .checked_mul(4)
        .ok_or_else(|| VshotError::Recording("the frame is too wide".into()))?;
    let expected = row_bytes
        .checked_mul(height)
        .ok_or_else(|| VshotError::Recording("the frame is too large".into()))?;
    if memory.stride < row_bytes {
        return Err(VshotError::Recording(format!(
            "the portal's frame has a stride of {} bytes, which is less than one row of {width} \
             pixels",
            memory.stride
        )));
    }
    if memory.bytes.len() < memory.stride * height {
        return Err(VshotError::Recording(format!(
            "the portal's frame has {} bytes, which is less than {height} rows of {} bytes",
            memory.bytes.len(),
            memory.stride
        )));
    }

    // What the bytes mean: whether the red and blue ends are swapped, and what
    // to do about a fourth byte that is not alpha.
    let (swap, opaque) = match spa_format {
        SPA_BGRX => (true, true),
        SPA_BGRA => (true, false),
        SPA_RGBX => (false, true),
        SPA_RGBA => (false, false),
        other => {
            return Err(VshotError::Recording(format!(
                "the portal is casting pixels in SPA format {other}, which this path cannot \
                 convert"
            )))
        }
    };

    let mut pixels = vec![0u8; expected];
    for (destination_row, source_row) in pixels.chunks_exact_mut(row_bytes).zip(
        memory
            .bytes
            .chunks_exact(memory.stride.max(row_bytes))
            .take(height),
    ) {
        for (destination, source) in destination_row
            .chunks_exact_mut(4)
            .zip(source_row[..row_bytes].chunks_exact(4))
        {
            let word = u32::from_le_bytes([source[0], source[1], source[2], source[3]]);
            let word = if swap {
                (word & 0x0000_00FF) << 16 | (word & 0x00FF_0000) >> 16 | (word & 0xFF00_FF00)
            } else {
                word
            };
            let word = if opaque { word | 0xFF00_0000 } else { word };
            destination.copy_from_slice(&word.to_le_bytes());
        }
    }
    Frame::new(Size::new(width as u32, height as u32), pixels)
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

#[cfg(test)]
mod tests {
    use super::*;

    /// The two layouts a screen cast produces map onto the two DRM names the
    /// encoder imports; anything else has to be refused, because a wrong
    /// mapping is a valid file with the colours swapped.
    #[test]
    fn the_formats_the_encoder_imports_are_the_ones_known() {
        assert_eq!(
            fourcc_for(SPA_BGRX).unwrap(),
            crate::capture::dmabuf::DRM_FORMAT_XRGB8888
        );
        assert_eq!(
            fourcc_for(SPA_BGRA).unwrap(),
            crate::capture::dmabuf::DRM_FORMAT_ARGB8888
        );
        for unknown in [SPA_RGBX, SPA_RGBA, 0, 999] {
            let error = fourcc_for(unknown).unwrap_err().to_string();
            assert!(
                error.contains(&unknown.to_string()),
                "the refusal has to name the format: {error}"
            );
        }
    }

    /// A memory frame becomes vshot's RGBA: BGRx is the layout a screen comes
    /// out as, and the byte that is not alpha has to come out opaque.
    #[test]
    fn a_bgrx_frame_becomes_rgba() {
        // Two pixels: one blue-ish, one with a zero fourth byte.
        let bytes = vec![10, 20, 30, 0, 40, 50, 60, 0];
        let memory = pipewire::Memory {
            bytes: &bytes,
            stride: 8,
        };
        let frame = rgba_from_memory(&memory, 2, 1, SPA_BGRX).unwrap();
        assert_eq!(frame.pixels(), &[30, 20, 10, 255, 60, 50, 40, 255]);
        assert_eq!(frame.size(), Size::new(2, 1));
    }

    /// BGRA keeps its alpha: a window recording has to stay see-through where
    /// the window is.
    #[test]
    fn a_bgra_frame_keeps_its_alpha() {
        let bytes = vec![10, 20, 30, 40];
        let memory = pipewire::Memory {
            bytes: &bytes,
            stride: 4,
        };
        let frame = rgba_from_memory(&memory, 1, 1, SPA_BGRA).unwrap();
        assert_eq!(frame.pixels(), &[30, 20, 10, 40]);
    }

    /// The rows of a memory frame can be padded; the padding must not reach
    /// the encoder, which takes tight rows.
    #[test]
    fn row_padding_is_dropped() {
        let bytes = vec![
            1, 2, 3, 4, 0xAA, 0xBB, 0xCC, 0xDD, 5, 6, 7, 8, 9, 10, 11, 12,
        ];
        let memory = pipewire::Memory {
            bytes: &bytes,
            stride: 8,
        };
        let frame = rgba_from_memory(&memory, 1, 2, SPA_BGRA).unwrap();
        assert_eq!(frame.pixels(), &[3, 2, 1, 4, 7, 6, 5, 8]);
    }

    /// A frame that is too small for the stride it claims is refused rather
    /// than read past its end.
    #[test]
    fn a_short_frame_is_refused() {
        let bytes = vec![0u8; 4];
        let memory = pipewire::Memory {
            bytes: &bytes,
            stride: 8,
        };
        assert!(rgba_from_memory(&memory, 2, 1, SPA_BGRX).is_err());
        let memory = pipewire::Memory {
            bytes: &bytes,
            stride: 4,
        };
        assert!(rgba_from_memory(&memory, 2, 1, SPA_BGRX).is_err());
    }
}

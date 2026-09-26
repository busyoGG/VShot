// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Instant replay: `vshot replay`.
//!
//! A replay is a recording that keeps its last window of history in memory
//! instead of on disk.  It runs the same frame source, the same GPU encoder
//! and the same audio side as `record`; the one difference is where the
//! encoded packets go — an in-memory ring rather than an MP4 muxer.  Nothing
//! is written until a save is triggered, and then the packets already in the
//! ring are copied straight into a fresh MP4 (a remux, no re-encode), so the
//! steady state costs one encode and one in-memory push, and the trigger costs
//! one mux.
//!
//! # Why this shape
//!
//! The alternative — keeping raw frames and encoding them when the user hits
//! the key — is what the numbers rule out: 4K60 NV12 is ~12 MB a frame, so 30
//! seconds is ~22 GB, while the same window as encoded packets at 30 Mbps is
//! ~110 MB.  A replay therefore has to encode continuously; the only real
//! choices are how much history and how cheaply.  The ring keeps the window as
//! packets (bounded, small), the encoder is bounded-GOP (so the ring is a
//! fraction of an all-intra stream and every GOP boundary is a valid start),
//! and a save is a stream copy.
//!
//! # How it is driven
//!
//! `vshot replay start` runs the session in the foreground (or detaches with
//! `--background`) and owns the pid file and the ring.  A save is triggered by
//! `vshot replay save`, which writes a one-line request into a control socket
//! the session owns; the session answers by writing the file and reporting
//! where it went.  The control channel is the same shape the pin daemon uses,
//! so a compositor keybinding can trigger a save with no display of its own.
//!
//! The session is single-threaded: the control socket is polled between
//! frames, so a save is handled at a frame boundary with the ring consistent.
//! A save that asks for more than the ring holds gets everything there is.

use std::io::{Read, Write};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use crate::capture::Capturer;
use crate::error::{Result, VshotError};
use crate::wayland::WaylandSession;

use super::avcodec::{ReplayRecorder, VideoCodec};
use super::{debug_enabled, resolve_source, sleep_interruptible, Source};
use crate::capture::window_copy::Follow;

/// How many times a second frames are taken at most, when `--fps` says
/// nothing.  A replay defaults lower than a recording: it is left running for
/// long stretches, and 30 fps halves the encoder's work while a replay of
/// motion is still smooth.
pub const DEFAULT_FPS: u32 = 30;

/// The ring's window when `--window` and the config say nothing, in seconds.
pub const DEFAULT_WINDOW: u64 = 30;

/// The key-frame distance when `--gop` and the config say nothing, in
/// seconds.  One second is the usual compromise: the ring is a fraction of an
/// all-intra stream, and a save starts within a second of the requested edge.
pub const DEFAULT_GOP: u64 = 1;

/// The longest GOP a save can still start from: with a ten-second key-frame
/// distance the ring has to hold the whole GOP past the window, which is why
/// the flag is bounded rather than free.
const MAX_GOP: u64 = 10;

/// A parsed replay request, from `vshot replay start`.
#[derive(Clone, Debug, PartialEq)]
pub struct ReplayRequest {
    /// What to record: a monitor, the whole desktop, a region or a window —
    /// the same shapes `record` takes.
    pub target: super::RecordTarget,
    /// Seconds of history the ring keeps.
    pub window: u64,
    pub fps: u32,
    /// The video codec to encode with.
    pub encoder: VideoCodec,
    /// Which hardware encoder runs the session (`--encoder-backend`), as on
    /// the recording side.
    pub encoder_backend: crate::record::avcodec::EncoderBackend,
    /// Draw the cursor into the frames, like `--cursor` for screenshots.
    pub cursor: bool,
    /// Keep the microphone in the ring, and which input.
    pub mic: Option<super::MicChoice>,
    /// Keep the recorded window's own application's audio in the ring
    /// (`--app-audio`), as `record` does.
    pub app_audio: bool,
    /// Take frames from the desktop portal instead of the compositor's own
    /// protocols.
    pub portal: bool,
    /// Where a save lands, when one is triggered without its own path.
    pub save_dir: Option<PathBuf>,
    /// The key-frame distance, in seconds (1-10).
    pub gop_secs: u64,
    /// The windows a `--follow` window replay moves between, as on the
    /// recording side: the ring keeps one window at a time, switching to
    /// whichever of them the focus lands on, and stays where it is when the
    /// focus is anywhere else.
    pub follow: Vec<String>,
}

impl ReplayRequest {
    fn frame_interval(&self) -> Duration {
        Duration::from_nanos(1_000_000_000 / u64::from(self.fps.max(1)))
    }

    fn gop_frames(&self) -> u32 {
        u32::try_from((self.fps.max(1) as u64).saturating_mul(self.gop_secs.clamp(1, MAX_GOP)))
            .unwrap_or(u32::MAX)
    }
}

/// Where the control socket lives: `VSHOT_REPLAY_SOCKET` overrides, else
/// `$XDG_RUNTIME_DIR/vshot-replay-<uid>.sock`, else `/tmp`.  Separate from the
/// recording's pid file so the two never collide.
pub fn socket_path() -> PathBuf {
    if let Some(override_path) = std::env::var_os("VSHOT_REPLAY_SOCKET") {
        let path = PathBuf::from(override_path);
        if !path.as_os_str().is_empty() {
            return path;
        }
    }
    let runtime = std::env::var_os("XDG_RUNTIME_DIR")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp"));
    runtime.join(format!(
        "vshot-replay-{}.sock",
        rustix::process::getuid().as_raw()
    ))
}

/// The pid file the session writes, so `replay stop` can signal it.
fn pid_file() -> PathBuf {
    super::pid_file_named("replay")
}

/// Whether a replay is already running, from the pid file's point of view.
fn running_pid() -> Option<i32> {
    let text = std::fs::read_to_string(pid_file()).ok()?;
    let pid: i32 = text.trim().parse().ok()?;
    let alive = unsafe { libc::kill(pid, 0) } == 0;
    alive.then_some(pid)
}

/// One line of the control protocol.  `Save` carries an optional path and an
/// optional number of seconds to take from the ring; `Stop` ends the session.
#[derive(Clone, Debug, PartialEq, serde::Serialize, serde::Deserialize)]
#[serde(tag = "command", rename_all = "kebab-case")]
enum ReplayCommand {
    /// Write the last `seconds` of the ring (all of it when `seconds` is
    /// absent or zero) to `path`, or the session's own default when absent.
    Save {
        #[serde(default)]
        path: Option<PathBuf>,
        #[serde(default)]
        seconds: Option<u64>,
    },
    /// How much history the ring holds, and where a default save would land.
    Status,
    /// End the session.
    Stop,
}

/// What the session answers a control line with.
#[derive(Clone, Debug, PartialEq, serde::Serialize, serde::Deserialize)]
#[serde(tag = "reply", rename_all = "kebab-case")]
enum ReplayReply {
    /// A save finished: the file and how much it holds.
    Saved { path: PathBuf, seconds: f64 },
    /// The ring's current span, in seconds, and how many saves it has served.
    Status { span_seconds: f64, saves: u64 },
    /// The session is ending.
    Stopping,
    /// Something went wrong; the reason is the text.
    Error { message: String },
}

/// Sends one control line to a running session, starting nothing (a replay is
/// long-lived; `save` never spawns it).
fn send_command(command: &ReplayCommand) -> Result<ReplayReply> {
    let path = socket_path();
    let mut payload = serde_json::to_vec(command).map_err(|error| {
        VshotError::Recording(format!("failed to encode replay request: {error}"))
    })?;
    payload.push(b'\n');
    let mut stream = UnixStream::connect(&path).map_err(|error| {
        VshotError::Recording(format!(
            "no replay is running (nothing answers on {}): {error}",
            path.display()
        ))
    })?;
    stream
        .set_read_timeout(Some(Duration::from_secs(30)))
        .map_err(|error| {
            VshotError::Recording(format!("could not set the replay socket timeout: {error}"))
        })?;
    stream.write_all(&payload).map_err(|error| {
        VshotError::Recording(format!("failed to send the replay request: {error}"))
    })?;
    let mut buffer = Vec::new();
    let mut chunk = [0u8; 1024];
    loop {
        let read = stream.read(&mut chunk).map_err(|error| {
            VshotError::Recording(format!("failed to read the replay reply: {error}"))
        })?;
        if read == 0 {
            break;
        }
        buffer.extend_from_slice(&chunk[..read]);
        if buffer.contains(&b'\n') {
            break;
        }
    }
    let reply: ReplayReply = serde_json::from_slice(&buffer).map_err(|error| {
        VshotError::Recording(format!("the replay session returned invalid JSON: {error}"))
    })?;
    Ok(reply)
}

/// `vshot replay save`: asks the running session to write its history to a
/// file and reports where it went.
pub fn save(path: Option<PathBuf>, seconds: Option<u64>) -> Result<PathBuf> {
    match send_command(&ReplayCommand::Save { path, seconds })? {
        ReplayReply::Saved { path, seconds } => {
            eprintln!("vshot: saved {seconds:.1}s into {}", path.display());
            Ok(path)
        }
        // The session's own sentence is already worded for the user.
        ReplayReply::Error { message } => Err(VshotError::Bare(message)),
        other => Err(VshotError::Recording(format!(
            "the replay session answered a save with {other:?}"
        ))),
    }
}

/// `vshot replay status`: what the running session holds.
pub fn status() -> Result<()> {
    match send_command(&ReplayCommand::Status)? {
        ReplayReply::Status {
            span_seconds,
            saves,
        } => {
            println!("{span_seconds:.1}\t{saves}");
            Ok(())
        }
        ReplayReply::Error { message } => Err(VshotError::Bare(message)),
        other => Err(VshotError::Recording(format!(
            "the replay session answered a status with {other:?}"
        ))),
    }
}

/// `vshot replay stop`: asks the session to end and waits for it to leave.
pub fn stop() -> Result<()> {
    match send_command(&ReplayCommand::Stop) {
        Ok(ReplayReply::Stopping) => {}
        Ok(ReplayReply::Error { message }) => return Err(VshotError::Bare(message)),
        Ok(other) => {
            return Err(VshotError::Recording(format!(
                "the replay session answered a stop with {other:?}"
            )))
        }
        // A session that has already died but left its socket: fall through to
        // the pid file, which is the authority on whether it is really gone.
        Err(_) => {}
    }
    let deadline = Instant::now() + Duration::from_secs(10);
    while Instant::now() < deadline {
        if running_pid().is_none() {
            let _ = std::fs::remove_file(socket_path());
            println!("vshot: replay stopped");
            return Ok(());
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    Err(VshotError::Recording(
        "the replay is still finishing after 10 seconds".into(),
    ))
}

/// The save path for a request: the session's own default when the control
/// line names none.  The directory is made when vshot chose it (the videos
/// directory), an error of the user's when they named a missing one.
fn resolve_save_path(
    requested: Option<&Path>,
    save_dir: Option<&Path>,
    label: &str,
) -> Result<PathBuf> {
    let (raw, owned_default) = match requested {
        Some(path) => (path.to_string_lossy().to_string(), false),
        None => {
            let base = match save_dir {
                Some(dir) => dir.to_path_buf(),
                None => super::videos_directory().ok_or_else(|| {
                    VshotError::Recording(
                        "no videos directory is known ($XDG_VIDEOS_DIR, the user-dirs file, or \
                         $HOME/Videos); name the file with `replay save PATH`"
                            .into(),
                    )
                })?,
            };
            let name = format!("{label}-%Y%m%d-%H%M%S.mp4");
            (base.join(name).to_string_lossy().to_string(), true)
        }
    };
    let expanded = chrono::Local::now().format(&raw).to_string();
    let mut path = PathBuf::from(expanded);
    if path.extension().and_then(|ext| ext.to_str()) != Some("mp4") {
        path.set_extension("mp4");
    }
    match path.parent().filter(|dir| !dir.as_os_str().is_empty()) {
        Some(directory) if owned_default => {
            std::fs::create_dir_all(directory).map_err(|source| {
                VshotError::Recording(format!(
                    "could not create the videos directory {}: {source}",
                    directory.display()
                ))
            })?;
        }
        Some(directory) if !directory.is_dir() => {
            return Err(VshotError::Recording(format!(
                "the directory {} does not exist",
                directory.display()
            )));
        }
        _ => {}
    }
    Ok(path)
}

/// Runs a replay session to completion.  This is `vshot replay start`: it owns
/// the ring, the pid file and the control socket, and returns when the session
/// is stopped.
pub fn run(request: &ReplayRequest) -> Result<()> {
    if let Some(pid) = running_pid() {
        return Err(VshotError::Recording(format!(
            "a replay is already running (pid {pid}); stop it with `vshot replay stop` first"
        )));
    }
    if request.portal {
        return Err(VshotError::Recording(
            "replay does not support the portal yet: the portal's own frame loop is not wired to \
             the ring. Record through the portal instead (`vshot record ... --portal`)"
                .into(),
        ));
    }
    if request.window == 0 {
        return Err(VshotError::Recording(
            "the replay window has to be at least one second".into(),
        ));
    }

    // The control socket and the stop handler are the session's own, whatever
    // the frame source; both loops share them.
    let listener = bind_control_socket()?;
    listener.set_nonblocking(true).map_err(|error| {
        VshotError::Recording(format!(
            "could not set the replay socket non-blocking: {error}"
        ))
    })?;
    let interrupted = super::install_stop_handler()?;
    super::write_pid_file_named("replay")?;

    // A window replay has a different frame source — the compositor's own copy
    // of one window — so it runs its own loop; everything around it (the ring,
    // the control socket, the pid file) is shared.
    let outcome = match &request.target {
        super::RecordTarget::Window(target) => {
            window_session(request, target, &listener, &interrupted)
        }
        _ => screen_session(request, &listener, &interrupted),
    };

    let _ = std::fs::remove_file(pid_file());
    let _ = std::fs::remove_file(socket_path());
    outcome
}

/// The screen (monitor/all/region) replay session: the shared frame source,
/// encoded into the ring.
fn screen_session(
    request: &ReplayRequest,
    listener: &UnixListener,
    interrupted: &AtomicBool,
) -> Result<()> {
    // --- the frame source and its geometry --------------------------------
    let wayland = WaylandSession::connect()?;
    let topology = wayland.output_infos()?;
    let mut capture = Capturer::connect()?;
    let (source, (encoded_width, encoded_height)) =
        resolve_source(&request.target, &mut capture, &topology, request.cursor)?;

    // --- the microphone ---------------------------------------------------
    // A screen, the whole desktop or a region has no single application, so
    // `--app-audio` has nothing to attach to here (the CLI refuses it); the
    // soundtrack is the microphone the request asked for.
    let mut mic = super::open_soundtrack(request.mic.as_ref())?;
    let mic_format = mic.format();

    // --- the encoder and the ring -----------------------------------------
    // `auto` resolves once, here, so the probe and the open agree; NVENC has
    // no dma-buf import and always takes the software path.
    let backend = request.encoder_backend.resolve();
    let dmabuf_fourcc = if backend == crate::record::avcodec::EncoderBackend::Nvenc {
        None
    } else {
        super::probe_zero_copy(&mut capture, &source, encoded_width, encoded_height)
    };
    let gop_frames = request.gop_frames();
    let retention = request
        .window
        .saturating_add(request.gop_secs.clamp(1, MAX_GOP));
    let mut recorder = ReplayRecorder::start(
        encoded_width,
        encoded_height,
        request.encoder,
        dmabuf_fourcc,
        mic_format,
        retention,
        gop_frames,
        request.fps,
        backend,
    )?;
    if debug_enabled() {
        eprintln!(
            "vshot: replay {encoded_width}x{encoded_height} at {} fps with {} ({}) through \
             libavcodec {}, keeping {}s in memory (GOP {} frames)",
            request.fps,
            request.encoder.word(),
            backend.word(),
            super::avcodec::libavcodec_version(),
            request.window,
            gop_frames
        );
        if recorder.audio_channels() > 0 {
            eprintln!(
                "vshot: microphone kept in the ring: {} Hz, {} channel(s), AAC",
                recorder.audio_rate(),
                recorder.audio_channels()
            );
        }
    }

    mic.arm();
    session_loop(
        &mut capture,
        &source,
        request,
        dmabuf_fourcc.is_some(),
        &mut recorder,
        &mut mic,
        listener,
        interrupted,
    )
}

/// The window replay session: the compositor's own copy of one window,
/// encoded into the ring.  Reuses the recording window loop through the
/// [`crate::record::avcodec::VideoSink`] the ring implements, with the control
/// socket polled between frames.
///
/// Which loop depends on what the session speaks, exactly as on the recording
/// side: the wlroots route copies the window through
/// `ext_image_copy_capture_v1`, a Plasma session has no such protocol and
/// copies it through KWin's own `ScreenShot2.CaptureWindow`, and a compositor
/// with neither — niri — casts it through its own screen-cast service.
fn window_session(
    request: &ReplayRequest,
    target: &super::WindowTarget,
    listener: &UnixListener,
    interrupted: &AtomicBool,
) -> Result<()> {
    if crate::capture::active_output::Session::detect()
        == crate::capture::active_output::Session::KWin
    {
        return kwin_window_session(request, target, listener, interrupted);
    }
    // The same fallback a window *recording* takes, because it is the same
    // frame source; only the sink differs — a ring instead of a file.
    if !super::window_capture_supported() && super::screencast::available() {
        return screencast_window_session(request, target, listener, interrupted);
    }
    use super::avcodec::VideoSink;
    let (mut capture, shape, name) = super::window::open_window_capture(target)?;
    // `--mic` and `--app-audio` are independent and may both be given: the
    // microphone is the room, the window's own application audio is the
    // window's sound, and the ring keeps both summed into its one track.
    let app_audio = if request.app_audio {
        super::open_app_audio(&name)?
    } else {
        None
    };
    let mic = super::open_soundtrack_with(request.mic.as_ref(), app_audio)?;
    let mic_format = mic.format();
    let gop_frames = request.gop_frames();
    let retention = request
        .window
        .saturating_add(request.gop_secs.clamp(1, MAX_GOP));
    // A window replay always captures dma-bufs; on NVENC the ring keeps them
    // as RGBA instead, because that backend cannot import one.
    let backend = request.encoder_backend.resolve();
    let mut recorder = ReplayRecorder::start(
        shape.width,
        shape.height,
        request.encoder,
        Some(shape.fourcc),
        mic_format,
        retention,
        gop_frames,
        request.fps,
        backend,
    )?;
    if debug_enabled() {
        eprintln!(
            "vshot: window replay {}x{} at {} fps with {} ({}) through libavcodec {}, keeping \
             {}s in memory (GOP {} frames)",
            shape.width,
            shape.height,
            request.fps,
            request.encoder.word(),
            backend.word(),
            super::avcodec::libavcodec_version(),
            request.window,
            gop_frames
        );
    }
    mic.arm();
    // The recording window loop wants a `RecordRequest`; what it reads that
    // matters here is the frame rate and the `--app-audio` flag, which tells a
    // `--follow` switch to carry the application stream across to the new
    // window while the microphone keeps running.
    let loop_request = super::RecordRequest {
        target: request.target.clone(),
        output: None,
        fps: request.fps,
        cursor: request.cursor,
        duration: None,
        encoder: request.encoder,
        encoder_backend: request.encoder_backend,
        portal: false,
        mic: None,
        // The audio side is already resolved into `mic` above; the loop takes
        // the `Soundtrack` itself.
        app_audio: request.app_audio,
        follow: request.follow.clone(),
    };
    let follow = Follow::new(request.follow.clone());
    let _ = VideoSink::canvas(&recorder);
    super::window::loop_over(
        &mut capture,
        &mut recorder,
        &loop_request,
        &follow,
        name,
        mic,
        interrupted,
        |sink| {
            // The control socket is polled at every frame boundary, so a save
            // is served with the ring consistent and the loop keeps running.
            if let Some(stream) = accept_control(listener) {
                handle_control(stream, sink, request)
            } else {
                Ok(false)
            }
        },
    )
}

/// The KWin window replay session: KWin's own copy of one window, by its
/// `QUuid`, encoded into the ring.  The twin of [`window_session`] for the
/// desktops the wlroots route cannot serve — a Plasma session speaks neither
/// `ext_foreign_toplevel_list_v1` nor `ext_image_copy_capture_v1`.  The frame
/// loop, the follow logic and the per-frame control socket are exactly the
/// ones a KWin window *recording* uses; only the sink differs — a ring instead
/// of a file.
fn kwin_window_session(
    request: &ReplayRequest,
    target: &super::WindowTarget,
    listener: &UnixListener,
    interrupted: &AtomicBool,
) -> Result<()> {
    use super::avcodec::VideoSink;
    // The window is captured once before the ring opens, because the ring's
    // canvas comes from the window's own pixels — which only a capture
    // reports.  KWin's ScreenShot2 has no dma-buf, so the ring keeps RGBA
    // frames on every backend, not only NVENC.
    let loop_request = super::RecordRequest {
        target: request.target.clone(),
        output: None,
        fps: request.fps,
        cursor: request.cursor,
        duration: None,
        encoder: request.encoder,
        encoder_backend: request.encoder_backend,
        portal: false,
        mic: None,
        app_audio: request.app_audio,
        follow: request.follow.clone(),
    };
    let (mut capture, mut window, follow, first) =
        super::kwin_window::open_window_capture(&loop_request, target)?;
    // The ring's canvas, like a file's, has to be even for NV12 — see
    // `kwin_window::even_canvas`.  ScreenShot2 hands back the window's client
    // geometry, which can be odd; the fit path pads the extra column and row.
    let (width, height) = super::kwin_window::even_canvas(first.size());

    // `--mic` and `--app-audio` are independent and may both be given: the
    // microphone is the room, the window's own application audio is the
    // window's sound, and the ring keeps both summed into its one track.
    let app_audio = if request.app_audio {
        super::open_app_audio(&window.as_name())?
    } else {
        None
    };
    let mic = super::open_soundtrack_with(request.mic.as_ref(), app_audio)?;
    let mic_format = mic.format();
    let gop_frames = request.gop_frames();
    let retention = request
        .window
        .saturating_add(request.gop_secs.clamp(1, MAX_GOP));
    let backend = request.encoder_backend.resolve();
    let mut recorder = ReplayRecorder::start(
        width,
        height,
        request.encoder,
        // No dma-buf: ScreenShot2 writes pixels through a pipe, and the ring
        // keeps them as RGBA.
        None,
        mic_format,
        retention,
        gop_frames,
        request.fps,
        backend,
    )?;
    if debug_enabled() {
        eprintln!(
            "vshot: KWin window replay {width}x{height} at {} fps with {} ({}) through libavcodec \
             {}, keeping {}s in memory (GOP {} frames)",
            request.fps,
            request.encoder.word(),
            backend.word(),
            super::avcodec::libavcodec_version(),
            request.window,
            gop_frames
        );
    }
    let _ = VideoSink::canvas(&recorder);
    let mut mic = mic;
    mic.arm();
    super::kwin_window::loop_over(
        &mut capture,
        &mut recorder,
        &loop_request,
        &follow,
        &mut window,
        first,
        &mut mic,
        interrupted,
        |sink| {
            if let Some(stream) = accept_control(listener) {
                handle_control(stream, sink, request)
            } else {
                Ok(false)
            }
        },
    )
}

/// The screen-cast window replay session: the compositor's own cast of one
/// window, encoded into the ring.  The twin of [`window_session`] for a
/// compositor that speaks neither `ext_image_copy_capture_v1` nor KWin's
/// `ScreenShot2` — niri, whose capture support stops at outputs and whose
/// window pixels come from the screen-cast service its portal drives.  The
/// cast, the loop and the per-frame control socket are the ones a window
/// *recording* on that service uses; only the sink differs.
fn screencast_window_session(
    request: &ReplayRequest,
    target: &super::WindowTarget,
    listener: &UnixListener,
    interrupted: &AtomicBool,
) -> Result<()> {
    use super::avcodec::VideoSink;
    let backend = request.encoder_backend.resolve();
    let mut window = super::screencast::WindowCast::open(
        target,
        request.cursor,
        request.fps,
        super::screencast::allow_dmabuf(backend),
    )?;
    let geometry = window.geometry;
    let shape = super::cast::Shape::of(&geometry);
    let fourcc = super::cast::fourcc_for(geometry.spa_format)?;

    // `--mic` and `--app-audio` are independent and may both be given: the
    // microphone is the room, the window's own application audio is the
    // window's sound, and the ring keeps both summed into its one track.
    let app_audio = if request.app_audio {
        super::open_app_audio(&window.name)?
    } else {
        None
    };
    let mic = super::open_soundtrack_with(request.mic.as_ref(), app_audio)?;
    let mic_format = mic.format();
    let gop_frames = request.gop_frames();
    let retention = request
        .window
        .saturating_add(request.gop_secs.clamp(1, MAX_GOP));
    let mut recorder = ReplayRecorder::start(
        geometry.width,
        geometry.height,
        request.encoder,
        // The ring keeps dma-bufs only where the cast produced them and the
        // backend can import them; a memory cast is RGBA, exactly as on the
        // recording side.
        match shape {
            super::cast::Shape::Dmabuf => Some(fourcc),
            super::cast::Shape::Software => None,
        },
        mic_format,
        retention,
        gop_frames,
        request.fps,
        backend,
    )?;
    if debug_enabled() {
        eprintln!(
            "vshot: window replay {}x{} at {} fps with {} ({}) through libavcodec {}, keeping \
             {}s in memory (GOP {} frames)",
            geometry.width,
            geometry.height,
            request.fps,
            request.encoder.word(),
            backend.word(),
            super::avcodec::libavcodec_version(),
            request.window,
            gop_frames
        );
    }
    let _ = VideoSink::canvas(&recorder);
    let mut mic = mic;
    mic.arm();
    let ended = window.ended_flag();
    let outcome = super::cast::loop_over(
        &mut window.stream,
        &mut recorder,
        // A replay has no `--duration`: it runs until it is stopped.
        None,
        shape,
        geometry,
        &mut mic,
        interrupted,
        // A window that is resized keeps being kept: the ring holds one
        // canvas, and the new frames are fitted into it.
        true,
        move |sink| {
            // The compositor saying the cast is over ends the session: a
            // window that was closed stops the frames without an error.
            if ended.load(Ordering::Relaxed) {
                eprintln!(
                    "vshot: the replay ended: the compositor stopped casting the window; it may \
                     have been closed"
                );
                return Ok(true);
            }
            // The control socket is polled at every frame boundary, so a save
            // is served with the ring consistent and the loop keeps running.
            if let Some(stream) = accept_control(listener) {
                handle_control(stream, sink, request)
            } else {
                Ok(false)
            }
        },
    );
    window.stop();
    outcome
}

/// Binds the control socket, replacing a stale one (a socket left by a session
/// that died without cleaning up would otherwise block the next start).
fn bind_control_socket() -> Result<UnixListener> {
    let path = socket_path();
    if path.exists() {
        // A live session is refused earlier by the pid file; anything left
        // here is stale.
        let _ = std::fs::remove_file(&path);
    }
    UnixListener::bind(&path).map_err(|error| {
        VshotError::Recording(format!(
            "could not bind the replay control socket {}: {error}",
            path.display()
        ))
    })
}

/// The replay loop: grab, encode into the ring, poll the control socket, pace.
#[allow(clippy::too_many_arguments)] // the session's own shape
fn session_loop(
    capture: &mut Capturer,
    source: &Source,
    request: &ReplayRequest,
    zero_copy: bool,
    recorder: &mut ReplayRecorder,
    mic: &mut super::pipewire_audio::Soundtrack,
    listener: &UnixListener,
    interrupted: &AtomicBool,
) -> Result<()> {
    let started = Instant::now();
    let interval = request.frame_interval();
    let mut scratch = super::SceneScratch::new();
    let mut next_frame = started;
    let mut last_frame_at = started;
    let mut consecutive_errors = 0u32;
    let mut timeline_ms = 0u64;
    let mut covered_us = 0u64;

    // A capture the compositor *refused* is retried rather than counted as a
    // dropped frame — see [`super::Refusals`].
    let mut refusals = super::Refusals::default();
    loop {
        if interrupted.load(Ordering::Relaxed) {
            break;
        }
        // The control socket is polled between frames: a save is handled at a
        // frame boundary, with the ring consistent, and the loop keeps running
        // afterwards (a replay serves many saves).
        if let Some(stream) = accept_control(listener) {
            if handle_control(stream, recorder, request)? {
                break;
            }
        }
        let now = Instant::now();
        if now < next_frame {
            sleep_interruptible(next_frame - now, interrupted);
            if interrupted.load(Ordering::Relaxed) {
                break;
            }
        }
        let grab_started = Instant::now();
        let grabbed = source.grab(capture, request.cursor, zero_copy, &mut scratch);
        let frame = match grabbed {
            Ok(frame) => frame,
            Err(error) => {
                // A refusal is the compositor declining to be asked, which on
                // KWin comes and goes; anything else is a hiccup.  Neither is
                // fatal on its own.
                if let VshotError::ScreenshotDenied(explanation) = &error {
                    if !refusals.refused(explanation) {
                        return Err(error);
                    }
                } else {
                    consecutive_errors += 1;
                    if consecutive_errors >= 10 {
                        return Err(VshotError::Recording(format!(
                            "giving up after {consecutive_errors} frames in a row failed: {error}"
                        )));
                    }
                    eprintln!("vshot: dropping a frame: {error}");
                }
                let now = Instant::now();
                last_frame_at = now;
                next_frame = now + interval;
                continue;
            }
        };
        consecutive_errors = 0;
        refusals.delivered();
        let duration_ms = {
            covered_us += grab_started
                .saturating_duration_since(last_frame_at)
                .as_micros() as u64;
            last_frame_at = grab_started;
            let due_ms = covered_us / 1000;
            let step = due_ms.saturating_sub(timeline_ms);
            timeline_ms = timeline_ms.max(due_ms);
            u32::try_from(step).unwrap_or(1).max(1)
        };
        match &frame {
            super::Grabbed::Software(frame) => recorder.frame(frame, duration_ms)?,
            super::Grabbed::Dmabuf(dmabuf) => recorder.frame_dmabuf(
                dmabuf.fd,
                dmabuf.fourcc,
                dmabuf.modifier,
                dmabuf.offset as i32,
                dmabuf.stride as i32,
                duration_ms,
            )?,
        }
        super::pump_soundtrack(mic, recorder)?;
        next_frame += interval;
        let now = Instant::now();
        if now > next_frame + interval {
            next_frame = now;
        }
    }
    Ok(())
}

/// Accepts one control connection without blocking, or `None` when none is
/// waiting.
fn accept_control(listener: &UnixListener) -> Option<UnixStream> {
    match listener.accept() {
        Ok((stream, _)) => Some(stream),
        Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => None,
        Err(error) => {
            if debug_enabled() {
                eprintln!("vshot: the replay control socket refused a connection: {error}");
            }
            None
        }
    }
}

/// Handles one control line.  Returns `true` when the session should end.
fn handle_control(
    mut stream: UnixStream,
    recorder: &mut ReplayRecorder,
    request: &ReplayRequest,
) -> Result<bool> {
    let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
    let mut buffer = Vec::new();
    let mut chunk = [0u8; 1024];
    loop {
        match stream.read(&mut chunk) {
            Ok(0) => break,
            Ok(read) => {
                buffer.extend_from_slice(&chunk[..read]);
                if buffer.contains(&b'\n') {
                    break;
                }
            }
            Err(_) => break,
        }
    }
    let command: ReplayCommand = match serde_json::from_slice(&buffer) {
        Ok(command) => command,
        Err(error) => {
            let reply = ReplayReply::Error {
                message: format!("invalid replay control line: {error}"),
            };
            let _ = write_reply(&mut stream, &reply);
            return Ok(false);
        }
    };
    match command {
        ReplayCommand::Save { path, seconds } => {
            let reply = match do_save(recorder, request, path.as_deref(), seconds) {
                Ok((path, seconds)) => ReplayReply::Saved { path, seconds },
                Err(error) => ReplayReply::Error {
                    // The client relays this sentence verbatim, so the
                    // category prefix is stripped here: wrapping a
                    // "recording failed: ..." in another "recording failed:"
                    // reads as a stutter.
                    message: relay_message(&error),
                },
            };
            let _ = write_reply(&mut stream, &reply);
            Ok(false)
        }
        ReplayCommand::Status => {
            let reply = ReplayReply::Status {
                span_seconds: recorder.span_seconds(),
                saves: recorder.saves(),
            };
            let _ = write_reply(&mut stream, &reply);
            Ok(false)
        }
        ReplayCommand::Stop => {
            let _ = write_reply(&mut stream, &ReplayReply::Stopping);
            Ok(true)
        }
    }
}

/// Writes one reply line to the control stream.
fn write_reply(stream: &mut UnixStream, reply: &ReplayReply) -> std::io::Result<()> {
    let mut payload =
        serde_json::to_vec(reply).map_err(|error| std::io::Error::other(error.to_string()))?;
    payload.push(b'\n');
    stream.write_all(&payload)?;
    stream.flush()
}

/// The sentence a client relays for a failed control request: the error's own
/// text with any category prefix (`recording failed: `) removed, because the
/// client prints it as-is and would otherwise stutter the prefix.
fn relay_message(error: &VshotError) -> String {
    let text = error.to_string();
    match text.strip_prefix("recording failed: ") {
        Some(rest) => rest.to_string(),
        None => text,
    }
}

/// Saves the ring to a file and reports the path and length.  This is the
/// trigger's whole cost: a remux of packets already encoded, no re-encode.
fn do_save(
    recorder: &mut ReplayRecorder,
    request: &ReplayRequest,
    requested_path: Option<&Path>,
    seconds: Option<u64>,
) -> Result<(PathBuf, f64)> {
    let path = resolve_save_path(requested_path, request.save_dir.as_deref(), "replay")?;
    // No `--seconds` means the user's window, not the whole ring: the ring
    // holds one key-frame interval more than the window on purpose, and that
    // slack is not what a bare `replay save` asked for.
    let want = seconds.unwrap_or(request.window);
    let (_packets, saved_seconds) = recorder.save(&path, want)?;
    crate::notify::replay_saved(&path, saved_seconds);
    Ok((path, saved_seconds))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_default_frame_interval_follows_the_rate() {
        let request = ReplayRequest {
            target: super::super::RecordTarget::All,
            window: 30,
            fps: 30,
            encoder: VideoCodec::H264,
            encoder_backend: crate::record::avcodec::EncoderBackend::Auto,
            cursor: false,
            mic: None,
            app_audio: false,
            follow: Vec::new(),
            portal: false,
            save_dir: None,
            gop_secs: 1,
        };
        assert_eq!(request.frame_interval(), Duration::from_nanos(33_333_333));
        assert_eq!(request.gop_frames(), 30);
        let mut request2 = request.clone();
        request2.gop_secs = 2;
        assert_eq!(request2.gop_frames(), 60);
    }

    #[test]
    fn the_socket_path_is_separate_from_the_recording_pid_file() {
        // SAFETY: single-threaded test body, and the variable is restored.
        let saved = std::env::var_os("VSHOT_REPLAY_SOCKET");
        std::env::set_var("VSHOT_REPLAY_SOCKET", "/tmp/vshot-replay-probe.sock");
        assert_eq!(socket_path(), PathBuf::from("/tmp/vshot-replay-probe.sock"));
        match saved {
            Some(value) => std::env::set_var("VSHOT_REPLAY_SOCKET", value),
            None => std::env::remove_var("VSHOT_REPLAY_SOCKET"),
        }
    }

    #[test]
    fn a_save_never_claims_more_than_the_ring_holds() {
        // The default a bare `replay save` uses is the user's window; the ring
        // holds a little more (one key-frame interval) on purpose.
        let request = ReplayRequest {
            target: super::super::RecordTarget::All,
            window: 30,
            fps: 30,
            encoder: VideoCodec::H264,
            encoder_backend: crate::record::avcodec::EncoderBackend::Auto,
            cursor: false,
            mic: None,
            app_audio: false,
            follow: Vec::new(),
            portal: false,
            save_dir: None,
            gop_secs: 1,
        };
        assert_eq!(request.window, 30);
    }

    #[test]
    fn the_control_protocol_round_trips() {
        let command = ReplayCommand::Save {
            path: Some(PathBuf::from("/tmp/a.mp4")),
            seconds: Some(10),
        };
        let text = serde_json::to_string(&command).unwrap();
        assert_eq!(
            serde_json::from_str::<ReplayCommand>(&text).unwrap(),
            command
        );
        let reply = ReplayReply::Saved {
            path: PathBuf::from("/tmp/a.mp4"),
            seconds: 10.0,
        };
        let text = serde_json::to_string(&reply).unwrap();
        assert_eq!(serde_json::from_str::<ReplayReply>(&text).unwrap(), reply);
    }

    #[test]
    fn a_save_path_gains_an_mp4_suffix() {
        let path = resolve_save_path(Some(Path::new("/tmp/replay-now")), None, "replay").unwrap();
        assert_eq!(path.extension().unwrap(), "mp4");
    }
}

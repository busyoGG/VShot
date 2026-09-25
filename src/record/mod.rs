// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Screen recording: a frame loop over the existing capture backends, a
//! hardware encoder on the GPU, and an MP4 file written by ffmpeg's own
//! muxer.
//!
//! `record` writes that file; `replay` (`replay.rs`) runs the same frame loop
//! and encoder but keeps the encoded packets in an in-memory ring, writing a
//! file only when a save is triggered — and then as a stream copy, no
//! re-encode.  The two share the frame source, the zero-copy probe, the
//! encoder boundary and the audio side; what differs is the sink.
//!
//! The pieces, and why each is shaped the way it is:
//!
//! * **The frame source is the screenshot backends.**  `vshot record` asks
//!   the same `Capturer` that every screenshot route uses, one output at a
//!   time (`record monitor NAME`) or every output composed into the scene
//!   (`record all`).  That is what keeps the compatibility story honest: a
//!   session where `vshot monitor` works can record, and a session where it
//!   cannot gets the same diagnosis.
//!
//! * **The encoder is VAAPI**, reached through `dlopen`.  The C prototypes
//!   under `target/record-probe/` established the exact contract; the module
//!   docs there explain the traps.
//!
//! * **The container is libavformat's** — the same library that produced the
//!   packets.  An earlier revision wrote the MP4 boxes by hand and rebuilt
//!   avcC/hvcC/av1C out of the bitstream, and that reconstruction is exactly
//!   where container and bitstream drifted apart: the parameter sets a VAAPI
//!   encoder reports at open time are not always the ones it codes against,
//!   and AV1 needed its OBU stream put back together.  Handing the packets
//!   to libavformat, the way wf-recorder does, removes that whole class of
//!   failure.
//!
//! * **Timing is wallclock.**  The loop aims for the requested rate and each
//!   frame carries the real duration it was on screen; the muxer states those
//!   durations (variable frame rate) rather than pretending a nominal rate.  A
//!   recording of a slowly-changing screen therefore plays back at the speed it
//!   happened, and one that drops frames plays back short rather than slow.
//!
//! * **Stopping is a signal.**  `vshot record stop` reads the pid file and
//!   sends SIGTERM, which is the only mechanism that also works from a
//!   compositor keybinding; an interactive terminal's Ctrl+C works because
//!   SIGINT sets the same flag.  Either way the file is finished properly:
//!   libavformat writes the trailer, sample table and index included, before
//!   the process exits.
//!
//! # What a recording looks like on disk
//!
//! `--output PATH` names the file, strftime-expanded like `-o` for
//! screenshots.  The default is `vshot-YYYYmmdd-HHMMSS.mp4` in the videos
//! directory — `$XDG_VIDEOS_DIR`, else the one `~/.config/user-dirs.dirs`
//! names (a localised desktop keeps its videos outside `~/Videos`), else
//! `~/Videos` — and that directory is created when it is missing, because it
//! is vshot's own choice of location rather than the user's.  The final
//! `.mp4` suffix is added
//! when missing — `record` writes MP4 and nothing else for now, and a file
//! that a player tries to open as the wrong container is worse than a
//! suffix the user did not type.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::capture::dmabuf::DmabufFrame;
use crate::capture::Capturer;
use crate::error::{Result, VshotError};
use crate::geometry::Rect;
use crate::model::{Frame, SceneSnapshot};
use crate::wayland::topology::OutputInfo;
use crate::wayland::WaylandSession;

use self::avcodec::{EncoderBackend, Recorder, VideoCodec};

pub mod avcodec;
mod pipewire;
mod pipewire_audio;
mod portal;
pub(crate) mod replay;
mod window;

pub use replay::{
    save as replay_save, status as replay_status, stop as replay_stop, ReplayRequest,
};

/// How many times per second frames are taken at most.  The loop always
/// tries to keep up with this; a slower screen just produces fewer frames.
/// `--fps` overrides it, in 1..=240.
pub const DEFAULT_FPS: u32 = 60;

/// Longest edge the encoder accepts.  A VAAPI driver rejects surfaces above
/// its hardware limit with a clear error, so this is only here to give that
/// message a friendlier shape; 8K is the practical ceiling of current
/// hardware.
const MAX_DIMENSION: u32 = 8192;

/// How long the desktop is given to drop the picker's overlay before the
/// first frame is grabbed: the Qt helper's surfaces leave asynchronously,
/// and a frame taken too early would carry the picker into the video.
const PICK_SETTLE: Duration = Duration::from_millis(200);

/// What `vshot record` is asked to record.
#[derive(Clone, Debug, PartialEq)]
pub enum RecordTarget {
    /// One output by name, or the output under the pointer with `current`.
    Monitor(String),
    /// Every output, composed at its logical position — the video shape of
    /// `vshot all`.
    All,
    /// One rectangle of the desktop: the area `vshot region` would capture,
    /// recorded as it changes.  The rectangle has to sit inside a single
    /// output — no one compositor call copies a region that spans two — and
    /// is picked interactively when it is not fixed with `--geometry`.
    Region(RegionTarget),
    /// One window's *own pixels*, not the screen area it covers: the
    /// compositor copies the window itself, so a window that is covered by
    /// another one, or dragged half off the screen, still records whole.  That
    /// is the difference between this and recording the rectangle the window
    /// sits in, which is what a screen recording of that area would give.
    Window(WindowTarget),
}

/// Which rectangle `record region` records.
#[derive(Clone, Debug, PartialEq)]
pub enum RegionTarget {
    /// A fixed rectangle, in desktop logical coordinates (`x,y widthxheight`,
    /// the same shape `vshot region --geometry` takes).
    Fixed(Rect),
    /// A rectangle framed on the frozen desktop by the same Qt overlay
    /// `vshot region` uses.
    Pick,
}

/// Which window `record window` records.
#[derive(Clone, Debug, PartialEq)]
pub enum WindowTarget {
    /// The focused window, resolved through the compositor's own active-window
    /// query and matched against its window list.
    Active,
    /// The window the user clicks, through the same picker `vshot window pick`
    /// shows.
    Pick,
    /// A window whose app id or title matches this text: the whole name, else
    /// a case-insensitive substring of either.
    Filter(String),
}

/// Which microphone `record --mic` records, if any.
#[derive(Clone, Debug, PartialEq)]
pub enum MicChoice {
    /// The session's default source (what a bare `--mic` means).
    Default,
    /// A named PipeWire node.
    Device(String),
}

/// The remembered microphone setting: `--mic` overrides it, `--no-mic`
/// overrides it the other way, and the config's `record-mic` decides what
/// "neither" means.  Off unless it says otherwise.
pub(crate) fn default_mic() -> Option<MicChoice> {
    let remembered = crate::config::load().record.mic?;
    if remembered.is_empty() {
        Some(MicChoice::Default)
    } else {
        Some(MicChoice::Device(remembered))
    }
}

/// The remembered codec: `--encoder` overrides it, and the config's
/// `record.encoder` decides what "no flag" means.  H.264 unless it says
/// otherwise — so the config can only ever move the default off the codec
/// that is known to work everywhere.
pub(crate) fn default_encoder() -> VideoCodec {
    let Some(remembered) = crate::config::load().record.encoder else {
        return VideoCodec::H264;
    };
    // A name the CLI would refuse is a hand edit gone wrong; the built-in
    // default stands rather than failing every future recording on it.
    VideoCodec::parse(remembered.trim()).unwrap_or(VideoCodec::H264)
}

/// The remembered frame rate: `--fps` overrides it, and the config's
/// `record.fps` decides what "no flag" means.  The CLI's own default
/// (60) unless the file says otherwise; a rate outside 1-240 is the
/// built-in default rather than an error, for the same reason as the
/// encoder above.
pub(crate) fn default_fps() -> u32 {
    match crate::config::load().record.fps {
        Some(fps) if (1..=240).contains(&fps) => fps,
        _ => DEFAULT_FPS,
    }
}

/// The remembered portal switch: `--portal` turns it on, `--no-portal`
/// turns it off, and the config's `record.portal` decides what "neither"
/// means.  Off unless it says otherwise.
pub(crate) fn default_portal() -> bool {
    crate::config::load().record.portal.unwrap_or(false)
}

/// The remembered replay window: the config's `replay.window` when it is a
/// usable value, else the replay default.  A value outside 1-3600 is the
/// built-in default rather than an error, for the same reason as the encoder
/// below.
pub(crate) fn default_replay_window() -> u64 {
    match crate::config::load().replay.window {
        Some(seconds) if (1..=3600).contains(&seconds) => seconds,
        _ => replay::DEFAULT_WINDOW,
    }
}

/// The remembered replay frame rate: the config's `replay.fps` when it is in
/// range, else the replay default (30).
pub(crate) fn default_replay_fps() -> u32 {
    match crate::config::load().replay.fps {
        Some(fps) if (1..=240).contains(&fps) => fps,
        _ => replay::DEFAULT_FPS,
    }
}

/// The remembered replay codec: the config's `replay.encoder`, else h264.
pub(crate) fn default_replay_encoder() -> VideoCodec {
    let Some(remembered) = crate::config::load().replay.encoder else {
        return VideoCodec::H264;
    };
    VideoCodec::parse(remembered.trim()).unwrap_or(VideoCodec::H264)
}

/// The remembered hardware backend: `--encoder-backend` overrides it, and the
/// config's `record.encoder-backend` decides what "no flag" means.  `auto`
/// unless it says otherwise, so a machine with an NVIDIA card records on it
/// without anyone having to name the backend.
pub(crate) fn default_encoder_backend() -> EncoderBackend {
    let Some(remembered) = crate::config::load().record.encoder_backend else {
        return EncoderBackend::Auto;
    };
    EncoderBackend::parse(remembered.trim()).unwrap_or(EncoderBackend::Auto)
}

/// The remembered replay backend: the config's `replay.encoder-backend`, else
/// `auto`, the same shape as the recording side.
pub(crate) fn default_replay_encoder_backend() -> EncoderBackend {
    let Some(remembered) = crate::config::load().replay.encoder_backend else {
        return EncoderBackend::Auto;
    };
    EncoderBackend::parse(remembered.trim()).unwrap_or(EncoderBackend::Auto)
}

/// The remembered replay microphone: the config's `replay.mic`, the same
/// shape as `record.mic` (an empty string is the default source).
pub(crate) fn default_replay_mic() -> Option<MicChoice> {
    let remembered = crate::config::load().replay.mic?;
    if remembered.is_empty() {
        Some(MicChoice::Default)
    } else {
        Some(MicChoice::Device(remembered))
    }
}

/// The remembered replay portal switch: the config's `replay.portal`, off
/// unless it says otherwise.
pub(crate) fn default_replay_portal() -> bool {
    crate::config::load().replay.portal.unwrap_or(false)
}

/// The remembered key-frame distance: the config's `replay.gop` when it is in
/// range, else the replay default (1 second).
pub(crate) fn default_replay_gop() -> u64 {
    match crate::config::load().replay.gop {
        Some(seconds) if (1..=10).contains(&seconds) => seconds,
        _ => replay::DEFAULT_GOP,
    }
}

/// Lists the session's audio capture sources, for `vshot record mics` and
/// the settings window.  This is the "automatic detection" side of the
/// microphone option: the entries are what the session actually has.
pub fn list_mics() -> Result<Vec<pipewire_audio::Source>> {
    pipewire_audio::sources()
}

/// `vshot record mics`: prints what [`list_mics`] found, one source per
/// line, tab-separated as `serial`, `name`, `description`.  The tab shape is
/// the same one the C side hands over — deliberately plain, so both a person
/// and the settings window can read it, and a caller that wants one field
/// can cut on the first two tabs.
pub fn print_mics() -> Result<()> {
    let sources = list_mics()?;
    for source in &sources {
        println!("{}\t{}\t{}", source.serial, source.name, source.description);
    }
    if sources.is_empty() {
        // Not an error: a session with no inputs is a valid session, and a
        // caller piping this into something should get an empty list rather
        // than a failure.  Saying so on stderr is for the person who ran it.
        eprintln!("vshot: this session has no audio inputs to record");
    }
    Ok(())
}

/// A parsed recording request.
#[derive(Clone, Debug, PartialEq)]
pub struct RecordRequest {
    pub target: RecordTarget,
    /// Output path; strftime expanded, `-` means stdout is refused (a video
    /// is not something a terminal carries) and therefore rejected earlier.
    pub output: Option<std::path::PathBuf>,
    pub fps: u32,
    /// Draw the cursor into the frames, like `--cursor` for screenshots.
    pub cursor: bool,
    /// Seconds to record, or `None` to run until stopped.
    pub duration: Option<u64>,
    /// The video codec to encode with; h264 unless `--encoder` says
    /// otherwise.
    pub encoder: VideoCodec,
    /// Which hardware encoder runs the session (`--encoder-backend`): VAAPI,
    /// NVENC, or `auto` to pick whichever the machine has.  VAAPI unless the
    /// config or the flag says otherwise.
    pub encoder_backend: EncoderBackend,
    /// Take the frames from the XDG desktop portal instead of the
    /// compositor's own protocols (`--portal`).
    pub portal: bool,
    /// Record the microphone into the same MP4 (`--mic`), and which input.
    /// `None` is a silent recording — the sound is an opt-in.
    pub mic: Option<MicChoice>,
    /// Record the recorded *window's own* audio (`--app-audio`) instead of the
    /// microphone: the sound of the application the window belongs to, found
    /// by the window's process id.  Only a window recording has a window to
    /// attach it to.
    pub app_audio: bool,
    /// The windows a `--follow` window recording moves between (`--follow
    /// NAME`, repeated): the capture switches to whichever of them the focus
    /// lands on, and stays where it is when the focus is on anything else.
    /// Empty is an ordinary window recording, and the empty case is what keeps
    /// a recording from asking the compositor for the focus at all.
    pub follow: Vec<String>,
}

impl RecordRequest {
    /// The frame interval the loop aims for, from the requested rate.
    fn frame_interval(&self) -> Duration {
        Duration::from_nanos(1_000_000_000 / u64::from(self.fps.max(1)))
    }
}

/// Where a pid file lives: `VSHOT_<NAME>_PIDFILE` overrides, else
/// `$XDG_RUNTIME_DIR/vshot-<name>-<uid>.pid`, else `/tmp`.  A recording and a
/// replay each have their own, so stopping one never signals the other.
pub(crate) fn pid_file_named(name: &str) -> std::path::PathBuf {
    if let Some(override_path) = std::env::var_os(format!("VSHOT_{}_PIDFILE", name.to_uppercase()))
    {
        let path = std::path::PathBuf::from(override_path);
        if !path.as_os_str().is_empty() {
            return path;
        }
    }
    let runtime = std::env::var_os("XDG_RUNTIME_DIR")
        .filter(|value| !value.is_empty())
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| std::path::PathBuf::from("/tmp"));
    runtime.join(format!(
        "vshot-{}-{}.pid",
        name,
        rustix::process::getuid().as_raw()
    ))
}

/// Where the recording's pid file lives.
fn pid_file() -> std::path::PathBuf {
    pid_file_named("record")
}

/// Whether a recording is already running, from the pid file's point of
/// view: a pid that still exists owns the title.
fn running_pid() -> Option<i32> {
    let text = std::fs::read_to_string(pid_file()).ok()?;
    let pid: i32 = text.trim().parse().ok()?;
    // `kill(pid, 0)` — "can this pid be signalled" is the liveness question.
    let alive = unsafe { libc::kill(pid, 0) } == 0;
    alive.then_some(pid)
}

/// Runs `vshot record stop`: signals the recording named by the pid file and
/// waits for the process to leave.
pub fn stop() -> Result<()> {
    let path = pid_file();
    let Some(pid) = running_pid() else {
        return Err(VshotError::Recording(format!(
            "no recording is running (no live pid in {})",
            path.display()
        )));
    };
    unsafe {
        if libc::kill(pid, libc::SIGTERM) != 0 {
            return Err(VshotError::Recording(format!(
                "could not stop the recording (pid {pid}): {}",
                std::io::Error::last_os_error()
            )));
        }
    }
    // The recording writes its trailer (the moov box, sample table and
    // index included) after the signal, which takes a moment for a long
    // file; wait for the pid to disappear rather than returning while the
    // file is still being finished.
    let deadline = Instant::now() + Duration::from_secs(10);
    while Instant::now() < deadline {
        let alive = unsafe { libc::kill(pid, 0) } == 0;
        if !alive {
            let _ = std::fs::remove_file(&path);
            println!("vshot: recording (pid {pid}) stopped");
            return Ok(());
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    Err(VshotError::Recording(format!(
        "the recording (pid {pid}) is still finishing after 10 seconds; it may be writing a long \
         file"
    )))
}

/// The frame source, in both of its shapes.  Sampling happens per frame so
/// the loop and the encoder stay the only timing authorities.
pub(crate) enum Source {
    /// One output, captured alone: `record monitor`.
    Monitor { name: String },
    /// One rectangle of one output, captured alone: `record region`.  The
    /// rectangle is in output-local logical coordinates, which is the space
    /// `capture_output_region` speaks.
    Region {
        name: String,
        local: Rect,
        scale: u32,
    },
    /// Every output composed, at the scene's scale: `record all`.
    Scene { infos: Vec<OutputInfo> },
}

/// What one grab of the loop produced: either a software frame (the
/// compatibility path, or `record all`'s composition) or a dma-buf the
/// compositor just rendered into (the zero-copy fast path).
pub(crate) enum Grabbed {
    Software(Frame),
    Dmabuf(DmabufFrame),
}

impl Source {
    /// Grabs one frame at the request's shape.  The frame's density is the
    /// scale its pixels are in; the composited scene is at the highest scale
    /// of the layout, a single output at its own.
    ///
    /// `zero_copy` asks for the dma-buf path; a source that cannot provide
    /// one (a scene composition, a compositor without linux-dmabuf) answers
    /// with a software frame instead — the fallback is per-frame, so a
    /// recording survives a compositor that changes its mind.
    pub(crate) fn grab(
        &self,
        capture: &mut Capturer,
        cursor: bool,
        zero_copy: bool,
        scratch: &mut SceneScratch,
    ) -> Result<Grabbed> {
        match self {
            Source::Monitor { name } => {
                if zero_copy {
                    if let Some(dmabuf) = capture.capture_output_dmabuf(name, cursor)? {
                        return Ok(Grabbed::Dmabuf(dmabuf));
                    }
                }
                capture.capture_output(name, cursor).map(Grabbed::Software)
            }
            Source::Region { name, local, scale } => {
                if zero_copy {
                    if let Some(dmabuf) = capture.capture_region_dmabuf(name, *local, cursor)? {
                        return Ok(Grabbed::Dmabuf(dmabuf));
                    }
                }
                capture
                    .capture_region(name, *local, *scale, cursor)
                    .map(Grabbed::Software)
            }
            Source::Scene { infos } => {
                let scene = compose(capture, infos, cursor, scratch)?;
                Ok(Grabbed::Software(scene.frame().clone()))
            }
        }
    }

    /// What the loop's zero-copy probe should ask the compositor for, or
    /// `None` when this source cannot use the dma-buf path at all (a scene
    /// composition is built on the CPU by definition).
    pub(crate) fn dmabuf_probe(&self) -> Option<(&str, Option<Rect>)> {
        match self {
            Source::Monitor { name } => Some((name, None)),
            Source::Region { name, local, .. } => Some((name, Some(*local))),
            Source::Scene { .. } => None,
        }
    }
}

/// Reused across `record all` frames so the composed buffer is not
/// reallocated 60 times a second.  `SceneSnapshot` has no incremental
/// interface, so the scene is rebuilt per frame; this keeps only the
/// bookkeeping the rebuild needs.
pub(crate) struct SceneScratch {
    outputs: Vec<crate::model::OutputSnapshot>,
}

impl SceneScratch {
    pub(crate) fn new() -> Self {
        Self {
            outputs: Vec::new(),
        }
    }
}

/// Composes one scene: every output captured, laid out at its logical
/// position, at the layout's highest scale — the same composition `vshot
/// all` produces, per frame.
fn compose(
    capture: &mut Capturer,
    infos: &[OutputInfo],
    cursor: bool,
    scratch: &mut SceneScratch,
) -> Result<SceneSnapshot> {
    scratch.outputs.clear();
    for info in infos {
        let frame = capture.capture_output(&info.name, cursor)?;
        scratch.outputs.push(crate::model::OutputSnapshot::new(
            info.global_id,
            info.name.clone(),
            info.geometry,
            info.scale,
            frame,
        )?);
    }
    SceneSnapshot::from_outputs(std::mem::take(&mut scratch.outputs))
}

/// Runs a recording to completion (a stop signal, `--duration`, or an
/// error).  Returns the file that was written.
pub fn run(request: &RecordRequest) -> Result<std::path::PathBuf> {
    if let Some(pid) = running_pid() {
        return Err(VshotError::Recording(format!(
            "a recording is already running (pid {pid}); stop it with `vshot record stop` first"
        )));
    }
    // The portal route takes its frames from the compositor's screen-cast
    // service instead of its capture protocols, and the compositor's own
    // picker decides what is recorded — so a window recording goes through
    // here too, and the loop is its own.
    if request.portal {
        return portal::run(request);
    }
    // A window recording has a different frame source — the compositor's own
    // copy of one window — so it runs its own loop; everything around it (the
    // output path, the stop signal, the pid file, the report) is shared.
    if let RecordTarget::Window(target) = &request.target {
        return window::run(request, target);
    }

    // --- the frame source and its geometry --------------------------------
    let wayland = WaylandSession::connect()?;
    let topology = wayland.output_infos()?;
    let mut capture = Capturer::connect()?;

    let (source, geometry) =
        resolve_source(&request.target, &mut capture, &topology, request.cursor)?;
    let path = prepare_output_path(request)?;

    // The microphone, when one was asked for: opened before the encoder,
    // because the audio encoder's rate and channel count come from the
    // negotiation and the MP4's audio stream is declared in its header.  It
    // comes after the pick and the region check on purpose — a request that
    // never reaches the encoder must not have opened anything, and a
    // cancelled pick must leave nothing behind.
    let mic = open_soundtrack(request.mic.as_ref())?;
    let mic_format = mic.format();

    // --- the encoder and muxer --------------------------------------------
    let (encoded_width, encoded_height) = geometry;
    // Which hardware encoder this session runs.  `auto` resolves once, here,
    // so the probe, the pool and the open all agree on the backend.
    let backend = request.encoder_backend.resolve();

    // The zero-copy pool: only a single-output source can use it (a composed
    // scene is built on the CPU by definition), and only when the session
    // offers linux-dmabuf buffers of a shape the encoder accepts.  A region
    // probes its own rectangle, so the offer that comes back is the region's
    // pixel size — the shape the pool is built for.  The probe is one extra
    // capture, before the encoder opens.  NVENC has no dma-buf import at all
    // (ffmpeg's CUDA hwcontext maps CUDA memory only), so it records the
    // software path and the probe is skipped outright.
    let dmabuf_fourcc = if backend == EncoderBackend::Nvenc {
        if debug_enabled() {
            eprintln!(
                "vshot: the NVENC backend records the software path (no dma-buf import); \
                 reading frames back through system memory"
            );
        }
        None
    } else {
        probe_zero_copy(&mut capture, &source, encoded_width, encoded_height)
    };
    let recorder = match (dmabuf_fourcc, mic_format) {
        (Some(fourcc), Some(format)) => Recorder::start_dmabuf_mic(
            &path,
            encoded_width,
            encoded_height,
            request.encoder,
            fourcc,
            format,
            backend,
        )?,
        (Some(fourcc), None) => Recorder::start_dmabuf(
            &path,
            encoded_width,
            encoded_height,
            request.encoder,
            fourcc,
            backend,
        )?,
        (None, Some(format)) => Recorder::start_mic(
            &path,
            encoded_width,
            encoded_height,
            request.encoder,
            format,
            backend,
        )?,
        (None, None) => Recorder::start(
            &path,
            encoded_width,
            encoded_height,
            request.encoder,
            backend,
        )?,
    };
    if debug_enabled() {
        eprintln!(
            "vshot: recording {encoded_width}x{encoded_height} at {} fps with {} ({}) through \
             libavcodec {} and libavformat's MP4 muxer",
            request.fps,
            request.encoder.word(),
            backend.word(),
            avcodec::libavcodec_version()
        );
        if recorder.audio_channels() > 0 {
            eprintln!(
                "vshot: microphone soundtrack: {} Hz, {} channel(s), AAC",
                recorder.audio_rate(),
                recorder.audio_channels()
            );
        }
    }

    // --- stopping ---------------------------------------------------------
    let interrupted = install_stop_handler()?;

    write_pid_file()?;
    // Everything before this line is setup, and the streams have been running
    // through it: arming keeps the samples from here on, so the soundtrack
    // starts where the recording does.
    mic.arm();
    // The loop writes the file and reports what went into it; the pid file
    // goes away whatever happened, because a stale pid would make the next
    // `stop` signal an unrelated process.
    let outcome = record_loop(
        &mut capture,
        &source,
        request,
        dmabuf_fourcc.is_some(),
        recorder,
        mic,
        &interrupted,
    );
    let _ = std::fs::remove_file(pid_file());

    report_outcome(outcome, &path)
}

/// The output path, with its directory made when vshot chose it and an error
/// of ours when the user's own directory is missing.  Shared by every
/// recording shape.
fn prepare_output_path(request: &RecordRequest) -> Result<std::path::PathBuf> {
    let path = resolve_output_path(request.output.as_deref())?;
    // The default videos directory is vshot's own choice, so it gets made if
    // it is missing — `~/.config/user-dirs.dirs` can name one that no desktop
    // has created yet.  A path the user named is theirs: a missing directory
    // there is a mistake worth an error of our own rather than the muxer's
    // bare "No such file or directory".
    match path.parent().filter(|dir| !dir.as_os_str().is_empty()) {
        Some(directory) if request.output.is_none() => {
            std::fs::create_dir_all(directory).map_err(|source| {
                VshotError::Recording(format!(
                    "could not create the videos directory {}: {source}",
                    directory.display()
                ))
            })?;
        }
        Some(directory) if !directory.is_dir() => {
            return Err(VshotError::Recording(format!(
                "the directory {} does not exist (create it, or drop --output to record into the \
                 videos directory)",
                directory.display()
            )));
        }
        _ => {}
    }
    Ok(path)
}

/// Opens the microphone when the request asked for one, at the same time the
/// caller opens its frame source.  Returns a silent [`Soundtrack`] when
/// nothing was asked for.
///
/// This is shared by the recording loops that have no single application to
/// attach to (a screen, the whole desktop, a region, the portal): their only
/// audio source is the microphone.
pub(crate) fn open_soundtrack(choice: Option<&MicChoice>) -> Result<pipewire_audio::Soundtrack> {
    open_soundtrack_with(choice, None)
}

/// Builds the recording's whole soundtrack: the microphone the request asked
/// for, plus (when `app` is given) the recorded window's own application
/// audio, summed into one stream.  Either half may be absent.
pub(crate) fn open_soundtrack_with(
    choice: Option<&MicChoice>,
    app: Option<pipewire_audio::Mic>,
) -> Result<pipewire_audio::Soundtrack> {
    let mic = open_microphone_for(choice)?;
    pipewire_audio::Soundtrack::new(mic, app)
}

/// The microphone side of [`open_soundtrack`], shared with the window loop: a
/// window recording takes the same [`MicChoice`], so the opening is written
/// once.
pub(crate) fn open_microphone_for(
    choice: Option<&MicChoice>,
) -> Result<Option<pipewire_audio::Mic>> {
    let Some(choice) = choice else {
        return Ok(None);
    };
    let target = match choice {
        MicChoice::Default => None,
        MicChoice::Device(name) => Some(name.as_str()),
    };
    let mic = pipewire_audio::Mic::open(target)?;
    if debug_enabled() {
        let format = mic.format();
        eprintln!(
            "vshot: microphone {} Hz, {} channel(s)",
            format.rate, format.channels
        );
    }
    Ok(Some(mic))
}

/// Opens the captured window's own audio: the sound of the application the
/// window belongs to.
///
/// The window is named the same way the capture names it (its foreign-toplevel
/// `app_id` and `title`); the pid comes from the compositor's own IPC
/// ([`crate::capture::window_pid`]), and the PipeWire node is the playback
/// stream that pid opened ([`pipewire_audio::app_playback_node`]).  A window
/// whose application is silent right now has no node, which is not an error —
/// the recording goes on without a soundtrack, and the caller is told on
/// stderr.
///
/// This is the whole of `--app-audio`.  It only has a window to attach to; a
/// monitor, region or `all` recording has no single application, and the CLI
/// refuses the combination before reaching here.
pub(crate) fn open_app_audio(
    name: &crate::capture::window_copy::Name,
) -> Result<Option<pipewire_audio::Mic>> {
    let Some(node) = app_audio_node(name)? else {
        eprintln!("vshot: recording without application audio");
        return Ok(None);
    };
    let mic = pipewire_audio::Mic::open(Some(&node))?;
    if debug_enabled() {
        let format = mic.format();
        eprintln!(
            "vshot: application audio (node {node}) {} Hz, {} channel(s)",
            format.rate, format.channels
        );
    }
    Ok(Some(mic))
}

/// The PipeWire playback node of the application the window `name` belongs to,
/// found the way [`open_app_audio`] finds it: the window's pid from the
/// compositor, then the playback stream that pid owns.
///
/// `Ok(None)` is an application that is not playing anything right now (or a
/// compositor that cannot report the window's pid), which is a state rather
/// than an error, and what the caller does about it differs: a recording that
/// is starting goes silent, while a `--follow` switch keeps the soundtrack it
/// already has — moving to the session's default input instead would put the
/// microphone under a window whose audio was asked for, which is worse than a
/// gap.
pub(crate) fn app_audio_node(name: &crate::capture::window_copy::Name) -> Result<Option<String>> {
    let pid = match crate::capture::window_pid::pid_for(name)? {
        Some(pid) => pid,
        None => {
            eprintln!(
                "vshot: this compositor cannot report the pid of the window `{}`, so its audio \
                 cannot be picked out",
                crate::capture::window_pid::describe(name)
            );
            return Ok(None);
        }
    };
    match pipewire_audio::app_playback_node(pid)? {
        Some(node) => Ok(Some(node)),
        None => {
            eprintln!(
                "vshot: the window's application (pid {pid}) is not playing anything right now"
            );
            Ok(None)
        }
    }
}

/// Drains the soundtrack into the recorder: every sample that arrived since
/// the last call is queued and encoded, so the soundtrack follows the video's
/// own frames rather than a timer of its own.  When `--mic` and `--app-audio`
/// were both asked for, the two streams are summed here, sample-for-sample.
pub(crate) fn pump_soundtrack(
    soundtrack: &mut pipewire_audio::Soundtrack,
    recorder: &mut impl crate::record::avcodec::AudioSink,
) -> Result<()> {
    soundtrack.pump(recorder)
}

/// Installs the SIGINT/SIGTERM flag every recording shape stops on.
pub(crate) fn install_stop_handler() -> Result<Arc<AtomicBool>> {
    let interrupted = Arc::new(AtomicBool::new(false));
    signal_hook::flag::register(signal_hook::consts::SIGINT, interrupted.clone())
        .and_then(|_| {
            signal_hook::flag::register(signal_hook::consts::SIGTERM, interrupted.clone())
        })
        .map_err(|error| {
            VshotError::Recording(format!("could not install the stop handler: {error}"))
        })?;
    Ok(interrupted)
}

/// Reports a finished recording: the frame count and length on stderr, a
/// desktop notification, and the failure path when the loop gave up.
pub(crate) fn report_outcome(
    outcome: Result<(usize, f64)>,
    path: &std::path::Path,
) -> Result<std::path::PathBuf> {
    let (frames, seconds) = match outcome {
        Ok(done) => done,
        Err(error) => {
            crate::notify::recording_failed(&error.to_string());
            return Err(error);
        }
    };
    eprintln!(
        "vshot: recorded {frames} frames ({seconds:.1}s) into {}",
        path.display()
    );
    crate::notify::recording_finished(path, frames, seconds);
    Ok(path.to_path_buf())
}

/// The output `current` means: the one the compositor says the user is on.
///
/// A screenshot reads this from the seat, because by then its frozen overlay
/// is on screen and the pointer has entered it.  A recording has nothing of
/// its own on screen — that is the point of it — and a Wayland client learns
/// nothing about the pointer until it owns a surface under it, so the seat can
/// only ever answer "no output" here.  The compositor is asked instead,
/// through the same query a pin already uses to land on the monitor the user
/// is working on (`capture::active_output`): the output under the pointer
/// where the compositor reports one (Hyprland does), the focused output
/// otherwise.
fn current_output(topology: &[OutputInfo]) -> Result<&OutputInfo> {
    let active = crate::capture::active_output::active_output().ok_or_else(|| {
        VshotError::Recording(
            "this compositor does not report which output the user is on; name one \
             (`vshot record monitor NAME`), or record the whole desktop with `vshot record all`"
                .into(),
        )
    })?;
    match_active(&active, topology).ok_or_else(|| {
        VshotError::Recording(format!(
            "the compositor reports the current output as {}, which this session's topology does \
             not describe; name one with `vshot record monitor NAME`",
            active.name.as_deref().unwrap_or("unnamed")
        ))
    })
}

/// Which output a compositor's answer points at: by name where this session
/// knows that name, else by the logical rectangle it occupies.
fn match_active<'a>(
    active: &crate::capture::active_output::ActiveOutput,
    topology: &'a [OutputInfo],
) -> Option<&'a OutputInfo> {
    if let Some(name) = active.name.as_deref() {
        if let Some(info) = topology.iter().find(|info| info.name == name) {
            return Some(info);
        }
    }
    // A compositor that names an output this session cannot see — a stale
    // answer, an output that just went away — still hands over a rectangle to
    // match on.
    let rect = active.rect?;
    topology.iter().find(|info| info.geometry == rect)
}

fn monitor_source(info: &OutputInfo) -> Result<(Source, (u32, u32))> {
    Ok((
        Source::Monitor {
            name: info.name.clone(),
        },
        (info.pixel_size.width, info.pixel_size.height),
    ))
}

/// Resolves what a `record`/`replay` request names into the frame source and
/// the shape it encodes to.  Shared by both loops: a replay records exactly
/// what a recording would, so the target resolution is the same.
///
/// A region frames its rectangle now, on a frozen picture of the desktop,
/// before anything else has happened — a cancelled pick leaves nothing behind.
/// `--geometry` skips the picker and the rectangle is used as given.
pub(crate) fn resolve_source(
    target: &RecordTarget,
    capture: &mut Capturer,
    topology: &[OutputInfo],
    cursor: bool,
) -> Result<(Source, (u32, u32))> {
    let picked_region = match target {
        RecordTarget::Region(RegionTarget::Fixed(region)) => Some(*region),
        RecordTarget::Region(RegionTarget::Pick) => Some(pick_region(capture, topology, cursor)?),
        _ => None,
    };

    match target {
        RecordTarget::Monitor(name) if name == "current" => {
            let info = current_output(topology)?;
            monitor_source(info)
        }
        RecordTarget::Monitor(name) => {
            let info = topology
                .iter()
                .find(|info| &info.name == name)
                .ok_or_else(|| {
                    VshotError::IncompleteTopology(format!("unknown output `{name}`"))
                })?;
            monitor_source(info)
        }
        RecordTarget::All => {
            let width = topology
                .iter()
                .map(|info| info.geometry.right())
                .collect::<Result<Vec<_>>>()?
                .into_iter()
                .max()
                .unwrap_or(0)
                - topology
                    .iter()
                    .map(|info| info.geometry.left())
                    .min()
                    .unwrap_or(0);
            let height = topology
                .iter()
                .map(|info| info.geometry.bottom())
                .collect::<Result<Vec<_>>>()?
                .into_iter()
                .max()
                .unwrap_or(0)
                - topology
                    .iter()
                    .map(|info| info.geometry.top())
                    .min()
                    .unwrap_or(0);
            let scale = topology
                .iter()
                .map(|info| info.scale)
                .max()
                .unwrap_or(1)
                .max(1);
            let (width, height) = (
                u32::try_from(width).map_err(|_| {
                    VshotError::Recording("the desktop layout is too wide to record".into())
                })?,
                u32::try_from(height).map_err(|_| {
                    VshotError::Recording("the desktop layout is too tall to record".into())
                })?,
            );
            let encoded_width = width.saturating_mul(scale);
            let encoded_height = height.saturating_mul(scale);
            Ok((
                Source::Scene {
                    infos: topology.to_vec(),
                },
                (encoded_width, encoded_height),
            ))
        }
        RecordTarget::Region(_) => {
            let region = picked_region.expect("the picker ran before this match");
            region_source(topology, region)
        }
        // A window has its own frame source and its own loop; the caller
        // handles it before this point.
        RecordTarget::Window(_) => Err(VshotError::Recording(
            "a window replay runs its own loop".into(),
        )),
    }
}

/// Probes the compositor's dma-buf offer for a source and, when it is one the
/// encoder can import at the shape asked for, builds the zero-copy pool and
/// returns the fourcc.  `None` means the software path: the offer was absent,
/// y-inverted, the wrong shape, or an unsupported format — every case a
/// recording falls back on rather than failing.
///
/// Shared by the recording and replay loops so both enable zero copy on the
/// same terms.
pub(crate) fn probe_zero_copy(
    capture: &mut Capturer,
    source: &Source,
    encoded_width: u32,
    encoded_height: u32,
) -> Option<u32> {
    let (name, region) = source.dmabuf_probe()?;
    match capture.probe_dmabuf_offer_region(name, region) {
        Ok(Some((fourcc, width, height, y_invert))) => {
            if y_invert {
                if debug_enabled() {
                    eprintln!(
                        "vshot: zero-copy unavailable (compositor renders y-inverted); using \
                         the software path"
                    );
                }
            } else if width != encoded_width || height != encoded_height {
                if debug_enabled() {
                    eprintln!(
                        "vshot: zero-copy offer is {width}x{height}, encoder is \
                         {encoded_width}x{encoded_height}; using the software path"
                    );
                }
            } else if fourcc != crate::capture::dmabuf::DRM_FORMAT_ARGB8888
                && fourcc != crate::capture::dmabuf::DRM_FORMAT_XRGB8888
            {
                if debug_enabled() {
                    eprintln!(
                        "vshot: zero-copy offer is fourcc 0x{fourcc:08x}, which the encoder \
                         cannot import; using the software path"
                    );
                }
            } else if let Err(error) = capture.build_dmabuf_pool(width, height, fourcc) {
                if debug_enabled() {
                    eprintln!(
                        "vshot: building the zero-copy pool failed ({error}); using the \
                               software path"
                    );
                }
            } else {
                if debug_enabled() {
                    eprintln!(
                        "vshot: zero-copy capture enabled ({width}x{height}, fourcc 0x{fourcc:08x})"
                    );
                }
                return Some(fourcc);
            }
        }
        Ok(None) => {
            if debug_enabled() {
                eprintln!("vshot: no linux-dmabuf offer; using the software path");
            }
        }
        Err(error) => {
            if debug_enabled() {
                eprintln!("vshot: dma-buf probe failed ({error}); using the software path");
            }
        }
    }
    None
}

/// The output a region recording will read from, and the frame shape it
/// produces.
///
/// The rectangle is in desktop logical coordinates and has to sit inside a
/// single output — no compositor call copies a region that spans two — so the
/// output is found first and the rectangle is moved into that output's own
/// logical space, which is what the capture speaks.  The frame is the
/// rectangle's own pixels, so the encoded shape is its logical size scaled by
/// that output's scale.
fn region_source(topology: &[OutputInfo], region: Rect) -> Result<(Source, (u32, u32))> {
    if region.is_empty() {
        return Err(VshotError::Recording(
            "the region to record has no area".into(),
        ));
    }
    let output = topology
        .iter()
        .find(|output| region.intersection(output.geometry) == Some(region))
        .ok_or_else(|| {
            VshotError::Recording(
                "the region has to sit inside a single output; no compositor call copies a \
                 region that spans two. Pick a smaller rectangle, or record the whole screen \
                 with `vshot record monitor`"
                    .into(),
            )
        })?;
    let local = Rect::new(
        region.origin.x - output.geometry.origin.x,
        region.origin.y - output.geometry.origin.y,
        region.size.width,
        region.size.height,
    );
    let scale = output.scale.max(1);
    let width = region.size.width.saturating_mul(scale);
    let height = region.size.height.saturating_mul(scale);
    if width > MAX_DIMENSION || height > MAX_DIMENSION {
        return Err(VshotError::Recording(format!(
            "the region encodes to {width}x{height}, beyond the encoder's {MAX_DIMENSION}-pixel \
             limit; pick a smaller rectangle"
        )));
    }
    Ok((
        Source::Region {
            name: output.name.clone(),
            local,
            scale,
        },
        (width, height),
    ))
}

/// Frames the rectangle to record on the frozen desktop, through the same Qt
/// overlay `vshot region` uses.  A cancelled pick comes back as an error, so
/// the caller never reaches the encoder.
///
/// The picker draws over a picture of the desktop, which is why the scene is
/// composed here rather than taken from the loop: the loop has not started
/// yet, and what gets recorded is the rectangle the user framed, not the
/// pixels under it.
fn pick_region(capture: &mut Capturer, topology: &[OutputInfo], cursor: bool) -> Result<Rect> {
    let scene = crate::record::window::picker_scene(capture, topology, cursor)?;
    let region = crate::qt_overlay::select_region(&scene)?;
    let region = crate::selection::validate_selection(&scene, region)?;
    // The picker's surfaces leave asynchronously; a frame grabbed too early
    // would carry them into the video.  The overlay is gone by the time the
    // helper returns, but the compositor may still be compositing its last
    // frame — the same settle the screenshot side uses after its pickers.
    std::thread::sleep(PICK_SETTLE);
    Ok(region)
}

/// Whether the loop traces each stage to stderr (`VSHOT_RECORD_DEBUG=1`).
fn debug_enabled() -> bool {
    std::env::var_os("VSHOT_RECORD_DEBUG").is_some()
}

/// The main loop: grab, encode, mux, sleep the remainder of the interval.
/// Returns the finished file, its frame count and its duration.
fn record_loop(
    capture: &mut Capturer,
    source: &Source,
    request: &RecordRequest,
    zero_copy: bool,
    mut recorder: Recorder,
    mut mic: pipewire_audio::Soundtrack,
    interrupted: &Arc<AtomicBool>,
) -> Result<(usize, f64)> {
    let started = Instant::now();
    let interval = request.frame_interval();
    let mut scratch = SceneScratch {
        outputs: Vec::new(),
    };
    let mut next_frame = started;
    let mut last_frame_at = started;
    let mut consecutive_errors = 0u32;
    // The timeline so far in milliseconds, and the real time those frames
    // covered.  Each frame's duration is the *difference* between the two, so
    // rounding to whole milliseconds cannot accumulate: at 60 fps a frame is
    // 16.67ms, and truncating every one of them to 16 would quietly lose four
    // percent of the recording's length.
    let mut timeline_ms = 0u64;
    let mut covered_us = 0u64;

    loop {
        if interrupted.load(Ordering::Relaxed) {
            break;
        }
        if let Some(seconds) = request.duration {
            if started.elapsed() >= Duration::from_secs(seconds) {
                break;
            }
        }
        // Pace: sleep until this frame's slot.  A slot already past means
        // the machine is behind and the frame goes out immediately; the
        // muxer's per-frame durations carry the real timing either way.
        let now = Instant::now();
        if now < next_frame {
            let wait = next_frame - now;
            // A stop signal must interrupt the sleep, or `stop` on a slow
            // frame rate would hang for a whole interval.
            sleep_interruptible(wait, interrupted);
            if interrupted.load(Ordering::Relaxed) {
                break;
            }
        }
        let grab_started = Instant::now();
        let grabbed = source.grab(capture, request.cursor, zero_copy, &mut scratch);
        let grab_ms = grab_started.elapsed().as_secs_f64() * 1000.0;
        let frame = match grabbed {
            Ok(frame) => frame,
            Err(error) => {
                consecutive_errors += 1;
                if consecutive_errors >= 10 {
                    return Err(VshotError::Recording(format!(
                        "giving up after {consecutive_errors} frames in a row failed: {error}"
                    )));
                }
                // A hiccup (a workspace switch, a busy compositor) is not
                // fatal; the frame is skipped and the loop moves on.
                eprintln!("vshot: dropping a frame: {error}");
                let now = Instant::now();
                last_frame_at = now;
                next_frame = now + interval;
                continue;
            }
        };
        consecutive_errors = 0;
        // The frame's timestamp is the moment its pixels were taken: what
        // counts is how long the *previous* frame was on screen, which is the
        // interval between two grab starts.  Anchoring on the grab's end
        // instead would fold each grab's own duration — and its jitter, a few
        // milliseconds of it — into the timeline: 13ms and 21ms alternating
        // around a true 16.7ms, which is judder a player faithfully reproduces.
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
        let encode_started = Instant::now();
        // The recorder encodes and muxes in one step: the frame's packets go
        // straight into libavformat, so nothing here has to reconstruct a
        // container out of a byte stream.
        match &frame {
            Grabbed::Software(frame) => recorder.frame(frame, duration_ms)?,
            Grabbed::Dmabuf(dmabuf) => recorder.frame_dmabuf(
                dmabuf.fd,
                dmabuf.fourcc,
                dmabuf.modifier,
                dmabuf.offset as i32,
                dmabuf.stride as i32,
                duration_ms,
            )?,
        }
        let encode_ms = encode_started.elapsed().as_secs_f64() * 1000.0;
        // The soundtrack for the interval this frame covered: everything the
        // microphone has produced since the last frame is queued and encoded
        // now, so the audio track grows with the video's own frame clock.
        pump_soundtrack(&mut mic, &mut recorder)?;
        if debug_enabled() {
            let (shape, kind) = match &frame {
                Grabbed::Software(frame) => (
                    format!("{}x{}", frame.size().width, frame.size().height),
                    "software",
                ),
                Grabbed::Dmabuf(dmabuf) => {
                    (format!("{}x{}", dmabuf.width, dmabuf.height), "dmabuf")
                }
            };
            eprintln!(
                "vshot: frame {}: grabbed {shape} ({kind}) in {grab_ms:.1}ms, muxed in \
                 {encode_ms:.1}ms",
                recorder.frames()
            );
        }
        next_frame += interval;
        // More than a frame interval behind: resynchronise rather than
        // sprinting to catch up on a desktop that has moved on.
        let now = Instant::now();
        if now > next_frame + interval {
            next_frame = now;
        }
    }

    // Writing the trailer is what leaves a seekable file behind: the sample
    // table and the index go in here.  A recording that never reaches this
    // point is missing them, which is a file a player reports honestly
    // rather than one that plays as something it is not.
    recorder.finish()?;

    Ok((recorder.frames() as usize, recorder.seconds()))
}

/// `nanosleep` in slices, so a signal's flag is noticed within a few
/// milliseconds even for a long interval.
pub(crate) fn sleep_interruptible(duration: Duration, interrupted: &AtomicBool) {
    let deadline = Instant::now() + duration;
    while Instant::now() < deadline {
        if interrupted.load(Ordering::Relaxed) {
            return;
        }
        let remaining = deadline.saturating_duration_since(Instant::now());
        std::thread::sleep(remaining.min(Duration::from_millis(5)));
    }
}

fn write_pid_file() -> Result<()> {
    write_pid_file_named("record")
}

/// Writes a pid file for the process, so a later `stop` can signal it.  The
/// name picks which file: a recording and a replay each own one.
pub(crate) fn write_pid_file_named(name: &str) -> Result<()> {
    let path = pid_file_named(name);
    std::fs::write(&path, std::process::id().to_string()).map_err(|source| {
        VshotError::Recording(format!(
            "could not write the pid file {}: {source}; set VSHOT_{}_PIDFILE to a writable path",
            path.display(),
            name.to_uppercase()
        ))
    })
}

/// The output path: the user's, strftime-expanded; otherwise the videos
/// directory.  A missing `.mp4` suffix is added — `record` writes MP4, and a
/// suffix mismatch is a worse mistake than an added suffix.  The caller
/// prepares the directory; nothing is created here.
pub(crate) fn resolve_output_path(
    requested: Option<&std::path::Path>,
) -> Result<std::path::PathBuf> {
    let raw = match requested {
        Some(path) => {
            let text = path.to_string_lossy().to_string();
            if text == "-" {
                return Err(VshotError::Recording(
                    "`-` (stdout) cannot carry a recording; give --output a file path".into(),
                ));
            }
            text
        }
        None => {
            let base = videos_directory().ok_or_else(|| {
                VshotError::Recording(
                    "no videos directory is known ($XDG_VIDEOS_DIR, the user-dirs file, or \
                     $HOME/Videos); name the file with --output"
                        .into(),
                )
            })?;
            base.join("vshot-%Y%m%d-%H%M%S.mp4")
                .to_string_lossy()
                .to_string()
        }
    };
    let expanded = chrono::Local::now().format(&raw).to_string();
    let mut path = std::path::PathBuf::from(expanded);
    if path.extension().and_then(|ext| ext.to_str()) != Some("mp4") {
        path.set_extension("mp4");
    }
    Ok(path)
}

/// The videos directory: `$XDG_VIDEOS_DIR`, else the one the user-dirs file
/// names, else `$HOME/Videos`.
pub(crate) fn videos_directory() -> Option<std::path::PathBuf> {
    if let Some(directory) = std::env::var_os("XDG_VIDEOS_DIR") {
        let path = std::path::PathBuf::from(directory);
        if !path.as_os_str().is_empty() {
            return Some(path);
        }
    }
    // xdg-user-dirs keeps the localised name here (`$HOME/视频` on this
    // machine).  Without reading it the answer would be `~/Videos`, a
    // directory a non-English desktop never creates.
    if let Some(path) = user_dirs_videos() {
        return Some(path);
    }
    std::env::var_os("HOME").map(|home| std::path::PathBuf::from(home).join("Videos"))
}

/// `XDG_VIDEOS_DIR` from `$XDG_CONFIG_HOME/user-dirs.dirs`, with `$HOME`
/// expanded the way that file writes it.
fn user_dirs_videos() -> Option<std::path::PathBuf> {
    let config = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|value| !value.is_empty())
        .map(std::path::PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME").map(|home| std::path::PathBuf::from(home).join(".config"))
        })?;
    let text = std::fs::read_to_string(config.join("user-dirs.dirs")).ok()?;
    let value = user_dirs_value(&text, "XDG_VIDEOS_DIR")?;
    let expanded = match value.strip_prefix("$HOME") {
        Some(rest) => {
            let home = std::env::var("HOME").ok()?;
            format!("{home}{rest}")
        }
        None => value,
    };
    let path = std::path::PathBuf::from(expanded);
    if path.as_os_str().is_empty() {
        None
    } else {
        Some(path)
    }
}

/// The value of one `XDG_..._DIR="..."` line: comments, blanks and every
/// other key are skipped, and the quotes come off.
fn user_dirs_value(text: &str, key: &str) -> Option<String> {
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let Some((name, value)) = line.split_once('=') else {
            continue;
        };
        if name.trim() != key {
            continue;
        }
        let value = value.trim().trim_matches('"');
        if !value.is_empty() {
            return Some(value.to_string());
        }
    }
    None
}

/// The rectangle of an output inside the composed scene, for `record all`'s
/// geometry — kept for the tests, which check that the composition matches
/// the layout the topology describes.
#[allow(dead_code)]
fn scene_bounds(infos: &[OutputInfo]) -> Result<Rect> {
    let left = infos
        .iter()
        .map(|info| info.geometry.left())
        .min()
        .unwrap_or(0);
    let top = infos
        .iter()
        .map(|info| info.geometry.top())
        .min()
        .unwrap_or(0);
    let right = infos
        .iter()
        .map(|info| info.geometry.right())
        .collect::<Result<Vec<_>>>()?
        .into_iter()
        .max()
        .unwrap_or(0);
    let bottom = infos
        .iter()
        .map(|info| info.geometry.bottom())
        .collect::<Result<Vec<_>>>()?
        .into_iter()
        .max()
        .unwrap_or(0);
    let width = u32::try_from(right - left)
        .map_err(|_| VshotError::Recording("the desktop layout is too wide".into()))?;
    let height = u32::try_from(bottom - top)
        .map_err(|_| VshotError::Recording("the desktop layout is too tall".into()))?;
    Ok(Rect::new(left, top, width, height))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::capture::active_output::ActiveOutput;

    #[test]
    fn the_frame_interval_follows_the_rate() {
        let mut request = RecordRequest {
            target: RecordTarget::All,
            output: None,
            fps: 60,
            cursor: false,
            duration: None,
            encoder: VideoCodec::H264,
            encoder_backend: EncoderBackend::Auto,
            portal: false,
            mic: None,
            app_audio: false,
            follow: Vec::new(),
        };
        assert_eq!(request.frame_interval(), Duration::from_nanos(16_666_666));
        request.fps = 30;
        assert_eq!(request.frame_interval(), Duration::from_nanos(33_333_333));
        // A zero rate cannot divide by zero; the clamp keeps one frame per
        // second rather than a panic.
        request.fps = 0;
        assert_eq!(request.frame_interval(), Duration::from_secs(1));
    }

    #[test]
    fn a_directory_becomes_a_file_with_an_mp4_suffix() {
        let path = resolve_output_path(Some(std::path::Path::new("/tmp/a/b.mp4"))).unwrap();
        assert_eq!(path.extension().unwrap(), "mp4");
        // A suffixless path gains one; the format is not a guess.
        let path = resolve_output_path(Some(std::path::Path::new("/tmp/vshot-now"))).unwrap();
        assert_eq!(path.extension().unwrap(), "mp4");
        assert!(path.to_string_lossy().starts_with("/tmp/vshot-now"));
    }

    #[test]
    fn stdout_is_refused_for_a_recording() {
        let error = resolve_output_path(Some(std::path::Path::new("-"))).unwrap_err();
        assert!(error.to_string().contains("stdout"), "{error}");
    }

    #[test]
    fn the_videos_line_is_read_out_of_a_user_dirs_file() {
        let text = "# written by xdg-user-dirs-update\n\
                    XDG_DESKTOP_DIR=\"$HOME/桌面\"\n\
                    XDG_PICTURES_DIR=\"$HOME/图片\"\n\
                    XDG_VIDEOS_DIR=\"$HOME/视频\"\n\
                    XDG_PROJECTS_DIR=\"$HOME/项目\"\n";
        assert_eq!(
            user_dirs_value(text, "XDG_VIDEOS_DIR").as_deref(),
            Some("$HOME/视频")
        );
        // Another key is not the answer, and neither is a missing line.
        assert_eq!(user_dirs_value(text, "XDG_MUSIC_DIR"), None);
        assert_eq!(user_dirs_value("", "XDG_VIDEOS_DIR"), None);
        // An absolute path is kept as it stands.
        assert_eq!(
            user_dirs_value("XDG_VIDEOS_DIR=\"/mnt/rec\"\n", "XDG_VIDEOS_DIR").as_deref(),
            Some("/mnt/rec")
        );
    }

    #[test]
    fn the_current_output_matches_by_name_then_by_rect() {
        let topology = [
            output(1, "DP-3", Rect::new(0, 0, 1920, 1080)),
            output(2, "DP-2", Rect::new(1920, 0, 1920, 1080)),
        ];
        // The name is what the compositor knows the output by, and it wins
        // even when the rectangle would point somewhere else.
        let active = ActiveOutput {
            name: Some("DP-2".to_owned()),
            rect: Some(Rect::new(0, 0, 1920, 1080)),
        };
        assert_eq!(match_active(&active, &topology).unwrap().name, "DP-2");
        // A name this session cannot see falls back to the rectangle.
        let active = ActiveOutput {
            name: Some("HDMI-A-1".to_owned()),
            rect: Some(Rect::new(1920, 0, 1920, 1080)),
        };
        assert_eq!(match_active(&active, &topology).unwrap().name, "DP-2");
        // Nothing to go on: no output, and the caller says so rather than
        // recording the wrong screen.
        let active = ActiveOutput {
            name: Some("HDMI-A-1".to_owned()),
            rect: None,
        };
        assert!(match_active(&active, &topology).is_none());
        assert!(match_active(&ActiveOutput::default(), &topology).is_none());
    }

    #[test]
    fn a_region_resolves_to_its_output_and_its_own_pixels() {
        let mut topology = [
            output(1, "DP-3", Rect::new(0, 0, 1920, 1080)),
            output(2, "DP-2", Rect::new(1920, 0, 3840, 2160)),
        ];
        // The rectangle is moved into its output's own logical space, and the
        // frame shape is the region's pixels, scaled by that output's scale.
        topology[1].scale = 2;
        topology[1].pixel_size = crate::geometry::Size::new(7680, 4320);
        let (source, (width, height)) =
            region_source(&topology, Rect::new(2020, 100, 400, 300)).unwrap();
        match source {
            Source::Region { name, local, scale } => {
                assert_eq!(name, "DP-2");
                assert_eq!(local, Rect::new(100, 100, 400, 300));
                assert_eq!(scale, 2);
            }
            _ => panic!("expected a region source"),
        }
        assert_eq!((width, height), (800, 600));

        // A rectangle that hangs off its output is refused rather than
        // silently clipped: no compositor call copies a region across two
        // outputs, and a clipped recording would not be what was drawn.
        let error = match region_source(&topology, Rect::new(1900, 100, 200, 200)) {
            Err(error) => error,
            Ok(_) => panic!("a region across two outputs must be refused"),
        };
        assert!(error.to_string().contains("single output"), "{error}");
        let error = match region_source(&topology, Rect::new(0, 0, 0, 0)) {
            Err(error) => error,
            Ok(_) => panic!("an empty region must be refused"),
        };
        assert!(error.to_string().contains("no area"), "{error}");
    }

    /// One output of a laid-out desktop, for the matching tests above.
    fn output(global_id: u32, name: &str, geometry: Rect) -> OutputInfo {
        OutputInfo {
            global_id,
            name: name.to_owned(),
            geometry,
            pixel_size: geometry.size,
            scale: 1,
            transform: wayland_client::protocol::wl_output::Transform::Normal,
        }
    }

    #[test]
    fn the_default_path_expands_strftime() {
        // Only the shape is checked: the exact second depends on the clock.
        std::env::set_var("XDG_VIDEOS_DIR", "/tmp/vshot-videos");
        let path = resolve_output_path(None).unwrap();
        let name = path.file_name().unwrap().to_string_lossy().to_string();
        assert!(name.starts_with("vshot-"), "{name}");
        assert!(name.ends_with(".mp4"), "{name}");
        assert!(!name.contains('%'), "strftime must be expanded: {name}");
        assert_eq!(
            path.parent().unwrap(),
            std::path::Path::new("/tmp/vshot-videos")
        );
        std::env::remove_var("XDG_VIDEOS_DIR");
    }
}

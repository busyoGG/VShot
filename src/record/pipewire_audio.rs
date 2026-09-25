// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! The Rust side of the microphone client (`pipewire_audio.c`).
//!
//! `record --mic` records a soundtrack beside the video.  The microphone is a
//! PipeWire source node, and the same libpipewire the portal's screen cast
//! already uses reads it — a capture stream whose buffers arrive on a PipeWire
//! thread and are copied into a ring the recording loop drains between
//! frames.  [`Mic`] is the RAII wrapper; the C file explains the shape.
//!
//! Two things about the boundary:
//!
//! * **The samples are borrowed per read, not owned per buffer.**  The C side
//!   copies every buffer into its own ring (an audio stream cannot drop
//!   samples the way a screen cast drops frames), so [`Mic::read`] fills a
//!   caller-provided buffer and returns how many frames it wrote: 0 is
//!   "nothing yet", which is the normal state between two video frames.
//!
//! * **Arming is a line in the timeline.**  The stream is opened before the
//!   video encoder is (the audio encoder needs its rate and channel count at
//!   open time), so the first moments of audio belong to no recording;
//!   [`Mic::arm`] drops what is already buffered and keeps everything after.
//!
//! Frame counts here are *per channel*: `read` fills `frames * channels`
//! floats, interleaved, which is what the AAC encoder's staging buffer wants.

use std::ffi::{c_char, c_int, CStr};
use std::time::Duration;

use crate::error::{Result, VshotError};

/// The opaque client from `pipewire_audio.c`.
#[repr(C)]
struct VshotPwa {
    _private: [u8; 0],
}

/// The C client, when the build had libpipewire's headers.
#[cfg(vshot_pipewire)]
mod binding {
    use super::{c_char, c_int, VshotPwa};

    extern "C" {
        pub(super) fn vshot_pwa_available() -> c_int;
        pub(super) fn vshot_pwa_load_error() -> *const c_char;
        pub(super) fn vshot_pwa_list_sources(
            out: *mut c_char,
            out_len: c_int,
            timeout_ms: c_int,
            err: *mut c_char,
            err_len: c_int,
        ) -> c_int;
        pub(super) fn vshot_pwa_find_app_node(
            pid: *const c_char,
            out: *mut c_char,
            out_len: c_int,
            timeout_ms: c_int,
            err: *mut c_char,
            err_len: c_int,
        ) -> c_int;
        pub(super) fn vshot_pwa_open(
            target: *const c_char,
            timeout_ms: c_int,
            err: *mut c_char,
            err_len: c_int,
        ) -> *mut VshotPwa;
        pub(super) fn vshot_pwa_geometry(
            pw: *mut VshotPwa,
            rate: *mut c_int,
            channels: *mut c_int,
        ) -> c_int;
        pub(super) fn vshot_pwa_arm(pw: *mut VshotPwa);
        pub(super) fn vshot_pwa_read(pw: *mut VshotPwa, out: *mut f32, frames: c_int) -> c_int;
        pub(super) fn vshot_pwa_queued_frames(pw: *mut VshotPwa) -> c_int;
        pub(super) fn vshot_pwa_error(pw: *mut VshotPwa) -> *const c_char;
        pub(super) fn vshot_pwa_close(pw: *mut VshotPwa);
    }
}

/// The same entry points, for a build that had no libpipewire headers: every
/// one of them answers "there is no client", so `--mic` fails with a sentence
/// naming the package instead of a link error at build time.
#[cfg(not(vshot_pipewire))]
mod binding {
    use super::{c_char, c_int, VshotPwa};

    /// The message every entry point answers with; a `&'static` string so the
    /// pointer it hands out stays valid.
    const MESSAGE: &[u8] =
        b"this build was made without libpipewire's headers, so the microphone cannot be \
read; install libpipewire and rebuild\n\0";

    fn message() -> *const c_char {
        MESSAGE.as_ptr().cast::<c_char>()
    }

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pwa_available() -> c_int {
        0
    }

    pub(super) unsafe fn vshot_pwa_load_error() -> *const c_char {
        message()
    }

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pwa_list_sources(
        out: *mut c_char,
        out_len: c_int,
        timeout_ms: c_int,
        err: *mut c_char,
        err_len: c_int,
    ) -> c_int {
        -1
    }

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pwa_find_app_node(
        pid: *const c_char,
        out: *mut c_char,
        out_len: c_int,
        timeout_ms: c_int,
        err: *mut c_char,
        err_len: c_int,
    ) -> c_int {
        -1
    }

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pwa_open(
        target: *const c_char,
        timeout_ms: c_int,
        err: *mut c_char,
        err_len: c_int,
    ) -> *mut VshotPwa {
        std::ptr::null_mut()
    }

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pwa_geometry(
        pw: *mut VshotPwa,
        rate: *mut c_int,
        channels: *mut c_int,
    ) -> c_int {
        -1
    }

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pwa_arm(pw: *mut VshotPwa) {}

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pwa_read(pw: *mut VshotPwa, out: *mut f32, frames: c_int) -> c_int {
        -1
    }

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pwa_queued_frames(pw: *mut VshotPwa) -> c_int {
        -1
    }

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pwa_error(pw: *mut VshotPwa) -> *const c_char {
        message()
    }

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pwa_close(pw: *mut VshotPwa) {}
}

/// Whether the microphone can be read at all: libpipewire present at run
/// time, and the client compiled in.
pub fn available() -> bool {
    unsafe { binding::vshot_pwa_available() != 0 }
}

/// Why it cannot be, when it cannot.
pub fn load_error() -> String {
    take(unsafe { binding::vshot_pwa_load_error() })
}

/// One audio capture source, as `vshot record mics` lists it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Source {
    /// The PipeWire object serial, which `--mic` accepts as a name.
    pub serial: String,
    /// The node name (`alsa_input...`), the stable handle for the config.
    pub name: String,
    /// The human-readable description, for showing a person.
    pub description: String,
}

/// Lists the session's audio capture sources.  An empty session is an empty
/// list, not an error; `Err` means the session could not be reached at all.
pub fn sources() -> Result<Vec<Source>> {
    if !available() {
        return Err(VshotError::Recording(format!(
            "listing the microphones needs libpipewire: {}",
            load_error()
        )));
    }
    // 64 KiB is room for a few hundred sources at ~100 bytes a line; a
    // session that fills it is one this list was never going to be read
    // from, and the C side leaves out what does not fit.
    let mut buffer = vec![0 as c_char; 64 * 1024];
    let mut error = [0 as c_char; 512];
    let count = unsafe {
        binding::vshot_pwa_list_sources(
            buffer.as_mut_ptr(),
            buffer.len() as c_int,
            0,
            error.as_mut_ptr(),
            error.len() as c_int,
        )
    };
    if count < 0 {
        return Err(VshotError::Recording(format!(
            "could not list the microphones: {}",
            take(error.as_ptr())
        )));
    }
    let text = take(buffer.as_ptr());
    let sources = text
        .lines()
        .filter_map(|line| {
            // `serial \t name \t description`; a line that does not carry all
            // three fields is one the C side would not have written, so a
            // malformed line is skipped rather than made into a bad source.
            let mut fields = line.splitn(3, '\t');
            let serial = fields.next()?;
            let name = fields.next()?;
            let description = fields.next()?;
            Some(Source {
                serial: serial.to_owned(),
                name: name.to_owned(),
                description: description.to_owned(),
            })
        })
        .collect();
    Ok(sources)
}

fn take(pointer: *const c_char) -> String {
    if pointer.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(pointer) }
        .to_string_lossy()
        .into_owned()
}

/// The PipeWire node serial one application is playing into, found by its
/// process id (`application.process.id`).  `None` means the application has no
/// playback stream right now — a window that is silent, or one whose audio is
/// on another process — which is a state, not an error.  `Err` is a session
/// that could not be read.
pub fn app_playback_node(pid: i32) -> Result<Option<String>> {
    if !available() {
        return Err(VshotError::Recording(format!(
            "recording an application's audio needs libpipewire: {}",
            load_error()
        )));
    }
    let pid = std::ffi::CString::new(pid.to_string())
        .map_err(|_| VshotError::Recording("the pid contains a NUL byte".into()))?;
    let mut buffer = [0 as c_char; 64];
    let mut error = [0 as c_char; 512];
    let status = unsafe {
        binding::vshot_pwa_find_app_node(
            pid.as_ptr(),
            buffer.as_mut_ptr(),
            buffer.len() as c_int,
            super::pipewire::milliseconds(OPEN_TIMEOUT),
            error.as_mut_ptr(),
            error.len() as c_int,
        )
    };
    match status {
        1 => Ok(Some(take(buffer.as_ptr()))),
        0 => Ok(None),
        _ => Err(VshotError::Recording(format!(
            "could not look up the application's audio node: {}",
            take(error.as_ptr())
        ))),
    }
}

/// The microphone's negotiated shape: the rate and channel count the audio
/// encoder must be opened with.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Format {
    pub rate: u32,
    pub channels: u32,
}

/// A connected microphone stream.
pub struct Mic {
    handle: *mut VshotPwa,
    format: Format,
}

// The C client owns a PipeWire thread loop with its own synchronisation, and
// the handle is only ever used from the thread that opened it; moving that
// thread's ownership between threads is sound, exactly as for `Stream`.
unsafe impl Send for Mic {}

/// How long one opening may take.  The negotiation is a couple of round trips
/// on the local socket, and the source is a device that is already running;
/// seconds are already generous, and a deadline that is never reached is
/// better than one that cuts a slow session off.
const OPEN_TIMEOUT: Duration = Duration::from_secs(15);

impl Mic {
    /// Opens the microphone: the named node when `target` is given, else
    /// whatever the session's policy makes the default source.
    pub fn open(target: Option<&str>) -> Result<Mic> {
        if !available() {
            return Err(VshotError::Recording(format!(
                "recording the microphone needs libpipewire: {}",
                load_error()
            )));
        }
        let target = match target {
            Some(name) if !name.trim().is_empty() => {
                std::ffi::CString::new(name).map_err(|_| {
                    VshotError::Recording("the microphone name contains a NUL byte".into())
                })?
            }
            _ => std::ffi::CString::new("").expect("an empty name has no NUL"),
        };
        // The C client writes its own sentence here, because it is the side
        // that knows which stage failed.
        let mut buffer = [0 as c_char; 512];
        let handle = unsafe {
            binding::vshot_pwa_open(
                target.as_ptr(),
                super::pipewire::milliseconds(OPEN_TIMEOUT),
                buffer.as_mut_ptr(),
                buffer.len() as c_int,
            )
        };
        if handle.is_null() {
            let detail = take(buffer.as_ptr());
            // A session with no default input answers "no target node
            // available", which says nothing about what to do; name the
            // command that lists the sources.
            let hint = if detail.contains("no target node") {
                " — this session has no default input device; name one \
                 (`wpctl status` lists the sources, and a bare number is a node serial)"
            } else {
                ""
            };
            return Err(VshotError::Recording(format!(
                "could not open the microphone: {detail}{hint}"
            )));
        }
        let (mut rate, mut channels) = (0 as c_int, 0 as c_int);
        let status = unsafe { binding::vshot_pwa_geometry(handle, &mut rate, &mut channels) };
        if status != 0 || rate <= 0 || channels <= 0 {
            unsafe { binding::vshot_pwa_close(handle) };
            return Err(VshotError::Recording(
                "the microphone stream has no audio format".into(),
            ));
        }
        Ok(Mic {
            handle,
            format: Format {
                rate: rate as u32,
                channels: channels as u32,
            },
        })
    }

    /// The negotiated rate and channel count.
    pub fn format(&self) -> Format {
        self.format
    }

    /// The line between "before the recording" and "the recording": samples
    /// from now on are kept, whatever was buffered is dropped.  Called once
    /// the video side is ready to write frames.
    pub fn arm(&self) {
        unsafe { binding::vshot_pwa_arm(self.handle) };
    }

    /// Reads up to `out.len() / channels` frames into `out`, interleaved.
    /// Returns the number of frames written; 0 means nothing arrived yet,
    /// which between two video frames is the normal state, not a failure.
    pub fn read(&self, out: &mut [f32]) -> Result<usize> {
        let channels = self.format.channels as usize;
        if channels == 0 || out.is_empty() {
            return Ok(0);
        }
        let frames = c_int::try_from(out.len() / channels).unwrap_or(c_int::MAX);
        let status = unsafe { binding::vshot_pwa_read(self.handle, out.as_mut_ptr(), frames) };
        if status < 0 {
            return Err(VshotError::Recording(format!(
                "the microphone stream ended: {}",
                take(unsafe { binding::vshot_pwa_error(self.handle) })
            )));
        }
        Ok(status as usize)
    }

    /// Frames waiting to be read right now, without reading any.  `0` is
    /// "nothing has arrived yet", not a failure.
    ///
    /// This is what lets two streams be summed sample-for-sample: the mixer
    /// reads `min` of the two counts from both, so neither can slide ahead of
    /// the other and the samples added together always come from the same
    /// span of wall-clock time.
    pub fn queued(&self) -> Result<usize> {
        let frames = unsafe { binding::vshot_pwa_queued_frames(self.handle) };
        if frames < 0 {
            return Err(VshotError::Recording(format!(
                "the microphone stream ended: {}",
                take(unsafe { binding::vshot_pwa_error(self.handle) })
            )));
        }
        Ok(frames as usize)
    }
}

impl Drop for Mic {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { binding::vshot_pwa_close(self.handle) };
            self.handle = std::ptr::null_mut();
        }
    }
}

/// The recording's whole soundtrack: the microphone and, when `--app-audio`
/// asked for it, the recorded window's own application audio — summed into
/// one stream.
///
/// One MP4 holds one audio track here, and a person wants to hear their own
/// voice *and* the game, so the two streams are summed sample-for-sample
/// rather than written as two tracks most players would play only the first
/// of.  Both streams negotiate the same shape — `pipewire_audio.c` asks for
/// 48 kHz stereo float — so the sum is an exact sample-wise addition and needs
/// no resampling; the shapes are compared here and a mismatch is refused
/// rather than quietly misplayed.
///
/// The microphone never moves once opened; the application stream does, on a
/// `--follow` switch, and [`set_app`] is how the loop swaps it without
/// touching the microphone beside it.
///
/// [`set_app`]: Soundtrack::set_app
pub struct Soundtrack {
    /// The microphone, when one was asked for and opened.
    mic: Option<Mic>,
    /// The recorded window's own application audio, when `--app-audio` asked
    /// for it and the window's application had a playback stream to attach
    /// to.
    app: Option<Mic>,
    /// The shape the mix is done in: the rate and channel count of whichever
    /// streams are present (they are required to agree).
    format: Option<Format>,
    /// One buffer per side, reused across frames so a recording allocates
    /// nothing per frame.
    mic_scratch: Vec<f32>,
    app_scratch: Vec<f32>,
}

impl Soundtrack {
    /// Wraps the streams that were opened, or answers an error when the two do
    /// not share a shape (they always do in practice, because the C side asks
    /// PipeWire for the same one on both — a difference would mean the
    /// negotiation answered something unexpected).
    pub fn new(mic: Option<Mic>, app: Option<Mic>) -> Result<Soundtrack> {
        let format = match (&mic, &app) {
            (Some(a), Some(b)) if a.format != b.format => {
                return Err(VshotError::Recording(format!(
                    "the microphone ({} Hz, {} channel(s)) and the application audio ({} Hz, {} \
                     channel(s)) negotiated different shapes, so they cannot be summed",
                    a.format.rate, a.format.channels, b.format.rate, b.format.channels
                )))
            }
            (Some(a), _) => Some(a.format),
            (None, Some(b)) => Some(b.format),
            (None, None) => None,
        };
        // One second of audio in one buffer: far more than any frame interval
        // produces, so the ring is drained in one or two reads and the buffer
        // never has to grow again.
        let room = format
            .map(|format| format.rate.max(8000) as usize * format.channels.max(1) as usize)
            .unwrap_or(0);
        Ok(Soundtrack {
            mic,
            app,
            format,
            mic_scratch: vec![0.0; room],
            app_scratch: vec![0.0; room],
        })
    }

    /// Whether any audio is being kept at all.  A silent recording opens no
    /// audio encoder and declares no audio stream.
    pub fn is_empty(&self) -> bool {
        self.mic.is_none() && self.app.is_none()
    }

    /// The negotiated shape the audio encoder has to be opened with, or `None`
    /// for a silent recording.
    pub fn format(&self) -> Option<Format> {
        self.format
    }

    /// The line between "before the recording" and "the recording": every
    /// stream drops what it has buffered and keeps what follows, so the first
    /// sample of the file is the first sample after this call.
    pub fn arm(&self) {
        if let Some(mic) = &self.mic {
            mic.arm();
        }
        if let Some(app) = &self.app {
            app.arm();
        }
    }

    /// Replaces the application stream — the `--follow` switch's new window's
    /// own audio — leaving the microphone untouched.  An error is returned
    /// when the new stream does not share the mix's shape; the caller reports
    /// it and keeps the stream it had.
    ///
    /// The outgoing stream is drained into `recorder` first, *paired with the
    /// microphone*: during the round trip that opens the new stream the loop
    /// does not pump, and both streams buffer that whole interval.  Draining
    /// the old application alone would feed that interval once, and the
    /// microphone would then feed the same interval again through its own
    /// backlog — the audio would come out a second too long for every switch.
    /// Pairing them here counts the interval once, as the sum it should be.
    /// (Before that fix, a single switch added ~1.03 seconds of audio to an
    /// 8-second recording.)
    pub fn set_app(
        &mut self,
        app: Option<Mic>,
        recorder: &mut impl crate::record::avcodec::AudioSink,
    ) -> Result<()> {
        if let (Some(format), Some(new)) = (self.format, &app) {
            if new.format != format {
                return Err(VshotError::Recording(format!(
                    "the new window's audio ({} Hz, {} channel(s)) does not match the \
                     recording's ({} Hz, {} channel(s))",
                    new.format.rate, new.format.channels, format.rate, format.channels
                )));
            }
        }
        // Drain the stream being replaced before it is dropped, so the audio
        // up to the switch reaches the file — paired with the microphone when
        // there is one.
        if let Some(format) = self.format {
            let channels = format.channels.max(1) as usize;
            let room = self.app_scratch.len() / channels;
            if let Some(old) = self.app.take() {
                match &self.mic {
                    Some(mic) => {
                        while mix_pair(
                            mic,
                            &old,
                            recorder,
                            &mut self.mic_scratch,
                            &mut self.app_scratch,
                            channels,
                            room,
                        )? > 0
                        {}
                    }
                    None => Self::drain(&old, recorder, &mut self.app_scratch, channels, room)?,
                }
            }
        }
        // Arm the replacement straight away: its samples before this point
        // belong to the previous window's application, and the recording's
        // timeline has no place for them.
        if let Some(new) = &app {
            new.arm();
        }
        self.app = app;
        Ok(())
    }

    /// Drains every stream and feeds the recorder the sum: whatever arrived
    /// since the last call is queued and encoded now, so the soundtrack grows
    /// with the video's own frame clock rather than a timer of its own.
    pub fn pump(&mut self, recorder: &mut impl crate::record::avcodec::AudioSink) -> Result<()> {
        let Some(format) = self.format else {
            return Ok(());
        };
        let channels = format.channels.max(1) as usize;
        let room = self.mic_scratch.len() / channels;
        match (&self.mic, &self.app) {
            (Some(mic), None) => Self::drain(mic, recorder, &mut self.mic_scratch, channels, room)?,
            (None, Some(app)) => Self::drain(app, recorder, &mut self.app_scratch, channels, room)?,
            (Some(mic), Some(app)) => Self::drain_summed(
                mic,
                app,
                recorder,
                &mut self.mic_scratch,
                &mut self.app_scratch,
                channels,
                room,
            )?,
            (None, None) => return Ok(()),
        }
        recorder.audio_pump()
    }

    /// Passes one stream through unchanged: a recording with a single source
    /// has nothing to sum.
    fn drain(
        mic: &Mic,
        recorder: &mut impl crate::record::avcodec::AudioSink,
        scratch: &mut [f32],
        channels: usize,
        room: usize,
    ) -> Result<()> {
        loop {
            let queued = mic.queued()?;
            if queued == 0 {
                break;
            }
            let want = queued.min(room);
            let read = mic.read(&mut scratch[..want * channels])?;
            if read == 0 {
                break;
            }
            recorder.audio_feed(&scratch[..read * channels], read)?;
        }
        Ok(())
    }

    /// Sums the two streams sample-for-sample, over the span of wall-clock
    /// time both of them can supply.
    ///
    /// The two streams are written by two independent PipeWire callbacks, so
    /// they are never byte-aligned: at any instant one is a graph quantum
    /// ahead of the other.  The rule is built around that, and it lives in
    /// [`mix_pair`] so [`Soundtrack::set_app`] can use it too when it closes
    /// one application stream and opens the next.
    fn drain_summed(
        mic: &Mic,
        app: &Mic,
        recorder: &mut impl crate::record::avcodec::AudioSink,
        mic_scratch: &mut [f32],
        app_scratch: &mut [f32],
        channels: usize,
        room: usize,
    ) -> Result<()> {
        while mix_pair(mic, app, recorder, mic_scratch, app_scratch, channels, room)? > 0 {}
        Ok(())
    }
}

/// How far one side's backlog may exceed the other's before the excess is fed
/// alone.  A graph quantum is a few hundred frames; this is several of them, so
/// a jittery quantum still waits while a real stall is caught.
const BACKLOG_SLACK: usize = 4096;

/// Which side a [`MixPlan::Solo`] feeds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Side {
    Mic,
    App,
}

/// What one step of the mix should do, decided from the two backlogs and the
/// buffer room alone.  Kept apart from the reading so the rule — the part that
/// was wrong twice — can be tested without a PipeWire session.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum MixPlan {
    /// Nothing to pair right now; wait for the next call.
    Idle,
    /// Sum `n` frames from each side.
    Pair(usize),
    /// Pass `n` frames from one side alone, the other having stalled.
    Solo(Side, usize),
}

/// The mix rule.  See [`mix_pair`] for why it is what it is.
fn mix_plan(mic_queued: usize, app_queued: usize, room: usize) -> MixPlan {
    if mic_queued == 0 && app_queued == 0 {
        return MixPlan::Idle;
    }
    // A side far ahead has the other side's stall in its ring: feed *only* the
    // excess alone, and let the matched part stay paired.
    if mic_queued > app_queued + BACKLOG_SLACK {
        return MixPlan::Solo(Side::Mic, (mic_queued - app_queued).min(room));
    }
    if app_queued > mic_queued + BACKLOG_SLACK {
        return MixPlan::Solo(Side::App, (app_queued - mic_queued).min(room));
    }
    // The steady state: pair up what both can supply.
    let count = mic_queued.min(app_queued).min(room);
    if count == 0 {
        return MixPlan::Idle;
    }
    MixPlan::Pair(count)
}

/// Sums a quantum of the two streams into `recorder`, and answers how many
/// frames it fed (0 when there is nothing left to pair right now).
///
/// The rule has two halves, and both are needed:
///
/// * **Normally, pair.**  Each side is read from up to the smaller of the two
///   queued counts, and only that much is summed and fed: one frame of
///   wall-clock time becomes one frame of the mix, never two.  The rest waits
///   in the faster side's ring for the next call, a few milliseconds later.
///   Feeding each side the moment the other merely lagged would count the same
///   instant twice and make the audio track *grow* faster than the video:
///   measured at 26% — an 8-second recording whose audio came out 10.1
///   seconds.
///
/// * **When one side has run far ahead, the excess goes through alone.**  A
///   `--follow` switch replaces the application stream, and opening the
///   replacement is a blocking round trip during which the loop does not pump
///   at all (measured: ~1.0 second).  The microphone keeps running through it
///   and buffers that whole second; the new application stream has nothing
///   yet, so a strict pairing could never push that second out — the
///   microphone would fall silent for it and then reappear a second late.  The
///   same shape happens whenever one source is genuinely stopped and the other
///   is not.
///
///   A backlog far larger than a quantum is exactly that: audio of an interval
///   the other side never supplied, which belongs in the file even if it has to
///   go in alone.  So when one side's backlog exceeds the other's by more than
///   `BACKLOG_SLACK`, only that excess is passed through alone — never the
///   matched part, which stays paired so a quantum of jitter never drifts the
///   two apart.
fn mix_pair(
    mic: &Mic,
    app: &Mic,
    recorder: &mut impl crate::record::avcodec::AudioSink,
    mic_scratch: &mut [f32],
    app_scratch: &mut [f32],
    channels: usize,
    room: usize,
) -> Result<usize> {
    match mix_plan(mic.queued()?, app.queued()?, room) {
        MixPlan::Idle => Ok(0),
        MixPlan::Solo(side, frames) => {
            let (stream, scratch) = match side {
                Side::Mic => (mic, mic_scratch),
                Side::App => (app, app_scratch),
            };
            let read = stream.read(&mut scratch[..frames * channels])?;
            if read == 0 {
                return Ok(0);
            }
            recorder.audio_feed(&scratch[..read * channels], read)?;
            Ok(read)
        }
        MixPlan::Pair(frames) => {
            let mic_read = mic.read(&mut mic_scratch[..frames * channels])?;
            let app_read = app.read(&mut app_scratch[..frames * channels])?;
            let frames = mic_read.min(app_read);
            if frames == 0 {
                return Ok(0);
            }
            sum_into(mic_scratch, app_scratch, frames * channels);
            recorder.audio_feed(&mic_scratch[..frames * channels], frames)?;
            Ok(frames)
        }
    }
}

/// Adds the first `len` samples of `source` into `destination`, sample for
/// sample.  A plain sum with no normalisation: a mix bus adds.  Halving both
/// sides would make a solo source half as loud for no reason, and the AAC
/// encoder takes float, so a loud moment inter-modulates softly rather than
/// clipping hard.
fn sum_into(destination: &mut [f32], source: &[f32], len: usize) {
    let len = len.min(destination.len()).min(source.len());
    for index in 0..len {
        destination[index] += source[index];
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_client_reports_itself_one_way_or_the_other() {
        // On this development machine libpipewire is installed, so the client
        // loads; on a machine without it the error names the package.  Either
        // answer is valid, but not both at once.
        let available = available();
        let error = load_error();
        assert!(available || !error.is_empty(), "one of the two answers");
    }

    #[test]
    fn summing_two_streams_adds_them_sample_for_sample() {
        // The microphone and the application audio are laid over each other,
        // not averaged: a voice at 0.5 and a game at 0.25 come out at 0.75.
        let mut mix = [0.5, -0.5, 0.25, 0.0];
        let app = [0.25, 0.25, -0.25, 1.0];
        sum_into(&mut mix, &app, 4);
        assert_eq!(mix, [0.75, -0.25, 0.0, 1.0]);
    }

    #[test]
    fn summing_stops_at_the_shorter_length() {
        // The two sides hold the same number of frames on the normal path, but
        // the helper must not read past either buffer when they do not.
        let mut mix = [1.0, 1.0, 1.0];
        let app = [1.0];
        sum_into(&mut mix, &app, 3);
        assert_eq!(mix, [2.0, 1.0, 1.0]);
    }

    #[test]
    fn a_loud_moment_inter_modulates_rather_than_clipping() {
        // Two float samples that sum past 1.0 are not clamped: the encoder
        // takes float, so the mix bus keeps the true sum.
        let mut mix = [0.9];
        let app = [0.9];
        sum_into(&mut mix, &app, 1);
        assert!((mix[0] - 1.8).abs() < 1e-6, "the sum is kept, not clipped");
    }

    #[test]
    fn an_empty_mix_is_idle() {
        assert_eq!(mix_plan(0, 0, 8192), MixPlan::Idle);
    }

    #[test]
    fn a_quantum_of_jitter_pairs_rather_than_drifting() {
        // The two streams are never byte-aligned; a quantum apart is the normal
        // state and must pair, so the mix never grows faster than the video.
        assert_eq!(mix_plan(480, 512, 8192), MixPlan::Pair(480));
        assert_eq!(mix_plan(512, 480, 8192), MixPlan::Pair(480));
        // Even a whole second of lead that is still under the slack pairs.
        assert_eq!(mix_plan(48_000, 48_000, 8192), MixPlan::Pair(8192));
    }

    #[test]
    fn a_stalled_side_lets_the_other_through_alone() {
        // A `--follow` switch leaves the application side empty while the
        // microphone buffers the whole blocking round trip: the excess must go
        // through alone, or the microphone is silenced for that interval and
        // reappears a second late (the bug this rule exists for).  The excess
        // is fed in chunks of the buffer room.
        assert_eq!(mix_plan(48_000, 0, 8192), MixPlan::Solo(Side::Mic, 8192));
        assert_eq!(mix_plan(0, 48_000, 8192), MixPlan::Solo(Side::App, 8192));
        // With room for it all, the whole excess goes at once.
        assert_eq!(
            mix_plan(20_000, 0, usize::MAX),
            MixPlan::Solo(Side::Mic, 20_000)
        );
    }

    #[test]
    fn a_small_lead_with_an_empty_side_waits() {
        // Under the slack, an empty side is read as a momentary gap and the
        // other side's audio waits in its ring for the next call rather than
        // being fed alone — which is what keeps a jittery quantum from drifting
        // the two apart.
        assert_eq!(mix_plan(1024, 0, 8192), MixPlan::Idle);
        assert_eq!(mix_plan(0, 1024, 8192), MixPlan::Idle);
    }

    #[test]
    fn the_slack_is_the_line_between_waiting_and_going_alone() {
        // Just under the slack waits; just over it switches to solo.
        assert_eq!(mix_plan(BACKLOG_SLACK, 0, usize::MAX), MixPlan::Idle);
        assert_eq!(
            mix_plan(BACKLOG_SLACK + 1, 0, usize::MAX),
            MixPlan::Solo(Side::Mic, BACKLOG_SLACK + 1)
        );
        // A lead of exactly the slack over a side that also has data still
        // pairs: only a difference *greater* than the slack goes alone.
        assert_eq!(
            mix_plan(BACKLOG_SLACK + 512, 512, usize::MAX),
            MixPlan::Pair(512)
        );
    }
}

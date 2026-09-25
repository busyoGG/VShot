// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! The Rust side of the recording shim (`shim.c`).
//!
//! The shim hides libavcodec's and libavformat's types; this module declares
//! its entry points and wraps them in RAII.  Everything about *why* the
//! encoder runs on libavcodec lives in the shim's own comment: the short
//! version is that direct libva calls crash inside radeonsi on this machine
//! while libavcodec's VAAPI path does not, and wf-recorder's design (encode
//! through libavcodec) is the evidence for the route.
//!
//! A recording is one [`Recorder`]: the VAAPI encoder and the MP4 muxer that
//! libavformat provides, held together in the shim so a captured frame goes
//! from pixels to a muxed packet without a bitstream crossing back into
//! Rust.  That is what keeps the container honest — the same library that
//! produced the packets writes the sample table and the parameter sets.
//!
//! Two frame inputs mirror the shim: the software path takes one packed RGBA
//! buffer per frame (compatibility — it works wherever the screenshots
//! work), and the dma-buf path takes the buffer the compositor rendered into
//! (zero copy — the only way 4K stays above 60 fps).

use std::ffi::{c_char, c_int, CStr};
use std::fmt;
use std::path::Path;

use crate::error::{Result, VshotError};
use crate::model::Frame;

/// The opaque recorder handle from `shim.c`.
#[repr(C)]
struct VshotRec {
    _private: [u8; 0],
}

extern "C" {
    fn vshot_rec_start(
        path: *const c_char,
        width: c_int,
        height: c_int,
        codec: *const c_char,
        qp: c_int,
        backend: c_int,
    ) -> *mut VshotRec;
    fn vshot_rec_start_dmabuf(
        path: *const c_char,
        width: c_int,
        height: c_int,
        codec: *const c_char,
        qp: c_int,
        fourcc: u32,
        backend: c_int,
    ) -> *mut VshotRec;
    fn vshot_rec_start_mic(
        path: *const c_char,
        width: c_int,
        height: c_int,
        codec: *const c_char,
        qp: c_int,
        mic_rate: c_int,
        mic_channels: c_int,
        backend: c_int,
    ) -> *mut VshotRec;
    fn vshot_rec_start_dmabuf_mic(
        path: *const c_char,
        width: c_int,
        height: c_int,
        codec: *const c_char,
        qp: c_int,
        fourcc: u32,
        mic_rate: c_int,
        mic_channels: c_int,
        backend: c_int,
    ) -> *mut VshotRec;
    fn vshot_rec_frame(rec: *mut VshotRec, rgba: *const u8, duration_ms: c_int) -> c_int;
    fn vshot_rec_resize_fit(rec: *mut VshotRec, width: c_int, height: c_int, fourcc: u32) -> c_int;
    fn vshot_rec_frame_dmabuf(
        rec: *mut VshotRec,
        fd: c_int,
        fourcc: u32,
        modifier: u64,
        offset: c_int,
        stride: c_int,
        duration_ms: c_int,
    ) -> c_int;
    fn vshot_rec_finish(rec: *mut VshotRec) -> c_int;
    fn vshot_rec_frames(rec: *mut VshotRec) -> i64;
    fn vshot_rec_seconds(rec: *mut VshotRec) -> f64;
    fn vshot_rec_last_error(rec: *mut VshotRec) -> *const c_char;
    fn vshot_rec_free(rec: *mut VshotRec);
    fn vshot_rec_available() -> c_int;
    fn vshot_rec_load_error() -> *const c_char;
    fn vshot_rec_version() -> u32;
    fn vshot_av_enc_filter_available() -> c_int;
    // The audio side: the microphone's samples, encoded to AAC and muxed as
    // a second stream in the same MP4.
    fn vshot_rec_audio_feed(rec: *mut VshotRec, samples: *const f32, frames: c_int) -> c_int;
    fn vshot_rec_audio_pump(rec: *mut VshotRec) -> c_int;
    fn vshot_rec_audio_rate(rec: *mut VshotRec) -> c_int;
    fn vshot_rec_audio_channels(rec: *mut VshotRec) -> c_int;
    fn vshot_rec_audio_error(rec: *mut VshotRec) -> *const c_char;
    // The replay session: the same encoder and audio side, but the packets
    // go into an in-memory ring instead of a file, and a save copies them
    // into a fresh MP4.
    fn vshot_rec_start_replay(
        width: c_int,
        height: c_int,
        codec: *const c_char,
        qp: c_int,
        dmabuf: c_int,
        fourcc: u32,
        mic_rate: c_int,
        mic_channels: c_int,
        window_ms: i64,
        gop_frames: c_int,
        fps: c_int,
        backend: c_int,
    ) -> *mut VshotRec;
    fn vshot_rec_replay_save(
        rec: *mut VshotRec,
        path: *const c_char,
        seconds: c_int,
        out_ms: *mut i64,
    ) -> c_int;
    fn vshot_rec_replay_span(rec: *mut VshotRec) -> f64;
    // Whether a hardware backend can be opened on this machine; used by
    // `--encoder-backend auto` and by the zero-copy decision.
    fn vshot_av_enc_backend_probe(backend: c_int, err: *mut c_char, err_len: usize) -> c_int;
}

/// Whether the whole recorder can run: libavcodec, libavutil and the
/// libavformat muxer.  Used by `record` to fail early with a useful message
/// rather than mid-file.
pub fn recorder_available() -> bool {
    unsafe { vshot_rec_available() != 0 }
}

/// Whether libavfilter is present too — the zero-copy path's extra
/// dependency.  The software path does not need it.
pub fn filter_available() -> bool {
    unsafe { vshot_av_enc_filter_available() != 0 }
}

/// Why the recorder is unavailable, when it is.
pub fn load_error() -> String {
    unsafe { c_take(vshot_rec_load_error()) }
}

/// The libavcodec version, for the debug trace.
pub fn libavcodec_version() -> String {
    let version = unsafe { vshot_rec_version() };
    format!(
        "{}.{}.{}",
        version >> 16,
        (version >> 8) & 0xff,
        version & 0xff
    )
}

fn c_take(pointer: *const c_char) -> String {
    if pointer.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(pointer) }
        .to_string_lossy()
        .into_owned()
}

/// Quality, matching the levels the old libva path used.
const FRAME_QP: c_int = 26;

/// The video codecs `--encoder` accepts, in the order the help lists them.
/// Each maps to the ffmpeg encoder named `<word>_vaapi`; whether the
/// running ffmpeg and GPU actually provide it is checked at open time.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VideoCodec {
    H264,
    Hevc,
    Av1,
}

impl VideoCodec {
    /// The word that names this codec on the command line and in the
    /// encoder's ffmpeg name.
    pub const fn word(self) -> &'static str {
        match self {
            Self::H264 => "h264",
            Self::Hevc => "hevc",
            Self::Av1 => "av1",
        }
    }

    /// Parses the command-line word.
    pub fn parse(word: &str) -> Option<Self> {
        match word {
            "h264" => Some(Self::H264),
            "hevc" => Some(Self::Hevc),
            "av1" => Some(Self::Av1),
            _ => None,
        }
    }

    /// Every codec, for the help text and the tests.
    pub const ALL: [Self; 3] = [Self::H264, Self::Hevc, Self::Av1];
}

impl fmt::Display for VideoCodec {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.word())
    }
}

/// Which hardware encoder runs the session.  VAAPI is the AMD/Intel one (a
/// DRM render node, and the only one that can import a compositor dma-buf
/// without a copy); NVENC is the NVIDIA one (a CUDA device).  `auto` picks
/// whichever the machine actually has, VAAPI first, so a machine with both —
/// or a laptop that switches GPUs — records with no flag.
///
/// The value passed to the shim is the discriminant; the C side names it
/// `VSHOT_BACKEND_*`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EncoderBackend {
    /// Pick VAAPI or NVENC from what the machine has.
    Auto,
    Vaapi,
    Nvenc,
}

impl EncoderBackend {
    /// The word that names this backend on the command line.
    pub const fn word(self) -> &'static str {
        match self {
            Self::Auto => "auto",
            Self::Vaapi => "vaapi",
            Self::Nvenc => "nvenc",
        }
    }

    /// Parses the command-line word.
    pub fn parse(word: &str) -> Option<Self> {
        match word {
            "auto" => Some(Self::Auto),
            "vaapi" => Some(Self::Vaapi),
            "nvenc" => Some(Self::Nvenc),
            _ => None,
        }
    }

    /// The shim's own value: `VSHOT_BACKEND_VAAPI` (0) or `VSHOT_BACKEND_NVENC`
    /// (1).  `Auto` is resolved by [`Self::resolve`] before this is asked, so
    /// reaching it here means the caller skipped that step; VAAPI is the
    /// conservative answer.
    pub(crate) const fn shim_value(self) -> c_int {
        match self {
            Self::Nvenc => 1,
            _ => 0,
        }
    }

    /// The backend a session really runs: `Auto` becomes VAAPI when a VAAPI
    /// device opens, else NVENC when a CUDA device does, else the requested
    /// backend so the open failure names the thing the user asked for.
    pub fn resolve(self) -> Self {
        match self {
            Self::Auto => {
                if backend_available(Self::Vaapi) {
                    Self::Vaapi
                } else if backend_available(Self::Nvenc) {
                    Self::Nvenc
                } else {
                    // Neither opened; let the recording report VAAPI's reason,
                    // which on a machine with no GPU encoder is the useful one.
                    Self::Vaapi
                }
            }
            other => other,
        }
    }

    /// Every backend, for the help text and the tests.
    pub const ALL: [Self; 3] = [Self::Auto, Self::Vaapi, Self::Nvenc];
}

impl fmt::Display for EncoderBackend {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.word())
    }
}

/// Whether a hardware backend can be opened on this machine (the shim's own
/// probe).  Cheap: it opens the device and closes it without encoding.
pub fn backend_available(backend: EncoderBackend) -> bool {
    let mut error = [0 as c_char; 256];
    let status = unsafe {
        vshot_av_enc_backend_probe(backend.shim_value(), error.as_mut_ptr(), error.len())
    };
    status == 0
}

/// Why a backend could not be opened, for a failure message.
pub fn backend_error(backend: EncoderBackend) -> String {
    let mut error = [0 as c_char; 256];
    let status = unsafe {
        vshot_av_enc_backend_probe(backend.shim_value(), error.as_mut_ptr(), error.len())
    };
    if status == 0 {
        String::new()
    } else {
        c_take(error.as_ptr())
    }
}

/// What a recording loop needs from a video sink: a [`Recorder`] muxes the
/// frames into its file, a [`ReplayRecorder`] keeps them in the ring.  The
/// window loop is written against this so `record window` and `replay start
/// window` share one implementation.
pub(crate) trait VideoSink {
    /// Encodes one dma-buf frame.  `duration_ms` is how long it was on screen.
    #[allow(clippy::too_many_arguments)] // the descriptor's own shape
    fn frame_dmabuf(
        &mut self,
        fd: c_int,
        fourcc: u32,
        modifier: u64,
        offset: c_int,
        stride: c_int,
        duration_ms: u32,
    ) -> Result<()>;

    /// Encodes one packed-RGBA frame.  The window loop uses this on a backend
    /// that cannot import a dma-buf (NVENC): the capture side reads the
    /// buffer back to system memory and hands the pixels over.
    fn frame_rgba(&mut self, rgba: &[u8], duration_ms: u32) -> Result<()>;

    /// Whether the sink wants dma-buf frames (a VAAPI session) or RGBA ones
    /// (an NVENC session, which has no dma-buf import).
    fn wants_dmabuf(&self) -> bool;

    /// Follows a source that was resized mid-session: the new-size frames are
    /// fitted into the canvas the session was opened with.
    fn resize_fit(&mut self, width: u32, height: u32, fourcc: u32) -> Result<()>;

    /// The canvas the session was opened with, which a resize fits into.
    fn canvas(&self) -> (u32, u32);
}

impl VideoSink for Recorder {
    fn frame_dmabuf(
        &mut self,
        fd: c_int,
        fourcc: u32,
        modifier: u64,
        offset: c_int,
        stride: c_int,
        duration_ms: u32,
    ) -> Result<()> {
        Recorder::frame_dmabuf(self, fd, fourcc, modifier, offset, stride, duration_ms)
    }

    fn frame_rgba(&mut self, rgba: &[u8], duration_ms: u32) -> Result<()> {
        Recorder::frame_rgba(self, rgba, duration_ms)
    }

    fn wants_dmabuf(&self) -> bool {
        self.dmabuf
    }

    fn resize_fit(&mut self, width: u32, height: u32, fourcc: u32) -> Result<()> {
        Recorder::resize_fit(self, width, height, fourcc)
    }

    fn canvas(&self) -> (u32, u32) {
        (Recorder::width(self), Recorder::height(self))
    }
}

impl VideoSink for ReplayRecorder {
    fn frame_dmabuf(
        &mut self,
        fd: c_int,
        fourcc: u32,
        modifier: u64,
        offset: c_int,
        stride: c_int,
        duration_ms: u32,
    ) -> Result<()> {
        ReplayRecorder::frame_dmabuf(self, fd, fourcc, modifier, offset, stride, duration_ms)
    }

    fn frame_rgba(&mut self, rgba: &[u8], duration_ms: u32) -> Result<()> {
        ReplayRecorder::frame_rgba(self, rgba, duration_ms)
    }

    fn wants_dmabuf(&self) -> bool {
        self.dmabuf
    }

    fn resize_fit(&mut self, width: u32, height: u32, fourcc: u32) -> Result<()> {
        // A replay's ring holds one canvas, like a file: the new frames are
        // fitted into it.  The shim's fit chain is the same one a recording
        // uses, reached through the same entry point.
        if self.handle.is_null() {
            return Err(VshotError::Recording("the replay is closed".into()));
        }
        let status = unsafe {
            vshot_rec_resize_fit(
                self.handle,
                c_int::try_from(width).unwrap_or(0),
                c_int::try_from(height).unwrap_or(0),
                fourcc,
            )
        };
        if status != 0 {
            return Err(VshotError::Recording(
                self.last_error("following the window resize failed"),
            ));
        }
        Ok(())
    }

    fn canvas(&self) -> (u32, u32) {
        (self.width, self.height)
    }
}

/// A whole recording: the GPU encoder and the MP4 muxer, both inside the
/// shim.  Created once per recording, fed one frame at a time, finished once
/// — the file is complete and seekable when `finish` returns.
pub struct Recorder {
    handle: *mut VshotRec,
    /// The canvas the file was opened with.  One MP4 holds one frame size
    /// from its first packet to its trailer, so a source that is resized
    /// mid-recording is fitted into this one instead of changing it.
    width: u32,
    height: u32,
    /// Whether this session encodes dma-bufs (the zero-copy VAAPI path) or
    /// RGBA pixels (the software path, and every NVENC session).
    dmabuf: bool,
}

// The recorder is created and used on the recording thread only; the raw
// handle is not an issue for `Send` since nothing else ever touches it.
// The loop is single-threaded by design (see `record::run`).
impl Recorder {
    /// Starts recording `width` x `height` with `codec` into `path`, taking
    /// tightly packed RGBA frames, on the given hardware backend.
    pub fn start(
        path: &Path,
        width: u32,
        height: u32,
        codec: VideoCodec,
        backend: EncoderBackend,
    ) -> Result<Self> {
        Self::open(path, width, height, codec, None, None, backend)
    }

    /// The zero-copy variant: frames arrive as dma-bufs with this DRM fourcc
    /// instead of as RGBA pixels.  On NVENC the fourcc is dropped — that
    /// backend has no dma-buf import, so the caller's frames are read back
    /// through the software path instead (see [`Self::open`]).
    pub fn start_dmabuf(
        path: &Path,
        width: u32,
        height: u32,
        codec: VideoCodec,
        fourcc: u32,
        backend: EncoderBackend,
    ) -> Result<Self> {
        Self::open(path, width, height, codec, Some(fourcc), None, backend)
    }

    /// The same two, with a soundtrack: `mic` is the microphone's negotiated
    /// rate and channel count, and the recorder opens an AAC encoder for it
    /// and declares a second stream in the MP4 before writing the header.
    pub fn start_mic(
        path: &Path,
        width: u32,
        height: u32,
        codec: VideoCodec,
        mic: crate::record::pipewire_audio::Format,
        backend: EncoderBackend,
    ) -> Result<Self> {
        Self::open(path, width, height, codec, None, Some(mic), backend)
    }

    pub fn start_dmabuf_mic(
        path: &Path,
        width: u32,
        height: u32,
        codec: VideoCodec,
        fourcc: u32,
        mic: crate::record::pipewire_audio::Format,
        backend: EncoderBackend,
    ) -> Result<Self> {
        Self::open(path, width, height, codec, Some(fourcc), Some(mic), backend)
    }

    fn open(
        path: &Path,
        width: u32,
        height: u32,
        codec: VideoCodec,
        fourcc: Option<u32>,
        mic: Option<crate::record::pipewire_audio::Format>,
        backend: EncoderBackend,
    ) -> Result<Self> {
        if !recorder_available() {
            return Err(VshotError::Recording(format!(
                "MP4 recording needs libavcodec and libavformat (the ffmpeg libraries): {}",
                load_error()
            )));
        }
        // NVENC has no way to import a compositor dma-buf (ffmpeg's CUDA
        // hwcontext maps CUDA memory only), so a dma-buf request becomes the
        // software path: the capture side hands over RGBA pixels and the shim
        // converts and uploads them.  The caller is expected to have skipped
        // building the zero-copy pool already; this is the second gate, so a
        // stray fourcc can never reach the filtergraph that cannot use it.
        let fourcc = match (fourcc, backend.resolve()) {
            (Some(_), EncoderBackend::Nvenc) => None,
            (fourcc, _) => fourcc,
        };
        if fourcc.is_some() && !filter_available() {
            return Err(VshotError::Recording(
                "zero-copy recording needs libavfilter (the ffmpeg libraries): install ffmpeg"
                    .into(),
            ));
        }
        if width == 0 || height == 0 {
            return Err(VshotError::Recording(
                "refusing to record a 0-pixel frame".into(),
            ));
        }
        // libavcodec rejects frames above the hardware's limits with its own
        // error; the explicit check here makes the message ours.
        if width > crate::record::MAX_DIMENSION || height > crate::record::MAX_DIMENSION {
            return Err(VshotError::Recording(format!(
                "{width}x{height} exceeds the {}-pixel recording limit",
                crate::record::MAX_DIMENSION
            )));
        }
        use std::os::unix::ffi::OsStrExt as _;
        let c_path = std::ffi::CString::new(path.as_os_str().as_bytes())
            .map_err(|_| VshotError::Recording("the output path contains a NUL byte".into()))?;
        let word = std::ffi::CString::new(codec.word()).expect("codec words have no NUL");
        let shim_backend = backend.resolve().shim_value();
        let (mic_rate, mic_channels) = match mic {
            Some(format) => (
                c_int::try_from(format.rate).unwrap_or(0),
                c_int::try_from(format.channels).unwrap_or(0),
            ),
            None => (0, 0),
        };
        let handle = match (fourcc, mic) {
            (Some(fourcc), Some(_)) => unsafe {
                vshot_rec_start_dmabuf_mic(
                    c_path.as_ptr(),
                    width as c_int,
                    height as c_int,
                    word.as_ptr(),
                    FRAME_QP,
                    fourcc,
                    mic_rate,
                    mic_channels,
                    shim_backend,
                )
            },
            (Some(fourcc), None) => unsafe {
                vshot_rec_start_dmabuf(
                    c_path.as_ptr(),
                    width as c_int,
                    height as c_int,
                    word.as_ptr(),
                    FRAME_QP,
                    fourcc,
                    shim_backend,
                )
            },
            (None, Some(_)) => unsafe {
                vshot_rec_start_mic(
                    c_path.as_ptr(),
                    width as c_int,
                    height as c_int,
                    word.as_ptr(),
                    FRAME_QP,
                    mic_rate,
                    mic_channels,
                    shim_backend,
                )
            },
            (None, None) => unsafe {
                vshot_rec_start(
                    c_path.as_ptr(),
                    width as c_int,
                    height as c_int,
                    word.as_ptr(),
                    FRAME_QP,
                    shim_backend,
                )
            },
        };
        if handle.is_null() {
            let detail = load_error();
            // The most common refusal is a composed desktop wider than the
            // hardware encoder allows (this GPU's H.264 caps at 4096; a
            // two-4K `record all` is 5760, which is why hevc is the answer
            // there).  Say so, because the libavcodec message alone
            // ("Invalid argument") does not.
            let hint = if width > 4096 && codec == VideoCodec::H264 {
                " — this GPU's H.264 encoder is limited to 4096 pixels of \
                 width; record a single output, or try --encoder hevc"
            } else {
                ""
            };
            // A backend the user named explicitly and that could not open: its
            // own probe says why (no CUDA device, no render node), which is
            // more useful than the generic open failure underneath it.
            let backend_note = if backend != EncoderBackend::Auto {
                let why = backend_error(backend);
                if why.is_empty() {
                    String::new()
                } else {
                    format!(
                        " (the {} backend could not be opened: {why})",
                        backend.word()
                    )
                }
            } else {
                String::new()
            };
            return Err(VshotError::Recording(format!(
                "could not start the {} recorder: {detail}{backend_note}{hint}",
                codec.word()
            )));
        }
        Ok(Self {
            handle,
            width,
            height,
            dmabuf: fourcc.is_some(),
        })
    }

    /// Encodes one captured frame and muxes it.  `duration_ms` is how long
    /// the frame was on screen; it becomes the sample's duration, which is
    /// what makes a recording play back at the speed it happened.
    pub fn frame(&mut self, frame: &Frame, duration_ms: u32) -> Result<()> {
        self.frame_rgba(frame.pixels(), duration_ms)
    }

    /// Encodes one RGBA buffer: `width * height` tightly packed pixels.
    pub fn frame_rgba(&mut self, rgba: &[u8], duration_ms: u32) -> Result<()> {
        if self.handle.is_null() {
            return Err(VshotError::Recording("the recorder is closed".into()));
        }
        let status = unsafe { vshot_rec_frame(self.handle, rgba.as_ptr(), duration_ms as c_int) };
        if status != 0 {
            return Err(VshotError::Recording(
                self.last_error("encoding a frame failed"),
            ));
        }
        Ok(())
    }

    /// Encodes one dma-buf frame.  `fd` is the buffer the compositor has
    /// just rendered into; `fourcc`, `modifier`, `offset` and `stride`
    /// describe it.  The descriptor's file descriptor must stay open for as
    /// long as the buffer is in use (the recorder's pool owns them).
    #[allow(clippy::too_many_arguments)] // the descriptor's own shape
    pub fn frame_dmabuf(
        &mut self,
        fd: c_int,
        fourcc: u32,
        modifier: u64,
        offset: c_int,
        stride: c_int,
        duration_ms: u32,
    ) -> Result<()> {
        if self.handle.is_null() {
            return Err(VshotError::Recording("the recorder is closed".into()));
        }
        let status = unsafe {
            vshot_rec_frame_dmabuf(
                self.handle,
                fd,
                fourcc,
                modifier,
                offset,
                stride,
                duration_ms as c_int,
            )
        };
        if status != 0 {
            return Err(VshotError::Recording(
                self.last_error("encoding a dma-buf frame failed"),
            ));
        }
        Ok(())
    }

    /// Follows a source that was resized mid-recording: the dma-buf chain is
    /// rebuilt so the new-size frames are fitted into the canvas the file was
    /// opened with (scaled down if larger, centred if smaller).  No frame is
    /// encoded — the next [`Self::frame_dmabuf`] feeds the new chain.
    /// `fourcc` is the format the capture side now delivers; 0 keeps the one
    /// the recording was opened with.
    pub fn resize_fit(&mut self, width: u32, height: u32, fourcc: u32) -> Result<()> {
        if self.handle.is_null() {
            return Err(VshotError::Recording("the recorder is closed".into()));
        }
        let status = unsafe {
            vshot_rec_resize_fit(
                self.handle,
                c_int::try_from(width).unwrap_or(0),
                c_int::try_from(height).unwrap_or(0),
                fourcc,
            )
        };
        if status != 0 {
            return Err(VshotError::Recording(
                self.last_error("following the window resize failed"),
            ));
        }
        Ok(())
    }

    /// The frame size the recording was opened with, which is the size its
    /// file reports — and what a resize is fitted into.
    pub fn width(&self) -> u32 {
        self.width
    }

    pub fn height(&self) -> u32 {
        self.height
    }

    /// Flushes the encoder and writes the container's trailer, leaving a
    /// seekable file behind.
    pub fn finish(&mut self) -> Result<()> {
        if self.handle.is_null() {
            return Ok(());
        }
        if unsafe { vshot_rec_finish(self.handle) } != 0 {
            return Err(VshotError::Recording(
                self.last_error("finishing the recording failed"),
            ));
        }
        Ok(())
    }

    /// Queues interleaved float samples from the microphone: `samples` holds
    /// `frames * channels` floats, and `frames` is what the C side counts
    /// (samples per channel) — passing the float count instead doubles the
    /// soundtrack for a stereo input.
    pub fn audio_feed(&mut self, samples: &[f32], frames: usize) -> Result<()> {
        if self.handle.is_null() || frames == 0 {
            return Ok(());
        }
        let frames = c_int::try_from(frames).unwrap_or(c_int::MAX);
        let status = unsafe { vshot_rec_audio_feed(self.handle, samples.as_ptr(), frames) };
        if status != 0 {
            return Err(VshotError::Recording(
                self.audio_error("queueing microphone samples failed"),
            ));
        }
        Ok(())
    }

    /// Encodes and muxes everything the audio staging buffer holds.  Called
    /// once per video frame; nothing to do when no soundtrack is recorded.
    pub fn audio_pump(&mut self) -> Result<()> {
        if self.handle.is_null() {
            return Ok(());
        }
        if unsafe { vshot_rec_audio_pump(self.handle) } != 0 {
            return Err(VshotError::Recording(
                self.audio_error("encoding the microphone audio failed"),
            ));
        }
        Ok(())
    }

    /// The AAC encoder's rate, or 0 when no soundtrack is recorded.
    pub fn audio_rate(&self) -> u32 {
        if self.handle.is_null() {
            return 0;
        }
        unsafe { vshot_rec_audio_rate(self.handle).max(0) as u32 }
    }

    /// The AAC encoder's channel count, or 0 when no soundtrack is recorded.
    pub fn audio_channels(&self) -> u32 {
        if self.handle.is_null() {
            return 0;
        }
        unsafe { vshot_rec_audio_channels(self.handle).max(0) as u32 }
    }

    fn audio_error(&self, what: &str) -> String {
        let detail = c_take(unsafe { vshot_rec_audio_error(self.handle) });
        if detail.is_empty() {
            what.to_string()
        } else {
            format!("{what}: {detail}")
        }
    }

    /// How many packets reached the muxer.
    pub fn frames(&self) -> u64 {
        if self.handle.is_null() {
            return 0;
        }
        unsafe { vshot_rec_frames(self.handle).max(0) as u64 }
    }

    /// The timeline's length in seconds: the sum of the frames' real
    /// durations, not a nominal frame rate times a count.
    pub fn seconds(&self) -> f64 {
        if self.handle.is_null() {
            return 0.0;
        }
        unsafe { vshot_rec_seconds(self.handle) }
    }

    fn last_error(&self, what: &str) -> String {
        let detail = c_take(unsafe { vshot_rec_last_error(self.handle) });
        if detail.is_empty() {
            what.to_string()
        } else {
            format!("{what}: {detail}")
        }
    }
}

impl Drop for Recorder {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { vshot_rec_free(self.handle) };
            self.handle = std::ptr::null_mut();
        }
    }
}

/// A replay session: the same GPU encoder and audio side as a [`Recorder`],
/// but the encoded packets are kept in an in-memory ring instead of a file.
/// Nothing reaches the disk until [`ReplayRecorder::save`] is called, and then
/// the packets are copied into an MP4 unchanged — a remux, no re-encode.
///
/// The steady-state cost is one encode plus one in-memory push (a recording
/// minus the disk write), which is what makes this cheap enough to leave
/// running while a game has the GPU.
pub struct ReplayRecorder {
    handle: *mut VshotRec,
    width: u32,
    height: u32,
    saves: u64,
    /// Whether the ring holds dma-buf-encoded frames (VAAPI zero-copy) or
    /// RGBA-encoded ones (the software path, and every NVENC session).
    dmabuf: bool,
}

impl ReplayRecorder {
    /// Starts a replay session.  `retention_secs` is how much history the ring
    /// keeps — the user's window plus one key-frame interval, so a save can
    /// always start at a key frame at the requested edge; `gop_frames` is the
    /// key-frame distance.  `mic` is the microphone to keep in the ring.
    #[allow(clippy::too_many_arguments)] // the session's own shape
    pub fn start(
        width: u32,
        height: u32,
        codec: VideoCodec,
        fourcc: Option<u32>,
        mic: Option<crate::record::pipewire_audio::Format>,
        retention_secs: u64,
        gop_frames: u32,
        fps: u32,
        backend: EncoderBackend,
    ) -> Result<Self> {
        if !recorder_available() {
            return Err(VshotError::Recording(format!(
                "replay needs libavcodec and libavformat (the ffmpeg libraries): {}",
                load_error()
            )));
        }
        // As on the recording side: NVENC records the software path, so a
        // dma-buf request becomes RGBA frames the shim converts and uploads.
        let fourcc = match (fourcc, backend.resolve()) {
            (Some(_), EncoderBackend::Nvenc) => None,
            (fourcc, _) => fourcc,
        };
        if fourcc.is_some() && !filter_available() {
            return Err(VshotError::Recording(
                "zero-copy replay needs libavfilter (the ffmpeg libraries): install ffmpeg".into(),
            ));
        }
        if width == 0 || height == 0 {
            return Err(VshotError::Recording(
                "refusing to replay a 0-pixel frame".into(),
            ));
        }
        if width > crate::record::MAX_DIMENSION || height > crate::record::MAX_DIMENSION {
            return Err(VshotError::Recording(format!(
                "{width}x{height} exceeds the {}-pixel recording limit",
                crate::record::MAX_DIMENSION
            )));
        }
        let word = std::ffi::CString::new(codec.word()).expect("codec words have no NUL");
        let (mic_rate, mic_channels) = match mic {
            Some(format) => (
                c_int::try_from(format.rate).unwrap_or(0),
                c_int::try_from(format.channels).unwrap_or(0),
            ),
            None => (0, 0),
        };
        let window_ms = i64::try_from(retention_secs.saturating_mul(1000)).unwrap_or(i64::MAX);
        let handle = unsafe {
            vshot_rec_start_replay(
                width as c_int,
                height as c_int,
                word.as_ptr(),
                FRAME_QP,
                fourcc.is_some() as c_int,
                fourcc.unwrap_or(0),
                mic_rate,
                mic_channels,
                window_ms,
                c_int::try_from(gop_frames).unwrap_or(0),
                c_int::try_from(fps).unwrap_or(0),
                backend.resolve().shim_value(),
            )
        };
        if handle.is_null() {
            let detail = load_error();
            let hint = if width > 4096 && codec == VideoCodec::H264 {
                " — this GPU's H.264 encoder is limited to 4096 pixels of width; replay a single \
                 output, or try --encoder hevc"
            } else {
                ""
            };
            return Err(VshotError::Recording(format!(
                "could not start the {} replay: {detail}{hint}",
                codec.word()
            )));
        }
        Ok(Self {
            handle,
            width,
            height,
            saves: 0,
            dmabuf: fourcc.is_some(),
        })
    }

    /// Encodes one packed-RGBA frame into the ring.  The window replay uses
    /// this on a backend without dma-buf import (NVENC).
    pub fn frame_rgba(&mut self, rgba: &[u8], duration_ms: u32) -> Result<()> {
        if self.handle.is_null() {
            return Err(VshotError::Recording("the replay is closed".into()));
        }
        let status = unsafe { vshot_rec_frame(self.handle, rgba.as_ptr(), duration_ms as c_int) };
        if status != 0 {
            return Err(VshotError::Recording(
                self.last_error("encoding a frame failed"),
            ));
        }
        Ok(())
    }

    /// Encodes one dma-buf frame into the ring.  `duration_ms` is how long the
    /// frame was on screen; it becomes the packet's duration on the ring's
    /// timeline.
    #[allow(clippy::too_many_arguments)] // the descriptor's own shape
    pub fn frame_dmabuf(
        &mut self,
        fd: c_int,
        fourcc: u32,
        modifier: u64,
        offset: c_int,
        stride: c_int,
        duration_ms: u32,
    ) -> Result<()> {
        if self.handle.is_null() {
            return Err(VshotError::Recording("the replay is closed".into()));
        }
        let status = unsafe {
            vshot_rec_frame_dmabuf(
                self.handle,
                fd,
                fourcc,
                modifier,
                offset,
                stride,
                duration_ms as c_int,
            )
        };
        if status != 0 {
            return Err(VshotError::Recording(
                self.last_error("encoding a dma-buf frame into the replay failed"),
            ));
        }
        Ok(())
    }

    /// Encodes one software frame into the ring (the compatibility path).
    pub fn frame(&mut self, frame: &Frame, duration_ms: u32) -> Result<()> {
        if self.handle.is_null() {
            return Err(VshotError::Recording("the replay is closed".into()));
        }
        let status =
            unsafe { vshot_rec_frame(self.handle, frame.pixels().as_ptr(), duration_ms as c_int) };
        if status != 0 {
            return Err(VshotError::Recording(
                self.last_error("encoding a frame into the replay failed"),
            ));
        }
        Ok(())
    }

    /// How much history the ring currently holds, in seconds.
    pub fn span_seconds(&self) -> f64 {
        if self.handle.is_null() {
            return 0.0;
        }
        unsafe { vshot_rec_replay_span(self.handle) }
    }

    /// Writes the last `seconds` of the ring to `path` as a stream copy.
    /// Returns the number of packets written and the file's real length in
    /// seconds (the key-frame-aligned start can make it a little longer than
    /// asked).  `seconds == 0` saves the whole window.
    pub fn save(&mut self, path: &std::path::Path, seconds: u64) -> Result<(u64, f64)> {
        if self.handle.is_null() {
            return Err(VshotError::Recording("the replay is closed".into()));
        }
        use std::os::unix::ffi::OsStrExt as _;
        let c_path = std::ffi::CString::new(path.as_os_str().as_bytes())
            .map_err(|_| VshotError::Recording("the save path contains a NUL byte".into()))?;
        let mut out_ms: i64 = 0;
        let written = unsafe {
            vshot_rec_replay_save(self.handle, c_path.as_ptr(), seconds as c_int, &mut out_ms)
        };
        if written < 0 {
            return Err(VshotError::Recording(
                self.last_error("saving the replay failed"),
            ));
        }
        self.saves += 1;
        Ok((written as u64, out_ms as f64 / 1000.0))
    }

    /// How many saves this session has served.
    pub fn saves(&self) -> u64 {
        self.saves
    }

    /// Queues interleaved float samples from the microphone into the ring's
    /// audio side, exactly as a recording does.
    pub fn audio_feed(&mut self, samples: &[f32], frames: usize) -> Result<()> {
        if self.handle.is_null() || frames == 0 {
            return Ok(());
        }
        let frames = c_int::try_from(frames).unwrap_or(c_int::MAX);
        let status = unsafe { vshot_rec_audio_feed(self.handle, samples.as_ptr(), frames) };
        if status != 0 {
            return Err(VshotError::Recording(
                self.audio_error("queueing microphone samples for the replay failed"),
            ));
        }
        Ok(())
    }

    /// Encodes and rings everything the audio staging buffer holds.
    pub fn audio_pump(&mut self) -> Result<()> {
        if self.handle.is_null() {
            return Ok(());
        }
        if unsafe { vshot_rec_audio_pump(self.handle) } != 0 {
            return Err(VshotError::Recording(
                self.audio_error("encoding the replay's microphone audio failed"),
            ));
        }
        Ok(())
    }

    pub fn audio_rate(&self) -> u32 {
        if self.handle.is_null() {
            return 0;
        }
        unsafe { vshot_rec_audio_rate(self.handle).max(0) as u32 }
    }

    pub fn audio_channels(&self) -> u32 {
        if self.handle.is_null() {
            return 0;
        }
        unsafe { vshot_rec_audio_channels(self.handle).max(0) as u32 }
    }

    fn audio_error(&self, what: &str) -> String {
        let detail = c_take(unsafe { vshot_rec_audio_error(self.handle) });
        if detail.is_empty() {
            what.to_string()
        } else {
            format!("{what}: {detail}")
        }
    }

    fn last_error(&self, what: &str) -> String {
        let detail = c_take(unsafe { vshot_rec_last_error(self.handle) });
        if detail.is_empty() {
            what.to_string()
        } else {
            format!("{what}: {detail}")
        }
    }
}

impl Drop for ReplayRecorder {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { vshot_rec_free(self.handle) };
            self.handle = std::ptr::null_mut();
        }
    }
}

/// What a recording loop needs from a sink to feed it a soundtrack: a
/// [`Recorder`] writes the audio into its file, a [`ReplayRecorder`] keeps it
/// in the ring.  The pump in `record` is written against this so the two share
/// one implementation.
pub(crate) trait AudioSink {
    fn audio_feed(&mut self, samples: &[f32], frames: usize) -> Result<()>;
    fn audio_pump(&mut self) -> Result<()>;
}

impl AudioSink for Recorder {
    fn audio_feed(&mut self, samples: &[f32], frames: usize) -> Result<()> {
        Recorder::audio_feed(self, samples, frames)
    }

    fn audio_pump(&mut self) -> Result<()> {
        Recorder::audio_pump(self)
    }
}

impl AudioSink for ReplayRecorder {
    fn audio_feed(&mut self, samples: &[f32], frames: usize) -> Result<()> {
        ReplayRecorder::audio_feed(self, samples, frames)
    }

    fn audio_pump(&mut self) -> Result<()> {
        ReplayRecorder::audio_pump(self)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_shim_reports_itself_available_on_a_machine_with_ffmpeg() {
        // The library presence check is what `record` uses to fail early;
        // on this development machine ffmpeg is installed, so only the
        // shape of the answer is asserted (a missing ffmpeg makes the
        // string non-empty, which is equally valid).
        let available = recorder_available();
        let error = load_error();
        assert!(
            available || !error.is_empty(),
            "either the recorder loads or it explains why not"
        );
        if available {
            let version = libavcodec_version();
            assert_eq!(version.split('.').count(), 3, "{version}");
        }
    }

    #[test]
    fn codec_words_round_trip_through_parse() {
        for codec in VideoCodec::ALL {
            assert_eq!(VideoCodec::parse(codec.word()), Some(codec), "{codec}");
        }
        assert_eq!(VideoCodec::parse("vp9"), None);
        assert_eq!(VideoCodec::parse(""), None);
    }
}

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
    ) -> *mut VshotRec;
    fn vshot_rec_start_dmabuf(
        path: *const c_char,
        width: c_int,
        height: c_int,
        codec: *const c_char,
        qp: c_int,
        fourcc: u32,
    ) -> *mut VshotRec;
    fn vshot_rec_start_mic(
        path: *const c_char,
        width: c_int,
        height: c_int,
        codec: *const c_char,
        qp: c_int,
        mic_rate: c_int,
        mic_channels: c_int,
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
    ) -> *mut VshotRec;
    fn vshot_rec_frame(rec: *mut VshotRec, rgba: *const u8, duration_ms: c_int) -> c_int;
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

/// A whole recording: the GPU encoder and the MP4 muxer, both inside the
/// shim.  Created once per recording, fed one frame at a time, finished once
/// — the file is complete and seekable when `finish` returns.
pub struct Recorder {
    handle: *mut VshotRec,
}

// The recorder is created and used on the recording thread only; the raw
// handle is not an issue for `Send` since nothing else ever touches it.
// The loop is single-threaded by design (see `record::run`).
impl Recorder {
    /// Starts recording `width` x `height` with `codec` into `path`, taking
    /// tightly packed RGBA frames.
    pub fn start(path: &Path, width: u32, height: u32, codec: VideoCodec) -> Result<Self> {
        Self::open(path, width, height, codec, None, None)
    }

    /// The zero-copy variant: frames arrive as dma-bufs with this DRM fourcc
    /// instead of as RGBA pixels.
    pub fn start_dmabuf(
        path: &Path,
        width: u32,
        height: u32,
        codec: VideoCodec,
        fourcc: u32,
    ) -> Result<Self> {
        Self::open(path, width, height, codec, Some(fourcc), None)
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
    ) -> Result<Self> {
        Self::open(path, width, height, codec, None, Some(mic))
    }

    pub fn start_dmabuf_mic(
        path: &Path,
        width: u32,
        height: u32,
        codec: VideoCodec,
        fourcc: u32,
        mic: crate::record::pipewire_audio::Format,
    ) -> Result<Self> {
        Self::open(path, width, height, codec, Some(fourcc), Some(mic))
    }

    fn open(
        path: &Path,
        width: u32,
        height: u32,
        codec: VideoCodec,
        fourcc: Option<u32>,
        mic: Option<crate::record::pipewire_audio::Format>,
    ) -> Result<Self> {
        if !recorder_available() {
            return Err(VshotError::Recording(format!(
                "MP4 recording needs libavcodec and libavformat (the ffmpeg libraries): {}",
                load_error()
            )));
        }
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
                )
            },
            (None, None) => unsafe {
                vshot_rec_start(
                    c_path.as_ptr(),
                    width as c_int,
                    height as c_int,
                    word.as_ptr(),
                    FRAME_QP,
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
            return Err(VshotError::Recording(format!(
                "could not start the {} recorder: {detail}{hint}",
                codec.word()
            )));
        }
        Ok(Self { handle })
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

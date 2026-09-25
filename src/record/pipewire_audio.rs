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
}

impl Drop for Mic {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { binding::vshot_pwa_close(self.handle) };
            self.handle = std::ptr::null_mut();
        }
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
}

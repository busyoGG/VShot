// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! dma-buf buffers for zero-copy capture.
//!
//! The recording loop's fast path has the compositor render each screencopy
//! frame straight into a dma-buf, which the encoder imports into a VAAPI
//! surface without any pixel passing through the CPU.  The buffer itself is
//! allocated by the shim (libgbm, dlopen'd — see `record::shim`'s comment);
//! this module wraps that allocation in RAII so the capture side can hand
//! the compositor a `wl_buffer` built on it and the recorder can hand the
//! encoder the descriptor facts.
//!
//! The buffer's file descriptor stays owned by the shim handle: callers
//! read it, the compositor receives a kernel-level duplicate of it, but
//! nobody closes it except this wrapper's `Drop`.

use std::ffi::{c_char, c_int, c_void, CStr};
use std::os::fd::RawFd;

use crate::error::{Result, VshotError};

extern "C" {
    fn vshot_gbm_buffer_create(
        width: c_int,
        height: c_int,
        fourcc: u32,
        modifier_out: *mut u64,
        fd_out: *mut c_int,
        stride_out: *mut u32,
        offset_out: *mut u32,
    ) -> *mut c_void;
    fn vshot_gbm_buffer_create_with_modifiers(
        width: c_int,
        height: c_int,
        fourcc: u32,
        modifiers: *const u64,
        count: c_int,
        modifier_out: *mut u64,
        fd_out: *mut c_int,
        stride_out: *mut u32,
        offset_out: *mut u32,
    ) -> *mut c_void;
    fn vshot_gbm_buffer_destroy(handle: *mut c_void);
    fn vshot_gbm_available() -> c_int;
    fn vshot_gbm_load_error() -> *const c_char;
}

/// Whether libgbm is present, i.e. whether zero-copy capture can work at
/// all.  A missing libgbm is not fatal: the recorder falls back to the
/// software path.
pub fn available() -> bool {
    unsafe { vshot_gbm_available() != 0 }
}

/// Why libgbm is unavailable, when it is.
pub fn load_error() -> String {
    let pointer = unsafe { vshot_gbm_load_error() };
    if pointer.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(pointer) }
        .to_string_lossy()
        .into_owned()
}

/// One dma-buf the compositor can render into, with the descriptor facts a
/// `wl_buffer` and the encoder need.
pub struct GbmBuffer {
    handle: *mut c_void,
    fd: RawFd,
    fourcc: u32,
    modifier: u64,
    stride: u32,
    offset: u32,
    width: u32,
    height: u32,
}

impl std::fmt::Debug for GbmBuffer {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("GbmBuffer")
            .field("fd", &self.fd)
            .field("fourcc", &format_args!("0x{:08x}", self.fourcc))
            .field("modifier", &format_args!("0x{:x}", self.modifier))
            .field("stride", &self.stride)
            .field("size", &format_args!("{}x{}", self.width, self.height))
            .finish()
    }
}

/// The descriptor facts the shim reads back out of a fresh allocation.
#[derive(Default)]
struct Allocation {
    modifier: u64,
    fd: RawFd,
    stride: u32,
    offset: u32,
}

/// The refusal both allocators share: a buffer with no pixels cannot be
/// allocated, and the size comes from the compositor, so it is worth checking.
fn zero_pixel(width: u32, height: u32) -> Option<VshotError> {
    (width == 0 || height == 0).then(|| {
        VshotError::WaylandProtocol("refusing to allocate a 0-pixel capture buffer".into())
    })
}

impl GbmBuffer {
    /// Allocates a linear-preferred dma-buf of the given fourcc and size.
    pub fn create(width: u32, height: u32, fourcc: u32) -> Result<Self> {
        if let Some(error) = zero_pixel(width, height) {
            return Err(error);
        }
        let mut allocation = Allocation::default();
        let handle = unsafe {
            vshot_gbm_buffer_create(
                width as c_int,
                height as c_int,
                fourcc,
                &mut allocation.modifier,
                &mut allocation.fd,
                &mut allocation.stride,
                &mut allocation.offset,
            )
        };
        Self::wrap(handle, width, height, fourcc, allocation)
    }

    /// Allocates a dma-buf of the given fourcc and size from the modifiers the
    /// compositor advertised, which is what an `ext_image_copy_capture` client
    /// has to do: there the client is the side that allocates the buffer a
    /// window gets copied into, and the modifier has to be one the compositor
    /// can import.  GBM picks the first modifier of the list it can allocate.
    pub fn create_with_modifiers(
        width: u32,
        height: u32,
        fourcc: u32,
        modifiers: &[u64],
    ) -> Result<Self> {
        if let Some(error) = zero_pixel(width, height) {
            return Err(error);
        }
        let mut allocation = Allocation::default();
        let handle = unsafe {
            vshot_gbm_buffer_create_with_modifiers(
                width as c_int,
                height as c_int,
                fourcc,
                modifiers.as_ptr(),
                i32::try_from(modifiers.len()).unwrap_or(i32::MAX),
                &mut allocation.modifier,
                &mut allocation.fd,
                &mut allocation.stride,
                &mut allocation.offset,
            )
        };
        Self::wrap(handle, width, height, fourcc, allocation)
    }

    fn wrap(
        handle: *mut c_void,
        width: u32,
        height: u32,
        fourcc: u32,
        allocation: Allocation,
    ) -> Result<Self> {
        if handle.is_null() {
            let detail = load_error();
            return Err(VshotError::WaylandProtocol(format!(
                "could not allocate a {width}x{height} dma-buf: {detail}"
            )));
        }
        Ok(Self {
            handle,
            fd: allocation.fd,
            fourcc,
            modifier: allocation.modifier,
            stride: allocation.stride,
            offset: allocation.offset,
            width,
            height,
        })
    }

    /// The borrowed file descriptor (owned by this wrapper).
    pub fn fd(&self) -> RawFd {
        self.fd
    }

    pub fn fourcc(&self) -> u32 {
        self.fourcc
    }

    pub fn modifier(&self) -> u64 {
        self.modifier
    }

    pub fn stride(&self) -> u32 {
        self.stride
    }

    pub fn offset(&self) -> u32 {
        self.offset
    }

    pub fn width(&self) -> u32 {
        self.width
    }

    pub fn height(&self) -> u32 {
        self.height
    }

    /// Reads this buffer back to system memory as tightly packed RGBA.
    ///
    /// The zero-copy VAAPI path never calls this: the encoder imports the
    /// dma-buf itself.  NVENC cannot — ffmpeg's CUDA hwcontext maps CUDA
    /// memory only, never an `AV_PIX_FMT_DRM_PRIME` frame — so on NVIDIA the
    /// capture side has to hand over pixels, and this is the copy that gets
    /// them.  It is one `mmap` of the (CPU-accessible) buffer and one channel
    /// swap per pixel: the buffer's memory order is BGRA (XRGB8888/ARGB8888
    /// are `B,G,R,X` in little-endian memory) and the encoder's software path
    /// takes RGBA.  The alpha byte is forced opaque, as on the software capture
    /// paths, because a compositor's alpha byte is not a transparency the
    /// recording should carry.
    pub fn read_rgba(&self) -> Result<Vec<u8>> {
        let width = self.width as usize;
        let height = self.height as usize;
        let stride = self.stride as usize;
        let offset = self.offset as usize;
        let row_bytes = width * 4;
        let length = offset + stride.saturating_mul(height.saturating_sub(1)) + row_bytes;
        // SAFETY: the buffer is a dma-buf this process owns and the
        // compositor has finished writing into (the frame's `ready` event
        // precedes this call).  The mapping is read-only and lives only as
        // long as `map`; the length covers the pixels and nothing is written
        // through it.
        let map =
            unsafe { memmap2::MmapOptions::new().len(length).map(self.fd) }.map_err(|error| {
                VshotError::WaylandProtocol(format!(
                    "could not map the capture buffer for a CPU readback: {error}"
                ))
            })?;
        let mut pixels = vec![0u8; width * height * 4];
        for row in 0..height {
            let source = &map[offset + row * stride..][..row_bytes];
            let destination = &mut pixels[row * row_bytes..][..row_bytes];
            for x in 0..width {
                // BGRA in memory -> RGBA out; alpha forced opaque.
                destination[x * 4] = source[x * 4 + 2];
                destination[x * 4 + 1] = source[x * 4 + 1];
                destination[x * 4 + 2] = source[x * 4];
                destination[x * 4 + 3] = 255;
            }
        }
        Ok(pixels)
    }
}

impl Drop for GbmBuffer {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            unsafe { vshot_gbm_buffer_destroy(self.handle) };
            self.handle = std::ptr::null_mut();
        }
    }
}

/// What a zero-copy capture hands to the recorder: the descriptor facts of
/// the buffer the compositor just rendered into.  The buffer stays owned by
/// the capture session's pool; the fd is valid until that session is
/// dropped, and each capture overwrites the pixels of whichever pool entry
/// it picked.
#[derive(Clone, Copy, Debug)]
pub struct DmabufFrame {
    pub fd: RawFd,
    pub fourcc: u32,
    pub modifier: u64,
    pub offset: u32,
    pub stride: u32,
    pub width: u32,
    pub height: u32,
}

/// The DRM fourcc of ARGB8888 (little-endian `AR24`), the format screencopy
/// uses for opaque and cursor-overlaid frames on every compositor seen so
/// far.
pub const DRM_FORMAT_ARGB8888: u32 = 0x3432_5241;
/// The DRM fourcc of XRGB8888 (little-endian `XR24`).
pub const DRM_FORMAT_XRGB8888: u32 = 0x3432_5258;

#[cfg(test)]
mod tests {
    use super::*;

    /// What modifier does GBM actually hand back?  The window-capture path
    /// needs a specific answer here: it passes the modifiers the compositor
    /// advertised, and the buffer it gets has to be one the compositor can
    /// import.  Prints rather than asserts, because the answer is a property
    /// of the machine.
    #[test]
    #[ignore = "needs a DRM render node"]
    fn reports_what_gbm_allocates() {
        let dcc = [
            0u64,
            0x200000028a6bf04,
            0x200000028a67f04,
            0x200000028a01f04,
        ];
        for (what, list) in [
            ("linear alone", vec![0u64]),
            ("linear then dcc", dcc.to_vec()),
            ("dcc then linear", vec![dcc[1], 0]),
        ] {
            match GbmBuffer::create_with_modifiers(256, 256, DRM_FORMAT_XRGB8888, &list) {
                Ok(buffer) => eprintln!(
                    "vshot: {what} -> modifier 0x{:x} (stride {})",
                    buffer.modifier(),
                    buffer.stride()
                ),
                Err(error) => eprintln!("vshot: {what} -> {error}"),
            }
        }
        // The plain allocator, which is what the screencopy pool uses.
        match GbmBuffer::create(256, 256, DRM_FORMAT_XRGB8888) {
            Ok(buffer) => eprintln!("vshot: create() -> modifier 0x{:x}", buffer.modifier()),
            Err(error) => eprintln!("vshot: create() -> {error}"),
        }
    }
}

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

impl GbmBuffer {
    /// Allocates a linear-preferred dma-buf of the given fourcc and size.
    pub fn create(width: u32, height: u32, fourcc: u32) -> Result<Self> {
        if width == 0 || height == 0 {
            return Err(VshotError::WaylandProtocol(
                "refusing to allocate a 0-pixel capture buffer".into(),
            ));
        }
        let mut modifier = 0u64;
        let mut fd = -1;
        let mut stride = 0u32;
        let mut offset = 0u32;
        let handle = unsafe {
            vshot_gbm_buffer_create(
                width as c_int,
                height as c_int,
                fourcc,
                &mut modifier,
                &mut fd,
                &mut stride,
                &mut offset,
            )
        };
        if handle.is_null() {
            let detail = load_error();
            return Err(VshotError::WaylandProtocol(format!(
                "could not allocate a {width}x{height} dma-buf: {detail}"
            )));
        }
        Ok(Self {
            handle,
            fd,
            fourcc,
            modifier,
            stride,
            offset,
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

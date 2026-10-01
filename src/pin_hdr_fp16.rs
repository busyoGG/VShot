// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! The half-float picture surface a pinned HDR image is drawn on.
//!
//! The C shim beside this file does the drawing: it holds one EGL context on a
//! render node, one half-float compose texture the size of an output, and one
//! dma-buf per picture buffer.  This module is the thin, safe boundary to it —
//! open a context, upload a picture and a shadow mask, draw the shadow, the
//! picture and its rim into the compose texture, copy the region that changed
//! into a buffer, and hand that buffer to the compositor.
//!
//! Why a half-float surface at all: a pinned HDR image is PQ-encoded light, and
//! the shadow and the rim that belong with it have to travel in the *same*
//! buffer, in one commit, or the picture trails its own edge while it is dragged.
//! A ten-bit `wl_shm` buffer cannot carry that — its alpha is two bits — while
//! `ABGR16161616F` carries ten bits of colour and sixteen of alpha.
//!
//! Everything here is best-effort.  [`Surface::open`] answers `None` on a machine
//! with no EGL, no GBM or no usable render node, and the helper keeps its SDR
//! path; a build without the C headers reports the same once, per context.

use std::ffi::{c_char, c_int, c_void, CStr};
use std::os::fd::RawFd;
use std::path::Path;

use crate::error::{Result, VshotError};

/// `DRM_FORMAT_ABGR16161616F`, the fourcc a half-float picture buffer carries.
pub(crate) const FORMAT_ABGR16161616F: u32 = 0x4834_4241;

/// One allocated picture buffer, as the C side hands it over.  The descriptor is
/// owned by the context — the compositor gets a kernel-level duplicate of it and
/// nobody here closes it — and `slot` is the index [`Surface::present`] answers
/// to.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Buffer {
    pub slot: c_int,
    pub fd: RawFd,
    pub stride: u32,
    pub offset: u32,
    pub modifier: u64,
    pub format: u32,
}

/// The C side's own layout; kept in step with `pin_hdr_fp16.h` by hand.
#[repr(C)]
struct RawBuffer {
    slot: c_int,
    fd: c_int,
    stride: u32,
    offset: u32,
    modifier: u64,
    format: u32,
}

extern "C" {
    fn vshot_fp16_open(render_node: *const c_char, why: *mut c_char, why_len: usize)
        -> *mut c_void;
    fn vshot_fp16_close(ctx: *mut c_void);
    fn vshot_fp16_alloc(
        ctx: *mut c_void,
        width: u32,
        height: u32,
        format: u32,
        modifier: u64,
        out: *mut RawBuffer,
        why: *mut c_char,
        why_len: usize,
    ) -> c_int;
    fn vshot_fp16_free_buffer(ctx: *mut c_void, buffer: *mut RawBuffer);
    fn vshot_fp16_image(
        ctx: *mut c_void,
        id: u64,
        words: *const u32,
        width: u32,
        height: u32,
    ) -> c_int;
    fn vshot_fp16_mask(
        ctx: *mut c_void,
        id: u64,
        width: c_int,
        height: c_int,
        radius: c_int,
        spread: c_int,
        offset: c_int,
        opacity: c_int,
    ) -> c_int;
    fn vshot_fp16_drop(ctx: *mut c_void, id: u64);
    fn vshot_fp16_begin(ctx: *mut c_void, width: u32, height: u32) -> c_int;
    fn vshot_fp16_clear(ctx: *mut c_void, x: c_int, y: c_int, width: c_int, height: c_int);
    fn vshot_fp16_draw_shadow(
        ctx: *mut c_void,
        mask_id: u64,
        x: c_int,
        y: c_int,
        width: c_int,
        height: c_int,
    ) -> c_int;
    fn vshot_fp16_draw_image(
        ctx: *mut c_void,
        image_id: u64,
        x: c_int,
        y: c_int,
        width: c_int,
        height: c_int,
        radius: c_int,
    ) -> c_int;
    #[allow(clippy::too_many_arguments)]
    fn vshot_fp16_draw_rim(
        ctx: *mut c_void,
        x: c_int,
        y: c_int,
        width: c_int,
        height: c_int,
        radius: c_int,
        thickness: c_int,
        red: f32,
        green: f32,
        blue: f32,
        alpha: f32,
    ) -> c_int;
    fn vshot_fp16_present(
        ctx: *mut c_void,
        slot: c_int,
        x: c_int,
        y: c_int,
        width: c_int,
        height: c_int,
    ) -> c_int;
}

/// The reason the C side keeps for the last thing that failed, read out of the
/// buffer it writes into.
fn why(buffer: &[c_char]) -> String {
    let text = unsafe { CStr::from_ptr(buffer.as_ptr()) };
    text.to_string_lossy().into_owned()
}

/// A drawing context: one render node, one EGL context, one compose texture.
pub(crate) struct Surface {
    raw: *mut c_void,
}

impl std::fmt::Debug for Surface {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("fp16::Surface")
            .finish_non_exhaustive()
    }
}

impl Surface {
    /// Opens `render_node` and makes a context current on it.  The error is the
    /// C side's own reason, for the debug trace.
    pub(crate) fn open(render_node: &Path) -> std::result::Result<Self, String> {
        let path = std::ffi::CString::new(render_node.as_os_str().as_encoded_bytes())
            .map_err(|_| "the render node's path has a NUL in it".to_string())?;
        let mut buffer = [0 as c_char; 256];
        let raw = unsafe { vshot_fp16_open(path.as_ptr(), buffer.as_mut_ptr(), buffer.len()) };
        if raw.is_null() {
            return Err(why(&buffer));
        }
        Ok(Self { raw })
    }

    /// Allocates one picture buffer of `width` x `height` with `modifier`.
    pub(crate) fn alloc(&self, width: u32, height: u32, modifier: u64) -> Result<Buffer> {
        let mut out = RawBuffer {
            slot: -1,
            fd: -1,
            stride: 0,
            offset: 0,
            modifier: 0,
            format: 0,
        };
        let mut buffer = [0 as c_char; 256];
        let code = unsafe {
            vshot_fp16_alloc(
                self.raw,
                width,
                height,
                FORMAT_ABGR16161616F,
                modifier,
                &mut out,
                buffer.as_mut_ptr(),
                buffer.len(),
            )
        };
        if code != 0 {
            return Err(VshotError::PinSurface(why(&buffer)));
        }
        Ok(Buffer {
            slot: out.slot,
            fd: out.fd,
            stride: out.stride,
            offset: out.offset,
            modifier: out.modifier,
            format: out.format,
        })
    }

    pub(crate) fn free_buffer(&self, buffer: &Buffer) {
        let mut raw = RawBuffer {
            slot: buffer.slot,
            fd: buffer.fd,
            stride: buffer.stride,
            offset: buffer.offset,
            modifier: buffer.modifier,
            format: buffer.format,
        };
        unsafe { vshot_fp16_free_buffer(self.raw, &mut raw) };
    }

    /// Uploads one pin's pixels, the packed ten-bit words the capture wrote.
    /// Kept under `id` until it is dropped, so a drag re-uploads nothing.
    pub(crate) fn image(&self, id: u64, words: &[u32], width: u32, height: u32) -> Result<()> {
        let code = unsafe { vshot_fp16_image(self.raw, id, words.as_ptr(), width, height) };
        if code != 0 {
            return Err(VshotError::PinSurface(
                "the pin's pixels could not be uploaded".into(),
            ));
        }
        Ok(())
    }

    /// Builds (and keeps under `id`) the soft mask a shadow is drawn from.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn mask(
        &self,
        id: u64,
        width: i32,
        height: i32,
        radius: i32,
        spread: i32,
        offset: i32,
        opacity: i32,
    ) -> Result<()> {
        let code = unsafe {
            vshot_fp16_mask(self.raw, id, width, height, radius, spread, offset, opacity)
        };
        if code != 0 {
            return Err(VshotError::PinSurface(
                "the pin's shadow could not be built".into(),
            ));
        }
        Ok(())
    }

    pub(crate) fn drop(&self, id: u64) {
        unsafe { vshot_fp16_drop(self.raw, id) };
    }

    pub(crate) fn begin(&self, width: u32, height: u32) -> Result<()> {
        if unsafe { vshot_fp16_begin(self.raw, width, height) } != 0 {
            return Err(VshotError::PinSurface(
                "the picture could not be started".into(),
            ));
        }
        Ok(())
    }

    pub(crate) fn clear(&self, x: i32, y: i32, width: i32, height: i32) {
        unsafe { vshot_fp16_clear(self.raw, x, y, width, height) };
    }

    pub(crate) fn draw_shadow(&self, mask_id: u64, x: i32, y: i32, width: i32, height: i32) {
        unsafe { vshot_fp16_draw_shadow(self.raw, mask_id, x, y, width, height) };
    }

    pub(crate) fn draw_image(
        &self,
        image_id: u64,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        radius: i32,
    ) {
        unsafe { vshot_fp16_draw_image(self.raw, image_id, x, y, width, height, radius) };
    }

    /// Draws the stroke around a pin.  `colour` is three PQ codes: only the
    /// caller knows what the light behind a colour is on this output.  `alpha`
    /// is the stroke's opacity, 0..1.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn draw_rim(
        &self,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        radius: i32,
        thickness: i32,
        colour: [f32; 3],
        alpha: f32,
    ) {
        unsafe {
            vshot_fp16_draw_rim(
                self.raw, x, y, width, height, radius, thickness, colour[0], colour[1], colour[2],
                alpha,
            )
        };
    }

    /// Copies a rectangle of the compose texture into `slot`, ready to attach.
    /// `false` when that slot has no buffer in it.
    pub(crate) fn present(&self, slot: c_int, x: i32, y: i32, width: i32, height: i32) -> bool {
        unsafe { vshot_fp16_present(self.raw, slot, x, y, width, height) == 0 }
    }
}

impl Drop for Surface {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            unsafe { vshot_fp16_close(self.raw) };
        }
    }
}

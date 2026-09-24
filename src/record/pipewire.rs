//! The Rust side of the portal's PipeWire client (`pipewire_client.c`).
//!
//! The XDG portal does not hand frames over itself: `OpenPipeWireRemote`
//! returns a file descriptor to the compositor's PipeWire connection, and the
//! frames come from a node on it.  Connecting to that node and turning its
//! buffers into something the encoder can take is C's job — libpipewire is a
//! C library with C callbacks, and the frame arrives on a PipeWire thread,
//! which is exactly the shape a C shim carries well.  [`Stream`] is the RAII
//! wrapper around it.
//!
//! Two things about the C boundary shape this module:
//!
//! * **libpipewire is `dlopen`ed by the shim, not linked.**  The headers are a
//!   build-time dependency (the C file cannot be compiled without them), the
//!   library is not: a machine without it still builds, and the portal route
//!   says what is missing when it is asked for.  When the headers were missing
//!   at build time too, the whole client is compiled out and the stub below
//!   answers for it.
//!
//! * **A frame is borrowed, not owned.**  The pixels live in a buffer the
//!   compositor owns and PipeWire recycles; handing out an owned copy of the
//!   pointer would let a frame outlive its buffer.  [`Frame`] therefore
//!   borrows the stream, and the next `next` gives the previous buffer back —
//!   so at most one frame is in hand at a time, which is what the recording
//!   loop does anyway.

use std::ffi::{c_char, c_int, CStr};
use std::fmt;
use std::marker::PhantomData;
use std::os::fd::RawFd;
use std::time::Duration;

use crate::error::{Result, VshotError};

/// The opaque client from `pipewire_client.c`.
#[repr(C)]
struct VshotPw {
    _private: [u8; 0],
}

/// One frame, as the C client hands it over.  The layout is the C struct's,
/// field for field: `kind` says which of the two shapes it is, and the fields
/// that shape does not use are zero.
///
/// Public because the C client fills it in, but nothing above this module
/// needs it: [`Frame`] is the shape the recording loop reads.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct RawFrame {
    /// 0 none, 1 dma-buf, 2 memory.
    pub kind: c_int,
    pub fd: c_int,
    pub fourcc: u32,
    pub modifier: u64,
    pub offset: c_int,
    pub stride: c_int,
    /// The pixels, for a memory frame; null for a dma-buf.
    pub data: *const u8,
    pub size: u64,
    pub width: c_int,
    pub height: c_int,
    /// The `SPA_VIDEO_FORMAT_*` the stream negotiated.
    pub spa_format: u32,
    /// PipeWire's own sequence number, for the debug trace.
    pub seq: i64,
    pub time_ns: i64,
}

impl Default for RawFrame {
    fn default() -> Self {
        RawFrame {
            kind: 0,
            fd: -1,
            fourcc: 0,
            modifier: 0,
            offset: 0,
            stride: 0,
            data: std::ptr::null(),
            size: 0,
            width: 0,
            height: 0,
            spa_format: 0,
            seq: 0,
            time_ns: 0,
        }
    }
}

/// What a frame's kind is: a dma-buf the encoder imports, or memory.
const KIND_DMABUF: c_int = 1;
const KIND_MEMORY: c_int = 2;

/// The C client, when the build had libpipewire's headers.
#[cfg(vshot_pipewire)]
mod binding {
    use super::{c_char, c_int, RawFrame, VshotPw};

    extern "C" {
        pub(super) fn vshot_pw_available() -> c_int;
        pub(super) fn vshot_pw_load_error() -> *const c_char;
        pub(super) fn vshot_pw_open(
            fd: c_int,
            node_id: u32,
            allow_dmabuf: c_int,
            fps: c_int,
            timeout_ms: c_int,
            err: *mut c_char,
            err_len: c_int,
        ) -> *mut VshotPw;
        pub(super) fn vshot_pw_geometry(
            pw: *mut VshotPw,
            width: *mut c_int,
            height: *mut c_int,
            spa_format: *mut u32,
            is_dmabuf: *mut c_int,
        ) -> c_int;
        pub(super) fn vshot_pw_next(
            pw: *mut VshotPw,
            out: *mut RawFrame,
            timeout_ms: c_int,
        ) -> c_int;
        pub(super) fn vshot_pw_error(pw: *mut VshotPw) -> *const c_char;
        pub(super) fn vshot_pw_close(pw: *mut VshotPw);
    }
}

/// The same entry points, for a build that had no libpipewire headers: every
/// one of them answers "there is no client", so the portal route fails with a
/// sentence naming the package instead of with a link error at build time.
#[cfg(not(vshot_pipewire))]
mod binding {
    use super::{c_char, c_int, RawFrame, VshotPw};

    /// The message every entry point answers with; a `&'static` string so the
    /// pointer it hands out stays valid.
    const MESSAGE: &[u8] =
        b"this build was made without libpipewire's headers, so the XDG portal's \
screen-cast stream cannot be read; install libpipewire and rebuild\n\0";

    fn message() -> *const c_char {
        MESSAGE.as_ptr().cast::<c_char>()
    }

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pw_available() -> c_int {
        0
    }

    pub(super) unsafe fn vshot_pw_load_error() -> *const c_char {
        message()
    }

    #[allow(clippy::too_many_arguments)]
    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pw_open(
        fd: c_int,
        node_id: u32,
        allow_dmabuf: c_int,
        fps: c_int,
        timeout_ms: c_int,
        err: *mut c_char,
        err_len: c_int,
    ) -> *mut VshotPw {
        std::ptr::null_mut()
    }

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pw_geometry(
        pw: *mut VshotPw,
        width: *mut c_int,
        height: *mut c_int,
        spa_format: *mut u32,
        is_dmabuf: *mut c_int,
    ) -> c_int {
        -1
    }

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pw_next(
        pw: *mut VshotPw,
        out: *mut RawFrame,
        timeout_ms: c_int,
    ) -> c_int {
        -1
    }

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pw_error(pw: *mut VshotPw) -> *const c_char {
        message()
    }

    #[allow(unused_variables)]
    pub(super) unsafe fn vshot_pw_close(pw: *mut VshotPw) {}
}

/// Whether the portal's frames can be read at all: libpipewire present at run
/// time, and the client compiled in.
pub fn available() -> bool {
    unsafe { binding::vshot_pw_available() != 0 }
}

/// Why they cannot be, when they cannot.
pub fn load_error() -> String {
    take(unsafe { binding::vshot_pw_load_error() })
}

fn take(pointer: *const c_char) -> String {
    if pointer.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(pointer) }
        .to_string_lossy()
        .into_owned()
}

/// A frame's own size, in pixels, and the SPA format its pixels are in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Geometry {
    pub width: u32,
    pub height: u32,
    /// `SPA_VIDEO_FORMAT_*`: what the pixels are, for the software path's
    /// byte-order conversion.
    pub spa_format: u32,
    /// Whether the frames arrive as dma-bufs (the zero-copy shape) rather
    /// than memory.
    pub dmabuf: bool,
}

/// A dma-buf frame's import parameters: the four numbers plus the size the
/// encoder needs to import it, in the form `Recorder::frame_dmabuf` takes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Dmabuf {
    pub fd: RawFd,
    pub fourcc: u32,
    pub modifier: u64,
    pub offset: i32,
    pub stride: i32,
}

/// A connected PipeWire stream, reading the node the portal's `Start` named.
///
/// One frame is in hand at a time: [`Stream::next`] returns a frame that
/// borrows the stream, and the buffer goes back to PipeWire on the following
/// call (or when the stream closes).  That is not a restriction to design
/// around — the recording loop encodes each frame before asking for the next
/// one — and it is what keeps the pixels valid for as long as they are
/// reachable.
pub struct Stream {
    handle: *mut VshotPw,
}

// The C client owns a PipeWire thread loop with its own synchronisation; the
// handle itself is only ever used from the thread that opened it, but moving
// that thread's ownership between threads is sound.
unsafe impl Send for Stream {}

impl Stream {
    /// Connects to `node_id` on the PipeWire connection `fd` belongs to.
    ///
    /// `allow_dmabuf` asks for dma-buf buffers, which is the zero-copy shape
    /// the encoder imports directly; `false` asks for memory buffers, which
    /// the software path converts.  `fps` is the rate asked of the compositor
    /// — a screen cast produces a frame when the screen changes, so it is a
    /// limit on how often the source will sample, not a promise that frames
    /// arrive that often.  `timeout` bounds the negotiation, which is a
    /// handful of round trips on the local socket rather than anything a user
    /// takes part in.
    pub fn open(
        fd: RawFd,
        node_id: u32,
        allow_dmabuf: bool,
        fps: u32,
        timeout: Duration,
    ) -> Result<Stream> {
        // The C client writes its own sentence here, because it is the side
        // that knows which stage failed.
        let mut buffer = [0 as c_char; 512];
        let handle = unsafe {
            binding::vshot_pw_open(
                fd,
                node_id,
                c_int::from(allow_dmabuf),
                c_int::try_from(fps).unwrap_or(60),
                milliseconds(timeout),
                buffer.as_mut_ptr(),
                buffer.len() as c_int,
            )
        };
        if handle.is_null() {
            return Err(VshotError::Recording(format!(
                "could not read the portal's screen-cast stream: {}",
                take(buffer.as_ptr())
            )));
        }
        Ok(Stream { handle })
    }

    /// The stream's negotiated shape, once it has one.
    pub fn geometry(&self) -> Result<Geometry> {
        let (mut width, mut height) = (0 as c_int, 0 as c_int);
        let (mut spa_format, mut is_dmabuf) = (0_u32, 0 as c_int);
        let code = unsafe {
            binding::vshot_pw_geometry(
                self.handle,
                &mut width,
                &mut height,
                &mut spa_format,
                &mut is_dmabuf,
            )
        };
        if code != 0 {
            return Err(VshotError::Recording(format!(
                "the portal's screen-cast stream has no shape yet: {}",
                self.error()
            )));
        }
        let (width, height) = match (u32::try_from(width), u32::try_from(height)) {
            (Ok(width), Ok(height)) => (width, height),
            _ => {
                return Err(VshotError::Recording(format!(
                    "the portal offered a frame of {width}x{height}, which is not a size"
                )))
            }
        };
        Ok(Geometry {
            width,
            height,
            spa_format,
            dmabuf: is_dmabuf != 0,
        })
    }

    /// Waits up to `timeout` for the next frame.
    ///
    /// `None` means nothing arrived in time, which is normal rather than a
    /// failure: the compositor sends a frame when the screen it is casting
    /// changes and is otherwise quiet, so a still screen produces a long
    /// gap.  An error means the stream itself ended.
    pub fn next(&mut self, timeout: Duration) -> Result<Option<Frame<'_>>> {
        let mut raw = RawFrame::default();
        let code = unsafe { binding::vshot_pw_next(self.handle, &mut raw, milliseconds(timeout)) };
        match code {
            0 => Ok(Some(Frame {
                raw,
                _borrow: PhantomData,
            })),
            1 => Ok(None),
            _ => Err(VshotError::Recording(format!(
                "the portal's screen-cast stream ended: {}",
                self.error()
            ))),
        }
    }

    /// Why the stream ended, when it did.
    pub fn error(&self) -> String {
        take(unsafe { binding::vshot_pw_error(self.handle) })
    }
}

impl Drop for Stream {
    fn drop(&mut self) {
        // Closing gives any frame still in hand back to PipeWire first, so the
        // compositor is not left holding a buffer until the connection dies.
        unsafe { binding::vshot_pw_close(self.handle) };
    }
}

impl fmt::Debug for Stream {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.debug_struct("pipewire::Stream").finish()
    }
}

/// One frame from the stream.  Alive at most until the next
/// [`Stream::next`], which returns its buffer to PipeWire.
pub struct Frame<'a> {
    raw: RawFrame,
    _borrow: PhantomData<&'a mut Stream>,
}

impl Frame<'_> {
    /// The frame's size in the pixels it actually carries, which is the
    /// stream's geometry.
    pub fn size(&self) -> (u32, u32) {
        (
            u32::try_from(self.raw.width).unwrap_or(0),
            u32::try_from(self.raw.height).unwrap_or(0),
        )
    }

    /// The `SPA_VIDEO_FORMAT_*` of the pixels, for the software path's
    /// conversion.
    pub fn spa_format(&self) -> u32 {
        self.raw.spa_format
    }

    /// The dma-buf this frame lives in, for the zero-copy path.
    pub fn dmabuf(&self) -> Option<Dmabuf> {
        (self.raw.kind == KIND_DMABUF && self.raw.fd >= 0).then_some(Dmabuf {
            fd: self.raw.fd,
            fourcc: self.raw.fourcc,
            modifier: self.raw.modifier,
            offset: self.raw.offset,
            stride: self.raw.stride,
        })
    }

    /// The pixels, for a frame that came through memory, with the stride its
    /// rows have: a buffer's rows can be padded, so the caller needs both.
    pub fn memory<'a>(&self) -> Option<Memory<'a>> {
        if self.raw.kind != KIND_MEMORY || self.raw.data.is_null() {
            return None;
        }
        let size = usize::try_from(self.raw.size).ok()?;
        if size == 0 {
            return None;
        }
        // SAFETY: the C client owns this buffer and does not touch it while
        // the frame is in hand — it is the buffer PipeWire handed over, still
        // held on the stream until the next `next` gives it back, and the
        // frame cannot outlive that call because it borrows the stream.
        let bytes = unsafe { std::slice::from_raw_parts(self.raw.data, size) };
        Some(Memory {
            bytes,
            stride: usize::try_from(self.raw.stride).unwrap_or(0),
        })
    }

    /// PipeWire's sequence number, for the debug trace: gaps in it are frames
    /// the compositor did not send.
    pub fn sequence(&self) -> i64 {
        self.raw.seq
    }
}

impl fmt::Debug for Frame<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let (width, height) = self.size();
        formatter
            .debug_struct("pipewire::Frame")
            .field("size", &format_args!("{width}x{height}"))
            .field("kind", &self.raw.kind)
            .field("spa_format", &self.raw.spa_format)
            .field("sequence", &self.raw.seq)
            .finish()
    }
}

/// A memory-backed frame's pixels: the buffer PipeWire mapped for the stream,
/// and the stride its rows have (a row can be padded at its end, and the
/// pixels still have to be read row by row).
pub struct Memory<'a> {
    pub bytes: &'a [u8],
    pub stride: usize,
}

impl fmt::Debug for Memory<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("pipewire::Memory")
            .field("bytes", &self.bytes.len())
            .field("stride", &self.stride)
            .finish()
    }
}

/// A `Duration` in the milliseconds the C client counts in, clamped to what
/// an `int` holds: a timeout longer than 24 days is "wait forever" in
/// practice, and the alternative is an overflow.
pub(crate) fn milliseconds(timeout: Duration) -> c_int {
    c_int::try_from(timeout.as_millis()).unwrap_or(c_int::MAX)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The C struct and the Rust mirror have to agree field for field, or the
    /// client writes a frame into the wrong places.  The offsets are the C
    /// layout's, computed the same way a C compiler would.
    #[test]
    fn the_frame_layout_is_the_c_structs() {
        assert_eq!(std::mem::align_of::<RawFrame>(), 8);
        assert_eq!(std::mem::offset_of!(RawFrame, kind), 0);
        assert_eq!(std::mem::offset_of!(RawFrame, fd), 4);
        assert_eq!(std::mem::offset_of!(RawFrame, fourcc), 8);
        assert_eq!(std::mem::offset_of!(RawFrame, modifier), 16);
        assert_eq!(std::mem::offset_of!(RawFrame, offset), 24);
        assert_eq!(std::mem::offset_of!(RawFrame, stride), 28);
        assert_eq!(std::mem::offset_of!(RawFrame, data), 32);
        assert_eq!(std::mem::offset_of!(RawFrame, size), 40);
        assert_eq!(std::mem::offset_of!(RawFrame, width), 48);
        assert_eq!(std::mem::offset_of!(RawFrame, height), 52);
        assert_eq!(std::mem::offset_of!(RawFrame, spa_format), 56);
        assert_eq!(std::mem::offset_of!(RawFrame, seq), 64);
        assert_eq!(std::mem::offset_of!(RawFrame, time_ns), 72);
        assert_eq!(std::mem::size_of::<RawFrame>(), 80);
    }

    /// The available/why pair has to be consistent whichever binding was
    /// compiled in.
    #[test]
    fn availability_comes_with_a_reason() {
        if available() {
            assert_eq!(load_error(), "");
        } else {
            assert!(
                !load_error().is_empty(),
                "an unavailable client has to say why"
            );
        }
    }

    /// Milliseconds clamp rather than wrap: u64::MAX milliseconds would be a
    /// negative wait without this.
    #[test]
    fn long_timeouts_saturate() {
        assert_eq!(milliseconds(Duration::ZERO), 0);
        assert_eq!(milliseconds(Duration::from_millis(1500)), 1500);
        assert_eq!(
            milliseconds(Duration::from_millis(u64::MAX / 2)),
            c_int::MAX
        );
    }
}

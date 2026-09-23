use std::collections::HashMap;
use std::os::fd::{AsFd, BorrowedFd};
use std::time::{Duration, Instant};

use memmap2::{MmapMut, MmapOptions};
use wayland_client::protocol::{
    wl_buffer, wl_compositor, wl_output, wl_registry, wl_shm, wl_shm_pool,
};
use wayland_client::{Connection, Dispatch, EventQueue, QueueHandle, WEnum};
use wayland_protocols::wp::linux_dmabuf::zv1::client::{
    zwp_linux_buffer_params_v1, zwp_linux_dmabuf_v1,
};
use wayland_protocols_wlr::screencopy::v1::client::{
    zwlr_screencopy_frame_v1, zwlr_screencopy_manager_v1,
};

use crate::error::{Result, VshotError};
use crate::geometry::{Rect, Size};
use crate::model::Frame;

use super::dmabuf::{DmabufFrame, GbmBuffer};

#[derive(Debug)]
struct CaptureBuffer {
    _file: tempfile::NamedTempFile,
    map: MmapMut,
    buffer: wl_buffer::WlBuffer,
    width: u32,
    height: u32,
    stride: usize,
    format: wl_shm::Format,
}

impl CaptureBuffer {
    fn new<State>(
        shm: &wl_shm::WlShm,
        width: u32,
        height: u32,
        stride: u32,
        format: wl_shm::Format,
        qh: &QueueHandle<State>,
    ) -> Result<Self>
    where
        State: Dispatch<wl_shm_pool::WlShmPool, ()> + Dispatch<wl_buffer::WlBuffer, ()> + 'static,
    {
        if !matches!(format, wl_shm::Format::Argb8888 | wl_shm::Format::Xrgb8888) {
            return Err(VshotError::UnsupportedOutput(format!(
                "wlr-screencopy returned unsupported wl_shm format {format:?}"
            )));
        }
        let width_usize = usize::try_from(width)
            .map_err(|_| VshotError::WaylandProtocol("capture width is too large".into()))?;
        let height_usize = usize::try_from(height)
            .map_err(|_| VshotError::WaylandProtocol("capture height is too large".into()))?;
        let stride = usize::try_from(stride)
            .map_err(|_| VshotError::WaylandProtocol("capture stride is too large".into()))?;
        let minimum_stride = width_usize
            .checked_mul(4)
            .ok_or_else(|| VshotError::WaylandProtocol("capture stride overflows".into()))?;
        if stride < minimum_stride {
            return Err(VshotError::WaylandProtocol(format!(
                "compositor returned a stride of {stride} for a {width}-pixel frame"
            )));
        }
        let bytes = stride
            .checked_mul(height_usize)
            .ok_or_else(|| VshotError::WaylandProtocol("capture buffer size overflows".into()))?;
        let pool_size = i32::try_from(bytes).map_err(|_| {
            VshotError::WaylandProtocol("capture buffer exceeds Wayland's signed size limit".into())
        })?;
        let file = tempfile::Builder::new()
            .prefix("vshot-")
            .tempfile_in("/dev/shm")
            .map_err(|source| {
                VshotError::WaylandProtocol(format!("failed to create capture SHM file: {source}"))
            })?;
        file.as_file()
            .set_len(u64::try_from(bytes).map_err(|_| {
                VshotError::WaylandProtocol("capture buffer size is invalid".into())
            })?)
            .map_err(|source| {
                VshotError::WaylandProtocol(format!("failed to size capture SHM file: {source}"))
            })?;
        let map =
            unsafe { MmapOptions::new().len(bytes).map_mut(file.as_file()) }.map_err(|source| {
                VshotError::WaylandProtocol(format!("failed to map capture SHM buffer: {source}"))
            })?;
        let pool = shm.create_pool(file.as_fd(), pool_size, qh, ());
        let buffer = pool.create_buffer(
            0,
            i32::try_from(width)
                .map_err(|_| VshotError::WaylandProtocol("capture width is too large".into()))?,
            i32::try_from(height)
                .map_err(|_| VshotError::WaylandProtocol("capture height is too large".into()))?,
            i32::try_from(stride)
                .map_err(|_| VshotError::WaylandProtocol("capture stride is too large".into()))?,
            format,
            qh,
            (),
        );
        pool.destroy();
        Ok(Self {
            _file: file,
            map,
            buffer,
            width,
            height,
            stride,
            format,
        })
    }

    fn into_frame(self, y_invert: bool) -> Result<Frame> {
        convert_shm_pixels(
            &self.map,
            self.width,
            self.height,
            self.stride,
            self.format,
            y_invert,
        )
    }
}

fn convert_shm_pixels(
    map: &[u8],
    width: u32,
    height: u32,
    stride: usize,
    format: wl_shm::Format,
    y_invert: bool,
) -> Result<Frame> {
    if !matches!(format, wl_shm::Format::Argb8888 | wl_shm::Format::Xrgb8888) {
        return Err(VshotError::UnsupportedOutput(format!(
            "unsupported wl_shm format {format:?}"
        )));
    }
    let width = usize::try_from(width)
        .map_err(|_| VshotError::WaylandProtocol("capture width is too large".into()))?;
    let height = usize::try_from(height)
        .map_err(|_| VshotError::WaylandProtocol("capture height is too large".into()))?;
    let minimum_stride = width
        .checked_mul(4)
        .ok_or_else(|| VshotError::WaylandProtocol("capture stride overflows".into()))?;
    if stride < minimum_stride {
        return Err(VshotError::WaylandProtocol(
            "capture stride is smaller than the frame width".into(),
        ));
    }
    let expected = stride
        .checked_mul(height)
        .ok_or_else(|| VshotError::WaylandProtocol("capture buffer size overflows".into()))?;
    if map.len() < expected {
        return Err(VshotError::WaylandProtocol(
            "capture SHM mapping is smaller than the advertised stride".into(),
        ));
    }
    let pixel_count = width
        .checked_mul(height)
        .and_then(|area| area.checked_mul(4))
        .ok_or_else(|| VshotError::WaylandProtocol("capture frame is too large".into()))?;
    let mut pixels = vec![0u8; pixel_count];
    // The pixel format is BGRA in memory and vshot works in RGBA — the same
    // four bytes with the red and blue ends swapped.  Swapping them as whole
    // 32-bit words instead of one byte at a time is what lets the loop go
    // several times faster: it moves the same pixels with a fraction of the
    // memory traffic, which matters because this runs once per grabbed frame.
    let alpha = match format {
        wl_shm::Format::Argb8888 => None,
        wl_shm::Format::Xrgb8888 => Some(0xFF00_0000),
        _ => unreachable!("unsupported format was rejected above"),
    };
    let row_bytes = width * 4;
    for (destination_y, destination_row) in pixels.chunks_exact_mut(row_bytes).enumerate() {
        let source_y = if y_invert {
            height - 1 - destination_y
        } else {
            destination_y
        };
        let source_row = &map[source_y * stride..source_y * stride + row_bytes];
        for (destination, source) in destination_row
            .chunks_exact_mut(4)
            .zip(source_row.chunks_exact(4))
        {
            let word = u32::from_le_bytes([source[0], source[1], source[2], source[3]]);
            let swapped = (word & 0x0000_00FF) << 16
                | (word & 0x00FF_0000) >> 16
                | (word & 0xFF00_FF00)
                | alpha.unwrap_or(word & 0xFF00_0000);
            destination.copy_from_slice(&swapped.to_le_bytes());
        }
    }
    Frame::new(Size::new(width as u32, height as u32), pixels)
}

impl Drop for CaptureBuffer {
    fn drop(&mut self) {
        self.buffer.destroy();
    }
}

#[derive(Debug)]
struct PendingCapture {
    _frame: zwlr_screencopy_frame_v1::ZwlrScreencopyFrameV1,
    buffer: Option<CaptureBuffer>,
    /// The shm offer from the `buffer` event, remembered so the shm buffer
    /// is allocated lazily — only when the dma-buf path does not take the
    /// frame.  (Version 3 defers the copy to `buffer_done`, so nothing
    /// needs the buffer before then.)
    shm_offer: Option<ShmOffer>,
    /// Set for a capture that wants a linux-dmabuf buffer.
    want_dmabuf: bool,
    /// The pool slot the compositor's `linux_dmabuf` event picked.
    dmabuf_slot: Option<usize>,
    y_invert: bool,
    copy_sent: bool,
    complete: bool,
    error: Option<String>,
}

/// The wl_shm buffer parameters of one capture, from its `buffer` event.
#[derive(Clone, Copy, Debug)]
struct ShmOffer {
    format: wl_shm::Format,
    width: u32,
    height: u32,
    stride: u32,
}

/// Size of the zero-copy buffer pool.  Each slot holds a full-size dma-buf
/// (33 MB at 4K); four slots let the compositor render into one while the
/// encoder still reads the previous ones, without the cost of a deeper pool.
const DMABUF_POOL_SLOTS: usize = 4;

/// One pooled dma-buf with the `wl_buffer` the compositor renders into.
struct DmabufSlot {
    gbm: GbmBuffer,
    wl_buffer: wl_buffer::WlBuffer,
}

impl std::fmt::Debug for DmabufSlot {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("DmabufSlot")
            .field("fd", &self.gbm.fd())
            .finish_non_exhaustive()
    }
}

impl Drop for DmabufSlot {
    fn drop(&mut self) {
        self.wl_buffer.destroy();
    }
}

/// A rotating set of dma-bufs a screencopy capture can render into,
/// together with the shape they were built for.  A capture whose offer does
/// not match the pool's shape falls back to the shm path rather than
/// rebuilding mid-recording.
#[derive(Debug)]
struct DmabufPool {
    slots: Vec<DmabufSlot>,
    next: usize,
    width: u32,
    height: u32,
    fourcc: u32,
}

#[derive(Clone, Copy, Debug)]
struct OutputUserData {
    global_id: u32,
}

#[derive(Debug, Default)]
struct CaptureState {
    shm: Option<wl_shm::WlShm>,
    manager: Option<zwlr_screencopy_manager_v1::ZwlrScreencopyManagerV1>,
    manager_version: u32,
    dmabuf: Option<zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1>,
    dmabuf_version: u32,
    outputs: HashMap<u32, wl_output::WlOutput>,
    output_names: HashMap<u32, String>,
    pending: Option<PendingCapture>,
    /// Zero-copy pool, built on first use and reused for the session.
    pool: Option<DmabufPool>,
    /// The last linux-dmabuf offer the compositor made, whatever capture it
    /// went by: `(fourcc, width, height)`.  A probe reads it after a plain
    /// shm capture, because every frame event carries the offer whether or
    /// not one wants the buffer.
    probe_offer: Option<(u32, u32, u32)>,
    /// The y-inversion flag of the last capture.  A probe reads it to know
    /// whether the zero-copy path (which cannot flip) is usable.
    probe_y_invert: bool,
}

impl CaptureState {
    fn fail_pending(&mut self, message: impl Into<String>) {
        if let Some(pending) = self.pending.as_mut() {
            pending.error = Some(message.into());
            pending.complete = true;
        }
    }
}

pub struct WlrCapture {
    event_queue: EventQueue<CaptureState>,
    state: CaptureState,
}

impl WlrCapture {
    pub fn connect() -> Result<Self> {
        let connection = Connection::connect_to_env()
            .map_err(|error| VshotError::WaylandConnection(error.to_string()))?;
        let mut event_queue = connection.new_event_queue::<CaptureState>();
        let qh = event_queue.handle();
        let state = CaptureState::default();
        connection.display().get_registry(&qh, ());
        let mut state = state;
        event_queue
            .roundtrip(&mut state)
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        event_queue
            .roundtrip(&mut state)
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        if state.shm.is_none() {
            return Err(VshotError::MissingCapability("wl_shm".into()));
        }
        if state.manager.is_none() {
            return Err(VshotError::MissingCapability(
                "zwlr_screencopy_manager_v1".into(),
            ));
        }
        if state.outputs.is_empty() {
            return Err(VshotError::MissingCapability(
                "at least one wl_output".into(),
            ));
        }
        Ok(Self { event_queue, state })
    }

    pub fn capture_output(&mut self, name: &str, cursor: bool) -> Result<Frame> {
        self.capture(name, None, cursor)
    }

    /// Captures one rectangle of an output, given in *output-local logical*
    /// coordinates — the space `capture_output_region` is defined in, and the
    /// space the region picker works in.
    ///
    /// Only that rectangle is rendered into the buffer and converted, so a
    /// scrolled capture of a small region costs a fraction of a full-screen
    /// grab.  That is what lets frames be taken fast enough to keep up with a
    /// page that is still moving.  The compositor clips the rectangle to the
    /// output's extents.
    pub fn capture_region(&mut self, name: &str, region: Rect, cursor: bool) -> Result<Frame> {
        self.capture(name, Some(region), cursor)
    }

    fn capture(&mut self, name: &str, region: Option<Rect>, cursor: bool) -> Result<Frame> {
        if self.state.pending.is_some() {
            return Err(VshotError::WaylandProtocol(
                "a capture is already in progress".into(),
            ));
        }
        let (global_id, output) = self
            .state
            .output_names
            .iter()
            .find(|(_, output_name)| output_name.as_str() == name)
            .and_then(|(global_id, _)| {
                self.state
                    .outputs
                    .get(global_id)
                    .cloned()
                    .map(|output| (*global_id, output))
            })
            .ok_or_else(|| VshotError::IncompleteTopology(format!("unknown output `{name}`")))?;
        let manager =
            self.state.manager.as_ref().cloned().ok_or_else(|| {
                VshotError::MissingCapability("zwlr_screencopy_manager_v1".into())
            })?;
        let qh = self.event_queue.handle();
        let overlay_cursor = if cursor { 1 } else { 0 };
        let frame = match region {
            Some(region) => {
                let (x, y, width, height) = region_arguments(region)?;
                manager.capture_output_region(overlay_cursor, &output, x, y, width, height, &qh, ())
            }
            None => manager.capture_output(overlay_cursor, &output, &qh, ()),
        };
        self.state.pending = Some(PendingCapture {
            _frame: frame,
            buffer: None,
            shm_offer: None,
            want_dmabuf: false,
            dmabuf_slot: None,
            y_invert: false,
            copy_sent: false,
            complete: false,
            error: None,
        });
        let _ = global_id;

        let wait_started = Instant::now();
        self.dispatch_until(Instant::now() + Duration::from_secs(10))?;
        let wait_ms = wait_started.elapsed().as_secs_f64() * 1000.0;
        let pending = self.state.pending.take().ok_or_else(|| {
            VshotError::WaylandProtocol("capture disappeared before completion".into())
        })?;
        if let Some(error) = pending.error {
            return Err(VshotError::WaylandProtocol(error));
        }
        let buffer = pending.buffer.ok_or_else(|| {
            VshotError::WaylandProtocol("screencopy completed without a buffer".into())
        })?;
        let convert_started = Instant::now();
        let frame = buffer.into_frame(pending.y_invert);
        if std::env::var_os("VSHOT_RECORD_DEBUG").is_some() {
            eprintln!(
                "vshot:   capture: compositor+shm-copy {wait_ms:.1}ms pixel-convert {:.1}ms",
                convert_started.elapsed().as_secs_f64() * 1000.0
            );
        }
        frame
    }

    /// Captures one output straight into a dma-buf: the compositor renders
    /// into a buffer the encoder can import, and no pixel passes through
    /// the CPU.  This is the fast path the recorder runs `record monitor`
    /// through; the shm `capture_output` stays as the compatibility path.
    ///
    /// The buffer pool is built on the first call, from the compositor's
    /// own linux-dmabuf offer — that is where the fourcc and the size come
    /// from.  Later calls rotate through the pool.
    ///
    /// Fails — and the caller falls back to `capture_output` — when the
    /// session lacks linux-dmabuf, the compositor offers no dma-buf, the
    /// buffer's shape does not match the pool, or the frame needs a
    /// y-flip the zero-copy chain cannot apply.
    pub fn capture_output_dmabuf(&mut self, name: &str, cursor: bool) -> Result<DmabufFrame> {
        if self.state.pending.is_some() {
            return Err(VshotError::WaylandProtocol(
                "a capture is already in progress".into(),
            ));
        }
        if !super::dmabuf::available() {
            return Err(VshotError::MissingCapability(format!(
                "zero-copy capture needs libgbm: {}",
                super::dmabuf::load_error()
            )));
        }
        if self.state.manager_version < 3 {
            return Err(VshotError::MissingCapability(
                "screencopy version 3 (linux-dmabuf buffers)".into(),
            ));
        }
        let (global_id, output) = self
            .state
            .output_names
            .iter()
            .find(|(_, output_name)| output_name.as_str() == name)
            .and_then(|(global_id, _)| {
                self.state
                    .outputs
                    .get(global_id)
                    .cloned()
                    .map(|output| (*global_id, output))
            })
            .ok_or_else(|| VshotError::IncompleteTopology(format!("unknown output `{name}`")))?;
        let manager =
            self.state.manager.as_ref().cloned().ok_or_else(|| {
                VshotError::MissingCapability("zwlr_screencopy_manager_v1".into())
            })?;
        let qh = self.event_queue.handle();
        let overlay_cursor = if cursor { 1 } else { 0 };
        let frame = manager.capture_output(overlay_cursor, &output, &qh, ());
        self.state.pending = Some(PendingCapture {
            _frame: frame,
            buffer: None,
            shm_offer: None,
            want_dmabuf: true,
            dmabuf_slot: None,
            y_invert: false,
            copy_sent: false,
            complete: false,
            error: None,
        });
        let _ = global_id;

        let wait_started = Instant::now();
        self.dispatch_until(Instant::now() + Duration::from_secs(10))?;
        let wait_ms = wait_started.elapsed().as_secs_f64() * 1000.0;
        let pending = self.state.pending.take().ok_or_else(|| {
            VshotError::WaylandProtocol("capture disappeared before completion".into())
        })?;
        if let Some(error) = pending.error {
            return Err(VshotError::WaylandProtocol(error));
        }
        if pending.y_invert {
            // The GPU path cannot flip; the shm path can.  Say so rather
            // than reading the frame upside down.
            return Err(VshotError::UnsupportedOutput(
                "the compositor renders this output y-inverted, which the zero-copy path cannot \
                 capture; the software path handles it"
                    .into(),
            ));
        }
        let slot = pending.dmabuf_slot.ok_or_else(|| {
            VshotError::UnsupportedOutput(
                "the compositor offered no linux-dmabuf buffer for this capture".into(),
            )
        })?;
        let pool = self.state.pool.as_ref().ok_or_else(|| {
            VshotError::WaylandProtocol("the dma-buf pool disappeared mid-capture".into())
        })?;
        let slot_ref = pool.slots.get(slot).ok_or_else(|| {
            VshotError::WaylandProtocol("the dma-buf pool slot vanished mid-capture".into())
        })?;
        let gbm = &slot_ref.gbm;
        if std::env::var_os("VSHOT_RECORD_DEBUG").is_some() {
            eprintln!(
                "vshot:   capture: compositor+dmabuf-copy {wait_ms:.1}ms (zero copy, no \
                 conversion)"
            );
        }
        Ok(DmabufFrame {
            fd: gbm.fd(),
            fourcc: gbm.fourcc(),
            modifier: gbm.modifier(),
            offset: gbm.offset(),
            stride: gbm.stride(),
            width: gbm.width(),
            height: gbm.height(),
        })
    }

    /// Builds the zero-copy buffer pool for one output shape.  Called
    /// between captures (never mid-capture); the recorder does it once the
    /// first probe of the output has established the fourcc and size from
    /// the compositor's own offer.
    pub fn build_dmabuf_pool(&mut self, width: u32, height: u32, fourcc: u32) -> Result<()> {
        if self.state.pool.is_some() {
            return Ok(());
        }
        let dmabuf = self
            .state
            .dmabuf
            .as_ref()
            .cloned()
            .ok_or_else(|| VshotError::MissingCapability("zwp_linux_dmabuf_v1".into()))?;
        let qh = self.event_queue.handle();
        let mut slots = Vec::with_capacity(DMABUF_POOL_SLOTS);
        for _ in 0..DMABUF_POOL_SLOTS {
            let gbm = GbmBuffer::create(width, height, fourcc)?;
            let modifier = gbm.modifier();
            let params = dmabuf.create_params(&qh, ());
            let borrowed = unsafe { BorrowedFd::borrow_raw(gbm.fd()) };
            params.add(
                borrowed,
                0,
                gbm.offset(),
                gbm.stride(),
                (modifier >> 32) as u32,
                (modifier & 0xffff_ffff) as u32,
            );
            let wl_buffer = params.create_immed(
                i32::try_from(width).map_err(|_| {
                    VshotError::WaylandProtocol("capture width is too large".into())
                })?,
                i32::try_from(height).map_err(|_| {
                    VshotError::WaylandProtocol("capture height is too large".into())
                })?,
                fourcc,
                zwp_linux_buffer_params_v1::Flags::empty(),
                &qh,
                (),
            );
            params.destroy();
            slots.push(DmabufSlot { gbm, wl_buffer });
        }
        self.state.pool = Some(DmabufPool {
            slots,
            next: 0,
            width,
            height,
            fourcc,
        });
        Ok(())
    }

    /// The dma-buf format/size the compositor offered for an output, read
    /// from a plain shm capture: every screencopy frame event carries the
    /// linux-dmabuf offer whether or not a client wants the buffer, so the
    /// offer can be sampled without disturbing the compatibility capture.
    /// The fourcc comes back as the DRM fourcc the pool must use.
    pub fn probe_dmabuf_offer(&mut self, name: &str) -> Result<(u32, u32, u32, bool)> {
        if self.state.manager_version < 3 {
            return Err(VshotError::MissingCapability(
                "screencopy version 3 (linux-dmabuf buffers)".into(),
            ));
        }
        if self.state.dmabuf.is_none() {
            return Err(VshotError::MissingCapability("zwp_linux_dmabuf_v1".into()));
        }
        self.state.probe_offer = None;
        let _frame = self.capture_output(name, false)?;
        let y_invert = self.state.probe_y_invert;
        self.state
            .probe_offer
            .take()
            .map(|(fourcc, width, height)| (fourcc, width, height, y_invert))
            .ok_or_else(|| {
                VshotError::UnsupportedOutput(
                    "the compositor offered no linux-dmabuf buffer for this output".into(),
                )
            })
    }

    fn dispatch_until(&mut self, deadline: Instant) -> Result<()> {
        loop {
            self.event_queue
                .dispatch_pending(&mut self.state)
                .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
            if self
                .state
                .pending
                .as_ref()
                .is_some_and(|pending| pending.complete)
            {
                return Ok(());
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                self.state.pending.take();
                return Err(VshotError::CaptureTimeout);
            }
            self.event_queue
                .flush()
                .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
            let Some(read_guard) = self.event_queue.prepare_read() else {
                continue;
            };
            let fd = read_guard.connection_fd();
            let mut poll_fds = [rustix::event::PollFd::new(
                &fd,
                rustix::event::PollFlags::IN | rustix::event::PollFlags::ERR,
            )];
            let timeout = rustix::event::Timespec::try_from(remaining).map_err(|_| {
                VshotError::WaylandProtocol("capture timeout is out of range".into())
            })?;
            let ready = rustix::event::poll(&mut poll_fds, Some(&timeout)).map_err(|error| {
                VshotError::WaylandProtocol(format!("failed to poll Wayland connection: {error}"))
            })?;
            if ready == 0 {
                drop(read_guard);
                self.state.pending.take();
                return Err(VshotError::CaptureTimeout);
            }
            read_guard
                .read()
                .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        }
    }
}

/// The four integers `capture_output_region` takes, in its own order, checked:
/// a rectangle that will not fit them is not something to send a compositor.
fn region_arguments(region: Rect) -> Result<(i32, i32, i32, i32)> {
    let width = i32::try_from(region.size.width).map_err(|_| {
        VshotError::WaylandProtocol("the capture region is too wide to ask for".into())
    })?;
    let height = i32::try_from(region.size.height).map_err(|_| {
        VshotError::WaylandProtocol("the capture region is too tall to ask for".into())
    })?;
    Ok((region.origin.x, region.origin.y, width, height))
}

impl Dispatch<wl_registry::WlRegistry, ()> for CaptureState {
    fn event(
        state: &mut Self,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_registry::Event::Global {
            name,
            interface,
            version,
        } = event
        {
            match interface.as_str() {
                "wl_shm" if state.shm.is_none() => {
                    state.shm = Some(registry.bind(name, version.min(1), qh, ()));
                }
                "zwlr_screencopy_manager_v1" if state.manager.is_none() => {
                    let bind_version = version.min(3);
                    state.manager_version = bind_version;
                    state.manager = Some(registry.bind(name, bind_version, qh, ()));
                }
                "zwp_linux_dmabuf_v1" if state.dmabuf.is_none() => {
                    // Version 3 is what screencopy's dmabuf event needs;
                    // binding lower is still fine for the shm path.
                    let bind_version = version.min(3);
                    state.dmabuf_version = bind_version;
                    state.dmabuf = Some(registry.bind(name, bind_version, qh, ()));
                }
                "wl_output" => {
                    let output = registry.bind::<wl_output::WlOutput, _, _>(
                        name,
                        version.min(4),
                        qh,
                        OutputUserData { global_id: name },
                    );
                    state.outputs.insert(name, output);
                }
                _ => {}
            }
        }
    }
}

impl Dispatch<wl_shm::WlShm, ()> for CaptureState {
    fn event(
        _: &mut Self,
        _: &wl_shm::WlShm,
        _: wl_shm::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_output::WlOutput, OutputUserData> for CaptureState {
    fn event(
        state: &mut Self,
        _: &wl_output::WlOutput,
        event: wl_output::Event,
        data: &OutputUserData,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_output::Event::Name { name } = event {
            state.output_names.insert(data.global_id, name);
        }
    }
}

impl Dispatch<wl_shm_pool::WlShmPool, ()> for CaptureState {
    fn event(
        _: &mut Self,
        _: &wl_shm_pool::WlShmPool,
        _: wl_shm_pool::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_buffer::WlBuffer, ()> for CaptureState {
    fn event(
        _: &mut Self,
        _: &wl_buffer::WlBuffer,
        _: wl_buffer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1, ()> for CaptureState {
    fn event(
        _: &mut Self,
        _: &zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1,
        _: zwp_linux_dmabuf_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<zwp_linux_buffer_params_v1::ZwpLinuxBufferParamsV1, ()> for CaptureState {
    fn event(
        _: &mut Self,
        _: &zwp_linux_buffer_params_v1::ZwpLinuxBufferParamsV1,
        _: zwp_linux_buffer_params_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<zwlr_screencopy_manager_v1::ZwlrScreencopyManagerV1, ()> for CaptureState {
    fn event(
        _: &mut Self,
        _: &zwlr_screencopy_manager_v1::ZwlrScreencopyManagerV1,
        _: zwlr_screencopy_manager_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<zwlr_screencopy_frame_v1::ZwlrScreencopyFrameV1, ()> for CaptureState {
    fn event(
        state: &mut Self,
        frame: &zwlr_screencopy_frame_v1::ZwlrScreencopyFrameV1,
        event: zwlr_screencopy_frame_v1::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        match event {
            zwlr_screencopy_frame_v1::Event::Buffer {
                format,
                width,
                height,
                stride,
            } => {
                let format = match format {
                    WEnum::Value(format) => format,
                    WEnum::Unknown(value) => {
                        state.fail_pending(format!("unknown screencopy wl_shm format {value}"));
                        return;
                    }
                };
                let copy_immediately = state.manager_version < 3;
                let Some(pending) = state.pending.as_mut() else {
                    return;
                };
                if pending.shm_offer.is_some() {
                    pending.error = Some("screencopy sent more than one SHM buffer".into());
                    pending.complete = true;
                    return;
                }
                pending.shm_offer = Some(ShmOffer {
                    format,
                    width,
                    height,
                    stride,
                });
                // A pre-v3 compositor has no buffer_done: the copy has to
                // go out with the offer, so the shm buffer is built now.
                if copy_immediately && !pending.want_dmabuf {
                    let Some(shm) = state.shm.as_ref().cloned() else {
                        state.fail_pending("wl_shm disappeared during capture");
                        return;
                    };
                    match CaptureBuffer::new(&shm, width, height, stride, format, qh) {
                        Ok(buffer) => {
                            frame.copy(&buffer.buffer);
                            pending.copy_sent = true;
                            pending.buffer = Some(buffer);
                        }
                        Err(error) => {
                            pending.error = Some(error.to_string());
                            pending.complete = true;
                        }
                    }
                }
            }
            zwlr_screencopy_frame_v1::Event::LinuxDmabuf {
                format,
                width,
                height,
            } => {
                // Every frame event carries the offer, wanted or not; keep
                // the newest for `probe_dmabuf_offer`.
                state.probe_offer = Some((format, width, height));
                let Some(pending) = state.pending.as_mut() else {
                    return;
                };
                if !pending.want_dmabuf {
                    return;
                }
                // The offer must match exactly what the pool was built
                // for: the same fourcc and size.  A mismatch (a scale
                // change, a rotated buffer) makes this capture fall back
                // to the shm path.
                if let Some(pool) = state.pool.as_ref() {
                    if pool.fourcc == format && pool.width == width && pool.height == height {
                        pending.dmabuf_slot = Some(pool.next);
                    }
                }
            }
            zwlr_screencopy_frame_v1::Event::BufferDone => {
                // The zero-copy branch first: bind the pool's wl_buffer and
                // copy.  Nothing shm was allocated for this capture.
                if state
                    .pending
                    .as_ref()
                    .is_some_and(|pending| pending.dmabuf_slot.is_some())
                {
                    let Some(pending) = state.pending.as_mut() else {
                        return;
                    };
                    if let Some(pool) = state.pool.as_mut() {
                        let slot = pending.dmabuf_slot.expect("checked above");
                        let slot_ref = &pool.slots[slot];
                        frame.copy(&slot_ref.wl_buffer);
                        pending.copy_sent = true;
                        pool.next = (slot + 1) % pool.slots.len();
                        return;
                    }
                }
                // The shm branch: allocate the buffer now (lazily) and copy.
                let Some(pending) = state.pending.as_mut() else {
                    return;
                };
                if pending.copy_sent {
                    return;
                }
                let Some(offer) = pending.shm_offer else {
                    pending.error =
                        Some("screencopy sent buffer_done without a usable buffer".into());
                    pending.complete = true;
                    return;
                };
                let Some(shm) = state.shm.as_ref().cloned() else {
                    pending.error = Some("wl_shm disappeared during capture".into());
                    pending.complete = true;
                    return;
                };
                match CaptureBuffer::new(
                    &shm,
                    offer.width,
                    offer.height,
                    offer.stride,
                    offer.format,
                    qh,
                ) {
                    Ok(buffer) => {
                        frame.copy(&buffer.buffer);
                        pending.copy_sent = true;
                        pending.buffer = Some(buffer);
                    }
                    Err(error) => {
                        pending.error = Some(error.to_string());
                        pending.complete = true;
                    }
                }
            }
            zwlr_screencopy_frame_v1::Event::Flags {
                flags: WEnum::Value(flags),
            } => {
                state.probe_y_invert = flags.contains(zwlr_screencopy_frame_v1::Flags::YInvert);
                if let Some(pending) = state.pending.as_mut() {
                    pending.y_invert = state.probe_y_invert;
                }
            }
            zwlr_screencopy_frame_v1::Event::Ready { .. } => {
                if let Some(pending) = state.pending.as_mut() {
                    pending.complete = true;
                    frame.destroy();
                }
            }
            zwlr_screencopy_frame_v1::Event::Failed => {
                state.fail_pending("compositor failed to copy the requested output");
                frame.destroy();
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_compositor::WlCompositor, ()> for CaptureState {
    fn event(
        _: &mut Self,
        _: &wl_compositor::WlCompositor,
        _: wl_compositor::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn converts_xrgb_with_padding_and_y_inversion() {
        let map = vec![
            3, 2, 1, 99, 0, 0, 0, 0, // source row 0
            6, 5, 4, 88, 0, 0, 0, 0, // source row 1
        ];
        let frame = convert_shm_pixels(&map, 1, 2, 8, wl_shm::Format::Xrgb8888, true).unwrap();
        assert_eq!(frame.size(), Size::new(1, 2));
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(0, 0)),
            Some([4, 5, 6, 255])
        );
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(0, 1)),
            Some([1, 2, 3, 255])
        );
    }

    #[test]
    fn converts_argb_alpha_without_reading_padding() {
        let map = vec![30, 20, 10, 77, 255, 255, 255, 255];
        let frame = convert_shm_pixels(&map, 1, 1, 8, wl_shm::Format::Argb8888, false).unwrap();
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(0, 0)),
            Some([10, 20, 30, 77])
        );
    }
}

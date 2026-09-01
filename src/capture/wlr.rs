use std::collections::HashMap;
use std::os::fd::AsFd;
use std::time::{Duration, Instant};

use memmap2::{MmapMut, MmapOptions};
use wayland_client::protocol::{
    wl_buffer, wl_compositor, wl_output, wl_registry, wl_shm, wl_shm_pool,
};
use wayland_client::{Connection, Dispatch, EventQueue, QueueHandle, WEnum};
use wayland_protocols_wlr::screencopy::v1::client::{
    zwlr_screencopy_frame_v1, zwlr_screencopy_manager_v1,
};

use crate::error::{Result, VshotError};
use crate::geometry::Size;
use crate::model::Frame;

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
    for destination_y in 0..height {
        let source_y = if y_invert {
            height - 1 - destination_y
        } else {
            destination_y
        };
        let source_row = source_y * stride;
        let destination_row = destination_y * width * 4;
        for x in 0..width {
            let source = source_row + x * 4;
            let destination = destination_row + x * 4;
            pixels[destination] = map[source + 2];
            pixels[destination + 1] = map[source + 1];
            pixels[destination + 2] = map[source];
            pixels[destination + 3] = match format {
                wl_shm::Format::Argb8888 => map[source + 3],
                wl_shm::Format::Xrgb8888 => 255,
                _ => unreachable!("unsupported format was rejected above"),
            };
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
    y_invert: bool,
    copy_sent: bool,
    complete: bool,
    error: Option<String>,
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
    outputs: HashMap<u32, wl_output::WlOutput>,
    output_names: HashMap<u32, String>,
    pending: Option<PendingCapture>,
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
        let frame = manager.capture_output(if cursor { 1 } else { 0 }, &output, &qh, ());
        self.state.pending = Some(PendingCapture {
            _frame: frame,
            buffer: None,
            y_invert: false,
            copy_sent: false,
            complete: false,
            error: None,
        });
        let _ = global_id;

        self.dispatch_until(Instant::now() + Duration::from_secs(10))?;
        let pending = self.state.pending.take().ok_or_else(|| {
            VshotError::WaylandProtocol("capture disappeared before completion".into())
        })?;
        if let Some(error) = pending.error {
            return Err(VshotError::WaylandProtocol(error));
        }
        let buffer = pending.buffer.ok_or_else(|| {
            VshotError::WaylandProtocol("screencopy completed without a buffer".into())
        })?;
        buffer.into_frame(pending.y_invert)
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
                let Some(shm) = state.shm.as_ref().cloned() else {
                    state.fail_pending("wl_shm disappeared during capture");
                    return;
                };
                let copy_immediately = state.manager_version < 3;
                let Some(pending) = state.pending.as_mut() else {
                    return;
                };
                if pending.buffer.is_some() {
                    pending.error = Some("screencopy sent more than one SHM buffer".into());
                    pending.complete = true;
                    return;
                }
                match CaptureBuffer::new(&shm, width, height, stride, format, qh) {
                    Ok(buffer) => {
                        if copy_immediately {
                            frame.copy(&buffer.buffer);
                            pending.copy_sent = true;
                        }
                        pending.buffer = Some(buffer);
                    }
                    Err(error) => {
                        pending.error = Some(error.to_string());
                        pending.complete = true;
                    }
                }
            }
            zwlr_screencopy_frame_v1::Event::BufferDone => {
                let Some(pending) = state.pending.as_mut() else {
                    return;
                };
                let Some(buffer) = pending.buffer.as_ref() else {
                    pending.error =
                        Some("screencopy sent buffer_done without an SHM buffer".into());
                    pending.complete = true;
                    return;
                };
                if !pending.copy_sent {
                    frame.copy(&buffer.buffer);
                    pending.copy_sent = true;
                }
            }
            zwlr_screencopy_frame_v1::Event::Flags {
                flags: WEnum::Value(flags),
            } => {
                if let Some(pending) = state.pending.as_mut() {
                    pending.y_invert = flags.contains(zwlr_screencopy_frame_v1::Flags::YInvert);
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

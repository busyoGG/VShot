use std::os::unix::io::AsFd;

use memmap2::{MmapMut, MmapOptions};
use wayland_client::protocol::{wl_buffer, wl_shm, wl_shm_pool, wl_surface};
use wayland_client::{Dispatch, QueueHandle};
use wayland_protocols_wlr::layer_shell::v1::client::zwlr_layer_surface_v1;

use crate::edit::EditPipeline;
use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect};
use crate::model::{Frame, ImageDocument};
use crate::wayland::input::{Annotation, EditorState, EditorTool};

#[derive(Clone, Copy, Debug)]
pub(crate) struct SurfaceUserData {
    pub(crate) output_id: u32,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct LayerSurfaceUserData {
    pub(crate) output_id: u32,
}

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct PoolUserData;

#[derive(Clone, Copy, Debug)]
pub(crate) struct BufferUserData {
    pub(crate) output_id: u32,
    pub(crate) slot: usize,
}

pub(crate) struct ShmSlot {
    pub(crate) _file: tempfile::NamedTempFile,
    pub(crate) map: MmapMut,
    dimmed: Option<Vec<u8>>,
    pub(crate) buffer: wl_buffer::WlBuffer,
    pub(crate) stride: usize,
    pub(crate) width: u32,
    pub(crate) height: u32,
    pub(crate) available: bool,
}

impl std::fmt::Debug for ShmSlot {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("ShmSlot")
            .field("width", &self.width)
            .field("height", &self.height)
            .field("available", &self.available)
            .finish_non_exhaustive()
    }
}

impl ShmSlot {
    pub(crate) fn new<State>(
        shm: &wl_shm::WlShm,
        width: u32,
        height: u32,
        output_id: u32,
        slot: usize,
        format: wl_shm::Format,
        qh: &QueueHandle<State>,
    ) -> Result<Self>
    where
        State: Dispatch<wl_shm_pool::WlShmPool, PoolUserData>
            + Dispatch<wl_buffer::WlBuffer, BufferUserData>
            + 'static,
    {
        let stride = usize::try_from(width)
            .ok()
            .and_then(|width| width.checked_mul(4))
            .ok_or_else(|| VshotError::WaylandProtocol("SHM stride overflows".into()))?;
        let bytes = stride
            .checked_mul(
                usize::try_from(height)
                    .map_err(|_| VshotError::WaylandProtocol("SHM height is too large".into()))?,
            )
            .ok_or_else(|| VshotError::WaylandProtocol("SHM buffer size overflows".into()))?;
        let pool_size = i32::try_from(bytes).map_err(|_| {
            VshotError::WaylandProtocol("SHM buffer exceeds Wayland's signed size limit".into())
        })?;
        let file = tempfile::Builder::new()
            .prefix("vshot-")
            .tempfile_in("/dev/shm")
            .map_err(|source| {
                VshotError::WaylandProtocol(format!("failed to create SHM file: {source}"))
            })?;
        file.as_file()
            .set_len(
                u64::try_from(bytes).map_err(|_| {
                    VshotError::WaylandProtocol("SHM buffer size is invalid".into())
                })?,
            )
            .map_err(|source| {
                VshotError::WaylandProtocol(format!("failed to size SHM file: {source}"))
            })?;
        let mut map =
            unsafe { MmapOptions::new().len(bytes).map_mut(file.as_file()) }.map_err(|source| {
                VshotError::WaylandProtocol(format!("failed to map SHM buffer: {source}"))
            })?;
        map.fill(0);
        let pool = shm.create_pool(file.as_fd(), pool_size, qh, PoolUserData);
        let buffer = pool.create_buffer(
            0,
            i32::try_from(width)
                .map_err(|_| VshotError::WaylandProtocol("SHM width is too large".into()))?,
            i32::try_from(height)
                .map_err(|_| VshotError::WaylandProtocol("SHM height is too large".into()))?,
            i32::try_from(stride)
                .map_err(|_| VshotError::WaylandProtocol("SHM stride is too large".into()))?,
            format,
            qh,
            BufferUserData { output_id, slot },
        );
        pool.destroy();
        Ok(Self {
            _file: file,
            map,
            dimmed: None,
            buffer,
            stride,
            width,
            height,
            available: true,
        })
    }

    pub(crate) fn render(
        &mut self,
        frame: &Frame,
        selection: Option<Rect>,
        output_geometry: Rect,
        scale: u32,
    ) -> Result<()> {
        self.render_editor(frame, selection, output_geometry, scale, None, None)
    }

    pub(crate) fn render_editor(
        &mut self,
        frame: &Frame,
        selection: Option<Rect>,
        output_geometry: Rect,
        scale: u32,
        editor: Option<&EditorState>,
        _scene_bounds: Option<Rect>,
    ) -> Result<()> {
        if frame.size().width != self.width || frame.size().height != self.height {
            return Err(VshotError::WaylandProtocol(format!(
                "frozen frame is {}x{}, but the layer surface buffer is {}x{}",
                frame.size().width,
                frame.size().height,
                self.width,
                self.height
            )));
        }
        let map_len = checked_buffer_len(self.stride, self.width, self.height)?;
        if self.map.len() < map_len {
            return Err(VshotError::WaylandProtocol(
                "SHM map is smaller than its declared dimensions".into(),
            ));
        }
        if let Some(editor) = editor {
            let mut rendered = frame.clone();
            render_annotations(&mut rendered, editor, output_geometry, scale)?;
            rendered.copy_rgba_to_bgra(&mut self.map[..map_len], self.stride, Point::new(0, 0))?;
        } else if selection.is_none() {
            frame.copy_rgba_to_bgra(&mut self.map[..map_len], self.stride, Point::new(0, 0))?;
        }
        let Some(selection) = selection else {
            if let Some(editor) = editor {
                render_toolbar(&mut self.map, self.stride, output_geometry, scale, editor)?;
            }
            return Ok(());
        };
        if scale == 0 {
            return Err(VshotError::WaylandProtocol(
                "output scale cannot be zero".into(),
            ));
        }
        let selection_right = selection.right()?;
        let selection_bottom = selection.bottom()?;
        if editor.is_none() {
            if self
                .dimmed
                .as_ref()
                .is_none_or(|dimmed| dimmed.len() != map_len)
            {
                let mut dimmed = vec![0; map_len];
                frame.copy_rgba_to_bgra(&mut dimmed, self.stride, Point::new(0, 0))?;
                dim_bgra(&mut dimmed, self.stride, self.width, self.height)?;
                self.dimmed = Some(dimmed);
            }
            let dimmed = self.dimmed.as_ref().ok_or_else(|| {
                VshotError::WaylandProtocol("failed to initialize dimmed SHM cache".into())
            })?;
            self.map[..map_len].copy_from_slice(dimmed);
        } else {
            for pixel_y in 0..self.height {
                let logical_y = output_geometry.top() + (pixel_y / scale) as i32;
                for pixel_x in 0..self.width {
                    let logical_x = output_geometry.left() + (pixel_x / scale) as i32;
                    let inside = logical_x >= selection.left()
                        && logical_x < selection_right
                        && logical_y >= selection.top()
                        && logical_y < selection_bottom;
                    let index = pixel_y as usize * self.stride + pixel_x as usize * 4;
                    if !inside {
                        self.map[index] /= 2;
                        self.map[index + 1] /= 2;
                        self.map[index + 2] /= 2;
                    }
                }
            }
        }
        if let Some(intersection) = selection.intersection(output_geometry) {
            let left =
                u32::try_from(i64::from(intersection.left()) - i64::from(output_geometry.left()))
                    .map_err(|_| VshotError::WaylandProtocol("selection x is out of range".into()))?
                    .checked_mul(scale)
                    .ok_or_else(|| VshotError::WaylandProtocol("selection x overflows".into()))?;
            let top =
                u32::try_from(i64::from(intersection.top()) - i64::from(output_geometry.top()))
                    .map_err(|_| VshotError::WaylandProtocol("selection y is out of range".into()))?
                    .checked_mul(scale)
                    .ok_or_else(|| VshotError::WaylandProtocol("selection y overflows".into()))?;
            let right =
                u32::try_from(i64::from(intersection.right()?) - i64::from(output_geometry.left()))
                    .map_err(|_| {
                        VshotError::WaylandProtocol("selection right edge is out of range".into())
                    })?
                    .checked_mul(scale)
                    .ok_or_else(|| {
                        VshotError::WaylandProtocol("selection right edge overflows".into())
                    })?
                    .min(self.width);
            let bottom =
                u32::try_from(i64::from(intersection.bottom()?) - i64::from(output_geometry.top()))
                    .map_err(|_| {
                        VshotError::WaylandProtocol("selection bottom edge is out of range".into())
                    })?
                    .checked_mul(scale)
                    .ok_or_else(|| {
                        VshotError::WaylandProtocol("selection bottom edge overflows".into())
                    })?
                    .min(self.height);
            if editor.is_none() && left < right && top < bottom {
                let source_rect = Rect::new(
                    i32::try_from(left).map_err(|_| {
                        VshotError::WaylandProtocol("selection x is out of range".into())
                    })?,
                    i32::try_from(top).map_err(|_| {
                        VshotError::WaylandProtocol("selection y is out of range".into())
                    })?,
                    right - left,
                    bottom - top,
                );
                copy_rgba_rect_to_bgra(
                    frame.pixels(),
                    frame.size().width,
                    frame.size().height,
                    &mut self.map[..map_len],
                    self.stride,
                    source_rect,
                    source_rect.origin,
                )?;
            }
            let thickness = scale.clamp(1, 4);
            for pixel_y in top..bottom {
                for pixel_x in left..right {
                    if pixel_y < top + thickness
                        || pixel_y + thickness >= bottom
                        || pixel_x < left + thickness
                        || pixel_x + thickness >= right
                    {
                        let index = pixel_y as usize * self.stride + pixel_x as usize * 4;
                        self.map[index] = 0;
                        self.map[index + 1] = 220;
                        self.map[index + 2] = 255;
                        self.map[index + 3] = 255;
                    }
                }
            }
        }
        if let Some(editor) = editor {
            render_handles(
                &mut self.map,
                self.stride,
                self.width,
                output_geometry,
                scale,
                editor,
            )?;
            render_toolbar(&mut self.map, self.stride, output_geometry, scale, editor)?;
        }
        Ok(())
    }
}

fn checked_buffer_len(stride: usize, width: u32, height: u32) -> Result<usize> {
    let row_bytes = usize::try_from(width)
        .ok()
        .and_then(|width| width.checked_mul(4))
        .ok_or_else(|| VshotError::InvalidGeometry("SHM row is too large".into()))?;
    if stride < row_bytes {
        return Err(VshotError::InvalidGeometry(
            "SHM stride is smaller than the pixel row".into(),
        ));
    }
    stride
        .checked_mul(
            usize::try_from(height)
                .map_err(|_| VshotError::InvalidGeometry("SHM height is too large".into()))?,
        )
        .ok_or_else(|| VshotError::InvalidGeometry("SHM buffer is too large".into()))
}

fn dim_bgra(map: &mut [u8], stride: usize, width: u32, height: u32) -> Result<()> {
    let width = usize::try_from(width)
        .map_err(|_| VshotError::InvalidGeometry("BGRA width is too large".into()))?;
    let row_bytes = width
        .checked_mul(4)
        .ok_or_else(|| VshotError::InvalidGeometry("BGRA row is too large".into()))?;
    let height = usize::try_from(height)
        .map_err(|_| VshotError::InvalidGeometry("BGRA height is too large".into()))?;
    let required = stride
        .checked_mul(height)
        .ok_or_else(|| VshotError::InvalidGeometry("BGRA buffer is too large".into()))?;
    if stride < row_bytes || required > map.len() {
        return Err(VshotError::InvalidGeometry(
            "BGRA buffer is too small for its dimensions".into(),
        ));
    }
    for y in 0..height {
        let row = y * stride;
        for x in 0..width {
            let index = row + x * 4;
            map[index] /= 2;
            map[index + 1] /= 2;
            map[index + 2] /= 2;
        }
    }
    Ok(())
}

fn copy_rgba_rect_to_bgra(
    source: &[u8],
    source_width: u32,
    source_height: u32,
    destination: &mut [u8],
    destination_stride: usize,
    source_rect: Rect,
    destination_origin: Point,
) -> Result<()> {
    if source_rect.is_empty() {
        return Err(VshotError::InvalidGeometry(
            "source rectangle must not be empty".into(),
        ));
    }
    if source_rect.left() < 0 || source_rect.top() < 0 {
        return Err(VshotError::InvalidGeometry(
            "source rectangle origin must be non-negative".into(),
        ));
    }
    if destination_origin.x < 0 || destination_origin.y < 0 {
        return Err(VshotError::InvalidGeometry(
            "destination origin must be non-negative".into(),
        ));
    }
    let source_right = usize::try_from(source_rect.right()?)
        .map_err(|_| VshotError::InvalidGeometry("source rectangle is out of range".into()))?;
    let source_bottom = usize::try_from(source_rect.bottom()?)
        .map_err(|_| VshotError::InvalidGeometry("source rectangle is out of range".into()))?;
    let source_x = usize::try_from(source_rect.left())
        .map_err(|_| VshotError::InvalidGeometry("source rectangle is out of range".into()))?;
    let source_y = usize::try_from(source_rect.top())
        .map_err(|_| VshotError::InvalidGeometry("source rectangle is out of range".into()))?;
    let source_width = usize::try_from(source_width)
        .map_err(|_| VshotError::InvalidGeometry("source width is too large".into()))?;
    let source_height = usize::try_from(source_height)
        .map_err(|_| VshotError::InvalidGeometry("source height is too large".into()))?;
    let source_row_bytes = source_width
        .checked_mul(4)
        .ok_or_else(|| VshotError::InvalidGeometry("source row is too large".into()))?;
    let source_len = source_row_bytes
        .checked_mul(source_height)
        .ok_or_else(|| VshotError::InvalidGeometry("source buffer is too large".into()))?;
    if source_right > source_width || source_bottom > source_height || source.len() < source_len {
        return Err(VshotError::InvalidGeometry(
            "source rectangle is outside the source buffer".into(),
        ));
    }
    let width = usize::try_from(source_rect.size.width)
        .map_err(|_| VshotError::InvalidGeometry("source rectangle is too large".into()))?;
    let height = usize::try_from(source_rect.size.height)
        .map_err(|_| VshotError::InvalidGeometry("source rectangle is too large".into()))?;
    let row_bytes = width
        .checked_mul(4)
        .ok_or_else(|| VshotError::InvalidGeometry("copy row is too large".into()))?;
    let destination_x = usize::try_from(destination_origin.x)
        .map_err(|_| VshotError::InvalidGeometry("destination origin is out of range".into()))?;
    let destination_y = usize::try_from(destination_origin.y)
        .map_err(|_| VshotError::InvalidGeometry("destination origin is out of range".into()))?;
    let destination_last_row = destination_y
        .checked_add(height.saturating_sub(1))
        .ok_or_else(|| VshotError::InvalidGeometry("destination is too large".into()))?;
    let required = destination_last_row
        .checked_mul(destination_stride)
        .and_then(|row| row.checked_add(destination_x.checked_mul(4)?))
        .and_then(|row| row.checked_add(row_bytes))
        .ok_or_else(|| VshotError::InvalidGeometry("destination is too large".into()))?;
    if destination_stride < row_bytes || required > destination.len() {
        return Err(VshotError::InvalidGeometry(
            "destination buffer is too small".into(),
        ));
    }
    for row in 0..height {
        let source_row = (source_y + row) * source_row_bytes + source_x * 4;
        let destination_row = (destination_y + row) * destination_stride + destination_x * 4;
        for column in 0..width {
            let source_index = source_row + column * 4;
            let destination_index = destination_row + column * 4;
            destination[destination_index] = source[source_index + 2];
            destination[destination_index + 1] = source[source_index + 1];
            destination[destination_index + 2] = source[source_index];
            destination[destination_index + 3] = source[source_index + 3];
        }
    }
    Ok(())
}

fn map_point(point: Point, output: Rect, scale: u32) -> Result<Point> {
    if scale == 0 {
        return Err(VshotError::WaylandProtocol(
            "output scale cannot be zero".into(),
        ));
    }
    let x = (i64::from(point.x) - i64::from(output.left()))
        .checked_mul(i64::from(scale))
        .ok_or_else(|| VshotError::WaylandProtocol("annotation x overflows".into()))?;
    let y = (i64::from(point.y) - i64::from(output.top()))
        .checked_mul(i64::from(scale))
        .ok_or_else(|| VshotError::WaylandProtocol("annotation y overflows".into()))?;
    Ok(Point::new(
        i32::try_from(x)
            .map_err(|_| VshotError::WaylandProtocol("annotation x is out of range".into()))?,
        i32::try_from(y)
            .map_err(|_| VshotError::WaylandProtocol("annotation y is out of range".into()))?,
    ))
}

fn map_rect(rect: Rect, output: Rect, scale: u32) -> Result<Rect> {
    let origin = map_point(rect.origin, output, scale)?;
    let width = rect
        .size
        .width
        .checked_mul(scale)
        .ok_or_else(|| VshotError::WaylandProtocol("annotation width overflows".into()))?;
    let height = rect
        .size
        .height
        .checked_mul(scale)
        .ok_or_else(|| VshotError::WaylandProtocol("annotation height overflows".into()))?;
    Ok(Rect::new(origin.x, origin.y, width, height))
}

fn render_annotations(
    frame: &mut Frame,
    editor: &EditorState,
    output: Rect,
    scale: u32,
) -> Result<()> {
    let mut pipeline = EditPipeline::new();
    for annotation in editor.annotations() {
        match annotation {
            Annotation::Shape { tool, rect } => {
                let rect = map_rect(*rect, output, scale)?;
                match tool {
                    EditorTool::Rectangle => {
                        pipeline =
                            pipeline.rectangle_stroke(rect, [255, 70, 70, 255], scale.clamp(1, 4));
                    }
                    EditorTool::Ellipse => {
                        let center = Point::new(
                            rect.left().saturating_add((rect.size.width / 2) as i32),
                            rect.top().saturating_add((rect.size.height / 2) as i32),
                        );
                        let radius = rect.size.width.min(rect.size.height) / 2;
                        if radius > 0 {
                            pipeline = pipeline.circle_stroke(
                                center,
                                radius,
                                [255, 70, 70, 255],
                                scale.clamp(1, 4),
                            );
                        }
                    }
                    _ => {}
                }
            }
            Annotation::Stroke { tool, points } => {
                let points = points
                    .iter()
                    .map(|point| map_point(*point, output, scale))
                    .collect::<Result<Vec<_>>>()?;
                let width = scale.clamp(1, 4);
                match tool {
                    EditorTool::Arrow if points.len() >= 2 => {
                        pipeline = pipeline.arrow(
                            points[0],
                            *points.last().unwrap(),
                            [255, 70, 70, 255],
                            width,
                        );
                    }
                    EditorTool::Mosaic | EditorTool::Blur => {
                        if let Some(bounds) = annotation.bounds() {
                            pipeline = pipeline.mosaic(
                                map_rect(bounds, output, scale)?,
                                12_u32.saturating_mul(scale).max(1),
                            );
                        }
                    }
                    EditorTool::Pen | EditorTool::Draw | EditorTool::Line => {
                        pipeline = pipeline.freehand(points, [255, 70, 70, 255], width);
                    }
                    _ => {}
                }
            }
            Annotation::Text {
                origin,
                text,
                scale: text_scale,
            } => {
                pipeline = pipeline.text(
                    map_point(*origin, output, scale)?,
                    text,
                    [255, 255, 255, 255],
                    text_scale.saturating_mul(scale).max(1),
                );
            }
        }
    }
    let document = pipeline.apply(ImageDocument::new(frame.clone()))?;
    *frame = document.into_frame();
    Ok(())
}

fn render_handles(
    map: &mut [u8],
    stride: usize,
    buffer_width: u32,
    output: Rect,
    scale: u32,
    editor: &EditorState,
) -> Result<()> {
    let Some(selection) = editor.selection() else {
        return Ok(());
    };
    let Some(intersection) = selection.intersection(output) else {
        return Ok(());
    };
    let left = u32::try_from(i64::from(intersection.left()) - i64::from(output.left()))
        .map_err(|_| VshotError::WaylandProtocol("handle x out of range".into()))?
        .saturating_mul(scale);
    let top = u32::try_from(i64::from(intersection.top()) - i64::from(output.top()))
        .map_err(|_| VshotError::WaylandProtocol("handle y out of range".into()))?
        .saturating_mul(scale);
    let right = u32::try_from(i64::from(intersection.right()?) - i64::from(output.left()))
        .map_err(|_| VshotError::WaylandProtocol("handle right out of range".into()))?
        .saturating_mul(scale)
        .min(buffer_width);
    let bottom = u32::try_from(i64::from(intersection.bottom()?) - i64::from(output.top()))
        .map_err(|_| VshotError::WaylandProtocol("handle bottom out of range".into()))?
        .saturating_mul(scale);
    let size = (scale * 3).clamp(4, 12);
    for &(x, y) in &[
        (left, top),
        (right.saturating_sub(size), top),
        (left, bottom.saturating_sub(size)),
        (right.saturating_sub(size), bottom.saturating_sub(size)),
    ] {
        draw_bgra_rect(map, stride, x, y, size, size, [255, 255, 255, 255]);
    }
    Ok(())
}

fn render_toolbar(
    map: &mut [u8],
    stride: usize,
    output: Rect,
    scale: u32,
    editor: &EditorState,
) -> Result<()> {
    for item in editor.toolbar().items() {
        let Some(rect) = item.rect.intersection(output) else {
            continue;
        };
        let local = map_rect(rect, output, scale)?;
        let color = if item.tool == editor.tool() {
            [35, 150, 220, 255]
        } else {
            [35, 35, 45, 240]
        };
        draw_bgra_rect(
            map,
            stride,
            u32::try_from(local.left()).unwrap_or(0),
            u32::try_from(local.top()).unwrap_or(0),
            local.size.width,
            local.size.height,
            color,
        );
        draw_icon(map, stride, local, item.tool, scale);
    }
    Ok(())
}

fn draw_bgra_rect(
    map: &mut [u8],
    stride: usize,
    x: u32,
    y: u32,
    width: u32,
    height: u32,
    color: [u8; 4],
) {
    let max_height = map.len() / stride;
    for py in y as usize..(y.saturating_add(height) as usize).min(max_height) {
        for px in x as usize..(x.saturating_add(width) as usize).min(stride / 4) {
            let index = py * stride + px * 4;
            if index + 3 < map.len() {
                map[index..index + 4].copy_from_slice(&[color[2], color[1], color[0], color[3]]);
            }
        }
    }
}

fn draw_icon(map: &mut [u8], stride: usize, rect: Rect, tool: EditorTool, scale: u32) {
    let color = [255, 255, 255, 255];
    let x = u32::try_from(rect.left().max(0)).unwrap_or(0);
    let y = u32::try_from(rect.top().max(0)).unwrap_or(0);
    let w = rect.size.width;
    let h = rect.size.height;
    let pad = scale.clamp(1, 4);
    match tool {
        EditorTool::Rectangle => {
            draw_bgra_rect(map, stride, x + w / 4, y + h / 4, w / 2, h / 2, color)
        }
        EditorTool::Ellipse => draw_bgra_rect(map, stride, x + w / 4, y + h / 4, w / 2, pad, color),
        EditorTool::Arrow | EditorTool::Line => {
            draw_bgra_rect(map, stride, x + w / 5, y + h / 2, w * 3 / 5, pad, color)
        }
        EditorTool::Pen | EditorTool::Draw => {
            draw_bgra_rect(map, stride, x + w / 4, y + h / 3, w / 2, pad, color)
        }
        EditorTool::Text => draw_bgra_rect(map, stride, x + w / 3, y + h / 4, pad, h / 2, color),
        EditorTool::Mosaic | EditorTool::Blur => draw_bgra_rect(
            map,
            stride,
            x + w / 4,
            y + h / 4,
            w / 2,
            h / 2,
            [170, 170, 170, 255],
        ),
        _ => {}
    }
}

impl Drop for ShmSlot {
    fn drop(&mut self) {
        self.buffer.destroy();
    }
}

pub(crate) struct OverlaySurface {
    pub(crate) output_id: u32,
    pub(crate) _layer_surface: zwlr_layer_surface_v1::ZwlrLayerSurfaceV1,
    pub(crate) surface: wl_surface::WlSurface,
    pub(crate) configured: bool,
    pub(crate) closed: bool,
    pub(crate) width: u32,
    pub(crate) height: u32,
    pub(crate) slots: Vec<ShmSlot>,
    pub(crate) pending_redraw: bool,
}

impl std::fmt::Debug for OverlaySurface {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("OverlaySurface")
            .field("output_id", &self.output_id)
            .field("configured", &self.configured)
            .field("closed", &self.closed)
            .field("width", &self.width)
            .field("height", &self.height)
            .field("slots", &self.slots)
            .finish_non_exhaustive()
    }
}

impl OverlaySurface {
    pub(crate) fn attach_available(&mut self) -> Option<usize> {
        self.slots.iter().position(|slot| slot.available)
    }

    pub(crate) fn commit_slot(&mut self, slot: usize) {
        self.slots[slot].available = false;
        self.surface.attach(Some(&self.slots[slot].buffer), 0, 0);
        self.surface.damage(0, 0, i32::MAX, i32::MAX);
        self.surface.commit();
    }
}

impl Drop for OverlaySurface {
    fn drop(&mut self) {
        // Unmap the surface before destroying its role and base surface.
        self.surface.attach(None, 0, 0);
        self.surface.commit();
        self._layer_surface.destroy();
        self.surface.destroy();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::geometry::Size;

    #[test]
    fn dim_bgra_preserves_alpha_and_row_padding() {
        let mut map = vec![
            10, 20, 30, 40, 101, 102, 103, 104, 77, 78, 79, 80, 20, 21, 22, 23, 201, 202, 203, 204,
            88, 89, 90, 91,
        ];

        dim_bgra(&mut map, 12, 2, 2).unwrap();

        assert_eq!(
            map,
            vec![
                5, 10, 15, 40, 50, 51, 51, 104, 77, 78, 79, 80, 10, 10, 11, 23, 100, 101, 101, 204,
                88, 89, 90, 91,
            ]
        );
    }

    #[test]
    fn copy_rgba_rect_to_bgra_copies_only_requested_pixels() {
        let frame = Frame::new(
            Size::new(3, 2),
            vec![
                1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
                24,
            ],
        )
        .unwrap();
        let mut destination = vec![0xee; 32];

        copy_rgba_rect_to_bgra(
            frame.pixels(),
            frame.size().width,
            frame.size().height,
            &mut destination,
            16,
            Rect::new(1, 0, 2, 2),
            Point::new(0, 0),
        )
        .unwrap();

        assert_eq!(&destination[0..8], &[7, 6, 5, 8, 11, 10, 9, 12]);
        assert_eq!(&destination[16..24], &[19, 18, 17, 20, 23, 22, 21, 24]);
        assert_eq!(destination[8..16], [0xee; 8]);
        assert_eq!(destination[24..32], [0xee; 8]);
    }

    #[test]
    fn pixel_helpers_reject_short_buffers() {
        assert!(dim_bgra(&mut [0; 7], 8, 2, 1).is_err());

        let frame = Frame::solid(Size::new(2, 1), [1, 2, 3, 4]).unwrap();
        assert!(copy_rgba_rect_to_bgra(
            frame.pixels(),
            frame.size().width,
            frame.size().height,
            &mut [0; 7],
            8,
            Rect::new(0, 0, 2, 1),
            Point::new(0, 0),
        )
        .is_err());
    }
}

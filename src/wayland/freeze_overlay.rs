use std::os::unix::io::AsFd;

use memmap2::{MmapMut, MmapOptions};
use wayland_client::protocol::{wl_buffer, wl_shm, wl_shm_pool, wl_surface};
use wayland_client::{Dispatch, QueueHandle};
use wayland_protocols_wlr::layer_shell::v1::client::zwlr_layer_surface_v1;

use crate::edit::EditPipeline;
use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect, Size};
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

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum BufferToken {
    Parent(u64),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct BufferUserData {
    pub(crate) output_id: u32,
    pub(crate) token: BufferToken,
}

pub(crate) fn release_buffer_if_current(
    current: BufferToken,
    released: BufferToken,
    available: &mut bool,
) -> bool {
    if current != released {
        return false;
    }
    *available = true;
    true
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ContentMode {
    Empty,
    Raw,
    Dimmed,
    EditorDimmed,
}

pub(crate) struct ShmSlot {
    pub(crate) _file: tempfile::NamedTempFile,
    pub(crate) map: MmapMut,
    dimmed: Option<Vec<u8>>,
    editor_annotations: Option<Vec<Annotation>>,
    editor_bgra: Option<Vec<u8>>,
    editor_dimmed: Option<Vec<u8>>,
    content_mode: ContentMode,
    last_selection: Option<Rect>,
    pub(crate) token: BufferToken,
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
        userdata: BufferUserData,
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
            userdata,
        );
        pool.destroy();
        Ok(Self {
            _file: file,
            map,
            dimmed: None,
            editor_annotations: None,
            editor_bgra: None,
            editor_dimmed: None,
            content_mode: ContentMode::Empty,
            last_selection: None,
            token: userdata.token,
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

    fn validate_frame(&self, frame: &Frame) -> Result<usize> {
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
        Ok(map_len)
    }

    fn ensure_dimmed_cache(&mut self, frame: &Frame, map_len: usize) -> Result<()> {
        if self
            .dimmed
            .as_ref()
            .is_none_or(|dimmed| dimmed.len() != map_len)
        {
            self.dimmed = Some(build_dimmed_bgra(
                frame,
                self.stride,
                self.width,
                self.height,
            )?);
        }
        Ok(())
    }

    fn ensure_editor_cache(
        &mut self,
        frame: &Frame,
        editor: &EditorState,
        output_geometry: Rect,
        scale: u32,
        map_len: usize,
    ) -> Result<bool> {
        let annotations_changed = self.editor_annotations.as_deref() != Some(editor.annotations());
        let editor_bgra_missing = self
            .editor_bgra
            .as_ref()
            .is_none_or(|bgra| bgra.len() != map_len);
        let editor_dimmed_missing = self
            .editor_dimmed
            .as_ref()
            .is_none_or(|dimmed| dimmed.len() != map_len);
        let cache_changed = annotations_changed || editor_bgra_missing || editor_dimmed_missing;
        if annotations_changed || editor_bgra_missing {
            let mut rendered = frame.clone();
            render_annotations(&mut rendered, editor, output_geometry, scale)?;
            let mut bgra = vec![0; map_len];
            rendered.copy_rgba_to_bgra(&mut bgra, self.stride, Point::new(0, 0))?;
            self.editor_annotations = Some(editor.annotations().to_vec());
            self.editor_bgra = Some(bgra);
            self.editor_dimmed = None;
        }
        if self
            .editor_dimmed
            .as_ref()
            .is_none_or(|dimmed| dimmed.len() != map_len)
        {
            let mut dimmed = self
                .editor_bgra
                .as_ref()
                .ok_or_else(|| {
                    VshotError::WaylandProtocol("failed to initialize editor BGRA cache".into())
                })?
                .clone();
            dim_bgra(&mut dimmed, self.stride, self.width, self.height)?;
            self.editor_dimmed = Some(dimmed);
        }
        Ok(cache_changed)
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
        let map_len = self.validate_frame(frame)?;
        if scale == 0 {
            return Err(VshotError::WaylandProtocol(
                "output scale cannot be zero".into(),
            ));
        }
        let selection = match editor {
            Some(editor) => editor.selection(),
            None => selection,
        };

        if let Some(editor) = editor {
            if self.ensure_editor_cache(frame, editor, output_geometry, scale, map_len)? {
                self.content_mode = ContentMode::Empty;
                self.last_selection = None;
            }
            if self.content_mode != ContentMode::EditorDimmed {
                let dimmed = self.editor_dimmed.as_deref().ok_or_else(|| {
                    VshotError::WaylandProtocol("failed to initialize editor dimmed cache".into())
                })?;
                self.map[..map_len].copy_from_slice(dimmed);
                self.content_mode = ContentMode::EditorDimmed;
                self.last_selection = None;
            }
        } else if selection.is_some() {
            self.ensure_dimmed_cache(frame, map_len)?;
            if self.content_mode != ContentMode::Dimmed {
                let dimmed = self.dimmed.as_deref().ok_or_else(|| {
                    VshotError::WaylandProtocol("failed to initialize dimmed SHM cache".into())
                })?;
                self.map[..map_len].copy_from_slice(dimmed);
                self.content_mode = ContentMode::Dimmed;
                self.last_selection = None;
            }
        } else if self.content_mode != ContentMode::Raw {
            frame.copy_rgba_to_bgra(&mut self.map[..map_len], self.stride, Point::new(0, 0))?;
            self.content_mode = ContentMode::Raw;
            self.last_selection = None;
        }

        let editor_dirty = if editor.is_some() {
            selection_dirty_rect(self.last_selection, selection, scale)?
        } else {
            None
        };
        let restore_rects = if let Some(dirty) = editor_dirty {
            vec![dirty]
        } else {
            selection_restore_rects(self.last_selection, selection)?
        };
        if !restore_rects.is_empty() {
            let source = if editor.is_some() {
                self.editor_dimmed.as_deref().ok_or_else(|| {
                    VshotError::WaylandProtocol("failed to initialize editor dimmed cache".into())
                })?
            } else {
                self.dimmed.as_deref().ok_or_else(|| {
                    VshotError::WaylandProtocol("failed to initialize dimmed SHM cache".into())
                })?
            };
            for dirty in restore_rects {
                restore_bgra_logical_rect(
                    source,
                    &mut self.map[..map_len],
                    self.stride,
                    Size::new(self.width, self.height),
                    dirty,
                    output_geometry,
                    scale,
                )?;
            }
        }

        if editor.is_some() {
            if let Some(editor) = editor {
                if let Some(toolbar) = toolbar_bounds(editor)? {
                    restore_bgra_logical_rect(
                        self.editor_dimmed.as_deref().ok_or_else(|| {
                            VshotError::WaylandProtocol(
                                "failed to initialize editor dimmed cache".into(),
                            )
                        })?,
                        &mut self.map[..map_len],
                        self.stride,
                        Size::new(self.width, self.height),
                        toolbar,
                        output_geometry,
                        scale,
                    )?;
                }
            }
        } else if selection.is_none() {
            self.last_selection = None;
        }

        if let Some(selection) = selection {
            let copy_rects = if editor.is_some() {
                selection_copy_rect(editor_dirty, Some(selection))
                    .into_iter()
                    .collect()
            } else {
                selection_copy_rects(self.last_selection, Some(selection))?
            };
            for copy_selection in copy_rects {
                if let Some(mapped_copy) = logical_rect_to_output_pixels(
                    copy_selection,
                    output_geometry,
                    scale,
                    Size::new(self.width, self.height),
                )? {
                    let pixel_rect = mapped_copy;
                    if let Some(editor_bgra) = editor.and(self.editor_bgra.as_deref()) {
                        copy_bgra_rect(
                            editor_bgra,
                            self.stride,
                            Size::new(self.width, self.height),
                            &mut self.map[..map_len],
                            self.stride,
                            pixel_rect,
                            pixel_rect.origin,
                        )?;
                    } else {
                        copy_rgba_rect_to_bgra(
                            frame.pixels(),
                            frame.size().width,
                            frame.size().height,
                            &mut self.map[..map_len],
                            self.stride,
                            pixel_rect,
                            pixel_rect.origin,
                        )?;
                    }
                }
            }
            if let Some(mapped) = map_selection_to_output_pixels(
                selection,
                output_geometry,
                scale,
                Size::new(self.width, self.height),
            )? {
                draw_selection_border(&mut self.map, self.stride, mapped, scale)?;
            }
        }
        if let Some(editor) = editor {
            render_handles(
                &mut self.map,
                self.stride,
                self.width,
                self.height,
                output_geometry,
                scale,
                editor,
            )?;
            render_toolbar(
                &mut self.map,
                self.stride,
                self.width,
                self.height,
                output_geometry,
                scale,
                editor,
            )?;
        }
        self.last_selection = selection;
        Ok(())
    }
}

fn rect_union(first: Option<Rect>, second: Option<Rect>) -> Result<Option<Rect>> {
    let mut result: Option<Rect> = None;
    for rect in [first, second]
        .into_iter()
        .flatten()
        .filter(|rect| !rect.is_empty())
    {
        result = Some(match result {
            None => rect,
            Some(current) => {
                let left = i64::from(current.left()).min(i64::from(rect.left()));
                let top = i64::from(current.top()).min(i64::from(rect.top()));
                let right = i64::from(current.right()?).max(i64::from(rect.right()?));
                let bottom = i64::from(current.bottom()?).max(i64::from(rect.bottom()?));
                Rect::new(
                    i32::try_from(left).map_err(|_| {
                        VshotError::InvalidGeometry("union left edge is out of range".into())
                    })?,
                    i32::try_from(top).map_err(|_| {
                        VshotError::InvalidGeometry("union top edge is out of range".into())
                    })?,
                    u32::try_from(right - left).map_err(|_| {
                        VshotError::InvalidGeometry("union width is out of range".into())
                    })?,
                    u32::try_from(bottom - top).map_err(|_| {
                        VshotError::InvalidGeometry("union height is out of range".into())
                    })?,
                )
            }
        });
    }
    Ok(result)
}

fn expand_rect(rect: Rect, padding: u32) -> Result<Rect> {
    let left = i64::from(rect.left()) - i64::from(padding);
    let top = i64::from(rect.top()) - i64::from(padding);
    let right = i64::from(rect.right()?) + i64::from(padding);
    let bottom = i64::from(rect.bottom()?) + i64::from(padding);
    Ok(Rect::new(
        i32::try_from(left).map_err(|_| {
            VshotError::InvalidGeometry("expanded left edge is out of range".into())
        })?,
        i32::try_from(top)
            .map_err(|_| VshotError::InvalidGeometry("expanded top edge is out of range".into()))?,
        u32::try_from(right - left)
            .map_err(|_| VshotError::InvalidGeometry("expanded width is out of range".into()))?,
        u32::try_from(bottom - top)
            .map_err(|_| VshotError::InvalidGeometry("expanded height is out of range".into()))?,
    ))
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct MappedSelection {
    rect: Rect,
    top: bool,
    right: bool,
    bottom: bool,
    left: bool,
    corners: [bool; 4],
}

fn rect_difference(outer: Rect, cut: Option<Rect>) -> Result<Vec<Rect>> {
    if outer.is_empty() {
        return Ok(Vec::new());
    }
    let Some(intersection) = cut.and_then(|cut| outer.intersection(cut)) else {
        return Ok(vec![outer]);
    };
    let outer_right = outer.right()?;
    let outer_bottom = outer.bottom()?;
    let intersection_right = intersection.right()?;
    let intersection_bottom = intersection.bottom()?;
    let mut result = Vec::with_capacity(4);

    if outer.top() < intersection.top() {
        result.push(Rect::new(
            outer.left(),
            outer.top(),
            outer.size.width,
            u32::try_from(i64::from(intersection.top()) - i64::from(outer.top())).map_err(
                |_| {
                    VshotError::InvalidGeometry(
                        "rectangle difference height is out of range".into(),
                    )
                },
            )?,
        ));
    }
    if intersection_bottom < outer_bottom {
        result.push(Rect::new(
            outer.left(),
            intersection_bottom,
            outer.size.width,
            u32::try_from(i64::from(outer_bottom) - i64::from(intersection_bottom)).map_err(
                |_| {
                    VshotError::InvalidGeometry(
                        "rectangle difference height is out of range".into(),
                    )
                },
            )?,
        ));
    }
    if outer.left() < intersection.left() {
        result.push(Rect::new(
            outer.left(),
            intersection.top(),
            u32::try_from(i64::from(intersection.left()) - i64::from(outer.left())).map_err(
                |_| {
                    VshotError::InvalidGeometry("rectangle difference width is out of range".into())
                },
            )?,
            intersection.size.height,
        ));
    }
    if intersection_right < outer_right {
        result.push(Rect::new(
            intersection_right,
            intersection.top(),
            u32::try_from(i64::from(outer_right) - i64::from(intersection_right)).map_err(
                |_| {
                    VshotError::InvalidGeometry("rectangle difference width is out of range".into())
                },
            )?,
            intersection.size.height,
        ));
    }
    Ok(result)
}

fn selection_border_rects(selection: Rect) -> Result<Vec<Rect>> {
    if selection.is_empty() {
        return Ok(Vec::new());
    }
    let right = selection.right()?;
    let bottom = selection.bottom()?;
    let mut result = Vec::with_capacity(4);
    result.push(Rect::new(
        selection.left(),
        selection.top(),
        selection.size.width,
        1,
    ));
    if selection.size.height > 1 {
        result.push(Rect::new(
            selection.left(),
            bottom - 1,
            selection.size.width,
            1,
        ));
    }
    if selection.size.width > 1 {
        result.push(Rect::new(
            selection.left(),
            selection.top(),
            1,
            selection.size.height,
        ));
        result.push(Rect::new(
            right - 1,
            selection.top(),
            1,
            selection.size.height,
        ));
    }
    Ok(result)
}

fn selection_restore_rects(previous: Option<Rect>, current: Option<Rect>) -> Result<Vec<Rect>> {
    let Some(previous) = previous else {
        return Ok(Vec::new());
    };
    let Some(current) = current else {
        return Ok(vec![previous]);
    };
    let mut result = rect_difference(previous, Some(current))?;
    result.extend(selection_border_rects(previous)?);
    Ok(result)
}

fn selection_copy_rects(previous: Option<Rect>, current: Option<Rect>) -> Result<Vec<Rect>> {
    let Some(current) = current else {
        return Ok(Vec::new());
    };
    let mut result = rect_difference(current, previous)?;
    if let Some(previous) = previous {
        for border in selection_border_rects(previous)? {
            if let Some(overlap) = border.intersection(current) {
                result.push(overlap);
            }
        }
    }
    Ok(result)
}

fn selection_copy_rect(dirty: Option<Rect>, selection: Option<Rect>) -> Option<Rect> {
    dirty.and_then(|dirty| selection.and_then(|selection| selection.intersection(dirty)))
}

fn selection_dirty_rect(
    first: Option<Rect>,
    second: Option<Rect>,
    scale: u32,
) -> Result<Option<Rect>> {
    let padding = handle_padding(scale)?;
    rect_union(
        first.map(|rect| expand_rect(rect, padding)).transpose()?,
        second.map(|rect| expand_rect(rect, padding)).transpose()?,
    )
}

fn handle_padding(scale: u32) -> Result<u32> {
    if scale == 0 {
        return Err(VshotError::WaylandProtocol(
            "output scale cannot be zero".into(),
        ));
    }
    let size = scale.saturating_mul(3).clamp(4, 12);
    Ok(size.saturating_add(scale - 1) / scale)
}

fn map_selection_to_output_pixels(
    selection: Rect,
    output: Rect,
    scale: u32,
    buffer_size: Size,
) -> Result<Option<MappedSelection>> {
    let Some(rect) = logical_rect_to_output_pixels(selection, output, scale, buffer_size)? else {
        return Ok(None);
    };
    let output_right = output.right()?;
    let output_bottom = output.bottom()?;
    let selection_right = selection.right()?;
    let selection_bottom = selection.bottom()?;
    let top_left = Point::new(selection.left(), selection.top());
    let top_right = Point::new(
        selection_right.checked_sub(1).ok_or_else(|| {
            VshotError::InvalidGeometry("selection top-right corner is out of range".into())
        })?,
        selection.top(),
    );
    let bottom_left = Point::new(
        selection.left(),
        selection_bottom.checked_sub(1).ok_or_else(|| {
            VshotError::InvalidGeometry("selection bottom-left corner is out of range".into())
        })?,
    );
    let bottom_right = Point::new(top_right.x, bottom_left.y);
    Ok(Some(MappedSelection {
        rect,
        top: selection.top() >= output.top() && selection.top() < output_bottom,
        right: selection_right > output.left() && selection_right <= output_right,
        bottom: selection_bottom > output.top() && selection_bottom <= output_bottom,
        left: selection.left() >= output.left() && selection.left() < output_right,
        corners: [
            output.contains(top_left),
            output.contains(top_right),
            output.contains(bottom_left),
            output.contains(bottom_right),
        ],
    }))
}

fn logical_rect_to_output_pixels(
    rect: Rect,
    output: Rect,
    scale: u32,
    buffer_size: Size,
) -> Result<Option<Rect>> {
    if scale == 0 {
        return Err(VshotError::WaylandProtocol(
            "output scale cannot be zero".into(),
        ));
    }
    let Some(intersection) = rect.intersection(output) else {
        return Ok(None);
    };
    let scale = i64::from(scale);
    let left = (i64::from(intersection.left()) - i64::from(output.left()))
        .checked_mul(scale)
        .ok_or_else(|| VshotError::WaylandProtocol("output rectangle x overflows".into()))?;
    let top = (i64::from(intersection.top()) - i64::from(output.top()))
        .checked_mul(scale)
        .ok_or_else(|| VshotError::WaylandProtocol("output rectangle y overflows".into()))?;
    let right = left
        .checked_add(
            i64::from(intersection.size.width)
                .checked_mul(scale)
                .ok_or_else(|| {
                    VshotError::WaylandProtocol("output rectangle width overflows".into())
                })?,
        )
        .ok_or_else(|| VshotError::WaylandProtocol("output rectangle right overflows".into()))?;
    let bottom = top
        .checked_add(
            i64::from(intersection.size.height)
                .checked_mul(scale)
                .ok_or_else(|| {
                    VshotError::WaylandProtocol("output rectangle height overflows".into())
                })?,
        )
        .ok_or_else(|| VshotError::WaylandProtocol("output rectangle bottom overflows".into()))?;
    let left = left.max(0);
    let top = top.max(0);
    let right = right.min(i64::from(buffer_size.width));
    let bottom = bottom.min(i64::from(buffer_size.height));
    if left >= right || top >= bottom {
        return Ok(None);
    }
    Ok(Some(Rect::new(
        i32::try_from(left).map_err(|_| {
            VshotError::WaylandProtocol("output rectangle x is out of range".into())
        })?,
        i32::try_from(top).map_err(|_| {
            VshotError::WaylandProtocol("output rectangle y is out of range".into())
        })?,
        u32::try_from(right - left).map_err(|_| {
            VshotError::WaylandProtocol("output rectangle width is out of range".into())
        })?,
        u32::try_from(bottom - top).map_err(|_| {
            VshotError::WaylandProtocol("output rectangle height is out of range".into())
        })?,
    )))
}

fn restore_bgra_logical_rect(
    source: &[u8],
    destination: &mut [u8],
    stride: usize,
    source_size: Size,
    logical_rect: Rect,
    output: Rect,
    scale: u32,
) -> Result<()> {
    let Some(pixel_rect) = logical_rect_to_output_pixels(logical_rect, output, scale, source_size)?
    else {
        return Ok(());
    };
    copy_bgra_rect(
        source,
        stride,
        source_size,
        destination,
        stride,
        pixel_rect,
        pixel_rect.origin,
    )
}

fn toolbar_bounds(editor: &EditorState) -> Result<Option<Rect>> {
    editor
        .toolbar()
        .items()
        .iter()
        .try_fold(None, |bounds, item| rect_union(bounds, Some(item.rect)))
}

fn draw_selection_border(
    map: &mut [u8],
    stride: usize,
    mapped: MappedSelection,
    scale: u32,
) -> Result<()> {
    let thickness = usize::try_from(scale.clamp(1, 4)).unwrap_or(1);
    let rect = mapped.rect;
    let right = usize::try_from(rect.right()?)
        .map_err(|_| VshotError::InvalidGeometry("selection right is out of range".into()))?;
    let bottom = usize::try_from(rect.bottom()?)
        .map_err(|_| VshotError::InvalidGeometry("selection bottom is out of range".into()))?;
    let left = usize::try_from(rect.left())
        .map_err(|_| VshotError::InvalidGeometry("selection left is out of range".into()))?;
    let top = usize::try_from(rect.top())
        .map_err(|_| VshotError::InvalidGeometry("selection top is out of range".into()))?;
    if stride < 4
        || left > right
        || top > bottom
        || right > stride / 4
        || bottom > map.len() / stride
    {
        return Err(VshotError::InvalidGeometry(
            "selection border is outside the buffer".into(),
        ));
    }
    let width = right - left;
    let height = bottom - top;
    let thickness_x = thickness.min(width);
    let thickness_y = thickness.min(height);
    let color = [0, 220, 255, 255];

    if mapped.top {
        for y in top..top + thickness_y {
            for x in left..right {
                let index = y * stride + x * 4;
                map[index..index + 4].copy_from_slice(&color);
            }
        }
    }
    if mapped.bottom {
        for y in bottom - thickness_y..bottom {
            for x in left..right {
                let index = y * stride + x * 4;
                map[index..index + 4].copy_from_slice(&color);
            }
        }
    }
    if mapped.left {
        for y in top..bottom {
            for x in left..left + thickness_x {
                let index = y * stride + x * 4;
                map[index..index + 4].copy_from_slice(&color);
            }
        }
    }
    if mapped.right {
        for y in top..bottom {
            for x in right - thickness_x..right {
                let index = y * stride + x * 4;
                map[index..index + 4].copy_from_slice(&color);
            }
        }
    }
    Ok(())
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

fn build_dimmed_bgra(frame: &Frame, stride: usize, width: u32, height: u32) -> Result<Vec<u8>> {
    let map_len = checked_buffer_len(stride, width, height)?;
    let mut dimmed = vec![0; map_len];
    frame.copy_rgba_to_bgra(&mut dimmed, stride, Point::new(0, 0))?;
    dim_bgra(&mut dimmed, stride, width, height)?;
    Ok(dimmed)
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

fn copy_bgra_rect(
    source: &[u8],
    source_stride: usize,
    source_size: Size,
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
    let source_len = checked_buffer_len(source_stride, source_size.width, source_size.height)?;
    if source.len() < source_len {
        return Err(VshotError::InvalidGeometry(
            "source buffer is too small for its dimensions".into(),
        ));
    }
    let source_x = usize::try_from(source_rect.left())
        .map_err(|_| VshotError::InvalidGeometry("source rectangle is out of range".into()))?;
    let source_y = usize::try_from(source_rect.top())
        .map_err(|_| VshotError::InvalidGeometry("source rectangle is out of range".into()))?;
    let source_right = usize::try_from(source_rect.right()?)
        .map_err(|_| VshotError::InvalidGeometry("source rectangle is out of range".into()))?;
    let source_bottom = usize::try_from(source_rect.bottom()?)
        .map_err(|_| VshotError::InvalidGeometry("source rectangle is out of range".into()))?;
    let source_width = usize::try_from(source_size.width)
        .map_err(|_| VshotError::InvalidGeometry("source width is too large".into()))?;
    let source_height = usize::try_from(source_size.height)
        .map_err(|_| VshotError::InvalidGeometry("source height is too large".into()))?;
    if source_right > source_width || source_bottom > source_height {
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
        let source_row = (source_y + row)
            .checked_mul(source_stride)
            .and_then(|row| row.checked_add(source_x.checked_mul(4)?))
            .ok_or_else(|| VshotError::InvalidGeometry("source row is too large".into()))?;
        let destination_row = (destination_y + row)
            .checked_mul(destination_stride)
            .and_then(|row| row.checked_add(destination_x.checked_mul(4)?))
            .ok_or_else(|| VshotError::InvalidGeometry("destination row is too large".into()))?;
        destination[destination_row..destination_row + row_bytes]
            .copy_from_slice(&source[source_row..source_row + row_bytes]);
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

/// Converts an annotation stroke width from logical pixels to device pixels.
fn device_stroke_width(logical_width: u32, scale: u32) -> Result<u32> {
    let width = logical_width
        .max(1)
        .checked_mul(scale)
        .ok_or_else(|| VshotError::WaylandProtocol("annotation width overflows".into()))?;
    Ok(width.clamp(1, 4096))
}

fn render_annotations(
    frame: &mut Frame,
    editor: &EditorState,
    output: Rect,
    scale: u32,
) -> Result<()> {
    let mut pipeline = EditPipeline::new();
    for annotation in editor.annotations() {
        let color = annotation.color();
        let width = device_stroke_width(annotation.width(), scale)?;
        let dash = annotation.dash();
        let head = annotation.head();
        let arrow_style = annotation.arrow_style();
        let strength = annotation.strength();
        match annotation {
            Annotation::Shape {
                tool, rect, mask, ..
            } => {
                let rect = map_rect(*rect, output, scale)?;
                match tool {
                    EditorTool::Rectangle => {
                        pipeline = pipeline.rectangle_stroke(rect, color, width, dash);
                    }
                    EditorTool::Ellipse => {
                        pipeline = pipeline.ellipse_stroke(rect, color, width, dash);
                    }
                    EditorTool::Mosaic | EditorTool::Blur => {
                        let block = crate::edit::mosaic_block_size(strength, scale);
                        pipeline = match mask {
                            crate::edit::ShapeMask::Rect => pipeline.mosaic(rect, block),
                            crate::edit::ShapeMask::Ellipse => pipeline.mosaic_ellipse(rect, block),
                        };
                    }
                    _ => {}
                }
            }
            Annotation::Stroke { tool, points, .. } => {
                let points = points
                    .iter()
                    .map(|point| map_point(*point, output, scale))
                    .collect::<Result<Vec<_>>>()?;
                match tool {
                    EditorTool::Arrow if points.len() >= 2 => {
                        pipeline = pipeline.arrow_with_style(
                            points[0],
                            *points.last().unwrap(),
                            color,
                            width,
                            dash,
                            head,
                            arrow_style,
                        );
                    }
                    EditorTool::Mosaic | EditorTool::Blur => {
                        let radius = crate::edit::mosaic_brush_radius(strength, width);
                        pipeline = pipeline.mosaic_brush(points, radius);
                    }
                    EditorTool::Pen | EditorTool::Draw | EditorTool::Line => {
                        pipeline = pipeline.freehand(points, color, width, dash);
                    }
                    _ => {}
                }
            }
            Annotation::Image { rect, pixels } => {
                // Both the rect and the image are in the scene's logical
                // pixels; the frame is in this output's device pixels.
                let rect = map_rect(*rect, output, scale)?;
                pipeline = pipeline.blit_scaled(rect, pixels.clone());
            }
            Annotation::Text {
                origin,
                text,
                scale: text_scale,
                bitmap,
                ..
            } => {
                let origin = map_point(*origin, output, scale)?;
                pipeline = match bitmap {
                    Some(bitmap) => pipeline.blit(origin, bitmap.clone()),
                    None => {
                        pipeline.text(origin, text, color, text_scale.saturating_mul(scale).max(1))
                    }
                };
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
    buffer_height: u32,
    output: Rect,
    scale: u32,
    editor: &EditorState,
) -> Result<()> {
    let Some(selection) = editor.selection() else {
        return Ok(());
    };
    let Some(mapped) = map_selection_to_output_pixels(
        selection,
        output,
        scale,
        Size::new(buffer_width, buffer_height),
    )?
    else {
        return Ok(());
    };
    let size = scale.saturating_mul(3).clamp(4, 12);
    let rect = mapped.rect;
    let left = u32::try_from(rect.left()).unwrap_or(0);
    let top = u32::try_from(rect.top()).unwrap_or(0);
    let right = u32::try_from(rect.right()?)
        .map_err(|_| VshotError::WaylandProtocol("handle right is out of range".into()))?;
    let bottom = u32::try_from(rect.bottom()?)
        .map_err(|_| VshotError::WaylandProtocol("handle bottom is out of range".into()))?;
    for (visible, x, y) in [
        (mapped.corners[0], left, top),
        (mapped.corners[1], right.saturating_sub(size), top),
        (mapped.corners[2], left, bottom.saturating_sub(size)),
        (
            mapped.corners[3],
            right.saturating_sub(size),
            bottom.saturating_sub(size),
        ),
    ] {
        if visible {
            draw_bgra_rect(map, stride, x, y, size, size, [255, 255, 255, 255]);
        }
    }
    Ok(())
}

fn render_toolbar(
    map: &mut [u8],
    stride: usize,
    buffer_width: u32,
    buffer_height: u32,
    output: Rect,
    scale: u32,
    editor: &EditorState,
) -> Result<()> {
    let buffer_size = Size::new(buffer_width, buffer_height);
    for item in editor.toolbar().items() {
        let Some(local) = logical_rect_to_output_pixels(item.rect, output, scale, buffer_size)?
        else {
            continue;
        };
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
    pub(crate) pending_parent_redraw: bool,
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
    fn build_dimmed_bgra_converts_and_dims_the_complete_frame() {
        let frame = Frame::solid(Size::new(2, 1), [100, 80, 60, 255]).unwrap();

        assert_eq!(
            build_dimmed_bgra(&frame, 12, 2, 1).unwrap(),
            vec![30, 40, 50, 255, 30, 40, 50, 255, 0, 0, 0, 0]
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
    fn copy_bgra_rect_preserves_channels_and_row_padding() {
        let source = vec![
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0xaa, 0xbb, 0xcc, 0xdd, 13, 14, 15, 16, 17, 18,
            19, 20, 21, 22, 23, 24, 0xee, 0xff, 0x11, 0x22,
        ];
        let mut destination = vec![0x99; 32];

        copy_bgra_rect(
            &source,
            16,
            Size::new(3, 2),
            &mut destination,
            16,
            Rect::new(1, 0, 2, 2),
            Point::new(0, 0),
        )
        .unwrap();

        assert_eq!(&destination[0..8], &[5, 6, 7, 8, 9, 10, 11, 12]);
        assert_eq!(&destination[16..24], &[17, 18, 19, 20, 21, 22, 23, 24]);
        assert_eq!(destination[8..16], [0x99; 8]);
        assert_eq!(destination[24..32], [0x99; 8]);
    }

    #[test]
    fn rect_union_and_expand_keep_signed_edges_safe() {
        let first = Rect::new(-10, -5, 4, 3);
        let second = Rect::new(-4, -8, 10, 6);
        assert_eq!(
            rect_union(Some(first), Some(second)).unwrap(),
            Some(Rect::new(-10, -8, 16, 6))
        );
        assert_eq!(rect_union(None, Some(first)).unwrap(), Some(first));
        assert_eq!(expand_rect(first, 2).unwrap(), Rect::new(-12, -7, 8, 7));
    }

    #[test]
    fn selection_delta_uses_non_overlapping_bands() {
        let previous = Rect::new(10, 10, 100, 100);
        let current = Rect::new(20, 20, 100, 100);

        assert_eq!(
            rect_difference(previous, Some(current)).unwrap(),
            vec![Rect::new(10, 10, 100, 10), Rect::new(10, 20, 10, 90)]
        );
        assert_eq!(
            rect_difference(current, Some(previous)).unwrap(),
            vec![Rect::new(20, 110, 100, 10), Rect::new(110, 20, 10, 90)]
        );

        let copied = selection_copy_rects(Some(previous), Some(current)).unwrap();
        assert!(!copied.contains(&current));
        assert!(copied.contains(&Rect::new(20, 110, 100, 10)));
        assert!(copied.contains(&Rect::new(110, 20, 10, 90)));
    }

    #[test]
    fn logical_rect_maps_and_clips_negative_output_origin() {
        let output = Rect::new(-1920, 10, 100, 50);
        assert_eq!(
            logical_rect_to_output_pixels(
                Rect::new(-1930, 0, 30, 40),
                output,
                2,
                Size::new(200, 100),
            )
            .unwrap(),
            Some(Rect::new(0, 0, 40, 60))
        );
        assert_eq!(
            logical_rect_to_output_pixels(
                Rect::new(-1900, 20, 20, 10),
                output,
                2,
                Size::new(60, 100),
            )
            .unwrap(),
            Some(Rect::new(40, 20, 20, 20))
        );
    }

    #[test]
    fn cross_output_mapping_only_draws_global_edges_and_owned_corners() {
        let selection = Rect::new(0, 10, 200, 20);
        let left_output = map_selection_to_output_pixels(
            selection,
            Rect::new(0, 0, 100, 100),
            1,
            Size::new(100, 100),
        )
        .unwrap()
        .unwrap();
        assert_eq!(left_output.rect, Rect::new(0, 10, 100, 20));
        assert_eq!(
            [
                left_output.top,
                left_output.right,
                left_output.bottom,
                left_output.left,
            ],
            [true, false, true, true]
        );
        assert_eq!(left_output.corners, [true, false, true, false]);

        let right_output = map_selection_to_output_pixels(
            selection,
            Rect::new(100, 0, 100, 100),
            1,
            Size::new(100, 100),
        )
        .unwrap()
        .unwrap();
        assert_eq!(right_output.rect, Rect::new(0, 10, 100, 20));
        assert_eq!(
            [
                right_output.top,
                right_output.right,
                right_output.bottom,
                right_output.left,
            ],
            [true, true, true, false]
        );
        assert_eq!(right_output.corners, [false, true, false, true]);
    }

    #[test]
    fn selection_copy_is_limited_to_the_current_selection_and_dirty_region() {
        let old = Some(Rect::new(10, 10, 10, 10));
        let current = Some(Rect::new(18, 18, 10, 10));
        let dirty = selection_dirty_rect(old, current, 1).unwrap();

        assert_eq!(
            selection_copy_rect(dirty, current),
            Some(Rect::new(18, 18, 10, 10))
        );
        assert_eq!(
            selection_copy_rect(Some(Rect::new(0, 0, 5, 5)), current),
            None
        );
    }

    #[test]
    fn selection_dirty_rect_expands_old_and_new_handles_in_logical_units() {
        assert_eq!(handle_padding(1).unwrap(), 4);
        assert_eq!(handle_padding(2).unwrap(), 3);
        assert_eq!(
            selection_dirty_rect(
                Some(Rect::new(10, 20, 10, 10)),
                Some(Rect::new(30, 40, 10, 10)),
                2,
            )
            .unwrap(),
            Some(Rect::new(7, 17, 36, 36))
        );
    }

    #[test]
    fn incremental_restore_covers_old_and_new_selection_union() {
        let source = (0..32).map(|value| value as u8).collect::<Vec<_>>();
        let mut destination = vec![0xee; source.len()];
        let output = Rect::new(-5, 10, 2, 1);
        let old_selection = Rect::new(-5, 10, 1, 1);
        let new_selection = Rect::new(-4, 10, 1, 1);
        let dirty = selection_dirty_rect(Some(old_selection), Some(new_selection), 2).unwrap();

        restore_bgra_logical_rect(
            &source,
            &mut destination,
            16,
            Size::new(4, 2),
            dirty.unwrap(),
            output,
            2,
        )
        .unwrap();

        assert_eq!(&destination[..16], &source[..16]);
        assert_eq!(&destination[16..], &source[16..]);
    }

    #[test]
    fn selection_border_writes_only_visible_edges() {
        let mut map = vec![0x11; 8 * 8 * 4];
        let mapped = MappedSelection {
            rect: Rect::new(1, 1, 6, 6),
            top: true,
            right: true,
            bottom: true,
            left: true,
            corners: [true; 4],
        };

        draw_selection_border(&mut map, 8 * 4, mapped, 1).unwrap();

        let mut changed = 0;
        for y in 0..8 {
            for x in 0..8 {
                let pixel = &map[(y * 8 + x) * 4..(y * 8 + x + 1) * 4];
                if pixel == [0, 220, 255, 255] {
                    changed += 1;
                } else if (2..6).contains(&x) && (2..6).contains(&y) {
                    assert_eq!(pixel, [0x11; 4]);
                }
            }
        }
        assert_eq!(changed, 20);
    }

    #[test]
    fn stale_buffer_release_does_not_release_the_replacement_token() {
        let mut available = false;
        assert!(!release_buffer_if_current(
            BufferToken::Parent(22),
            BufferToken::Parent(21),
            &mut available,
        ));
        assert!(!available);
        assert!(release_buffer_if_current(
            BufferToken::Parent(22),
            BufferToken::Parent(22),
            &mut available,
        ));
        assert!(available);
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

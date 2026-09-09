use std::collections::HashMap;

use wayland_client::protocol::wl_output;
use wayland_protocols::wp::cursor_shape::v1::client::{
    wp_cursor_shape_device_v1, wp_cursor_shape_manager_v1,
};

use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect, Size};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct OutputInfo {
    pub global_id: u32,
    pub name: String,
    pub geometry: Rect,
    pub pixel_size: Size,
    pub scale: u32,
    pub transform: wl_output::Transform,
}

impl OutputInfo {
    pub fn is_supported(&self) -> bool {
        let expected_width = self.geometry.size.width.checked_mul(self.scale);
        let expected_height = self.geometry.size.height.checked_mul(self.scale);
        self.scale > 0
            && self.transform == wl_output::Transform::Normal
            && !self.geometry.is_empty()
            && expected_width == Some(self.pixel_size.width)
            && expected_height == Some(self.pixel_size.height)
    }
}

#[derive(Clone, Debug, Default)]
pub(crate) struct OutputData {
    pub(crate) name: Option<String>,
    pub(crate) logical_position: Option<Point>,
    pub(crate) logical_size: Option<Size>,
    pub(crate) pixel_size: Option<Size>,
    pub(crate) scale: u32,
    pub(crate) transform: Option<wl_output::Transform>,
    pub(crate) wl_done: bool,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct OutputUserData {
    pub(crate) global_id: u32,
}

#[derive(Debug, Default)]
pub(crate) struct TopologyState {
    pub(crate) compositor: Option<wayland_client::protocol::wl_compositor::WlCompositor>,
    pub(crate) shm: Option<wayland_client::protocol::wl_shm::WlShm>,
    pub(crate) shm_argb8888: bool,
    pub(crate) shm_xrgb8888: bool,
    pub(crate) layer_shell: Option<wayland_protocols_wlr::layer_shell::v1::client::zwlr_layer_shell_v1::ZwlrLayerShellV1>,
    pub(crate) cursor_shape_manager: Option<wp_cursor_shape_manager_v1::WpCursorShapeManagerV1>,
    pub(crate) cursor_shape_device: Option<wp_cursor_shape_device_v1::WpCursorShapeDeviceV1>,
    pub(crate) cursor_enter_serial: Option<u32>,
    pub(crate) cursor_shape: Option<wp_cursor_shape_device_v1::Shape>,
    pub(crate) xdg_output_manager: Option<wayland_protocols::xdg::xdg_output::zv1::client::zxdg_output_manager_v1::ZxdgOutputManagerV1>,
    pub(crate) seats: Vec<wayland_client::protocol::wl_seat::WlSeat>,
    pub(crate) output_proxies: HashMap<u32, wayland_client::protocol::wl_output::WlOutput>,
    pub(crate) xdg_outputs: HashMap<u32, wayland_protocols::xdg::xdg_output::zv1::client::zxdg_output_v1::ZxdgOutputV1>,
    pub(crate) outputs: HashMap<u32, OutputData>,
    pub(crate) pointer: Option<wayland_client::protocol::wl_pointer::WlPointer>,
    pub(crate) keyboard: Option<wayland_client::protocol::wl_keyboard::WlKeyboard>,
    pub(crate) pointer_capability: bool,
    pub(crate) keyboard_capability: bool,
    pub(crate) topology_changed: bool,
}

impl TopologyState {
    pub(crate) fn ensure_xdg_outputs<State>(&mut self, qh: &wayland_client::QueueHandle<State>)
    where
        State: wayland_client::Dispatch<
                wayland_protocols::xdg::xdg_output::zv1::client::zxdg_output_v1::ZxdgOutputV1,
                u32,
            > + 'static,
    {
        let Some(manager) = self.xdg_output_manager.as_ref().cloned() else {
            return;
        };
        let output_ids = self.output_proxies.keys().copied().collect::<Vec<_>>();
        for global_id in output_ids {
            if self.xdg_outputs.contains_key(&global_id) {
                continue;
            }
            if let Some(output) = self.output_proxies.get(&global_id) {
                let xdg_output = manager.get_xdg_output(output, qh, global_id);
                self.xdg_outputs.insert(global_id, xdg_output);
            }
        }
    }

    pub(crate) fn shm_format(&self) -> Option<wayland_client::protocol::wl_shm::Format> {
        if self.shm_argb8888 {
            Some(wayland_client::protocol::wl_shm::Format::Argb8888)
        } else if self.shm_xrgb8888 {
            Some(wayland_client::protocol::wl_shm::Format::Xrgb8888)
        } else {
            None
        }
    }

    pub(crate) fn output_infos(&self) -> Result<Vec<OutputInfo>> {
        if self.output_proxies.is_empty() {
            return Err(VshotError::IncompleteTopology(
                "no wl_output globals were advertised".into(),
            ));
        }
        if self.xdg_output_manager.is_none() {
            return Err(VshotError::MissingCapability(
                "zxdg_output_manager_v1".into(),
            ));
        }
        if self.xdg_outputs.len() != self.output_proxies.len() {
            return Err(VshotError::IncompleteTopology(
                "not every wl_output has an xdg-output object".into(),
            ));
        }
        if !self.pointer_capability {
            return Err(VshotError::MissingCapability("seat pointer".into()));
        }
        if !self.keyboard_capability {
            return Err(VshotError::MissingCapability("seat keyboard".into()));
        }

        let mut ids = self.output_proxies.keys().copied().collect::<Vec<_>>();
        ids.sort_unstable();
        let mut outputs = Vec::with_capacity(ids.len());
        for global_id in ids {
            let output = self.outputs.get(&global_id).ok_or_else(|| {
                VshotError::IncompleteTopology(format!(
                    "wl_output {global_id} has no tracked metadata"
                ))
            })?;
            let name = output.name.clone().ok_or_else(|| {
                VshotError::IncompleteTopology(format!("wl_output {global_id} has no output name"))
            })?;
            let logical_position = output.logical_position.ok_or_else(|| {
                VshotError::IncompleteTopology(format!(
                    "output {name} has no xdg-output logical position"
                ))
            })?;
            let logical_size = output.logical_size.ok_or_else(|| {
                VshotError::IncompleteTopology(format!(
                    "output {name} has no xdg-output logical size"
                ))
            })?;
            let pixel_size = output.pixel_size.ok_or_else(|| {
                VshotError::IncompleteTopology(format!(
                    "output {name} has no current wl_output mode"
                ))
            })?;
            let transform = output.transform.ok_or_else(|| {
                VshotError::IncompleteTopology(format!("output {name} has no wl_output transform"))
            })?;
            let info = OutputInfo {
                global_id,
                name,
                geometry: Rect::new(
                    logical_position.x,
                    logical_position.y,
                    logical_size.width,
                    logical_size.height,
                ),
                pixel_size,
                scale: output.scale,
                transform,
            };
            if !info.is_supported() {
                return Err(VshotError::UnsupportedOutput(format!(
                    "output {} is not an unrotated integer-scale mapping (logical {}x{}, pixels {}x{}, scale {}, transform {:?})",
                    info.name,
                    info.geometry.size.width,
                    info.geometry.size.height,
                    info.pixel_size.width,
                    info.pixel_size.height,
                    info.scale,
                    info.transform
                )));
            }
            outputs.push(info);
        }
        Ok(outputs)
    }
}

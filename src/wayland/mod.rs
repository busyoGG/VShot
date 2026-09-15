pub mod freeze_overlay;
pub mod input;
pub mod topology;

use std::collections::{HashMap, HashSet};
use std::time::{Duration, Instant};

use wayland_client::protocol::{
    wl_buffer, wl_compositor, wl_keyboard, wl_output, wl_pointer, wl_registry, wl_seat, wl_shm,
    wl_shm_pool, wl_surface,
};
use wayland_client::{Connection, Dispatch, EventQueue, Proxy, QueueHandle, WEnum};
use wayland_protocols::wp::cursor_shape::v1::client::{
    wp_cursor_shape_device_v1, wp_cursor_shape_manager_v1,
};
use wayland_protocols::xdg::xdg_output::zv1::client::{zxdg_output_manager_v1, zxdg_output_v1};
use wayland_protocols_wlr::layer_shell::v1::client::{zwlr_layer_shell_v1, zwlr_layer_surface_v1};

use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect};
use crate::model::SceneSnapshot;

use self::freeze_overlay::{
    release_buffer_if_current, BufferToken, BufferUserData, LayerSurfaceUserData, OverlaySurface,
    PoolUserData, ShmSlot, SurfaceUserData,
};
use self::input::{
    global_point, EditorState, ResizeHandle, SelectionEvent, SelectionResult, SelectionTracker,
    BTN_LEFT, KEY_ESC,
};
use self::topology::{OutputData, OutputInfo, OutputUserData, TopologyState};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum RedrawTarget {
    ParentSelection,
    ParentEditor,
}

pub struct WaylandSession {
    event_queue: EventQueue<WaylandState>,
    state: WaylandState,
}

#[derive(Debug, Default)]
struct WaylandState {
    topology: TopologyState,
    scene: Option<SceneSnapshot>,
    overlays: HashMap<u32, OverlaySurface>,
    ready_outputs: HashSet<u32>,
    pointer_output: Option<u32>,
    pointer_grab_output: Option<u32>,
    // (output id, surface-local logical x/y) of the last pointer focus; the
    // compositor only routes pointer events to mapped surfaces, so this is
    // populated once the frozen overlays exist.
    pointer_position: Option<(u32, f64, f64)>,
    keyboard_focus_output: Option<u32>,
    selection: SelectionTracker,
    selection_mode: bool,
    editor: Option<EditorState>,
    selection_outcome: Option<SelectionResult>,
    interaction_redraw_pending: bool,
    next_buffer_id: u64,
    error: Option<VshotError>,
}

impl WaylandState {
    fn fail(&mut self, error: VshotError) {
        if self.error.is_none() {
            self.error = Some(error);
        }
    }

    fn request_interaction_redraw(&mut self) {
        self.interaction_redraw_pending = true;
    }

    fn take_interaction_redraw(&mut self) -> bool {
        std::mem::take(&mut self.interaction_redraw_pending)
    }

    fn redraw_target(&self) -> RedrawTarget {
        if self.editor.is_some() {
            RedrawTarget::ParentEditor
        } else {
            RedrawTarget::ParentSelection
        }
    }

    fn pointer_motion_output(&self) -> Option<u32> {
        self.pointer_grab_output.or(self.pointer_output)
    }

    fn set_cursor_shape(&mut self, shape: wp_cursor_shape_device_v1::Shape) {
        let Some(serial) = self.topology.cursor_enter_serial else {
            return;
        };
        let Some(device) = self.topology.cursor_shape_device.clone() else {
            return;
        };
        if self.topology.cursor_shape == Some(shape) {
            return;
        }
        device.set_shape(serial, shape);
        self.topology.cursor_shape = Some(shape);
    }

    fn update_cursor_shape(&mut self) {
        let shape = self
            .editor
            .as_ref()
            .and_then(|editor| {
                editor.pointer().map(|point| {
                    if editor.toolbar_hit_test(point).is_some() || !editor.tool().is_selection() {
                        wp_cursor_shape_device_v1::Shape::Pointer
                    } else {
                        cursor_shape_for_handle(editor.hit_test_selection(point))
                    }
                })
            })
            .unwrap_or(wp_cursor_shape_device_v1::Shape::Default);
        self.set_cursor_shape(shape);
    }

    fn selection_for_render(&self) -> Option<Rect> {
        match self.editor.as_ref() {
            Some(editor) => editor.selection(),
            None => self.selection.selection(),
        }
    }

    fn clear_overlays(&mut self) {
        self.editor = None;
        self.topology.cursor_enter_serial = None;
        self.topology.cursor_shape = None;
        self.interaction_redraw_pending = false;
        self.keyboard_focus_output = None;
        self.overlays.clear();
        self.ready_outputs.clear();
    }

    fn origins(&self) -> HashMap<u32, Point> {
        self.topology
            .outputs
            .iter()
            .filter_map(|(id, output)| output.logical_position.map(|origin| (*id, origin)))
            .collect()
    }

    fn process_selection_event(&mut self, event: SelectionEvent) -> bool {
        if self.selection_outcome.is_some() {
            return false;
        }
        let origins = self.origins();
        let result = self
            .selection
            .handle(event, |output_id| origins.get(&output_id).copied());
        let redraw = self.selection.dragging();
        match result {
            Ok(result) => {
                if !matches!(result, SelectionResult::Continue) {
                    self.selection_outcome = Some(result);
                }
            }
            Err(error) => self.fail(error),
        }
        redraw
    }

    fn process_editor_event(&mut self, event: SelectionEvent) {
        let origins = self.origins();
        let Some(editor) = self.editor.as_mut() else {
            return;
        };
        let result =
            editor.handle_event_with_origin(event, |output_id| origins.get(&output_id).copied());
        if let Err(error) = result {
            self.fail(error);
        }
    }

    fn next_buffer_token(&mut self) -> Result<u64> {
        let token = self
            .next_buffer_id
            .checked_add(1)
            .ok_or_else(|| VshotError::WaylandProtocol("buffer token exhausted".into()))?;
        self.next_buffer_id = token;
        Ok(token)
    }

    fn redraw_editor(&mut self) {
        let Some(scene) = self.scene.clone() else {
            self.fail(VshotError::WaylandProtocol(
                "cannot redraw an overlay before a frozen scene is installed".into(),
            ));
            return;
        };
        let Some(editor_snapshot) = self.editor.clone() else {
            return;
        };
        let selection = self.selection_for_render();
        let scene_bounds = Some(scene.bounds());
        let output_ids = self.overlays.keys().copied().collect::<Vec<_>>();
        for output_id in output_ids {
            let Some(output) = scene.output(output_id).cloned() else {
                self.fail(VshotError::IncompleteTopology(format!(
                    "frozen scene has no output {output_id}"
                )));
                continue;
            };
            let Some(overlay) = self.overlays.get_mut(&output_id) else {
                continue;
            };
            if !overlay.configured || overlay.closed {
                continue;
            }
            let Some(slot_index) = overlay.attach_available() else {
                overlay.pending_parent_redraw = true;
                continue;
            };
            if let Err(error) = overlay.slots[slot_index].render_editor(
                &output.frame,
                selection,
                output.geometry,
                output.scale,
                Some(&editor_snapshot),
                scene_bounds,
            ) {
                self.fail(error);
                continue;
            }
            overlay.pending_parent_redraw = false;
            overlay.commit_slot(slot_index);
        }
    }

    fn redraw_selection(&mut self) {
        let Some(scene) = self.scene.clone() else {
            self.fail(VshotError::WaylandProtocol(
                "cannot redraw an overlay before a frozen scene is installed".into(),
            ));
            return;
        };
        let selection = self.selection.selection();
        let output_ids = self.overlays.keys().copied().collect::<Vec<_>>();
        for output_id in output_ids {
            let Some(output) = scene.output(output_id).cloned() else {
                self.fail(VshotError::IncompleteTopology(format!(
                    "frozen scene has no output {output_id}"
                )));
                continue;
            };
            let Some(overlay) = self.overlays.get_mut(&output_id) else {
                continue;
            };
            if !overlay.configured || overlay.closed {
                continue;
            }
            let Some(slot_index) = overlay.attach_available() else {
                overlay.pending_parent_redraw = true;
                continue;
            };
            if let Err(error) = overlay.slots[slot_index].render_editor(
                &output.frame,
                selection,
                output.geometry,
                output.scale,
                None,
                None,
            ) {
                self.fail(error);
                continue;
            }
            overlay.pending_parent_redraw = false;
            overlay.commit_slot(slot_index);
        }
    }

    fn handle_buffer_release(&mut self, data: BufferUserData) {
        let mut redraw = false;
        if let Some(overlay) = self.overlays.get_mut(&data.output_id) {
            match data.token {
                BufferToken::Parent(_) => {
                    if let Some(slot) = overlay
                        .slots
                        .iter_mut()
                        .find(|slot| slot.token == data.token)
                    {
                        if release_buffer_if_current(slot.token, data.token, &mut slot.available) {
                            redraw = overlay.pending_parent_redraw;
                        }
                    }
                }
            }
        }
        if redraw {
            self.request_interaction_redraw();
        }
    }
}

impl WaylandSession {
    pub fn connect() -> Result<Self> {
        let connection = Connection::connect_to_env()
            .map_err(|error| VshotError::WaylandConnection(error.to_string()))?;
        let mut event_queue = connection.new_event_queue::<WaylandState>();
        let qh = event_queue.handle();
        let mut state = WaylandState::default();
        connection.display().get_registry(&qh, ());
        event_queue
            .roundtrip(&mut state)
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        state.topology.ensure_xdg_outputs(&qh);
        event_queue
            .roundtrip(&mut state)
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        if let Some(error) = state.error.take() {
            return Err(error);
        }
        validate_capabilities(&state.topology)?;
        state.topology.output_infos()?;
        Ok(Self { event_queue, state })
    }

    pub fn output_infos(&self) -> Result<Vec<OutputInfo>> {
        self.state.topology.output_infos()
    }

    /// What the operations that read the pointer ask for, in the place they
    /// ask for it: a compositor can advertise a seat without a pointer — a
    /// nested KWin under another session advertises a keyboard-only one — and
    /// every capture that only reads pixels still has to work there.
    fn require_seat_pointer(&self) -> Result<()> {
        if !self.state.topology.pointer_capability || self.state.topology.pointer.is_none() {
            return Err(VshotError::MissingCapability("seat pointer".into()));
        }
        Ok(())
    }

    /// Selecting on the frozen scene needs both halves of the seat: the pointer
    /// to draw with and the keyboard to finish or cancel.
    fn require_seat_interaction(&self) -> Result<()> {
        self.require_seat_pointer()?;
        if !self.state.topology.keyboard_capability || self.state.topology.keyboard.is_none() {
            return Err(VshotError::MissingCapability("seat keyboard".into()));
        }
        Ok(())
    }

    pub fn set_scene(&mut self, scene: SceneSnapshot) {
        self.state.scene = Some(scene);
    }

    /// Freezes the scene on screen.  A plain freeze asks nothing of the seat —
    /// it is a picture and the capture follows — while `selection_mode` draws
    /// on it with the pointer and finishes from the keyboard, so that one needs
    /// both.
    pub fn show_frozen(&mut self, selection_mode: bool) -> Result<()> {
        if selection_mode {
            self.require_seat_interaction()?;
        }
        let scene = self.state.scene.clone().ok_or_else(|| {
            VshotError::WaylandProtocol("cannot show an overlay without a scene snapshot".into())
        })?;
        let infos = self.state.topology.output_infos()?;
        if infos.len() != scene.outputs().len() {
            return Err(VshotError::TopologyChanged);
        }
        if !self.state.overlays.is_empty() {
            self.destroy_overlays()?;
        }
        self.state.selection.reset();
        self.state.pointer_output = None;
        self.state.pointer_grab_output = None;
        self.state.keyboard_focus_output = None;
        self.state.selection_mode = selection_mode;
        self.state.editor = None;
        self.state.interaction_redraw_pending = false;
        self.state.selection_outcome = None;
        self.state.ready_outputs.clear();
        self.state.error = None;
        let keyboard_owner_output = if selection_mode {
            infos.first().map(|info| info.global_id)
        } else {
            None
        };
        let qh = self.event_queue.handle();
        let compositor = self
            .state
            .topology
            .compositor
            .clone()
            .ok_or_else(|| VshotError::MissingCapability("wl_compositor".into()))?;
        let layer_shell = self
            .state
            .topology
            .layer_shell
            .clone()
            .ok_or_else(|| VshotError::MissingCapability("zwlr_layer_shell_v1".into()))?;

        for info in infos {
            let output = self
                .state
                .topology
                .output_proxies
                .get(&info.global_id)
                .cloned()
                .ok_or(VshotError::TopologyChanged)?;
            let surface = compositor.create_surface(
                &qh,
                SurfaceUserData {
                    output_id: info.global_id,
                },
            );
            surface.set_buffer_scale(i32::try_from(info.scale).map_err(|_| {
                VshotError::UnsupportedOutput(format!("output {} scale is too large", info.name))
            })?);
            let layer_surface = layer_shell.get_layer_surface(
                &surface,
                Some(&output),
                zwlr_layer_shell_v1::Layer::Overlay,
                "vshot".to_string(),
                &qh,
                LayerSurfaceUserData {
                    output_id: info.global_id,
                },
            );
            // Zero dimensions plus all four anchors asks the compositor for the full output.
            layer_surface.set_size(0, 0);
            layer_surface.set_anchor(zwlr_layer_surface_v1::Anchor::all());
            layer_surface.set_exclusive_zone(-1);
            let keyboard_interactivity = if keyboard_owner_output == Some(info.global_id) {
                zwlr_layer_surface_v1::KeyboardInteractivity::Exclusive
            } else {
                zwlr_layer_surface_v1::KeyboardInteractivity::None
            };
            layer_surface.set_keyboard_interactivity(keyboard_interactivity);
            surface.commit();
            self.state.overlays.insert(
                info.global_id,
                OverlaySurface {
                    output_id: info.global_id,
                    surface,
                    _layer_surface: layer_surface,
                    configured: false,
                    closed: false,
                    width: info.geometry.size.width,
                    height: info.geometry.size.height,
                    slots: Vec::new(),
                    pending_parent_redraw: false,
                },
            );
        }

        self.dispatch_until(
            Instant::now() + Duration::from_secs(10),
            VshotError::OverlayTimeout,
            |state| state.ready_outputs.len() == state.overlays.len(),
        )?;
        if let Some(error) = self.state.error.take() {
            return Err(error);
        }
        if self.state.topology.topology_changed {
            return Err(VshotError::TopologyChanged);
        }
        Ok(())
    }

    /// The output the pointer is on: the one thing here that needs a pointer,
    /// so it is asked for here rather than at connect.
    pub fn wait_for_current_output(&mut self) -> Result<u32> {
        self.require_seat_pointer()?;
        self.dispatch_until(
            Instant::now() + Duration::from_secs(5),
            VshotError::CurrentOutputTimeout,
            |state| state.pointer_output.is_some(),
        )?;
        if self.state.topology.topology_changed {
            return Err(VshotError::TopologyChanged);
        }
        self.state
            .pointer_output
            .ok_or(VshotError::CurrentOutputTimeout)
    }

    /// Global pointer position in logical coordinates, if known. The
    /// compositor only routes pointer events to this client once the frozen
    /// overlay surfaces are mapped, so call this after `show_frozen`; the
    /// short event wait makes the first pointer enter/motion visible. A
    /// timeout is not fatal and simply yields `None`, as does a seat without a
    /// pointer — there is nothing to wait for in either case.
    pub fn pointer_position(&mut self) -> Result<Option<Point>> {
        if self.state.topology.pointer.is_none() {
            return Ok(None);
        }
        let deadline = Instant::now() + Duration::from_millis(300);
        let _ = self.dispatch_until(deadline, VshotError::CurrentOutputTimeout, |state| {
            state.pointer_position.is_some()
        });
        let Some((output_id, local_x, local_y)) = self.state.pointer_position else {
            return Ok(None);
        };
        let Some(output) = self
            .state
            .topology
            .output_infos()?
            .into_iter()
            .find(|info| info.global_id == output_id)
        else {
            return Ok(None);
        };
        global_point(output.geometry.origin, local_x, local_y).map(Some)
    }

    fn dispatch_until<F>(
        &mut self,
        deadline: Instant,
        timeout: VshotError,
        mut done: F,
    ) -> Result<()>
    where
        F: FnMut(&WaylandState) -> bool,
    {
        loop {
            self.event_queue
                .dispatch_pending(&mut self.state)
                .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
            if self.state.take_interaction_redraw() {
                match self.state.redraw_target() {
                    RedrawTarget::ParentEditor => self.state.redraw_editor(),
                    RedrawTarget::ParentSelection if self.state.selection_mode => {
                        self.state.redraw_selection()
                    }
                    RedrawTarget::ParentSelection => {}
                }
            }
            if let Some(error) = self.state.error.take() {
                return Err(error);
            }
            if done(&self.state) {
                return Ok(());
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err(timeout);
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
            let timeout_spec = rustix::event::Timespec::try_from(remaining).map_err(|_| {
                VshotError::WaylandProtocol("Wayland timeout is out of range".into())
            })?;
            let ready =
                rustix::event::poll(&mut poll_fds, Some(&timeout_spec)).map_err(|error| {
                    VshotError::WaylandProtocol(format!(
                        "failed to poll Wayland connection: {error}"
                    ))
                })?;
            if ready == 0 {
                drop(read_guard);
                return Err(timeout);
            }
            read_guard
                .read()
                .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        }
    }

    fn clear_overlays(&mut self) {
        self.state.clear_overlays();
    }

    pub fn destroy_overlays(&mut self) -> Result<()> {
        self.clear_overlays();
        self.event_queue
            .flush()
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))
    }
}

impl Drop for WaylandSession {
    fn drop(&mut self) {
        self.clear_overlays();
        let _ = self.event_queue.flush();
    }
}

fn decode_wayland_keycode(key: u32) -> u32 {
    // wl_keyboard reports Linux evdev keycodes directly.
    key
}

/// What the session needs before it can freeze the desktop and draw on it.
///
/// Only the drawing side is required here.  The seat is deliberately left out:
/// a compositor can offer a seat without a pointer — a nested KWin under
/// another session advertises a keyboard only — and everything vshot does with
/// pixels (capturing an output, a region, the whole scene) still works there.
/// The pointer and keyboard are asked for by the operations that read them:
/// [`WaylandSession::wait_for_current_output`], [`WaylandSession::show_frozen`]
/// in selection mode.
fn validate_capabilities(topology: &TopologyState) -> Result<()> {
    if topology.compositor.is_none() {
        return Err(VshotError::MissingCapability("wl_compositor".into()));
    }
    if topology.shm.is_none() {
        return Err(VshotError::MissingCapability("wl_shm".into()));
    }
    if topology.shm_format().is_none() {
        return Err(VshotError::MissingCapability(
            "wl_shm ARGB8888 or XRGB8888 format".into(),
        ));
    }
    if topology.layer_shell.is_none() {
        return Err(VshotError::MissingCapability("zwlr_layer_shell_v1".into()));
    }
    if topology.xdg_output_manager.is_none() {
        return Err(VshotError::MissingCapability(
            "zxdg_output_manager_v1".into(),
        ));
    }
    if topology.output_proxies.is_empty() {
        return Err(VshotError::MissingCapability(
            "at least one wl_output".into(),
        ));
    }
    Ok(())
}

fn ensure_cursor_shape_device(state: &mut WaylandState, qh: &QueueHandle<WaylandState>) {
    let Some(manager) = state.topology.cursor_shape_manager.clone() else {
        return;
    };
    let Some(pointer) = state.topology.pointer.clone() else {
        return;
    };
    if state.topology.cursor_shape_device.is_none() {
        state.topology.cursor_shape_device = Some(manager.get_pointer(&pointer, qh, ()));
    }
}

fn selection_cursor_shape() -> wp_cursor_shape_device_v1::Shape {
    wp_cursor_shape_device_v1::Shape::Crosshair
}

fn cursor_shape_for_handle(handle: ResizeHandle) -> wp_cursor_shape_device_v1::Shape {
    use wp_cursor_shape_device_v1::Shape;

    match handle {
        ResizeHandle::Top | ResizeHandle::Bottom => Shape::NsResize,
        ResizeHandle::Left | ResizeHandle::Right => Shape::EwResize,
        ResizeHandle::TopLeft | ResizeHandle::BottomRight => Shape::NwseResize,
        ResizeHandle::TopRight | ResizeHandle::BottomLeft => Shape::NeswResize,
        ResizeHandle::Move => Shape::Move,
        ResizeHandle::None => Shape::Default,
    }
}

impl Dispatch<wl_registry::WlRegistry, ()> for WaylandState {
    fn event(
        state: &mut Self,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        match event {
            wl_registry::Event::Global {
                name,
                interface,
                version,
            } => match interface.as_str() {
                "wl_compositor" if state.topology.compositor.is_none() => {
                    state.topology.compositor = Some(registry.bind(name, version.min(4), qh, ()));
                }
                "wl_shm" if state.topology.shm.is_none() => {
                    state.topology.shm = Some(registry.bind(name, version.min(1), qh, ()));
                }
                "zwlr_layer_shell_v1" if state.topology.layer_shell.is_none() => {
                    state.topology.layer_shell = Some(registry.bind(name, version.min(4), qh, ()));
                }
                "wp_cursor_shape_manager_v1" if state.topology.cursor_shape_manager.is_none() => {
                    state.topology.cursor_shape_manager =
                        Some(registry.bind(name, version.min(2), qh, ()));
                    ensure_cursor_shape_device(state, qh);
                }
                "zxdg_output_manager_v1" if state.topology.xdg_output_manager.is_none() => {
                    state.topology.xdg_output_manager =
                        Some(registry.bind(name, version.min(3), qh, ()));
                }
                "wl_output" => {
                    let output = registry.bind::<wl_output::WlOutput, _, _>(
                        name,
                        version.min(4),
                        qh,
                        OutputUserData { global_id: name },
                    );
                    state.topology.output_proxies.insert(name, output);
                    state.topology.outputs.insert(
                        name,
                        OutputData {
                            scale: 1,
                            ..Default::default()
                        },
                    );
                }
                "wl_seat" => {
                    let seat = registry.bind(name, version.min(7), qh, ());
                    state.topology.seats.push(seat);
                }
                _ => {}
            },
            wl_registry::Event::GlobalRemove { .. } => state.topology.topology_changed = true,
            _ => {}
        }
    }
}

impl Dispatch<wp_cursor_shape_manager_v1::WpCursorShapeManagerV1, ()> for WaylandState {
    fn event(
        _: &mut Self,
        _: &wp_cursor_shape_manager_v1::WpCursorShapeManagerV1,
        _: wp_cursor_shape_manager_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wp_cursor_shape_device_v1::WpCursorShapeDeviceV1, ()> for WaylandState {
    fn event(
        _: &mut Self,
        _: &wp_cursor_shape_device_v1::WpCursorShapeDeviceV1,
        _: wp_cursor_shape_device_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_compositor::WlCompositor, ()> for WaylandState {
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

impl Dispatch<wl_shm::WlShm, ()> for WaylandState {
    fn event(
        state: &mut Self,
        _: &wl_shm::WlShm,
        event: wl_shm::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_shm::Event::Format {
            format: WEnum::Value(format),
        } = event
        {
            match format {
                wl_shm::Format::Argb8888 => state.topology.shm_argb8888 = true,
                wl_shm::Format::Xrgb8888 => state.topology.shm_xrgb8888 = true,
                _ => {}
            }
        }
    }
}

impl Dispatch<zwlr_layer_shell_v1::ZwlrLayerShellV1, ()> for WaylandState {
    fn event(
        _: &mut Self,
        _: &zwlr_layer_shell_v1::ZwlrLayerShellV1,
        _: zwlr_layer_shell_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_output::WlOutput, OutputUserData> for WaylandState {
    fn event(
        state: &mut Self,
        _: &wl_output::WlOutput,
        event: wl_output::Event,
        data: &OutputUserData,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        let Some(output) = state.topology.outputs.get_mut(&data.global_id) else {
            return;
        };
        match event {
            wl_output::Event::Geometry {
                transform: WEnum::Value(transform),
                ..
            } => output.transform = Some(transform),
            wl_output::Event::Mode {
                flags: WEnum::Value(flags),
                width,
                height,
                ..
            } if flags.contains(wl_output::Mode::Current) => {
                if let (Ok(width), Ok(height)) = (u32::try_from(width), u32::try_from(height)) {
                    output.pixel_size = Some(crate::geometry::Size::new(width, height));
                }
            }
            wl_output::Event::Scale { factor } if factor > 0 => output.scale = factor as u32,
            wl_output::Event::Name { name } => output.name = Some(name),
            wl_output::Event::Done => output.wl_done = true,
            _ => {}
        }
    }
}

impl Dispatch<zxdg_output_manager_v1::ZxdgOutputManagerV1, ()> for WaylandState {
    fn event(
        _: &mut Self,
        _: &zxdg_output_manager_v1::ZxdgOutputManagerV1,
        _: zxdg_output_manager_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<zxdg_output_v1::ZxdgOutputV1, u32> for WaylandState {
    fn event(
        state: &mut Self,
        _: &zxdg_output_v1::ZxdgOutputV1,
        event: zxdg_output_v1::Event,
        global_id: &u32,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        let Some(output) = state.topology.outputs.get_mut(global_id) else {
            return;
        };
        match event {
            zxdg_output_v1::Event::LogicalPosition { x, y } => {
                output.logical_position = Some(Point::new(x, y))
            }
            zxdg_output_v1::Event::LogicalSize { width, height } => {
                if let (Ok(width), Ok(height)) = (u32::try_from(width), u32::try_from(height)) {
                    output.logical_size = Some(crate::geometry::Size::new(width, height));
                }
            }
            zxdg_output_v1::Event::Name { name } if output.name.is_none() => {
                output.name = Some(name)
            }
            zxdg_output_v1::Event::Done => {}
            _ => {}
        }
    }
}

impl Dispatch<wl_seat::WlSeat, ()> for WaylandState {
    fn event(
        state: &mut Self,
        seat: &wl_seat::WlSeat,
        event: wl_seat::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        let wl_seat::Event::Capabilities { capabilities } = event else {
            return;
        };
        let WEnum::Value(capabilities) = capabilities else {
            return;
        };
        if capabilities.contains(wl_seat::Capability::Pointer) {
            if state.topology.pointer.is_none() {
                state.topology.pointer = Some(seat.get_pointer(qh, ()));
            }
            state.topology.pointer_capability = true;
        } else {
            state.topology.pointer_capability = false;
        }
        if capabilities.contains(wl_seat::Capability::Keyboard) {
            if state.topology.keyboard.is_none() {
                state.topology.keyboard = Some(seat.get_keyboard(qh, ()));
            }
            state.topology.keyboard_capability = true;
        } else {
            state.topology.keyboard_capability = false;
        }
        ensure_cursor_shape_device(state, qh);
    }
}

impl Dispatch<wl_pointer::WlPointer, ()> for WaylandState {
    fn event(
        state: &mut Self,
        _: &wl_pointer::WlPointer,
        event: wl_pointer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_pointer::Event::Enter {
                serial,
                surface,
                surface_x,
                surface_y,
                ..
            } => {
                state.topology.cursor_enter_serial = Some(serial);
                state.topology.cursor_shape = None;
                if state.selection_mode {
                    state.set_cursor_shape(selection_cursor_shape());
                }
                let Some(surface_data) = surface.data::<SurfaceUserData>() else {
                    if state.selection_mode || state.editor.is_some() {
                        state.fail(VshotError::WaylandProtocol(
                            "wl_pointer enter surface has no SurfaceUserData".into(),
                        ));
                    }
                    return;
                };
                state.pointer_output = Some(surface_data.output_id);
                state.pointer_position = Some((surface_data.output_id, surface_x, surface_y));
                if state.pointer_grab_output.is_some() {
                    // During an implicit grab, motion coordinates remain relative to
                    // the surface that received the button press. Do not overwrite
                    // the drag position with coordinates from a new pointer focus.
                    return;
                }
                let event = SelectionEvent::PointerMoved {
                    output_id: surface_data.output_id,
                    local_x: surface_x,
                    local_y: surface_y,
                };
                if state.editor.is_some() {
                    state.process_editor_event(event);
                    state.update_cursor_shape();
                    state.request_interaction_redraw();
                } else {
                    let should_redraw = state.process_selection_event(event);
                    if state.selection_mode && should_redraw {
                        state.request_interaction_redraw();
                    }
                }
            }
            wl_pointer::Event::Leave { surface, .. } => {
                state.topology.cursor_enter_serial = None;
                state.topology.cursor_shape = None;
                if state.pointer_grab_output.is_none() {
                    if let Some(surface_data) = surface.data::<SurfaceUserData>() {
                        if state.pointer_output == Some(surface_data.output_id) {
                            state.pointer_output = None;
                        }
                        if state
                            .pointer_position
                            .as_ref()
                            .map(|(output_id, _, _)| *output_id)
                            == Some(surface_data.output_id)
                        {
                            state.pointer_position = None;
                        }
                    }
                }
            }
            wl_pointer::Event::Motion {
                surface_x,
                surface_y,
                ..
            } => {
                let Some(output_id) = state.pointer_motion_output() else {
                    if state.selection_mode || state.editor.is_some() {
                        state.fail(VshotError::WaylandProtocol(
                            "wl_pointer motion has no focused or grabbed output".into(),
                        ));
                    }
                    return;
                };
                state.pointer_position = Some((output_id, surface_x, surface_y));
                let event = SelectionEvent::PointerMoved {
                    output_id,
                    local_x: surface_x,
                    local_y: surface_y,
                };
                if state.editor.is_some() {
                    state.process_editor_event(event);
                    state.update_cursor_shape();
                    state.request_interaction_redraw();
                } else {
                    let should_redraw = state.process_selection_event(event);
                    if state.selection_mode && should_redraw {
                        state.request_interaction_redraw();
                    }
                }
            }
            wl_pointer::Event::Button {
                button,
                state: WEnum::Value(button_state),
                time,
                ..
            } => {
                let pressed = matches!(button_state, wl_pointer::ButtonState::Pressed);
                if button == BTN_LEFT && pressed {
                    state.pointer_grab_output = state.pointer_output;
                }
                let event = SelectionEvent::ButtonWithTimestamp {
                    button,
                    pressed,
                    timestamp: time,
                };
                if state.editor.is_some() {
                    state.process_editor_event(event);
                    state.update_cursor_shape();
                    state.request_interaction_redraw();
                } else {
                    let should_redraw = state.process_selection_event(event);
                    if state.selection_mode && should_redraw {
                        state.request_interaction_redraw();
                    }
                }
                if button == BTN_LEFT && !pressed {
                    state.pointer_grab_output = None;
                }
            }
            wl_pointer::Event::Button {
                state: WEnum::Unknown(value),
                ..
            } => state.fail(VshotError::WaylandProtocol(format!(
                "unknown wl_pointer button state value {value}"
            ))),
            _ => {}
        }
    }
}

impl Dispatch<wl_keyboard::WlKeyboard, ()> for WaylandState {
    fn event(
        state: &mut Self,
        _: &wl_keyboard::WlKeyboard,
        event: wl_keyboard::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_keyboard::Event::Keymap {
                format: WEnum::Unknown(value),
                ..
            } if state.selection_mode || state.editor.is_some() => {
                state.fail(VshotError::WaylandProtocol(format!(
                    "interactive keyboard received unknown keymap format value {value}"
                )));
            }
            wl_keyboard::Event::Enter { surface, .. } => {
                let Some(surface_data) = surface.data::<SurfaceUserData>() else {
                    if state.selection_mode || state.editor.is_some() {
                        state.fail(VshotError::WaylandProtocol(
                            "interactive keyboard enter surface has no SurfaceUserData".into(),
                        ));
                    }
                    return;
                };
                state.keyboard_focus_output = Some(surface_data.output_id);
            }
            wl_keyboard::Event::Leave { surface, .. } => {
                let Some(surface_data) = surface.data::<SurfaceUserData>() else {
                    if state.selection_mode || state.editor.is_some() {
                        state.fail(VshotError::WaylandProtocol(
                            "interactive keyboard leave surface has no SurfaceUserData".into(),
                        ));
                    }
                    return;
                };
                if state.keyboard_focus_output == Some(surface_data.output_id) {
                    state.keyboard_focus_output = None;
                }
            }
            wl_keyboard::Event::Key {
                key,
                state: WEnum::Value(key_state),
                ..
            } => {
                let pressed = matches!(key_state, wl_keyboard::KeyState::Pressed);
                let key = decode_wayland_keycode(key);
                let event = SelectionEvent::Key { key, pressed };
                if state.editor.is_some() {
                    state.process_editor_event(event);
                    state.request_interaction_redraw();
                } else if key == KEY_ESC && pressed {
                    state.process_selection_event(event);
                }
            }
            wl_keyboard::Event::Key {
                state: WEnum::Unknown(value),
                ..
            } => state.fail(VshotError::WaylandProtocol(format!(
                "unknown wl_keyboard key state value {value}"
            ))),
            _ => {}
        }
    }
}

impl Dispatch<wl_surface::WlSurface, SurfaceUserData> for WaylandState {
    fn event(
        _: &mut Self,
        _: &wl_surface::WlSurface,
        _: wl_surface::Event,
        _: &SurfaceUserData,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<zwlr_layer_surface_v1::ZwlrLayerSurfaceV1, LayerSurfaceUserData> for WaylandState {
    fn event(
        state: &mut Self,
        layer_surface: &zwlr_layer_surface_v1::ZwlrLayerSurfaceV1,
        event: zwlr_layer_surface_v1::Event,
        data: &LayerSurfaceUserData,
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        match event {
            zwlr_layer_surface_v1::Event::Configure {
                serial,
                width,
                height,
            } => {
                layer_surface.ack_configure(serial);
                let Some(scene) = state.scene.clone() else {
                    state.fail(VshotError::WaylandProtocol(
                        "layer surface configured before scene installation".into(),
                    ));
                    return;
                };
                let Some(output) = scene.output(data.output_id).cloned() else {
                    state.fail(VshotError::TopologyChanged);
                    return;
                };
                if width != output.geometry.size.width || height != output.geometry.size.height {
                    state.fail(VshotError::UnsupportedOutput(format!(
                        "layer surface for output {} configured as {}x{}, expected {}x{}",
                        output.name,
                        width,
                        height,
                        output.geometry.size.width,
                        output.geometry.size.height
                    )));
                    return;
                }
                let Some(shm) = state.topology.shm.clone() else {
                    state.fail(VshotError::MissingCapability("wl_shm".into()));
                    return;
                };
                let Some(shm_format) = state.topology.shm_format() else {
                    state.fail(VshotError::MissingCapability(
                        "wl_shm ARGB8888 or XRGB8888 format".into(),
                    ));
                    return;
                };
                let mut slots = Vec::new();
                for _ in 0..2 {
                    let token = match state.next_buffer_token() {
                        Ok(token) => token,
                        Err(error) => {
                            state.fail(error);
                            return;
                        }
                    };
                    match ShmSlot::new(
                        &shm,
                        output.frame.size().width,
                        output.frame.size().height,
                        BufferUserData {
                            output_id: data.output_id,
                            token: BufferToken::Parent(token),
                        },
                        shm_format,
                        qh,
                    ) {
                        Ok(mut slot) => {
                            let result =
                                slot.render(&output.frame, None, output.geometry, output.scale);
                            if let Err(error) = result {
                                state.fail(error);
                                return;
                            }
                            slots.push(slot);
                        }
                        Err(error) => {
                            state.fail(error);
                            return;
                        }
                    }
                }
                let Some(overlay) = state.overlays.get_mut(&data.output_id) else {
                    state.fail(VshotError::TopologyChanged);
                    return;
                };
                overlay.configured = true;
                overlay.width = width;
                overlay.height = height;
                overlay.slots = slots;
                overlay.pending_parent_redraw = false;
                overlay.commit_slot(0);
                state.ready_outputs.insert(data.output_id);
                if state.selection_mode && state.selection.dragging() {
                    state.request_interaction_redraw();
                }
            }
            zwlr_layer_surface_v1::Event::Closed => {
                if let Some(overlay) = state.overlays.get_mut(&data.output_id) {
                    overlay.closed = true;
                }
                state.fail(VshotError::WaylandProtocol(format!(
                    "compositor closed frozen overlay for output {}",
                    data.output_id
                )));
            }
            _ => {}
        }
    }
}

impl Dispatch<wl_shm_pool::WlShmPool, PoolUserData> for WaylandState {
    fn event(
        _: &mut Self,
        _: &wl_shm_pool::WlShmPool,
        _: wl_shm_pool::Event,
        _: &PoolUserData,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_buffer::WlBuffer, BufferUserData> for WaylandState {
    fn event(
        state: &mut Self,
        _: &wl_buffer::WlBuffer,
        event: wl_buffer::Event,
        data: &BufferUserData,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if matches!(event, wl_buffer::Event::Release) {
            state.handle_buffer_release(*data);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{
        cursor_shape_for_handle, decode_wayland_keycode, selection_cursor_shape, RedrawTarget,
        SelectionEvent, SelectionResult, WaylandState,
    };
    use crate::geometry::{Point, Rect};
    use crate::wayland::input::ResizeHandle;
    use crate::wayland::input::{EditorState, BTN_LEFT, KEY_ESC};
    use wayland_protocols::wp::cursor_shape::v1::client::wp_cursor_shape_device_v1::Shape;

    #[test]
    fn pointer_motion_keeps_using_the_grabbed_output() {
        let mut state = WaylandState {
            pointer_output: Some(1),
            pointer_grab_output: Some(2),
            ..WaylandState::default()
        };

        assert_eq!(state.pointer_motion_output(), Some(2));
        state.pointer_output = Some(3);
        assert_eq!(state.pointer_motion_output(), Some(2));
        state.pointer_grab_output = None;
        assert_eq!(state.pointer_motion_output(), Some(3));
    }

    #[test]
    fn selection_redraw_targets_parent_without_editor() {
        let state = WaylandState {
            selection_mode: true,
            ..WaylandState::default()
        };
        assert_eq!(state.redraw_target(), RedrawTarget::ParentSelection);

        let state = WaylandState {
            selection_mode: false,
            editor: Some(EditorState::new(Rect::new(0, 0, 10, 10))),
            ..WaylandState::default()
        };
        assert_eq!(state.redraw_target(), RedrawTarget::ParentEditor);
    }

    #[test]
    fn editor_selection_none_does_not_fall_back_to_tracker_selection() {
        let mut state = WaylandState::default();
        state.process_selection_event(SelectionEvent::GlobalPointerMoved {
            point: Point::new(10, 10),
            timestamp: 1,
        });
        state.process_selection_event(SelectionEvent::ButtonWithTimestamp {
            button: BTN_LEFT,
            pressed: true,
            timestamp: 2,
        });
        state.process_selection_event(SelectionEvent::GlobalPointerMoved {
            point: Point::new(20, 20),
            timestamp: 3,
        });
        assert!(state.selection.selection().is_some());

        state.editor = Some(EditorState::new(Rect::new(0, 0, 100, 100)));

        assert_eq!(state.selection_for_render(), None);
    }

    #[test]
    fn preserves_wayland_raw_keyboard_codes() {
        assert_eq!(decode_wayland_keycode(1), 1); // Escape
        assert_eq!(decode_wayland_keycode(28), 28); // Enter
        assert_eq!(decode_wayland_keycode(103), 103); // Up
        assert_eq!(decode_wayland_keycode(106), 106); // Right
    }

    #[test]
    fn selection_uses_crosshair_cursor_shape() {
        assert_eq!(selection_cursor_shape(), Shape::Crosshair);
    }

    #[test]
    fn resize_handles_map_to_matching_cursor_shapes() {
        assert_eq!(cursor_shape_for_handle(ResizeHandle::Top), Shape::NsResize);
        assert_eq!(
            cursor_shape_for_handle(ResizeHandle::Bottom),
            Shape::NsResize
        );
        assert_eq!(cursor_shape_for_handle(ResizeHandle::Left), Shape::EwResize);
        assert_eq!(
            cursor_shape_for_handle(ResizeHandle::Right),
            Shape::EwResize
        );
        assert_eq!(
            cursor_shape_for_handle(ResizeHandle::TopLeft),
            Shape::NwseResize
        );
        assert_eq!(
            cursor_shape_for_handle(ResizeHandle::BottomRight),
            Shape::NwseResize
        );
        assert_eq!(
            cursor_shape_for_handle(ResizeHandle::TopRight),
            Shape::NeswResize
        );
        assert_eq!(
            cursor_shape_for_handle(ResizeHandle::BottomLeft),
            Shape::NeswResize
        );
        assert_eq!(cursor_shape_for_handle(ResizeHandle::Move), Shape::Move);
        assert_eq!(cursor_shape_for_handle(ResizeHandle::None), Shape::Default);
    }

    #[test]
    fn selection_keeps_the_first_terminal_outcome() {
        let mut cancelled = WaylandState::default();
        cancelled.process_selection_event(SelectionEvent::Key {
            key: KEY_ESC,
            pressed: true,
        });
        cancelled.process_selection_event(SelectionEvent::GlobalPointerMoved {
            point: Point::new(1, 1),
            timestamp: 1,
        });
        cancelled.process_selection_event(SelectionEvent::ButtonWithTimestamp {
            button: BTN_LEFT,
            pressed: true,
            timestamp: 2,
        });
        assert_eq!(
            cancelled.selection_outcome,
            Some(SelectionResult::Cancelled)
        );

        let mut completed = WaylandState::default();
        completed.process_selection_event(SelectionEvent::GlobalPointerMoved {
            point: Point::new(1, 1),
            timestamp: 1,
        });
        completed.process_selection_event(SelectionEvent::ButtonWithTimestamp {
            button: BTN_LEFT,
            pressed: true,
            timestamp: 2,
        });
        completed.process_selection_event(SelectionEvent::GlobalPointerMoved {
            point: Point::new(4, 5),
            timestamp: 3,
        });
        completed.process_selection_event(SelectionEvent::ButtonWithTimestamp {
            button: BTN_LEFT,
            pressed: false,
            timestamp: 4,
        });
        completed.process_selection_event(SelectionEvent::Key {
            key: KEY_ESC,
            pressed: true,
        });
        assert_eq!(
            completed.selection_outcome,
            Some(SelectionResult::Completed(Rect::new(1, 1, 4, 5)))
        );
    }

    #[test]
    fn selection_redraws_only_after_drag_state_changes() {
        let mut state = WaylandState::default();
        assert!(
            !state.process_selection_event(SelectionEvent::GlobalPointerMoved {
                point: Point::new(1, 1),
                timestamp: 1,
            })
        );
        assert!(
            state.process_selection_event(SelectionEvent::ButtonWithTimestamp {
                button: BTN_LEFT,
                pressed: true,
                timestamp: 2,
            })
        );
        state.request_interaction_redraw();
        assert!(
            state.process_selection_event(SelectionEvent::GlobalPointerMoved {
                point: Point::new(4, 5),
                timestamp: 3,
            })
        );
        state.request_interaction_redraw();
        assert!(
            !state.process_selection_event(SelectionEvent::ButtonWithTimestamp {
                button: BTN_LEFT,
                pressed: false,
                timestamp: 4,
            })
        );

        assert!(state.take_interaction_redraw());
        assert!(!state.take_interaction_redraw());
        assert_eq!(
            state.selection_outcome,
            Some(SelectionResult::Completed(Rect::new(1, 1, 4, 5)))
        );
    }

    #[test]
    fn clearing_overlays_resets_keyboard_focus_and_cursor_focus() {
        let mut state = WaylandState {
            keyboard_focus_output: Some(7),
            ..WaylandState::default()
        };
        state.topology.cursor_enter_serial = Some(9);
        state.topology.cursor_shape = Some(Shape::Pointer);

        state.clear_overlays();

        assert_eq!(state.keyboard_focus_output, None);
        assert_eq!(state.topology.cursor_enter_serial, None);
        assert_eq!(state.topology.cursor_shape, None);
    }
}

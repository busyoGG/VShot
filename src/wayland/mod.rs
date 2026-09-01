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
use wayland_protocols::xdg::xdg_output::zv1::client::{zxdg_output_manager_v1, zxdg_output_v1};
use wayland_protocols_wlr::layer_shell::v1::client::{zwlr_layer_shell_v1, zwlr_layer_surface_v1};

use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect, Size};
use crate::model::SceneSnapshot;

use self::freeze_overlay::{
    BufferUserData, LayerSurfaceUserData, OverlaySurface, PoolUserData, ShmSlot, SurfaceUserData,
};
use self::input::{
    Annotation, EditorState, EditorTool, SelectionEvent, SelectionResult, SelectionTracker,
    ToolbarLayout, BTN_LEFT, KEY_ESC,
};
use self::topology::{OutputData, OutputInfo, OutputUserData, TopologyState};

const INTERACTION_TIMEOUT: Duration = Duration::from_secs(30);

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
    keyboard_focus_output: Option<u32>,
    selection: SelectionTracker,
    selection_mode: bool,
    editor: Option<EditorState>,
    selection_outcome: Option<SelectionResult>,
    interaction_redraw_pending: bool,
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

    fn clear_overlays(&mut self) {
        self.editor = None;
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
        let redraw =
            self.selection.dragging() || matches!(&result, Ok(SelectionResult::Completed(_)));
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

    fn redraw_all(&mut self) {
        let Some(scene) = self.scene.clone() else {
            self.fail(VshotError::WaylandProtocol(
                "cannot redraw an overlay before a frozen scene is installed".into(),
            ));
            return;
        };
        let selection = self
            .editor
            .as_ref()
            .and_then(EditorState::selection)
            .or_else(|| self.selection.selection());
        let editor_snapshot = self.editor.clone();
        let editor = editor_snapshot.as_ref();
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
                overlay.pending_redraw = true;
                continue;
            };
            if let Err(error) = overlay.slots[slot_index].render_editor(
                &output.frame,
                selection,
                output.geometry,
                output.scale,
                editor,
                scene_bounds,
            ) {
                self.fail(error);
                continue;
            }
            overlay.pending_redraw = false;
            overlay.commit_slot(slot_index);
        }
    }

    fn handle_buffer_release(&mut self, data: BufferUserData) {
        let mut redraw = false;
        if let Some(overlay) = self.overlays.get_mut(&data.output_id) {
            if let Some(slot) = overlay.slots.get_mut(data.slot) {
                slot.available = true;
            }
            redraw = overlay.pending_redraw;
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

    pub fn set_scene(&mut self, scene: SceneSnapshot) {
        self.state.scene = Some(scene);
    }

    pub fn show_frozen(&mut self, selection_mode: bool) -> Result<()> {
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
                    pending_redraw: false,
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

    pub fn wait_for_current_output(&mut self) -> Result<u32> {
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
                self.state.redraw_all();
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

    pub fn select_region(&mut self) -> Result<crate::geometry::Rect> {
        let deadline = Instant::now() + INTERACTION_TIMEOUT;
        loop {
            if let Some(outcome) = self.state.selection_outcome.take() {
                return match outcome {
                    SelectionResult::Completed(rect) => Ok(rect),
                    SelectionResult::Cancelled => Err(VshotError::SelectionCancelled),
                    SelectionResult::Continue => continue,
                };
            }
            if let Some(error) = self.state.error.take() {
                return Err(error);
            }
            if self.state.topology.topology_changed {
                return Err(VshotError::TopologyChanged);
            }
            if Instant::now() >= deadline {
                return Err(VshotError::SelectionTimeout);
            }
            self.dispatch_until(deadline, VshotError::SelectionTimeout, |state| {
                state.selection_outcome.is_some()
            })?;
        }
    }

    pub fn start_editor(&mut self, selection: Rect) -> Result<()> {
        let bounds = self
            .state
            .scene
            .as_ref()
            .ok_or_else(|| VshotError::WaylandProtocol("editor requires a scene".into()))?
            .bounds();
        let mut editor = EditorState::with_selection(bounds, selection)?;
        if let Some(point) = self.state.selection.current_point() {
            editor.set_pointer(point);
        }
        let tools = [
            EditorTool::Select,
            EditorTool::Pen,
            EditorTool::Rectangle,
            EditorTool::Ellipse,
            EditorTool::Arrow,
            EditorTool::Text,
            EditorTool::Blur,
        ];
        let item_size = Size::new(34, 30);
        let toolbar_width = item_size
            .width
            .checked_mul(tools.len() as u32)
            .and_then(|width| width.checked_add(6 * (tools.len() as u32 - 1)))
            .unwrap_or(item_size.width);
        let bounds_right = bounds.right()?;
        let selection_bottom = selection.bottom()?;
        let toolbar_x = selection
            .left()
            .max(bounds.left())
            .min(bounds_right.saturating_sub(toolbar_width as i32));
        let toolbar_y = if selection.top() - 38 >= bounds.top() {
            selection.top() - 36
        } else {
            selection_bottom.saturating_add(6)
        };
        editor.set_toolbar(ToolbarLayout::horizontal(
            Point::new(toolbar_x, toolbar_y),
            item_size,
            6,
            &tools,
        ));
        self.state.editor = Some(editor);
        self.state.selection_mode = false;
        self.state.redraw_all();
        if let Some(error) = self.state.error.take() {
            return Err(error);
        }
        Ok(())
    }

    pub fn edit_region(&mut self) -> Result<(Rect, Vec<Annotation>)> {
        let deadline = Instant::now() + INTERACTION_TIMEOUT;
        loop {
            if let Some(error) = self.state.error.take() {
                return Err(error);
            }
            if self.state.topology.topology_changed {
                return Err(VshotError::TopologyChanged);
            }
            let Some(editor) = self.state.editor.as_ref() else {
                return Err(VshotError::WaylandProtocol("editor is not active".into()));
            };
            if editor.is_confirmed() {
                let mut editor = self.state.editor.take().ok_or_else(|| {
                    VshotError::WaylandProtocol("editor state disappeared".into())
                })?;
                let selection = editor.selection().ok_or_else(|| {
                    VshotError::Selection("cannot save an empty selection".into())
                })?;
                return Ok((selection, editor.take_annotations()));
            }
            if editor.is_cancelled() {
                return Err(VshotError::SelectionCancelled);
            }
            if Instant::now() >= deadline {
                return Err(VshotError::EditorTimeout);
            }
            self.dispatch_until(deadline, VshotError::EditorTimeout, |state| {
                state
                    .editor
                    .as_ref()
                    .is_some_and(|editor| editor.is_confirmed() || editor.is_cancelled())
            })?;
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
    if topology.seats.is_empty() {
        return Err(VshotError::MissingCapability("wl_seat".into()));
    }
    if !topology.pointer_capability || topology.pointer.is_none() {
        return Err(VshotError::MissingCapability("seat pointer".into()));
    }
    if !topology.keyboard_capability || topology.keyboard.is_none() {
        return Err(VshotError::MissingCapability("seat keyboard".into()));
    }
    Ok(())
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
                surface,
                surface_x,
                surface_y,
                ..
            } => {
                let Some(surface_data) = surface.data::<SurfaceUserData>() else {
                    if state.selection_mode || state.editor.is_some() {
                        state.fail(VshotError::WaylandProtocol(
                            "wl_pointer enter surface has no SurfaceUserData".into(),
                        ));
                    }
                    return;
                };
                state.pointer_output = Some(surface_data.output_id);
                let event = SelectionEvent::PointerMoved {
                    output_id: surface_data.output_id,
                    local_x: surface_x,
                    local_y: surface_y,
                };
                if state.editor.is_some() {
                    state.process_editor_event(event);
                    state.request_interaction_redraw();
                } else {
                    let should_redraw = state.process_selection_event(event);
                    if state.selection_mode && should_redraw {
                        state.request_interaction_redraw();
                    }
                }
            }
            wl_pointer::Event::Leave { surface, .. } => {
                if state.pointer_grab_output.is_none() {
                    if let Some(surface_data) = surface.data::<SurfaceUserData>() {
                        if state.pointer_output == Some(surface_data.output_id) {
                            state.pointer_output = None;
                        }
                    }
                }
            }
            wl_pointer::Event::Motion {
                surface_x,
                surface_y,
                ..
            } => {
                let Some(output_id) = state.pointer_output.or(state.pointer_grab_output) else {
                    if state.selection_mode || state.editor.is_some() {
                        state.fail(VshotError::WaylandProtocol(
                            "wl_pointer motion has no focused or grabbed output".into(),
                        ));
                    }
                    return;
                };
                let event = SelectionEvent::PointerMoved {
                    output_id,
                    local_x: surface_x,
                    local_y: surface_y,
                };
                if state.editor.is_some() {
                    state.process_editor_event(event);
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
                for slot_index in 0..2 {
                    match ShmSlot::new(
                        &shm,
                        output.frame.size().width,
                        output.frame.size().height,
                        data.output_id,
                        slot_index,
                        shm_format,
                        qh,
                    ) {
                        Ok(mut slot) => {
                            if let Err(error) =
                                slot.render(&output.frame, None, output.geometry, output.scale)
                            {
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
                overlay.commit_slot(0);
                state.ready_outputs.insert(data.output_id);
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
    use super::{decode_wayland_keycode, SelectionEvent, SelectionResult, WaylandState};
    use crate::geometry::{Point, Rect};
    use crate::wayland::input::{BTN_LEFT, KEY_ESC};

    #[test]
    fn preserves_wayland_raw_keyboard_codes() {
        assert_eq!(decode_wayland_keycode(1), 1); // Escape
        assert_eq!(decode_wayland_keycode(28), 28); // Enter
        assert_eq!(decode_wayland_keycode(103), 103); // Up
        assert_eq!(decode_wayland_keycode(106), 106); // Right
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
            state.process_selection_event(SelectionEvent::ButtonWithTimestamp {
                button: BTN_LEFT,
                pressed: false,
                timestamp: 4,
            })
        );
        state.request_interaction_redraw();

        assert!(state.take_interaction_redraw());
        assert!(!state.take_interaction_redraw());
        assert_eq!(
            state.selection_outcome,
            Some(SelectionResult::Completed(Rect::new(1, 1, 4, 5)))
        );
    }

    #[test]
    fn clearing_overlays_resets_keyboard_focus() {
        let mut state = WaylandState {
            keyboard_focus_output: Some(7),
            ..WaylandState::default()
        };

        state.clear_overlays();

        assert_eq!(state.keyboard_focus_output, None);
    }
}

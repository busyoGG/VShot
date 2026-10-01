// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

pub mod freeze_overlay;
pub mod input;
pub mod topology;

use std::collections::{HashMap, HashSet};
use std::os::fd::BorrowedFd;
use std::sync::Arc;
use std::time::{Duration, Instant};

use wayland_client::protocol::{
    wl_buffer, wl_compositor, wl_keyboard, wl_output, wl_pointer, wl_region, wl_registry, wl_seat,
    wl_shm, wl_shm_pool, wl_surface,
};
use wayland_client::{Connection, Dispatch, EventQueue, Proxy, QueueHandle, WEnum};
use wayland_protocols::wp::color_management::v1::client::wp_color_manager_v1::TransferFunction;
use wayland_protocols::wp::color_management::v1::client::{
    wp_color_management_output_v1, wp_color_management_surface_v1, wp_color_manager_v1,
    wp_image_description_info_v1, wp_image_description_v1,
};
use wayland_protocols::wp::cursor_shape::v1::client::{
    wp_cursor_shape_device_v1, wp_cursor_shape_manager_v1,
};
use wayland_protocols::wp::linux_dmabuf::zv1::client::{
    zwp_linux_buffer_params_v1, zwp_linux_dmabuf_v1,
};
use wayland_protocols::xdg::xdg_output::zv1::client::{zxdg_output_manager_v1, zxdg_output_v1};
use wayland_protocols_wlr::layer_shell::v1::client::{zwlr_layer_shell_v1, zwlr_layer_surface_v1};

use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect};
use crate::model::{HdrFrame, Primaries, SceneSnapshot, Transfer};

use self::freeze_overlay::{
    release_buffer_if_current, BufferToken, BufferUserData, LayerSurfaceUserData, OverlayColor,
    OverlaySurface, PinSlot, PoolUserData, ShmSlot, SurfaceUserData,
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

/// One output that was given a colour description, as the HDR pin helper needs
/// it: the gamut the output's own description names, the light one unit of
/// content stands for there, and whether the output is an HDR one at all.  A
/// surface shows codes written in that gamut and against that white, so a pin
/// from elsewhere — or a plain sRGB one — has to be written in both before it
/// goes on; and a PQ surface only means what it says on an output whose own
/// curve is PQ, so the third says whether to draw one at all.
pub type PinOutputColor = (String, Option<Primaries>, Option<f32>, bool);

/// One output's HDR half, as a backdrop surface needs it: the frozen frame and
/// the light level its `1.0` stands for — the output's own SDR white — which is
/// what turns the frame back into the absolute PQ codes the surface shows.
#[derive(Clone, Debug)]
pub struct BackdropFrame {
    pub name: String,
    pub frame: HdrFrame,
    pub reference_nits: f32,
}

/// One output's backdrop pixels, encoded once.  A backdrop never re-renders,
/// so the surface's two buffers share the same words rather than encoding the
/// frame twice.
#[derive(Clone, Debug)]
struct HdrBackdrop {
    words: Arc<Vec<u32>>,
}

/// One output's picture buffer, as the helper allocated and drew it: the
/// descriptor facts a `wl_buffer` needs, and the size the compositor should read
/// from it.  The descriptor stays the helper's; this side only borrows it for
/// the length of the `add` request.
#[derive(Clone, Copy, Debug)]
pub struct PinBufferSpec {
    pub fd: std::os::fd::RawFd,
    pub offset: u32,
    pub stride: u32,
    pub modifier: u64,
    pub format: u32,
    pub width: u32,
    pub height: u32,
}

/// One output's own image description, asked for and ready, waiting for the
/// commit that puts it on a surface.
#[derive(Debug)]
struct PendingColor {
    output: wp_color_management_output_v1::WpColorManagementOutputV1,
    description: wp_image_description_v1::WpImageDescriptionV1,
}

/// What one image description's `wp_image_description_info_v1` has said so far.
/// The events arrive one field at a time and end with `done`, so this is what
/// they are collected into.
#[derive(Debug, Default)]
struct ColorInfo {
    /// The gamut read from the description's chromaticity coordinates.
    primaries: Option<Primaries>,
    /// The light one unit of content stands for, from the description's
    /// `luminances`.  This is the level an SDR picture drawn on this output has
    /// to be encoded against: its `1.0` is the output's own SDR white, not
    /// BT.2408's 203 cd/m², and a code written against the wrong one comes out
    /// at the wrong light.
    reference_nits: Option<f32>,
    /// The transfer function the description names, from its `tf_named` or its
    /// `tf_power`.  This is the one field that says whether the output is an
    /// HDR one: an output in PQ or HLG is showing HDR, and an output in the
    /// sRGB curve is not, whatever its gamut or its white.  A description that
    /// names no curve — including one that names it as a `tf_curve`, which is
    /// left unread — leaves this `None` rather than assuming one; see
    /// [`WaylandSession::apply_pin_color`].
    transfer: Option<Transfer>,
}

/// The gamut a description's chromaticities describe, in the millionth-unit
/// integers the protocol carries them as.
///
/// The coordinates are taken as they are: a set this pipeline has a name for
/// reads as that name, and any other gamut keeps the matrix its coordinates
/// imply instead of being read as BT.709, which would shift every colour of a
/// Display P3 or EDID-only output.
fn primaries_of(r_x: i32, r_y: i32, g_x: i32, g_y: i32, b_x: i32, b_y: i32) -> Primaries {
    const SCALE: f32 = 1_000_000.0;
    Primaries::from_chromaticities(
        (r_x as f32 / SCALE, r_y as f32 / SCALE),
        (g_x as f32 / SCALE, g_y as f32 / SCALE),
        (b_x as f32 / SCALE, b_y as f32 / SCALE),
    )
}

/// The transfer function one of the protocol's names stands for, or `None` for
/// a curve this pipeline has no name for.
///
/// The three that matter are the ones that decide what a surface's pixels mean:
/// PQ and HLG are the HDR curves, and everything the protocol calls an SDR curve
/// — `srgb`, `ext_srgb`, `bt1886`, `gamma22`, `compound_power_2_4` — is the SDR
/// one, since they differ only in the gamma below the point where a pin's codes
/// are decided.  A name this does not know is left unknown rather than folded
/// into the nearest one: reading an unfamiliar curve as sRGB is exactly the
/// mistake that shows a PQ buffer as a dark SDR picture.
fn transfer_of_named(named: TransferFunction) -> Option<Transfer> {
    match named {
        TransferFunction::St2084Pq => Some(Transfer::Pq),
        TransferFunction::Hlg => Some(Transfer::Hlg),
        TransferFunction::ExtLinear => Some(Transfer::Linear),
        TransferFunction::Srgb
        | TransferFunction::ExtSrgb
        | TransferFunction::Bt1886
        | TransferFunction::Gamma22
        | TransferFunction::CompoundPower24 => Some(Transfer::Srgb),
        _ => None,
    }
}

/// Whether an output whose description named `transfer` is showing HDR.
///
/// Only the curve answers this: PQ and HLG are the HDR ones, and an output in
/// any SDR curve is an SDR output whatever its gamut or its white.  An output
/// whose description never named a curve at all counts as HDR, which is what the
/// pin helper assumed before it could read the curve — a compositor that does
/// not answer the question keeps the behaviour it had rather than losing its
/// pins.
fn is_hdr(transfer: Option<Transfer>) -> bool {
    match transfer {
        Some(transfer) => matches!(transfer, Transfer::Pq | Transfer::Hlg),
        None => true,
    }
}

#[derive(Debug, Default)]
struct WaylandState {
    topology: TopologyState,
    scene: Option<SceneSnapshot>,
    overlays: HashMap<u32, OverlaySurface>,
    /// Set while the overlays are an HDR backdrop rather than the SDR freeze
    /// editor: the pixels to show, keyed by output id.  A backdrop surface sits
    /// on the top layer, below the Qt helper's overlay layer, hands the pointer
    /// nothing, and carries the output's own colour description.
    hdr_backdrop: HashMap<u32, HdrBackdrop>,
    /// Output ids whose overlays are the pin daemon's HDR image surfaces rather
    /// than a frozen frame.  They carry no scene: every buffer starts as
    /// transparent ten-bit words, and [`WaylandSession::show_pin_image`] puts an
    /// image into one of them later.
    pin_surfaces: HashSet<u32>,
    /// Output ids whose image description came back `ready`, and those whose
    /// query failed, between `get_image_description` and the flood of events.
    cm_ready: HashSet<u32>,
    cm_failed: HashSet<u32>,
    /// The gamut each output's own description names, as its
    /// `wp_image_description_info_v1` reported it, keyed by output global id.
    /// A pin's codes are written in the gamut of the output they were captured
    /// on, so this is what says whether a pin dragged onto another screen has
    /// to be re-encoded before it is uploaded.
    cm_gamut: HashMap<u32, Primaries>,
    /// The light one unit of content stands for on each output, from the same
    /// description.  A picture that is *not* HDR is drawn on this output's
    /// surface, so its codes have to be written against this level rather than
    /// a fixed one.
    cm_white: HashMap<u32, f32>,
    /// The transfer function each output's own description names, from the same
    /// description.  An output whose curve is PQ or HLG is showing HDR and can
    /// take a pin's PQ codes as they are; one whose curve is the sRGB curve is
    /// an SDR output, and a PQ surface there is read as sRGB — which is what
    /// made every pin on an SDR output dark and its highlights clip.
    cm_transfer: HashMap<u32, Transfer>,
    /// The description being read, between `get_information` and its `done`,
    /// keyed by the output the answer belongs to.
    cm_info: HashMap<u32, ColorInfo>,
    /// Descriptions that came back ready and are waiting for
    /// [`WaylandSession::apply_surface_color`] to commit them.
    pending_color: HashMap<u32, PendingColor>,
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
        self.hdr_backdrop.clear();
        self.pin_surfaces.clear();
        self.ready_outputs.clear();
        self.cm_ready.clear();
        self.cm_failed.clear();
        self.cm_gamut.clear();
        self.cm_white.clear();
        self.cm_transfer.clear();
        self.cm_info.clear();
        self.pending_color.clear();
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
                // A picture buffer coming back is what lets the helper's next
                // compose go out; nothing is queued here, because the helper
                // offers its waiting picture again on the very next wake-up.
                BufferToken::Pin(_) => {
                    overlay.release_pin_slot(data.token);
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
        // The output mapping is deliberately not validated here.  Reading it is
        // what fails on an output this side cannot compose into a scene
        // (rotated or flipped), and that is a property of the *scene* routes:
        // a compositor that draws the focused window itself — KWin, niri — never
        // needs an output, and refusing it in `connect` would take away the one
        // route that still works there.  Every route that does need the
        // topology asks for it, and gets the same diagnosis from there.
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
                    pin_slots: Vec::new(),
                    pending_parent_redraw: false,
                    color: None,
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

    /// Shows each output's HDR frame on a backdrop surface, so the frozen
    /// picture behind the Qt helper's overlay is the light the screen showed
    /// rather than its SDR map.
    ///
    /// The surface carries the **output's own** colour description.  That is
    /// what makes the compositor hand the pixels through untouched: it is the
    /// very description the monitor itself uses, so there is nothing to convert
    /// — and, the point of it, nothing to tone-map.  A description that merely
    /// resembles the output's, which is all a `QSurfaceFormat` colour space
    /// gives Qt, is a different one, and the compositor tone-maps it into the
    /// panel's own range, dimming the whole picture instead of showing it.
    ///
    /// The surfaces sit on the top layer — below the helper's overlay layer,
    /// above every window — and take no input.  The answer is the outputs that
    /// really carry an HDR backdrop, by name: an empty list means none was
    /// wanted or possible, and the caller keeps its SDR overlay.  An output
    /// whose description never became ready is left out of the list as well —
    /// its ten-bit buffer would be read as sRGB, far darker than the screen, and
    /// the caller draws its own SDR frame there instead.
    pub fn show_hdr_backdrop(&mut self, frames: &[BackdropFrame]) -> Result<Vec<String>> {
        if frames.is_empty() || self.state.topology.color_manager.is_none() {
            return Ok(Vec::new());
        }
        if self.state.topology.shm_format_10bit().is_none() {
            return Ok(Vec::new());
        }
        let scene = self.state.scene.clone().ok_or_else(|| {
            VshotError::WaylandProtocol("cannot show a backdrop without a scene snapshot".into())
        })?;
        let infos = self.state.topology.output_infos()?;
        if infos.len() != scene.outputs().len() {
            return Err(VshotError::TopologyChanged);
        }
        let mut wanted = HashMap::new();
        for info in &infos {
            if let Some(frame) = frames.iter().find(|frame| frame.name == info.name) {
                // Encoded once: a backdrop never re-renders, and the surface's
                // two buffers share the words.  The codes are written in the
                // frame's **own** primaries, which are the output's — the very
                // ones the description this surface carries names — so a
                // wide-gamut output's picture is not silently re-read as
                // BT.2020.  Encoding into BT.2020 while declaring an EDID-only
                // description is what shifted every colour on a P3-like panel.
                wanted.insert(
                    info.global_id,
                    HdrBackdrop {
                        words: Arc::new(
                            frame
                                .frame
                                .to_rgb10_pq_in(frame.frame.primaries(), frame.reference_nits),
                        ),
                    },
                );
            }
        }
        if wanted.is_empty() {
            return Ok(Vec::new());
        }
        if !self.state.overlays.is_empty() {
            self.destroy_overlays()?;
        }
        self.state.selection.reset();
        self.state.pointer_output = None;
        self.state.pointer_grab_output = None;
        self.state.keyboard_focus_output = None;
        self.state.selection_mode = false;
        self.state.editor = None;
        self.state.interaction_redraw_pending = false;
        self.state.selection_outcome = None;
        self.state.ready_outputs.clear();
        self.state.cm_ready.clear();
        self.state.cm_failed.clear();
        self.state.cm_gamut.clear();
        self.state.error = None;
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

        // Only the outputs that carry a frame get a surface: another output's
        // own overlay is drawn by the helper, and a second, undescribed copy
        // here would be a duplicate render of the same picture.
        for info in infos
            .iter()
            .filter(|info| wanted.contains_key(&info.global_id))
        {
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
                // Below the helper's overlay layer, above every window.
                zwlr_layer_shell_v1::Layer::Top,
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
            // A picture takes no input: the helper's overlay layer is above
            // this one and owns the pointer and the keyboard.
            layer_surface
                .set_keyboard_interactivity(zwlr_layer_surface_v1::KeyboardInteractivity::None);
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
                    pin_slots: Vec::new(),
                    pending_parent_redraw: false,
                    color: None,
                },
            );
        }

        self.state.hdr_backdrop = wanted;
        self.dispatch_until(
            Instant::now() + Duration::from_secs(10),
            VshotError::OverlayTimeout,
            |state| state.ready_outputs.len() == state.overlays.len(),
        )?;
        let colored = self.attach_backdrop_color()?;
        if let Some(error) = self.state.error.take() {
            return Err(error);
        }
        if self.state.topology.topology_changed {
            return Err(VshotError::TopologyChanged);
        }
        Ok(infos
            .iter()
            .filter(|info| colored.contains(&info.global_id))
            .map(|info| info.name.clone())
            .collect())
    }

    /// Gives each backdrop surface its output's own colour description, and
    /// answers which outputs got one.
    ///
    /// The objects are created on this connection — a surface's description has
    /// to come from the same client — and kept on the overlay so they outlive
    /// the surface.  An output whose description never became ready is left off
    /// the answer: its surface reads as sRGB, which would show a PQ buffer far
    /// darker than the screen, so the caller paints the SDR frame there instead.
    fn attach_backdrop_color(&mut self) -> Result<HashSet<u32>> {
        let ids = self
            .state
            .hdr_backdrop
            .keys()
            .copied()
            .collect::<Vec<u32>>();
        self.request_surface_color(&ids)?;
        self.apply_surface_color()
    }

    /// Asks each of `ids`' outputs for the description it publishes, keeps the
    /// ones that came back `ready`, and answers their ids.
    ///
    /// Nothing is set on a surface here: a description is applied on a commit,
    /// and the commit that attaches a surface's first buffer is the one that
    /// maps it — so the caller decides when the description goes on.  Splitting
    /// the ask from the set is what lets a pin surface have its description in
    /// place before its first picture is drawn.
    fn request_surface_color(&mut self, ids: &[u32]) -> Result<HashSet<u32>> {
        let mut ready = HashSet::new();
        if self.state.topology.color_manager.is_none() {
            return Ok(ready);
        }
        // The sets are per call: a stale entry from an earlier overlay would
        // make the wait below return before this call's events had arrived.
        self.state.cm_ready.clear();
        self.state.cm_failed.clear();
        self.state.pending_color.clear();
        let manager = self
            .state
            .topology
            .color_manager
            .clone()
            .expect("checked above");
        let qh = self.event_queue.handle();
        let mut pending = Vec::new();
        for global_id in ids.iter().copied() {
            let Some(output) = self.state.topology.output_proxies.get(&global_id).cloned() else {
                continue;
            };
            let color_output = manager.get_output(&output, &qh, global_id);
            let description = color_output.get_image_description(&qh, global_id);
            pending.push((global_id, color_output, description));
        }
        if pending.is_empty() {
            return Ok(ready);
        }
        let expected = pending.len();
        self.dispatch_until(
            Instant::now() + Duration::from_secs(5),
            VshotError::OverlayTimeout,
            |state| state.cm_ready.len() + state.cm_failed.len() >= expected,
        )?;
        // The gamut each description names, read from the description itself:
        // an output's own description is exactly what a pin surface is given,
        // and a pin's codes are written in the gamut of the output they came
        // from, so this is what a pin dragged onto another screen has to be
        // re-encoded into.
        self.read_description_gamuts(&pending);
        let debug = std::env::var_os("VSHOT_HDR_DEBUG").is_some();
        for (global_id, color_output, description) in pending {
            if !self.state.cm_ready.contains(&global_id) {
                if debug {
                    eprintln!(
                        "vshot: hdr: output {global_id} image description not ready; showing its \
                         SDR frame instead"
                    );
                }
                continue;
            }
            self.state.pending_color.insert(
                global_id,
                PendingColor {
                    output: color_output,
                    description,
                },
            );
            ready.insert(global_id);
        }
        Ok(ready)
    }

    /// Asks every ready description what gamut it names and keeps the answer.
    ///
    /// A description that never says is left out rather than guessed at: the
    /// caller then has no destination gamut to convert into and leaves the
    /// codes as they are, which is right whenever the two outputs agree — the
    /// case every pin is in unless the user drags one between screens.
    fn read_description_gamuts(
        &mut self,
        pending: &[(
            u32,
            wp_color_management_output_v1::WpColorManagementOutputV1,
            wp_image_description_v1::WpImageDescriptionV1,
        )],
    ) {
        let qh = self.event_queue.handle();
        self.state.cm_info.clear();
        let mut descriptions = Vec::new();
        for (global_id, _, description) in pending {
            if !self.state.cm_ready.contains(global_id) {
                continue;
            }
            self.state.cm_info.insert(*global_id, ColorInfo::default());
            // `global_id` is the info object's user data, which is how the
            // events below find the output they belong to.
            descriptions.push(description.get_information(&qh, *global_id));
        }
        if descriptions.is_empty() {
            return;
        }
        let expected = descriptions.len();
        let _ = self.dispatch_until(
            Instant::now() + Duration::from_secs(2),
            VshotError::OverlayTimeout,
            |state| state.cm_info.len() < expected,
        );
        // The objects are inert once their `done` has arrived, so they are
        // dropped here rather than kept alive for the life of the session.
        drop(descriptions);
        self.state.cm_info.clear();
    }

    /// Sets the descriptions [`WaylandSession::request_surface_color`] kept on
    /// their surfaces and commits, and answers the ids that got one.
    fn apply_surface_color(&mut self) -> Result<HashSet<u32>> {
        let mut colored = HashSet::new();
        let Some(manager) = self.state.topology.color_manager.clone() else {
            return Ok(colored);
        };
        let qh = self.event_queue.handle();
        let pending = std::mem::take(&mut self.state.pending_color);
        for (global_id, color) in pending {
            let Some(overlay) = self.state.overlays.get_mut(&global_id) else {
                continue;
            };
            let color_surface = manager.get_surface(&overlay.surface, &qh, ());
            color_surface.set_image_description(
                &color.description,
                wp_color_manager_v1::RenderIntent::Perceptual,
            );
            // The description is double-buffered, so the commit that already
            // attached the buffer has to be followed by another one for it to
            // take effect.
            overlay.surface.commit();
            overlay.color = Some(OverlayColor {
                _output: color.output,
                _description: color.description,
                _surface: color_surface,
            });
            colored.insert(global_id);
        }
        // Requests only leave on a flush, and nothing here dispatches again
        // before the caller draws: without this the compositor keeps the
        // surface's default (sRGB) description, which reads a PQ buffer as SDR
        // and shows a picture far darker than the screen's.
        self.event_queue
            .flush()
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        Ok(colored)
    }

    /// Creates one layer surface per output for pinned HDR images: an empty
    /// input region, and the output's own image description -- so the pixels
    /// drawn into it are shown as the light they stand for, with no conversion
    /// and no tone map in between.
    ///
    /// This is the backdrop without a frozen frame: nothing is captured, and the
    /// surfaces carry no buffer at all until
    /// [`WaylandSession::install_pin_buffers`] hands them the half-float dma-bufs
    /// the caller draws into.  It answers the names of the outputs whose image
    /// description came back `ready`; on any other output a half-float buffer
    /// would be read as sRGB, so the caller keeps its SDR picture there and
    /// installs no buffers.
    ///
    /// `layer` is the caller's to choose.  A pin belongs on `Overlay`, where the
    /// Qt pin surfaces are, and the compositor stacks the surfaces of one layer
    /// in the order they were mapped — so whoever maps these has to map them
    /// *before* the chrome that draws the badges and the menus.
    pub fn show_pin_surfaces(
        &mut self,
        layer: zwlr_layer_shell_v1::Layer,
        namespace: &str,
    ) -> Result<Vec<String>> {
        let Some(compositor) = self.state.topology.compositor.clone() else {
            return Err(VshotError::MissingCapability("wl_compositor".into()));
        };
        // No colour management means no output can be told what its pixels are,
        // so a half-float buffer here would be read as sRGB and look far darker
        // than the screen; there is nothing this helper could usefully show, and
        // no surface worth mapping.  The caller keeps its SDR picture.
        if self.state.topology.color_manager.is_none() {
            return Ok(Vec::new());
        }
        // A pinned HDR image is a half-float dma-buf, and both halves of that
        // have to be on offer: the protocol that names the buffer, and a layout
        // for the format the compose side allocates.
        if self.state.topology.dmabuf.is_none() {
            return Err(VshotError::MissingCapability("zwp_linux_dmabuf_v1".into()));
        }
        if self
            .state
            .topology
            .dmabuf_modifier_order(crate::pin_hdr_fp16::FORMAT_ABGR16161616F)
            .is_none()
        {
            return Err(VshotError::MissingCapability(
                "a dma-buf modifier for ABGR16161616F".into(),
            ));
        }
        let Some(layer_shell) = self.state.topology.layer_shell.clone() else {
            return Err(VshotError::MissingCapability("zwlr_layer_shell_v1".into()));
        };
        let infos = self.state.topology.output_infos()?;
        if !self.state.overlays.is_empty() {
            self.clear_overlays();
        }
        self.state.ready_outputs.clear();
        // No scene here, and none wanted: these surfaces are told which output
        // they are for, and their buffers arrive through `install_pin_buffers`.
        self.state.pin_surfaces.clear();
        let qh = self.event_queue.handle();
        for info in &infos {
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
                layer,
                namespace.to_string(),
                &qh,
                LayerSurfaceUserData {
                    output_id: info.global_id,
                },
            );
            layer_surface.set_size(0, 0);
            layer_surface.set_anchor(zwlr_layer_surface_v1::Anchor::all());
            layer_surface.set_exclusive_zone(-1);
            layer_surface
                .set_keyboard_interactivity(zwlr_layer_surface_v1::KeyboardInteractivity::None);
            // A picture takes no input, and this one sits above every window:
            // an empty input region is what keeps a click outside the pin
            // going to what is under it instead of to us.
            let region = compositor.create_region(&qh, ());
            surface.set_input_region(Some(&region));
            region.destroy();
            // No buffer yet: the compositor answers the first commit with the
            // configure that says how big the surface is, and a buffer before
            // that is a protocol error.
            surface.commit();

            self.state.pin_surfaces.insert(info.global_id);
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
                    pin_slots: Vec::new(),
                    pending_parent_redraw: false,
                    color: None,
                },
            );
        }
        self.dispatch_until(
            Instant::now() + Duration::from_secs(10),
            VshotError::OverlayTimeout,
            |state| state.ready_outputs.len() == state.overlays.len(),
        )?;
        let ids = infos
            .iter()
            .map(|info| info.global_id)
            .collect::<Vec<u32>>();
        // The description is asked for and kept now, but *set* only after the
        // caller has installed buffers: the compositor applies a surface's
        // description on a commit, and the commit that attaches the first buffer
        // is the one that maps the surface.
        let colored = self.request_surface_color(&ids)?;
        if let Some(error) = self.state.error.take() {
            return Err(error);
        }
        if self.state.topology.topology_changed {
            return Err(VshotError::TopologyChanged);
        }
        Ok(infos
            .iter()
            .filter(|info| colored.contains(&info.global_id))
            .map(|info| info.name.clone())
            .collect())
    }

    /// The layouts one format's picture buffer may be allocated in, most
    /// preferred first, or `None` when the compositor will not take the format.
    pub fn dmabuf_modifier_order(&self, fourcc: u32) -> Option<Vec<u64>> {
        self.state.topology.dmabuf_modifier_order(fourcc)
    }

    /// Builds one `wl_buffer` per entry of `specs` on `name`'s pin surface, and
    /// maps the surface with the first of them.
    ///
    /// The pixels are the caller's: it allocated the dma-bufs, and it has
    /// already drawn into the first one, because the commit here is what puts
    /// that buffer in front of the compositor.  Only the first buffer is
    /// committed; the others are held until [`WaylandSession::present_pin_buffer`]
    /// names them.
    ///
    /// `create_immed` can fail on the compositor's side without saying so, so
    /// this waits for the round trip that would carry the `failed` event and
    /// reports the refusal rather than leaving an unusable buffer behind.
    pub fn install_pin_buffers(&mut self, name: &str, specs: &[PinBufferSpec]) -> Result<()> {
        let Some(global_id) = self.output_id_of(name) else {
            return Err(VshotError::TopologyChanged);
        };
        let Some(dmabuf) = self.state.topology.dmabuf.clone() else {
            return Err(VshotError::MissingCapability("zwp_linux_dmabuf_v1".into()));
        };
        let qh = self.event_queue.handle();
        let mut slots = Vec::with_capacity(specs.len());
        for (index, spec) in specs.iter().enumerate() {
            let params = dmabuf.create_params(&qh, ());
            let borrowed = unsafe { BorrowedFd::borrow_raw(spec.fd) };
            params.add(
                borrowed,
                0,
                spec.offset,
                spec.stride,
                (spec.modifier >> 32) as u32,
                (spec.modifier & 0xffff_ffff) as u32,
            );
            let buffer = params.create_immed(
                i32::try_from(spec.width).unwrap_or(0),
                i32::try_from(spec.height).unwrap_or(0),
                spec.format,
                zwp_linux_buffer_params_v1::Flags::empty(),
                &qh,
                BufferUserData {
                    output_id: global_id,
                    token: BufferToken::Pin(index as u64),
                },
            );
            params.destroy();
            slots.push(PinSlot {
                buffer,
                token: BufferToken::Pin(index as u64),
                available: true,
            });
        }
        self.event_queue
            .flush()
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        self.event_queue
            .roundtrip(&mut self.state)
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        if let Some(error) = self.state.error.take() {
            return Err(error);
        }
        let Some(overlay) = self.state.overlays.get_mut(&global_id) else {
            return Err(VshotError::TopologyChanged);
        };
        overlay.pin_slots = slots;
        // The first buffer is committed now: it is what maps the surface, and the
        // caller has already drawn into it.  Its whole extent is damaged because
        // the compositor has never seen this surface before.
        let (width, height) = specs
            .first()
            .map(|spec| (spec.width.max(1), spec.height.max(1)))
            .unwrap_or((1, 1));
        if !overlay.present_pin_slot(0, Rect::new(0, 0, width, height)) {
            return Err(VshotError::PinSurface(
                "the pin surface would not take its first buffer".into(),
            ));
        }
        self.event_queue
            .flush()
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        Ok(())
    }

    /// Gives the pin surfaces their output's own colour description, now that
    /// they have buffers.  Answers each output that was given one, with the
    /// gamut its own description names, the light one unit of content stands for
    /// there, and whether the output is an HDR one.
    ///
    /// The surface carries the output's own description, so a PQ buffer written
    /// into it is passed through as the light it stands for — but only on an
    /// output whose own curve *is* PQ.  On an output described with the sRGB
    /// curve the same buffer is decoded as sRGB, which is what made every pin on
    /// an SDR output come out dark with its highlights clipped: the codes are
    /// PQ, the surface read them as if they were not.  The transfer function is
    /// therefore reported, and the caller keeps its SDR picture on an output
    /// that is not HDR — the same rule the capture side already follows in
    /// [`crate::capture::wlr`], where a 10-bit buffer is only decoded as HDR on
    /// an output whose description says PQ or HLG.
    ///
    /// An output whose description named no curve at all is reported as HDR:
    /// that is what the helper did before it could read the curve, so a
    /// compositor that does not answer the question keeps the behaviour it had
    /// rather than losing its pins.
    pub fn apply_pin_color(&mut self) -> Result<Vec<PinOutputColor>> {
        let colored = self.apply_surface_color()?;
        Ok(self
            .state
            .pin_surfaces
            .iter()
            .filter(|id| colored.contains(id))
            .filter_map(|id| {
                let name = self.state.topology.outputs.get(id)?.name.clone()?;
                let hdr = is_hdr(self.state.cm_transfer.get(id).copied());
                Some((
                    name,
                    self.state.cm_gamut.get(id).copied(),
                    self.state.cm_white.get(id).copied(),
                    hdr,
                ))
            })
            .collect())
    }

    /// Commits one picture buffer of `name`'s pin surface having changed only
    /// `damage` of it, in device pixels.  `false` means that slot was still with
    /// the compositor; the caller keeps what it wanted to show and offers it
    /// again after the next pump.
    pub fn present_pin_buffer(&mut self, name: &str, slot: usize, damage: Rect) -> Result<bool> {
        let Some(global_id) = self.output_id_of(name) else {
            return Ok(false);
        };
        let Some(overlay) = self.state.overlays.get_mut(&global_id) else {
            return Ok(false);
        };
        if !overlay.present_pin_slot(slot, damage) {
            return Ok(false);
        }
        self.event_queue
            .flush()
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        Ok(true)
    }

    /// Whether one of `name`'s picture buffers is free to be drawn into again.
    pub fn pin_slot_available(&self, name: &str, slot: usize) -> bool {
        let Some(global_id) = self.output_id_of(name) else {
            return false;
        };
        self.state
            .overlays
            .get(&global_id)
            .and_then(|overlay| overlay.pin_slots.get(slot))
            .is_some_and(|pin| pin.available)
    }

    /// The global id of the output called `name`.
    fn output_id_of(&self, name: &str) -> Option<u32> {
        self.state
            .topology
            .outputs
            .iter()
            .find(|(_, data)| data.name.as_deref() == Some(name))
            .map(|(global_id, _)| *global_id)
    }

    /// Pumps the connection for up to `timeout`: buffer releases, configure
    /// events, anything the compositor has to say.  A timeout is not an error.
    pub fn pump(&mut self, timeout: Duration) -> Result<()> {
        self.pump_watching(None, Some(timeout))
    }

    /// Pumps the connection until it or `extra` has something to say.
    ///
    /// A client that waits on its own socket as well as the compositor — the
    /// pin helper, whose next command and whose next buffer release are two
    /// different descriptors — otherwise has to wake on a timer and poll one of
    /// the two, which is a delay on every update and a spin on top.  Waiting on
    /// both at once costs nothing and wakes the moment either happens.
    ///
    /// `None` for `timeout` waits as long as it takes.
    pub fn pump_watching(
        &mut self,
        extra: Option<BorrowedFd<'_>>,
        timeout: Option<Duration>,
    ) -> Result<()> {
        self.event_queue
            .dispatch_pending(&mut self.state)
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        if self.state.take_interaction_redraw() {
            // Nothing here draws an editor, but the state machine may still ask.
        }
        if let Some(error) = self.state.error.take() {
            return Err(error);
        }
        if timeout.is_some_and(|timeout| timeout.is_zero()) {
            return Ok(());
        }
        let Some(read_guard) = self.event_queue.prepare_read() else {
            return Ok(());
        };
        let fd = read_guard.connection_fd();
        let mut poll_fds = Vec::with_capacity(2);
        poll_fds.push(rustix::event::PollFd::new(
            &fd,
            rustix::event::PollFlags::IN | rustix::event::PollFlags::ERR,
        ));
        if let Some(extra) = &extra {
            poll_fds.push(rustix::event::PollFd::new(
                extra,
                rustix::event::PollFlags::IN
                    | rustix::event::PollFlags::ERR
                    | rustix::event::PollFlags::HUP,
            ));
        }
        let timeout_spec = match timeout {
            Some(timeout) => Some(rustix::event::Timespec::try_from(timeout).map_err(|_| {
                VshotError::WaylandProtocol("Wayland timeout is out of range".into())
            })?),
            None => None,
        };
        let ready = rustix::event::poll(&mut poll_fds, timeout_spec.as_ref()).map_err(|error| {
            VshotError::WaylandProtocol(format!("failed to poll Wayland connection: {error}"))
        })?;
        let connection_ready = ready > 0
            && poll_fds[0]
                .revents()
                .intersects(rustix::event::PollFlags::IN | rustix::event::PollFlags::ERR);
        if !connection_ready {
            drop(read_guard);
            return Ok(());
        }
        read_guard
            .read()
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        self.event_queue
            .dispatch_pending(&mut self.state)
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        if let Some(error) = self.state.error.take() {
            return Err(error);
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
            .map_err(|error| VshotError::WaylandProtocol(error.to_string()))?;
        // Flushing only puts the destroy requests on the wire; the compositor
        // has not acted on them yet, and the next thing the caller may do is
        // read the desktop back through the compositor -- a scrolling capture,
        // which would otherwise grab the very surfaces being torn down.  A
        // round trip is what makes "destroyed" mean the compositor has already
        // re-rendered without them.
        self.event_queue
            .roundtrip(&mut self.state)
            .map(|_| ())
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
                }
                "wp_color_manager_v1" if state.topology.color_manager.is_none() => {
                    // Version 1 is enough: the backdrop only asks an output for
                    // its own image description and sets that on a surface.
                    state.topology.color_manager =
                        Some(registry.bind(name, version.min(1), qh, ()));
                }
                "zwp_linux_dmabuf_v1" if state.topology.dmabuf.is_none() => {
                    // Version 3 is where `modifier` arrives, and it is the last
                    // version that advertises the formats as a flat table this
                    // client does not have to chase through `feedback` objects.
                    state.topology.dmabuf = Some(registry.bind(name, version.min(3), qh, ()));
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

impl Dispatch<zwp_linux_buffer_params_v1::ZwpLinuxBufferParamsV1, ()> for WaylandState {
    fn event(
        state: &mut Self,
        _: &zwp_linux_buffer_params_v1::ZwpLinuxBufferParamsV1,
        event: zwp_linux_buffer_params_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        // `create_immed` answers nothing, so a refused buffer is only ever
        // reported here.  It is recorded as the session's error and named by
        // whichever call was waiting for the round trip.
        if let zwp_linux_buffer_params_v1::Event::Failed = event {
            state.fail(VshotError::PinSurface(
                "the compositor refused a pinned picture buffer".into(),
            ));
        }
    }
}

impl Dispatch<zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1, ()> for WaylandState {
    fn event(
        state: &mut Self,
        _: &zwp_linux_dmabuf_v1::ZwpLinuxDmabufV1,
        event: zwp_linux_dmabuf_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        // The flat format/modifier table: what a picture buffer may be made of.
        // A format with no modifier event yet is still worth keeping, because a
        // compositor that supports version 1 or 2 offers implicit modifiers only.
        match event {
            zwp_linux_dmabuf_v1::Event::Format { format } => {
                state.topology.dmabuf_formats.entry(format).or_default();
            }
            zwp_linux_dmabuf_v1::Event::Modifier {
                format,
                modifier_hi,
                modifier_lo,
            } => {
                let modifier = (u64::from(modifier_hi) << 32) | u64::from(modifier_lo);
                let list = state.topology.dmabuf_formats.entry(format).or_default();
                if !list.contains(&modifier) {
                    list.push(modifier);
                }
            }
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

impl Dispatch<wp_color_manager_v1::WpColorManagerV1, ()> for WaylandState {
    fn event(
        _: &mut Self,
        _: &wp_color_manager_v1::WpColorManagerV1,
        _: wp_color_manager_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        // The supported-feature and done events only describe what the
        // compositor can do; a backdrop only asks an output for its own
        // description, which every version of the protocol has.
    }
}

impl Dispatch<wp_color_management_output_v1::WpColorManagementOutputV1, u32> for WaylandState {
    fn event(
        _: &mut Self,
        _: &wp_color_management_output_v1::WpColorManagementOutputV1,
        _: wp_color_management_output_v1::Event,
        _: &u32,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        // An output that changes its description mid-session leaves the
        // backdrop with the one it was given; the next run asks again.
    }
}

impl Dispatch<wp_image_description_v1::WpImageDescriptionV1, u32> for WaylandState {
    fn event(
        state: &mut Self,
        _: &wp_image_description_v1::WpImageDescriptionV1,
        event: wp_image_description_v1::Event,
        data: &u32,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wp_image_description_v1::Event::Ready { .. } => {
                state.cm_ready.insert(*data);
            }
            wp_image_description_v1::Event::Failed { .. } => {
                state.cm_failed.insert(*data);
            }
            _ => {}
        }
    }
}

impl Dispatch<wp_image_description_info_v1::WpImageDescriptionInfoV1, u32> for WaylandState {
    fn event(
        state: &mut Self,
        _: &wp_image_description_info_v1::WpImageDescriptionInfoV1,
        event: wp_image_description_info_v1::Event,
        data: &u32,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        let Some(info) = state.cm_info.get_mut(data) else {
            return;
        };
        match event {
            wp_image_description_info_v1::Event::Primaries {
                r_x,
                r_y,
                g_x,
                g_y,
                b_x,
                b_y,
                ..
            } => info.primaries = Some(primaries_of(r_x, r_y, g_x, g_y, b_x, b_y)),
            // The reference white is what a *non-HDR* picture on this output has
            // to be encoded against, and the default BT.2408 suggests (203) is
            // not what an output says about itself.  A level of zero is the
            // protocol's way of saying it is unknown, and is left out rather
            // than carried as a white of no light.
            wp_image_description_info_v1::Event::Luminances { reference_lum, .. }
                if reference_lum > 0 =>
            {
                info.reference_nits = Some(reference_lum as f32);
            }
            // The curve the description names, which is the only thing that says
            // whether this output is an HDR one.  Its gamut and its white say
            // nothing about that: a wide-gamut SDR output and a PQ one can agree
            // on both, and only the transfer tells them apart.  Reading it is
            // what stops a PQ pin surface being put on an output that would
            // decode those codes as sRGB.
            wp_image_description_info_v1::Event::TfNamed {
                tf: WEnum::Value(named),
            } => info.transfer = transfer_of_named(named),
            // A parametric curve is still an answer: a pure power curve with the
            // sRGB exponent is sRGB, and one with PQ's is not representable that
            // way at all (PQ is not a power curve), so only the two that map
            // cleanly are read.  Anything else leaves the transfer unknown, and
            // an unknown transfer keeps the output on the HDR path — see
            // [`is_hdr`] — rather than being guessed at.
            wp_image_description_info_v1::Event::TfPower { eexp } => {
                // 2.2 and the sRGB curve's nominal 2.4 both name the SDR curve
                // this pipeline writes; the protocol carries four decimals.
                if eexp == 24_000 || eexp == 22_000 {
                    info.transfer = Some(Transfer::Srgb);
                } else if eexp == 10_000 {
                    info.transfer = Some(Transfer::Linear);
                }
            }
            wp_image_description_info_v1::Event::Done => {
                // Taking the entry out is what tells the wait above this
                // output has answered.
                if let Some(info) = state.cm_info.remove(data) {
                    if let Some(primaries) = info.primaries {
                        state.cm_gamut.insert(*data, primaries);
                    }
                    if let Some(white) = info.reference_nits {
                        state.cm_white.insert(*data, white);
                    }
                    if let Some(transfer) = info.transfer {
                        state.cm_transfer.insert(*data, transfer);
                    }
                }
            }
            _ => {}
        }
    }
}

impl Dispatch<wp_color_management_surface_v1::WpColorManagementSurfaceV1, ()> for WaylandState {
    fn event(
        _: &mut Self,
        _: &wp_color_management_surface_v1::WpColorManagementSurfaceV1,
        _: wp_color_management_surface_v1::Event,
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
                wl_shm::Format::Argb2101010 => state.topology.shm_argb2101010 = true,
                wl_shm::Format::Xrgb2101010 => state.topology.shm_xrgb2101010 = true,
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

impl Dispatch<wl_region::WlRegion, ()> for WaylandState {
    fn event(
        _: &mut Self,
        _: &wl_region::WlRegion,
        _: wl_region::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        // A region has no events; this exists so an empty input region can be
        // made for a surface that must take no input.
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
                // A pinned-image surface carries no scene, and no buffer here:
                // the half-float dma-bufs it draws into arrive through
                // `install_pin_buffers`, and the caller allocates them on a
                // render node this side has nothing to do with.  The configure
                // only says the surface is ready for one.
                if state.pin_surfaces.contains(&data.output_id) {
                    let Some(overlay) = state.overlays.get_mut(&data.output_id) else {
                        state.fail(VshotError::TopologyChanged);
                        return;
                    };
                    overlay.configured = true;
                    overlay.width = width;
                    overlay.height = height;
                    overlay.pending_parent_redraw = false;
                    state.ready_outputs.insert(data.output_id);
                    return;
                }
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
                // An HDR backdrop shows the output's own ten-bit frame; the SDR
                // freeze editor shows the composed scene at eight bits.
                let backdrop = state
                    .hdr_backdrop
                    .get(&data.output_id)
                    .map(|backdrop| Arc::clone(&backdrop.words));
                let shm_format = match &backdrop {
                    Some(_) => match state.topology.shm_format_10bit() {
                        Some(format) => format,
                        None => {
                            state.fail(VshotError::MissingCapability(
                                "wl_shm XRGB2101010 format".into(),
                            ));
                            return;
                        }
                    },
                    None => match state.topology.shm_format() {
                        Some(format) => format,
                        None => {
                            state.fail(VshotError::MissingCapability(
                                "wl_shm ARGB8888 or XRGB8888 format".into(),
                            ));
                            return;
                        }
                    },
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
                            let result = match &backdrop {
                                Some(words) => slot.render_words(words),
                                None => {
                                    slot.render(&output.frame, None, output.geometry, output.scale)
                                }
                            };
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
        cursor_shape_for_handle, decode_wayland_keycode, is_hdr, selection_cursor_shape,
        transfer_of_named, RedrawTarget, SelectionEvent, SelectionResult, WaylandState,
    };
    use crate::geometry::{Point, Rect};
    use crate::model::Transfer;
    use crate::wayland::input::ResizeHandle;
    use crate::wayland::input::{EditorState, BTN_LEFT, KEY_ESC};
    use wayland_protocols::wp::color_management::v1::client::wp_color_manager_v1::TransferFunction;
    use wayland_protocols::wp::cursor_shape::v1::client::wp_cursor_shape_device_v1::Shape;

    #[test]
    fn only_pq_and_hlg_name_an_hdr_output() {
        assert_eq!(
            transfer_of_named(TransferFunction::St2084Pq),
            Some(Transfer::Pq)
        );
        assert_eq!(
            transfer_of_named(TransferFunction::Hlg),
            Some(Transfer::Hlg)
        );
        // Every SDR curve the protocol names, including the two deprecated ones
        // and the two that differ from sRGB only below the point a pin's codes
        // are decided.  Folding these into the SDR curve is what keeps an SDR
        // output from being handed a PQ buffer.
        for named in [
            TransferFunction::Srgb,
            TransferFunction::ExtSrgb,
            TransferFunction::Bt1886,
            TransferFunction::Gamma22,
            TransferFunction::CompoundPower24,
        ] {
            assert_eq!(transfer_of_named(named), Some(Transfer::Srgb), "{named:?}");
        }
        assert_eq!(
            transfer_of_named(TransferFunction::ExtLinear),
            Some(Transfer::Linear)
        );
        // A curve this pipeline has no name for stays unknown rather than being
        // read as the nearest one.
        assert_eq!(transfer_of_named(TransferFunction::St428), None);
        assert_eq!(transfer_of_named(TransferFunction::Log100), None);
    }

    #[test]
    fn an_output_is_hdr_only_when_its_own_curve_is() {
        assert!(is_hdr(Some(Transfer::Pq)));
        assert!(is_hdr(Some(Transfer::Hlg)));
        assert!(!is_hdr(Some(Transfer::Srgb)));
        assert!(!is_hdr(Some(Transfer::Linear)));
        // An output whose description never answered keeps the behaviour the
        // helper had before it could ask.
        assert!(is_hdr(None));
    }

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

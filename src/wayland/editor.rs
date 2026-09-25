// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

use super::{
    global_point, SelectionEvent, BTN_LEFT, BTN_RIGHT, KEY_CONFIRM, KEY_DOWN, KEY_ESC, KEY_LEFT,
    KEY_RIGHT, KEY_UP,
};
use crate::edit::{ArrowStyle, LineDash, ShapeMask, TextBitmap, DEFAULT_MOSAIC_STRENGTH};
use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect, Size};

const DEFAULT_DOUBLE_CLICK_INTERVAL_MS: u32 = 400;
const DEFAULT_DOUBLE_CLICK_DISTANCE: u32 = 8;
const DEFAULT_DRAG_THRESHOLD: u32 = 4;
const DEFAULT_HANDLE_DISTANCE: u32 = 4;

/// Tools understood by the input state machine.
///
/// The editor deliberately keeps this list independent from `EditPipeline`:
/// the latter currently contains image operations, while these values describe
/// pointer interaction and can be mapped to drawing operations by the caller.
#[derive(Clone, Copy, Debug, Default, Eq, Hash, PartialEq)]
pub enum EditorTool {
    /// Selection, movement and resize mode.
    #[default]
    Select,
    /// Freehand drawing.
    Pen,
    /// Freehand drawing alias for clients that call the tool "draw".
    Draw,
    /// A straight line between the gesture endpoints.
    Line,
    /// A straight line with an arrow head supplied by the renderer.
    Arrow,
    /// A rectangle shape.
    Rectangle,
    /// An ellipse shape.
    Ellipse,
    /// A text/drawing tool whose payload can be supplied by the caller.
    Text,
    /// A pixelation/mosaic stroke supplied by the caller.
    Mosaic,
    /// Backwards-compatible alias for pixelation clients.
    Blur,
    /// Eraser gesture; the caller decides how to apply it to annotations.
    Eraser,
}

/// Short aliases that make the public API convenient for callers with a
/// different naming convention.
pub type Tool = EditorTool;
pub type EditorMode = EditorTool;

#[allow(non_upper_case_globals)]
impl EditorTool {
    pub const Selection: Self = Self::Select;
    pub const Freehand: Self = Self::Pen;

    pub const fn is_selection(self) -> bool {
        matches!(self, Self::Select)
    }

    pub const fn is_drawing(self) -> bool {
        !self.is_selection()
    }
}

/// The part of a selection that a pointer gesture is manipulating.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum ResizeHandle {
    None,
    Move,
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
}

pub type SelectionHandle = ResizeHandle;

impl ResizeHandle {
    pub const fn is_resize(self) -> bool {
        !matches!(self, Self::None | Self::Move)
    }

    pub const fn is_move(self) -> bool {
        matches!(self, Self::Move)
    }
}

/// A toolbar button with a global-coordinate hit rectangle.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ToolbarItem {
    pub tool: EditorTool,
    pub rect: Rect,
}

pub type ToolbarButton = ToolbarItem;

impl ToolbarItem {
    pub const fn new(tool: EditorTool, rect: Rect) -> Self {
        Self { tool, rect }
    }

    pub const fn tool(tool: EditorTool, rect: Rect) -> Self {
        Self::new(tool, rect)
    }

    pub fn contains(self, point: Point) -> bool {
        self.rect.contains(point)
    }
}

/// Toolbar geometry used by [`EditorState::toolbar_hit_test`].
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct ToolbarLayout {
    items: Vec<ToolbarItem>,
}

impl ToolbarLayout {
    pub fn new(items: Vec<ToolbarItem>) -> Self {
        Self { items }
    }

    pub fn from_items<I>(items: I) -> Self
    where
        I: IntoIterator<Item = ToolbarItem>,
    {
        Self::new(items.into_iter().collect())
    }

    /// Creates equally sized buttons across `bounds`.
    pub fn from_bounds(bounds: Rect, tools: &[EditorTool]) -> Self {
        if tools.is_empty() || bounds.size.width == 0 || bounds.size.height == 0 {
            return Self::default();
        }
        let count = tools.len() as u32;
        let base_width = bounds.size.width / count;
        let remainder = bounds.size.width % count;
        let mut x = i64::from(bounds.left());
        let mut items = Vec::with_capacity(tools.len());
        for (index, &tool) in tools.iter().enumerate() {
            let width = base_width + u32::from((index as u32) < remainder);
            let next_x = x + i64::from(width);
            if let Some(rect) = rect_from_edges(
                x,
                i64::from(bounds.top()),
                next_x,
                bounds_bottom(bounds).unwrap_or(i64::from(bounds.top())),
            ) {
                items.push(ToolbarItem::new(tool, rect));
            }
            x = next_x;
        }
        Self::new(items)
    }

    /// Creates buttons with a fixed size and spacing, preserving negative
    /// origins.  Buttons that cannot be represented by `Rect` are omitted.
    pub fn horizontal(origin: Point, item_size: Size, spacing: u32, tools: &[EditorTool]) -> Self {
        let mut x = i64::from(origin.x);
        let top = i64::from(origin.y);
        let mut items = Vec::with_capacity(tools.len());
        for &tool in tools {
            let right = x + i64::from(item_size.width);
            let bottom = top + i64::from(item_size.height);
            if let Some(rect) = rect_from_edges(x, top, right, bottom) {
                items.push(ToolbarItem::new(tool, rect));
            }
            x = right + i64::from(spacing);
        }
        Self::new(items)
    }

    pub fn items(&self) -> &[ToolbarItem] {
        &self.items
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    pub fn hit_test(&self, point: Point) -> Option<EditorTool> {
        self.items
            .iter()
            .find(|item| item.contains(point))
            .map(|item| item.tool)
    }
}

/// A renderer-independent annotation produced by a drawing gesture.
///
/// `color` is RGBA8 and `width` is the stroke width in logical pixels; the
/// renderer multiplies the width by the output scale when drawing device pixels.
/// `dash` selects the line style, `head` scales an arrow's head and `mask`
/// picks the filled area of mosaic shapes.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Annotation {
    Stroke {
        tool: EditorTool,
        points: Vec<Point>,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
        head: u32,
        arrow_style: ArrowStyle,
        strength: u32,
    },
    Shape {
        tool: EditorTool,
        rect: Rect,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
        mask: ShapeMask,
        strength: u32,
    },
    Text {
        origin: Point,
        text: String,
        scale: u32,
        color: [u8; 4],
        /// Font family the label was rasterized with; empty means the Qt
        /// application default font. Informational only: the label pixels are
        /// carried by `bitmap`.
        font: String,
        /// Pre-rendered label pixels from the Qt helper. `None` falls back to
        /// the built-in 5x7 ASCII glyph renderer.
        bitmap: Option<TextBitmap>,
    },
    /// A pasted image. `rect` is where it lands on the canvas, in global
    /// logical pixels; `pixels` is the image at its own resolution, so the two
    /// generally differ and the blit has to rescale. The pixels come from the
    /// Qt helper as a raw RGBA8888 file, the same way a text label's bitmap
    /// does.
    Image { rect: Rect, pixels: TextBitmap },
}

pub const DEFAULT_ANNOTATION_COLOR: [u8; 4] = [255, 64, 64, 255];
pub const DEFAULT_TEXT_COLOR: [u8; 4] = [255, 255, 255, 255];
pub const DEFAULT_ANNOTATION_WIDTH: u32 = 1;

impl Annotation {
    pub fn stroke(tool: EditorTool, points: Vec<Point>) -> Self {
        Self::Stroke {
            tool,
            points,
            color: DEFAULT_ANNOTATION_COLOR,
            width: DEFAULT_ANNOTATION_WIDTH,
            dash: LineDash::Solid,
            head: 1,
            arrow_style: ArrowStyle::Open,
            strength: DEFAULT_MOSAIC_STRENGTH,
        }
    }

    pub fn shape(tool: EditorTool, rect: Rect) -> Self {
        Self::Shape {
            tool,
            rect,
            color: DEFAULT_ANNOTATION_COLOR,
            width: DEFAULT_ANNOTATION_WIDTH,
            dash: LineDash::Solid,
            mask: ShapeMask::Rect,
            strength: DEFAULT_MOSAIC_STRENGTH,
        }
    }

    pub fn text(origin: Point, text: impl Into<String>, scale: u32) -> Self {
        Self::Text {
            origin,
            text: text.into(),
            scale,
            color: DEFAULT_TEXT_COLOR,
            font: String::new(),
            bitmap: None,
        }
    }

    pub const fn color(&self) -> [u8; 4] {
        match self {
            Self::Stroke { color, .. } | Self::Shape { color, .. } | Self::Text { color, .. } => {
                *color
            }
            Self::Image { .. } => DEFAULT_ANNOTATION_COLOR,
        }
    }

    pub const fn width(&self) -> u32 {
        match self {
            Self::Stroke { width, .. } | Self::Shape { width, .. } => *width,
            Self::Text { .. } | Self::Image { .. } => DEFAULT_ANNOTATION_WIDTH,
        }
    }

    pub const fn dash(&self) -> LineDash {
        match self {
            Self::Stroke { dash, .. } | Self::Shape { dash, .. } => *dash,
            Self::Text { .. } | Self::Image { .. } => LineDash::Solid,
        }
    }

    /// Arrow head size multiplier; meaningful for arrow strokes only.
    pub const fn head(&self) -> u32 {
        match self {
            Self::Stroke { head, .. } => *head,
            Self::Shape { .. } | Self::Text { .. } | Self::Image { .. } => 1,
        }
    }

    /// Arrow head shape; meaningful for arrow strokes only.
    pub const fn arrow_style(&self) -> ArrowStyle {
        match self {
            Self::Stroke { arrow_style, .. } => *arrow_style,
            Self::Shape { .. } | Self::Text { .. } | Self::Image { .. } => ArrowStyle::Open,
        }
    }

    /// Filled-area shape of mosaic annotations.
    pub const fn mask(&self) -> ShapeMask {
        match self {
            Self::Shape { mask, .. } => *mask,
            _ => ShapeMask::Rect,
        }
    }

    /// Mosaic strength level 1..3 (block size / smear radius factor).
    pub const fn strength(&self) -> u32 {
        match self {
            Self::Stroke { strength, .. } | Self::Shape { strength, .. } => *strength,
            Self::Text { .. } | Self::Image { .. } => DEFAULT_MOSAIC_STRENGTH,
        }
    }

    pub const fn tool(&self) -> EditorTool {
        match self {
            Self::Stroke { tool, .. } | Self::Shape { tool, .. } => *tool,
            Self::Text { .. } => EditorTool::Text,
            Self::Image { .. } => EditorTool::Select,
        }
    }

    pub fn bounds(&self) -> Option<Rect> {
        match self {
            Self::Shape { rect, .. } => Some(*rect),
            Self::Image { rect, .. } => Some(*rect),
            Self::Stroke { points, .. } => points_bounds(points),
            Self::Text {
                origin,
                text,
                scale,
                ..
            } => text_bounds(*origin, text, *scale),
        }
    }

    pub fn points(&self) -> &[Point] {
        match self {
            Self::Stroke { points, .. } => points,
            Self::Shape { .. } | Self::Text { .. } | Self::Image { .. } => &[],
        }
    }

    pub fn text_content(&self) -> Option<(&str, Point, u32)> {
        match self {
            Self::Text {
                origin,
                text,
                scale,
                ..
            } => Some((text, *origin, *scale)),
            _ => None,
        }
    }

    /// Pre-rendered label pixels of a text annotation, if the helper sent any.
    pub const fn text_bitmap(&self) -> Option<&TextBitmap> {
        match self {
            Self::Text { bitmap, .. } => bitmap.as_ref(),
            _ => None,
        }
    }
}

/// The currently active pointer gesture.  All coordinates are global logical
/// coordinates and can therefore span outputs with negative origins.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub enum GestureState {
    #[default]
    Idle,
    Toolbar {
        tool: EditorTool,
    },
    Selecting {
        anchor: Point,
        current: Point,
    },
    Moving {
        anchor: Point,
        current: Point,
        origin: Rect,
    },
    Resizing {
        handle: ResizeHandle,
        anchor: Point,
        current: Point,
        origin: Rect,
    },
    Drawing {
        tool: EditorTool,
        points: Vec<Point>,
    },
    TextEditing {
        origin: Point,
        text: String,
    },
}

pub type PointerGesture = GestureState;
pub type Gesture = GestureState;

/// Result of feeding one input event into [`EditorState`].
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EditorEventResult {
    Continue,
    Changed,
    Confirmed,
    Cancelled,
}

pub type EditorResult = EditorEventResult;

/// Terminal status of the editor.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EditorOutcome {
    Active,
    Confirmed,
    Cancelled,
}

#[derive(Clone, Copy, Debug)]
struct Click {
    timestamp: u32,
    point: Point,
}

/// Pure pointer/key state machine for a frozen-frame editor.
///
/// The state machine does not talk to Wayland or render anything.  The caller
/// supplies global pointer events (or uses `handle_event_with_origin` for local
/// output coordinates), reads `selection`, `gesture` and `annotations`, and
/// maps the resulting independent annotation types to its edit/render layer.
#[derive(Clone, Debug)]
pub struct EditorState {
    bounds: Rect,
    selection: Option<Rect>,
    pointer: Option<Point>,
    tool: EditorTool,
    toolbar: ToolbarLayout,
    gesture: GestureState,
    annotations: Vec<Annotation>,
    outcome: EditorOutcome,
    last_click: Option<Click>,
    double_click_interval_ms: u32,
    double_click_distance: u32,
    drag_threshold: u32,
    handle_distance: u32,
}

impl EditorState {
    /// Creates an empty editor constrained to `bounds`.
    pub fn new(bounds: Rect) -> Self {
        Self {
            bounds,
            selection: None,
            pointer: None,
            tool: EditorTool::Select,
            toolbar: ToolbarLayout::default(),
            gesture: GestureState::Idle,
            annotations: Vec::new(),
            outcome: EditorOutcome::Active,
            last_click: None,
            double_click_interval_ms: DEFAULT_DOUBLE_CLICK_INTERVAL_MS,
            double_click_distance: DEFAULT_DOUBLE_CLICK_DISTANCE,
            drag_threshold: DEFAULT_DRAG_THRESHOLD,
            handle_distance: DEFAULT_HANDLE_DISTANCE,
        }
    }

    /// Checked constructor for callers that want invalid/empty bounds rejected.
    pub fn try_new(bounds: Rect) -> Result<Self> {
        validate_non_empty_rect(bounds, "editor bounds")?;
        Ok(Self::new(bounds))
    }

    /// Creates an editor with an initial selection.  `bounds` is the first
    /// argument to mirror `new`; use `new_with_selection` when the opposite
    /// argument order is more convenient.
    pub fn with_selection(bounds: Rect, selection: Rect) -> Result<Self> {
        let mut state = Self::try_new(bounds)?;
        state.set_selection(selection)?;
        Ok(state)
    }

    pub fn new_with_selection(selection: Rect, bounds: Rect) -> Result<Self> {
        Self::with_selection(bounds, selection)
    }

    pub fn from_selection(selection: Rect, bounds: Rect) -> Result<Self> {
        Self::new_with_selection(selection, bounds)
    }

    pub fn bounds(&self) -> Rect {
        self.bounds
    }

    pub fn set_bounds(&mut self, bounds: Rect) -> Result<()> {
        validate_non_empty_rect(bounds, "editor bounds")?;
        let selection = self
            .selection
            .and_then(|selection| selection.intersection(bounds));
        self.bounds = bounds;
        self.selection = selection;
        Ok(())
    }

    pub fn selection(&self) -> Option<Rect> {
        self.selection
    }

    pub fn set_selection(&mut self, selection: Rect) -> Result<()> {
        validate_non_empty_rect(selection, "selection")?;
        let selection = selection.intersection(self.bounds).ok_or_else(|| {
            VshotError::Selection("selection does not intersect editor bounds".into())
        })?;
        self.selection = Some(selection);
        self.last_click = None;
        Ok(())
    }

    pub fn clear_selection(&mut self) {
        self.selection = None;
        self.last_click = None;
    }

    pub fn pointer(&self) -> Option<Point> {
        self.pointer
    }

    pub fn current_point(&self) -> Option<Point> {
        self.pointer()
    }

    pub fn set_pointer(&mut self, point: Point) {
        self.pointer = Some(point);
    }

    pub fn tool(&self) -> EditorTool {
        self.tool
    }

    pub fn selected_tool(&self) -> EditorTool {
        self.tool()
    }

    pub fn set_tool(&mut self, tool: EditorTool) {
        self.tool = tool;
        self.last_click = None;
    }

    pub fn toolbar(&self) -> &ToolbarLayout {
        &self.toolbar
    }

    pub fn set_toolbar(&mut self, toolbar: ToolbarLayout) {
        self.toolbar = toolbar;
    }

    pub fn set_toolbar_items<I>(&mut self, items: I)
    where
        I: IntoIterator<Item = ToolbarItem>,
    {
        self.toolbar = ToolbarLayout::from_items(items);
    }

    pub fn toolbar_hit_test(&self, point: Point) -> Option<EditorTool> {
        self.toolbar.hit_test(point)
    }

    pub fn hit_test_toolbar(&self, point: Point) -> Option<EditorTool> {
        self.toolbar_hit_test(point)
    }

    pub fn selection_handle_at(&self, point: Point) -> ResizeHandle {
        self.hit_test_selection(point)
    }

    pub fn hit_test_selection(&self, point: Point) -> ResizeHandle {
        let Some(selection) = self.selection else {
            return ResizeHandle::None;
        };
        let left = i64::from(selection.left());
        let top = i64::from(selection.top());
        let right = match selection.right() {
            Ok(right) => i64::from(right),
            Err(_) => return ResizeHandle::None,
        };
        let bottom = match selection.bottom() {
            Ok(bottom) => i64::from(bottom),
            Err(_) => return ResizeHandle::None,
        };
        let width = right - left;
        let height = bottom - top;
        let distance = i64::from(self.handle_distance).min((width.min(height) / 5).max(1));
        let x = i64::from(point.x);
        let y = i64::from(point.y);
        let near_left = (x - left).abs() <= distance;
        let near_right = (x - (right - 1)).abs() <= distance;
        let near_top = (y - top).abs() <= distance;
        let near_bottom = (y - (bottom - 1)).abs() <= distance;
        let within_x = x >= left - distance && x <= right - 1 + distance;
        let within_y = y >= top - distance && y <= bottom - 1 + distance;

        if near_left && near_top {
            ResizeHandle::TopLeft
        } else if near_right && near_top {
            ResizeHandle::TopRight
        } else if near_right && near_bottom {
            ResizeHandle::BottomRight
        } else if near_left && near_bottom {
            ResizeHandle::BottomLeft
        } else if near_top && within_x {
            ResizeHandle::Top
        } else if near_right && within_y {
            ResizeHandle::Right
        } else if near_bottom && within_x {
            ResizeHandle::Bottom
        } else if near_left && within_y {
            ResizeHandle::Left
        } else if selection.contains(point) {
            ResizeHandle::Move
        } else {
            ResizeHandle::None
        }
    }

    pub fn gesture(&self) -> &GestureState {
        &self.gesture
    }

    pub fn gesture_state(&self) -> &GestureState {
        self.gesture()
    }

    pub fn is_drawing(&self) -> bool {
        matches!(self.gesture, GestureState::Drawing { .. })
    }

    pub fn drawing_points(&self) -> Option<&[Point]> {
        match &self.gesture {
            GestureState::Drawing { points, .. } => Some(points),
            _ => None,
        }
    }

    pub fn annotations(&self) -> &[Annotation] {
        &self.annotations
    }

    pub fn annotations_mut(&mut self) -> &mut Vec<Annotation> {
        &mut self.annotations
    }

    pub fn take_annotations(&mut self) -> Vec<Annotation> {
        std::mem::take(&mut self.annotations)
    }

    pub fn clear_annotations(&mut self) {
        self.annotations.clear();
    }

    pub fn outcome(&self) -> EditorOutcome {
        self.outcome
    }

    pub fn is_confirmed(&self) -> bool {
        matches!(self.outcome, EditorOutcome::Confirmed)
    }

    pub fn is_cancelled(&self) -> bool {
        matches!(self.outcome, EditorOutcome::Cancelled)
    }

    pub fn is_active(&self) -> bool {
        matches!(self.outcome, EditorOutcome::Active)
    }

    pub fn set_double_click_interval_ms(&mut self, milliseconds: u32) {
        self.double_click_interval_ms = milliseconds;
    }

    pub fn set_double_click_timeout(&mut self, milliseconds: u32) {
        self.set_double_click_interval_ms(milliseconds);
    }

    pub fn double_click_interval_ms(&self) -> u32 {
        self.double_click_interval_ms
    }

    pub fn set_double_click_distance(&mut self, pixels: u32) {
        self.double_click_distance = pixels;
    }

    pub fn double_click_distance(&self) -> u32 {
        self.double_click_distance
    }

    pub fn set_drag_threshold(&mut self, pixels: u32) {
        self.drag_threshold = pixels;
    }

    pub fn drag_threshold(&self) -> u32 {
        self.drag_threshold
    }

    pub fn set_handle_distance(&mut self, pixels: u32) {
        self.handle_distance = pixels;
    }

    pub fn handle_distance(&self) -> u32 {
        self.handle_distance
    }

    /// Handles a global-coordinate event.  `GlobalPointerMoved`, `ButtonAt`,
    /// `ButtonWithTimestamp` and `Button` are accepted directly; the latter
    /// three use the event's explicit point or the most recent pointer point.
    /// Local-coordinate pointer events should use `handle_event_with_origin`.
    pub fn handle_event(&mut self, event: SelectionEvent) -> Result<EditorEventResult> {
        self.handle_event_with_origin(event, |_| None)
    }

    pub fn handle(&mut self, event: SelectionEvent) -> Result<EditorEventResult> {
        self.handle_event(event)
    }

    /// Handles a local-coordinate pointer event through an output-origin
    /// callback.  This is a convenience alias for `handle_event_with_origin`.
    pub fn handle_local_event<F>(
        &mut self,
        event: SelectionEvent,
        output_origin: F,
    ) -> Result<EditorEventResult>
    where
        F: FnOnce(u32) -> Option<Point>,
    {
        self.handle_event_with_origin(event, output_origin)
    }

    /// Handles an event and resolves local output coordinates through the
    /// supplied global output-origin callback.  The callback is called only
    /// for `PointerMoved`/`PointerMovedAt` events.
    pub fn handle_event_with_origin<F>(
        &mut self,
        event: SelectionEvent,
        output_origin: F,
    ) -> Result<EditorEventResult>
    where
        F: FnOnce(u32) -> Option<Point>,
    {
        match event {
            SelectionEvent::PointerMoved {
                output_id,
                local_x,
                local_y,
            }
            | SelectionEvent::PointerMovedAt {
                output_id,
                local_x,
                local_y,
                ..
            } => {
                let origin = output_origin(output_id).ok_or_else(|| {
                    VshotError::Selection(format!("pointer entered unknown output {output_id}"))
                })?;
                self.handle_pointer(global_point(origin, local_x, local_y)?)
            }
            SelectionEvent::GlobalPointerMoved { point, .. } => self.handle_pointer(point),
            SelectionEvent::ButtonAt {
                button,
                pressed,
                timestamp,
                point,
            } => {
                self.pointer = Some(point);
                self.handle_button(button, pressed, Some(timestamp))
            }
            SelectionEvent::ButtonWithTimestamp {
                button,
                pressed,
                timestamp,
            } => {
                let point = self.pointer.ok_or_else(|| {
                    VshotError::Selection(
                        "timestamped button arrived before pointer position".into(),
                    )
                })?;
                self.pointer = Some(point);
                self.handle_button(button, pressed, Some(timestamp))
            }
            SelectionEvent::Button { button, pressed } => {
                let point = self.pointer.ok_or_else(|| {
                    VshotError::Selection("button arrived before pointer position".into())
                })?;
                self.pointer = Some(point);
                self.handle_button(button, pressed, None)
            }
            SelectionEvent::KeyAt {
                key,
                pressed,
                timestamp: _,
            }
            | SelectionEvent::Key { key, pressed } => self.handle_key_event(key, pressed),
            SelectionEvent::TextInput { text } => self.input_text(&text),
        }
    }

    pub fn handle_global_pointer(&mut self, point: Point) -> Result<EditorEventResult> {
        self.handle_pointer(point)
    }

    pub fn handle_button_at(
        &mut self,
        button: u32,
        pressed: bool,
        timestamp: u32,
        point: Point,
    ) -> Result<EditorEventResult> {
        self.handle_event(SelectionEvent::ButtonAt {
            button,
            pressed,
            timestamp,
            point,
        })
    }

    pub fn handle_key(&mut self, key: u32, pressed: bool) -> Result<EditorEventResult> {
        self.handle_event(SelectionEvent::Key { key, pressed })
    }

    pub fn input_text(&mut self, text: &str) -> Result<EditorEventResult> {
        let GestureState::TextEditing { text: buffer, .. } = &mut self.gesture else {
            return Ok(EditorEventResult::Continue);
        };
        if text
            .bytes()
            .any(|byte| byte != b'\n' && !(0x20..=0x7e).contains(&byte))
        {
            return Err(VshotError::UnsupportedText(
                "interactive text supports printable ASCII only".into(),
            ));
        }
        buffer.push_str(text);
        Ok(EditorEventResult::Changed)
    }

    pub fn backspace(&mut self) -> EditorEventResult {
        if let GestureState::TextEditing { text, .. } = &mut self.gesture {
            text.pop();
            EditorEventResult::Changed
        } else {
            EditorEventResult::Continue
        }
    }

    pub fn finish_text(&mut self) -> Result<EditorEventResult> {
        let GestureState::TextEditing { origin, text } = self.gesture.clone() else {
            return Ok(EditorEventResult::Continue);
        };
        self.gesture = GestureState::Idle;
        if !text.is_empty() {
            self.annotations.push(Annotation::text(origin, text, 2));
        }
        Ok(EditorEventResult::Changed)
    }

    pub fn text_editing(&self) -> Option<(&str, Point)> {
        match &self.gesture {
            GestureState::TextEditing { origin, text } => Some((text, *origin)),
            _ => None,
        }
    }

    pub fn confirm(&mut self) -> EditorEventResult {
        if !self.is_active() {
            return self.terminal_result();
        }
        if self.selection.is_none() || !matches!(self.gesture, GestureState::Idle) {
            return EditorEventResult::Continue;
        }
        self.outcome = EditorOutcome::Confirmed;
        EditorEventResult::Confirmed
    }

    pub fn cancel(&mut self) -> EditorEventResult {
        if !self.is_active() {
            return self.terminal_result();
        }
        self.gesture = GestureState::Idle;
        self.outcome = EditorOutcome::Cancelled;
        EditorEventResult::Cancelled
    }

    fn handle_pointer(&mut self, point: Point) -> Result<EditorEventResult> {
        self.pointer = Some(point);
        if !self.is_active() {
            return Ok(self.terminal_result());
        }
        let gesture = self.gesture.clone();
        match gesture {
            GestureState::Idle | GestureState::Toolbar { .. } => Ok(EditorEventResult::Continue),
            GestureState::Selecting { anchor, .. } => {
                let current = self.bound_point(point)?;
                let selection = selection_between(anchor, current)?.intersection(self.bounds);
                self.selection = selection;
                self.gesture = GestureState::Selecting { anchor, current };
                Ok(EditorEventResult::Changed)
            }
            GestureState::Moving { anchor, origin, .. } => {
                let current = self.bound_point(point)?;
                self.selection = Some(self.move_rect(origin, anchor, current)?);
                self.gesture = GestureState::Moving {
                    anchor,
                    current,
                    origin,
                };
                Ok(EditorEventResult::Changed)
            }
            GestureState::Resizing {
                handle,
                anchor,
                origin,
                ..
            } => {
                let current = self.bound_point(point)?;
                self.selection = Some(self.resize_rect(origin, handle, current)?);
                self.gesture = GestureState::Resizing {
                    handle,
                    anchor,
                    current,
                    origin,
                };
                Ok(EditorEventResult::Changed)
            }
            GestureState::Drawing { tool, mut points } => {
                let current = self.bound_point(point)?;
                if points
                    .last()
                    .is_none_or(|last| distance_exceeds(*last, current, 1))
                {
                    points.push(current);
                }
                self.gesture = GestureState::Drawing { tool, points };
                Ok(EditorEventResult::Changed)
            }
            GestureState::TextEditing { .. } => Ok(EditorEventResult::Continue),
        }
    }

    fn handle_button(
        &mut self,
        button: u32,
        pressed: bool,
        timestamp: Option<u32>,
    ) -> Result<EditorEventResult> {
        if button == BTN_RIGHT && pressed {
            return Ok(self.cancel());
        }
        if button != BTN_LEFT {
            return Ok(EditorEventResult::Continue);
        }
        if !self.is_active() {
            return Ok(self.terminal_result());
        }
        let point = self.pointer.ok_or_else(|| {
            VshotError::Selection("left button arrived before pointer position".into())
        })?;
        if pressed {
            return self.begin_left_gesture(point, timestamp);
        }
        self.end_left_gesture(point, timestamp)
    }

    fn begin_left_gesture(
        &mut self,
        point: Point,
        timestamp: Option<u32>,
    ) -> Result<EditorEventResult> {
        if let Some(tool) = self.toolbar_hit_test(point) {
            self.tool = tool;
            self.last_click = None;
            self.gesture = GestureState::Toolbar { tool };
            return Ok(EditorEventResult::Changed);
        }

        if matches!(self.gesture, GestureState::TextEditing { .. }) {
            self.finish_text()?;
        }
        if self.tool == EditorTool::Text {
            if let Some(index) = self.annotations.iter().position(|annotation| {
                annotation.tool() == EditorTool::Text
                    && annotation
                        .bounds()
                        .is_some_and(|bounds| bounds.contains(point))
            }) {
                let Annotation::Text { origin, text, .. } = self.annotations.remove(index) else {
                    unreachable!();
                };
                self.gesture = GestureState::TextEditing { origin, text };
            } else {
                self.gesture = GestureState::TextEditing {
                    origin: self.bound_point(point)?,
                    text: String::new(),
                };
            }
            return Ok(EditorEventResult::Changed);
        }
        if self.tool.is_selection() {
            if let Some(selection) = self.selection {
                let possible_double_click =
                    timestamp.is_some_and(|timestamp| self.is_double_click(timestamp, point));
                let handle = self.hit_test_selection(point);
                if possible_double_click && !matches!(handle, ResizeHandle::None) {
                    self.gesture = GestureState::Moving {
                        anchor: point,
                        current: point,
                        origin: selection,
                    };
                    return Ok(EditorEventResult::Continue);
                }
                match handle {
                    ResizeHandle::Move => {
                        self.gesture = GestureState::Moving {
                            anchor: point,
                            current: point,
                            origin: selection,
                        };
                        if possible_double_click {
                            return Ok(EditorEventResult::Continue);
                        }
                    }
                    handle if handle.is_resize() => {
                        self.gesture = GestureState::Resizing {
                            handle,
                            anchor: point,
                            current: point,
                            origin: selection,
                        };
                    }
                    ResizeHandle::None => {
                        let anchor = self.bound_point(point)?;
                        self.selection = None;
                        self.last_click = None;
                        self.gesture = GestureState::Selecting {
                            anchor,
                            current: anchor,
                        };
                    }
                    ResizeHandle::TopLeft
                    | ResizeHandle::Top
                    | ResizeHandle::TopRight
                    | ResizeHandle::Right
                    | ResizeHandle::BottomRight
                    | ResizeHandle::Bottom
                    | ResizeHandle::BottomLeft
                    | ResizeHandle::Left => unreachable!(),
                }
            } else {
                let anchor = self.bound_point(point)?;
                self.gesture = GestureState::Selecting {
                    anchor,
                    current: anchor,
                };
            }
        } else {
            let point = self.bound_point(point)?;
            self.gesture = GestureState::Drawing {
                tool: self.tool,
                points: vec![point],
            };
        }
        Ok(EditorEventResult::Changed)
    }

    fn end_left_gesture(
        &mut self,
        point: Point,
        timestamp: Option<u32>,
    ) -> Result<EditorEventResult> {
        let gesture = self.gesture.clone();
        match gesture {
            GestureState::Idle => Ok(EditorEventResult::Continue),
            GestureState::Toolbar { .. } => {
                self.gesture = GestureState::Idle;
                Ok(EditorEventResult::Changed)
            }
            GestureState::Selecting { anchor, .. } => {
                let current = self.bound_point(point)?;
                let moved = distance_exceeds(anchor, current, self.drag_threshold);
                self.gesture = GestureState::Idle;
                if moved {
                    self.selection = selection_between(anchor, current)?.intersection(self.bounds);
                    self.last_click = None;
                    if self.selection.is_some() {
                        Ok(EditorEventResult::Changed)
                    } else {
                        Ok(EditorEventResult::Continue)
                    }
                } else {
                    self.selection = None;
                    Ok(EditorEventResult::Continue)
                }
            }
            GestureState::Moving { anchor, origin, .. } => {
                let current = self.bound_point(point)?;
                let moved = distance_exceeds(anchor, current, self.drag_threshold);
                self.gesture = GestureState::Idle;
                if moved {
                    self.selection = Some(self.move_rect(origin, anchor, current)?);
                    self.last_click = None;
                    Ok(EditorEventResult::Changed)
                } else {
                    self.selection = Some(origin);
                    if let Some(timestamp) = timestamp {
                        if self.is_double_click(timestamp, current) {
                            self.last_click = None;
                            self.outcome = EditorOutcome::Confirmed;
                            return Ok(EditorEventResult::Confirmed);
                        }
                        self.last_click = Some(Click {
                            timestamp,
                            point: current,
                        });
                    } else {
                        self.last_click = None;
                    }
                    Ok(EditorEventResult::Continue)
                }
            }
            GestureState::Resizing {
                handle,
                anchor,
                origin,
                ..
            } => {
                let current = self.bound_point(point)?;
                let moved = distance_exceeds(anchor, current, self.drag_threshold);
                self.gesture = GestureState::Idle;
                self.selection = Some(self.resize_rect(origin, handle, current)?);
                if moved {
                    self.last_click = None;
                    Ok(EditorEventResult::Changed)
                } else {
                    if let Some(timestamp) = timestamp {
                        self.last_click = Some(Click {
                            timestamp,
                            point: current,
                        });
                    } else {
                        self.last_click = None;
                    }
                    Ok(EditorEventResult::Continue)
                }
            }
            GestureState::TextEditing { .. } => Ok(EditorEventResult::Continue),
            GestureState::Drawing { tool, mut points } => {
                let point = self.bound_point(point)?;
                if points
                    .last()
                    .is_none_or(|last| distance_exceeds(*last, point, 1))
                {
                    points.push(point);
                }
                self.gesture = GestureState::Idle;
                if points.len() == 1 {
                    if let Some(timestamp) = timestamp {
                        if self.is_double_click(timestamp, point) {
                            self.last_click = None;
                            self.outcome = EditorOutcome::Confirmed;
                            return Ok(EditorEventResult::Confirmed);
                        }
                        self.last_click = Some(Click { timestamp, point });
                    }
                    return Ok(EditorEventResult::Continue);
                }
                self.last_click = None;
                if let Some(annotation) = finish_annotation(tool, points)? {
                    self.annotations.push(annotation);
                    Ok(EditorEventResult::Changed)
                } else {
                    Ok(EditorEventResult::Continue)
                }
            }
        }
    }

    fn handle_key_event(&mut self, key: u32, pressed: bool) -> Result<EditorEventResult> {
        if !pressed {
            return Ok(EditorEventResult::Continue);
        }
        if key == KEY_ESC {
            return Ok(self.cancel());
        }
        if matches!(self.gesture, GestureState::TextEditing { .. }) {
            if key == super::KEY_BACKSPACE {
                return Ok(self.backspace());
            }
            if key == KEY_CONFIRM || key == super::KEY_KPENTER {
                return self.finish_text();
            }
            if let Some(character) = raw_key_character(key) {
                return self.input_text(&character.to_string());
            }
            return Ok(EditorEventResult::Continue);
        }
        if key == KEY_CONFIRM || key == super::KEY_KPENTER {
            return Ok(self.confirm());
        }
        if !self.is_active() || !matches!(self.gesture, GestureState::Idle) {
            return Ok(EditorEventResult::Continue);
        }
        let delta = match key {
            KEY_UP => Point::new(0, -1),
            KEY_DOWN => Point::new(0, 1),
            KEY_LEFT => Point::new(-1, 0),
            KEY_RIGHT => Point::new(1, 0),
            _ => return Ok(EditorEventResult::Continue),
        };
        let Some(selection) = self.selection else {
            return Ok(EditorEventResult::Continue);
        };
        self.selection = Some(self.move_rect_by_delta(selection, delta)?);
        self.last_click = None;
        Ok(EditorEventResult::Changed)
    }

    fn is_double_click(&self, timestamp: u32, point: Point) -> bool {
        let Some(previous) = self.last_click else {
            return false;
        };
        timestamp.wrapping_sub(previous.timestamp) <= self.double_click_interval_ms
            && !distance_exceeds(previous.point, point, self.double_click_distance)
    }

    fn terminal_result(&self) -> EditorEventResult {
        match self.outcome {
            EditorOutcome::Active => EditorEventResult::Continue,
            EditorOutcome::Confirmed => EditorEventResult::Confirmed,
            EditorOutcome::Cancelled => EditorEventResult::Cancelled,
        }
    }

    fn bound_point(&self, point: Point) -> Result<Point> {
        let (left, top, right, bottom) = rect_edges(self.bounds)?;
        let x = i64::from(point.x).clamp(left, right - 1);
        let y = i64::from(point.y).clamp(top, bottom - 1);
        Ok(Point::new(
            i32::try_from(x)
                .map_err(|_| VshotError::Selection("pointer x is outside editor bounds".into()))?,
            i32::try_from(y)
                .map_err(|_| VshotError::Selection("pointer y is outside editor bounds".into()))?,
        ))
    }

    fn move_rect(&self, origin: Rect, anchor: Point, current: Point) -> Result<Rect> {
        let dx = current.x as i64 - anchor.x as i64;
        let dy = current.y as i64 - anchor.y as i64;
        self.move_rect_by_i64(origin, dx, dy)
    }

    fn move_rect_by_delta(&self, origin: Rect, delta: Point) -> Result<Rect> {
        self.move_rect_by_i64(origin, i64::from(delta.x), i64::from(delta.y))
    }

    fn move_rect_by_i64(&self, origin: Rect, dx: i64, dy: i64) -> Result<Rect> {
        let (left, top, right, bottom) = rect_edges(self.bounds)?;
        let (origin_left, origin_top, origin_right, origin_bottom) = rect_edges(origin)?;
        let width = origin_right - origin_left;
        let height = origin_bottom - origin_top;
        let new_left = (origin_left + dx).clamp(left, right - width);
        let new_top = (origin_top + dy).clamp(top, bottom - height);
        rect_from_edges(new_left, new_top, new_left + width, new_top + height)
            .ok_or_else(|| VshotError::Selection("moved selection is out of range".into()))
    }

    fn resize_rect(&self, origin: Rect, handle: ResizeHandle, point: Point) -> Result<Rect> {
        let (bounds_left, bounds_top, bounds_right, bounds_bottom) = rect_edges(self.bounds)?;
        let (mut left, mut top, mut right, mut bottom) = rect_edges(origin)?;
        let x = i64::from(point.x);
        let y = i64::from(point.y);
        match handle {
            ResizeHandle::TopLeft => {
                left = x.clamp(bounds_left, right - 1);
                top = y.clamp(bounds_top, bottom - 1);
            }
            ResizeHandle::Top => top = y.clamp(bounds_top, bottom - 1),
            ResizeHandle::TopRight => {
                right = (x + 1).clamp(left + 1, bounds_right);
                top = y.clamp(bounds_top, bottom - 1);
            }
            ResizeHandle::Right => right = (x + 1).clamp(left + 1, bounds_right),
            ResizeHandle::BottomRight => {
                right = (x + 1).clamp(left + 1, bounds_right);
                bottom = (y + 1).clamp(top + 1, bounds_bottom);
            }
            ResizeHandle::Bottom => bottom = (y + 1).clamp(top + 1, bounds_bottom),
            ResizeHandle::BottomLeft => {
                left = x.clamp(bounds_left, right - 1);
                bottom = (y + 1).clamp(top + 1, bounds_bottom);
            }
            ResizeHandle::Left => left = x.clamp(bounds_left, right - 1),
            ResizeHandle::None | ResizeHandle::Move => {}
        }
        rect_from_edges(left, top, right, bottom)
            .ok_or_else(|| VshotError::Selection("resized selection is out of range".into()))
    }
}

fn validate_non_empty_rect(rect: Rect, name: &str) -> Result<()> {
    if rect.is_empty() {
        return Err(VshotError::InvalidGeometry(format!(
            "{name} must not be empty"
        )));
    }
    rect_edges(rect).map(|_| ())
}

fn rect_edges(rect: Rect) -> Result<(i64, i64, i64, i64)> {
    let right = rect.right()?;
    let bottom = rect.bottom()?;
    if right <= rect.left() || bottom <= rect.top() {
        return Err(VshotError::InvalidGeometry(
            "rectangle must not be empty".into(),
        ));
    }
    Ok((
        i64::from(rect.left()),
        i64::from(rect.top()),
        i64::from(right),
        i64::from(bottom),
    ))
}

fn bounds_bottom(bounds: Rect) -> Option<i64> {
    bounds.bottom().ok().map(i64::from)
}

fn rect_from_edges(left: i64, top: i64, right: i64, bottom: i64) -> Option<Rect> {
    if right <= left || bottom <= top {
        return None;
    }
    let width = u32::try_from(right - left).ok()?;
    let height = u32::try_from(bottom - top).ok()?;
    Some(Rect::new(
        i32::try_from(left).ok()?,
        i32::try_from(top).ok()?,
        width,
        height,
    ))
}

fn selection_between(start: Point, end: Point) -> Result<Rect> {
    // `normalized_rect` includes the final pointer pixel, which matches the
    // existing SelectionTracker semantics.
    super::normalized_rect(start, end)
}

fn distance_exceeds(a: Point, b: Point, threshold: u32) -> bool {
    let dx = i128::from(a.x) - i128::from(b.x);
    let dy = i128::from(a.y) - i128::from(b.y);
    let distance_squared = dx * dx + dy * dy;
    let threshold = i128::from(threshold);
    distance_squared > threshold * threshold
}

fn points_bounds(points: &[Point]) -> Option<Rect> {
    let first = *points.first()?;
    let (min_x, max_x, min_y, max_y) = points.iter().skip(1).fold(
        (first.x, first.x, first.y, first.y),
        |(min_x, max_x, min_y, max_y), point| {
            (
                min_x.min(point.x),
                max_x.max(point.x),
                min_y.min(point.y),
                max_y.max(point.y),
            )
        },
    );
    super::normalized_rect(Point::new(min_x, min_y), Point::new(max_x, max_y)).ok()
}

fn raw_key_character(key: u32) -> Option<char> {
    match key {
        2 => Some('1'),
        3 => Some('2'),
        4 => Some('3'),
        5 => Some('4'),
        6 => Some('5'),
        7 => Some('6'),
        8 => Some('7'),
        9 => Some('8'),
        10 => Some('9'),
        11 => Some('0'),
        16 => Some('q'),
        17 => Some('w'),
        18 => Some('e'),
        19 => Some('r'),
        20 => Some('t'),
        21 => Some('y'),
        22 => Some('u'),
        23 => Some('i'),
        24 => Some('o'),
        25 => Some('p'),
        30 => Some('a'),
        31 => Some('s'),
        32 => Some('d'),
        33 => Some('f'),
        34 => Some('g'),
        35 => Some('h'),
        36 => Some('j'),
        37 => Some('k'),
        38 => Some('l'),
        44 => Some('z'),
        45 => Some('x'),
        46 => Some('c'),
        47 => Some('v'),
        48 => Some('b'),
        49 => Some('n'),
        50 => Some('m'),
        57 => Some(' '),
        _ => None,
    }
}

fn text_bounds(origin: Point, text: &str, scale: u32) -> Option<Rect> {
    if scale == 0 {
        return None;
    }
    let max_columns = text.lines().map(str::len).max().unwrap_or(0);
    let width = u32::try_from(max_columns)
        .ok()?
        .checked_mul(scale)?
        .checked_mul(6)?;
    let lines = u32::try_from(text.lines().count().max(1)).ok()?;
    let height = lines.checked_mul(scale)?.checked_mul(8)?;
    Some(Rect::new(origin.x, origin.y, width.max(1), height.max(1)))
}

fn finish_annotation(tool: EditorTool, points: Vec<Point>) -> Result<Option<Annotation>> {
    if points.is_empty() {
        return Ok(None);
    }
    match tool {
        EditorTool::Rectangle | EditorTool::Ellipse => {
            let rect = points_bounds(&points).ok_or_else(|| {
                VshotError::Selection("drawing gesture has invalid geometry".into())
            })?;
            Ok(Some(Annotation::shape(tool, rect)))
        }
        _ => Ok(Some(Annotation::stroke(tool, points))),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wayland::input::KEY_BACKSPACE;

    fn point(x: i32, y: i32) -> Point {
        Point::new(x, y)
    }

    fn move_to(editor: &mut EditorState, point: Point) {
        editor
            .handle_event(SelectionEvent::GlobalPointerMoved {
                point,
                timestamp: 0,
            })
            .unwrap();
    }

    fn click(editor: &mut EditorState, point: Point, timestamp: u32) {
        editor
            .handle_event(SelectionEvent::ButtonAt {
                button: BTN_LEFT,
                pressed: true,
                timestamp,
                point,
            })
            .unwrap();
        editor
            .handle_event(SelectionEvent::ButtonAt {
                button: BTN_LEFT,
                pressed: false,
                timestamp,
                point,
            })
            .unwrap();
    }

    fn drag(editor: &mut EditorState, start: Point, end: Point, timestamp: u32) {
        editor
            .handle_event(SelectionEvent::ButtonAt {
                button: BTN_LEFT,
                pressed: true,
                timestamp,
                point: start,
            })
            .unwrap();
        move_to(editor, end);
        editor
            .handle_event(SelectionEvent::ButtonAt {
                button: BTN_LEFT,
                pressed: false,
                timestamp: timestamp.wrapping_add(10),
                point: end,
            })
            .unwrap();
    }

    #[test]
    fn double_click_confirms_and_handles_timestamp_wraparound() {
        let bounds = Rect::new(-100, -50, 300, 200);
        let mut editor = EditorState::with_selection(bounds, Rect::new(-10, 10, 40, 30)).unwrap();
        click(&mut editor, point(0, 20), u32::MAX - 20);
        assert_eq!(editor.outcome(), EditorOutcome::Active);
        let result = editor
            .handle_event(SelectionEvent::ButtonAt {
                button: BTN_LEFT,
                pressed: true,
                timestamp: 15,
                point: point(1, 21),
            })
            .unwrap();
        assert_eq!(result, EditorEventResult::Continue);
        let result = editor
            .handle_event(SelectionEvent::ButtonAt {
                button: BTN_LEFT,
                pressed: false,
                timestamp: 20,
                point: point(1, 21),
            })
            .unwrap();
        assert_eq!(result, EditorEventResult::Confirmed);
        assert!(editor.is_confirmed());
    }

    #[test]
    fn double_click_on_resize_handle_also_confirms() {
        let bounds = Rect::new(0, 0, 200, 100);
        let mut editor = EditorState::with_selection(bounds, Rect::new(20, 20, 50, 30)).unwrap();
        click(&mut editor, point(20, 20), 100);
        assert_eq!(
            editor
                .handle_event(SelectionEvent::ButtonAt {
                    button: BTN_LEFT,
                    pressed: true,
                    timestamp: 150,
                    point: point(20, 20),
                })
                .unwrap(),
            EditorEventResult::Continue
        );
        assert_eq!(
            editor
                .handle_event(SelectionEvent::ButtonAt {
                    button: BTN_LEFT,
                    pressed: false,
                    timestamp: 160,
                    point: point(20, 20),
                })
                .unwrap(),
            EditorEventResult::Confirmed
        );
    }

    #[test]
    fn double_click_requires_small_position_and_drag_distance() {
        let bounds = Rect::new(0, 0, 200, 100);
        let mut editor = EditorState::with_selection(bounds, Rect::new(20, 20, 50, 30)).unwrap();
        click(&mut editor, point(30, 30), 100);
        drag(&mut editor, point(30, 30), point(40, 30), 150);
        assert!(!editor.is_confirmed());

        let mut editor = EditorState::with_selection(bounds, Rect::new(20, 20, 50, 30)).unwrap();
        click(&mut editor, point(30, 30), 100);
        click(&mut editor, point(45, 30), 150);
        assert!(!editor.is_confirmed());
    }

    #[test]
    fn right_button_and_escape_cancel() {
        let bounds = Rect::new(-20, -20, 100, 100);
        let mut editor = EditorState::new(bounds);
        move_to(&mut editor, point(0, 0));
        let result = editor
            .handle_event(SelectionEvent::ButtonAt {
                button: BTN_RIGHT,
                pressed: true,
                timestamp: 1,
                point: point(0, 0),
            })
            .unwrap();
        assert_eq!(result, EditorEventResult::Cancelled);
        assert!(editor.is_cancelled());

        let mut editor = EditorState::new(bounds);
        let result = editor
            .handle_event(SelectionEvent::Key {
                key: KEY_ESC,
                pressed: true,
            })
            .unwrap();
        assert_eq!(result, EditorEventResult::Cancelled);
    }

    #[test]
    fn moves_and_resizes_all_selection_edges_inside_negative_bounds() {
        let bounds = Rect::new(-100, -100, 200, 160);
        let mut editor = EditorState::with_selection(bounds, Rect::new(-50, -40, 40, 30)).unwrap();
        drag(&mut editor, point(-30, -20), point(-10, 0), 10);
        assert_eq!(editor.selection(), Some(Rect::new(-30, -20, 40, 30)));

        let mut editor = EditorState::with_selection(bounds, Rect::new(-50, -40, 40, 30)).unwrap();
        drag(&mut editor, point(-50, -40), point(-70, -60), 10);
        assert_eq!(editor.selection(), Some(Rect::new(-70, -60, 60, 50)));

        let mut editor = EditorState::with_selection(bounds, Rect::new(0, 0, 40, 30)).unwrap();
        drag(&mut editor, point(39, 15), point(99, 15), 10);
        assert_eq!(editor.selection(), Some(Rect::new(0, 0, 100, 30)));

        let mut editor = EditorState::with_selection(bounds, Rect::new(-50, -40, 40, 30)).unwrap();
        drag(&mut editor, point(-50, -25), point(-99, -25), 10);
        assert_eq!(editor.selection(), Some(Rect::new(-99, -40, 89, 30)));
    }

    #[test]
    fn hit_testing_distinguishes_edges_corners_and_outside() {
        let mut editor =
            EditorState::with_selection(Rect::new(0, 0, 200, 100), Rect::new(20, 20, 60, 40))
                .unwrap();
        editor.set_handle_distance(2);

        assert_eq!(
            editor.hit_test_selection(point(20, 20)),
            ResizeHandle::TopLeft
        );
        assert_eq!(
            editor.hit_test_selection(point(79, 20)),
            ResizeHandle::TopRight
        );
        assert_eq!(
            editor.hit_test_selection(point(20, 59)),
            ResizeHandle::BottomLeft
        );
        assert_eq!(
            editor.hit_test_selection(point(79, 59)),
            ResizeHandle::BottomRight
        );
        assert_eq!(editor.hit_test_selection(point(45, 20)), ResizeHandle::Top);
        assert_eq!(
            editor.hit_test_selection(point(79, 40)),
            ResizeHandle::Right
        );
        assert_eq!(
            editor.hit_test_selection(point(45, 59)),
            ResizeHandle::Bottom
        );
        assert_eq!(editor.hit_test_selection(point(20, 40)), ResizeHandle::Left);
        assert_eq!(editor.hit_test_selection(point(45, 40)), ResizeHandle::Move);
        assert_eq!(
            editor.hit_test_selection(point(100, 80)),
            ResizeHandle::None
        );
    }

    #[test]
    fn clicking_outside_selection_starts_a_new_selection() {
        let bounds = Rect::new(0, 0, 200, 120);
        let mut editor = EditorState::with_selection(bounds, Rect::new(20, 20, 40, 30)).unwrap();

        drag(&mut editor, point(100, 80), point(140, 100), 1);

        assert_eq!(editor.selection(), Some(Rect::new(100, 80, 41, 21)));
        assert!(matches!(editor.gesture(), GestureState::Idle));
    }

    #[test]
    fn clamps_move_and_resize_to_bounds() {
        let bounds = Rect::new(-10, -10, 20, 20);
        let mut editor = EditorState::with_selection(bounds, Rect::new(-5, -5, 8, 8)).unwrap();
        drag(&mut editor, point(0, 0), point(100, 100), 1);
        assert_eq!(editor.selection(), Some(Rect::new(2, 2, 8, 8)));

        let mut editor = EditorState::with_selection(bounds, Rect::new(-5, -5, 8, 8)).unwrap();
        drag(&mut editor, point(-5, -5), point(-100, -100), 1);
        assert_eq!(editor.selection(), Some(Rect::new(-10, -10, 13, 13)));
    }

    #[test]
    fn toolbar_hit_testing_selects_tools_without_starting_drawing() {
        let bounds = Rect::new(-100, -100, 400, 300);
        let tools = [EditorTool::Pen, EditorTool::Rectangle, EditorTool::Arrow];
        let toolbar = ToolbarLayout::horizontal(point(-20, -20), Size::new(20, 10), 2, &tools);
        let mut editor = EditorState::new(bounds);
        editor.set_toolbar(toolbar);
        assert_eq!(
            editor.toolbar_hit_test(point(-19, -19)),
            Some(EditorTool::Pen)
        );
        assert_eq!(
            editor.toolbar_hit_test(point(2, -19)),
            Some(EditorTool::Rectangle)
        );
        assert_eq!(editor.toolbar_hit_test(point(100, 100)), None);

        click(&mut editor, point(-19, -19), 10);
        assert_eq!(editor.tool(), EditorTool::Pen);
        assert!(matches!(editor.gesture(), GestureState::Idle));
    }

    #[test]
    fn drawing_gesture_records_points_and_annotation() {
        let mut editor = EditorState::new(Rect::new(-100, -100, 200, 200));
        editor.set_tool(EditorTool::Pen);
        drag(&mut editor, point(-10, -10), point(10, 10), 1);
        assert_eq!(editor.annotations().len(), 1);
        assert_eq!(editor.annotations()[0].tool(), EditorTool::Pen);
        assert!(editor.annotations()[0].bounds().is_some());
        assert!(matches!(editor.gesture(), GestureState::Idle));
    }

    #[test]
    fn inherited_pointer_allows_click_without_new_motion() {
        let mut editor =
            EditorState::with_selection(Rect::new(0, 0, 200, 100), Rect::new(20, 20, 50, 30))
                .unwrap();
        editor.set_pointer(point(30, 30));
        assert_eq!(
            editor
                .handle_event(SelectionEvent::ButtonWithTimestamp {
                    button: BTN_LEFT,
                    pressed: true,
                    timestamp: 1,
                })
                .unwrap(),
            EditorEventResult::Changed
        );
        assert_eq!(
            editor
                .handle_event(SelectionEvent::ButtonWithTimestamp {
                    button: BTN_LEFT,
                    pressed: false,
                    timestamp: 1,
                })
                .unwrap(),
            EditorEventResult::Continue
        );
    }

    #[test]
    fn text_can_be_created_and_reopened_for_editing() {
        let mut editor = EditorState::new(Rect::new(0, 0, 200, 100));
        editor.set_tool(EditorTool::Text);
        click(&mut editor, point(10, 10), 1);
        editor
            .handle_event(SelectionEvent::TextInput { text: "old".into() })
            .unwrap();
        assert_eq!(
            editor
                .handle_event(SelectionEvent::Key {
                    key: KEY_CONFIRM,
                    pressed: true,
                })
                .unwrap(),
            EditorEventResult::Changed
        );
        assert_eq!(editor.annotations().len(), 1);

        click(&mut editor, point(10, 10), 20);
        assert_eq!(editor.text_editing().map(|(text, _)| text), Some("old"));
        editor
            .handle_event(SelectionEvent::Key {
                key: KEY_BACKSPACE,
                pressed: true,
            })
            .unwrap();
        editor
            .handle_event(SelectionEvent::TextInput { text: "!".into() })
            .unwrap();
        editor
            .handle_event(SelectionEvent::Key {
                key: KEY_CONFIRM,
                pressed: true,
            })
            .unwrap();
        assert_eq!(editor.annotations().len(), 1);
        assert_eq!(
            editor.annotations()[0]
                .text_content()
                .map(|(text, _, _)| text),
            Some("ol!")
        );
    }

    #[test]
    fn local_pointer_events_can_be_resolved_without_wayland_state() {
        let mut editor = EditorState::new(Rect::new(-100, -100, 200, 200));
        editor
            .handle_event_with_origin(
                SelectionEvent::PointerMovedAt {
                    output_id: 7,
                    local_x: 5.9,
                    local_y: 6.2,
                    timestamp: 10,
                },
                |output_id| (output_id == 7).then_some(point(-20, -30)),
            )
            .unwrap();
        assert_eq!(editor.pointer(), Some(point(-15, -24)));
    }
}

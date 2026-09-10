#![allow(dead_code)]

use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect};

// Keep the editor in a separate file while exposing it through `input`.  This
// avoids changing `wayland/mod.rs`, which owns the live Wayland dispatch code.
#[path = "editor.rs"]
pub mod editor;
#[allow(unused_imports)]
pub use editor::*;

pub const BTN_LEFT: u32 = 0x110;
pub const BTN_RIGHT: u32 = 0x111;

// Linux evdev key codes.  Wayland's wl_keyboard::key event carries these raw
// codes (without the XKB +8 offset used by some higher-level APIs).
pub const KEY_ESC: u32 = 1;
pub const KEY_ENTER: u32 = 28;
pub const KEY_RETURN: u32 = KEY_ENTER;
pub const KEY_KPENTER: u32 = 96;
pub const KEY_UP: u32 = 103;
pub const KEY_LEFT: u32 = 105;
pub const KEY_RIGHT: u32 = 106;
pub const KEY_DOWN: u32 = 108;
pub const KEY_BACKSPACE: u32 = 14;
pub const KEY_CONFIRM: u32 = KEY_ENTER;

#[derive(Clone, Debug, PartialEq)]
pub enum SelectionEvent {
    /// Existing pointer event kept source-compatible with the original
    /// selection tracker.  Coordinates are local to `output_id`.
    PointerMoved {
        output_id: u32,
        local_x: f64,
        local_y: f64,
    },
    /// Timestamped form for consumers that need click/gesture timing.
    PointerMovedAt {
        output_id: u32,
        local_x: f64,
        local_y: f64,
        timestamp: u32,
    },
    /// A global-coordinate pointer event for pure state-machine consumers.
    GlobalPointerMoved {
        point: Point,
        timestamp: u32,
    },
    /// Existing button event kept source-compatible with the original
    /// selection tracker.  It uses the most recent pointer position.
    Button {
        button: u32,
        pressed: bool,
    },
    /// Timestamped button event with an explicit global pointer position.
    /// Wayland button events do not contain coordinates, so a dispatcher can
    /// pass its last motion/enter position here without hidden state.
    ButtonAt {
        button: u32,
        pressed: bool,
        timestamp: u32,
        point: Point,
    },
    /// Timestamped button event that uses the most recent pointer position.
    ButtonWithTimestamp {
        button: u32,
        pressed: bool,
        timestamp: u32,
    },
    /// Existing key event kept source-compatible with the original tracker.
    Key {
        key: u32,
        pressed: bool,
    },
    /// Key event carrying the compositor's event time.
    KeyAt {
        key: u32,
        pressed: bool,
        timestamp: u32,
    },
    TextInput {
        text: String,
    },
}

impl SelectionEvent {
    pub const fn pointer_moved_at(
        output_id: u32,
        local_x: f64,
        local_y: f64,
        timestamp: u32,
    ) -> Self {
        Self::PointerMovedAt {
            output_id,
            local_x,
            local_y,
            timestamp,
        }
    }

    pub const fn global_pointer_moved(point: Point, timestamp: u32) -> Self {
        Self::GlobalPointerMoved { point, timestamp }
    }

    pub const fn button_at(button: u32, pressed: bool, timestamp: u32, point: Point) -> Self {
        Self::ButtonAt {
            button,
            pressed,
            timestamp,
            point,
        }
    }

    pub const fn button_with_timestamp(button: u32, pressed: bool, timestamp: u32) -> Self {
        Self::ButtonWithTimestamp {
            button,
            pressed,
            timestamp,
        }
    }

    pub const fn key_at(key: u32, pressed: bool, timestamp: u32) -> Self {
        Self::KeyAt {
            key,
            pressed,
            timestamp,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SelectionResult {
    Continue,
    Completed(Rect),
    Cancelled,
}

#[derive(Clone, Debug, Default)]
pub struct SelectionTracker {
    current_output: Option<u32>,
    current_point: Option<Point>,
    anchor: Option<Point>,
    dragging: bool,
}

impl SelectionTracker {
    pub fn new() -> Self {
        Self::default()
    }

    /// Clears all pointer state belonging to the current interaction.
    pub fn reset(&mut self) {
        self.current_output = None;
        self.current_point = None;
        self.anchor = None;
        self.dragging = false;
    }

    pub fn current_output(&self) -> Option<u32> {
        self.current_output
    }

    pub fn current_point(&self) -> Option<Point> {
        self.current_point
    }

    pub fn anchor(&self) -> Option<Point> {
        self.anchor
    }

    pub fn dragging(&self) -> bool {
        self.dragging
    }

    pub fn selection(&self) -> Option<Rect> {
        self.anchor
            .zip(self.current_point)
            .and_then(|(start, end)| normalized_rect(start, end).ok())
    }

    pub fn handle(
        &mut self,
        event: SelectionEvent,
        output_origin: impl FnOnce(u32) -> Option<Point>,
    ) -> Result<SelectionResult> {
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
            } => self.update_pointer(output_id, local_x, local_y, output_origin),
            SelectionEvent::GlobalPointerMoved { point, .. } => {
                self.current_point = Some(point);
                Ok(SelectionResult::Continue)
            }
            SelectionEvent::ButtonAt {
                button,
                pressed,
                point,
                ..
            } => {
                self.current_point = Some(point);
                self.handle_button(button, pressed)
            }
            SelectionEvent::ButtonWithTimestamp {
                button, pressed, ..
            }
            | SelectionEvent::Button { button, pressed } => self.handle_button(button, pressed),
            SelectionEvent::KeyAt { key, pressed, .. } | SelectionEvent::Key { key, pressed }
                if key == KEY_ESC && pressed =>
            {
                Ok(SelectionResult::Cancelled)
            }
            _ => Ok(SelectionResult::Continue),
        }
    }

    fn update_pointer(
        &mut self,
        output_id: u32,
        local_x: f64,
        local_y: f64,
        output_origin: impl FnOnce(u32) -> Option<Point>,
    ) -> Result<SelectionResult> {
        let origin = output_origin(output_id).ok_or_else(|| {
            VshotError::Selection(format!("pointer entered unknown output {output_id}"))
        })?;
        let point = global_point(origin, local_x, local_y)?;
        self.current_output = Some(output_id);
        self.current_point = Some(point);
        Ok(SelectionResult::Continue)
    }

    fn handle_button(&mut self, button: u32, pressed: bool) -> Result<SelectionResult> {
        match (button, pressed) {
            (BTN_RIGHT, true) => Ok(SelectionResult::Cancelled),
            (BTN_LEFT, true) => {
                let point = self.current_point.ok_or_else(|| {
                    VshotError::Selection("left click arrived before pointer position".into())
                })?;
                self.anchor = Some(point);
                self.dragging = true;
                Ok(SelectionResult::Continue)
            }
            (BTN_LEFT, false) => {
                if !self.dragging {
                    return Ok(SelectionResult::Continue);
                }
                let point = self.current_point.ok_or_else(|| {
                    VshotError::Selection(
                        "left-button release arrived without pointer position".into(),
                    )
                })?;
                let start = self.anchor.ok_or_else(|| {
                    VshotError::Selection("left-button release arrived without an anchor".into())
                })?;
                self.dragging = false;
                let rect = normalized_rect(start, point)?;
                if rect.is_empty() {
                    return Err(VshotError::Selection("selected region is empty".into()));
                }
                Ok(SelectionResult::Completed(rect))
            }
            _ => Ok(SelectionResult::Continue),
        }
    }
}

pub(crate) fn global_point(origin: Point, local_x: f64, local_y: f64) -> Result<Point> {
    if !local_x.is_finite() || !local_y.is_finite() {
        return Err(VshotError::Selection(
            "compositor sent invalid pointer coordinates".into(),
        ));
    }
    let local_x = local_x.floor().clamp(i32::MIN as f64, i32::MAX as f64) as i32;
    let local_y = local_y.floor().clamp(i32::MIN as f64, i32::MAX as f64) as i32;
    Ok(Point::new(
        origin.x.saturating_add(local_x),
        origin.y.saturating_add(local_y),
    ))
}

pub fn normalized_rect(start: Point, end: Point) -> Result<Rect> {
    let left = start.x.min(end.x);
    let top = start.y.min(end.y);
    let right = start
        .x
        .max(end.x)
        .checked_add(1)
        .ok_or_else(|| VshotError::Selection("selection right edge overflows".into()))?;
    let bottom = start
        .y
        .max(end.y)
        .checked_add(1)
        .ok_or_else(|| VshotError::Selection("selection bottom edge overflows".into()))?;
    if right <= left || bottom <= top {
        return Err(VshotError::Selection("selected region is empty".into()));
    }
    Ok(Rect::new(
        left,
        top,
        (right - left) as u32,
        (bottom - top) as u32,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tracks_drag_across_outputs_in_global_coordinates() {
        let mut tracker = SelectionTracker::new();
        let origin = |output_id| match output_id {
            1 => Some(Point::new(-100, 0)),
            2 => Some(Point::new(0, 0)),
            _ => None,
        };
        assert_eq!(
            tracker
                .handle(
                    SelectionEvent::PointerMoved {
                        output_id: 1,
                        local_x: 10.0,
                        local_y: 20.0
                    },
                    origin
                )
                .unwrap(),
            SelectionResult::Continue
        );
        assert_eq!(
            tracker
                .handle(
                    SelectionEvent::Button {
                        button: BTN_LEFT,
                        pressed: true
                    },
                    |_| None
                )
                .unwrap(),
            SelectionResult::Continue
        );
        assert_eq!(
            tracker
                .handle(
                    SelectionEvent::PointerMoved {
                        output_id: 2,
                        local_x: 30.0,
                        local_y: 40.0
                    },
                    origin
                )
                .unwrap(),
            SelectionResult::Continue
        );
        let result = tracker
            .handle(
                SelectionEvent::Button {
                    button: BTN_LEFT,
                    pressed: false,
                },
                |_| None,
            )
            .unwrap();
        assert_eq!(
            result,
            SelectionResult::Completed(Rect::new(-90, 20, 121, 21))
        );
    }

    #[test]
    fn accepts_negative_surface_coordinates_during_pointer_grab() {
        let mut tracker = SelectionTracker::new();
        tracker
            .handle(
                SelectionEvent::PointerMoved {
                    output_id: 1,
                    local_x: -5.2,
                    local_y: -6.1,
                },
                |_| Some(Point::new(100, 200)),
            )
            .unwrap();

        assert_eq!(tracker.current_point(), Some(Point::new(94, 193)));
    }

    #[test]
    fn right_button_cancels_initial_selection() {
        let mut tracker = SelectionTracker::new();
        tracker
            .handle(
                SelectionEvent::PointerMoved {
                    output_id: 1,
                    local_x: 2.0,
                    local_y: 3.0,
                },
                |_| Some(Point::new(0, 0)),
            )
            .unwrap();
        assert_eq!(
            tracker
                .handle(
                    SelectionEvent::Button {
                        button: BTN_RIGHT,
                        pressed: true,
                    },
                    |_| None,
                )
                .unwrap(),
            SelectionResult::Cancelled
        );
    }

    #[test]
    fn escape_cancels() {
        let mut tracker = SelectionTracker::new();
        assert_eq!(
            tracker
                .handle(
                    SelectionEvent::Key {
                        key: KEY_ESC,
                        pressed: true
                    },
                    |_| None
                )
                .unwrap(),
            SelectionResult::Cancelled
        );
    }

    #[test]
    fn reset_clears_pointer_and_drag_state() {
        let mut tracker = SelectionTracker::new();
        tracker
            .handle(
                SelectionEvent::PointerMoved {
                    output_id: 7,
                    local_x: 12.0,
                    local_y: 14.0,
                },
                |_| Some(Point::new(-10, 20)),
            )
            .unwrap();
        tracker
            .handle(
                SelectionEvent::Button {
                    button: BTN_LEFT,
                    pressed: true,
                },
                |_| None,
            )
            .unwrap();

        tracker.reset();

        assert_eq!(tracker.current_output(), None);
        assert_eq!(tracker.current_point(), None);
        assert_eq!(tracker.anchor(), None);
        assert!(!tracker.dragging());
        assert_eq!(tracker.selection(), None);
    }

    #[test]
    fn release_completes_without_an_extra_motion_event() {
        let mut tracker = SelectionTracker::new();
        let origin = |output_id| (output_id == 1).then_some(Point::new(100, 200));
        tracker
            .handle(
                SelectionEvent::PointerMoved {
                    output_id: 1,
                    local_x: 4.0,
                    local_y: 5.0,
                },
                origin,
            )
            .unwrap();
        tracker
            .handle(
                SelectionEvent::Button {
                    button: BTN_LEFT,
                    pressed: true,
                },
                |_| None,
            )
            .unwrap();

        let result = tracker
            .handle(
                SelectionEvent::Button {
                    button: BTN_LEFT,
                    pressed: false,
                },
                |_| None,
            )
            .unwrap();

        assert_eq!(
            result,
            SelectionResult::Completed(Rect::new(104, 205, 1, 1))
        );
    }
}

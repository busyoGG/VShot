#![allow(dead_code)]

use crate::error::Result;
use crate::geometry::{Point, Rect};
use crate::model::{Frame, ImageDocument};

/// Line style of a stroked annotation.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum LineDash {
    #[default]
    Solid,
    Dashed,
    Dotted,
}

impl LineDash {
    /// Parses the wire representation emitted by the Qt helper.
    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "solid" => Some(Self::Solid),
            "dashed" => Some(Self::Dashed),
            "dotted" => Some(Self::Dotted),
            _ => None,
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::Solid => "solid",
            Self::Dashed => "dashed",
            Self::Dotted => "dotted",
        }
    }

    /// Returns the `(on, off)` run lengths in device pixels for a stroke of
    /// `width` device pixels.  Callers treat `Solid` specially, so the pattern
    /// returned here only matters for the broken styles.
    pub fn pattern(self, width: u32) -> (u32, u32) {
        let width = width.max(1);
        match self {
            Self::Solid => (1, 0),
            Self::Dashed => ((3 * width).max(4), (2 * width).max(3)),
            Self::Dotted => (1, (2 * width).max(2)),
        }
    }

    /// Shortens the `on` run so the stamped squares of a `size`-wide pen add up
    /// to the nominal dash length instead of `dash + size - 1`.
    pub fn drawn_on(self, width: u32) -> u32 {
        let (on, _) = self.pattern(width);
        (on.saturating_sub(width.saturating_sub(1))).max(1)
    }
}

/// Arrow head shape used by arrow annotations.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum ArrowStyle {
    #[default]
    Open,
    Filled,
}

impl ArrowStyle {
    /// Parses the wire representation emitted by the Qt helper.
    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "open" => Some(Self::Open),
            "filled" => Some(Self::Filled),
            _ => None,
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::Open => "open",
            Self::Filled => "filled",
        }
    }
}

/// Area shape used by filled/pixelated annotations such as mosaic regions.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum ShapeMask {
    #[default]
    Rect,
    Ellipse,
}

impl ShapeMask {
    /// Parses the wire representation emitted by the Qt helper.
    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "rect" => Some(Self::Rect),
            "ellipse" => Some(Self::Ellipse),
            _ => None,
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::Rect => "rect",
            Self::Ellipse => "ellipse",
        }
    }
}

/// Mosaic strength levels 1..3: the device-pixel block size of rectangular
/// and elliptical pixelation, and the smear radius factor of the freehand
/// brush.  Level 2 matches the historical fixed 12px block.
pub const DEFAULT_MOSAIC_STRENGTH: u32 = 2;

/// Device-pixel mosaic block size for `strength` at the given output scale.
pub fn mosaic_block_size(strength: u32, scale: u32) -> u32 {
    let base = 12u32.saturating_mul(scale).max(1);
    match strength {
        1 => (base / 2).max(4),
        3 => base.saturating_mul(2),
        _ => base,
    }
}

/// Device-pixel smear radius for `strength` given the brush stroke width.
pub fn mosaic_brush_radius(strength: u32, stroke_width: u32) -> u32 {
    let base = (stroke_width / 2).max(1);
    match strength {
        1 => (base / 2).max(1),
        3 => base.saturating_mul(2),
        _ => base,
    }
}

/// A pre-rendered text bitmap produced by the Qt helper: the user-selected
/// font is rasterized there, so vshot only has to composite the pixels.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TextBitmap {
    pub width: u32,
    pub height: u32,
    /// Straight-alpha RGBA8888 pixels, top-down rows of `width * 4` bytes.
    pub pixels: Vec<u8>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum EditOperation {
    Crop(Rect),
    RectangleStroke {
        rect: Rect,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    },
    CircleStroke {
        center: Point,
        radius: u32,
        color: [u8; 4],
        width: u32,
    },
    EllipseStroke {
        rect: Rect,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    },
    Arrow {
        start: Point,
        end: Point,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
        head: u32,
        arrow_style: ArrowStyle,
    },
    Freehand {
        points: Vec<Point>,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    },
    Text {
        origin: Point,
        text: String,
        color: [u8; 4],
        scale: u32,
    },
    /// Composites a helper-rendered text bitmap; `origin` is the bitmap's
    /// top-left corner in device pixels.
    Blit {
        origin: Point,
        bitmap: TextBitmap,
    },
    Mosaic {
        rect: Rect,
        block_size: u32,
    },
    MosaicEllipse {
        rect: Rect,
        block_size: u32,
    },
    MosaicBrush {
        points: Vec<Point>,
        radius: u32,
    },
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct EditPipeline {
    operations: Vec<EditOperation>,
}

impl EditPipeline {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn crop(mut self, rect: Rect) -> Self {
        self.operations.push(EditOperation::Crop(rect));
        self
    }

    pub(crate) fn rectangle_stroke(
        mut self,
        rect: Rect,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    ) -> Self {
        self.operations.push(EditOperation::RectangleStroke {
            rect,
            color,
            width,
            dash,
        });
        self
    }

    pub(crate) fn circle_stroke(
        mut self,
        center: Point,
        radius: u32,
        color: [u8; 4],
        width: u32,
    ) -> Self {
        self.operations.push(EditOperation::CircleStroke {
            center,
            radius,
            color,
            width,
        });
        self
    }

    pub(crate) fn ellipse_stroke(
        mut self,
        rect: Rect,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    ) -> Self {
        self.operations.push(EditOperation::EllipseStroke {
            rect,
            color,
            width,
            dash,
        });
        self
    }

    pub(crate) fn arrow(
        self,
        start: Point,
        end: Point,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
        head: u32,
    ) -> Self {
        self.arrow_with_style(start, end, color, width, dash, head, ArrowStyle::Open)
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn arrow_with_style(
        mut self,
        start: Point,
        end: Point,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
        head: u32,
        arrow_style: ArrowStyle,
    ) -> Self {
        self.operations.push(EditOperation::Arrow {
            start,
            end,
            color,
            width,
            dash,
            head,
            arrow_style,
        });
        self
    }

    pub(crate) fn freehand(
        mut self,
        points: Vec<Point>,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    ) -> Self {
        self.operations.push(EditOperation::Freehand {
            points,
            color,
            width,
            dash,
        });
        self
    }

    pub(crate) fn text(
        mut self,
        origin: Point,
        text: impl Into<String>,
        color: [u8; 4],
        scale: u32,
    ) -> Self {
        self.operations.push(EditOperation::Text {
            origin,
            text: text.into(),
            color,
            scale,
        });
        self
    }

    pub(crate) fn blit(mut self, origin: Point, bitmap: TextBitmap) -> Self {
        self.operations.push(EditOperation::Blit { origin, bitmap });
        self
    }

    pub(crate) fn mosaic(mut self, rect: Rect, block_size: u32) -> Self {
        self.operations
            .push(EditOperation::Mosaic { rect, block_size });
        self
    }

    pub(crate) fn mosaic_ellipse(mut self, rect: Rect, block_size: u32) -> Self {
        self.operations
            .push(EditOperation::MosaicEllipse { rect, block_size });
        self
    }

    pub(crate) fn mosaic_brush(mut self, points: Vec<Point>, radius: u32) -> Self {
        self.operations
            .push(EditOperation::MosaicBrush { points, radius });
        self
    }

    pub fn operations(&self) -> &[EditOperation] {
        &self.operations
    }

    pub fn apply(&self, document: ImageDocument) -> Result<ImageDocument> {
        self.operations
            .iter()
            .try_fold(document, |mut document, operation| {
                match operation {
                    EditOperation::Crop(rect) => document = document.crop(*rect)?,
                    EditOperation::RectangleStroke {
                        rect,
                        color,
                        width,
                        dash,
                    } => document.stroke_rectangle(*rect, *color, *width, *dash)?,
                    EditOperation::CircleStroke {
                        center,
                        radius,
                        color,
                        width,
                    } => document.stroke_circle(*center, *radius, *color, *width)?,
                    EditOperation::EllipseStroke {
                        rect,
                        color,
                        width,
                        dash,
                    } => document.stroke_ellipse(*rect, *color, *width, *dash)?,
                    EditOperation::Arrow {
                        start,
                        end,
                        color,
                        width,
                        dash,
                        head,
                        arrow_style,
                    } => document.draw_arrow_with_style(
                        *start,
                        *end,
                        *color,
                        *width,
                        *dash,
                        *head,
                        *arrow_style,
                    )?,
                    EditOperation::Freehand {
                        points,
                        color,
                        width,
                        dash,
                    } => document.draw_freehand(points, *color, *width, *dash)?,
                    EditOperation::Text {
                        origin,
                        text,
                        color,
                        scale,
                    } => document.draw_text(*origin, text, *color, *scale)?,
                    EditOperation::Blit { origin, bitmap } => {
                        document.draw_bitmap(*origin, bitmap)?
                    }
                    EditOperation::Mosaic { rect, block_size } => {
                        document.mosaic(*rect, *block_size)?
                    }
                    EditOperation::MosaicEllipse { rect, block_size } => {
                        document.mosaic_ellipse(*rect, *block_size)?
                    }
                    EditOperation::MosaicBrush { points, radius } => {
                        document.mosaic_brush(points, *radius)?
                    }
                }
                Ok(document)
            })
    }
}

pub fn crop_operation(frame: &Frame, rect: Rect) -> Result<ImageDocument> {
    EditPipeline::new()
        .crop(rect)
        .apply(ImageDocument::new(frame.clone()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::geometry::{Rect, Size};

    #[test]
    fn arrow_style_parses_only_supported_wire_values() {
        assert_eq!(ArrowStyle::parse("open"), Some(ArrowStyle::Open));
        assert_eq!(ArrowStyle::parse("filled"), Some(ArrowStyle::Filled));
        assert_eq!(ArrowStyle::parse("closed"), None);
        assert_eq!(ArrowStyle::default().name(), "open");
    }
    #[test]
    fn initial_crop_operation_is_executable() {
        let frame = Frame::solid(Size::new(4, 4), [1, 2, 3, 255]).unwrap();
        let document = crop_operation(&frame, Rect::new(1, 1, 2, 2)).unwrap();
        assert_eq!(document.frame().size(), Size::new(2, 2));
    }

    #[test]
    fn mosaic_strength_scales_block_size_and_smear_radius() {
        assert_eq!(mosaic_block_size(2, 1), 12);
        assert_eq!(mosaic_block_size(1, 1), 6);
        assert_eq!(mosaic_block_size(1, 2), 12);
        assert_eq!(mosaic_block_size(3, 2), 48);
        assert_eq!(mosaic_brush_radius(2, 8), 4);
        assert_eq!(mosaic_brush_radius(1, 8), 2);
        assert_eq!(mosaic_brush_radius(3, 8), 8);
        assert_eq!(mosaic_brush_radius(3, 1), 2);
    }
}

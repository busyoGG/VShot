#![allow(dead_code)]

use crate::error::{Result, VshotError};
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

impl TextBitmap {
    /// The same label at `target` device pixels per logical pixel, given that
    /// it was rasterized at `source`.
    ///
    /// The helper draws the label at the scene's highest output scale, but the
    /// frame it lands on carries the density of the output the selection came
    /// from (see `crate::main::crop_native`), and a rect on a lower-density
    /// screen is cropped at *that* screen's scale.  Resampling here keeps the
    /// one contract the helper holds — the bitmap is always in scene device
    /// pixels — instead of moving the "which output holds this rect" rule into
    /// the helper as well.
    ///
    /// Each target pixel averages the source pixels it covers, weighted by
    /// alpha: the antialiased rim of a downscaled label is then the average of
    /// what was drawn, rather than one pixel of it picked out.  A label that
    /// only goes up in scale comes back as it was, since a bitmap rendered at
    /// the scene's scale is already the sharpest copy the helper produced.
    pub fn resampled(&self, source: u32, target: u32) -> TextBitmap {
        let source = u64::from(source.max(1));
        let target = u64::from(target.max(1));
        if source == target || self.width == 0 || self.height == 0 || target > source {
            return self.clone();
        }
        let width = resampled_length(self.width, source, target);
        let height = resampled_length(self.height, source, target);
        let mut pixels = vec![0u8; width as usize * height as usize * 4];
        for y in 0..height {
            let (top, bottom) = covered_span(u64::from(y), self.height, source, target);
            for x in 0..width {
                let (left, right) = covered_span(u64::from(x), self.width, source, target);
                let mut alpha_sum = 0u64;
                let mut weighted = [0u64; 3];
                let mut count = 0u64;
                for row in top..bottom {
                    for column in left..right {
                        let offset = (row * u64::from(self.width) + column) as usize * 4;
                        let Some(pixel) = self.pixels.get(offset..offset + 4) else {
                            continue;
                        };
                        let alpha = u64::from(pixel[3]);
                        alpha_sum += alpha;
                        for channel in 0..3 {
                            weighted[channel] += u64::from(pixel[channel]) * alpha;
                        }
                        count += 1;
                    }
                }
                if count == 0 {
                    continue;
                }
                let destination = (y * width + x) as usize * 4;
                // A fully transparent source span keeps the colour at zero
                // rather than dividing by it; every other span divides by the
                // alpha it accumulated, which is what makes the average
                // colour-weighted instead of biased towards the transparent
                // pixels around a glyph.
                let divisor = alpha_sum.max(1);
                pixels[destination + 3] = (alpha_sum / count) as u8;
                for channel in 0..3 {
                    pixels[destination + channel] = (weighted[channel] / divisor) as u8;
                }
            }
        }
        TextBitmap {
            width,
            height,
            pixels,
        }
    }
}

/// How many pixels long a `length`-pixel span becomes at `target`/`source`.
fn resampled_length(length: u32, source: u64, target: u64) -> u32 {
    let scaled = u64::from(length) * target;
    u32::try_from(scaled.div_ceil(source).max(1)).unwrap_or(u32::MAX)
}

/// Half-open source span the `index`-th target pixel covers.  The end is
/// rounded up so a span can never come out empty — a target pixel covering less
/// than one source pixel still has to take one.
pub(crate) fn covered_span(index: u64, length: u32, source: u64, target: u64) -> (u64, u64) {
    let length = u64::from(length);
    let start = (index * source / target).min(length.saturating_sub(1));
    let end = ((index + 1) * source).div_ceil(target);
    (start, end.clamp(start + 1, length))
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
    /// Composites a pasted image into `rect`, rescaling it when the rect is not
    /// the bitmap's own size -- which it generally is not, since the paste
    /// shrinks to fit the canvas and the handles resize it afterwards.
    BlitScaled {
        rect: Rect,
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

    pub(crate) fn blit_scaled(mut self, rect: Rect, bitmap: TextBitmap) -> Self {
        self.operations
            .push(EditOperation::BlitScaled { rect, bitmap });
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
                    EditOperation::BlitScaled { rect, bitmap } => {
                        document.draw_bitmap_scaled(*rect, bitmap)?
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

/// Converts helper annotations from global (scene) logical coordinates into
/// `selection`-local device pixels and folds them into one pipeline. Shared
/// by the capture flow and by pin editing, which renders annotations on top
/// of a pinned image instead of a frozen scene.
///
/// `scale` is the frame's density — device pixels per logical pixel of the
/// pixels being annotated.  `bitmap_scale` is the density the helper rasterized
/// its text bitmaps at, which is the scene's scale for a capture and the pin's
/// own for pin editing; text is resampled when the two differ.
pub fn pipeline_for_annotations(
    annotations: Vec<crate::wayland::input::Annotation>,
    selection: Rect,
    scale: u32,
    bitmap_scale: u32,
) -> Result<EditPipeline> {
    use crate::wayland::input::Annotation;

    let mut pipeline = EditPipeline::new();
    for annotation in annotations {
        let color = annotation.color();
        let width = device_width(annotation.width(), scale)?;
        let dash = annotation.dash();
        let head = annotation.head();
        let arrow_style = annotation.arrow_style();
        let strength = annotation.strength();
        match annotation {
            Annotation::Shape {
                tool, rect, mask, ..
            } => {
                let rect = local_rect(rect, selection, scale)?;
                match tool {
                    crate::wayland::input::EditorTool::Rectangle => {
                        pipeline = pipeline.rectangle_stroke(rect, color, width, dash);
                    }
                    crate::wayland::input::EditorTool::Ellipse => {
                        pipeline = pipeline.ellipse_stroke(rect, color, width, dash);
                    }
                    crate::wayland::input::EditorTool::Mosaic
                    | crate::wayland::input::EditorTool::Blur => {
                        let block = mosaic_block_size(strength, scale);
                        pipeline = match mask {
                            ShapeMask::Rect => pipeline.mosaic(rect, block),
                            ShapeMask::Ellipse => pipeline.mosaic_ellipse(rect, block),
                        };
                    }
                    _ => {}
                }
            }
            Annotation::Stroke { tool, points, .. } => {
                let points = points
                    .into_iter()
                    .map(|point| local_point(point, selection, scale))
                    .collect::<Result<Vec<_>>>()?;
                match tool {
                    crate::wayland::input::EditorTool::Arrow if points.len() >= 2 => {
                        pipeline = pipeline.arrow_with_style(
                            points[0],
                            *points.last().ok_or_else(|| {
                                VshotError::Selection("arrow has no endpoint".into())
                            })?,
                            color,
                            width,
                            dash,
                            head,
                            arrow_style,
                        );
                    }
                    crate::wayland::input::EditorTool::Pen
                    | crate::wayland::input::EditorTool::Draw
                    | crate::wayland::input::EditorTool::Line => {
                        pipeline = pipeline.freehand(points, color, width, dash);
                    }
                    crate::wayland::input::EditorTool::Mosaic
                    | crate::wayland::input::EditorTool::Blur => {
                        // Freehand mosaic smears discs along the path; the
                        // strength level scales the smear radius.
                        let radius = mosaic_brush_radius(strength, width);
                        pipeline = pipeline.mosaic_brush(points, radius);
                    }
                    _ => {}
                }
            }
            Annotation::Image { rect, pixels } => {
                // Both the destination rect and the image itself are in
                // logical pixels of the scene the helper drew on; the frame is
                // in device pixels of the output the selection came from, so
                // both go through the same scale the other annotations use.
                let rect = local_rect(rect, selection, scale)?;
                pipeline = pipeline.blit_scaled(rect, pixels.clone());
            }
            Annotation::Text {
                origin,
                text,
                scale: text_scale,
                bitmap,
                ..
            } => {
                let origin = local_point(origin, selection, scale)?;
                pipeline = match bitmap {
                    // Helper-rendered with the user-selected font: composite
                    // the bitmap, brought to this frame's density if the
                    // selection came from a lower-density output than the one
                    // the helper drew it for.
                    Some(bitmap) => pipeline.blit(origin, bitmap.resampled(bitmap_scale, scale)),
                    None => {
                        pipeline.text(origin, text, color, text_scale.saturating_mul(scale).max(1))
                    }
                };
            }
        }
    }
    Ok(pipeline)
}

/// Converts an annotation stroke width from logical pixels to device pixels.
fn device_width(logical_width: u32, scale: u32) -> Result<u32> {
    let width = logical_width
        .max(1)
        .checked_mul(scale)
        .ok_or_else(|| VshotError::InvalidGeometry("annotation width overflows".into()))?;
    Ok(width.clamp(1, 4096))
}

fn local_point(point: Point, selection: Rect, scale: u32) -> Result<Point> {
    let x = (i64::from(point.x) - i64::from(selection.left()))
        .checked_mul(i64::from(scale))
        .ok_or_else(|| VshotError::InvalidGeometry("annotation x overflows".into()))?;
    let y = (i64::from(point.y) - i64::from(selection.top()))
        .checked_mul(i64::from(scale))
        .ok_or_else(|| VshotError::InvalidGeometry("annotation y overflows".into()))?;
    Ok(Point::new(
        i32::try_from(x)
            .map_err(|_| VshotError::InvalidGeometry("annotation x is out of range".into()))?,
        i32::try_from(y)
            .map_err(|_| VshotError::InvalidGeometry("annotation y is out of range".into()))?,
    ))
}

fn local_rect(rect: Rect, selection: Rect, scale: u32) -> Result<Rect> {
    let origin = local_point(rect.origin, selection, scale)?;
    Ok(Rect::new(
        origin.x,
        origin.y,
        rect.size
            .width
            .checked_mul(scale)
            .ok_or_else(|| VshotError::InvalidGeometry("annotation width overflows".into()))?,
        rect.size
            .height
            .checked_mul(scale)
            .ok_or_else(|| VshotError::InvalidGeometry("annotation height overflows".into()))?,
    ))
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
    fn a_label_drawn_for_a_denser_screen_comes_down_to_the_frames_own_density() {
        // A 2x2 label rasterized at scale 2: two opaque red pixels beside two
        // opaque blue ones.  On a frame from a scale-1 output it is one pixel,
        // and that pixel is the average of the four — not one of them picked
        // out, and not four times the size it was previewed at.
        let bitmap = TextBitmap {
            width: 2,
            height: 2,
            pixels: vec![
                255, 0, 0, 255, 0, 0, 255, 255, //
                0, 0, 255, 255, 255, 0, 0, 255,
            ],
        };
        let resampled = bitmap.resampled(2, 1);
        assert_eq!((resampled.width, resampled.height), (1, 1));
        assert_eq!(resampled.pixels, vec![127, 0, 127, 255]);
    }

    #[test]
    fn transparent_pixels_do_not_darken_a_downscaled_label() {
        // One opaque white pixel next to a fully transparent one: averaging
        // the colour plainly would give half-brightness grey, which is what a
        // thin antialiased edge is made of.
        let bitmap = TextBitmap {
            width: 2,
            height: 1,
            pixels: vec![255, 255, 255, 255, 0, 0, 0, 0],
        };
        let resampled = bitmap.resampled(2, 1);
        assert_eq!(resampled.pixels, vec![255, 255, 255, 127]);
        // A scale that already matches is left exactly as the helper drew it.
        assert_eq!(bitmap.resampled(2, 2), bitmap);
        assert_eq!(bitmap.resampled(1, 2), bitmap);
    }

    #[test]
    fn a_label_is_downscaled_over_the_whole_span_even_when_it_does_not_divide() {
        // Three pixels at scale 2 cover one and a half at scale 1: the extra
        // source pixel is not dropped, it joins the last target pixel.
        let bitmap = TextBitmap {
            width: 3,
            height: 1,
            pixels: vec![0, 0, 0, 255, 0, 0, 0, 255, 90, 90, 90, 255],
        };
        let resampled = bitmap.resampled(2, 1);
        assert_eq!(resampled.width, 2);
        assert_eq!(&resampled.pixels[0..4], &[0, 0, 0, 255]);
        assert_eq!(&resampled.pixels[4..8], &[90, 90, 90, 255]);
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

    #[test]
    fn a_pasted_image_is_scaled_to_its_rect_and_composited_there() {
        let bitmap = TextBitmap {
            width: 2,
            height: 2,
            pixels: vec![
                255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255,
            ],
        };
        // The helper works in logical pixels of the scene it drew on; the frame
        // is in device pixels of that scene, so an image annotation goes
        // through the same conversion the other annotations do.
        let pipeline = pipeline_for_annotations(
            vec![crate::wayland::input::Annotation::Image {
                rect: Rect::new(2, 3, 4, 5),
                pixels: bitmap.clone(),
            }],
            Rect::new(0, 0, 20, 20),
            2,
            2,
        )
        .unwrap();
        assert_eq!(
            pipeline.operations(),
            [EditOperation::BlitScaled {
                rect: Rect::new(4, 6, 8, 10),
                bitmap,
            }]
        );

        let frame = Frame::solid(Size::new(20, 20), [0, 0, 0, 255]).unwrap();
        let document = pipeline
            .apply(ImageDocument::new(frame))
            .unwrap()
            .into_frame();
        // The source quadrants fill the destination, and nothing outside it is
        // touched.
        assert_eq!(
            document.pixel(Point::new(4, 6)),
            Some([255, 0, 0, 255]),
            "top-left quadrant"
        );
        assert_eq!(
            document.pixel(Point::new(11, 6)),
            Some([0, 255, 0, 255]),
            "top-right quadrant"
        );
        assert_eq!(
            document.pixel(Point::new(11, 15)),
            Some([255, 255, 0, 255]),
            "bottom-right quadrant"
        );
        assert_eq!(document.pixel(Point::new(3, 6)), Some([0, 0, 0, 255]));
        assert_eq!(document.pixel(Point::new(12, 6)), Some([0, 0, 0, 255]));
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#![allow(dead_code)]

use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect};
use crate::model::{Frame, HdrFrame, ImageDocument};

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

/// Which parts of a pen-tool path get painted.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum BezierFill {
    /// Stroke only. Even a path the pen closed is left hollow.
    Stroke,
    /// Fill instead of stroking: a closed path has its interior painted and its
    /// outline left off. An open path is stroked like the other two modes —
    /// there is no interior to fill, and filling one would close it behind the
    /// user's back.
    Fill,
    /// Fill and stroke, but the fill only happens on a path the pen actually
    /// closed; an open path is stroked alone. This is the historical
    /// behaviour, and the default.
    #[default]
    Both,
}

impl BezierFill {
    /// Parses the wire representation emitted by the Qt helper. Unknown and
    /// missing values both fall back to [`Self::Both`].
    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "stroke" => Some(Self::Stroke),
            "fill" => Some(Self::Fill),
            "both" => Some(Self::Both),
            _ => None,
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::Stroke => "stroke",
            Self::Fill => "fill",
            Self::Both => "both",
        }
    }

    /// Whether a path in this closure state has its interior painted. No mode
    /// fills an open path: filling one closes it implicitly, so a shape the
    /// user never closed would appear whole. `Stroke` never fills at all, which
    /// is what leaves even a closed path hollow.
    pub const fn fills(self, closed: bool) -> bool {
        match self {
            Self::Stroke => false,
            Self::Fill | Self::Both => closed,
        }
    }

    /// Whether a path in this closure state has its outline painted. `Fill`
    /// drops the outline of a closed path -- that is all that makes it a fill
    /// rather than a fill *and* an outline -- and keeps it on an open one,
    /// where there is no interior to paint and a path with no ink at all would
    /// simply vanish as the user drew it.
    pub const fn strokes(self, closed: bool) -> bool {
        match self {
            Self::Stroke | Self::Both => true,
            Self::Fill => !closed,
        }
    }
}

/// Mosaic strength levels 1..3: the device-pixel block size of rectangular
/// and elliptical pixelation, and the smear radius factor of the freehand
/// brush.  Level 2 matches the historical fixed 12px block.
pub const DEFAULT_MOSAIC_STRENGTH: u32 = 2;

/// Device pixels per logical pixel of the surface annotations are drawn on.
///
/// A capture is annotated on an output's own frame, whose density is a whole
/// number: one device pixel per logical pixel at scale 1, two at scale 2, and
/// so on.  A pin is annotated on an image that has been zoomed, and a zoomed
/// image is not a whole number of device pixels per logical pixel — a 160-pixel
/// pin shown at 176 logical pixels covers 0.909 of a device pixel each, so
/// rounding that to 1 drew every mark at the wrong size and in the wrong place,
/// and the editor's session was refused outright because its pixel dimensions
/// no longer matched the rect and the scale it declared.
///
/// The factor is therefore kept as it is rather than rounded to a whole number.
/// Every whole density still maps through unchanged — the arithmetic below is
/// exact for them — so a capture's rendering is untouched.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Scale {
    device: f64,
}

impl Scale {
    /// The scale of a surface with `device` device pixels per logical pixel.
    pub fn whole(device: u32) -> Self {
        Self {
            device: f64::from(device.max(1)),
        }
    }

    /// The scale an image `pixels` device pixels wide is shown at in `logical`
    /// logical pixels: a pin editor's zoom.
    ///
    /// Zero on either side falls back to 1, so a session that describes nothing
    /// draws at its own size rather than dividing by zero.
    pub fn ratio(pixels: u32, logical: u32) -> Self {
        if pixels == 0 || logical == 0 {
            return Self::whole(1);
        }
        Self {
            device: f64::from(pixels) / f64::from(logical),
        }
    }

    pub fn factor(self) -> f64 {
        self.device
    }

    /// A length in logical pixels, in device pixels.  Exact for whole scales,
    /// so an unzoomed capture maps through unchanged.
    pub fn length(self, logical: u32) -> u32 {
        let scaled = f64::from(logical) * self.device;
        if !scaled.is_finite() || scaled < 1.0 {
            return 1;
        }
        (scaled.round() as i64).clamp(1, i64::from(u32::MAX)) as u32
    }

    /// A signed coordinate difference in logical pixels, in device pixels,
    /// rounded to the nearest one.
    pub fn offset(self, logical: i64) -> i64 {
        let scaled = logical as f64 * self.device;
        if !scaled.is_finite() {
            return 0;
        }
        scaled.round() as i64
    }
}

/// Device-pixel mosaic block size for `strength` at the given scale.
pub fn mosaic_block_size(strength: u32, scale: Scale) -> u32 {
    let base = 12u32.saturating_mul(scale.length(1)).max(1);
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
    /// A sine wave along the device-space segment `start..end`. `width`,
    /// `amplitude` and `wavelength` are all device pixels; the caller derives
    /// the latter two from the logical stroke width (see `wave_amplitude`).
    Wave {
        start: Point,
        end: Point,
        color: [u8; 4],
        width: u32,
        amplitude: u32,
        wavelength: u32,
    },
    /// A pen-tool cubic Bezier path. `points` is interleaved — `points[2i]` is
    /// anchor `i`, `points[2i + 1]` its out-handle, both in device pixels — so
    /// its length is even. The in-handle is the mirror of the out-handle about
    /// the anchor. `closed` joins the last anchor back to the first with a
    /// straight segment; `fill` decides which parts of the path get painted.
    Bezier {
        points: Vec<Point>,
        closed: bool,
        fill: BezierFill,
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

    pub(crate) fn wave(
        mut self,
        start: Point,
        end: Point,
        color: [u8; 4],
        width: u32,
        amplitude: u32,
        wavelength: u32,
    ) -> Self {
        self.operations.push(EditOperation::Wave {
            start,
            end,
            color,
            width,
            amplitude,
            wavelength,
        });
        self
    }

    pub(crate) fn bezier(
        mut self,
        points: Vec<Point>,
        closed: bool,
        fill: BezierFill,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    ) -> Self {
        self.operations.push(EditOperation::Bezier {
            points,
            closed,
            fill,
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
                    EditOperation::Wave {
                        start,
                        end,
                        color,
                        width,
                        amplitude,
                        wavelength,
                    } => {
                        document.draw_wave(*start, *end, *color, *width, *amplitude, *wavelength)?
                    }
                    EditOperation::Bezier {
                        points,
                        closed,
                        fill,
                        color,
                        width,
                        dash,
                    } => document.draw_bezier(points, *closed, *fill, *color, *width, *dash)?,
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

    /// Applies the same operations to an HDR frame, rendering the annotations
    /// in HDR mode.
    ///
    /// The frame must already be cropped the same way the SDR frame was — an
    /// annotation pipeline's coordinates are crop-relative, so there is no crop
    /// for it to apply.  Mosaics are pixelated in linear light directly on the
    /// HDR frame; every other mark is rasterized into a transparent layer and
    /// composited **in linear light**, so a mark's brightness is not crushed by
    /// the SDR curve and a bright HDR backdrop shows through its edges.
    pub fn apply_to_hdr(&self, frame: HdrFrame) -> Result<HdrFrame> {
        let mut frame = frame;
        for operation in &self.operations {
            match operation {
                EditOperation::Crop(_) => {
                    // Already applied: the HDR frame arrives cropped the same way
                    // the SDR document is (see the note above), so running the
                    // crop again would cut it twice.
                }
                EditOperation::Mosaic { rect, block_size } => frame.mosaic(*rect, *block_size)?,
                EditOperation::MosaicEllipse { rect, block_size } => {
                    frame.mosaic_ellipse(*rect, *block_size)?
                }
                EditOperation::MosaicBrush { points, radius } => {
                    frame.mosaic_brush(points, *radius)?
                }
                paint => {
                    let mut layer = ImageDocument::new(Frame::solid(frame.size(), [0, 0, 0, 0])?);
                    match paint {
                        EditOperation::RectangleStroke {
                            rect,
                            color,
                            width,
                            dash,
                        } => layer.stroke_rectangle(*rect, *color, *width, *dash)?,
                        EditOperation::CircleStroke {
                            center,
                            radius,
                            color,
                            width,
                        } => layer.stroke_circle(*center, *radius, *color, *width)?,
                        EditOperation::EllipseStroke {
                            rect,
                            color,
                            width,
                            dash,
                        } => layer.stroke_ellipse(*rect, *color, *width, *dash)?,
                        EditOperation::Arrow {
                            start,
                            end,
                            color,
                            width,
                            dash,
                            head,
                            arrow_style,
                        } => layer.draw_arrow_with_style(
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
                        } => layer.draw_freehand(points, *color, *width, *dash)?,
                        EditOperation::Text {
                            origin,
                            text,
                            color,
                            scale,
                        } => layer.draw_text(*origin, text, *color, *scale)?,
                        EditOperation::Blit { origin, bitmap } => {
                            layer.draw_bitmap(*origin, bitmap)?
                        }
                        EditOperation::BlitScaled { rect, bitmap } => {
                            layer.draw_bitmap_scaled(*rect, bitmap)?
                        }
                        _ => unreachable!("the sampling operations are handled above"),
                    }
                    frame.composite_srgb_layer(layer.frame())?;
                }
            }
        }
        Ok(frame)
    }
}

pub fn crop_operation(frame: &Frame, rect: Rect) -> Result<ImageDocument> {
    EditPipeline::new()
        .crop(rect)
        .apply(ImageDocument::new(frame.clone()))
}

/// Peak deviation of a wave stroke from its centre line, in device pixels.
///
/// The floor of `max(width * 2, 4)` is applied to the *logical* width and the
/// result is then scaled, which is exactly what the Qt preview does; scaling
/// first and flooring afterwards would flatten the wave at high densities.
fn wave_amplitude(logical_width: u32, scale: Scale) -> u32 {
    scale.length(logical_width.saturating_mul(2).max(4))
}

/// One full period of a wave stroke along its line, in device pixels. Like
/// [`wave_amplitude`], `max(width * 6, 18)` is applied in logical pixels first.
fn wave_wavelength(logical_width: u32, scale: Scale) -> u32 {
    scale.length(logical_width.saturating_mul(6).max(18))
}

/// Resolves one wave dimension from the helper's logical-pixel value.
///
/// `explicit` is what the user typed, clamped by the parser to `1..=4096`; a
/// zero means the helper sent nothing, so `derive` computes it from the logical
/// stroke width.  An explicit value is a logical-pixel length like the derived
/// one, so it goes through the same scaling by the density.
fn wave_size(
    explicit: u32,
    logical_width: u32,
    scale: Scale,
    derive: fn(u32, Scale) -> u32,
) -> u32 {
    if explicit == 0 {
        derive(logical_width, scale)
    } else {
        scale.length(explicit)
    }
}

/// Converts an annotation stroke width from logical pixels to device pixels.
fn device_width(logical_width: u32, scale: Scale) -> u32 {
    scale.length(logical_width.max(1)).clamp(1, 4096)
}

fn local_point(point: Point, selection: Rect, scale: Scale) -> Result<Point> {
    let x = i64::from(point.x) - i64::from(selection.left());
    let y = i64::from(point.y) - i64::from(selection.top());
    Ok(Point::new(
        i32::try_from(scale.offset(x))
            .map_err(|_| VshotError::InvalidGeometry("annotation x is out of range".into()))?,
        i32::try_from(scale.offset(y))
            .map_err(|_| VshotError::InvalidGeometry("annotation y is out of range".into()))?,
    ))
}

fn local_rect(rect: Rect, selection: Rect, scale: Scale) -> Result<Rect> {
    let origin = local_point(rect.origin, selection, scale)?;
    Ok(Rect::new(
        origin.x,
        origin.y,
        scale.length(rect.size.width),
        scale.length(rect.size.height),
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
    fn bezier_fill_parses_only_supported_wire_values() {
        assert_eq!(BezierFill::parse("stroke"), Some(BezierFill::Stroke));
        assert_eq!(BezierFill::parse("fill"), Some(BezierFill::Fill));
        assert_eq!(BezierFill::parse("both"), Some(BezierFill::Both));
        assert_eq!(BezierFill::parse("hollow"), None);
        // "Both" is the historical fill-and-stroke behaviour, so it is what a
        // helper that sends nothing gets.
        assert_eq!(BezierFill::default(), BezierFill::Both);
        assert_eq!(BezierFill::default().name(), "both");
        // No mode fills an open path: an implicit closure would show the user a
        // shape they never closed. "fill" and "both" differ only in whether a
        // closed path keeps its outline.
        assert!(!BezierFill::Stroke.fills(true));
        assert!(!BezierFill::Stroke.fills(false));
        assert!(BezierFill::Fill.fills(true));
        assert!(!BezierFill::Fill.fills(false));
        assert!(BezierFill::Both.fills(true));
        assert!(!BezierFill::Both.fills(false));
        assert!(BezierFill::Stroke.strokes(true));
        assert!(BezierFill::Stroke.strokes(false));
        // The outline "fill" drops is exactly the closed path's: it is still
        // drawn on an open one, or a path in progress would be invisible.
        assert!(!BezierFill::Fill.strokes(true));
        assert!(BezierFill::Fill.strokes(false));
        assert!(BezierFill::Both.strokes(true));
        assert!(BezierFill::Both.strokes(false));
    }
    #[test]
    fn initial_crop_operation_is_executable() {
        let frame = Frame::solid(Size::new(4, 4), [1, 2, 3, 255]).unwrap();
        let document = crop_operation(&frame, Rect::new(1, 1, 2, 2)).unwrap();
        assert_eq!(document.frame().size(), Size::new(2, 2));
    }

    #[test]
    fn hdr_annotations_are_composited_in_linear_light() {
        // A grey HDR frame with a red box stroke: the mark lands on the border,
        // the untouched middle keeps its HDR value, and the mark's colour is
        // decoded to linear (pure red) rather than left as the sRGB byte.
        let hdr = HdrFrame::new(Size::new(8, 8), vec![[0.5, 0.5, 0.5, 1.0]; 64]).unwrap();
        let pipeline = EditPipeline::new().rectangle_stroke(
            Rect::new(0, 0, 8, 8),
            [255, 0, 0, 255],
            1,
            LineDash::Solid,
        );
        let out = pipeline.apply_to_hdr(hdr).unwrap();
        let border = out.pixel(0, 0).unwrap();
        assert!((border[0] - 1.0).abs() < 1e-3, "border red = {}", border[0]);
        assert!(border[1].abs() < 1e-3, "border green = {}", border[1]);
        let middle = out.pixel(4, 4).unwrap();
        assert!((middle[0] - 0.5).abs() < 1e-3, "middle = {}", middle[0]);
    }

    #[test]
    fn an_hdr_mosaic_pixelates_the_linear_values() {
        // Two columns, dark then bright: the mosaic averages in light, so both
        // sides become the linear mean rather than an sRGB-weighted one.
        let hdr = HdrFrame::new(
            Size::new(2, 1),
            vec![[0.0, 0.0, 0.0, 1.0], [4.0, 0.0, 0.0, 1.0]],
        )
        .unwrap();
        let pipeline = EditPipeline::new().mosaic(Rect::new(0, 0, 2, 1), 2);
        let out = pipeline.apply_to_hdr(hdr).unwrap();
        assert!((out.pixel(0, 0).unwrap()[0] - 2.0).abs() < 1e-4);
        assert!((out.pixel(1, 0).unwrap()[0] - 2.0).abs() < 1e-4);
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
        assert_eq!(mosaic_block_size(2, Scale::whole(1)), 12);
        assert_eq!(mosaic_block_size(1, Scale::whole(1)), 6);
        assert_eq!(mosaic_block_size(1, Scale::whole(2)), 12);
        assert_eq!(mosaic_block_size(3, Scale::whole(2)), 48);
        assert_eq!(mosaic_brush_radius(2, 8), 4);
        assert_eq!(mosaic_brush_radius(1, 8), 2);
        assert_eq!(mosaic_brush_radius(3, 8), 8);
        assert_eq!(mosaic_brush_radius(3, 1), 2);
    }

    #[test]
    fn wave_amplitude_and_wavelength_floor_in_logical_pixels() {
        // The floors bind before scaling: a logical width of 1 is amplified to
        // 4 / 18, not to 1 / 6, and only then multiplied by the density.
        assert_eq!(wave_amplitude(1, Scale::whole(1)), 4);
        assert_eq!(wave_wavelength(1, Scale::whole(1)), 18);
        assert_eq!(wave_amplitude(1, Scale::whole(3)), 12);
        assert_eq!(wave_wavelength(1, Scale::whole(3)), 54);
        // Above the floor the wave just follows the width.
        assert_eq!(wave_amplitude(3, Scale::whole(2)), 12);
        assert_eq!(wave_wavelength(3, Scale::whole(2)), 36);
    }
}

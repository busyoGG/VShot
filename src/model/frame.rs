// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#![allow(dead_code)]

use image::{DynamicImage, ImageFormat};

use crate::edit::{covered_span, ArrowStyle, BezierFill, LineDash, TextBitmap};
use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect, Size};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Frame {
    size: Size,
    pixels: Vec<u8>,
}

impl Frame {
    pub fn new(size: Size, pixels: Vec<u8>) -> Result<Self> {
        let expected = size
            .area()?
            .checked_mul(4)
            .ok_or_else(|| VshotError::InvalidGeometry("RGBA frame is too large".into()))?;
        if pixels.len() != expected {
            return Err(VshotError::InvalidGeometry(format!(
                "RGBA frame has {} bytes, expected {expected}",
                pixels.len()
            )));
        }
        Ok(Self { size, pixels })
    }

    pub fn solid(size: Size, rgba: [u8; 4]) -> Result<Self> {
        let mut pixels = vec![
            0;
            size.area()?.checked_mul(4).ok_or_else(|| {
                VshotError::InvalidGeometry("RGBA frame is too large".into())
            })?
        ];
        for pixel in pixels.as_chunks_mut::<4>().0 {
            pixel.copy_from_slice(&rgba);
        }
        Self::new(size, pixels)
    }

    pub fn from_png(bytes: &[u8]) -> Result<Self> {
        let image =
            image::load_from_memory_with_format(bytes, ImageFormat::Png).map_err(|error| {
                VshotError::PngDecode {
                    origin: "PNG input".into(),
                    message: error.to_string(),
                }
            })?;
        Self::from_dynamic_image(image)
    }

    pub fn from_dynamic_image(image: DynamicImage) -> Result<Self> {
        let rgba = image.to_rgba8();
        let (width, height) = rgba.dimensions();
        Self::new(Size::new(width, height), rgba.into_raw())
    }

    /// Encodes the frame as a PNG at the default compression level.
    pub fn to_png(&self) -> Result<Vec<u8>> {
        self.encode_png(None, PngCompression::default())
    }

    /// Encodes the frame as a PNG. `density` — the frame's device pixels per
    /// logical pixel, i.e. the scale of the output it was captured on — is
    /// declared as the PNG's physical resolution when given. The image then
    /// says which output it came from on its own, so pinning the file
    /// elsewhere needs no side record; a 2x capture declares 192 DPI.
    /// `compression` picks how hard the encoder works: the levels are all
    /// lossless and differ only in the time they cost and the size they buy.
    pub fn encode_png(&self, density: Option<u32>, compression: PngCompression) -> Result<Vec<u8>> {
        let width = self.size.width;
        let height = self.size.height;
        let mut bytes = Vec::new();
        {
            let mut encoder = png::Encoder::new(&mut bytes, width, height);
            encoder.set_color(png::ColorType::Rgba);
            encoder.set_depth(png::BitDepth::Eight);
            encoder.set_compression(compression.into());
            // Say the pixels are sRGB.  Without it a file of sRGB bytes has no
            // colour space at all, and a viewer on a wide-gamut display is free
            // to read them as that display's own gamut -- which is exactly what
            // a P3 or BT.2020 panel is, so an untagged capture came out tinted.
            encoder.set_source_srgb(png::SrgbRenderingIntent::Perceptual);
            if let Some(density) = density {
                let pixels_per_meter = density_to_pixels_per_meter(density);
                encoder.set_pixel_dims(Some(png::PixelDimensions {
                    xppu: pixels_per_meter,
                    yppu: pixels_per_meter,
                    unit: png::Unit::Meter,
                }));
            }
            let mut writer = encoder
                .write_header()
                .map_err(|error| VshotError::PngEncode(error.to_string()))?;
            writer
                .write_image_data(&self.pixels)
                .map_err(|error| VshotError::PngEncode(error.to_string()))?;
            writer
                .finish()
                .map_err(|error| VshotError::PngEncode(error.to_string()))?;
        }
        Ok(bytes)
    }

    pub const fn size(&self) -> Size {
        self.size
    }

    pub fn pixels(&self) -> &[u8] {
        &self.pixels
    }

    pub fn pixel(&self, point: Point) -> Option<[u8; 4]> {
        let x = u32::try_from(point.x).ok()?;
        let y = u32::try_from(point.y).ok()?;
        if x >= self.size.width || y >= self.size.height {
            return None;
        }
        let index = (y as usize * self.size.width as usize + x as usize) * 4;
        self.pixels
            .get(index..index + 4)
            .map(|pixel| [pixel[0], pixel[1], pixel[2], pixel[3]])
    }

    pub fn crop(&self, requested: Rect) -> Result<Self> {
        let bounds = Rect::new(0, 0, self.size.width, self.size.height);
        let crop = requested.intersection(bounds).ok_or_else(|| {
            VshotError::InvalidGeometry("crop does not intersect the frame".into())
        })?;
        let width = crop.size.width as usize;
        let height = crop.size.height as usize;
        let source_width = self.size.width as usize;
        let pixel_count = width
            .checked_mul(height)
            .and_then(|area| area.checked_mul(4))
            .ok_or_else(|| VshotError::InvalidGeometry("crop is too large".into()))?;
        let mut pixels = Vec::with_capacity(pixel_count);
        for row in 0..height {
            let start =
                ((crop.origin.y as usize + row) * source_width + crop.origin.x as usize) * 4;
            let end = start + width * 4;
            pixels.extend_from_slice(&self.pixels[start..end]);
        }
        Self::new(Size::new(crop.size.width, crop.size.height), pixels)
    }

    pub fn resize_nearest(&self, target: Size) -> Result<Self> {
        if target.width == 0 || target.height == 0 {
            return Err(VshotError::InvalidGeometry(
                "resized frame dimensions must be greater than zero".into(),
            ));
        }
        let target_width = usize::try_from(target.width)
            .map_err(|_| VshotError::InvalidGeometry("resized frame is too large".into()))?;
        let target_height = usize::try_from(target.height)
            .map_err(|_| VshotError::InvalidGeometry("resized frame is too large".into()))?;
        let source_width = usize::try_from(self.size.width)
            .map_err(|_| VshotError::InvalidGeometry("source frame is too large".into()))?;
        let source_height = usize::try_from(self.size.height)
            .map_err(|_| VshotError::InvalidGeometry("source frame is too large".into()))?;
        if source_width == 0 || source_height == 0 {
            return Err(VshotError::InvalidGeometry(
                "source frame dimensions must be greater than zero".into(),
            ));
        }
        let pixel_count = target_width
            .checked_mul(target_height)
            .and_then(|area| area.checked_mul(4))
            .ok_or_else(|| VshotError::InvalidGeometry("resized frame is too large".into()))?;
        let mut pixels = vec![0u8; pixel_count];
        for target_y in 0..target_height {
            let source_y =
                ((target_y as u128 * source_height as u128) / target_height as u128) as usize;
            for target_x in 0..target_width {
                let source_x =
                    ((target_x as u128 * source_width as u128) / target_width as u128) as usize;
                let source = (source_y * source_width + source_x) * 4;
                let destination = (target_y * target_width + target_x) * 4;
                pixels[destination..destination + 4]
                    .copy_from_slice(&self.pixels[source..source + 4]);
            }
        }
        Self::new(target, pixels)
    }

    pub(crate) fn stroke_rectangle(
        &mut self,
        rect: Rect,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    ) -> Result<()> {
        let (left, top, right, bottom) = checked_rect_bounds(rect)?;
        validate_stroke_width(width)?;
        let frame = self.size;
        if dash != LineDash::Solid {
            // Walk the band centerline so dashed squares cover the same pixels
            // the solid band would.
            let inset_left = left + i64::from(width - 1) / 2;
            let inset_top = top + i64::from(width - 1) / 2;
            let inset_right = (right - i64::from(width) / 2).max(inset_left + 1);
            let inset_bottom = (bottom - i64::from(width) / 2).max(inset_top + 1);
            let path = [
                Point::new(inset_left as i32, inset_top as i32),
                Point::new(inset_right as i32 - 1, inset_top as i32),
                Point::new(inset_right as i32 - 1, inset_bottom as i32 - 1),
                Point::new(inset_left as i32, inset_bottom as i32 - 1),
                Point::new(inset_left as i32, inset_top as i32),
            ];
            let bounds = polyline_bounds(&path, i64::from(width) + 1);
            stroke_with_coverage(self, color, bounds, |ink| {
                rasterize_dashed_polyline(ink, frame, &path, width, dash);
            });
            return Ok(());
        }
        stroke_with_coverage(self, color, (left, top, right, bottom), |ink| {
            rasterize_rect_border(ink, frame, left, top, right, bottom, width);
        });
        Ok(())
    }

    pub(crate) fn stroke_circle(
        &mut self,
        center: Point,
        radius: u32,
        color: [u8; 4],
        width: u32,
    ) -> Result<()> {
        if radius == 0 {
            return Err(VshotError::InvalidGeometry(
                "circle radius must be greater than zero".into(),
            ));
        }
        validate_stroke_width(width)?;
        let frame = self.size;
        let stroke_width = i128::from(width);
        let outer_radius = i128::from(radius) + (stroke_width + 1) / 2;
        let center_x = i128::from(center.x);
        let center_y = i128::from(center.y);
        let bounds = (
            clamp_i64(center_x - outer_radius),
            clamp_i64(center_y - outer_radius),
            clamp_i64(center_x + outer_radius + 1),
            clamp_i64(center_y + outer_radius + 1),
        );
        stroke_with_coverage(self, color, bounds, |ink| {
            rasterize_circle(ink, frame, center, radius, width);
        });
        Ok(())
    }

    pub(crate) fn stroke_ellipse(
        &mut self,
        rect: Rect,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    ) -> Result<()> {
        let (left, top, right, bottom) = checked_rect_bounds(rect)?;
        validate_stroke_width(width)?;
        let frame = self.size;
        let rect_width = i128::from(right - left);
        let rect_height = i128::from(bottom - top);
        let stroke_width = i128::from(width);
        let center_x = i128::from(left) + rect_width / 2;
        let center_y = i128::from(top) + rect_height / 2;
        if dash != LineDash::Solid {
            // The dashed walk stamps `width`-wide squares on the ring, so the
            // mask has to reach the ring midline plus that radius.
            let half_width = rect_width / 2;
            let half_height = rect_height / 2;
            let pad = stroke_width + 1;
            let bounds = (
                clamp_i64(center_x - half_width - pad),
                clamp_i64(center_y - half_height - pad),
                clamp_i64(center_x + half_width + pad),
                clamp_i64(center_y + half_height + pad),
            );
            stroke_with_coverage(self, color, bounds, |ink| {
                rasterize_dashed_ellipse(ink, frame, (left, top, right, bottom), width, dash);
            });
            return Ok(());
        }
        let outer_a = (rect_width + stroke_width) / 2;
        let outer_b = (rect_height + stroke_width) / 2;
        let bounds = (
            clamp_i64(center_x - outer_a),
            clamp_i64(center_y - outer_b),
            clamp_i64(center_x + outer_a + 1),
            clamp_i64(center_y + outer_b + 1),
        );
        stroke_with_coverage(self, color, bounds, |ink| {
            rasterize_ellipse(ink, frame, left, top, right, bottom, width);
        });
        Ok(())
    }

    pub(crate) fn draw_arrow(
        &mut self,
        start: Point,
        end: Point,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
        head: u32,
    ) -> Result<()> {
        self.draw_arrow_with_style(start, end, color, width, dash, head, ArrowStyle::Open)
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn draw_arrow_with_style(
        &mut self,
        start: Point,
        end: Point,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
        head: u32,
        style: ArrowStyle,
    ) -> Result<()> {
        validate_stroke_width(width)?;
        let dx = f64::from(end.x) - f64::from(start.x);
        let dy = f64::from(end.y) - f64::from(start.y);
        let length = dx.hypot(dy);
        if length == 0.0 {
            return Err(VshotError::InvalidGeometry(
                "arrow endpoints must be different".into(),
            ));
        }
        let unit_x = dx / length;
        let unit_y = dy / length;
        let head_size = f64::from(head.clamp(1, 8));
        let head_length = (6.0_f64.max(f64::from(width) * 4.0) * head_size).min(length);
        let wing_length = (head_length * 0.55).max(f64::from(width));
        let base_x = f64::from(end.x) - unit_x * head_length;
        let base_y = f64::from(end.y) - unit_y * head_length;
        let perpendicular_x = -unit_y;
        let perpendicular_y = unit_x;
        let left = point_from_f64(
            base_x + perpendicular_x * wing_length,
            base_y + perpendicular_y * wing_length,
        );
        let right = point_from_f64(
            base_x - perpendicular_x * wing_length,
            base_y - perpendicular_y * wing_length,
        );

        // The stem, the filled head and both wings all meet at the tip, so one
        // mask spans them: a translucent arrow then keeps a single alpha there
        // instead of darkening where the pieces overlap.
        let frame = self.size;
        let bounds = polyline_bounds(&[start, end, left, right], i64::from(width) + 1);
        stroke_with_coverage(self, color, bounds, |ink| {
            if dash == LineDash::Solid {
                rasterize_capsule(ink, frame, start, end, width);
            } else {
                rasterize_dashed_polyline(ink, frame, &[start, end], width, dash);
            }
            if style == ArrowStyle::Filled {
                rasterize_triangle(ink, frame, end, left, right);
            }
            rasterize_capsule(ink, frame, end, left, width);
            rasterize_capsule(ink, frame, end, right, width);
        });
        Ok(())
    }

    pub(crate) fn draw_freehand(
        &mut self,
        points: &[Point],
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    ) -> Result<()> {
        validate_stroke_width(width)?;
        if points.is_empty() {
            return Err(VshotError::InvalidGeometry(
                "freehand path must contain at least one point".into(),
            ));
        }
        let frame = self.size;
        if dash != LineDash::Solid && points.len() >= 2 {
            let bounds = polyline_bounds(points, i64::from(width) + 1);
            stroke_with_coverage(self, color, bounds, |ink| {
                rasterize_dashed_polyline(ink, frame, points, width, dash);
            });
            return Ok(());
        }
        let capsule_pad = i64::from(width.div_ceil(2)) + 1;
        if points.len() == 1 {
            let point = points[0];
            stroke_with_coverage(self, color, polyline_bounds(points, capsule_pad), |ink| {
                rasterize_capsule(ink, frame, point, point, width);
            });
            return Ok(());
        }
        // Successive capsules share every joint; the mask unions them, so a
        // translucent stroke keeps one alpha through the corner.
        stroke_with_coverage(self, color, polyline_bounds(points, capsule_pad), |ink| {
            for segment in points.windows(2) {
                rasterize_capsule(ink, frame, segment[0], segment[1], width);
            }
        });
        Ok(())
    }

    /// Draws a sine wave along the straight segment `start..end`.
    ///
    /// `width` is the device stroke width. `amplitude` (peak deviation from the
    /// centre line) and `wavelength` (one full period along the line) are in
    /// device pixels too, but the caller derives them from the *logical* stroke
    /// width — `amplitude = max(width * 2, 4)`, `wavelength = max(width * 6, 18)`
    /// — and then scales them up. That order matters: the `max` floors apply to
    /// the logical width, and the Qt preview computes them the same way, so a
    /// high-density capture has to keep the same wave shape.
    ///
    /// The wave is sampled every device pixel along the line (see
    /// [`wave_polyline`]) and rasterized through the shared coverage path, so a
    /// translucent wave keeps a single alpha through its dense turnarounds. A
    /// zero-length segment degenerates to a single dot, mirroring the pen.
    pub(crate) fn draw_wave(
        &mut self,
        start: Point,
        end: Point,
        color: [u8; 4],
        width: u32,
        amplitude: u32,
        wavelength: u32,
    ) -> Result<()> {
        validate_stroke_width(width)?;
        let frame = self.size;
        let points = wave_polyline(start, end, amplitude, wavelength);
        let pad = i64::from(width.div_ceil(2)) + 1;
        let bounds = polyline_bounds(&points, pad);
        stroke_with_coverage(self, color, bounds, |ink| {
            if points.len() == 1 {
                rasterize_capsule(ink, frame, start, start, width);
                return;
            }
            for segment in points.windows(2) {
                rasterize_capsule(ink, frame, segment[0], segment[1], width);
            }
        });
        Ok(())
    }

    /// Draws a pen-tool cubic Bezier path.
    ///
    /// `points` is interleaved — anchor, out-handle, anchor, out-handle — in
    /// device pixels; the in-handle of an anchor is the mirror of its
    /// out-handle about the anchor. The curve is flattened by adaptive de
    /// Casteljau subdivision (see [`bezier_polyline`]) and stroked through the
    /// shared coverage path, so a translucent path keeps one alpha through its
    /// joins. When `closed`, a straight segment joins the last anchor back to
    /// the first, matching the Qt side's `closeSubpath`.
    ///
    /// `fill` picks which parts get painted. Under [`BezierFill::Both`] the
    /// historical behaviour stands: a closed path has its enclosed area filled
    /// before the stroke — matching the Qt side's `fillPath`-then-`strokePath`
    /// order — with half the stroke's alpha, and an open path is stroked alone.
    /// [`BezierFill::Stroke`] never fills, even a closed path, and
    /// [`BezierFill::Fill`] paints a closed path's interior without its
    /// outline. No mode fills an open path: that would close it implicitly, so
    /// a shape the user never closed would appear whole.
    pub(crate) fn draw_bezier(
        &mut self,
        points: &[Point],
        closed: bool,
        fill: BezierFill,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    ) -> Result<()> {
        validate_stroke_width(width)?;
        let frame = self.size;
        let Some(&start) = points.first() else {
            return Ok(());
        };
        let mut polyline = bezier_polyline(points);
        if closed {
            // closeSubpath: a straight segment back to the first anchor.
            polyline.push(start);
        }
        if fill.fills(closed) && polyline.len() >= 3 {
            // "Translucent fill, solid stroke": the fill keeps the stroke's
            // colour at half its alpha, floored, exactly as Qt computes it.
            // Only a closed path gets here, so `fill_polygon`'s implicit
            // closure of the outline is never what the user sees.
            let mut fill_color = color;
            fill_color[3] = color[3] / 2;
            self.fill_polygon(&polyline, fill_color);
        }
        if !fill.strokes(closed) {
            // `Fill` on a closed path: the outline itself is left unpainted.
            return Ok(());
        }
        let pad = i64::from(width.div_ceil(2)) + 1;
        let bounds = polyline_bounds(&polyline, pad);
        if polyline.len() == 1 {
            stroke_with_coverage(self, color, bounds, |ink| {
                rasterize_capsule(ink, frame, polyline[0], polyline[0], width);
            });
            return Ok(());
        }
        stroke_with_coverage(self, color, bounds, |ink| {
            if dash == LineDash::Solid {
                for segment in polyline.windows(2) {
                    rasterize_capsule(ink, frame, segment[0], segment[1], width);
                }
            } else {
                rasterize_dashed_polyline(ink, frame, &polyline, width, dash);
            }
        });
        Ok(())
    }

    /// Fills the polygon `points` with `color`, source-over.
    ///
    /// The rule is even-odd — the default `QPainterPath` fill rule on the Qt
    /// side — so a self-intersecting outline fills its odd winding and no pixel
    /// is ever composited twice. Each pixel row is sampled at its centre
    /// (`y + 0.5`); the crossings of that horizontal line with every
    /// non-horizontal edge are sorted and the spans between successive pairs are
    /// filled. Sampling at pixel centres and treating each edge as half-open in
    /// `y` counts a shared vertex exactly once. The polygon is closed
    /// implicitly, even when the last point does not repeat the first.
    pub(crate) fn fill_polygon(&mut self, points: &[Point], color: [u8; 4]) {
        if points.len() < 3 || color[3] == 0 {
            return;
        }
        let frame_width = i64::from(self.size.width);
        let frame_height = i64::from(self.size.height);
        let mut min_y = i64::from(points[0].y);
        let mut max_y = min_y;
        for point in points {
            min_y = min_y.min(i64::from(point.y));
            max_y = max_y.max(i64::from(point.y));
        }
        let first_row = min_y.max(0);
        let last_row = max_y.min(frame_height - 1);
        if first_row > last_row {
            return;
        }
        let mut crossings: Vec<f64> = Vec::new();
        let edges = points.len();
        for row in first_row..=last_row {
            let scan = row as f64 + 0.5;
            crossings.clear();
            for index in 0..edges {
                let a = points[index];
                let b = points[(index + 1) % edges];
                let (ay, by) = (f64::from(a.y), f64::from(b.y));
                let (low, high) = if ay <= by { (ay, by) } else { (by, ay) };
                // Half-open in y: an edge owns its lower end, not its upper, so
                // a vertex shared by two edges crosses the scanline once. A
                // horizontal edge (low == high) is skipped.
                if scan < low || scan >= high {
                    continue;
                }
                let t = (scan - ay) / (by - ay);
                crossings.push(f64::from(a.x) + t * (f64::from(b.x) - f64::from(a.x)));
            }
            crossings.sort_by(f64::total_cmp);
            let mut pair = 0;
            while pair + 1 < crossings.len() {
                let span_start = crossings[pair];
                let span_end = crossings[pair + 1];
                pair += 2;
                // The pixel centre x + 0.5 must lie in [span_start, span_end).
                let first_x = ((span_start - 0.5).ceil() as i64).max(0);
                let last_x = (((span_end - 0.5).ceil() as i64) - 1).min(frame_width - 1);
                for x in first_x..=last_x {
                    self.blend_pixel_at(x, row, color);
                }
            }
        }
    }

    pub(crate) fn draw_text(
        &mut self,
        origin: Point,
        text: &str,
        color: [u8; 4],
        scale: u32,
    ) -> Result<()> {
        if scale == 0 {
            return Err(VshotError::InvalidGeometry(
                "text scale must be greater than zero".into(),
            ));
        }
        for byte in text.bytes() {
            if byte != b'\n' && !(0x20..=0x7e).contains(&byte) {
                return Err(VshotError::UnsupportedText(format!(
                    "character 0x{byte:02x} is not supported; only printable ASCII and newline are supported"
                )));
            }
        }

        let scale = i64::from(scale);
        let advance = scale
            .checked_mul(6)
            .ok_or_else(|| VshotError::InvalidGeometry("text advance is too large".into()))?;
        let line_height = scale
            .checked_mul(8)
            .ok_or_else(|| VshotError::InvalidGeometry("text line height is too large".into()))?;
        let mut cursor_x = i64::from(origin.x);
        let mut cursor_y = i64::from(origin.y);
        for byte in text.bytes() {
            if byte == b'\n' {
                cursor_x = i64::from(origin.x);
                cursor_y = cursor_y
                    .checked_add(line_height)
                    .ok_or_else(|| VshotError::InvalidGeometry("text position overflows".into()))?;
                continue;
            }
            let glyph = ascii_glyph(byte).ok_or_else(|| {
                VshotError::UnsupportedText(format!(
                    "character 0x{byte:02x} is not supported; only printable ASCII is supported"
                ))
            })?;
            for (row, bits) in glyph.iter().enumerate() {
                let row_offset = (row as i64)
                    .checked_mul(scale)
                    .ok_or_else(|| VshotError::InvalidGeometry("text position overflows".into()))?;
                for column in 0..5_i64 {
                    if bits & (1 << (4 - column)) == 0 {
                        continue;
                    }
                    let column_offset = column.checked_mul(scale).ok_or_else(|| {
                        VshotError::InvalidGeometry("text position overflows".into())
                    })?;
                    let pixel_x = cursor_x.checked_add(column_offset).ok_or_else(|| {
                        VshotError::InvalidGeometry("text position overflows".into())
                    })?;
                    let pixel_y = cursor_y.checked_add(row_offset).ok_or_else(|| {
                        VshotError::InvalidGeometry("text position overflows".into())
                    })?;
                    let pixel_x_end = pixel_x.checked_add(scale).ok_or_else(|| {
                        VshotError::InvalidGeometry("text position overflows".into())
                    })?;
                    let pixel_y_end = pixel_y.checked_add(scale).ok_or_else(|| {
                        VshotError::InvalidGeometry("text position overflows".into())
                    })?;
                    let x_start = pixel_x.max(0);
                    let x_end = pixel_x_end.min(i64::from(self.size.width));
                    let y_start = pixel_y.max(0);
                    let y_end = pixel_y_end.min(i64::from(self.size.height));
                    if x_start >= x_end || y_start >= y_end {
                        continue;
                    }
                    for y in y_start..y_end {
                        for x in x_start..x_end {
                            self.blend_pixel_at(x, y, color);
                        }
                    }
                }
            }
            cursor_x = cursor_x
                .checked_add(advance)
                .ok_or_else(|| VshotError::InvalidGeometry("text position overflows".into()))?;
        }
        Ok(())
    }

    pub(crate) fn draw_bitmap(&mut self, origin: Point, bitmap: &TextBitmap) -> Result<()> {
        if bitmap.width == 0 || bitmap.height == 0 {
            return Ok(());
        }
        let expected = (usize::try_from(bitmap.width).ok())
            .and_then(|width| width.checked_mul(usize::try_from(bitmap.height).ok()?))
            .and_then(|pixels| pixels.checked_mul(4));
        let expected = expected
            .ok_or_else(|| VshotError::InvalidGeometry("text bitmap is too large".into()))?;
        if bitmap.pixels.len() != expected {
            return Err(VshotError::InvalidGeometry(
                "text bitmap payload does not match its dimensions".into(),
            ));
        }
        let stride = bitmap.width as usize * 4;
        for row in 0..i64::from(bitmap.height) {
            for column in 0..i64::from(bitmap.width) {
                let offset = row as usize * stride + column as usize * 4;
                let Some(source) = bitmap.pixels.get(offset..offset + 4) else {
                    continue;
                };
                if source[3] == 0 {
                    continue;
                }
                self.blend_pixel_at(
                    i64::from(origin.x) + column,
                    i64::from(origin.y) + row,
                    [source[0], source[1], source[2], source[3]],
                );
            }
        }
        Ok(())
    }

    /// Composites a pasted image into `rect`, rescaling it to that rect.
    ///
    /// Each destination pixel averages the source pixels it covers, weighted by
    /// alpha, so a photo pasted at half size comes out as a proper reduction
    /// rather than a nearest-neighbour sample of it. A rect equal to the
    /// bitmap's own size takes the direct path: that is the common case when a
    /// pasted image is not resized at all.
    pub(crate) fn draw_bitmap_scaled(&mut self, rect: Rect, bitmap: &TextBitmap) -> Result<()> {
        if bitmap.width == 0 || bitmap.height == 0 || rect.size.width == 0 || rect.size.height == 0
        {
            return Ok(());
        }
        let expected = (usize::try_from(bitmap.width).ok())
            .and_then(|width| width.checked_mul(usize::try_from(bitmap.height).ok()?))
            .and_then(|pixels| pixels.checked_mul(4));
        let expected = expected
            .ok_or_else(|| VshotError::InvalidGeometry("pasted image is too large".into()))?;
        if bitmap.pixels.len() != expected {
            return Err(VshotError::InvalidGeometry(
                "pasted image payload does not match its dimensions".into(),
            ));
        }
        if rect.size.width == bitmap.width && rect.size.height == bitmap.height {
            return self.draw_bitmap(Point::new(rect.origin.x, rect.origin.y), bitmap);
        }
        let (left, top, right, bottom) = checked_rect_bounds(rect)?;
        let Some((visible_left, visible_right)) = clip_range(left, right, self.size.width) else {
            return Ok(());
        };
        let Some((visible_top, visible_bottom)) = clip_range(top, bottom, self.size.height) else {
            return Ok(());
        };
        let source_width = u64::from(bitmap.width);
        let source_height = u64::from(bitmap.height);
        let target_width = u64::from(rect.size.width);
        let target_height = u64::from(rect.size.height);
        let stride = bitmap.width as usize * 4;
        for y in visible_top..visible_bottom {
            let target_row = (y - top) as u64;
            let (source_top, source_bottom) =
                covered_span(target_row, bitmap.height, source_height, target_height);
            for x in visible_left..visible_right {
                let target_column = (x - left) as u64;
                let (source_left, source_right) =
                    covered_span(target_column, bitmap.width, source_width, target_width);
                let mut alpha_sum = 0u64;
                let mut weighted = [0u64; 3];
                let mut count = 0u64;
                for row in source_top..source_bottom {
                    for column in source_left..source_right {
                        let offset = row as usize * stride + column as usize * 4;
                        let Some(source) = bitmap.pixels.get(offset..offset + 4) else {
                            continue;
                        };
                        let alpha = u64::from(source[3]);
                        alpha_sum += alpha;
                        for (channel, value) in weighted.iter_mut().enumerate() {
                            *value += u64::from(source[channel]) * alpha;
                        }
                        count += 1;
                    }
                }
                if count == 0 || alpha_sum == 0 {
                    continue;
                }
                // Colour is divided by the alpha it accumulated rather than by
                // the pixel count, so the transparent pixels around an edge do
                // not drag the edge's colour toward black.
                let alpha = (alpha_sum / count) as u8;
                let color = [
                    (weighted[0] / alpha_sum) as u8,
                    (weighted[1] / alpha_sum) as u8,
                    (weighted[2] / alpha_sum) as u8,
                    alpha,
                ];
                self.blend_pixel_at(x, y, color);
            }
        }
        Ok(())
    }

    pub(crate) fn mosaic(&mut self, rect: Rect, block_size: u32) -> Result<()> {
        let (left, top, right, bottom) = checked_rect_bounds(rect)?;
        if block_size == 0 {
            return Err(VshotError::InvalidGeometry(
                "mosaic block size must be greater than zero".into(),
            ));
        }
        let Some((visible_left, visible_right)) = clip_range(left, right, self.size.width) else {
            return Ok(());
        };
        let Some((visible_top, visible_bottom)) = clip_range(top, bottom, self.size.height) else {
            return Ok(());
        };
        let block = i64::from(block_size);
        let first_x = left
            .checked_add(
                (visible_left - left)
                    .div_euclid(block)
                    .checked_mul(block)
                    .ok_or_else(|| {
                        VshotError::InvalidGeometry("mosaic block position overflows".into())
                    })?,
            )
            .ok_or_else(|| VshotError::InvalidGeometry("mosaic block position overflows".into()))?;
        let first_y = top
            .checked_add(
                (visible_top - top)
                    .div_euclid(block)
                    .checked_mul(block)
                    .ok_or_else(|| {
                        VshotError::InvalidGeometry("mosaic block position overflows".into())
                    })?,
            )
            .ok_or_else(|| VshotError::InvalidGeometry("mosaic block position overflows".into()))?;

        let mut block_y = first_y;
        while block_y < visible_bottom {
            let block_bottom = block_y.checked_add(block).ok_or_else(|| {
                VshotError::InvalidGeometry("mosaic block position overflows".into())
            })?;
            let y_start = block_y.max(visible_top);
            let y_end = block_bottom.min(visible_bottom);
            let mut block_x = first_x;
            while block_x < visible_right {
                let block_right = block_x.checked_add(block).ok_or_else(|| {
                    VshotError::InvalidGeometry("mosaic block position overflows".into())
                })?;
                let x_start = block_x.max(visible_left);
                let x_end = block_right.min(visible_right);
                let width = u128::try_from(x_end - x_start).map_err(|_| {
                    VshotError::InvalidGeometry("mosaic block width is out of range".into())
                })?;
                let height = u128::try_from(y_end - y_start).map_err(|_| {
                    VshotError::InvalidGeometry("mosaic block height is out of range".into())
                })?;
                let count = width.checked_mul(height).ok_or_else(|| {
                    VshotError::InvalidGeometry("mosaic block is too large".into())
                })?;
                let mut sums = [0u128; 4];
                for y in y_start..y_end {
                    for x in x_start..x_end {
                        let index = self.pixel_index_at(x, y).ok_or_else(|| {
                            VshotError::InvalidGeometry(
                                "mosaic pixel position is out of range".into(),
                            )
                        })?;
                        for (channel, sum) in sums.iter_mut().enumerate() {
                            *sum += u128::from(self.pixels[index + channel]);
                        }
                    }
                }
                let mut average = [0u8; 4];
                for (channel, value) in average.iter_mut().enumerate() {
                    *value = u8::try_from((sums[channel] + count / 2) / count).map_err(|_| {
                        VshotError::InvalidGeometry("mosaic average is out of range".into())
                    })?;
                }
                for y in y_start..y_end {
                    for x in x_start..x_end {
                        let index = self.pixel_index_at(x, y).ok_or_else(|| {
                            VshotError::InvalidGeometry(
                                "mosaic pixel position is out of range".into(),
                            )
                        })?;
                        self.pixels[index..index + 4].copy_from_slice(&average);
                    }
                }
                block_x = block_right;
            }
            block_y = block_bottom;
        }
        Ok(())
    }

    /// Pixelates the region inside the ellipse inscribed in `rect`, using the
    /// same rect-aligned block grid as [`Frame::mosaic`].  Boundary blocks are
    /// averaged and written per pixel so the ellipse edge stays smooth.
    pub(crate) fn mosaic_ellipse(&mut self, rect: Rect, block_size: u32) -> Result<()> {
        let (left, top, right, bottom) = checked_rect_bounds(rect)?;
        if block_size == 0 {
            return Err(VshotError::InvalidGeometry(
                "mosaic block size must be greater than zero".into(),
            ));
        }
        let Some((visible_left, visible_right)) = clip_range(left, right, self.size.width) else {
            return Ok(());
        };
        let Some((visible_top, visible_bottom)) = clip_range(top, bottom, self.size.height) else {
            return Ok(());
        };
        let block = i64::from(block_size);
        let center_x = left + (right - left) / 2;
        let center_y = top + (bottom - top) / 2;
        let a = i128::from((right - left).max(2) / 2);
        let b = i128::from((bottom - top).max(2) / 2);
        let a_squared = a * a;
        let b_squared = b * b;
        let threshold = a_squared * b_squared;
        let inside = |x: i64, y: i64| -> bool {
            let dx = i128::from(x) - i128::from(center_x);
            let dy = i128::from(y) - i128::from(center_y);
            dx * dx * b_squared + dy * dy * a_squared <= threshold
        };

        let first_x = left + (visible_left - left) / block * block;
        let first_y = top + (visible_top - top) / block * block;
        let mut block_y = first_y;
        while block_y < visible_bottom {
            let block_bottom = block_y + block;
            let y_start = block_y.max(visible_top);
            let y_end = block_bottom.min(visible_bottom);
            let mut block_x = first_x;
            while block_x < visible_right {
                let block_right = block_x + block;
                let x_start = block_x.max(visible_left);
                let x_end = block_right.min(visible_right);
                // The ellipse interior is convex, so a block whose corners are
                // all inside is fully inside and skips the per-pixel test.
                let fully_inside = inside(x_start, y_start)
                    && inside(x_end - 1, y_start)
                    && inside(x_start, y_end - 1)
                    && inside(x_end - 1, y_end - 1);
                let mut sums = [0u128; 4];
                let mut count = 0u128;
                for y in y_start..y_end {
                    for x in x_start..x_end {
                        if fully_inside || inside(x, y) {
                            let index = self.pixel_index_at(x, y).ok_or_else(|| {
                                VshotError::InvalidGeometry(
                                    "mosaic pixel position is out of range".into(),
                                )
                            })?;
                            for (channel, sum) in sums.iter_mut().enumerate() {
                                *sum += u128::from(self.pixels[index + channel]);
                            }
                            count += 1;
                        }
                    }
                }
                if count > 0 {
                    // Round half up to match the rectangular mosaic's block
                    // averaging exactly.
                    let half = count / 2;
                    let mut average = [0u8; 4];
                    for (channel, value) in average.iter_mut().enumerate() {
                        *value = u8::try_from((sums[channel] + half) / count).map_err(|_| {
                            VshotError::InvalidGeometry("mosaic average is out of range".into())
                        })?;
                    }
                    for y in y_start..y_end {
                        for x in x_start..x_end {
                            if fully_inside || inside(x, y) {
                                let index = self.pixel_index_at(x, y).ok_or_else(|| {
                                    VshotError::InvalidGeometry(
                                        "mosaic pixel position is out of range".into(),
                                    )
                                })?;
                                self.pixels[index..index + 4].copy_from_slice(&average);
                            }
                        }
                    }
                }
                block_x = block_right;
            }
            block_y = block_bottom;
        }
        Ok(())
    }

    /// Smears mosaic stamps of `radius` device pixels along the path.  Every
    /// stamp is averaged from a pre-mosaic snapshot, so overlapping stamps stay
    /// deterministic regardless of application order.
    pub(crate) fn mosaic_brush(&mut self, points: &[Point], radius: u32) -> Result<()> {
        if points.is_empty() {
            return Err(VshotError::InvalidGeometry(
                "mosaic brush path must contain at least one point".into(),
            ));
        }
        if radius == 0 {
            return Err(VshotError::InvalidGeometry(
                "mosaic brush radius must be greater than zero".into(),
            ));
        }
        let source = self.pixels.clone();
        let radius = i64::from(radius.min(512));
        let step = (radius / 2).max(1) as f64;
        let mut centers: Vec<(i64, i64)> = Vec::with_capacity(points.len());
        centers.push((i64::from(points[0].x), i64::from(points[0].y)));
        for segment in points.windows(2) {
            let ax = f64::from(segment[0].x);
            let ay = f64::from(segment[0].y);
            let bx = f64::from(segment[1].x);
            let by = f64::from(segment[1].y);
            let length = (bx - ax).hypot(by - ay);
            let count = ((length / step).ceil() as usize).max(1);
            for k in 1..=count {
                let t = k as f64 / count as f64;
                centers.push((
                    (ax + (bx - ax) * t).round() as i64,
                    (ay + (by - ay) * t).round() as i64,
                ));
            }
        }

        let width = i64::from(self.size.width);
        let height = i64::from(self.size.height);
        let radius_squared = radius * radius;
        let source_index = |x: i64, y: i64| -> Option<usize> {
            if x < 0 || y < 0 || x >= width || y >= height {
                return None;
            }
            usize::try_from((y * width + x) * 4).ok()
        };
        for (center_x, center_y) in centers {
            let mut sums = [0u64; 4];
            let mut count = 0u64;
            for dy in -radius..=radius {
                for dx in -radius..=radius {
                    if dx * dx + dy * dy > radius_squared {
                        continue;
                    }
                    if let Some(index) = source_index(center_x + dx, center_y + dy) {
                        for (channel, sum) in sums.iter_mut().enumerate() {
                            *sum += u64::from(source[index + channel]);
                        }
                        count += 1;
                    }
                }
            }
            if count == 0 {
                continue;
            }
            // Round half up to match the rectangular mosaic's block averaging.
            let half = count / 2;
            let mut average = [0u8; 4];
            for (channel, value) in average.iter_mut().enumerate() {
                *value = u8::try_from((sums[channel] + half) / count).map_err(|_| {
                    VshotError::InvalidGeometry("mosaic average is out of range".into())
                })?;
            }
            for dy in -radius..=radius {
                for dx in -radius..=radius {
                    if dx * dx + dy * dy > radius_squared {
                        continue;
                    }
                    if let Some(index) = self.pixel_index_at(center_x + dx, center_y + dy) {
                        self.pixels[index..index + 4].copy_from_slice(&average);
                    }
                }
            }
        }
        Ok(())
    }

    fn pixel_index_at(&self, x: i64, y: i64) -> Option<usize> {
        if x < 0 || y < 0 || x >= i64::from(self.size.width) || y >= i64::from(self.size.height) {
            return None;
        }
        let x = usize::try_from(x).ok()?;
        let y = usize::try_from(y).ok()?;
        let width = usize::try_from(self.size.width).ok()?;
        y.checked_mul(width)?.checked_add(x)?.checked_mul(4)
    }

    pub(crate) fn blend_pixel_at(&mut self, x: i64, y: i64, color: [u8; 4]) {
        let Some(index) = self.pixel_index_at(x, y) else {
            return;
        };
        blend_source_over(&mut self.pixels[index..index + 4], color);
    }

    pub fn copy_rgba_to_bgra(
        &self,
        destination: &mut [u8],
        destination_stride: usize,
        destination_origin: Point,
    ) -> Result<()> {
        if destination_origin.x < 0 || destination_origin.y < 0 {
            return Err(VshotError::InvalidGeometry(
                "destination origin must be non-negative".into(),
            ));
        }
        let x = destination_origin.x as usize;
        let y = destination_origin.y as usize;
        let width = self.size.width as usize;
        let height = self.size.height as usize;
        let row_bytes = width
            .checked_mul(4)
            .ok_or_else(|| VshotError::InvalidGeometry("destination row is too large".into()))?;
        let required = y
            .checked_add(height.saturating_sub(1))
            .and_then(|last_row| last_row.checked_mul(destination_stride))
            .and_then(|row_start| row_start.checked_add(x.checked_mul(4)?))
            .and_then(|row_start| row_start.checked_add(row_bytes))
            .ok_or_else(|| VshotError::InvalidGeometry("destination buffer is too large".into()))?;
        if destination_stride < row_bytes || required > destination.len() {
            return Err(VshotError::InvalidGeometry(
                "destination buffer is too small".into(),
            ));
        }
        for row in 0..height {
            let source_row = row
                .checked_mul(row_bytes)
                .ok_or_else(|| VshotError::InvalidGeometry("source row is too large".into()))?;
            let target_row = y
                .checked_add(row)
                .and_then(|row| row.checked_mul(destination_stride))
                .and_then(|row_start| row_start.checked_add(x.checked_mul(4)?))
                .ok_or_else(|| {
                    VshotError::InvalidGeometry("destination row is too large".into())
                })?;
            for column in 0..width {
                let offset = column.checked_mul(4).ok_or_else(|| {
                    VshotError::InvalidGeometry("pixel offset is too large".into())
                })?;
                let source = source_row + offset;
                let target = target_row + offset;
                destination[target] = self.pixels[source + 2];
                destination[target + 1] = self.pixels[source + 1];
                destination[target + 2] = self.pixels[source];
                destination[target + 3] = self.pixels[source + 3];
            }
        }
        Ok(())
    }
}

/// How hard the PNG encoder works to shrink the file. Every level is
/// lossless: they trade encoding time for size only.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum PngCompression {
    /// No DEFLATE and no row filtering. The largest files, written about as
    /// fast as the pixels can be copied.
    None,
    /// fdeflate with a single fixed row filter: almost as fast as `None` and
    /// a little smaller.
    Fastest,
    /// fdeflate with adaptive row filtering. Nearly as fast as `Fastest` and
    /// noticeably smaller, which is why it is the default — a 4K frame is
    /// still written in tens of milliseconds.
    #[default]
    Fast,
    /// DEFLATE level 6 with adaptive filtering, the level most PNG writers
    /// default to: roughly a quarter smaller than `Fast` and an order of
    /// magnitude slower.
    Balanced,
    /// DEFLATE level 9: the smallest files, at several times `Balanced`'s cost
    /// and for almost no additional gain.
    High,
}

impl PngCompression {
    /// Parses the `compression` value PNG's codec declares — the one
    /// `--format-param png.compression` and `cli.format.png.compression` carry.
    pub fn parse(value: &str) -> Result<Self> {
        match value {
            "none" => Ok(Self::None),
            "fastest" => Ok(Self::Fastest),
            "fast" => Ok(Self::Fast),
            "balanced" => Ok(Self::Balanced),
            "high" => Ok(Self::High),
            other => Err(VshotError::InvalidDestination(format!(
                "`{other}` is not a PNG compression level: expected one of \
                 none, fastest, fast, balanced, high"
            ))),
        }
    }
}

impl From<PngCompression> for png::Compression {
    fn from(value: PngCompression) -> Self {
        match value {
            PngCompression::None => Self::NoCompression,
            PngCompression::Fastest => Self::Fastest,
            PngCompression::Fast => Self::Fast,
            PngCompression::Balanced => Self::Balanced,
            PngCompression::High => Self::High,
        }
    }
}

/// A device density as PNG physical resolution: 96 DPI per density step, which
/// is the convention Qt reports back as dots per metre, so a 2x capture reads
/// as 192 DPI on the other side. Clamped to the densities the renderer knows.
fn density_to_pixels_per_meter(density: u32) -> u32 {
    let dpi = 96.0 * f64::from(density.clamp(1, 4));
    (dpi / 0.0254).round() as u32
}

fn checked_rect_bounds(rect: Rect) -> Result<(i64, i64, i64, i64)> {
    if rect.is_empty() {
        return Err(VshotError::InvalidGeometry(
            "drawing rectangle dimensions must be greater than zero".into(),
        ));
    }
    let left = i64::from(rect.origin.x);
    let top = i64::from(rect.origin.y);
    let right = left
        .checked_add(i64::from(rect.size.width))
        .ok_or_else(|| {
            VshotError::InvalidGeometry("drawing rectangle right edge overflows".into())
        })?;
    let bottom = top
        .checked_add(i64::from(rect.size.height))
        .ok_or_else(|| {
            VshotError::InvalidGeometry("drawing rectangle bottom edge overflows".into())
        })?;
    Ok((left, top, right, bottom))
}

fn validate_stroke_width(width: u32) -> Result<()> {
    if width == 0 {
        return Err(VshotError::InvalidGeometry(
            "stroke width must be greater than zero".into(),
        ));
    }
    Ok(())
}

fn clip_range(start: i64, end: i64, limit: u32) -> Option<(i64, i64)> {
    let start = start.max(0);
    let end = end.min(i64::from(limit));
    (start < end).then_some((start, end))
}

fn point_from_f64(x: f64, y: f64) -> Point {
    fn coordinate(value: f64) -> i32 {
        let value = if value.is_nan() { 0.0 } else { value.round() };
        if value <= f64::from(i32::MIN) {
            i32::MIN
        } else if value >= f64::from(i32::MAX) {
            i32::MAX
        } else {
            value as i32
        }
    }

    Point::new(coordinate(x), coordinate(y))
}

/// Rasterizes one stroke, compositing a translucent colour exactly once per
/// covered pixel.
///
/// A stroke whose coverage is fractional — an anti-aliased edge — has to reach
/// the frame that way, so this is where the two forms meet: an opaque stroke
/// keeps the original single-pass drawing, and everything else goes through a
/// coverage mask.
fn stroke_with_coverage(
    frame: &mut Frame,
    color: [u8; 4],
    bounds: (i64, i64, i64, i64),
    rasterize: impl FnOnce(&mut Ink<'_>),
) {
    if color[3] == 255 {
        let mut ink = Ink::Direct { frame, color };
        rasterize(&mut ink);
        return;
    }
    let mut mask = StrokeMask::new(bounds, frame.size);
    if mask.is_empty() {
        return;
    }
    rasterize(&mut Ink::Mask(&mut mask));
    mask.composite(frame, color);
}

/// How many samples per axis a rasterizer takes inside one pixel.
///
/// The pixel tests below are exact — a pixel is in the shape or out of it — so
/// on their own every edge of a diagonal or a curve is a staircase.  Qt's own
/// preview draws the same mark with `QPainter::Antialiasing`, and the committed
/// image is the one the user compares against that preview, so the rasterizers
/// that go by a pixel's centre instead take [`SUPERSAMPLE`]² samples spread over
/// the pixel and hand [`Ink`] the fraction that landed inside.  Four is the
/// usual trade: sixteen samples per pixel, and an edge that reads as smooth
/// rather than as the two-step stair a single sample leaves.
const SUPERSAMPLE: u32 = 4;

/// The offset of one sub-sample from its pixel's own sample point, in whole
/// pixels.  Sample `i` of `SUPERSAMPLE` sits at `(2i + 1) / (2 * SUPERSAMPLE)`
/// across the pixel, so the run is centred on the point the rasterizer would
/// have tested on its own — a shape that covered that point still covers half
/// the pixel's samples, and nothing shifts by the half pixel a corner-anchored
/// run would have moved it.
#[inline]
fn subsample_offset(index: u32) -> f64 {
    (f64::from(2 * index + 1)) / (2.0 * f64::from(SUPERSAMPLE)) - 0.5
}

/// Where a stroke rasterizer sends the pixels it covers.
///
/// A rasterizer that is exact — a rectangle's own rows and columns — plots
/// straight onto the frame or into a mask.  One that goes by a pixel's centre
/// sends a coverage fraction instead, so the pixel can take the colour only in
/// the proportion that landed inside the mark.
enum Ink<'a> {
    /// Blend `color` onto the frame, source-over, once per plot.
    Direct {
        frame: &'a mut Frame,
        color: [u8; 4],
    },
    /// Mark coverage; `StrokeMask::composite` paints it afterwards.
    Mask(&'a mut StrokeMask),
}

impl Ink<'_> {
    #[inline]
    fn plot(&mut self, x: i64, y: i64) {
        match self {
            Ink::Direct { frame, color } => frame.blend_pixel_at(x, y, *color),
            Ink::Mask(mask) => mask.plot(x, y),
        }
    }

    /// Adds `coverage` (0..=1) to the pixel at `(x, y)`.
    ///
    /// A fully covered pixel plots outright, so a shape's interior costs
    /// nothing beyond what it always did; only the edge pixels carry a
    /// fraction, and only those end up blended with an alpha below the
    /// stroke's own.
    #[inline]
    fn plot_coverage(&mut self, x: i64, y: i64, coverage: f64) {
        if coverage <= 0.0 {
            return;
        }
        if coverage >= 1.0 {
            self.plot(x, y);
            return;
        }
        match self {
            Ink::Direct { frame, color } => {
                let mut edge = *color;
                edge[3] = ((f64::from(color[3]) * coverage).round() as i64).clamp(0, 255) as u8;
                frame.blend_pixel_at(x, y, edge);
            }
            Ink::Mask(mask) => mask.plot_coverage(x, y, coverage),
        }
    }
}

/// A bounding-box-limited coverage mask for a single stroke.
///
/// A rasterizer that is exact marks every pixel the path covers, and a shared
/// pixel is marked once — the union of the path's pixels. That is why an opaque
/// stroke composites to the same bytes it would have got by blending each
/// segment separately: at alpha 255 a source-over blend is a plain copy, so
/// "keep the maximum at overlaps" and "blend every segment" land the same
/// result.  A rasterizer that goes by a pixel's centre instead adds a fraction
/// per sample; the mask keeps the largest, which is the same union read as
/// coverage rather than as a flag.
struct StrokeMask {
    x0: i64,
    y0: i64,
    width: usize,
    height: usize,
    /// Coverage per pixel, 0..=255.
    covered: Vec<u8>,
}

impl StrokeMask {
    /// Allocates a mask over `bounds` (exclusive right/bottom), clipped to the
    /// frame so it never exceeds the frame's own pixel count.
    fn new(bounds: (i64, i64, i64, i64), frame: Size) -> Self {
        let (bounds_left, bounds_top, bounds_right, bounds_bottom) = bounds;
        let x0 = bounds_left.max(0);
        let y0 = bounds_top.max(0);
        let x1 = bounds_right.min(i64::from(frame.width)).max(x0);
        let y1 = bounds_bottom.min(i64::from(frame.height)).max(y0);
        let width = (x1 - x0) as usize;
        let height = (y1 - y0) as usize;
        Self {
            x0,
            y0,
            width,
            height,
            covered: vec![0u8; width * height],
        }
    }

    fn is_empty(&self) -> bool {
        self.width == 0 || self.height == 0
    }

    /// The mask's index for a pixel, or `None` when it lies outside.
    #[inline]
    fn index_of(&self, x: i64, y: i64) -> Option<usize> {
        let local_x = x - self.x0;
        let local_y = y - self.y0;
        if local_x < 0 || local_y < 0 {
            return None;
        }
        let (local_x, local_y) = (local_x as usize, local_y as usize);
        if local_x >= self.width || local_y >= self.height {
            return None;
        }
        Some(local_y * self.width + local_x)
    }

    #[inline]
    fn plot(&mut self, x: i64, y: i64) {
        if let Some(index) = self.index_of(x, y) {
            self.covered[index] = 255;
        }
    }

    /// Raises the pixel's coverage to `coverage` (0..=1) if that is more than
    /// it already holds.
    #[inline]
    fn plot_coverage(&mut self, x: i64, y: i64, coverage: f64) {
        let Some(index) = self.index_of(x, y) else {
            return;
        };
        let value = (coverage * 255.0).round().clamp(0.0, 255.0) as u8;
        if value > self.covered[index] {
            self.covered[index] = value;
        }
    }

    fn composite(&self, frame: &mut Frame, color: [u8; 4]) {
        for row in 0..self.height {
            for column in 0..self.width {
                let coverage = self.covered[row * self.width + column];
                if coverage == 0 {
                    continue;
                }
                let mut edge = color;
                // At full coverage the colour is used as it stands, so an
                // opaque stroke stays byte-for-byte what it always was.
                if coverage < 255 {
                    edge[3] = ((u32::from(color[3]) * u32::from(coverage) + 127) / 255) as u8;
                }
                frame.blend_pixel_at(self.x0 + column as i64, self.y0 + row as i64, edge);
            }
        }
    }
}

/// The exclusive bounding box of `points`, grown by `pad` on every side so it
/// holds the whole stroke the polyline rasterizers will draw. Shared by the
/// freehand, dashed and arrow paths, and by the wave/bezier tools that sample
/// down to a polyline and reuse this entry point.
fn polyline_bounds(points: &[Point], pad: i64) -> (i64, i64, i64, i64) {
    let Some(first) = points.first() else {
        return (0, 0, 0, 0);
    };
    let mut min_x = i64::from(first.x);
    let mut min_y = i64::from(first.y);
    let mut max_x = min_x;
    let mut max_y = min_y;
    for point in &points[1..] {
        min_x = min_x.min(i64::from(point.x));
        min_y = min_y.min(i64::from(point.y));
        max_x = max_x.max(i64::from(point.x));
        max_y = max_y.max(i64::from(point.y));
    }
    (min_x - pad, min_y - pad, max_x + pad + 1, max_y + pad + 1)
}

/// Samples the sine wave along the device-space segment `start..end` as a
/// polyline, one sample per device pixel of arc length.
///
/// `amplitude` is the peak deviation from the centre line and `wavelength` one
/// full period along the line, both in device pixels. Going by arc length means
/// the sample count `n = max(2, ceil(length / 1) + 1)` — at least the two
/// endpoints, and roughly one sample per pixel in between, so a dense wave is
/// still smooth. The phase starts at the origin: sample `i` sits at distance
/// `u = (i / (n - 1)) * length` along the line and `amplitude * sin(2π u /
/// wavelength)` off it, so `i = 0` lands exactly on the centre line, and
/// `i = n - 1` does too only when the length is a whole number of wavelengths.
///
/// A zero-length segment returns the single `start` point; the caller turns
/// that into a dot.
fn wave_polyline(start: Point, end: Point, amplitude: u32, wavelength: u32) -> Vec<Point> {
    let dx = f64::from(end.x) - f64::from(start.x);
    let dy = f64::from(end.y) - f64::from(start.y);
    let length = dx.hypot(dy);
    if length == 0.0 {
        return vec![start];
    }
    // One sample per pixel, plus both endpoints. The cap only ever bites on a
    // segment far longer than any frame, where the loss of fidelity is moot.
    const STEP: f64 = 1.0;
    const MAX_SAMPLES: usize = 1 << 20;
    let samples = ((length / STEP).ceil() as u64).saturating_add(1);
    let n = usize::try_from(samples)
        .unwrap_or(MAX_SAMPLES)
        .clamp(2, MAX_SAMPLES);
    let dir_x = dx / length;
    let dir_y = dy / length;
    // The 90-degree rotation of `dir`: the direction the wave deviates in.
    let normal_x = -dir_y;
    let normal_y = dir_x;
    // Whole cycles only, so the wave finishes on `end` rather than wherever
    // the requested wavelength happened to leave its phase.  Both ends then
    // sit on the line the user dragged, which is what makes it read as a wave
    // drawn from A to B instead of one smeared off to one side.
    let cycles = (length / f64::from(wavelength.max(1))).round().max(1.0);
    let radians_per_pixel = std::f64::consts::TAU / (length / cycles);
    let mut points = Vec::with_capacity(n);
    let last = (n - 1) as f64;
    for i in 0..n {
        let u = (i as f64 / last) * length;
        let offset = f64::from(amplitude) * (radians_per_pixel * u).sin();
        points.push(point_from_f64(
            f64::from(start.x) + dir_x * u + normal_x * offset,
            f64::from(start.y) + dir_y * u + normal_y * offset,
        ));
    }
    points
}

/// A point in the Bezier subdivision's own floating-point space.
///
/// The control polygon is carried as `f64` through the recursion and rounded to
/// whole device pixels only when a chord is emitted, so repeated midpoint
/// subdivision does not accumulate rounding.
#[derive(Clone, Copy)]
struct FPoint {
    x: f64,
    y: f64,
}

fn to_f(point: Point) -> FPoint {
    FPoint {
        x: f64::from(point.x),
        y: f64::from(point.y),
    }
}

/// The exact midpoint of two control points (de Casteljau's first stage).
fn midpoint(a: FPoint, b: FPoint) -> FPoint {
    FPoint {
        x: (a.x + b.x) * 0.5,
        y: (a.y + b.y) * 0.5,
    }
}

/// Recursion cap for the adaptive subdivision: each level halves the curve, so
/// 16 levels bound the work for even the most contorted control points without
/// ever risking a deep call stack.
const BEZIER_MAX_DEPTH: u32 = 16;

/// Flatness tolerance in device pixels: a cubic is subdivided until both its
/// control points sit within this distance of the chord.
const BEZIER_FLATNESS: f64 = 0.25;

/// Whether the cubic `p0..p3` is flat enough to be one chord.
///
/// Each control point's perpendicular distance from the chord is the cross
/// product `(handle - p0) × (p3 - p0)` divided by the chord length; comparing
/// the cross product against `tolerance * chord` avoids the division. A
/// degenerate chord (the endpoints coincide) has no direction, so the test
/// falls back to how far the handles stray from `p0` — a loop then still gets
/// subdivided instead of collapsing to a dot.
fn cubic_is_flat(p0: FPoint, p1: FPoint, p2: FPoint, p3: FPoint) -> bool {
    let dx = p3.x - p0.x;
    let dy = p3.y - p0.y;
    let chord = dx.hypot(dy);
    if chord == 0.0 {
        let first = (p1.x - p0.x).hypot(p1.y - p0.y);
        let second = (p2.x - p0.x).hypot(p2.y - p0.y);
        return first <= BEZIER_FLATNESS && second <= BEZIER_FLATNESS;
    }
    let first = ((p1.x - p0.x) * dy - (p1.y - p0.y) * dx).abs();
    let second = ((p2.x - p0.x) * dy - (p2.y - p0.y) * dx).abs();
    let limit = BEZIER_FLATNESS * chord;
    first <= limit && second <= limit
}

/// Appends the flattened chords of the cubic `p0..p3`, excluding `p0` (the
/// caller already emitted it) and including `p3`.
///
/// Subdivision is de Casteljau at `t = 0.5`, repeated while the curve is not
/// flat enough and the depth cap has not been reached; the cap turns a
/// pathological segment into a bounded number of chords rather than an
/// unbounded recursion.
fn flatten_cubic(out: &mut Vec<Point>, p0: FPoint, p1: FPoint, p2: FPoint, p3: FPoint, depth: u32) {
    if depth >= BEZIER_MAX_DEPTH || cubic_is_flat(p0, p1, p2, p3) {
        out.push(point_from_f64(p3.x, p3.y));
        return;
    }
    let p01 = midpoint(p0, p1);
    let p12 = midpoint(p1, p2);
    let p23 = midpoint(p2, p3);
    let p012 = midpoint(p01, p12);
    let p123 = midpoint(p12, p23);
    let p0123 = midpoint(p012, p123);
    flatten_cubic(out, p0, p01, p012, p0123, depth + 1);
    flatten_cubic(out, p0123, p123, p23, p3, depth + 1);
}

/// Flattens the interleaved anchor/out-handle list into a polyline.
///
/// Every consecutive pair of anchors is one cubic segment: the first anchor,
/// its out-handle, the next anchor's in-handle (`2 * anchor - out_handle`) and
/// the next anchor. A list with fewer than two anchors yields just the anchor
/// it has, which the caller turns into a dot.
fn bezier_polyline(points: &[Point]) -> Vec<Point> {
    let Some(&start) = points.first() else {
        return Vec::new();
    };
    let mut polyline = vec![start];
    let anchors = points.len() / 2;
    for index in 0..anchors.saturating_sub(1) {
        let p0 = points[2 * index];
        let out = points[2 * index + 1];
        let next = points[2 * index + 2];
        let next_out = points[2 * index + 3];
        // The in-handle mirrors the out-handle through its anchor. Both are
        // integers, so the mirror is exact; saturating keeps a hand-crafted
        // extreme handle from overflowing.
        let in_handle = Point::new(
            next.x.saturating_mul(2).saturating_sub(next_out.x),
            next.y.saturating_mul(2).saturating_sub(next_out.y),
        );
        flatten_cubic(
            &mut polyline,
            to_f(p0),
            to_f(out),
            to_f(in_handle),
            to_f(next),
            0,
        );
    }
    polyline
}

/// Narrows a computed coordinate to `i64`, saturating instead of wrapping so a
/// far-off-canvas shape only ever yields an empty bounding box.
fn clamp_i64(value: i128) -> i64 {
    value.clamp(i128::from(i64::MIN), i128::from(i64::MAX)) as i64
}

/// Fills the `width`-thick border of the rectangle `(left, top, right, bottom)`
/// — the solid rectangle stroke, as a rasterizer.
fn rasterize_rect_border(
    ink: &mut Ink<'_>,
    frame: Size,
    left: i64,
    top: i64,
    right: i64,
    bottom: i64,
    width: u32,
) {
    let stroke_width = i64::from(width);
    let Some((x_start, x_end)) = clip_range(left, right, frame.width) else {
        return;
    };
    let Some((y_start, y_end)) = clip_range(top, bottom, frame.height) else {
        return;
    };
    for y in y_start..y_end {
        for x in x_start..x_end {
            if x - left < stroke_width
                || right - x <= stroke_width
                || y - top < stroke_width
                || bottom - y <= stroke_width
            {
                ink.plot(x, y);
            }
        }
    }
}

/// Draws the ring of a circle as a rasterizer.
///
/// The band is where the pixel's centre falls, so the edge is a staircase; the
/// samples spread over the pixel turn it into a smooth one.
fn rasterize_circle(ink: &mut Ink<'_>, frame: Size, center: Point, radius: u32, width: u32) {
    let radius = i128::from(radius);
    let stroke_width = i128::from(width);
    let outer_radius = radius + (stroke_width + 1) / 2;
    let center_x = i128::from(center.x);
    let center_y = i128::from(center.y);
    let x_start = (center_x - outer_radius).max(0);
    let x_end = (center_x + outer_radius + 1).min(i128::from(frame.width));
    let y_start = (center_y - outer_radius).max(0);
    let y_end = (center_y + outer_radius + 1).min(i128::from(frame.height));
    if x_start >= x_end || y_start >= y_end {
        return;
    }

    // Twice the squared radii, so a sample at distance `d` is inside when
    // `4d²` lies between them.  The samples are on a finer grid than the
    // pixels, so these are kept in floating point rather than scaled to whole
    // numbers as the centre test was.
    let inner_radius_twice = (radius * 2 - stroke_width).max(0) as f64;
    let outer_radius_twice = (radius * 2 + stroke_width) as f64;
    let inner_distance = inner_radius_twice * inner_radius_twice;
    let outer_distance = outer_radius_twice * outer_radius_twice;
    let center_x = center_x as f64;
    let center_y = center_y as f64;
    let samples = f64::from(SUPERSAMPLE * SUPERSAMPLE);
    for y in y_start..y_end {
        for x in x_start..x_end {
            let mut inside = 0u32;
            for sy in 0..SUPERSAMPLE {
                let py = y as f64 + subsample_offset(sy);
                for sx in 0..SUPERSAMPLE {
                    let px = x as f64 + subsample_offset(sx);
                    let dx = px - center_x;
                    let dy = py - center_y;
                    let distance = 4.0 * (dx * dx + dy * dy);
                    if distance >= inner_distance && distance <= outer_distance {
                        inside += 1;
                    }
                }
            }
            if inside > 0 {
                ink.plot_coverage(x as i64, y as i64, f64::from(inside) / samples);
            }
        }
    }
}

/// Draws the ring of the ellipse inscribed in `(left, top, right, bottom)`.
///
/// The band is where the pixel's centre falls, so the edge is a staircase; the
/// samples spread over the pixel turn it into a smooth one.
fn rasterize_ellipse(
    ink: &mut Ink<'_>,
    frame: Size,
    left: i64,
    top: i64,
    right: i64,
    bottom: i64,
    width: u32,
) {
    let rect_width = i128::from(right - left);
    let rect_height = i128::from(bottom - top);
    let stroke_width = i128::from(width);
    let center_x = i128::from(left) + rect_width / 2;
    let center_y = i128::from(top) + rect_height / 2;
    let outer_a = (rect_width + stroke_width) / 2;
    let outer_b = (rect_height + stroke_width) / 2;
    let inner_a = rect_width.saturating_sub(stroke_width) / 2;
    let inner_b = rect_height.saturating_sub(stroke_width) / 2;
    let outer_a_squared = (outer_a * outer_a) as f64;
    let outer_b_squared = (outer_b * outer_b) as f64;
    let inner_a_squared = (inner_a * inner_a) as f64;
    let inner_b_squared = (inner_b * inner_b) as f64;
    let outer_threshold = outer_a_squared * outer_b_squared;
    let inner_threshold = inner_a_squared * inner_b_squared;
    let has_inner = inner_a > 0 && inner_b > 0;
    let x_start = (center_x - outer_a).max(0);
    let x_end = (center_x + outer_a + 1).min(i128::from(frame.width));
    let y_start = (center_y - outer_b).max(0);
    let y_end = (center_y + outer_b + 1).min(i128::from(frame.height));
    if x_start >= x_end || y_start >= y_end {
        return;
    }
    let center_x = center_x as f64;
    let center_y = center_y as f64;
    let samples = f64::from(SUPERSAMPLE * SUPERSAMPLE);
    for y in y_start..y_end {
        for x in x_start..x_end {
            let mut inside = 0u32;
            for sy in 0..SUPERSAMPLE {
                let py = y as f64 + subsample_offset(sy);
                for sx in 0..SUPERSAMPLE {
                    let px = x as f64 + subsample_offset(sx);
                    let dx = px - center_x;
                    let dy = py - center_y;
                    // Inside the outer ellipse and outside the inner one:
                    // dx²/a² + dy²/b² <= 1 is equivalent to
                    // dx²·b² + dy²·a² <= a²·b².
                    let outside_outer =
                        dx * dx * outer_b_squared + dy * dy * outer_a_squared > outer_threshold;
                    if outside_outer {
                        continue;
                    }
                    if has_inner
                        && dx * dx * inner_b_squared + dy * dy * inner_a_squared < inner_threshold
                    {
                        continue;
                    }
                    inside += 1;
                }
            }
            if inside > 0 {
                ink.plot_coverage(x as i64, y as i64, f64::from(inside) / samples);
            }
        }
    }
}

/// Stamps the `size`-wide square centred on `(center_x, center_y)`.
fn rasterize_square(ink: &mut Ink<'_>, frame: Size, center_x: i64, center_y: i64, size: u32) {
    let size = i64::from(size.max(1));
    let half = (size - 1) / 2;
    let x_start = (center_x - half).max(0);
    let y_start = (center_y - half).max(0);
    let x_end = (center_x - half + size).min(i64::from(frame.width));
    let y_end = (center_y - half + size).min(i64::from(frame.height));
    for y in y_start..y_end {
        for x in x_start..x_end {
            ink.plot(x, y);
        }
    }
}

/// Walks the polyline at 1px steps, stamping `size`-wide squares wherever the
/// cumulative distance falls inside the dash pattern's on phase.
fn rasterize_dashed_polyline(
    ink: &mut Ink<'_>,
    frame: Size,
    points: &[Point],
    size: u32,
    dash: LineDash,
) {
    let (on, off) = dash.pattern(size);
    let on_draw = i64::from(dash.drawn_on(size)).max(1);
    let period = i64::from(on.max(1) + off).max(1);
    let mut base = 0.0_f64;
    for segment in points.windows(2) {
        let ax = f64::from(segment[0].x);
        let ay = f64::from(segment[0].y);
        let bx = f64::from(segment[1].x);
        let by = f64::from(segment[1].y);
        let length = (bx - ax).hypot(by - ay);
        let steps = (length.ceil() as i64).max(1);
        for k in 0..=steps {
            let t = k as f64 / steps as f64;
            let x = (ax + (bx - ax) * t).round() as i64;
            let y = (ay + (by - ay) * t).round() as i64;
            let distance = base + length * (k as f64) / steps as f64;
            if (distance.floor() as i64).rem_euclid(period) < on_draw {
                rasterize_square(ink, frame, x, y, size);
            }
        }
        base += length;
    }
}

/// Stamps dashes along the ellipse inscribed in `bounds`, mirroring the solid
/// ring's midline so dashed and solid ellipses overlap exactly. `bounds` holds
/// the exclusive `(left, top, right, bottom)` device edges.
fn rasterize_dashed_ellipse(
    ink: &mut Ink<'_>,
    frame: Size,
    bounds: (i64, i64, i64, i64),
    width: u32,
    dash: LineDash,
) {
    let (left, top, right, bottom) = bounds;
    let center_x = left as f64 + (right - left) as f64 / 2.0;
    let center_y = top as f64 + (bottom - top) as f64 / 2.0;
    let a = (right - left) as f64 / 2.0;
    let b = (bottom - top) as f64 / 2.0;
    let perimeter = std::f64::consts::PI * (3.0 * (a + b) - ((3.0 * a + b) * (a + 3.0 * b)).sqrt());
    let steps = (perimeter.ceil() as i64).clamp(32, 1 << 20).max(1);
    let (on, off) = dash.pattern(width);
    let on_draw = i64::from(dash.drawn_on(width)).max(1);
    let period = i64::from(on.max(1) + off).max(1);
    for k in 0..steps {
        let angle = 2.0 * std::f64::consts::PI * (k as f64) / (steps as f64);
        let x = (center_x + a * angle.cos()).round() as i64;
        let y = (center_y + b * angle.sin()).round() as i64;
        let distance = perimeter * (k as f64) / (steps as f64);
        if (distance.floor() as i64).rem_euclid(period) < on_draw {
            rasterize_square(ink, frame, x, y, width);
        }
    }
}

/// Fills the triangle `first, second, third`.
///
/// The edge is where a sample falls on one side of each side's line, which
/// leaves the head's two slanted sides a staircase; the samples spread over the
/// pixel turn it into a smooth one, as they do for the stem the head is joined
/// to.
fn rasterize_triangle(ink: &mut Ink<'_>, frame: Size, first: Point, second: Point, third: Point) {
    let min_x = i64::from(first.x)
        .min(i64::from(second.x))
        .min(i64::from(third.x))
        .max(0);
    let max_x = i64::from(first.x)
        .max(i64::from(second.x))
        .max(i64::from(third.x))
        .min(i64::from(frame.width).saturating_sub(1));
    let min_y = i64::from(first.y)
        .min(i64::from(second.y))
        .min(i64::from(third.y))
        .max(0);
    let max_y = i64::from(first.y)
        .max(i64::from(second.y))
        .max(i64::from(third.y))
        .min(i64::from(frame.height).saturating_sub(1));
    if min_x > max_x || min_y > max_y {
        return;
    }

    let edge = |a: Point, b: Point, x: f64, y: f64| {
        (f64::from(b.x) - f64::from(a.x)) * (y - f64::from(a.y))
            - (f64::from(b.y) - f64::from(a.y)) * (x - f64::from(a.x))
    };
    // The winding only picks which side of each edge is the inside, so it is
    // read once at the vertices rather than per sample.
    let area = edge(first, second, f64::from(third.x), f64::from(third.y));
    if area == 0.0 {
        return;
    }
    let samples = f64::from(SUPERSAMPLE * SUPERSAMPLE);
    for y in min_y..=max_y {
        for x in min_x..=max_x {
            let mut inside = 0u32;
            for sy in 0..SUPERSAMPLE {
                let py = y as f64 + subsample_offset(sy);
                for sx in 0..SUPERSAMPLE {
                    let px = x as f64 + subsample_offset(sx);
                    let first_edge = edge(first, second, px, py);
                    let second_edge = edge(second, third, px, py);
                    let third_edge = edge(third, first, px, py);
                    let hit = if area > 0.0 {
                        first_edge >= 0.0 && second_edge >= 0.0 && third_edge >= 0.0
                    } else {
                        first_edge <= 0.0 && second_edge <= 0.0 && third_edge <= 0.0
                    };
                    if hit {
                        inside += 1;
                    }
                }
            }
            ink.plot_coverage(x, y, f64::from(inside) / samples);
        }
    }
}

/// Draws the capsule — the `width`-thick stroke of the segment `start..end`.
///
/// The edge is where a pixel's centre falls within half the width of the line,
/// which leaves every diagonal and every round cap a staircase; the samples
/// spread over the pixel turn it into a smooth one.  This is the rasterizer
/// every polyline tool is built on — freehand, the arrow's stem, the wave, the
/// bezier — so it is where most of the jaggedness was.
fn rasterize_capsule(ink: &mut Ink<'_>, frame: Size, start: Point, end: Point, width: u32) {
    let padding = (i128::from(width) + 1) / 2;
    let start_x = i128::from(start.x);
    let start_y = i128::from(start.y);
    let end_x = i128::from(end.x);
    let end_y = i128::from(end.y);
    let x_start = (start_x.min(end_x) - padding).max(0);
    let x_end = (start_x.max(end_x) + padding + 1).min(i128::from(frame.width));
    let y_start = (start_y.min(end_y) - padding).max(0);
    let y_end = (start_y.max(end_y) + padding + 1).min(i128::from(frame.height));
    if x_start >= x_end || y_start >= y_end {
        return;
    }

    let x1 = f64::from(start.x);
    let y1 = f64::from(start.y);
    let x2 = f64::from(end.x);
    let y2 = f64::from(end.y);
    let dx = x2 - x1;
    let dy = y2 - y1;
    let length_squared = dx.mul_add(dx, dy * dy);
    let threshold = f64::from(width) / 2.0;
    let threshold_squared = threshold * threshold;
    let samples = f64::from(SUPERSAMPLE * SUPERSAMPLE);
    for y in y_start..y_end {
        for x in x_start..x_end {
            let mut inside = 0u32;
            for sy in 0..SUPERSAMPLE {
                let py = y as f64 + subsample_offset(sy);
                for sx in 0..SUPERSAMPLE {
                    let px = x as f64 + subsample_offset(sx);
                    let distance_squared = if length_squared == 0.0 {
                        let delta_x = px - x1;
                        let delta_y = py - y1;
                        delta_x.mul_add(delta_x, delta_y * delta_y)
                    } else {
                        let projection = ((px - x1) * dx + (py - y1) * dy) / length_squared;
                        let projection = projection.clamp(0.0, 1.0);
                        let nearest_x = x1 + projection * dx;
                        let nearest_y = y1 + projection * dy;
                        let delta_x = px - nearest_x;
                        let delta_y = py - nearest_y;
                        delta_x.mul_add(delta_x, delta_y * delta_y)
                    };
                    if distance_squared <= threshold_squared {
                        inside += 1;
                    }
                }
            }
            if inside > 0 {
                ink.plot_coverage(x as i64, y as i64, f64::from(inside) / samples);
            }
        }
    }
}

fn blend_source_over(destination: &mut [u8], source: [u8; 4]) {
    let source_alpha = u64::from(source[3]);
    if source_alpha == 0 {
        return;
    }
    if source_alpha == 255 {
        destination[..4].copy_from_slice(&source);
        return;
    }

    let destination_alpha = u64::from(destination[3]);
    let inverse_source_alpha = 255 - source_alpha;
    let alpha_numerator = source_alpha * 255 + destination_alpha * inverse_source_alpha;
    let output_alpha = ((alpha_numerator + 127) / 255).min(255) as u8;
    for channel in 0..3 {
        let numerator = u64::from(source[channel]) * source_alpha * 255
            + u64::from(destination[channel]) * destination_alpha * inverse_source_alpha;
        let value = ((numerator + alpha_numerator / 2) / alpha_numerator).min(255);
        destination[channel] = value as u8;
    }
    destination[3] = output_alpha;
}

fn ascii_glyph(byte: u8) -> Option<[u8; 7]> {
    Some(match byte {
        0x20 => [0, 0, 0, 0, 0, 0, 0],
        0x21 => [4, 4, 4, 4, 4, 0, 4],
        0x22 => [10, 10, 10, 0, 0, 0, 0],
        0x23 => [10, 31, 10, 31, 10, 0, 0],
        0x24 => [4, 15, 20, 14, 5, 30, 4],
        0x25 => [17, 2, 4, 8, 17, 0, 0],
        0x26 => [12, 18, 20, 8, 21, 18, 13],
        0x27 => [4, 4, 8, 0, 0, 0, 0],
        0x28 => [2, 4, 8, 8, 8, 4, 2],
        0x29 => [8, 4, 2, 2, 2, 4, 8],
        0x2a => [0, 4, 21, 14, 21, 4, 0],
        0x2b => [0, 4, 4, 31, 4, 4, 0],
        0x2c => [0, 0, 0, 0, 4, 4, 8],
        0x2d => [0, 0, 0, 31, 0, 0, 0],
        0x2e => [0, 0, 0, 0, 0, 12, 12],
        0x2f => [1, 2, 4, 8, 16, 0, 0],
        b'0' => [14, 17, 19, 21, 25, 17, 14],
        b'1' => [4, 12, 4, 4, 4, 4, 14],
        b'2' => [14, 17, 1, 2, 4, 8, 31],
        b'3' => [31, 2, 4, 2, 1, 17, 14],
        b'4' => [2, 6, 10, 18, 31, 2, 2],
        b'5' => [31, 16, 30, 1, 1, 17, 14],
        b'6' => [6, 8, 16, 30, 17, 17, 14],
        b'7' => [31, 1, 2, 4, 8, 8, 8],
        b'8' => [14, 17, 17, 14, 17, 17, 14],
        b'9' => [14, 17, 17, 15, 1, 2, 12],
        0x3a => [0, 12, 12, 0, 12, 12, 0],
        0x3b => [0, 12, 12, 0, 12, 12, 24],
        0x3c => [2, 4, 8, 16, 8, 4, 2],
        0x3d => [0, 0, 31, 0, 31, 0, 0],
        0x3e => [8, 4, 2, 1, 2, 4, 8],
        0x3f => [14, 17, 1, 2, 4, 0, 4],
        0x40 => [14, 17, 1, 13, 21, 21, 14],
        b'A' => [14, 17, 17, 31, 17, 17, 17],
        b'B' => [30, 17, 17, 30, 17, 17, 30],
        b'C' => [14, 17, 16, 16, 16, 17, 14],
        b'D' => [30, 17, 17, 17, 17, 17, 30],
        b'E' => [31, 16, 16, 30, 16, 16, 31],
        b'F' => [31, 16, 16, 30, 16, 16, 16],
        b'G' => [14, 17, 16, 23, 17, 17, 15],
        b'H' => [17, 17, 17, 31, 17, 17, 17],
        b'I' => [14, 4, 4, 4, 4, 4, 14],
        b'J' => [7, 2, 2, 2, 2, 18, 12],
        b'K' => [17, 18, 20, 24, 20, 18, 17],
        b'L' => [16, 16, 16, 16, 16, 16, 31],
        b'M' => [17, 27, 21, 21, 17, 17, 17],
        b'N' => [17, 25, 21, 19, 17, 17, 17],
        b'O' => [14, 17, 17, 17, 17, 17, 14],
        b'P' => [30, 17, 17, 30, 16, 16, 16],
        b'Q' => [14, 17, 17, 17, 21, 18, 13],
        b'R' => [30, 17, 17, 30, 20, 18, 17],
        b'S' => [15, 16, 16, 14, 1, 1, 30],
        b'T' => [31, 4, 4, 4, 4, 4, 4],
        b'U' => [17, 17, 17, 17, 17, 17, 14],
        b'V' => [17, 17, 17, 17, 17, 10, 4],
        b'W' => [17, 17, 17, 21, 21, 21, 10],
        b'X' => [17, 17, 10, 4, 10, 17, 17],
        b'Y' => [17, 17, 10, 4, 4, 4, 4],
        b'Z' => [31, 1, 2, 4, 8, 16, 31],
        0x5b => [14, 8, 8, 8, 8, 8, 14],
        0x5c => [16, 8, 4, 2, 1, 0, 0],
        0x5d => [14, 2, 2, 2, 2, 2, 14],
        0x5e => [4, 10, 17, 0, 0, 0, 0],
        0x5f => [0, 0, 0, 0, 0, 0, 31],
        0x60 => [8, 4, 2, 0, 0, 0, 0],
        b'a' => [0, 0, 14, 1, 15, 17, 15],
        b'b' => [16, 16, 22, 25, 17, 17, 30],
        b'c' => [0, 0, 14, 16, 16, 17, 14],
        b'd' => [1, 1, 13, 19, 17, 17, 15],
        b'e' => [0, 0, 14, 17, 31, 16, 14],
        b'f' => [6, 9, 8, 28, 8, 8, 8],
        b'g' => [0, 0, 15, 17, 15, 1, 14],
        b'h' => [16, 16, 22, 25, 17, 17, 17],
        b'i' => [4, 0, 12, 4, 4, 4, 14],
        b'j' => [2, 0, 6, 2, 2, 18, 12],
        b'k' => [16, 16, 18, 20, 24, 20, 18],
        b'l' => [12, 4, 4, 4, 4, 4, 14],
        b'm' => [0, 0, 26, 21, 21, 17, 17],
        b'n' => [0, 0, 30, 17, 17, 17, 17],
        b'o' => [0, 0, 14, 17, 17, 17, 14],
        b'p' => [0, 0, 30, 17, 30, 16, 16],
        b'q' => [0, 0, 15, 17, 15, 1, 1],
        b'r' => [0, 0, 22, 25, 16, 16, 16],
        b's' => [0, 0, 15, 16, 14, 1, 30],
        b't' => [8, 8, 28, 8, 8, 9, 6],
        b'u' => [0, 0, 17, 17, 17, 19, 13],
        b'v' => [0, 0, 17, 17, 17, 10, 4],
        b'w' => [0, 0, 17, 17, 21, 21, 10],
        b'x' => [0, 0, 17, 10, 4, 10, 17],
        b'y' => [0, 0, 17, 17, 15, 1, 14],
        b'z' => [0, 0, 31, 2, 4, 8, 31],
        0x7b => [2, 4, 4, 8, 4, 4, 2],
        0x7c => [4, 4, 4, 4, 4, 4, 4],
        0x7d => [8, 4, 4, 2, 4, 4, 8],
        0x7e => [0, 0, 9, 18, 0, 0, 0],
        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Whether a pixel carries the colour at all, whatever the alpha.
    ///
    /// A stroke's edge is anti-aliased now — a pixel the path only partly
    /// covers takes the colour in that proportion — so "is this pixel painted"
    /// is a question about its alpha being above zero, not about it being 255.
    /// Tests that name a pixel on a mark's *interior* still compare the whole
    /// value; the ones that sample an edge read through this.
    fn painted(frame: &Frame, point: Point, rgb: [u8; 3]) -> bool {
        frame.pixel(point).is_some_and(|pixel| {
            pixel[3] > 0 && pixel[0] == rgb[0] && pixel[1] == rgb[1] && pixel[2] == rgb[2]
        })
    }

    #[test]
    fn crop_preserves_top_left_rgba_pixels() {
        let frame = Frame::new(
            Size::new(3, 2),
            vec![
                1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255, 13, 14, 15, 255, 16, 17,
                18, 255,
            ],
        )
        .unwrap();
        let cropped = frame.crop(Rect::new(1, 0, 2, 2)).unwrap();
        assert_eq!(cropped.size(), Size::new(2, 2));
        assert_eq!(cropped.pixel(Point::new(0, 0)), Some([4, 5, 6, 255]));
        assert_eq!(cropped.pixel(Point::new(1, 1)), Some([16, 17, 18, 255]));
    }

    #[test]
    fn nearest_resize_preserves_corners() {
        let frame = Frame::new(
            Size::new(2, 2),
            vec![
                255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255,
            ],
        )
        .unwrap();
        let resized = frame.resize_nearest(Size::new(4, 4)).unwrap();
        assert_eq!(resized.size(), Size::new(4, 4));
        assert_eq!(resized.pixel(Point::new(0, 0)), Some([255, 0, 0, 255]));
        assert_eq!(resized.pixel(Point::new(3, 0)), Some([0, 255, 0, 255]));
        assert_eq!(resized.pixel(Point::new(0, 3)), Some([0, 0, 255, 255]));
        assert_eq!(resized.pixel(Point::new(3, 3)), Some([255, 255, 0, 255]));
    }

    #[test]
    fn png_round_trip() {
        let frame = Frame::solid(Size::new(2, 1), [10, 20, 30, 255]).unwrap();
        let encoded = frame.to_png().unwrap();
        assert_eq!(Frame::from_png(&encoded).unwrap(), frame);
    }

    #[test]
    fn png_with_density_declares_its_physical_resolution() {
        let frame = Frame::solid(Size::new(2, 1), [10, 20, 30, 255]).unwrap();
        let encoded = frame
            .encode_png(Some(2), PngCompression::default())
            .unwrap();
        // A declared density doubles as the DPI the pin side reads back, and
        // the file still has to be an ordinary PNG.
        assert_eq!(Frame::from_png(&encoded).unwrap(), frame);
        let phys = encoded
            .windows(4)
            .position(|chunk| chunk == &b"pHYs"[..])
            .expect("a density-declaring PNG carries a pHYs chunk");
        assert_eq!(
            u32::from_be_bytes(encoded[phys + 4..phys + 8].try_into().unwrap()),
            7559 // 192 DPI in pixels per metre
        );
        assert_eq!(
            u32::from_be_bytes(encoded[phys + 8..phys + 12].try_into().unwrap()),
            7559
        );
        assert_eq!(encoded[phys + 12], 1); // metre, not the unspecified unit

        // 96 DPI (a density of 1) is deliberately not what an undeclared PNG
        // gets: Qt reports that default itself, and the pin side filters it out.
        let single = frame
            .encode_png(Some(1), PngCompression::default())
            .unwrap();
        let phys = single
            .windows(4)
            .position(|chunk| chunk == &b"pHYs"[..])
            .expect("a density of 1 is still a declaration");
        assert_eq!(
            u32::from_be_bytes(single[phys + 4..phys + 8].try_into().unwrap()),
            3780
        );
    }

    #[test]
    fn png_declares_its_colour_space() {
        // The pixels are sRGB, and without the chunk a viewer on a wide-gamut
        // display is free to read them as that display's own gamut — a P3 or
        // BT.2020 panel — so an untagged capture came out tinted.
        let frame = Frame::solid(Size::new(2, 1), [10, 20, 30, 255]).unwrap();
        let encoded = frame.to_png().unwrap();
        assert_eq!(Frame::from_png(&encoded).unwrap(), frame);
        let srgb = encoded
            .windows(4)
            .position(|chunk| chunk == &b"sRGB"[..])
            .expect("the PNG declares its colour space");
        assert_eq!(encoded[srgb + 4], 0); // the perceptual rendering intent
    }

    #[test]
    fn png_compression_levels_trade_size_for_time() {
        // A gradient with a little noise on top: compressible, but not so
        // regular that every level lands on the same size.
        let mut pixels = Vec::with_capacity(64 * 64 * 4);
        for y in 0..64u32 {
            for x in 0..64u32 {
                pixels.extend_from_slice(&[
                    (x * 4) as u8,
                    (y * 4) as u8,
                    (x * y % 256) as u8,
                    255u8.wrapping_sub(((x * 7 + y * 13) % 5) as u8),
                ]);
            }
        }
        let frame = Frame::new(Size::new(64, 64), pixels).unwrap();

        let sizes: Vec<usize> = [
            PngCompression::None,
            PngCompression::Fastest,
            PngCompression::Fast,
            PngCompression::Balanced,
        ]
        .into_iter()
        .map(|compression| {
            let encoded = frame.encode_png(None, compression).unwrap();
            // Every level still has to be an ordinary PNG of the same pixels.
            assert_eq!(Frame::from_png(&encoded).unwrap(), frame);
            encoded.len()
        })
        .collect();

        // Uncompressed is the biggest, DEFLATE level 6 the smallest.
        assert!(sizes[0] > sizes[1], "{sizes:?}");
        assert!(sizes[2] > sizes[3], "{sizes:?}");
    }

    #[test]
    fn png_compression_parses_its_levels() {
        assert_eq!(PngCompression::default(), PngCompression::Fast);
        assert_eq!(PngCompression::parse("fast").unwrap(), PngCompression::Fast);
        assert_eq!(PngCompression::parse("high").unwrap(), PngCompression::High);
        let error = PngCompression::parse("slowest").unwrap_err();
        assert!(
            error.to_string().contains("not a PNG compression level"),
            "{error}"
        );
    }

    #[test]
    fn rectangle_stroke_blends_source_over_and_leaves_interior() {
        let mut frame = Frame::solid(Size::new(5, 5), [0, 0, 0, 0]).unwrap();
        frame
            .stroke_rectangle(Rect::new(1, 1, 3, 3), [255, 0, 0, 255], 1, LineDash::Solid)
            .unwrap();
        assert_eq!(frame.pixel(Point::new(1, 1)), Some([255, 0, 0, 255]));
        assert_eq!(frame.pixel(Point::new(2, 1)), Some([255, 0, 0, 255]));
        assert_eq!(frame.pixel(Point::new(3, 3)), Some([255, 0, 0, 255]));
        assert_eq!(frame.pixel(Point::new(2, 2)), Some([0, 0, 0, 0]));

        let mut opaque = Frame::solid(Size::new(1, 1), [10, 20, 30, 255]).unwrap();
        opaque
            .stroke_rectangle(
                Rect::new(0, 0, 1, 1),
                [110, 120, 130, 128],
                1,
                LineDash::Solid,
            )
            .unwrap();
        assert_eq!(opaque.pixel(Point::new(0, 0)), Some([60, 70, 80, 255]));
        opaque
            .stroke_rectangle(Rect::new(0, 0, 1, 1), [255, 0, 0, 0], 1, LineDash::Solid)
            .unwrap();
        assert_eq!(opaque.pixel(Point::new(0, 0)), Some([60, 70, 80, 255]));
    }

    #[test]
    fn dashed_rectangle_leaves_gaps_along_the_perimeter() {
        let mut frame = Frame::solid(Size::new(40, 4), [0, 0, 0, 0]).unwrap();
        frame
            .stroke_rectangle(
                Rect::new(2, 1, 36, 2),
                [255, 0, 0, 255],
                1,
                LineDash::Dashed,
            )
            .unwrap();
        // A width-1 dashed pen runs 4 on / 3 off, so x = 2 (d = 0) is inked,
        // x = 5 (d = 3) still on, and x = 6 (d = 4) falls in the first gap.
        assert_eq!(frame.pixel(Point::new(2, 1)), Some([255, 0, 0, 255]));
        assert_eq!(frame.pixel(Point::new(5, 1)), Some([255, 0, 0, 255]));
        assert_eq!(frame.pixel(Point::new(6, 1)), Some([0, 0, 0, 0]));
        // Dotted: one 1px dot per 3px period.
        let mut frame = Frame::solid(Size::new(40, 4), [0, 0, 0, 0]).unwrap();
        frame
            .stroke_rectangle(
                Rect::new(2, 1, 36, 2),
                [255, 0, 0, 255],
                1,
                LineDash::Dotted,
            )
            .unwrap();
        assert_eq!(frame.pixel(Point::new(2, 1)), Some([255, 0, 0, 255]));
        assert_eq!(frame.pixel(Point::new(3, 1)), Some([0, 0, 0, 0]));
        assert_eq!(frame.pixel(Point::new(5, 1)), Some([255, 0, 0, 255]));
    }

    #[test]
    fn circle_stroke_draws_ring() {
        let mut frame = Frame::solid(Size::new(9, 9), [0, 0, 0, 0]).unwrap();
        frame
            .stroke_circle(Point::new(4, 4), 2, [0, 255, 0, 255], 1)
            .unwrap();
        for point in [
            Point::new(4, 2),
            Point::new(6, 4),
            Point::new(4, 6),
            Point::new(2, 4),
        ] {
            assert_eq!(frame.pixel(point), Some([0, 255, 0, 255]), "{point:?}");
        }
        assert_eq!(frame.pixel(Point::new(4, 4)), Some([0, 0, 0, 0]));
    }

    #[test]
    fn ellipse_stroke_draws_ring_inside_rect() {
        let mut frame = Frame::solid(Size::new(10, 8), [0, 0, 0, 0]).unwrap();
        frame
            .stroke_ellipse(Rect::new(0, 0, 10, 8), [0, 255, 0, 255], 2, LineDash::Solid)
            .unwrap();
        // The ring runs along the rect's own edge, so the pixel on it is only
        // partly inside the band: painted, at whatever fraction the samples
        // found.  What matters is that the ring reaches the midpoint at all --
        // the interior stays clear.
        for point in [
            Point::new(0, 4),
            Point::new(9, 4),
            Point::new(4, 0),
            Point::new(4, 7),
        ] {
            assert!(painted(&frame, point, [0, 255, 0]), "{point:?}");
        }
        assert_eq!(frame.pixel(Point::new(4, 4)), Some([0, 0, 0, 0]));
        // A wide rect must produce an ellipse, not the circle the old fallback drew:
        // the circle radius (min/2 = 4) would miss (0, 4).
    }

    #[test]
    fn ellipse_stroke_fills_when_stroke_swallows_the_ring() {
        let mut frame = Frame::solid(Size::new(6, 6), [0, 0, 0, 0]).unwrap();
        frame
            .stroke_ellipse(Rect::new(1, 1, 4, 4), [255, 0, 0, 255], 3, LineDash::Solid)
            .unwrap();
        // inner axes collapse to zero: the ellipse becomes a filled disk
        assert_eq!(frame.pixel(Point::new(3, 3)), Some([255, 0, 0, 255]));
        assert_eq!(frame.pixel(Point::new(3, 1)), Some([255, 0, 0, 255]));
        assert_eq!(frame.pixel(Point::new(0, 0)), Some([0, 0, 0, 0]));
    }

    #[test]
    fn filled_arrow_head_fills_the_triangle_while_open_head_does_not() {
        let mut open = Frame::solid(Size::new(30, 30), [0, 0, 0, 0]).unwrap();
        open.draw_arrow_with_style(
            Point::new(2, 15),
            Point::new(25, 15),
            [0, 0, 255, 255],
            1,
            LineDash::Solid,
            2,
            ArrowStyle::Open,
        )
        .unwrap();
        let mut filled = Frame::solid(Size::new(30, 30), [0, 0, 0, 0]).unwrap();
        filled
            .draw_arrow_with_style(
                Point::new(2, 15),
                Point::new(25, 15),
                [0, 0, 255, 255],
                1,
                LineDash::Solid,
                2,
                ArrowStyle::Filled,
            )
            .unwrap();
        assert_eq!(open.pixel(Point::new(21, 15)), Some([0, 0, 255, 255]));
        assert_eq!(filled.pixel(Point::new(21, 15)), Some([0, 0, 255, 255]));
        assert_eq!(open.pixel(Point::new(20, 14)), Some([0, 0, 0, 0]));
        assert_eq!(filled.pixel(Point::new(20, 14)), Some([0, 0, 255, 255]));
    }
    #[test]
    fn arrow_draws_stem_tip_and_head() {
        let mut frame = Frame::solid(Size::new(12, 10), [0, 0, 0, 0]).unwrap();
        frame
            .draw_arrow(
                Point::new(1, 5),
                Point::new(9, 5),
                [0, 0, 255, 255],
                1,
                LineDash::Solid,
                1,
            )
            .unwrap();
        assert_eq!(frame.pixel(Point::new(4, 5)), Some([0, 0, 255, 255]));
        assert!(painted(&frame, Point::new(9, 5), [0, 0, 255]));
        assert!(painted(&frame, Point::new(8, 4), [0, 0, 255]));
    }

    #[test]
    fn arrow_head_size_scales_the_wings() {
        let mut small = Frame::solid(Size::new(30, 30), [0, 0, 0, 0]).unwrap();
        small
            .draw_arrow(
                Point::new(1, 5),
                Point::new(25, 5),
                [0, 0, 255, 255],
                1,
                LineDash::Solid,
                1,
            )
            .unwrap();
        // Head size 1 keeps the wings near the tip; the mid-wing point of the
        // size-3 head stays empty.
        assert_eq!(small.pixel(Point::new(16, 10)), Some([0, 0, 0, 0]));

        let mut large = Frame::solid(Size::new(30, 30), [0, 0, 0, 0]).unwrap();
        large
            .draw_arrow(
                Point::new(1, 5),
                Point::new(25, 5),
                [0, 0, 255, 255],
                1,
                LineDash::Solid,
                3,
            )
            .unwrap();
        // head length = min(max(6, 4w) * 3, length) = 18, so the base sits at
        // x = 7 and the wing reaches y = 15; the shaft midpoint of that wing
        // line is (16, 10), where the wing's own edge lands on the sample.
        assert!(painted(&large, Point::new(16, 10), [0, 0, 255]));
    }

    // The filled head is the one part of an arrow whose edges are neither
    // axis-aligned nor a capsule: its two sides run from the tip back to the
    // wing tips at a slant, and a binary fill leaves them a staircase.  Qt's
    // preview antialiases the same triangle, and the committed image is what
    // the user compares against that preview.
    #[test]
    fn a_filled_arrow_head_has_a_soft_slanted_edge() {
        // The head alone, drawn at the geometry `draw_arrow_with_style` derives
        // for a 2..25 arrow of head size 3: tip at (25, 15), base at x = 7, and
        // wings reaching y = 5 and y = 25.
        let mut frame = Frame::solid(Size::new(30, 30), [0, 0, 0, 0]).unwrap();
        let frame_size = frame.size;
        stroke_with_coverage(&mut frame, [0, 0, 255, 255], (0, 0, 30, 30), |ink| {
            rasterize_triangle(
                ink,
                frame_size,
                Point::new(25, 15),
                Point::new(7, 5),
                Point::new(7, 25),
            );
        });
        // Every pixel the slant crosses takes the ink only in part, so a fill
        // that goes by a sample point leaves none of them: a hard edge shows up
        // as a count of zero.
        let partial = (0..30)
            .flat_map(|x| (0..30).map(move |y| Point::new(x, y)))
            .filter(|point| {
                frame
                    .pixel(*point)
                    .is_some_and(|pixel| pixel[3] > 0 && pixel[3] < 255)
            })
            .count();
        assert!(partial > 0, "the slanted sides are drawn hard");
        // The inside is still solid, so the soft edge is an edge and not a blur
        // over the whole head.
        assert_eq!(frame.pixel(Point::new(20, 15)), Some([0, 0, 255, 255]));
    }

    #[test]
    fn dashed_arrow_keeps_a_solid_head() {
        let mut frame = Frame::solid(Size::new(30, 10), [0, 0, 0, 0]).unwrap();
        frame
            .draw_arrow(
                Point::new(1, 5),
                Point::new(25, 5),
                [0, 0, 255, 255],
                1,
                LineDash::Dashed,
                1,
            )
            .unwrap();
        // Shaft gap: d = 4 lies in the first 3px off run.
        assert_eq!(frame.pixel(Point::new(5, 5)), Some([0, 0, 0, 0]));
        // The head is always solid: head length 6 puts the wing tip at (19, 8),
        // so (22, 7) is on the head's own edge and is painted at least partly.
        assert!(painted(&frame, Point::new(22, 7), [0, 0, 255]));
    }

    #[test]
    fn freehand_path_is_clipped_to_the_frame() {
        let mut frame = Frame::solid(Size::new(5, 5), [0, 0, 0, 0]).unwrap();
        frame
            .draw_freehand(
                &[Point::new(-2, 2), Point::new(2, 2), Point::new(2, 7)],
                [255, 255, 0, 255],
                1,
                LineDash::Solid,
            )
            .unwrap();
        assert_eq!(frame.pixel(Point::new(0, 2)), Some([255, 255, 0, 255]));
        assert_eq!(frame.pixel(Point::new(2, 4)), Some([255, 255, 0, 255]));
    }

    #[test]
    fn translucent_polyline_keeps_one_alpha_through_its_join() {
        let mut frame = Frame::solid(Size::new(12, 12), [0, 0, 0, 0]).unwrap();
        frame
            .draw_freehand(
                &[Point::new(2, 2), Point::new(8, 2), Point::new(8, 8)],
                [255, 0, 0, 128],
                3,
                LineDash::Solid,
            )
            .unwrap();
        // The 90-degree corner (8, 2) is covered by both capsules; without the
        // coverage mask it composited twice and came out darker. It has to
        // match the once-covered middle of a segment.
        let midpoint = frame.pixel(Point::new(5, 2)).unwrap();
        let corner = frame.pixel(Point::new(8, 2)).unwrap();
        assert_eq!(midpoint[3], 128, "one 128-alpha coat stays 128");
        assert_eq!(
            corner[3], midpoint[3],
            "the join must not blend a second time"
        );
    }

    #[test]
    fn mosaic_ellipse_pixelates_only_inside_the_ellipse() {
        let mut pixels = Vec::with_capacity(400);
        for x in 0..10 {
            for _y in 0..10 {
                pixels.extend_from_slice(&[x * 10, 0, 0, 255]);
            }
        }
        let mut frame = Frame::new(Size::new(10, 10), pixels).unwrap();
        frame.mosaic_ellipse(Rect::new(0, 0, 10, 10), 2).unwrap();
        // Corner of the bounding rect lies outside the ellipse: untouched.
        assert_eq!(frame.pixel(Point::new(0, 0)), Some([0, 0, 0, 255]));
        // The block (4..6, 4..6) is fully inside; its average of R values
        // {40, 40, 50, 50} is 45.
        assert_eq!(frame.pixel(Point::new(4, 4)), Some([45, 0, 0, 255]));
    }

    #[test]
    fn mosaic_brush_stamps_discs_from_the_pristine_source() {
        let mut frame = Frame::solid(Size::new(8, 8), [0, 0, 0, 255]).unwrap();
        frame.blend_pixel_at(4, 4, [130, 0, 0, 255]);
        // Radius-2 disc centered at (3,3) covers the bright pixel (4,4) plus
        // twelve black pixels: average R = (130 + 12/2) / 13 = 10.
        frame.mosaic_brush(&[Point::new(3, 3)], 2).unwrap();
        assert_eq!(frame.pixel(Point::new(3, 3)), Some([10, 0, 0, 255]));
        // Disc reach ends at distance 2; (0, 0) stays untouched.
        assert_eq!(frame.pixel(Point::new(0, 0)), Some([0, 0, 0, 255]));
        assert_eq!(frame.pixel(Point::new(4, 4)), Some([10, 0, 0, 255]));
    }

    #[test]
    fn text_uses_ascii_five_by_seven_glyphs() {
        let mut frame = Frame::solid(Size::new(8, 8), [0, 0, 0, 0]).unwrap();
        frame
            .draw_text(Point::new(0, 0), "A", [255, 255, 255, 255], 1)
            .unwrap();
        assert_eq!(frame.pixel(Point::new(1, 0)), Some([255, 255, 255, 255]));
        assert_eq!(frame.pixel(Point::new(0, 3)), Some([255, 255, 255, 255]));
        assert_eq!(frame.pixel(Point::new(0, 0)), Some([0, 0, 0, 0]));
        assert!(matches!(
            frame.draw_text(Point::new(0, 0), "中", [255, 255, 255, 255], 1),
            Err(VshotError::UnsupportedText(_))
        ));
    }

    #[test]
    fn mosaic_averages_each_block_independently() {
        let mut frame = Frame::new(
            Size::new(4, 2),
            vec![
                0, 0, 0, 255, 10, 20, 30, 255, 100, 110, 120, 255, 110, 120, 130, 255, 20, 40, 60,
                255, 30, 60, 90, 255, 120, 130, 140, 255, 130, 140, 150, 255,
            ],
        )
        .unwrap();
        frame.mosaic(Rect::new(0, 0, 4, 2), 2).unwrap();
        assert_eq!(frame.pixel(Point::new(0, 0)), Some([15, 30, 45, 255]));
        assert_eq!(frame.pixel(Point::new(1, 1)), Some([15, 30, 45, 255]));
        assert_eq!(frame.pixel(Point::new(2, 0)), Some([115, 125, 135, 255]));
        assert_eq!(frame.pixel(Point::new(3, 1)), Some([115, 125, 135, 255]));
    }

    #[test]
    fn drawing_rejects_invalid_dimensions_and_handles_out_of_bounds() {
        let mut frame = Frame::solid(Size::new(3, 3), [0, 0, 0, 0]).unwrap();
        assert!(frame
            .stroke_rectangle(Rect::new(0, 0, 0, 1), [1, 2, 3, 255], 1, LineDash::Solid)
            .is_err());
        assert!(frame
            .stroke_rectangle(Rect::new(0, 0, 1, 1), [1, 2, 3, 255], 0, LineDash::Solid)
            .is_err());
        assert!(frame
            .stroke_circle(Point::new(1, 1), 0, [1, 2, 3, 255], 1)
            .is_err());
        assert!(frame
            .draw_arrow(
                Point::new(1, 1),
                Point::new(1, 1),
                [1, 2, 3, 255],
                1,
                LineDash::Solid,
                1
            )
            .is_err());
        assert!(frame
            .draw_freehand(&[], [1, 2, 3, 255], 1, LineDash::Solid)
            .is_err());
        assert!(frame
            .draw_text(Point::new(0, 0), "A", [1, 2, 3, 255], 0)
            .is_err());
        assert!(frame.mosaic(Rect::new(0, 0, 1, 1), 0).is_err());
        assert!(frame.mosaic_ellipse(Rect::new(0, 0, 1, 1), 0).is_err());
        assert!(frame.mosaic_brush(&[], 1).is_err());
        assert!(frame.mosaic_brush(&[Point::new(1, 1)], 0).is_err());

        let unchanged = frame.clone();
        frame
            .stroke_rectangle(Rect::new(20, 20, 2, 2), [1, 2, 3, 255], 1, LineDash::Solid)
            .unwrap();
        frame
            .stroke_circle(Point::new(-20, -20), 2, [1, 2, 3, 255], 1)
            .unwrap();
        frame
            .draw_freehand(
                &[Point::new(-20, -20), Point::new(-10, -10)],
                [1, 2, 3, 255],
                1,
                LineDash::Solid,
            )
            .unwrap();
        frame.mosaic(Rect::new(20, 20, 2, 2), 2).unwrap();
        frame.mosaic_ellipse(Rect::new(20, 20, 2, 2), 2).unwrap();
        frame.mosaic_brush(&[Point::new(-20, -20)], 2).unwrap();
        assert_eq!(frame, unchanged);
    }

    /// A 2x2 quadrant bitmap: red, green / blue, yellow at full alpha.
    fn quadrants() -> TextBitmap {
        TextBitmap {
            width: 2,
            height: 2,
            pixels: vec![
                255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255,
            ],
        }
    }

    #[test]
    fn draw_bitmap_scaled_enlarges_into_matching_quadrants() {
        let mut frame = Frame::solid(Size::new(8, 8), [0, 0, 0, 255]).unwrap();
        frame
            .draw_bitmap_scaled(Rect::new(0, 0, 8, 8), &quadrants())
            .unwrap();
        // 8x8 over a 2x2 source is an exact 4x blow-up, so each destination
        // pixel averages exactly one source pixel.
        assert_eq!(frame.pixel(Point::new(0, 0)), Some([255, 0, 0, 255]));
        assert_eq!(frame.pixel(Point::new(7, 0)), Some([0, 255, 0, 255]));
        assert_eq!(frame.pixel(Point::new(0, 7)), Some([0, 0, 255, 255]));
        assert_eq!(frame.pixel(Point::new(7, 7)), Some([255, 255, 0, 255]));
    }

    #[test]
    fn draw_bitmap_scaled_reduces_by_averaging_the_pixels_it_covers() {
        let mut frame = Frame::solid(Size::new(2, 1), [0, 0, 0, 255]).unwrap();
        let bitmap = TextBitmap {
            width: 4,
            height: 1,
            pixels: vec![
                255, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0, 255, 0, 0, 255, 255,
            ],
        };
        frame
            .draw_bitmap_scaled(Rect::new(0, 0, 2, 1), &bitmap)
            .unwrap();
        // Halving 4 pixels to 2 averages red with blue at full alpha.
        assert_eq!(frame.pixel(Point::new(0, 0)), Some([127, 0, 127, 255]));
        assert_eq!(frame.pixel(Point::new(1, 0)), Some([127, 0, 127, 255]));
    }

    #[test]
    fn draw_bitmap_scaled_at_its_own_size_matches_the_direct_blit() {
        let rect = Rect::new(1, 1, 2, 2);
        let mut scaled = Frame::solid(Size::new(4, 4), [0, 0, 0, 255]).unwrap();
        scaled.draw_bitmap_scaled(rect, &quadrants()).unwrap();
        let mut direct = Frame::solid(Size::new(4, 4), [0, 0, 0, 255]).unwrap();
        direct.draw_bitmap(Point::new(1, 1), &quadrants()).unwrap();
        assert_eq!(scaled, direct);
        // Transparent source pixels leave the canvas untouched on both paths.
        let opaque = Frame::solid(Size::new(4, 4), [9, 9, 9, 255]).unwrap();
        let transparent = TextBitmap {
            width: 1,
            height: 1,
            pixels: vec![1, 2, 3, 0],
        };
        assert_eq!(
            {
                let mut frame = opaque.clone();
                frame
                    .draw_bitmap_scaled(Rect::new(1, 1, 1, 1), &transparent)
                    .unwrap();
                frame
            },
            opaque
        );
    }

    #[test]
    fn draw_bitmap_scaled_clips_and_rejects_bad_payloads() {
        let mut frame = Frame::solid(Size::new(4, 4), [0, 0, 0, 255]).unwrap();
        // A rect starting at -4 covers dest 0..4 with its own columns 4..8,
        // which is the bitmap's bottom-right quadrant and nothing else.
        frame
            .draw_bitmap_scaled(Rect::new(-4, -4, 8, 8), &quadrants())
            .unwrap();
        assert_eq!(frame.pixel(Point::new(0, 0)), Some([255, 255, 0, 255]));
        assert_eq!(frame.pixel(Point::new(3, 3)), Some([255, 255, 0, 255]));
        // A rect wholly inside draws only its own footprint.
        let mut inside = Frame::solid(Size::new(8, 8), [0, 0, 0, 255]).unwrap();
        inside
            .draw_bitmap_scaled(Rect::new(0, 0, 8, 8), &quadrants())
            .unwrap();
        assert_eq!(inside.pixel(Point::new(0, 0)), Some([255, 0, 0, 255]));
        assert_eq!(inside.pixel(Point::new(3, 3)), Some([255, 0, 0, 255]));
        assert_eq!(inside.pixel(Point::new(4, 4)), Some([255, 255, 0, 255]));
        // An off-canvas rect and a zero-sized one are no-ops.
        let unchanged = frame.clone();
        frame
            .draw_bitmap_scaled(Rect::new(20, 20, 4, 4), &quadrants())
            .unwrap();
        frame
            .draw_bitmap_scaled(Rect::new(0, 0, 0, 4), &quadrants())
            .unwrap();
        assert_eq!(frame, unchanged);
        // A payload that disagrees with the declared dimensions is an error.
        let broken = TextBitmap {
            width: 2,
            height: 2,
            pixels: vec![0, 0, 0, 255],
        };
        assert!(frame
            .draw_bitmap_scaled(Rect::new(0, 0, 8, 8), &broken)
            .is_err());
    }

    // A wave at scale 1 with the smallest sizes: amplitude 6 and wavelength 18.
    // The line is 54px long, exactly three whole periods, so the far endpoint
    // comes back onto the centre line (`sin` of a whole number of periods is 0).
    const WAVE_START: Point = Point::new(4, 16);
    const WAVE_END: Point = Point::new(58, 16);

    #[test]
    fn wave_endpoints_sit_on_the_centre_line() {
        let mut frame = Frame::solid(Size::new(64, 32), [0, 0, 0, 0]).unwrap();
        frame
            .draw_wave(WAVE_START, WAVE_END, [255, 0, 0, 255], 1, 6, 18)
            .unwrap();
        // The wave is rounded to whole cycles, so both endpoints are covered
        // on the centre line y = 16.  The pen is a pixel wide, so the endpoint
        // pixel is only half covered by it — anti-aliased, as Qt's own preview
        // draws it — and what is asserted is that the colour lands there at
        // all rather than at some other row.
        assert!(painted(&frame, WAVE_START, [255, 0, 0]), "start");
        assert!(painted(&frame, WAVE_END, [255, 0, 0]), "end");
    }

    #[test]
    fn a_wave_ends_on_its_end_point_even_at_a_partial_period() {
        let mut frame = Frame::solid(Size::new(64, 32), [0, 0, 0, 0]).unwrap();
        // 50 pixels is not a whole multiple of the 18-pixel wavelength, so a
        // free-running phase would leave the wave off the centre line here.
        let end = Point::new(54, 16);
        frame
            .draw_wave(Point::new(4, 16), end, [255, 0, 0, 255], 1, 6, 18)
            .unwrap();
        assert!(painted(&frame, Point::new(4, 16), [255, 0, 0]), "start");
        assert!(painted(&frame, end, [255, 0, 0]), "end");
    }

    #[test]
    fn wave_crests_reach_the_amplitude_and_do_not_overshoot() {
        let mut frame = Frame::solid(Size::new(64, 32), [0, 0, 0, 0]).unwrap();
        // A width-1 pen: its half-width is 0.5, so the crest — 5.9 px off the
        // centre line at the sampled peak — reaches at most into the row at
        // offset 6 and no further.  Anti-aliasing puts part of the pen in that
        // row rather than all of it, so the row is read as painted rather than
        // as solid.
        frame
            .draw_wave(WAVE_START, WAVE_END, [255, 0, 0, 255], 1, 6, 18)
            .unwrap();
        let mut above = 0i64;
        let mut below = 0i64;
        for y in 0..32 {
            for x in 0..64 {
                if painted(&frame, Point::new(x, y), [255, 0, 0]) {
                    let up = i64::from(y) - 16;
                    above = above.max(up);
                    below = below.max(-up);
                }
            }
        }
        // The crest is 6 above the centre line, and the pen covers the row it
        // lands on: nothing reaches further than that.
        assert_eq!(above, 6, "crest reaches the amplitude");
        assert_eq!(below, 6, "trough reaches the amplitude");
    }

    #[test]
    fn wave_sampling_density_follows_the_line_length() {
        // n = max(2, ceil(length) + 1): one sample per device pixel plus both
        // endpoints, so a ten-times longer line gets ten times the samples.
        let short = wave_polyline(Point::new(0, 0), Point::new(9, 0), 4, 18);
        let long = wave_polyline(Point::new(0, 0), Point::new(99, 0), 4, 18);
        assert_eq!(short.len(), 10);
        assert_eq!(long.len(), 100);
        assert!(
            long.len() > short.len() * 5,
            "{} vs {}",
            long.len(),
            short.len()
        );
    }

    #[test]
    fn translucent_wave_keeps_one_alpha_through_its_crests() {
        let mut frame = Frame::solid(Size::new(64, 32), [0, 0, 0, 0]).unwrap();
        frame
            .draw_wave(WAVE_START, WAVE_END, [255, 0, 0, 128], 3, 6, 18)
            .unwrap();
        // A 3px pen at 128 alpha. The crest at x = 8 stands farthest off the
        // line (y = 16 + 6 * sin(80°) = 21.9); the centre line is crossed at
        // x = 22 (u = 18, one whole period). Dense sampling makes the wave
        // overlap itself at every turnaround, so without the coverage mask the
        // crest would composite twice and come out darker than the crossing.
        let crossing = frame.pixel(Point::new(22, 16)).unwrap();
        let crest = frame.pixel(Point::new(8, 22)).unwrap();
        assert_eq!(crossing[3], 128, "one 128-alpha coat stays 128");
        assert_eq!(
            crest[3], crossing[3],
            "a crest must not blend a second time"
        );
    }

    #[test]
    fn zero_length_wave_draws_a_dot_and_never_panics() {
        let mut frame = Frame::solid(Size::new(16, 16), [0, 0, 0, 0]).unwrap();
        // A click without a drag degenerates to a single dot on the point,
        // matching how the pen renders a zero-length path.
        frame
            .draw_wave(
                Point::new(8, 8),
                Point::new(8, 8),
                [0, 255, 0, 255],
                3,
                6,
                18,
            )
            .unwrap();
        assert_eq!(frame.pixel(Point::new(8, 8)), Some([0, 255, 0, 255]));

        // A zero wavelength would divide by zero if it were not floored.
        frame
            .draw_wave(
                Point::new(0, 0),
                Point::new(10, 0),
                [0, 255, 0, 255],
                1,
                6,
                0,
            )
            .unwrap();

        // Off-canvas and zero-width calls stay inside the frame and reject a
        // zero stroke width rather than panicking.
        let before = frame.clone();
        frame
            .draw_wave(
                Point::new(-50, -50),
                Point::new(-40, -40),
                [0, 255, 0, 255],
                1,
                6,
                18,
            )
            .unwrap();
        assert_eq!(frame, before);
        assert!(frame
            .draw_wave(Point::new(0, 0), Point::new(4, 0), [1, 2, 3, 255], 0, 6, 18)
            .is_err());
    }

    // A triangle with straight edges: the out-handle of every anchor sits on
    // the anchor itself, so each cubic segment degenerates to a line.
    const TRIANGLE: [Point; 6] = [
        Point::new(4, 4),
        Point::new(4, 4),
        Point::new(20, 4),
        Point::new(20, 4),
        Point::new(12, 20),
        Point::new(12, 20),
    ];

    #[test]
    fn a_bezier_whose_handles_sit_on_the_endpoints_flattens_to_one_chord() {
        // Both control points coincide with an endpoint, so the curve is a
        // straight line and must collapse to a single chord rather than being
        // subdivided down to the depth cap.
        let polyline = bezier_polyline(&TRIANGLE[0..4]);
        assert_eq!(polyline, vec![Point::new(4, 4), Point::new(20, 4)]);

        // And it strokes that chord: the midpoint is inked, a pixel a row below
        // is not.
        let mut frame = Frame::solid(Size::new(24, 12), [0, 0, 0, 0]).unwrap();
        frame
            .draw_bezier(
                &TRIANGLE[0..4],
                false,
                BezierFill::Both,
                [255, 0, 0, 255],
                1,
                LineDash::Solid,
            )
            .unwrap();
        assert_eq!(frame.pixel(Point::new(12, 4)), Some([255, 0, 0, 255]));
        assert_eq!(frame.pixel(Point::new(12, 6)), Some([0, 0, 0, 0]));
    }

    #[test]
    fn a_closed_bezier_fills_at_half_alpha_then_strokes_opaque() {
        let mut frame = Frame::solid(Size::new(24, 24), [0, 0, 0, 0]).unwrap();
        frame
            .draw_bezier(
                &TRIANGLE,
                true,
                BezierFill::Both,
                [255, 0, 0, 255],
                1,
                LineDash::Solid,
            )
            .unwrap();
        // The interior centroid (12, 9) is covered by the fill only: half of
        // the opaque stroke's alpha, floored.
        assert_eq!(frame.pixel(Point::new(12, 9)), Some([255, 0, 0, 127]));
        // A pixel on the top edge is stroked at full alpha; the fill underneath
        // is overwritten by the opaque stroke.
        assert_eq!(frame.pixel(Point::new(12, 4)), Some([255, 0, 0, 255]));
        // A pixel outside the triangle is untouched.
        assert_eq!(frame.pixel(Point::new(0, 0)), Some([0, 0, 0, 0]));

        // The same path left open only strokes: its interior stays clear.
        let mut open = Frame::solid(Size::new(24, 24), [0, 0, 0, 0]).unwrap();
        open.draw_bezier(
            &TRIANGLE,
            false,
            BezierFill::Both,
            [255, 0, 0, 255],
            1,
            LineDash::Solid,
        )
        .unwrap();
        assert_eq!(open.pixel(Point::new(12, 9)), Some([0, 0, 0, 0]));
        assert_eq!(open.pixel(Point::new(12, 4)), Some([255, 0, 0, 255]));
    }

    #[test]
    fn a_stroke_only_bezier_leaves_even_a_closed_path_hollow() {
        let mut frame = Frame::solid(Size::new(24, 24), [0, 0, 0, 0]).unwrap();
        frame
            .draw_bezier(
                &TRIANGLE,
                true,
                BezierFill::Stroke,
                [255, 0, 0, 255],
                1,
                LineDash::Solid,
            )
            .unwrap();
        // The interior stays clear even though the pen closed the path.
        assert_eq!(frame.pixel(Point::new(12, 9)), Some([0, 0, 0, 0]));
        // The outline is still stroked, including the segment that closes the
        // path: (12, 20) back to (4, 4) passes through (8, 12).  A stroke's
        // edge is anti-aliased, so those samples read as painted rather than as
        // a full 255.
        assert!(painted(&frame, Point::new(12, 4), [255, 0, 0]));
        assert!(painted(&frame, Point::new(8, 12), [255, 0, 0]));
    }

    #[test]
    fn a_fill_only_bezier_strokes_an_open_path_instead_of_closing_it() {
        let mut frame = Frame::solid(Size::new(24, 24), [0, 0, 0, 0]).unwrap();
        frame
            .draw_bezier(
                &TRIANGLE,
                false,
                BezierFill::Fill,
                [255, 0, 0, 255],
                1,
                LineDash::Solid,
            )
            .unwrap();
        // An open path has no interior to fill, and filling one would close the
        // outline implicitly -- showing the user a shape they never closed. The
        // outline is stroked instead, at the pen's own alpha rather than the
        // half-alpha coat a fill would leave.
        assert_eq!(frame.pixel(Point::new(12, 9)), Some([0, 0, 0, 0]));
        assert_eq!(frame.pixel(Point::new(12, 4)), Some([255, 0, 0, 255]));
        // Outside the outline, too, nothing is painted: the implicit closure
        // would have covered the triangle's interior.
        assert_eq!(frame.pixel(Point::new(0, 0)), Some([0, 0, 0, 0]));
    }

    #[test]
    fn a_both_bezier_does_not_fill_an_open_path() {
        let mut frame = Frame::solid(Size::new(24, 24), [0, 0, 0, 0]).unwrap();
        frame
            .draw_bezier(
                &TRIANGLE,
                false,
                BezierFill::Both,
                [255, 0, 0, 255],
                1,
                LineDash::Solid,
            )
            .unwrap();
        // Open paths are stroked alone under the default mode; only a path the
        // pen actually closed gets its interior painted.
        assert_eq!(frame.pixel(Point::new(12, 9)), Some([0, 0, 0, 0]));
        assert_eq!(frame.pixel(Point::new(12, 4)), Some([255, 0, 0, 255]));
    }

    #[test]
    fn fill_polygon_covers_a_concave_shape_without_spilling() {
        // An L: the notch at the bottom right is outside the polygon.
        let l_shape = [
            Point::new(2, 2),
            Point::new(12, 2),
            Point::new(12, 6),
            Point::new(6, 6),
            Point::new(6, 12),
            Point::new(2, 12),
        ];
        let mut frame = Frame::solid(Size::new(16, 16), [0, 0, 0, 0]).unwrap();
        frame.fill_polygon(&l_shape, [0, 255, 0, 255]);
        // Inside the arms.
        assert_eq!(frame.pixel(Point::new(10, 4)), Some([0, 255, 0, 255]));
        assert_eq!(frame.pixel(Point::new(4, 10)), Some([0, 255, 0, 255]));
        assert_eq!(frame.pixel(Point::new(4, 4)), Some([0, 255, 0, 255]));
        // The notch is not filled.
        assert_eq!(frame.pixel(Point::new(10, 10)), Some([0, 0, 0, 0]));
        // Nor is anything outside the bounding box's far corner.
        assert_eq!(frame.pixel(Point::new(0, 0)), Some([0, 0, 0, 0]));
        assert_eq!(frame.pixel(Point::new(14, 14)), Some([0, 0, 0, 0]));
    }

    #[test]
    fn translucent_bezier_keeps_one_alpha_through_its_corner() {
        // An L-shaped curve: a horizontal chord into a vertical one, meeting at
        // (20, 4). Both capsules cover that corner.
        let corner = [
            Point::new(4, 4),
            Point::new(4, 4),
            Point::new(20, 4),
            Point::new(20, 4),
            Point::new(20, 20),
            Point::new(20, 20),
        ];
        let mut frame = Frame::solid(Size::new(24, 24), [0, 0, 0, 0]).unwrap();
        frame
            .draw_bezier(
                &corner,
                false,
                BezierFill::Both,
                [255, 0, 0, 128],
                3,
                LineDash::Solid,
            )
            .unwrap();
        let midpoint = frame.pixel(Point::new(12, 4)).unwrap();
        let joint = frame.pixel(Point::new(20, 4)).unwrap();
        assert_eq!(midpoint[3], 128, "one 128-alpha coat stays 128");
        assert_eq!(
            joint[3], midpoint[3],
            "the corner must not blend a second time"
        );
    }

    #[test]
    fn a_bezier_with_extreme_handles_stops_at_the_depth_cap() {
        // Handles flung far from the anchors force subdivision all the way to
        // the cap; the point count stays bounded and the recursion never gets
        // deep enough to overflow the stack.
        let points = [
            Point::new(0, 0),
            Point::new(i32::MAX, i32::MAX),
            Point::new(100, 0),
            Point::new(0, i32::MIN),
        ];
        let polyline = bezier_polyline(&points);
        assert!(polyline.len() > 2, "the curve is subdivided");
        assert!(
            polyline.len() <= (1usize << BEZIER_MAX_DEPTH) + 1,
            "flattening is bounded by the depth cap, got {}",
            polyline.len()
        );
    }
}

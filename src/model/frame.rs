#![allow(dead_code)]

use image::{DynamicImage, ImageFormat};

use crate::edit::{ArrowStyle, LineDash, TextBitmap};
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
        for pixel in pixels.chunks_exact_mut(4) {
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
            self.stamp_dashed_polyline(&path, color, width, dash);
            return Ok(());
        }
        let stroke_width = i64::from(width);
        let Some((x_start, x_end)) = clip_range(left, right, self.size.width) else {
            return Ok(());
        };
        let Some((y_start, y_end)) = clip_range(top, bottom, self.size.height) else {
            return Ok(());
        };
        for y in y_start..y_end {
            for x in x_start..x_end {
                if x - left < stroke_width
                    || right - x <= stroke_width
                    || y - top < stroke_width
                    || bottom - y <= stroke_width
                {
                    self.blend_pixel_at(x, y, color);
                }
            }
        }
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
        let radius = i128::from(radius);
        let stroke_width = i128::from(width);
        let outer_radius = radius + (stroke_width + 1) / 2;
        let center_x = i128::from(center.x);
        let center_y = i128::from(center.y);
        let x_start = (center_x - outer_radius).max(0);
        let x_end = (center_x + outer_radius + 1).min(i128::from(self.size.width));
        let y_start = (center_y - outer_radius).max(0);
        let y_end = (center_y + outer_radius + 1).min(i128::from(self.size.height));
        if x_start >= x_end || y_start >= y_end {
            return Ok(());
        }

        let inner_radius_twice = (radius * 2 - stroke_width).max(0);
        let outer_radius_twice = radius * 2 + stroke_width;
        let inner_distance = inner_radius_twice * inner_radius_twice;
        let outer_distance = outer_radius_twice * outer_radius_twice;
        for y in y_start..y_end {
            for x in x_start..x_end {
                let dx = x - center_x;
                let dy = y - center_y;
                let distance = 4 * (dx * dx + dy * dy);
                if distance >= inner_distance && distance <= outer_distance {
                    self.blend_pixel_at(x as i64, y as i64, color);
                }
            }
        }
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
        if dash != LineDash::Solid {
            self.stamp_dashed_ellipse((left, top, right, bottom), color, width, dash);
            return Ok(());
        }
        let rect_width = i128::from(right - left);
        let rect_height = i128::from(bottom - top);
        let stroke_width = i128::from(width);
        let center_x = i128::from(left) + rect_width / 2;
        let center_y = i128::from(top) + rect_height / 2;
        let outer_a = (rect_width + stroke_width) / 2;
        let outer_b = (rect_height + stroke_width) / 2;
        let inner_a = rect_width.saturating_sub(stroke_width) / 2;
        let inner_b = rect_height.saturating_sub(stroke_width) / 2;
        let outer_a_squared = outer_a * outer_a;
        let outer_b_squared = outer_b * outer_b;
        let inner_a_squared = inner_a * inner_a;
        let inner_b_squared = inner_b * inner_b;
        let outer_threshold = outer_a_squared * outer_b_squared;
        let inner_threshold = inner_a_squared * inner_b_squared;
        let has_inner = inner_a > 0 && inner_b > 0;
        let x_start = (center_x - outer_a).max(0);
        let x_end = (center_x + outer_a + 1).min(i128::from(self.size.width));
        let y_start = (center_y - outer_b).max(0);
        let y_end = (center_y + outer_b + 1).min(i128::from(self.size.height));
        if x_start >= x_end || y_start >= y_end {
            return Ok(());
        }
        for y in y_start..y_end {
            for x in x_start..x_end {
                let dx = x - center_x;
                let dy = y - center_y;
                // Pixel is inside the outer ellipse and outside the inner ellipse:
                // dx²/a² + dy²/b² <= 1 is equivalent to dx²·b² + dy²·a² <= a²·b².
                if dx * dx * outer_b_squared + dy * dy * outer_a_squared > outer_threshold {
                    continue;
                }
                if has_inner
                    && dx * dx * inner_b_squared + dy * dy * inner_a_squared < inner_threshold
                {
                    continue;
                }
                self.blend_pixel_at(x as i64, y as i64, color);
            }
        }
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
        if dash == LineDash::Solid {
            self.stroke_segment(start, end, color, width);
        } else {
            self.stamp_dashed_polyline(&[start, end], color, width, dash);
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
        if style == ArrowStyle::Filled {
            self.fill_triangle(end, left, right, color);
        }
        self.stroke_segment(end, left, color, width);
        self.stroke_segment(end, right, color, width);
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
        if dash != LineDash::Solid && points.len() >= 2 {
            self.stamp_dashed_polyline(points, color, width, dash);
            return Ok(());
        }
        if points.len() == 1 {
            self.stroke_segment(points[0], points[0], color, width);
            return Ok(());
        }
        for segment in points.windows(2) {
            self.stroke_segment(segment[0], segment[1], color, width);
        }
        Ok(())
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

    fn stamp_square(&mut self, center_x: i64, center_y: i64, size: u32, color: [u8; 4]) {
        let size = i64::from(size.max(1));
        let half = (size - 1) / 2;
        let x_start = (center_x - half).max(0);
        let y_start = (center_y - half).max(0);
        let x_end = (center_x - half + size).min(i64::from(self.size.width));
        let y_end = (center_y - half + size).min(i64::from(self.size.height));
        for y in y_start..y_end {
            for x in x_start..x_end {
                self.blend_pixel_at(x, y, color);
            }
        }
    }

    /// Walks the polyline at 1px steps, stamping `size`-wide squares wherever
    /// the cumulative distance falls inside the dash pattern's on phase.
    fn stamp_dashed_polyline(
        &mut self,
        points: &[Point],
        color: [u8; 4],
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
                    self.stamp_square(x, y, size, color);
                }
            }
            base += length;
        }
    }

    /// Stamps dashes along the ellipse inscribed in the rect, mirroring the
    /// solid ring's midline so dashed and solid ellipses overlap exactly.
    /// `bounds` holds the exclusive `(left, top, right, bottom)` device edges.
    fn stamp_dashed_ellipse(
        &mut self,
        bounds: (i64, i64, i64, i64),
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    ) {
        let (left, top, right, bottom) = bounds;
        let center_x = left as f64 + (right - left) as f64 / 2.0;
        let center_y = top as f64 + (bottom - top) as f64 / 2.0;
        let a = (right - left) as f64 / 2.0;
        let b = (bottom - top) as f64 / 2.0;
        let perimeter =
            std::f64::consts::PI * (3.0 * (a + b) - ((3.0 * a + b) * (a + 3.0 * b)).sqrt());
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
                self.stamp_square(x, y, width, color);
            }
        }
    }

    fn fill_triangle(&mut self, first: Point, second: Point, third: Point, color: [u8; 4]) {
        let min_x = i64::from(first.x)
            .min(i64::from(second.x))
            .min(i64::from(third.x))
            .max(0);
        let max_x = i64::from(first.x)
            .max(i64::from(second.x))
            .max(i64::from(third.x))
            .min(i64::from(self.size.width).saturating_sub(1));
        let min_y = i64::from(first.y)
            .min(i64::from(second.y))
            .min(i64::from(third.y))
            .max(0);
        let max_y = i64::from(first.y)
            .max(i64::from(second.y))
            .max(i64::from(third.y))
            .min(i64::from(self.size.height).saturating_sub(1));
        if min_x > max_x || min_y > max_y {
            return;
        }

        let edge = |a: Point, b: Point, x: i64, y: i64| {
            (i128::from(b.x) - i128::from(a.x)) * (i128::from(y) - i128::from(a.y))
                - (i128::from(b.y) - i128::from(a.y)) * (i128::from(x) - i128::from(a.x))
        };
        let area = edge(first, second, i64::from(third.x), i64::from(third.y));
        if area == 0 {
            return;
        }
        for y in min_y..=max_y {
            for x in min_x..=max_x {
                let first_edge = edge(first, second, x, y);
                let second_edge = edge(second, third, x, y);
                let third_edge = edge(third, first, x, y);
                let inside = if area > 0 {
                    first_edge >= 0 && second_edge >= 0 && third_edge >= 0
                } else {
                    first_edge <= 0 && second_edge <= 0 && third_edge <= 0
                };
                if inside {
                    self.blend_pixel_at(x, y, color);
                }
            }
        }
    }

    fn stroke_segment(&mut self, start: Point, end: Point, color: [u8; 4], width: u32) {
        let padding = (i128::from(width) + 1) / 2;
        let start_x = i128::from(start.x);
        let start_y = i128::from(start.y);
        let end_x = i128::from(end.x);
        let end_y = i128::from(end.y);
        let x_start = (start_x.min(end_x) - padding).max(0);
        let x_end = (start_x.max(end_x) + padding + 1).min(i128::from(self.size.width));
        let y_start = (start_y.min(end_y) - padding).max(0);
        let y_end = (start_y.max(end_y) + padding + 1).min(i128::from(self.size.height));
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
        for y in y_start..y_end {
            for x in x_start..x_end {
                let px = x as f64;
                let py = y as f64;
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
                    self.blend_pixel_at(x as i64, y as i64, color);
                }
            }
        }
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
    /// Parses the `--png-compression` value.
    pub fn parse(value: &str) -> Result<Self> {
        match value {
            "none" => Ok(Self::None),
            "fastest" => Ok(Self::Fastest),
            "fast" => Ok(Self::Fast),
            "balanced" => Ok(Self::Balanced),
            "high" => Ok(Self::High),
            other => Err(VshotError::InvalidDestination(format!(
                "`--png-compression {other}` is not one of none, fastest, fast, balanced, high"
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
        assert!(error.to_string().contains("is not one of"), "{error}");
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
        // Ring passes through the rect edge midpoints; the center stays transparent.
        for point in [
            Point::new(0, 4),
            Point::new(9, 4),
            Point::new(4, 0),
            Point::new(4, 7),
        ] {
            assert_eq!(frame.pixel(point), Some([0, 255, 0, 255]), "{point:?}");
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
        assert_eq!(frame.pixel(Point::new(9, 5)), Some([0, 0, 255, 255]));
        assert_eq!(frame.pixel(Point::new(8, 4)), Some([0, 0, 255, 255]));
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
        // line is (16, 10).
        assert_eq!(large.pixel(Point::new(16, 10)), Some([0, 0, 255, 255]));
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
        // The head is always solid: head length 6 puts the wing tip at (19, 8).
        assert_eq!(frame.pixel(Point::new(22, 7)), Some([0, 0, 255, 255]));
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
}

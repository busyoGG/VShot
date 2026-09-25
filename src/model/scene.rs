// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#![allow(dead_code)]

use super::frame::Frame;
use crate::error::{Result, VshotError};
use crate::geometry::{Rect, Size};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct OutputSnapshot {
    pub global_id: u32,
    pub name: String,
    pub geometry: Rect,
    pub scale: u32,
    pub frame: Frame,
}

impl OutputSnapshot {
    pub fn new(
        global_id: u32,
        name: impl Into<String>,
        geometry: Rect,
        scale: u32,
        frame: Frame,
    ) -> Result<Self> {
        let name = name.into();
        if scale == 0 {
            return Err(VshotError::UnsupportedOutput(format!(
                "output {name} has an invalid scale"
            )));
        }
        let expected_width =
            geometry.size.width.checked_mul(scale).ok_or_else(|| {
                VshotError::UnsupportedOutput("output pixel width overflows".into())
            })?;
        let expected_height =
            geometry.size.height.checked_mul(scale).ok_or_else(|| {
                VshotError::UnsupportedOutput("output pixel height overflows".into())
            })?;
        if frame.size() != Size::new(expected_width, expected_height) {
            return Err(VshotError::UnsupportedOutput(format!(
                "output frame for {name} is {}x{}, expected {}x{} for logical scale {scale}",
                frame.size().width,
                frame.size().height,
                expected_width,
                expected_height
            )));
        }
        Ok(Self {
            global_id,
            name,
            geometry,
            scale,
            frame,
        })
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SceneSnapshot {
    bounds: Rect,
    scale: u32,
    frame: Frame,
    outputs: Vec<OutputSnapshot>,
}

impl SceneSnapshot {
    pub fn from_outputs(outputs: Vec<OutputSnapshot>) -> Result<Self> {
        if outputs.is_empty() {
            return Err(VshotError::IncompleteTopology(
                "no outputs were captured".into(),
            ));
        }
        let scale = outputs
            .iter()
            .map(|output| output.scale)
            .max()
            .ok_or_else(|| VshotError::IncompleteTopology("no output scale was captured".into()))?;
        if scale == 0 || outputs.iter().any(|output| output.scale == 0) {
            return Err(VshotError::UnsupportedOutput(
                "output scale must be greater than zero".into(),
            ));
        }
        let min_x = outputs
            .iter()
            .map(|output| output.geometry.left())
            .min()
            .unwrap();
        let min_y = outputs
            .iter()
            .map(|output| output.geometry.top())
            .min()
            .unwrap();
        let max_x = outputs
            .iter()
            .map(|output| output.geometry.right())
            .collect::<Result<Vec<_>>>()?
            .into_iter()
            .max()
            .unwrap();
        let max_y = outputs
            .iter()
            .map(|output| output.geometry.bottom())
            .collect::<Result<Vec<_>>>()?
            .into_iter()
            .max()
            .unwrap();
        let width = u32::try_from(i64::from(max_x) - i64::from(min_x))
            .map_err(|_| VshotError::UnsupportedOutput("scene width is out of range".into()))?;
        let height = u32::try_from(i64::from(max_y) - i64::from(min_y))
            .map_err(|_| VshotError::UnsupportedOutput("scene height is out of range".into()))?;
        let pixel_width = width
            .checked_mul(scale)
            .ok_or_else(|| VshotError::UnsupportedOutput("scene pixel width overflows".into()))?;
        let pixel_height = height
            .checked_mul(scale)
            .ok_or_else(|| VshotError::UnsupportedOutput("scene pixel height overflows".into()))?;
        let pixel_area = Size::new(pixel_width, pixel_height).area()?;
        let mut pixels = vec![
            0u8;
            pixel_area.checked_mul(4).ok_or_else(|| {
                VshotError::UnsupportedOutput("scene is too large".into())
            })?
        ];

        for output in &outputs {
            let destination_x = u32::try_from(i64::from(output.geometry.left()) - i64::from(min_x))
                .map_err(|_| {
                    VshotError::UnsupportedOutput("output x offset is out of range".into())
                })?
                .checked_mul(scale)
                .ok_or_else(|| VshotError::UnsupportedOutput("output x offset overflows".into()))?;
            let destination_y = u32::try_from(i64::from(output.geometry.top()) - i64::from(min_y))
                .map_err(|_| {
                    VshotError::UnsupportedOutput("output y offset is out of range".into())
                })?
                .checked_mul(scale)
                .ok_or_else(|| VshotError::UnsupportedOutput("output y offset overflows".into()))?;
            let target_size = Size::new(
                output
                    .geometry
                    .size
                    .width
                    .checked_mul(scale)
                    .ok_or_else(|| {
                        VshotError::UnsupportedOutput("output target width overflows".into())
                    })?,
                output
                    .geometry
                    .size
                    .height
                    .checked_mul(scale)
                    .ok_or_else(|| {
                        VshotError::UnsupportedOutput("output target height overflows".into())
                    })?,
            );
            let frame = if output.scale == scale {
                output.frame.clone()
            } else {
                output.frame.resize_nearest(target_size)?
            };
            copy_frame(
                &frame,
                &mut pixels,
                pixel_width as usize,
                destination_x as usize,
                destination_y as usize,
            )?;
        }

        let frame = Frame::new(Size::new(pixel_width, pixel_height), pixels)?;
        Ok(Self {
            bounds: Rect::new(min_x, min_y, width, height),
            scale,
            frame,
            outputs,
        })
    }

    pub const fn bounds(&self) -> Rect {
        self.bounds
    }

    pub const fn scale(&self) -> u32 {
        self.scale
    }

    pub fn frame(&self) -> &Frame {
        &self.frame
    }

    pub fn outputs(&self) -> &[OutputSnapshot] {
        &self.outputs
    }

    pub fn output(&self, global_id: u32) -> Option<&OutputSnapshot> {
        self.outputs
            .iter()
            .find(|output| output.global_id == global_id)
    }

    pub fn output_by_name(&self, name: &str) -> Option<&OutputSnapshot> {
        self.outputs.iter().find(|output| output.name == name)
    }

    pub fn crop(&self, requested: Rect) -> Result<Frame> {
        if requested.right()? > self.bounds.right()?
            || requested.bottom()? > self.bounds.bottom()?
            || requested.left() < self.bounds.left()
            || requested.top() < self.bounds.top()
        {
            return Err(VshotError::InvalidGeometry(format!(
                "geometry {requested} is outside frozen scene {}",
                self.bounds
            )));
        }
        let x = i64::from(requested.left()) - i64::from(self.bounds.left());
        let y = i64::from(requested.top()) - i64::from(self.bounds.top());
        let pixel_rect = Rect::new(
            i32::try_from(
                x.checked_mul(i64::from(self.scale))
                    .ok_or_else(|| VshotError::InvalidGeometry("crop x overflows".into()))?,
            )
            .map_err(|_| VshotError::InvalidGeometry("crop x is out of range".into()))?,
            i32::try_from(
                y.checked_mul(i64::from(self.scale))
                    .ok_or_else(|| VshotError::InvalidGeometry("crop y overflows".into()))?,
            )
            .map_err(|_| VshotError::InvalidGeometry("crop y is out of range".into()))?,
            requested
                .size
                .width
                .checked_mul(self.scale)
                .ok_or_else(|| VshotError::InvalidGeometry("crop width overflows".into()))?,
            requested
                .size
                .height
                .checked_mul(self.scale)
                .ok_or_else(|| VshotError::InvalidGeometry("crop height overflows".into()))?,
        );
        self.frame.crop(pixel_rect)
    }

    pub fn crop_output(&self, global_id: u32) -> Result<Frame> {
        let output = self
            .output(global_id)
            .ok_or_else(|| VshotError::InvalidGeometry(format!("unknown output id {global_id}")))?;
        Ok(output.frame.clone())
    }

    /// Crops `geometry` out of the frame of the single output that contains it
    /// whole, at that output's own scale.  `Ok(None)` when no one output covers
    /// it, which is when the composed scene is the only source of pixels.
    ///
    /// A window capture takes this path whenever it can: the scene stretches
    /// every output to the highest scale in the layout, so a window on a
    /// scale-1 output next to a scale-2 one has been nearest-upscaled by the
    /// composition, and cropping it there yields a blurred picture at double
    /// the size of the pixels that actually exist.
    pub fn crop_output_region(&self, geometry: Rect) -> Result<Option<(Frame, u32)>> {
        let Some(output) = self.outputs.iter().find(|output| {
            geometry
                .clamp_to(output.geometry)
                .is_some_and(|clamped| clamped == geometry)
        }) else {
            return Ok(None);
        };
        let scale = i32::try_from(output.scale)
            .map_err(|_| VshotError::InvalidGeometry("output scale is out of range".into()))?;
        let pixel = Rect::new(
            (geometry.left() - output.geometry.left()) * scale,
            (geometry.top() - output.geometry.top()) * scale,
            u32::try_from(geometry.size.width as i64 * i64::from(scale))
                .map_err(|_| VshotError::InvalidGeometry("crop width overflows".into()))?,
            u32::try_from(geometry.size.height as i64 * i64::from(scale))
                .map_err(|_| VshotError::InvalidGeometry("crop height overflows".into()))?,
        );
        Ok(Some((output.frame.crop(pixel)?, output.scale)))
    }
}

fn copy_frame(
    source: &Frame,
    destination: &mut [u8],
    destination_width: usize,
    x: usize,
    y: usize,
) -> Result<()> {
    let source_width = source.size().width as usize;
    let source_height = source.size().height as usize;
    let destination_row_bytes = destination_width
        .checked_mul(4)
        .ok_or_else(|| VshotError::UnsupportedOutput("scene row is too large".into()))?;
    let destination_height = destination.len() / destination_row_bytes;
    if x.checked_add(source_width)
        .is_none_or(|right| right > destination_width)
        || y.checked_add(source_height)
            .is_none_or(|bottom| bottom > destination_height)
    {
        return Err(VshotError::UnsupportedOutput(
            "output frame falls outside the scene".into(),
        ));
    }
    let source_row_bytes = source_width
        .checked_mul(4)
        .ok_or_else(|| VshotError::UnsupportedOutput("output row is too large".into()))?;
    let x_bytes = x
        .checked_mul(4)
        .ok_or_else(|| VshotError::UnsupportedOutput("output offset is too large".into()))?;
    for row in 0..source_height {
        let source_start = row
            .checked_mul(source_row_bytes)
            .ok_or_else(|| VshotError::UnsupportedOutput("source offset is too large".into()))?;
        let destination_start = y
            .checked_add(row)
            .and_then(|row| row.checked_mul(destination_row_bytes))
            .and_then(|row_start| row_start.checked_add(x_bytes))
            .ok_or_else(|| {
                VshotError::UnsupportedOutput("destination offset is too large".into())
            })?;
        let source_end = source_start + source_row_bytes;
        let destination_end = destination_start + source_row_bytes;
        destination[destination_start..destination_end]
            .copy_from_slice(&source.pixels()[source_start..source_end]);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::geometry::Point;

    fn output(id: u32, name: &str, geometry: Rect, rgba: [u8; 4]) -> OutputSnapshot {
        OutputSnapshot::new(
            id,
            name,
            geometry,
            1,
            Frame::solid(geometry.size, rgba).unwrap(),
        )
        .unwrap()
    }

    #[test]
    fn output_region_crop_keeps_the_outputs_own_pixels() {
        // Mixed scales: the scene composes at 2, so a crop taken from it would
        // be a double-size nearest upscale.  A rect that lies inside one output
        // comes out of that output's frame instead, at that output's scale.
        let a = output(1, "a", Rect::new(0, 0, 20, 10), [255, 0, 0, 255]);
        let b = OutputSnapshot::new(
            2,
            "b",
            Rect::new(20, 0, 20, 10),
            2,
            Frame::solid(Size::new(40, 20), [0, 255, 0, 255]).unwrap(),
        )
        .unwrap();
        let scene = SceneSnapshot::from_outputs(vec![a, b]).unwrap();

        // Scale 1: the pixels are exactly as large as the logical rect.
        let (frame, scale) = scene
            .crop_output_region(Rect::new(2, 2, 6, 4))
            .unwrap()
            .expect("output a holds the rect");
        assert_eq!((frame.size(), scale), (Size::new(6, 4), 1));
        // Scale 2: the same logical rect is twice as many device pixels.
        let (frame, scale) = scene
            .crop_output_region(Rect::new(22, 2, 6, 4))
            .unwrap()
            .expect("output b holds the rect");
        assert_eq!((frame.size(), scale), (Size::new(12, 8), 2));
        // Across the seam there is only the composed scene.
        assert!(scene
            .crop_output_region(Rect::new(18, 2, 6, 4))
            .unwrap()
            .is_none());
    }

    #[test]
    fn composes_negative_origins_and_gaps() {
        let scene = SceneSnapshot::from_outputs(vec![
            output(1, "left", Rect::new(-4, 0, 2, 2), [255, 0, 0, 255]),
            output(2, "right", Rect::new(1, 1, 2, 1), [0, 255, 0, 255]),
        ])
        .unwrap();
        assert_eq!(scene.bounds(), Rect::new(-4, 0, 7, 2));
        assert_eq!(scene.frame().size(), Size::new(7, 2));
        assert_eq!(
            scene.frame().pixel(Point::new(0, 0)),
            Some([255, 0, 0, 255])
        );
        assert_eq!(
            scene.frame().pixel(Point::new(5, 1)),
            Some([0, 255, 0, 255])
        );
        assert_eq!(scene.frame().pixel(Point::new(2, 0)), Some([0, 0, 0, 0]));
    }

    #[test]
    fn composes_mixed_scales_at_highest_scale() {
        let a = output(1, "a", Rect::new(0, 0, 2, 2), [255, 0, 0, 255]);
        let b = OutputSnapshot::new(
            2,
            "b",
            Rect::new(2, 0, 2, 2),
            2,
            Frame::solid(Size::new(4, 4), [0, 255, 0, 255]).unwrap(),
        )
        .unwrap();
        let scene = SceneSnapshot::from_outputs(vec![a, b]).unwrap();
        assert_eq!(scene.scale(), 2);
        assert_eq!(scene.frame().size(), Size::new(8, 4));
        assert_eq!(
            scene.frame().pixel(Point::new(0, 0)),
            Some([255, 0, 0, 255])
        );
        assert_eq!(
            scene.frame().pixel(Point::new(3, 3)),
            Some([255, 0, 0, 255])
        );
        assert_eq!(
            scene.frame().pixel(Point::new(4, 0)),
            Some([0, 255, 0, 255])
        );
    }
}

#![allow(dead_code)]

use super::frame::Frame;
use crate::edit::{ArrowStyle, LineDash, TextBitmap};
use crate::error::Result;
use crate::geometry::{Point, Rect};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ImageDocument {
    frame: Frame,
}

impl ImageDocument {
    pub fn new(frame: Frame) -> Self {
        Self { frame }
    }

    pub fn frame(&self) -> &Frame {
        &self.frame
    }

    pub fn into_frame(self) -> Frame {
        self.frame
    }

    pub fn crop(&self, rect: Rect) -> Result<Self> {
        Ok(Self::new(self.frame.crop(rect)?))
    }

    pub(crate) fn stroke_rectangle(
        &mut self,
        rect: Rect,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    ) -> Result<()> {
        self.frame.stroke_rectangle(rect, color, width, dash)
    }

    pub(crate) fn stroke_circle(
        &mut self,
        center: Point,
        radius: u32,
        color: [u8; 4],
        width: u32,
    ) -> Result<()> {
        self.frame.stroke_circle(center, radius, color, width)
    }

    pub(crate) fn stroke_ellipse(
        &mut self,
        rect: Rect,
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    ) -> Result<()> {
        self.frame.stroke_ellipse(rect, color, width, dash)
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
        self.frame
            .draw_arrow_with_style(start, end, color, width, dash, head, style)
    }

    pub(crate) fn draw_freehand(
        &mut self,
        points: &[Point],
        color: [u8; 4],
        width: u32,
        dash: LineDash,
    ) -> Result<()> {
        self.frame.draw_freehand(points, color, width, dash)
    }

    pub(crate) fn draw_text(
        &mut self,
        origin: Point,
        text: &str,
        color: [u8; 4],
        scale: u32,
    ) -> Result<()> {
        self.frame.draw_text(origin, text, color, scale)
    }

    pub(crate) fn draw_bitmap(&mut self, origin: Point, bitmap: &TextBitmap) -> Result<()> {
        self.frame.draw_bitmap(origin, bitmap)
    }

    pub(crate) fn draw_bitmap_scaled(&mut self, rect: Rect, bitmap: &TextBitmap) -> Result<()> {
        self.frame.draw_bitmap_scaled(rect, bitmap)
    }

    pub(crate) fn mosaic(&mut self, rect: Rect, block_size: u32) -> Result<()> {
        self.frame.mosaic(rect, block_size)
    }

    pub(crate) fn mosaic_ellipse(&mut self, rect: Rect, block_size: u32) -> Result<()> {
        self.frame.mosaic_ellipse(rect, block_size)
    }

    pub(crate) fn mosaic_brush(&mut self, points: &[Point], radius: u32) -> Result<()> {
        self.frame.mosaic_brush(points, radius)
    }
}

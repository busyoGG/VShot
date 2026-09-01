#![allow(dead_code)]

use crate::error::Result;
use crate::geometry::{Point, Rect};
use crate::model::{Frame, ImageDocument};

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum EditOperation {
    Crop(Rect),
    RectangleStroke {
        rect: Rect,
        color: [u8; 4],
        width: u32,
    },
    CircleStroke {
        center: Point,
        radius: u32,
        color: [u8; 4],
        width: u32,
    },
    Arrow {
        start: Point,
        end: Point,
        color: [u8; 4],
        width: u32,
    },
    Freehand {
        points: Vec<Point>,
        color: [u8; 4],
        width: u32,
    },
    Text {
        origin: Point,
        text: String,
        color: [u8; 4],
        scale: u32,
    },
    Mosaic {
        rect: Rect,
        block_size: u32,
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

    pub(crate) fn rectangle_stroke(mut self, rect: Rect, color: [u8; 4], width: u32) -> Self {
        self.operations
            .push(EditOperation::RectangleStroke { rect, color, width });
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

    pub(crate) fn arrow(mut self, start: Point, end: Point, color: [u8; 4], width: u32) -> Self {
        self.operations.push(EditOperation::Arrow {
            start,
            end,
            color,
            width,
        });
        self
    }

    pub(crate) fn freehand(mut self, points: Vec<Point>, color: [u8; 4], width: u32) -> Self {
        self.operations.push(EditOperation::Freehand {
            points,
            color,
            width,
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

    pub(crate) fn mosaic(mut self, rect: Rect, block_size: u32) -> Self {
        self.operations
            .push(EditOperation::Mosaic { rect, block_size });
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
                    EditOperation::RectangleStroke { rect, color, width } => {
                        document.stroke_rectangle(*rect, *color, *width)?
                    }
                    EditOperation::CircleStroke {
                        center,
                        radius,
                        color,
                        width,
                    } => document.stroke_circle(*center, *radius, *color, *width)?,
                    EditOperation::Arrow {
                        start,
                        end,
                        color,
                        width,
                    } => document.draw_arrow(*start, *end, *color, *width)?,
                    EditOperation::Freehand {
                        points,
                        color,
                        width,
                    } => document.draw_freehand(points, *color, *width)?,
                    EditOperation::Text {
                        origin,
                        text,
                        color,
                        scale,
                    } => document.draw_text(*origin, text, *color, *scale)?,
                    EditOperation::Mosaic { rect, block_size } => {
                        document.mosaic(*rect, *block_size)?
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
    fn initial_crop_operation_is_executable() {
        let frame = Frame::solid(Size::new(4, 4), [1, 2, 3, 255]).unwrap();
        let document = crop_operation(&frame, Rect::new(1, 1, 2, 2)).unwrap();
        assert_eq!(document.frame().size(), Size::new(2, 2));
    }
}

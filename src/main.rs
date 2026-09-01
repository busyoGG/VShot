mod capture;
mod cli;
mod edit;
mod error;
mod geometry;
mod model;
mod output;
mod selection;
mod wayland;

use std::process::ExitCode;

use crate::geometry::{Point, Rect};

use clap::Parser;

use capture::{CompositorWindowProvider, ProcessWindowProvider, WlrCapture};
use cli::{CaptureTarget, Cli};
use edit::EditPipeline;
use error::{Result, VshotError};
use model::{ImageDocument, OutputSnapshot, SceneSnapshot};
use wayland::input::{Annotation, EditorTool};
use wayland::topology::OutputInfo;
use wayland::WaylandSession;

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("vshot: {error}");
            ExitCode::from(1)
        }
    }
}

fn run() -> Result<()> {
    let request = Cli::parse().parse_request()?;
    let mut wayland = WaylandSession::connect()?;
    let output_infos = wayland.output_infos()?;
    let mut capture = WlrCapture::connect()?;

    let active_window = if matches!(request.target, CaptureTarget::ActiveWindow) {
        Some(ProcessWindowProvider.active_window()?)
    } else {
        None
    };

    let scene = capture_scene(&mut capture, &output_infos, request.cursor)?;
    let active_window_frame = active_window
        .as_ref()
        .map(|window| {
            selection::validate_selection(&scene, window.geometry)?;
            scene.crop(window.geometry)
        })
        .transpose()?;
    wayland.set_scene(scene.clone());

    let mut edits = EditPipeline::new();
    let frame = match &request.target {
        CaptureTarget::RegionFixed(geometry) => {
            let frame = selection::crop_fixed(&scene, *geometry)?;
            wayland.show_frozen(false)?;
            frame
        }
        CaptureTarget::RegionInteractive => {
            wayland.show_frozen(true)?;
            let geometry = wayland.select_region()?;
            let geometry = selection::validate_selection(&scene, geometry)?;
            wayland.start_editor(geometry)?;
            let (geometry, annotations) = wayland.edit_region()?;
            let frame = scene.crop(geometry)?;
            edits = pipeline_for_annotations(annotations, geometry, scene.scale())?;
            frame
        }
        CaptureTarget::Monitor(name) if name == "current" => {
            wayland.show_frozen(false)?;
            let output_id = wayland.wait_for_current_output()?;
            selection::crop_output(&scene, output_id)?
        }
        CaptureTarget::Monitor(name) => {
            let frame = selection::crop_monitor(&scene, name)?;
            wayland.show_frozen(false)?;
            frame
        }
        CaptureTarget::All => {
            wayland.show_frozen(false)?;
            scene.frame().clone()
        }
        CaptureTarget::ActiveWindow => {
            wayland.show_frozen(false)?;
            active_window_frame.ok_or_else(|| {
                VshotError::ActiveWindowUnavailable("active window capture was not produced".into())
            })?
        }
    };

    let document = edits.apply(ImageDocument::new(frame))?;
    let result = output::write_frame(document.frame(), &request.destination);
    let cleanup = wayland.destroy_overlays();
    result.and(cleanup)
}

fn pipeline_for_annotations(
    annotations: Vec<Annotation>,
    selection: Rect,
    scale: u32,
) -> Result<EditPipeline> {
    let mut pipeline = EditPipeline::new();
    for annotation in annotations {
        match annotation {
            Annotation::Shape { tool, rect } => {
                let rect = local_rect(rect, selection, scale)?;
                match tool {
                    EditorTool::Rectangle => {
                        pipeline =
                            pipeline.rectangle_stroke(rect, [255, 64, 64, 255], scale.clamp(1, 4));
                    }
                    EditorTool::Ellipse => {
                        let radius = rect.size.width.min(rect.size.height) / 2;
                        if radius > 0 {
                            let center = Point::new(
                                rect.left().saturating_add((rect.size.width / 2) as i32),
                                rect.top().saturating_add((rect.size.height / 2) as i32),
                            );
                            pipeline = pipeline.circle_stroke(
                                center,
                                radius,
                                [255, 64, 64, 255],
                                scale.clamp(1, 4),
                            );
                        }
                    }
                    _ => {}
                }
            }
            Annotation::Stroke { tool, points } => {
                let points = points
                    .into_iter()
                    .map(|point| local_point(point, selection, scale))
                    .collect::<Result<Vec<_>>>()?;
                let width = scale.clamp(1, 4);
                match tool {
                    EditorTool::Arrow if points.len() >= 2 => {
                        pipeline = pipeline.arrow(
                            points[0],
                            *points.last().ok_or_else(|| {
                                VshotError::Selection("arrow has no endpoint".into())
                            })?,
                            [255, 64, 64, 255],
                            width,
                        );
                    }
                    EditorTool::Pen | EditorTool::Draw | EditorTool::Line => {
                        pipeline = pipeline.freehand(points, [255, 64, 64, 255], width);
                    }
                    EditorTool::Mosaic | EditorTool::Blur => {
                        let bounds = points_bounds(&points)?;
                        pipeline = pipeline.mosaic(bounds, (12_u32).saturating_mul(scale).max(1));
                    }
                    _ => {}
                }
            }
            Annotation::Text {
                origin,
                text,
                scale: text_scale,
            } => {
                pipeline = pipeline.text(
                    local_point(origin, selection, scale)?,
                    text,
                    [255, 255, 255, 255],
                    text_scale.saturating_mul(scale).max(1),
                );
            }
        }
    }
    Ok(pipeline)
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

fn points_bounds(points: &[Point]) -> Result<Rect> {
    let first = *points
        .first()
        .ok_or_else(|| VshotError::Selection("drawing gesture has no points".into()))?;
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
    let right = max_x
        .checked_add(1)
        .ok_or_else(|| VshotError::InvalidGeometry("annotation right edge overflows".into()))?;
    let bottom = max_y
        .checked_add(1)
        .ok_or_else(|| VshotError::InvalidGeometry("annotation bottom edge overflows".into()))?;
    Ok(Rect::new(
        min_x,
        min_y,
        (right - min_x) as u32,
        (bottom - min_y) as u32,
    ))
}

fn capture_scene(
    capture: &mut WlrCapture,
    output_infos: &[OutputInfo],
    cursor: bool,
) -> Result<SceneSnapshot> {
    if output_infos.is_empty() {
        return Err(VshotError::IncompleteTopology(
            "no outputs are available to capture".into(),
        ));
    }
    let mut outputs = Vec::with_capacity(output_infos.len());
    for info in output_infos {
        let frame = capture.capture_output(&info.name, cursor)?;
        outputs.push(OutputSnapshot::new(
            info.global_id,
            info.name.clone(),
            info.geometry,
            info.scale,
            frame,
        )?);
    }
    SceneSnapshot::from_outputs(outputs)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::geometry::Size;
    use crate::model::Frame;
    use crate::wayland::input::Annotation;

    #[test]
    fn annotation_pipeline_maps_global_points_into_cropped_pixels() {
        let pipeline = pipeline_for_annotations(
            vec![Annotation::stroke(
                EditorTool::Pen,
                vec![Point::new(12, 22), Point::new(14, 24)],
            )],
            Rect::new(10, 20, 20, 20),
            2,
        )
        .unwrap();
        let frame = Frame::solid(Size::new(40, 40), [0, 0, 0, 0]).unwrap();
        let document = pipeline.apply(ImageDocument::new(frame)).unwrap();
        assert_eq!(
            document.frame().pixel(Point::new(4, 4)),
            Some([255, 64, 64, 255])
        );
    }

    #[test]
    fn annotation_pipeline_renders_text_and_mosaic() {
        let pipeline = pipeline_for_annotations(
            vec![
                Annotation::text(Point::new(0, 0), "A", 1),
                Annotation::stroke(EditorTool::Mosaic, vec![Point::new(2, 2), Point::new(5, 5)]),
            ],
            Rect::new(0, 0, 10, 10),
            1,
        )
        .unwrap();
        let frame = Frame::solid(Size::new(10, 10), [10, 20, 30, 255]).unwrap();
        let document = pipeline.apply(ImageDocument::new(frame)).unwrap();
        assert_ne!(
            document.frame().pixel(Point::new(1, 0)),
            Some([10, 20, 30, 255])
        );
    }
}

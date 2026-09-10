mod capture;
mod cli;
mod edit;
mod error;
mod geometry;
mod model;
mod output;
mod pin;
mod qt_overlay;
mod selection;
mod wayland;

use std::process::ExitCode;

use clap::Parser;

use capture::{CompositorWindowProvider, ProcessWindowProvider, WlrCapture};
use cli::{Action, CaptureTarget, Cli};
use edit::{pipeline_for_annotations, EditPipeline};
use error::{Result, VshotError};
use model::{ImageDocument, OutputSnapshot, SceneSnapshot};
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
    let cli = Cli::parse();
    let action = cli.parse_action()?;
    let request = match action {
        Action::Pin(invocation) => {
            // Pin management talks to the daemon only; no capture, no scene.
            return pin::run(invocation);
        }
        Action::PinApply(session) => {
            // Internal: render a pin-edit session, no Wayland capture needed.
            return pin::apply_edit(&session);
        }
        Action::Capture(request) => request,
    };
    let mut wayland = WaylandSession::connect()?;
    let output_infos = wayland.output_infos()?;
    let mut capture = WlrCapture::connect()?;

    // Metadata lookup is cheap and happens before the capture; a failure is
    // not fatal yet — the pixel fallback runs on the captured scene.
    let pixel_detect = matches!(
        request.target,
        CaptureTarget::ActiveWindow { pixel_detect: true }
    );
    let mut metadata_error = None;
    let metadata_window =
        if matches!(request.target, CaptureTarget::ActiveWindow { .. }) && !pixel_detect {
            match ProcessWindowProvider.active_window() {
                Ok(window) => Some(window),
                Err(error) => {
                    metadata_error = Some(error);
                    None
                }
            }
        } else {
            None
        };

    let scene = capture_scene(&mut capture, &output_infos, request.cursor)?;

    if !matches!(request.target, CaptureTarget::RegionInteractive) {
        wayland.set_scene(scene.clone());
    }

    let mut edits = EditPipeline::new();
    let frame = match &request.target {
        CaptureTarget::RegionFixed(geometry) => {
            let frame = selection::crop_fixed(&scene, *geometry)?;
            wayland.show_frozen(false)?;
            frame
        }
        CaptureTarget::RegionInteractive => {
            let (geometry, annotations) = qt_overlay::select_and_edit(&scene)?;
            let geometry = selection::validate_selection(&scene, geometry)?;
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
        CaptureTarget::ActiveWindow { .. } => {
            let geometry = match metadata_window {
                Some(window) => {
                    wayland.show_frozen(false)?;
                    window.geometry
                }
                None => {
                    // The overlay surfaces must be mapped before the
                    // compositor routes pointer events to this client, so
                    // show the frozen scene before reading the pointer.
                    wayland.show_frozen(false)?;
                    let cursor = wayland.pointer_position().unwrap_or(None);
                    match capture::detect_active_window(&scene, cursor) {
                        Ok(window) => window.geometry,
                        Err(pixel_error) => {
                            return Err(match metadata_error {
                                Some(metadata_error) => VshotError::ActiveWindowUnavailable(
                                    format!("{metadata_error}; pixel fallback also failed: {pixel_error}"),
                                ),
                                None => pixel_error,
                            });
                        }
                    }
                }
            };
            let geometry = selection::validate_selection(&scene, geometry)?;
            wayland.show_frozen(false)?;
            scene.crop(geometry)?
        }
    };

    let document = edits.apply(ImageDocument::new(frame))?;
    let result = output::write_frame(document.frame(), &request.destination);
    let cleanup = wayland.destroy_overlays();
    result.and(cleanup)
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
    use crate::edit::{ArrowStyle, LineDash};
    use crate::geometry::{Point, Rect, Size};
    use crate::model::Frame;
    use crate::wayland::input::{Annotation, EditorTool};

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
    fn annotation_pipeline_renders_text_and_mosaic_brush() {
        let mut pixels = vec![0u8; 400];
        for chunk in pixels.chunks_exact_mut(4) {
            chunk.copy_from_slice(&[0, 0, 0, 255]);
        }
        // A single bright pixel inside the stamp disc.
        pixels[(3 * 10 + 3) * 4] = 255;
        let frame = Frame::new(Size::new(10, 10), pixels).unwrap();
        let pipeline = pipeline_for_annotations(
            vec![
                Annotation::text(Point::new(8, 8), "A", 1),
                Annotation::Stroke {
                    tool: EditorTool::Mosaic,
                    points: vec![Point::new(2, 2), Point::new(5, 5)],
                    color: [255, 64, 64, 255],
                    width: 8,
                    dash: LineDash::Solid,
                    head: 1,
                    arrow_style: ArrowStyle::Open,
                    strength: 2,
                },
            ],
            Rect::new(0, 0, 10, 10),
            1,
        )
        .unwrap();
        let document = pipeline.apply(ImageDocument::new(frame)).unwrap();
        // Brush radius = width/2 = 4.  The stamp walk emits centers
        // (2,2), (3,3), (4,4), (5,5); (1, 0) is covered by the (2,2) and
        // (3,3) discs and the later one wins, spreading the brightness of
        // the pixel at (3, 3) as a 5/47-ish local average.
        assert_eq!(
            document.frame().pixel(Point::new(1, 0)),
            Some([5, 0, 0, 255])
        );
    }
}

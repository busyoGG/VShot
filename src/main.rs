mod capture;
mod cli;
mod edit;
mod error;
mod geometry;
mod inject;
mod longshot;
mod model;
mod output;
mod pin;
mod qt_overlay;
mod selection;
mod stitch;
mod wayland;

use std::process::ExitCode;
use std::time::Duration;

use clap::Parser;

use capture::{Capturer, CompositorWindowProvider, ProcessWindowProvider, WindowCandidate};
use cli::{Action, CaptureTarget, Cli};
use edit::{pipeline_for_annotations, EditPipeline};
use error::{Result, VshotError};
use geometry::Rect;
use model::{Frame, ImageDocument, OutputSnapshot, SceneSnapshot};
use wayland::topology::OutputInfo;
use wayland::WaylandSession;

/// How long `window pick` waits for the picker's layer surfaces to leave the
/// screen before capturing the frame the user is looking at.  The helper hides
/// its surfaces before it exits, so this only covers the compositor processing
/// that unmap — measured at well under a frame, with an order of magnitude of
/// slack for a busy session.  It is invisible: the editing overlay follows.
const PICK_SETTLE: Duration = Duration::from_millis(200);

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("vshot: {error}");
            // A compositor failure is only readable next to the display it came
            // from: with several compositors on one runtime directory, "no such
            // capability" may well describe a display the user is not looking
            // at (see `error::wayland_display_note`).
            if error.is_display_failure() {
                if let Some(note) = error::wayland_display_note() {
                    eprintln!("vshot: {note}");
                }
            }
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
    let mut capture = Capturer::connect()?;

    // The focused window's own screenshot, when the compositor offers one —
    // KWin does.  That is the exact answer: the compositor draws the window
    // itself, decorations included, at the density of the screen it lives on.
    // Every route below exists to reconstruct that from something else, so this
    // one is asked first and the rest is only reached when it cannot answer.
    let pixel_detect = matches!(
        request.target,
        CaptureTarget::ActiveWindow { pixel_detect: true }
    );
    let (native_window, mut unavailable) = match &request.target {
        CaptureTarget::ActiveWindow { .. } if !pixel_detect => {
            match capture.capture_active_window(request.cursor) {
                Some(Ok(window)) => (Some(window), None),
                Some(Err(error)) => (None, Some(error)),
                None => (None, None),
            }
        }
        _ => (None, None),
    };

    // Metadata lookup is cheap and happens before the capture; a failure is
    // not fatal yet — the pixel fallback runs on the captured scene, and the
    // reason only matters if that fails too.
    let metadata_window = if native_window.is_none()
        && matches!(request.target, CaptureTarget::ActiveWindow { .. })
        && !pixel_detect
    {
        match ProcessWindowProvider.active_window() {
            Ok(window) => Some(window),
            Err(error) => {
                // Keep the first reason: why the exact route failed says more
                // than why one of its substitutes did.
                unavailable.get_or_insert(error);
                None
            }
        }
    } else {
        None
    };

    let scene = capture_scene(&mut capture, &output_infos, request.cursor)?;

    if !matches!(
        request.target,
        CaptureTarget::RegionInteractive
            | CaptureTarget::WindowPick { .. }
            | CaptureTarget::LongShot { .. }
    ) {
        wayland.set_scene(scene.clone());
    }

    let mut edits = EditPipeline::new();
    // Device pixels per logical pixel of the frame, carried next to it: the
    // scene composes every output at the highest scale, but a monitor capture
    // keeps that output's own pixels, so the density follows the source.
    let (frame, density) = match &request.target {
        CaptureTarget::RegionFixed(geometry) => {
            let frame = selection::crop_fixed(&scene, *geometry)?;
            wayland.show_frozen(false)?;
            (frame, scene.scale())
        }
        CaptureTarget::RegionInteractive => {
            let (geometry, annotations) = qt_overlay::select_and_edit(&scene)?;
            let geometry = selection::validate_selection(&scene, geometry)?;
            let frame = scene.crop(geometry)?;
            edits = pipeline_for_annotations(annotations, geometry, scene.scale())?;
            (frame, scene.scale())
        }
        CaptureTarget::Monitor(name) if name == "current" => {
            wayland.show_frozen(false)?;
            let output_id = wayland.wait_for_current_output()?;
            let density = scene
                .output(output_id)
                .map_or(scene.scale(), |output| output.scale);
            (selection::crop_output(&scene, output_id)?, density)
        }
        CaptureTarget::Monitor(name) => {
            let frame = selection::crop_monitor(&scene, name)?;
            wayland.show_frozen(false)?;
            let density = scene
                .output_by_name(name)
                .map_or(scene.scale(), |output| output.scale);
            (frame, density)
        }
        CaptureTarget::All => {
            wayland.show_frozen(false)?;
            (scene.frame().clone(), scene.scale())
        }
        CaptureTarget::ActiveWindow { .. } => {
            if let Some((frame, density)) = native_window {
                (frame, density)
            } else if let Some(window) = metadata_window {
                wayland.show_frozen(false)?;
                let geometry = selection::validate_selection(&scene, window.geometry)?;
                // The window's own pixels at its own output's density — cropping
                // the composed scene would hand back a nearest-upscale of a
                // window that sits on a lower-density monitor.
                crop_window(&scene, geometry)?
            } else {
                // Neither, so the window has to be found in the frozen frame.
                // The overlay surfaces must be mapped before the compositor
                // routes pointer events to this client, so show the frozen
                // scene before reading the pointer.
                wayland.show_frozen(false)?;
                let cursor = wayland.pointer_position().unwrap_or(None);
                let geometry = match capture::detect_active_window(&scene, cursor) {
                    Ok(window) => window.geometry,
                    Err(pixel_error) => {
                        return Err(match unavailable {
                            Some(reason) => VshotError::ActiveWindowUnavailable(format!(
                                "{reason}; the pixel fallback also failed: {pixel_error}"
                            )),
                            None => pixel_error,
                        });
                    }
                };
                let geometry = selection::validate_selection(&scene, geometry)?;
                crop_window(&scene, geometry)?
            }
        }
        CaptureTarget::WindowPick { pixel_detect } => {
            // Compose the candidate set first: the compositor's window list
            // when it has one, the frozen frame's own signals otherwise.  The
            // picking overlay needs no pointer position up front — it takes
            // the candidates and asks the user.
            let mut metadata_error = None;
            let mut candidates = Vec::new();
            if !pixel_detect {
                match ProcessWindowProvider.windows() {
                    Ok(windows) => candidates = windows,
                    Err(error) => metadata_error = Some(error),
                }
            }
            if candidates.is_empty() {
                candidates = capture::detect_window_candidates(&scene)
                    .into_iter()
                    .map(|geometry| WindowCandidate {
                        geometry,
                        label: String::new(),
                    })
                    .collect();
            }
            if candidates.is_empty() {
                return Err(match metadata_error {
                    Some(metadata_error) => VshotError::WindowPickUnavailable(format!(
                        "{metadata_error}; the pixel fallback found no window either"
                    )),
                    None => VshotError::WindowPickUnavailable(
                        "the pixel fallback found no window in the frozen frame \
                         (seamless borderless tiling and uniform desktops carry no \
                         pixel signal)"
                            .into(),
                    ),
                });
            }
            let picked = qt_overlay::pick_window(&scene, &candidates, || {
                // The picker re-lists the windows as the pointer travels:
                // picking runs on a live desktop, and a workspace switch or a
                // moved window would otherwise leave the highlight pointing at
                // where a window used to be.  The pixel fallback has no window
                // list to re-read, so it keeps what it started with.
                if *pixel_detect {
                    return None;
                }
                ProcessWindowProvider.windows().ok()
            })?;
            // Picking runs on the live desktop and only decides *what* to
            // capture, so the pixels have to come from now: wait for the
            // compositor to drop the picker's surfaces (they are hidden, but
            // the request travels), capture again, and resolve the click
            // against the windows that exist at this point in time.
            std::thread::sleep(PICK_SETTLE);
            let scene = capture_scene(&mut capture, &output_infos, request.cursor)?;
            let geometry = picked
                .point
                .and_then(|point| ProcessWindowProvider.window_at(point))
                .unwrap_or(picked.rect);
            let (geometry, annotations) = qt_overlay::edit_selection(&scene, geometry)?;
            let geometry = selection::validate_selection(&scene, geometry)?;
            let (frame, density) = crop_window(&scene, geometry)?;
            edits = pipeline_for_annotations(annotations, geometry, density)?;
            (frame, density)
        }
        CaptureTarget::LongShot {
            region,
            options,
            inject,
        } => {
            // The region to scroll: the one asked for, or one picked off the
            // frozen desktop.  Either way it has to sit inside a single output
            // — a scroll container never spans two monitors.
            let region = match region {
                Some(region) => *region,
                None => qt_overlay::select_region(&scene)?,
            };
            let region = selection::validate_selection(&scene, region)?;
            let desktop = longshot::desktop_bounds(&output_infos)?;
            let mut injector = inject::Injector::open(desktop, *inject)?;
            let result =
                longshot::run(&mut capture, &output_infos, region, &mut injector, options)?;
            eprintln!(
                "vshot: stitched {} frames into {}x{} pixels ({}, wheel through {})",
                result.frames,
                result.frame.size().width,
                result.frame.size().height,
                longshot::stop_reason(result.stop),
                injector.backend_name()
            );
            if result.appended == 0 {
                // Not one scroll moved anything: the picture is a single frame.
                // Say which backend carried the wheel and where it was aimed,
                // because the causes look identical otherwise — a region that
                // cannot scroll (a panel, a bar, a window with nothing to
                // scroll), a pointer that never was over the region (only the
                // compositor protocol moves it), and a window that ignores
                // synthetic wheel events.
                eprintln!(
                    "vshot: nothing scrolled, so the result is a single frame of {}x{}: the \
wheel went through {} aimed at the region centre ({}, {}) on {}, and a region that has \
nothing to scroll, a pointer that is outside it, and a window that ignores synthetic wheel \
events all look the same from here",
                    result.frame.size().width,
                    result.frame.size().height,
                    injector.backend_name(),
                    region.origin.x + (region.size.width / 2) as i32,
                    region.origin.y + (region.size.height / 2) as i32,
                    result.output,
                );
            }
            (result.frame, result.density)
        }
    };

    let document = edits.apply(ImageDocument::new(frame))?;
    let result = output::write_frame(
        document.frame(),
        &request.destination,
        density,
        request.compression,
    );
    let cleanup = wayland.destroy_overlays();
    result.and(cleanup)
}

fn capture_scene(
    capture: &mut Capturer,
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

/// Crops a window rect at the resolution its pixels were captured at: straight
/// out of the one output that holds it whole, out of the composed scene only
/// when the window straddles a monitor seam.
fn crop_window(scene: &SceneSnapshot, geometry: Rect) -> Result<(Frame, u32)> {
    Ok(match scene.crop_output_region(geometry)? {
        Some(cropped) => cropped,
        None => (scene.crop(geometry)?, scene.scale()),
    })
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

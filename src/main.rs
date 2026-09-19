mod capture;
mod cli;
mod cli_i18n;
mod config;
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

use capture::active_output::Session;
use capture::niri;
use capture::window::{ProcessWindowRunner, WindowCommandRunner};
use capture::{Capturer, CompositorWindowProvider, ProcessWindowProvider, WindowCandidate};
use cli::{Action, CaptureTarget};
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
    let cli = cli::parse();
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
    // The topology is what the scene is composed from, so most routes need it.
    // A compositor's own window screenshot does not: it hands over the window's
    // pixels itself and never looks at an output.  A flipped or rotated output
    // is precisely what this side cannot map into a scene, so the failure is
    // held back here and only raised by a route that really needs the topology.
    let topology = wayland.output_infos();
    let known_outputs: &[OutputInfo] = topology.as_deref().unwrap_or(&[]);
    let mut capture = Capturer::connect()?;

    // niri picks the window itself and then hands over its picture, which is
    // the only way there: its IPC reports no position for a tiled window, so
    // nothing on this side could give our own picker a rectangle to highlight.
    // It therefore runs before anything is frozen or mapped — niri's crosshair
    // works on the live desktop, which is the whole point of picking.
    if let CaptureTarget::WindowPick {
        pixel_detect: false,
        no_blend,
    } = request.target
    {
        if let Some((frame, density)) = niri_picked_window(
            &mut |name, cursor| capture.capture_output(name, cursor),
            known_outputs,
            request.cursor,
            no_blend,
        )? {
            return finish_capture(EditPipeline::new(), frame, density, &request, &mut wayland);
        }
    }

    // The focused window's own screenshot, when the compositor offers one —
    // KWin does, and so does niri over its own IPC.  That is the exact answer:
    // the compositor draws the window itself, at the density of the screen it
    // lives on.  Every route below exists to reconstruct that from something
    // else, so this one is asked first and the rest is only reached when it
    // cannot answer.  niri is the reason it has to be asked twice: its IPC
    // describes window sizes but no position for a tiled window, so the
    // rectangle routes are not merely worse there, they cannot work at all.
    let pixel_detect = matches!(
        request.target,
        CaptureTarget::ActiveWindow {
            pixel_detect: true,
            ..
        }
    );
    let active_no_blend = match request.target {
        CaptureTarget::ActiveWindow { no_blend, .. } => no_blend,
        _ => false,
    };
    let (native_window, mut unavailable) = match &request.target {
        CaptureTarget::ActiveWindow { .. } if !pixel_detect => {
            match capture.capture_active_window(request.cursor) {
                Some(Ok(window)) => (Some(window), None),
                Some(Err(error)) => (None, Some(error)),
                None => match niri_active_window(
                    &mut |name, cursor| capture.capture_output(name, cursor),
                    known_outputs,
                    request.cursor,
                    active_no_blend,
                ) {
                    Ok(window) => (window, None),
                    Err(error) => (None, Some(error)),
                },
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
                // Keep the first reason: why the exact route failed says
                // more than why one of its substitutes did.
                unavailable.get_or_insert(error);
                None
            }
        }
    } else {
        None
    };

    let mut edits = EditPipeline::new();
    // Device pixels per logical pixel of the frame, carried next to it: the
    // scene composes every output at the highest scale, but a monitor capture
    // keeps that output's own pixels, so the density follows the source.
    //
    // A window the compositor drew itself is already done at this point, and
    // taking the scene apart for it would be waste — and on an output the scene
    // cannot represent (rotated or flipped) it would be a failure, in the one
    // place that does not need a scene at all.
    let (frame, density) = if let Some(window) = native_window {
        window
    } else {
        let output_infos = topology?;
        let scene = capture_scene(&mut capture, &output_infos, request.cursor)?;

        if !matches!(
            request.target,
            CaptureTarget::RegionInteractive
                | CaptureTarget::WindowPick { .. }
                | CaptureTarget::LongShot { .. }
        ) {
            wayland.set_scene(scene.clone());
        }

        match &request.target {
            CaptureTarget::RegionFixed(geometry) => {
                let (frame, density) = crop_native(&scene, *geometry)?;
                wayland.show_frozen(false)?;
                (frame, density)
            }
            CaptureTarget::RegionInteractive => {
                let (geometry, annotations) = qt_overlay::select_and_edit(&scene)?;
                let geometry = selection::validate_selection(&scene, geometry)?;
                let (frame, density) = crop_native(&scene, geometry)?;
                // The helper drew its text bitmaps at the scene's scale, which
                // is only the crop's density when the selection fell on the
                // highest-density output.
                edits = pipeline_for_annotations(annotations, geometry, density, scene.scale())?;
                (frame, density)
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
            // A window the compositor drew itself never reaches this arm — that
            // happens above, before the scene is composed.  So this is the
            // window list, and then the frozen frame.
            CaptureTarget::ActiveWindow { .. } => {
                if let Some(window) = metadata_window {
                    wayland.show_frozen(false)?;
                    let geometry = selection::validate_selection(&scene, window.geometry)?;
                    // The window's own pixels at its own output's density — cropping
                    // the composed scene would hand back a nearest-upscale of a
                    // window that sits on a lower-density monitor.
                    crop_native(&scene, geometry)?
                } else {
                    // No window list either, so the window has to be found in the
                    // frozen frame.  The overlay surfaces must be mapped before the
                    // compositor routes pointer events to this client, so show the
                    // frozen scene before reading the pointer.
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
                    crop_native(&scene, geometry)?
                }
            }
            CaptureTarget::WindowPick { pixel_detect, .. } => {
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
                let (frame, density) = crop_native(&scene, geometry)?;
                // Same reason as the interactive region above: the editor is
                // shared, so its text bitmaps are in scene device pixels.
                edits = pipeline_for_annotations(annotations, geometry, density, scene.scale())?;
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
        }
    };

    finish_capture(edits, frame, density, &request, &mut wayland)
}

/// Renders the annotations into the captured frame and writes it where the
/// request pointed.  The routes that hand over finished pixels — a compositor's
/// own window screenshot — have nothing to render and pass an empty pipeline.
fn finish_capture(
    edits: EditPipeline,
    frame: Frame,
    density: u32,
    request: &cli::Request,
    wayland: &mut WaylandSession,
) -> Result<()> {
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

/// niri's own capture of the focused window: niri names it, then draws it
/// itself.  `None` means this is not a niri session (or its IPC is not
/// answering), which leaves the caller to try the routes below.
fn niri_active_window(
    grab_output: &mut dyn FnMut(&str, bool) -> Result<Frame>,
    output_infos: &[OutputInfo],
    cursor: bool,
    no_blend: bool,
) -> Result<Option<(Frame, u32)>> {
    niri_active_window_for(
        grab_output,
        Session::detect(),
        output_infos,
        cursor,
        no_blend,
    )
}

/// The session check is a parameter so the contract — only a niri session is
/// asked, and anything else is left to the routes below — can be tested without
/// setting environment variables under a parallel test runner.
fn niri_active_window_for(
    grab_output: &mut dyn FnMut(&str, bool) -> Result<Frame>,
    session: Session,
    output_infos: &[OutputInfo],
    cursor: bool,
    no_blend: bool,
) -> Result<Option<(Frame, u32)>> {
    if session != Session::Niri {
        return Ok(None);
    }
    let runner = ProcessWindowRunner;
    let Some(window) = niri::focused_window(&runner)? else {
        return Ok(None);
    };
    let density = niri_density(&runner, &window, output_infos)?;
    let frame = if no_blend {
        niri_native_render(&runner, &window, cursor)?
    } else {
        niri::capture_composited(grab_output, &runner, &window, cursor, output_infos)?
    };
    Ok(Some((frame, density)))
}

/// niri's own window render exactly as it hands it over: the window surface
/// with its alpha and nothing behind it, and without niri's border (drawn on
/// the tile, not the window).  The spare route for `--no-blend`, where
/// locating the render on a capture of the screen is not wanted.
fn niri_native_render(
    runner: &impl WindowCommandRunner,
    window: &niri::NiriWindow,
    cursor: bool,
) -> Result<Frame> {
    eprintln!(
        "vshot: niri's own render of window {} is used as it is (--no-blend): a translucent \
         window comes out transparent and niri's border is not included",
        window.id
    );
    niri::capture_window(runner, window, cursor)
}

/// niri's own window picker, then the picked window's own picture.  `None`
/// means this is not a niri session, so the compositor-agnostic picker takes
/// over.  Cancelling comes back as the same error that picker's cancel does.
fn niri_picked_window(
    grab_output: &mut dyn FnMut(&str, bool) -> Result<Frame>,
    output_infos: &[OutputInfo],
    cursor: bool,
    no_blend: bool,
) -> Result<Option<(Frame, u32)>> {
    niri_picked_window_for(
        grab_output,
        Session::detect(),
        output_infos,
        cursor,
        no_blend,
    )
}

/// See [`niri_active_window_for`] for why the session is passed in.
fn niri_picked_window_for(
    grab_output: &mut dyn FnMut(&str, bool) -> Result<Frame>,
    session: Session,
    output_infos: &[OutputInfo],
    cursor: bool,
    no_blend: bool,
) -> Result<Option<(Frame, u32)>> {
    if session != Session::Niri {
        return Ok(None);
    }
    let runner = ProcessWindowRunner;
    // niri only draws a crosshair here — it highlights no window while the
    // pointer moves — so say what the user is looking at.
    eprintln!("vshot: niri is picking a window — click one, Esc cancels");
    let Some(window) = niri::pick_window(&runner)? else {
        return Err(VshotError::SelectionCancelled);
    };
    let density = niri_density(&runner, &window, output_infos)?;
    let frame = if no_blend {
        niri_native_render(&runner, &window, cursor)?
    } else {
        niri::capture_composited(grab_output, &runner, &window, cursor, output_infos)?
    };
    Ok(Some((frame, density)))
}

/// The density to show niri's window picture at: the scale of the output the
/// window is on, taken from the Wayland topology rather than from niri, so that
/// every PNG vshot writes for one output carries the same density — `monitor`,
/// `region` and `window` would otherwise disagree on a fractional-scale screen,
/// where niri states 1.25 while the topology (and the rest of vshot) counts
/// whole device pixels per logical pixel.  niri's own scale is the fallback
/// when the topology does not know that output; an unplaceable window is an
/// error instead, because assuming 1 is what sizes a pinned image wrong.
fn niri_density(
    runner: &impl WindowCommandRunner,
    window: &niri::NiriWindow,
    output_infos: &[OutputInfo],
) -> Result<u32> {
    let placed = niri::window_scale(runner, window)?;
    density_from_placement(window.id, placed, output_infos)
}

/// The choice itself, split from the query so the rule can be tested without a
/// compositor: the topology's scale for that output wins, niri's own is the
/// fallback, and a window niri cannot place is an error rather than a guess.
fn density_from_placement(
    window_id: u64,
    placed: Option<(String, u32)>,
    output_infos: &[OutputInfo],
) -> Result<u32> {
    let Some((name, niri_scale)) = placed else {
        return Err(VshotError::NiriScreenshot(format!(
            "niri places window {window_id} on no output, so the density its pixels are meant \
             to be shown at is unknown"
        )));
    };
    Ok(output_infos
        .iter()
        .find(|info| info.name == name)
        .map_or(niri_scale, |info| info.scale))
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

/// Crops a rect at the resolution its pixels were captured at: straight out of
/// the one output that holds it whole, out of the composed scene only when it
/// straddles a monitor seam.
///
/// Every rectangle the user draws takes this route — a region, a picked window,
/// a window the compositor named — because the composed scene is stretched to
/// the highest scale in the layout: cropping it for a rect on a lower-density
/// screen hands back a nearest-upscale of that screen's pixels, which is both
/// twice the size of what was selected and half the detail that was on screen.
/// The density that comes back with the frame is the source's own, so the PNG
/// (and the pin) is sized by where the pixels came from rather than by the
/// densest monitor in the session.
fn crop_native(scene: &SceneSnapshot, geometry: Rect) -> Result<(Frame, u32)> {
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
    fn another_compositor_is_never_asked_through_niri() {
        // Every session but niri has to fall through to its own routes: asking
        // `niri msg` there would read a niri that is not the session's, which is
        // the mistake `active_output::Session` exists to prevent.
        for session in [
            Session::Hyprland,
            Session::Sway,
            Session::KWin,
            Session::Unknown,
        ] {
            let never = &mut |_: &str, _: bool| -> Result<Frame> {
                unreachable!("{session:?} must not go through niri")
            };
            assert_eq!(
                niri_active_window_for(never, session, &[], false, false).unwrap(),
                None,
                "{session:?} must not go through niri"
            );
            let never = &mut |_: &str, _: bool| -> Result<Frame> {
                unreachable!("{session:?} must not go through niri")
            };
            assert_eq!(
                niri_picked_window_for(never, session, &[], false, false).unwrap(),
                None,
                "{session:?} must not go through niri"
            );
        }
    }

    #[test]
    fn the_topology_decides_the_density_of_a_niri_window() {
        // niri states a fractional scale on a fractional-scale screen while the
        // rest of vshot counts whole device pixels, so the topology's integer
        // for that output is what every other vshot PNG carries too.
        let outputs = [output("DP-2", 2), output("eDP-1", 1)];
        assert_eq!(
            density_from_placement(12, Some(("DP-2".to_owned(), 2)), &outputs).unwrap(),
            2
        );
        assert_eq!(
            density_from_placement(12, Some(("DP-2".to_owned(), 1)), &outputs).unwrap(),
            2,
            "the topology's scale wins over niri's"
        );
    }

    #[test]
    fn an_output_the_topology_does_not_know_falls_back_to_niris_scale() {
        let outputs = [output("DP-2", 2)];
        assert_eq!(
            density_from_placement(12, Some(("HDMI-A-1".to_owned(), 1)), &outputs).unwrap(),
            1
        );
    }

    #[test]
    fn a_window_niri_cannot_place_is_an_error_and_not_a_guess() {
        let error = density_from_placement(12, None, &[]).unwrap_err();
        let message = error.to_string();
        assert!(message.contains("no output"), "{message}");
        assert!(message.contains("12"), "{message}");
    }

    /// Two screens of different densities, the dense one on the right: the
    /// shape the region rule is about.
    fn output(name: &str, scale: u32) -> OutputInfo {
        OutputInfo {
            global_id: 1,
            name: name.to_owned(),
            geometry: Rect::new(0, 0, 1920, 1080),
            pixel_size: crate::geometry::Size::new(1920 * scale, 1080 * scale),
            scale,
            transform: wayland_client::protocol::wl_output::Transform::Normal,
        }
    }

    fn mixed_density_scene() -> SceneSnapshot {
        let dense = OutputSnapshot::new(
            1,
            "DP-2",
            Rect::new(1920, 0, 1920, 1080),
            2,
            Frame::solid(Size::new(3840, 2160), [255, 0, 0, 255]).unwrap(),
        )
        .unwrap();
        let plain = OutputSnapshot::new(
            2,
            "DP-3",
            Rect::new(0, 0, 1920, 1080),
            1,
            Frame::solid(Size::new(1920, 1080), [0, 255, 0, 255]).unwrap(),
        )
        .unwrap();
        SceneSnapshot::from_outputs(vec![dense, plain]).unwrap()
    }

    #[test]
    fn a_region_keeps_the_density_of_the_screen_it_is_on() {
        // The scene is stretched to its highest output scale, so cropping it
        // for a rect on the 1x screen would answer with a nearest-upscale: the
        // double-size picture that made a 1080p selection come out twice as
        // large as what was drawn on screen.
        let scene = mixed_density_scene();
        assert_eq!(scene.scale(), 2);

        let (frame, density) = crop_native(&scene, Rect::new(10, 20, 100, 50)).unwrap();
        assert_eq!((frame.size(), density), (Size::new(100, 50), 1));
        assert_eq!(frame.pixel(Point::new(0, 0)), Some([0, 255, 0, 255]));

        // A rect on the dense screen keeps its native pixels, exactly as the
        // composed scene would have given them.
        let (frame, density) = crop_native(&scene, Rect::new(1930, 20, 100, 50)).unwrap();
        assert_eq!((frame.size(), density), (Size::new(200, 100), 2));
        assert_eq!(frame.pixel(Point::new(0, 0)), Some([255, 0, 0, 255]));

        // A rect across the seam is the one case with nothing but the scene.
        let (frame, density) = crop_native(&scene, Rect::new(1900, 20, 100, 50)).unwrap();
        assert_eq!((frame.size(), density), (Size::new(200, 100), 2));
    }

    #[test]
    fn a_text_bitmap_is_brought_to_the_crops_density() {
        // The helper draws labels at the scene's scale — 2 here — while the
        // region it belongs to came off the 1x screen, whose frame is half
        // that.  Compositing the bitmap as-is would print the label at twice
        // the size the user saw while placing it.
        let bitmap = crate::edit::TextBitmap {
            width: 2,
            height: 2,
            pixels: vec![
                255, 255, 255, 255, 255, 255, 255, 255, //
                255, 255, 255, 255, 255, 255, 255, 255,
            ],
        };
        let annotation = Annotation::Text {
            origin: Point::new(0, 0),
            text: "A".into(),
            scale: 1,
            color: [255, 255, 255, 255],
            font: String::new(),
            bitmap: Some(bitmap),
        };
        let pipeline =
            pipeline_for_annotations(vec![annotation], Rect::new(0, 0, 10, 10), 1, 2).unwrap();
        let frame = Frame::solid(Size::new(10, 10), [0, 0, 0, 0]).unwrap();
        let document = pipeline.apply(ImageDocument::new(frame)).unwrap();
        // One pixel of the label, not a 2x2 block of it.
        assert_eq!(
            document.frame().pixel(Point::new(0, 0)),
            Some([255, 255, 255, 255])
        );
        assert_eq!(document.frame().pixel(Point::new(1, 1)), Some([0, 0, 0, 0]));
    }

    #[test]
    fn annotation_pipeline_maps_global_points_into_cropped_pixels() {
        let pipeline = pipeline_for_annotations(
            vec![Annotation::stroke(
                EditorTool::Pen,
                vec![Point::new(12, 22), Point::new(14, 24)],
            )],
            Rect::new(10, 20, 20, 20),
            2,
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

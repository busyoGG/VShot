// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

mod annotate;
mod capture;
mod cli;
mod cli_i18n;
mod config;
mod edit;
mod element;
mod error;
mod geometry;
mod inject;
mod longshot;
mod model;
mod notify;
mod ocr;
mod output;
mod parallel;
mod pin;
mod pin_hdr;
mod pin_hdr_fp16;
mod pixel_fd;
mod qt_overlay;
mod record;
mod selection;
mod selection_region;
mod stitch;
mod translate;
mod wayland;

use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::Duration;

use capture::active_output::Session;
use capture::niri;
use capture::window::{ProcessWindowRunner, WindowCommandRunner};
use capture::{Capturer, CompositorWindowProvider, ProcessWindowProvider, WindowCandidate};
use cli::{Action, CaptureTarget};
use edit::EditPipeline;
use error::{Result, VshotError};
use geometry::Rect;
use model::hdr::{Transfer, REFERENCE_WHITE_NITS};
use model::{
    Frame, HdrDecision, HdrFrame, ImageDocument, OutputColor, OutputSnapshot, SceneSnapshot,
    ToneMapOptions,
};
use output::HdrHalf;
use wayland::topology::OutputInfo;
use wayland::{BackdropFrame, WaylandSession};

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
    // An internal mode, the way the Qt helper has `--pin-server`: the pin daemon
    // spawns this process to show pinned HDR images on layer surfaces of their
    // own.  It is read before the CLI parser, so it is not a subcommand and
    // never appears in the command tree.
    let mut arguments = std::env::args_os().skip(1);
    if let Some(flag) = arguments.next() {
        if flag == "--pin-hdr-server" {
            let socket = arguments.next().ok_or_else(|| VshotError::HdrPin {
                path: PathBuf::from("--pin-hdr-server"),
                reason: "no socket path was given".into(),
            })?;
            return pin_hdr::run(Path::new(&socket));
        }
    }
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
        Action::Annotate(action) => {
            // The annotation overlay is a resident daemon of its own: drawing on
            // the live screen needs no capture and no scene from here, only a
            // socket to talk to, exactly like pin management.
            return annotate::run(action);
        }
        Action::Settings => {
            // The settings window is a plain toplevel over the config file: no
            // capture, no scene, no Wayland connection of our own.
            return qt_overlay::run_settings();
        }
        Action::Ocr {
            source,
            destination,
            json,
        } => return run_ocr(source, destination, json),
        Action::Translate {
            source,
            destination,
            provider,
            from,
            to,
            json,
        } => return run_translate(source, destination, &provider, &from, &to, json),
        Action::Record(action) => {
            // Recording owns its own loop and writer; nothing below is shared
            // with the screenshot routes beyond the `Capturer`.
            return match action {
                cli::RecordAction::Start(request) => record::run(&request).map(|_| ()),
                cli::RecordAction::Mics => record::print_mics(),
                cli::RecordAction::Stop => record::stop(),
            };
        }
        Action::Replay(action) => {
            // A replay is a long-lived session that owns its own ring, control
            // socket and pid file; the control shapes are one-line requests.
            return match action {
                cli::ReplayAction::Start {
                    request,
                    background,
                } => {
                    // The portal's own frame loop is not wired to the ring, so
                    // a replay cannot use it.  Refused here rather than inside
                    // `run`, because the background path never reaches `run`
                    // in this process: the session is a child, and a sentence
                    // written to its stderr goes nowhere.
                    if request.portal {
                        return Err(VshotError::Recording(
                            "replay does not support the portal yet: the portal's own frame loop \
                             is not wired to the ring. Record through the portal instead (`vshot \
                             record ... --portal`)"
                                .into(),
                        ));
                    }
                    if background {
                        return replay_background(&request);
                    }
                    record::replay::run(&request)
                }
                cli::ReplayAction::Save {
                    path,
                    seconds,
                    save_dir,
                } => record::replay_save(path, seconds, save_dir).map(|_| ()),
                cli::ReplayAction::Status => record::replay_status(),
                cli::ReplayAction::Stop => record::replay_stop(),
            };
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
    // An HDR output's frozen scene is tone-mapped down by the capture itself,
    // and the SDR half of the file is tone-mapped later from the annotated HDR
    // frame.  Both have to be the same map: the user draws over the frozen
    // scene, so a mark lands on the pixels the file will hold only when the
    // scene the mark was placed on is the file's own SDR half.
    capture.set_tone_map(request.tone_map);

    // Everything below freezes the desktop, and the annotation overlay is a
    // window like any other: left alone, its toolbar would be baked into the
    // frozen frame and then into the file.  The guard hides it for as long as
    // this process lives, so it covers every route here -- and it drops on the
    // way out, error or not, so nothing stays hidden.  Annotations themselves
    // are meant to be in the picture, which is why this hides the toolbar and
    // not the overlay.
    let _annotate = annotate::CaptureGuard::for_screenshot();

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
            return finish_capture(
                &EditPipeline::new(),
                frame,
                None,
                density,
                // niri's own picker gives no rectangle on this side, so there
                // is no place to put a pin back on.
                None,
                // No editor ran, so there are no marks to hand over.
                None,
                &request,
                &mut wayland,
                // niri's own picker has no annotation editor, so no Pin button.
                false,
            );
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

    let edits = EditPipeline::new();
    // Device pixels per logical pixel of the frame, carried next to it: the
    // scene composes every output at the highest scale, but a monitor capture
    // keeps that output's own pixels, so the density follows the source.
    //
    // A window the compositor drew itself is already done at this point, and
    // taking the scene apart for it would be waste — and on an output the scene
    // cannot represent (rotated or flipped) it would be a failure, in the one
    // place that does not need a scene at all.
    // The HDR half of the capture, filled below from the same instant the SDR
    // scene was taken: `Some` only when the selection's own output offered a
    // 10-bit buffer.  A capture that has one is written as the SDR/HDR pair.
    let mut hdr_frame: Option<HdrHalf> = None;
    // Where on the desktop the capture came from, in global logical pixels, for
    // the destinations that put the image back on the screen: a pin lands back
    // exactly there.  `None` for a target with no place of its own (a composed
    // desktop, or a window the compositor drew itself).
    let mut capture_rect: Option<crate::geometry::Rect> = None;
    // Set when the user finished an editing session with the toolbar's Pin
    // button: the composed image goes to the screen instead of to the
    // destination the command line named.
    let mut pin_result = false;
    // Set when that pin was made inside the editor's own handoff, while the
    // editor was still drawing the same picture.  There is then nothing left
    // for the CLI to put on the screen, and nothing left to wait for.
    let mut handed_over = false;
    // The helper's own render of the annotated capture, when it made one.  Qt is
    // the only annotation renderer, so when this is set there is nothing left to
    // rasterize on this side.
    let mut rendered: Option<qt_overlay::RenderedCapture> = None;
    // The marks the helper reported, when the session drew any.  Only the Pin
    // destination reads them: the pin daemon stores them so the pin opens for a
    // second edit on the user's own marks instead of on the pixels they were
    // flattened into.
    let mut marks: Option<serde_json::Value> = None;
    let (frame, density) = if let Some(window) = native_window {
        window
    } else {
        let output_infos = topology?;
        let scene = capture_scene(&mut capture, &output_infos, request.cursor)?;
        // One HDR readback per output that offers it, taken now so the SDR and
        // HDR halves describe the same instant.  Targets that reconstruct a
        // window or compose every output never keep an HDR half.
        let hdr_outputs = if target_wants_hdr(&request.target) {
            capture_hdr_outputs(&mut capture, &scene, request.cursor)
        } else {
            Vec::new()
        };

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
                hdr_frame = hdr_for_region(&hdr_outputs, &scene, *geometry, request.tone_map.hdr);
                capture_rect = Some(*geometry);
                wayland.show_frozen(false)?;
                (frame, density)
            }
            CaptureTarget::RegionInteractive => {
                // The frame the helper draws is shown on an HDR backdrop when
                // there is one to show, and the helper is told to leave it out.
                let backdrop =
                    show_hdr_backdrop(&mut wayland, &scene, &hdr_outputs, request.tone_map.hdr);
                // The editor's Pin button: the capture goes to the screen
                // instead of to the destination the command line named, and it
                // has to be there before the editor -- which is still drawing
                // the very marks it hands over -- takes its copy away.  So the
                // pin happens here, inside the release, rather than after the
                // session returns: this is the only point at which the editor
                // is both still up and done drawing.
                let mut handoff_error: Option<VshotError> = None;
                let outcome =
                    qt_overlay::select_and_edit(&scene, &backdrop, &mut |result, render| {
                        // `false` is the ordinary OK: the editor drew on the
                        // capture and left it to the destination the command
                        // line named, which is still to be written below.
                        match pin_from_editor(&scene, &hdr_outputs, &request, result, render) {
                            Ok(pinned) => handed_over = pinned,
                            Err(error) => handoff_error = Some(error),
                        }
                        Ok("{}".to_string())
                    })?;
                if let Some(error) = handoff_error {
                    return Err(error);
                }
                let geometry = selection::validate_selection(&scene, outcome.rect)?;
                // The editor's other answer is a scrolling capture: the region
                // is scrolled and stitched instead of kept as one frame.  Any
                // marks drawn are dropped with it, because the picture the
                // stitch is made of does not exist yet -- the rows come back
                // from the scroll, not from this scene -- so a mark has nowhere
                // to land.  This route has no flags of its own, so the options
                // are the ones the settings window remembers.
                if outcome.long {
                    // The scrolling capture reads the desktop through the
                    // compositor, so every surface VShot is holding has to be
                    // gone first or the grab returns VShot's own frozen
                    // picture: the HDR backdrop sits on the Top layer, below
                    // the helper's overlay but above the window being
                    // scrolled, and the helper's own overlay is already gone
                    // (its process has exited by the time this returns).  On an
                    // HDR output that is the whole capture -- the stitch would
                    // be rows of the freeze, identical every time.
                    wayland.destroy_overlays()?;
                    let (options, backend) =
                        longshot::options_from_defaults(&crate::config::load().long)?;
                    let desktop = longshot::desktop_bounds(&output_infos)?;
                    let mut injector = inject::Injector::open(desktop, backend)?;
                    let result = longshot::run(
                        &mut capture,
                        &output_infos,
                        geometry,
                        &mut injector,
                        &options,
                    )?;
                    report_long_capture(&result, &injector, geometry);
                    return finish_capture(
                        &EditPipeline::new(),
                        result.frame,
                        // A stitched capture has no HDR half: its rows come back
                        // from the scroll as ordinary 8-bit pixels.
                        None,
                        result.density,
                        // A stitched capture is drawn from rows the scroll
                        // handed back, so it has no place on the desktop to put
                        // a pin back onto.
                        None,
                        // The scrolling capture never opened an editor, so
                        // there are no marks to hand over.
                        None,
                        &request,
                        &mut wayland,
                        // A stitched capture is written, never pinned: the
                        // scrolling action ends the session the way OK does.
                        false,
                    );
                }
                let (frame, density) = crop_native(&scene, geometry)?;
                hdr_frame = hdr_for_region(&hdr_outputs, &scene, geometry, request.tone_map.hdr);
                capture_rect = Some(geometry);
                pin_result = outcome.pin;
                // The helper rendered the annotated capture itself and sent it
                // back, so this side never rasterizes the marks: the image it
                // gets is the one the user was looking at, which is the whole
                // point of having a single renderer.  The marks travel beside
                // it for the Pin destination, which hands them to the daemon so
                // the pin can be reopened for editing on them.
                rendered = outcome.composite;
                marks = outcome.marks;
                (frame, density)
            }
            CaptureTarget::Monitor(name) if name == "current" => {
                wayland.show_frozen(false)?;
                let output_id = wayland.wait_for_current_output()?;
                let density = scene
                    .output(output_id)
                    .map_or(scene.scale(), |output| output.scale);
                hdr_frame = scene.output(output_id).and_then(|output| {
                    hdr_for_name(
                        &hdr_outputs,
                        &scene,
                        &output.name,
                        None,
                        request.tone_map.hdr,
                    )
                });
                (selection::crop_output(&scene, output_id)?, density)
            }
            CaptureTarget::Monitor(name) => {
                let frame = selection::crop_monitor(&scene, name)?;
                wayland.show_frozen(false)?;
                let density = scene
                    .output_by_name(name)
                    .map_or(scene.scale(), |output| output.scale);
                hdr_frame = hdr_for_name(&hdr_outputs, &scene, name, None, request.tone_map.hdr);
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
                // A window the compositor named but could not place (niri's
                // tiled windows) falls through to the pixel fallback below,
                // which at least finds the window under the pointer.
                if let Some(geometry) = metadata_window.and_then(|window| window.geometry) {
                    wayland.show_frozen(false)?;
                    let geometry = selection::validate_selection(&scene, geometry)?;
                    hdr_frame =
                        hdr_for_region(&hdr_outputs, &scene, geometry, request.tone_map.hdr);
                    capture_rect = Some(geometry);
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
                        // The pixel detector always reports a rectangle; a
                        // `None` here would mean it found nothing, which it
                        // spells as an error instead.
                        Ok(window) => window.geometry.ok_or_else(|| {
                            VshotError::ActiveWindowUnavailable(
                                "the pixel fallback reported no window rectangle".into(),
                            )
                        })?,
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
                    hdr_frame =
                        hdr_for_region(&hdr_outputs, &scene, geometry, request.tone_map.hdr);
                    capture_rect = Some(geometry);
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
                        .map(|geometry| WindowCandidate::unlabelled(geometry, String::new()))
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
                let picked = qt_overlay::pick_window(
                    &scene,
                    &candidates,
                    || {
                        // The picker re-lists the windows as the pointer travels:
                        // picking runs on a live desktop, and a workspace switch or a
                        // moved window would otherwise leave the highlight pointing at
                        // where a window used to be.  The pixel fallback has no window
                        // list to re-read, so it keeps what it started with.
                        if *pixel_detect {
                            return None;
                        }
                        ProcessWindowProvider.windows().ok()
                    },
                    // The sources are tried in turn for whatever window the
                    // pointer is on: the accessibility tree first, because it
                    // knows the widgets rather than guessing them, and the pixel
                    // detector last, because it answers for any window at all.
                    // Both take the frozen frame, which the pixel source reads
                    // and the accessibility source ignores.
                    Some(&|window: &WindowCandidate| -> Option<Vec<crate::selection_region::RegionNode>> {
                        let request = element::ElementRequest {
                            window,
                            scene: &scene,
                            fallback: request.element_fallback,
                        };
                        element::elements_of(&request)
                    }),
                )?;
                // Picking runs on the live desktop and only decides *what* to
                // capture, so the pixels have to come from now: wait for the
                // compositor to drop the picker's surfaces (they are hidden, but
                // the request travels), capture again, and resolve the click
                // against the windows that exist at this point in time.
                std::thread::sleep(PICK_SETTLE);
                let scene = capture_scene(&mut capture, &output_infos, request.cursor)?;
                // A picked window is captured as the SDR/HDR pair like any
                // other, and its frozen frame gets the same backdrop treatment
                // as a dragged selection.
                let hdr_outputs = capture_hdr_outputs(&mut capture, &scene, request.cursor);
                let backdrop =
                    show_hdr_backdrop(&mut wayland, &scene, &hdr_outputs, request.tone_map.hdr);
                let geometry = picked
                    .point
                    .and_then(|point| ProcessWindowProvider.window_at(point))
                    .unwrap_or(picked.rect);
                // The picked window's editor is the region editor, so its Pin
                // button hands over the same way -- see the `RegionInteractive`
                // arm for why the pin has to happen inside the release.
                let mut handoff_error: Option<VshotError> = None;
                let outcome = qt_overlay::edit_selection(
                    &scene,
                    geometry,
                    &backdrop,
                    &mut |result, render| {
                        // `false` is the ordinary OK: the editor drew on the
                        // capture and left it to the destination the command
                        // line named, which is still to be written below.
                        match pin_from_editor(&scene, &hdr_outputs, &request, result, render) {
                            Ok(pinned) => handed_over = pinned,
                            Err(error) => handoff_error = Some(error),
                        }
                        Ok("{}".to_string())
                    },
                )?;
                if let Some(error) = handoff_error {
                    return Err(error);
                }
                let geometry = selection::validate_selection(&scene, outcome.rect)?;
                let (frame, density) = crop_native(&scene, geometry)?;
                hdr_frame = hdr_for_region(&hdr_outputs, &scene, geometry, request.tone_map.hdr);
                capture_rect = Some(geometry);
                pin_result = outcome.pin;
                // The picker hands the frame it captured to the same editor the
                // region path uses, so its marks come back the same way: as the
                // helper's own render rather than as data to rasterize again.
                rendered = outcome.composite;
                marks = outcome.marks;
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
                capture_rect = Some(region);
                let desktop = longshot::desktop_bounds(&output_infos)?;
                let mut injector = inject::Injector::open(desktop, *inject)?;
                let result =
                    longshot::run(&mut capture, &output_infos, region, &mut injector, options)?;
                report_long_capture(&result, &injector, region);
                (result.frame, result.density)
            }
        }
    };

    // A pin the editor handed over is already on the screen: the daemon held
    // its answer until the frame carrying it was presented, and the editor
    // stopped on that answer.  Only a session whose destination *was* the pin
    // is finished here; one the command line also named a file for is still
    // owed that file, so it falls through to the write below.
    if handed_over && matches!(&request.destination, cli::Destination::Pin) {
        return wayland.destroy_overlays();
    }
    // The handoff already put this image on the screen, so pinning it again
    // would only replace the pixels that are up with a second copy of
    // themselves.  The Pin button says where the image also goes, not that the
    // capture stops being written.
    let pin_result = pin_result && !handed_over;

    if let Some(rendered) = rendered {
        return finish_rendered_capture(
            rendered,
            &frame,
            hdr_frame,
            density,
            capture_rect,
            marks.as_ref(),
            &request,
            &mut wayland,
            pin_result,
        );
    }
    finish_capture(
        &edits,
        frame,
        hdr_frame,
        density,
        capture_rect,
        // A route that rendered on this side -- the scrolling capture -- has no
        // session marks to hand over: it never opened an editor.
        None,
        &request,
        &mut wayland,
        pin_result,
    )
}

/// Reports a finished scrolling capture: what it stitched, and -- when not one
/// scroll moved anything -- why the result is a single frame.
fn report_long_capture(
    result: &longshot::LongShotResult,
    injector: &inject::Injector,
    region: Rect,
) {
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
}

/// Starts a replay session detached from this terminal, so it outlives the
/// shell that started it.  The session re-execs this same program with the
/// same arguments minus `--background`; nothing of this process's stdio is
/// inherited, and its pid file is written by the child once it is up.
fn replay_background(request: &record::ReplayRequest) -> Result<()> {
    let exe = std::env::current_exe().map_err(|error| {
        VshotError::Recording(format!("cannot find the vshot executable: {error}"))
    })?;
    // Rebuild the child's arguments from the parsed request, so `--background`
    // (which would re-detach forever) is dropped and every other choice the
    // user made is carried over.
    let mut args: Vec<std::ffi::OsString> = vec!["replay".into(), "start".into()];
    match &request.target {
        record::RecordTarget::Monitor(name) => {
            args.push("monitor".into());
            args.push(name.clone().into());
        }
        record::RecordTarget::All => args.push("all".into()),
        record::RecordTarget::Region(record::RegionTarget::Fixed(rect)) => {
            args.push("region".into());
            args.push("--geometry".into());
            args.push(
                format!(
                    "{},{} {}x{}",
                    rect.origin.x, rect.origin.y, rect.size.width, rect.size.height
                )
                .into(),
            );
        }
        record::RecordTarget::Region(record::RegionTarget::Pick) => {
            args.push("region".into());
        }
        record::RecordTarget::Window(record::WindowTarget::Active) => args.push("window".into()),
        record::RecordTarget::Window(record::WindowTarget::Pick) => {
            args.push("window".into());
            args.push("--pick".into());
        }
        record::RecordTarget::Window(record::WindowTarget::Filter(name)) => {
            args.push("window".into());
            args.push(name.clone().into());
        }
    }
    args.push("--window".into());
    args.push(request.window.to_string().into());
    args.push("--fps".into());
    args.push(request.fps.to_string().into());
    args.push("--encoder".into());
    args.push(request.encoder.word().into());
    args.push("--encoder-backend".into());
    args.push(request.encoder_backend.word().into());
    args.push("--gop".into());
    args.push(request.gop_secs.to_string().into());
    if request.cursor {
        args.push("--cursor".into());
    }
    // The microphone: `--no-mic` when the request has none, so a remembered
    // `cli.replay.mic` cannot open a microphone in a session the user asked to
    // be silent.  Without it the child reads the config and turns one on.
    match &request.mic {
        Some(record::MicChoice::Default) => args.push("--mic".into()),
        Some(record::MicChoice::Device(name)) => {
            args.push("--mic".into());
            args.push(name.clone().into());
        }
        None => args.push("--no-mic".into()),
    }
    if request.app_audio {
        args.push("--app-audio".into());
    }
    // The windows a `--follow` replay moves between.  An empty list is said
    // out loud for the same reason as the microphone: otherwise the config's
    // `cli.replay.follow` re-arms a list the user turned off.
    if request.follow.is_empty() {
        args.push("--no-follow".into());
    } else {
        for name in &request.follow {
            args.push("--follow".into());
            args.push(name.clone().into());
        }
    }
    if let Some(dir) = &request.save_dir {
        args.push("--save-dir".into());
        args.push(dir.clone().into());
    }

    let child = std::process::Command::new(&exe)
        .args(&args)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(if std::env::var_os("VSHOT_RECORD_DEBUG").is_some() {
            std::process::Stdio::inherit()
        } else {
            std::process::Stdio::null()
        })
        .spawn()
        .map_err(|error| {
            VshotError::Recording(format!(
                "could not start the replay session in the background: {error}"
            ))
        })?;
    println!("vshot: replay session started (pid {})", child.id());
    Ok(())
}

/// Reads the text out of a region of the screen, or out of an image file.
///
/// The capture half is `vshot region`'s: the scene is frozen, the overlay
/// frames the text, and the frame is cropped.  What differs is what happens
/// next — no annotation editor, no PNG, just the recognized text going to
/// stdout or the clipboard.
fn run_ocr(source: cli::OcrSource, destination: cli::OcrDestination, json: bool) -> Result<()> {
    // A file needs no compositor at all, so it takes the shortest path: the
    // annotation editor's text tool goes through here, and it has no scene.
    if let cli::OcrSource::File(path) = &source {
        let bytes = std::fs::read(path).map_err(|error| {
            VshotError::Ocr(format!("cannot read `{}`: {error}", path.display()))
        })?;
        let frame = Frame::from_png(&bytes)?;
        let lines = ocr::recognize(&frame);
        return finish_recognition(lines, destination, json);
    }

    let mut wayland = WaylandSession::connect()?;
    let topology = wayland.output_infos()?;
    let mut capture = Capturer::connect()?;
    let scene = capture_scene(&mut capture, &topology, false)?;

    let geometry = match source {
        cli::OcrSource::Geometry(geometry) => geometry,
        cli::OcrSource::Screen => {
            // Framing the text is the whole interaction: `select_region` runs
            // the overlay in its select-only mode, so there is no toolbar and
            // Enter ends the session rather than opening an editor.
            wayland.set_scene(scene.clone());
            let picked = qt_overlay::select_region(&scene);
            wayland.show_frozen(false)?;
            let cleanup = wayland.destroy_overlays();
            // A cancelled pick and a cleanup failure are both reported after
            // the overlays are gone, so the desktop is never left frozen.
            let geometry =
                picked.and_then(|geometry| selection::validate_selection(&scene, geometry));
            cleanup?;
            geometry?
        }
        cli::OcrSource::File(_) => unreachable!("handled above"),
    };

    let geometry = selection::validate_selection(&scene, geometry)?;
    let (frame, _density) = crop_native(&scene, geometry)?;
    // The overlays have to be gone before the text is printed: an error path
    // that leaves them mapped would freeze the desktop behind the output.
    wayland.show_frozen(false)?;
    let cleanup = wayland.destroy_overlays();

    let lines = ocr::recognize(&frame);
    cleanup?;
    finish_recognition(lines, destination, json)
}

/// Ends a recognition: JSON to stdout when it was asked for, otherwise the
/// text to wherever the caller wanted it and a notification about it.
fn finish_recognition(
    lines: Result<Vec<ocr::OcrLine>>,
    destination: cli::OcrDestination,
    json: bool,
) -> Result<()> {
    if json {
        // A caller that asked for JSON is a program, not a person: the result
        // is machine-readable, so there is nothing to announce and no desktop
        // notification is sent.  A failure is returned as itself, unwrapped by
        // `?`, and never announced either.
        let lines = lines?;
        // `to_json` adds no trailing newline, for the same reason `join_lines`
        // does not: stdout is the one destination where a line without an end
        // leaves the shell prompt sitting on the last line.
        let mut written = ocr::to_json(&lines);
        written.push('\n');
        let mut stdout = std::io::stdout();
        stdout
            .write_all(written.as_bytes())
            .and_then(|()| stdout.flush())
            .map_err(|error| VshotError::Ocr(format!("cannot write to stdout: {error}")))
    } else {
        finish_ocr(lines.map(|lines| ocr::join_lines(&lines)), destination)
    }
}

/// Sends the recognized text where the request pointed, and says so once it is
/// there.
///
/// The announcement is what makes a run from a keybinding usable: nothing else
/// on screen changes when the text lands on the clipboard.  It is not part of
/// the result — a session with no notification daemon still gets its text — so
/// the failure of the recognition is reported here and returned as itself.
fn finish_ocr(text: Result<String>, destination: cli::OcrDestination) -> Result<()> {
    let to_clipboard = matches!(destination, cli::OcrDestination::Clipboard);
    match text {
        Ok(text) => {
            let outcome = write_ocr_text(&text, destination);
            match &outcome {
                Ok(()) => notify::ocr_finished(&text, to_clipboard),
                Err(error) => notify::ocr_failed(&error.to_string()),
            }
            outcome
        }
        Err(error) => {
            notify::ocr_failed(&error.to_string());
            Err(error)
        }
    }
}

/// Sends recognized text where the request pointed.
fn write_ocr_text(text: &str, destination: cli::OcrDestination) -> Result<()> {
    match destination {
        cli::OcrDestination::Stdout => {
            use std::io::Write as _;
            let mut stdout = std::io::stdout();
            // The newline that ends the last line is the terminal's, not the
            // text's: `join_lines` joins without one so that what is copied or
            // piped is exactly the recognized text, and stdout is the one
            // destination where a line that does not end leaves the shell
            // prompt sitting on the last line of the output.
            let mut written = text.to_string();
            if !written.is_empty() {
                written.push('\n');
            }
            stdout
                .write_all(written.as_bytes())
                .and_then(|()| stdout.flush())
                .map_err(|error| VshotError::Ocr(format!("cannot write to stdout: {error}")))
        }
        cli::OcrDestination::Clipboard => output::copy_text_to_clipboard(text),
    }
}

/// Reads what to translate from the route the request named, translates it,
/// and puts the result where the request pointed.
///
/// The four routes share the translation itself and differ only in where the
/// source lines come from and what the result is: the `--stdin-ocr` route
/// never touches the compositor (it is the editor's primitive), the file and
/// fixed-region routes recognize and translate, and the overlay route hands
/// the whole interaction to the Qt helper, which composites the translation
/// into a PNG of its own.
fn run_translate(
    source: cli::TranslateSource,
    destination: cli::TranslateDestination,
    provider: &str,
    from: &str,
    to: &str,
    json: bool,
) -> Result<()> {
    match source {
        cli::TranslateSource::StdinOcr => {
            // The envelope is already on stdin, so there is no capture and no
            // OCR run: this path has to stay fast and leave the desktop alone,
            // because the editor calls it while its window is on screen.
            let mut input = String::new();
            std::io::Read::read_to_string(&mut std::io::stdin(), &mut input).map_err(|error| {
                VshotError::Translate(format!("cannot read the OCR JSON from stdin: {error}"))
            })?;
            let defaults = crate::config::load().translate;
            let chain = translate::engine_chain(&defaults, provider)?;
            let providers = translate::as_providers(&chain);
            let translated = translate::translate_envelope_chain(&input, &providers, from, to)?;
            match destination {
                // The primitive always answers with the envelope; a caller
                // that asked for the clipboard gets the translated text.
                cli::TranslateDestination::Clipboard => {
                    output::copy_text_to_clipboard(&translated.text)
                }
                _ => write_plain_text(&translated.envelope),
            }
        }
        cli::TranslateSource::File(path) => {
            let bytes = std::fs::read(&path).map_err(|error| {
                VshotError::Translate(format!("cannot read `{}`: {error}", path.display()))
            })?;
            let frame = Frame::from_png(&bytes)?;
            finish_translation(
                ocr::recognize(&frame),
                destination,
                provider,
                from,
                to,
                json,
            )
        }
        cli::TranslateSource::Geometry(geometry) => {
            // The fixed-region route is `vshot ocr --geometry` up to the
            // recognition, then translation instead of output.
            let mut wayland = WaylandSession::connect()?;
            let topology = wayland.output_infos()?;
            let mut capture = Capturer::connect()?;
            let scene = capture_scene(&mut capture, &topology, false)?;
            let geometry = selection::validate_selection(&scene, geometry)?;
            let (frame, _density) = crop_native(&scene, geometry)?;
            // The overlays have to be gone before anything is printed: an
            // error path that leaves them mapped would freeze the desktop.
            wayland.show_frozen(false)?;
            let cleanup = wayland.destroy_overlays();
            let lines = ocr::recognize(&frame);
            cleanup?;
            finish_translation(lines, destination, provider, from, to, json)
        }
        cli::TranslateSource::Screen => run_translate_overlay(destination, provider, from, to),
    }
}

/// Ends a translation: the envelope to stdout when JSON was asked for,
/// otherwise the translated text to wherever the caller wanted it and — for
/// the routes that captured — a notification about it.
fn finish_translation(
    lines: Result<Vec<ocr::OcrLine>>,
    destination: cli::TranslateDestination,
    provider: &str,
    from: &str,
    to: &str,
    json: bool,
) -> Result<()> {
    let lines = lines?;
    let defaults = crate::config::load().translate;
    let chain = translate::engine_chain(&defaults, provider)?;
    let providers = translate::as_providers(&chain);
    let translated =
        translate::translate_envelope_chain(&ocr::to_json(&lines), &providers, from, to)?;
    if json {
        // A JSON caller is a program: no notification, and the envelope is the
        // whole answer.
        return write_plain_text(&translated.envelope);
    }
    // A failed line falls back to its source, which is otherwise
    // indistinguishable from a translation: say so on stderr, but leave the
    // exit code alone.
    translated.warn_left_in_source();
    let to_clipboard = matches!(destination, cli::TranslateDestination::Clipboard);
    let outcome = match destination {
        cli::TranslateDestination::Clipboard => output::copy_text_to_clipboard(&translated.text),
        _ => write_plain_text(&translated.text),
    };
    announce_translation(&translated.text, to_clipboard, &outcome);
    outcome
}

/// The overlay route: the Qt helper runs the whole interaction — pick a
/// region, recognize it, translate it, draw the translation over the original
/// — and writes the composited PNG to `result_path`.  This side only reports
/// or copies it.
fn run_translate_overlay(
    destination: cli::TranslateDestination,
    provider: &str,
    from: &str,
    to: &str,
) -> Result<()> {
    let mut wayland = WaylandSession::connect()?;
    let topology = wayland.output_infos()?;
    let mut capture = Capturer::connect()?;
    let scene = capture_scene(&mut capture, &topology, false)?;
    wayland.set_scene(scene.clone());

    // The helper needs an absolute path to write to.  A requested `--output`
    // is expanded (strftime and all) and made absolute; with no output the
    // composited PNG goes to a private temporary file that lives until this
    // function is done reading or copying it.
    let directory = tempfile::Builder::new()
        .prefix("vshot-translate-")
        .tempdir_in("/dev/shm")
        .or_else(|_| tempfile::tempdir())
        .map_err(|error| {
            VshotError::Translate(format!(
                "cannot create a temporary directory for the translation: {error}"
            ))
        })?;
    let result_path = match &destination {
        cli::TranslateDestination::Png(path) => absolute_output_path(path)?,
        _ => directory.path().join("translated.png"),
    };

    let outcome = qt_overlay::translate_overlay(&scene, from, to, provider, &result_path);
    wayland.show_frozen(false)?;
    let cleanup = wayland.destroy_overlays();
    let outcome = outcome.and_then(|outcome| cleanup.map(|()| outcome));
    let outcome = outcome?;

    let result = match &destination {
        // The helper wrote the file itself.
        cli::TranslateDestination::Png(_) => Ok(()),
        cli::TranslateDestination::Clipboard => {
            let image_path = outcome.image_path.as_deref().unwrap_or(&result_path);
            let bytes = std::fs::read(image_path).map_err(|error| {
                VshotError::Translate(format!(
                    "cannot read the translated image `{}`: {error}",
                    image_path.display()
                ))
            });
            bytes.and_then(|bytes| output::copy_png_to_clipboard(&bytes))
        }
        cli::TranslateDestination::Stdout => write_plain_text(&outcome.text),
    };
    let to_clipboard = matches!(destination, cli::TranslateDestination::Clipboard);
    announce_translation(&outcome.text, to_clipboard, &result);
    result
}

/// Expands a `--output` path and makes it absolute, so the Qt helper has a
/// path its own working directory cannot change the meaning of.
fn absolute_output_path(path: &std::path::Path) -> Result<std::path::PathBuf> {
    let expanded = output::expanded_output_path(path);
    let absolute = if expanded.is_absolute() {
        expanded
    } else {
        let directory = std::env::current_dir().map_err(|error| {
            VshotError::Translate(format!("cannot resolve the output path: {error}"))
        })?;
        directory.join(expanded)
    };
    // The helper opens this path itself, so the directory has to be there before
    // the session starts rather than at some write of ours afterwards.  A
    // strftime pattern routinely names one that does not exist yet — see
    // `output::create_parent_directories`.
    output::create_parent_directories(&absolute)?;
    Ok(absolute)
}

/// Says a translation finished, or failed, using the same switch `vshot ocr`
/// has.  A failure to announce is never a failure of the translation.
fn announce_translation(text: &str, to_clipboard: bool, outcome: &Result<()>) {
    match outcome {
        Ok(()) => notify::translate_finished(text, to_clipboard),
        Err(error) => notify::translate_failed(&error.to_string()),
    }
}

/// Prints one line without letting the shell prompt sit on it, the way
/// `vshot ocr` prints text.
fn write_plain_text(text: &str) -> Result<()> {
    let mut written = text.to_owned();
    if !written.is_empty() {
        written.push('\n');
    }
    write_stdout(&written)
}

/// Writes `text` to stdout exactly, adding no newline of its own.
fn write_stdout(text: &str) -> Result<()> {
    use std::io::Write as _;
    let mut stdout = std::io::stdout();
    stdout
        .write_all(text.as_bytes())
        .and_then(|()| stdout.flush())
        .map_err(|error| VshotError::Translate(format!("cannot write to stdout: {error}")))
}

/// Renders the annotations into the captured frame and writes it where the
/// request pointed.  The routes that hand over finished pixels — a compositor's
/// own window screenshot — have nothing to render and pass an empty pipeline.
///
/// Every destination leaves through here — the SDR/HDR pair, the clipboard, the
/// Pin button — so the two halves and the pin's place are decided once.
#[allow(clippy::too_many_arguments)] // the capture, its two halves, its density and its place
fn finish_capture(
    edits: &EditPipeline,
    frame: Frame,
    // The HDR half of the capture, when the selection's output offered a 10-bit
    // buffer.  It is the same crop as `frame`, so the pipeline runs over both:
    // the SDR one for the PNG, and the HDR one for the `.hdr` written beside it.
    hdr: Option<HdrHalf>,
    density: u32,
    // Where on the desktop the capture came from, in global logical pixels, for
    // a destination that puts the image back on the screen: the pin daemon
    // lands the pin exactly there.  `None` for a target with no place of its own
    // — a composed desktop, or a window the compositor drew itself, whose
    // rectangle this side never learns.
    capture_rect: Option<crate::geometry::Rect>,
    // The marks an editing session left, for the Pin destination; see
    // `finish_rendered_capture`.
    marks: Option<&serde_json::Value>,
    request: &cli::Request,
    wayland: &mut WaylandSession,
    // The editor's Pin button: the image goes to the screen whatever the command
    // line asked the destination to be.
    pin: bool,
) -> Result<()> {
    // The desktop is unfrozen before either image is encoded.  The editor's own
    // overlay is the helper's and goes when it exits, but the surfaces VShot
    // still holds — the HDR backdrop, when the frame is being shown on one —
    // keep the screen frozen while the annotations are composited into each half
    // and the HDR half is encoded.  That is the delay the user meets as a slow
    // close, and nothing below needs those surfaces, so they go first.
    let cleanup = wayland.destroy_overlays();
    let (sdr, hdr_out) = sdr_and_hdr(Some(edits), frame, hdr, request.tone_map)?;
    let result = write_capture_and_pin(
        &sdr,
        hdr_out.as_ref(),
        density,
        capture_rect,
        marks,
        // A route that rendered on this side has no session marks and so no
        // pristine base to keep them against.
        None,
        request,
        pin,
    );
    result.and(cleanup)
}

/// Writes the capture to whatever the command line asked for, and then to the
/// screen when the toolbar's Pin button was pressed.
///
/// The two are not alternatives.  `-o` names a file the user asked for, and the
/// Pin button says the image *also* goes on the screen; pressing it used to
/// replace the destination outright, so a capture pinned from the editor was
/// never written anywhere.  A command line that already named the pin as its
/// destination has had it, and is not pinned a second time.
#[allow(clippy::too_many_arguments)] // the capture, its two halves, its place and its marks
fn write_capture_and_pin(
    sdr: &Frame,
    hdr: Option<&HdrHalf>,
    density: u32,
    capture_rect: Option<crate::geometry::Rect>,
    marks: Option<&serde_json::Value>,
    // The capture before the marks were drawn on it, for a pin that carries
    // them; `None` on every route that never opened an editor.
    base: Option<&[u8]>,
    request: &cli::Request,
    pin: bool,
) -> Result<()> {
    let write = |destination: &cli::Destination| {
        output::write_frame_with_hdr(
            sdr,
            hdr,
            destination,
            density,
            request.compression,
            request.hdr_format,
            capture_rect.map(|rect| rect.origin),
            marks,
            base,
        )
    };
    let result = write(&request.destination);
    if pin && !matches!(request.destination, cli::Destination::Pin) {
        // Both are attempted even when the first failed: a full disk that lost
        // the file is no reason to also lose the pin, and the error that
        // surfaces is the first one, which is the one the user asked for.
        return result.and(write(&cli::Destination::Pin));
    }
    result
}

/// The pin half of a region session's handoff: the editor has finished drawing
/// and the capture it was drawing on has to be on the screen before it stops.
///
/// Run from inside the helper dialogue, on the editor's release, because that
/// is the only moment the editor is both still up and done drawing -- its
/// pixels reach this side as an argument, not through the dialogue's own
/// return, which happens after the helper is gone.  Only a session that asked
/// for the Pin button does anything here: every other session's release is a
/// plain goodbye, and its capture goes on to the destination the command line
/// named.
///
/// Everything this needs is rebuilt from the scene rather than kept in the
/// caller's locals, because the caller is still inside the expression that
/// produces them: the rect comes from the editor's own answer, and the capture
/// it was drawn on is re-cropped here.  Cropping the same rectangle off the
/// same frozen scene gives the same pixels, so the pin's pristine base is the
/// capture the user was looking at.
fn pin_from_editor(
    scene: &SceneSnapshot,
    hdr_outputs: &[HdrOutput],
    request: &cli::Request,
    result: &[u8],
    rendered: Option<&qt_overlay::RenderedCapture>,
) -> Result<bool> {
    // Parsed without the render, which is borrowed from the dialogue rather
    // than moved out of it: a destination the command line named still has to
    // be written once this handoff is done, and it wants these same pixels.
    let outcome = qt_overlay::parse_outcome_with_render(
        qt_overlay::HelperOutput {
            json: result.to_vec(),
            composite: None,
        },
        scene.bounds(),
    )?;
    if !outcome.pin {
        return Ok(false);
    }
    let Some(rendered) = rendered else {
        // The Pin button was pressed and the editor rendered nothing, which
        // only happens when its own render failed; it reported that through its
        // status, so there is nothing here to put on the screen.
        return Err(VshotError::Pin(
            "the editor asked to pin a capture it did not render".into(),
        ));
    };
    let geometry = selection::validate_selection(scene, outcome.rect)?;
    let (base, density) = crop_native(scene, geometry)?;
    let hdr = hdr_for_region(hdr_outputs, scene, geometry, request.tone_map.hdr);
    crate::pin::hand_off_region_pin(
        &base,
        hdr.as_ref(),
        density,
        geometry,
        rendered,
        outcome.marks.as_ref(),
        request.tone_map,
    )?;
    Ok(true)
}

/// [`finish_capture`] when the helper rendered the capture itself.
///
/// Qt is the only annotation renderer now, so its render *is* the SDR result and
/// there is nothing left to composite here: `composite` is written as it came.
/// The HDR half still has to be marked, and that is what the layer is for — an
/// opaque picture cannot be composited onto HDR, since it would replace the
/// light instead of marking it, so the marks alone are blended in linear light.
fn finish_rendered_capture(
    rendered: crate::qt_overlay::RenderedCapture,
    // The capture as it was before the helper drew on it, when the destination
    // is a pin that carries marks: the daemon keeps it as the picture those
    // marks belong to, so a second edit opens on the marks instead of on the
    // flattening.  Only read when `marks` is set.
    base: &Frame,
    hdr: Option<HdrHalf>,
    density: u32,
    capture_rect: Option<crate::geometry::Rect>,
    // The marks the helper reported, for the Pin destination: the daemon keeps
    // them so the pin can be opened for editing again on the user's own marks.
    marks: Option<&serde_json::Value>,
    request: &cli::Request,
    wayland: &mut WaylandSession,
    pin: bool,
) -> Result<()> {
    let cleanup = wayland.destroy_overlays();
    let sdr = rendered.composite;
    let hdr_out = match hdr {
        Some(HdrHalf {
            frame,
            reference_nits,
        }) => {
            let mut marked = frame;
            marked.composite_srgb_layer(&rendered.marks)?;
            marked.carries_hdr(request.tone_map.hdr).then_some(HdrHalf {
                frame: marked,
                reference_nits,
            })
        }
        None => None,
    };
    // The base is only needed by the pin destination, and encoding it costs a
    // PNG; a session that drew nothing has no marks to carry it for.  It is
    // read only when the pin really is one of the two destinations.
    let pin_wanted = pin && !matches!(request.destination, cli::Destination::Pin);
    let base_png = match (marks, pin_wanted) {
        (Some(_), true) => Some(base.to_png()?),
        _ => None,
    };
    let result = write_capture_and_pin(
        &sdr,
        hdr_out.as_ref(),
        density,
        capture_rect,
        marks,
        base_png.as_deref(),
        request,
        pin,
    );
    result.and(cleanup)
}

/// The two images of one capture: the SDR PNG and, when the content is really
/// HDR, the HDR frame written beside it.
///
/// Qt renders the annotations, so `edits` is the operations this side still
/// rasterizes itself — the scrolling capture's stitched frame, which was never
/// in an editing session — and is `None` on every path the helper rendered.
/// When it is `None` the SDR half is the frame as it came and the HDR half
/// keeps its light unmarked; the marks are composited by
/// [`finish_rendered_capture`], which has the helper's own render to take the
/// SDR half from.
///
/// When it is `Some`, both halves are annotated here: mosaics pixelate in
/// linear light and every other mark composites in linear light, and the SDR
/// half is vshot's own tone map of that same content, so both files describe
/// one set of marks over one set of light.  A 10-bit buffer that carries no
/// light beyond SDR white is not HDR content at all: it keeps the ordinary path
/// and no HDR file is written beside the PNG.
fn sdr_and_hdr(
    edits: Option<&EditPipeline>,
    frame: Frame,
    hdr: Option<HdrHalf>,
    tone_map: ToneMapOptions,
) -> Result<(Frame, Option<HdrHalf>)> {
    let hdr = match hdr {
        Some(HdrHalf {
            frame,
            reference_nits,
        }) => {
            let annotated = match edits {
                Some(edits) => edits.apply_to_hdr(frame)?,
                None => frame,
            };
            annotated.carries_hdr(tone_map.hdr).then_some(HdrHalf {
                frame: annotated,
                reference_nits,
            })
        }
        None => None,
    };
    // The SDR half is vshot's own tone map of the annotated HDR frame rather
    // than a second capture.  The compositor's own SDR rendition of an HDR
    // output cannot be used: Hyprland writes it against
    // `DEFAULT_SRGB_IMAGE_DESCRIPTION`, whose peak is 80 cd/m2, while the SDR
    // white an SDR image means is the output's own reference — 203 here — so it
    // puts ordinary SDR content well below white instead of on it (measured:
    // SDR white at sRGB 220 of 255, 145 of 203 cd/m2).  Taking it would dim the
    // whole capture, not just the highlights.
    match hdr {
        Some(half) => Ok((half.frame.tone_map_to_srgb_with(tone_map)?, Some(half))),
        None => match edits {
            Some(edits) => Ok((edits.apply(ImageDocument::new(frame))?.into_frame(), None)),
            None => Ok((frame, None)),
        },
    }
}

/// Whether a target keeps an HDR half **from the first capture**.  Only the
/// routes that end in one output's own pixels can: a composed desktop or a
/// reconstructed window has no single 10-bit buffer behind it.
///
/// `window pick` is not one of them here — the click decides the window on a
/// live desktop, so it takes its HDR half from the second capture, once the
/// window is known (see the `WindowPick` arm of [`run`]); running the first
/// capture HDR as well would only be thrown away.
fn target_wants_hdr(target: &CaptureTarget) -> bool {
    matches!(
        target,
        CaptureTarget::RegionFixed(_)
            | CaptureTarget::RegionInteractive
            | CaptureTarget::Monitor(_)
            | CaptureTarget::ActiveWindow { .. }
    )
}

/// One output's HDR half, kept together with what the compositor said about the
/// output it came from.
struct HdrOutput {
    name: String,
    frame: HdrFrame,
    /// The light the frame's `1.0` stands for, from the output's own description
    /// (or VShot's own when the compositor named none).
    reference_nits: f32,
    /// The output's own description: the encoding the frame's pixels are in, and
    /// what decides whether a backdrop surface can carry them.
    color: OutputColor,
}

/// Captures the HDR view of every output that offers one, keyed by output name.
/// Best effort: an output with no 10-bit buffer, or a backend that cannot hand
/// HDR pixels over at all, is simply absent, and the SDR path stands alone.
fn capture_hdr_outputs(
    capture: &mut Capturer,
    scene: &SceneSnapshot,
    cursor: bool,
) -> Vec<HdrOutput> {
    let debug = std::env::var_os("VSHOT_HDR_DEBUG").is_some();
    let mut outputs = Vec::new();
    for output in scene.outputs() {
        // Only an output the compositor itself describes as HDR can carry an
        // HDR half.  This is the Wayland reading of Starward's Windows rule —
        // ask the display, do not guess from the capture buffer — and it keeps
        // a 10-bit SDR output (which some compositors offer) or an undescribed
        // one from producing a bogus `.hdr`.
        let color = capture.output_color(&output.name).ok().flatten();
        if debug {
            eprintln!("vshot: hdr: output {} colour {color:?}", output.name);
        }
        let Some(color) = color.filter(OutputColor::is_hdr) else {
            continue;
        };
        // The reference white is what turns the frame's `1.0` back into an
        // absolute luminance, which the backdrop surface needs; a compositor
        // that describes no reference leaves VShot's own.
        let reference_nits = if color.reference_nits.is_finite() && color.reference_nits > 0.0 {
            color.reference_nits
        } else {
            REFERENCE_WHITE_NITS
        };
        match capture.capture_output_hdr(&output.name, cursor, color) {
            Ok(Some(hdr)) => outputs.push(HdrOutput {
                name: output.name.clone(),
                frame: hdr,
                reference_nits,
                color,
            }),
            Ok(None) => {
                if debug {
                    eprintln!("vshot: hdr: {} offered no HDR buffer", output.name);
                }
            }
            Err(error) => {
                if debug {
                    eprintln!("vshot: hdr: {} failed: {error}", output.name);
                }
            }
        }
    }
    outputs
}

/// The HDR half of one rectangle, cropped from the output that wholly contains
/// it, or `None` when no such output offered HDR or the rectangle holds no light
/// above SDR white.
fn hdr_for_region(
    hdr_outputs: &[HdrOutput],
    scene: &SceneSnapshot,
    geometry: Rect,
    decision: HdrDecision,
) -> Option<HdrHalf> {
    let output = scene.outputs().iter().find(|output| {
        output
            .geometry
            .clamp_to(geometry)
            .is_some_and(|clamped| clamped == geometry)
    })?;
    hdr_for_name(hdr_outputs, scene, &output.name, Some(geometry), decision)
}

/// The HDR half of one output — all of it, or the part `region` covers — or
/// `None` when it offered none, or when that rectangle holds no light above SDR
/// white.
///
/// `region` is in global logical pixels, the same space the scene's output
/// geometries are in; `None` asks about the whole output.  The test is the same
/// [`HdrFrame::carries_hdr`] the file gates use, so an output whose rectangle is
/// an ordinary SDR desktop is not handed to the backdrop — and the frozen frame
/// behind the selection is the SDR one, exactly as it is on a monitor with no
/// HDR at all.  Without this the overlay would be told to leave the frame out
/// for a backdrop that is not dimmer or brighter but *different*: the HDR read
/// of an SDR desktop is not the same image the compositor draws.
fn hdr_for_name(
    hdr_outputs: &[HdrOutput],
    scene: &SceneSnapshot,
    name: &str,
    region: Option<Rect>,
    decision: HdrDecision,
) -> Option<HdrHalf> {
    let output = hdr_outputs.iter().find(|output| output.name == name)?;
    let frame = match region {
        Some(region) => {
            let scene_output = scene
                .outputs()
                .iter()
                .find(|scene_output| scene_output.name == name)?;
            let scale = i32::try_from(scene_output.scale).ok()?;
            let local = Rect::new(
                (region.left() - scene_output.geometry.left()).checked_mul(scale)?,
                (region.top() - scene_output.geometry.top()).checked_mul(scale)?,
                region.size.width.checked_mul(scene_output.scale)?,
                region.size.height.checked_mul(scene_output.scale)?,
            );
            output.frame.crop(local).ok()?
        }
        None => output.frame.clone(),
    };
    frame.carries_hdr(decision).then_some(HdrHalf {
        frame,
        reference_nits: output.reference_nits,
    })
}

/// Puts the frozen HDR half of each output on a backdrop surface below the Qt
/// helper's overlay, and answers which outputs are shown that way.
///
/// The overlay then leaves the frame to the backdrop, so the picture behind the
/// selection is the light the screen showed rather than the tone map of it.
/// Best effort throughout: a session without colour management, or an output
/// without an HDR half, simply keeps the SDR overlay it always had.
fn show_hdr_backdrop(
    wayland: &mut WaylandSession,
    scene: &SceneSnapshot,
    hdr_outputs: &[HdrOutput],
    decision: HdrDecision,
) -> Vec<String> {
    if hdr_outputs.is_empty() {
        return Vec::new();
    }
    // The backdrop's surfaces are built from the scene, so the session needs it
    // even on the routes that hand the frame to the helper instead.
    wayland.set_scene(scene.clone());
    let frames = hdr_outputs
        .iter()
        // The surface declares the output's own description and the buffer is
        // written in the output's own primaries, so what the description says and
        // what the pixels hold are the same thing and the compositor hands them
        // through untouched.  The one thing that still has to match is the
        // transfer: the words are PQ codes, so an output described with any other
        // curve — HLG, say — would be handed a buffer it would decode wrongly, and
        // the helper draws the SDR frame there instead.
        .filter(|output| output.color.transfer == Transfer::Pq)
        // And only when the frame really carries light above SDR white: an
        // output handed a backdrop it does not need is handed one that is not
        // the picture the compositor draws, so the frozen frame behind the
        // selection would be a different image rather than a brighter one.
        .filter_map(|output| {
            hdr_for_name(hdr_outputs, scene, &output.name, None, decision).map(|half| {
                BackdropFrame {
                    name: output.name.clone(),
                    frame: half.frame,
                    reference_nits: half.reference_nits,
                }
            })
        })
        .collect::<Vec<_>>();
    if frames.is_empty() {
        return Vec::new();
    }
    match wayland.show_hdr_backdrop(&frames) {
        Ok(shown) => {
            if !shown.is_empty() && std::env::var_os("VSHOT_HDR_DEBUG").is_some() {
                eprintln!("vshot: hdr: backdrop on {}", shown.join(", "));
            }
            shown
        }
        Err(error) => {
            if std::env::var_os("VSHOT_HDR_DEBUG").is_some() {
                eprintln!("vshot: hdr: no backdrop ({error}); the overlay stays SDR");
            }
            Vec::new()
        }
    }
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
    // A Hyprland session running `hypr-dynamic-cursors` can be drawing the
    // pointer into the very frames this reads; see `hypr_cursor`.  The binding
    // is named so it lives until the scene is complete — including on the `?`
    // paths below, which is why it is not a plain `_`.
    let _cursors_suspended = capture::hypr_cursor::suspend();
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
    use crate::geometry::{Point, Rect, Size};
    use crate::model::hdr::Primaries;
    use crate::model::Frame;

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

    /// An HDR half painted one flat luminance, small enough to keep the tests
    /// cheap: a crop is told apart by the value it carries and nothing else.
    fn hdr_half(width: u32, height: u32, value: f32) -> HdrFrame {
        HdrFrame::new(
            Size::new(width, height),
            vec![[value, value, value, 1.0]; (width * height) as usize],
        )
        .unwrap()
    }

    /// One output's HDR half for the tests: a flat luminance over an output the
    /// compositor described exactly the way the backdrop needs.
    fn hdr_output_half(name: &str, width: u32, height: u32, value: f32) -> HdrOutput {
        HdrOutput {
            name: name.to_owned(),
            frame: hdr_half(width, height, value),
            reference_nits: REFERENCE_WHITE_NITS,
            color: OutputColor {
                transfer: Transfer::Pq,
                primaries: Primaries::Bt2020,
                reference_nits: REFERENCE_WHITE_NITS,
            },
        }
    }

    /// A two-screen scene — a 2x screen on the right, a 1x one on the left —
    /// each with its own HDR half, the shape `hdr_for_region` has to choose
    /// between.  Kept tiny: the rule only looks at geometry and scale, and a
    /// realistic 4K pair would cost hundreds of megabytes per test.
    fn hdr_scene() -> (SceneSnapshot, Vec<HdrOutput>) {
        let dense = OutputSnapshot::new(
            1,
            "DP-2",
            Rect::new(64, 0, 16, 16),
            2,
            Frame::solid(Size::new(32, 32), [255, 0, 0, 255]).unwrap(),
        )
        .unwrap();
        let plain = OutputSnapshot::new(
            2,
            "DP-3",
            Rect::new(0, 0, 16, 16),
            1,
            Frame::solid(Size::new(16, 16), [0, 255, 0, 255]).unwrap(),
        )
        .unwrap();
        let scene = SceneSnapshot::from_outputs(vec![dense, plain]).unwrap();
        let hdr = vec![
            hdr_output_half("DP-2", 32, 32, 4.0),
            hdr_output_half("DP-3", 16, 16, 2.0),
        ];
        (scene, hdr)
    }

    #[test]
    fn the_hdr_half_of_a_region_comes_from_the_output_that_contains_it() {
        // A region is an HDR half only when one output wholly contains it: the
        // 1x screen's five-by-four selection stays five by four, while the 2x
        // screen's doubles into its own native pixels, exactly as the SDR crop
        // does.
        let (scene, hdr) = hdr_scene();
        let hdr_only = HdrDecision::default();
        let plain = hdr_for_region(&hdr, &scene, Rect::new(2, 3, 5, 4), hdr_only).unwrap();
        assert_eq!(plain.frame.size(), Size::new(5, 4));
        assert_eq!(plain.frame.pixel(0, 0), Some([2.0, 2.0, 2.0, 1.0]));

        let dense = hdr_for_region(&hdr, &scene, Rect::new(66, 3, 5, 4), hdr_only).unwrap();
        assert_eq!(dense.frame.size(), Size::new(10, 8));
        assert_eq!(dense.frame.pixel(0, 0), Some([4.0, 4.0, 4.0, 1.0]));
    }

    #[test]
    fn a_region_of_plain_sdr_gets_no_hdr_half_even_on_an_hdr_output() {
        // The other half of the classification, and the one the user hit: an
        // output the compositor describes as HDR still holds ordinary SDR
        // content most of the time, and that content must keep the SDR path --
        // no second file, no HDR pin, no backdrop, and above all no white point
        // moved down over a handful of rounding pixels.
        let (scene, _) = hdr_scene();
        let sdr = vec![hdr_output_half("DP-2", 32, 32, 1.0)];
        let sdr_region = Rect::new(66, 3, 5, 4);
        assert!(hdr_for_region(&sdr, &scene, sdr_region, HdrDecision::default()).is_none());
        // The switches still reach it: one pixel is enough with the area test
        // off, and the output's own declaration is the whole answer when the
        // ratio is zero.
        assert!(hdr_for_region(
            &sdr,
            &scene,
            sdr_region,
            HdrDecision::from_config(true, 0.0)
        )
        .is_some());
        // And a frame that really does carry highlights is kept either way.
        let bright = vec![hdr_output_half("DP-2", 32, 32, 4.0)];
        assert!(hdr_for_region(&bright, &scene, sdr_region, HdrDecision::default()).is_some());
    }

    #[test]
    fn a_region_across_the_seam_has_no_single_hdr_output() {
        // Straddling the boundary between the two screens, there is no one
        // 10-bit buffer to read: the SDR scene is the only picture of it.
        let (scene, hdr) = hdr_scene();
        assert!(
            hdr_for_region(&hdr, &scene, Rect::new(63, 3, 5, 4), HdrDecision::default()).is_none()
        );
    }

    #[test]
    fn a_screen_that_offered_no_hdr_leaves_its_region_without_one() {
        let (scene, _) = hdr_scene();
        // Only the dense screen produced an HDR half this time.
        let hdr = vec![hdr_output_half("DP-2", 32, 32, 4.0)];
        assert!(
            hdr_for_region(&hdr, &scene, Rect::new(2, 3, 5, 4), HdrDecision::default()).is_none()
        );
        // The one that did still answers for its own screen.
        assert!(
            hdr_for_region(&hdr, &scene, Rect::new(66, 3, 5, 4), HdrDecision::default()).is_some()
        );
    }

    #[test]
    fn hdr_for_name_finds_only_a_named_output() {
        let (scene, _) = hdr_scene();
        let hdr = vec![hdr_output_half("DP-2", 32, 32, 4.0)];
        let decision = HdrDecision::default();
        assert_eq!(
            hdr_for_name(&hdr, &scene, "DP-2", None, decision)
                .unwrap()
                .frame
                .size(),
            Size::new(32, 32)
        );
        assert!(hdr_for_name(&hdr, &scene, "eDP-1", None, decision).is_none());
        // The whole output is tested too, so a named output holding nothing
        // above SDR white answers `None` rather than handing the backdrop an
        // image the compositor would have drawn differently.
        let sdr = vec![hdr_output_half("DP-2", 32, 32, 1.0)];
        assert!(hdr_for_name(&sdr, &scene, "DP-2", None, decision).is_none());
    }

    #[test]
    fn an_hdr_capture_tone_maps_its_own_sdr_half() {
        // The SDR PNG is vshot's own tone map of the HDR half, not a second
        // capture.  It cannot be the compositor's own rendition: that is written
        // against a peak of 80 cd/m2 while SDR white is the output's own
        // reference (203 here), so it puts ordinary SDR content below white —
        // taking it would dim the whole capture, not just the highlights.
        //
        // The peak lands on white; the mid channel is rolled off well below it,
        // and red stays the dominant channel — a real tone map, not the second
        // capture.
        let hdr = HdrFrame::new(
            Size::new(2, 1),
            vec![[4.0, 1.0, 1.0, 1.0], [1.0, 1.0, 1.0, 1.0]],
        )
        .unwrap();
        let (sdr, kept) = sdr_and_hdr(
            Some(&EditPipeline::new()),
            plain_frame(),
            Some(HdrHalf {
                frame: hdr,
                reference_nits: REFERENCE_WHITE_NITS,
            }),
            ToneMapOptions::default(),
        )
        .unwrap();
        let pixel = sdr.pixel(Point::new(0, 0)).unwrap();
        assert!(pixel[0] > 240, "the brightest channel did not near white");
        assert_eq!(pixel[1], pixel[2]);
        assert!((128..=200).contains(&pixel[1]), "green = {}", pixel[1]);
        let kept = kept.expect("the HDR half is kept");
        assert_eq!(kept.frame.pixel(0, 0), Some([4.0, 1.0, 1.0, 1.0]));
    }

    #[test]
    fn a_ten_bit_buffer_without_hdr_light_is_not_a_pair() {
        // A 10-bit buffer whose brightest pixel is only SDR white carries no
        // HDR content, so the ordinary frame is written alone: no phantom
        // `.hdr` appears beside a screenshot that never held any highlight.
        let flat = HdrFrame::new(Size::new(2, 1), vec![[1.0, 1.0, 1.0, 1.0]; 2]).unwrap();
        let (sdr, kept) = sdr_and_hdr(
            Some(&EditPipeline::new()),
            plain_frame(),
            Some(HdrHalf {
                frame: flat,
                reference_nits: REFERENCE_WHITE_NITS,
            }),
            ToneMapOptions::default(),
        )
        .unwrap();
        assert_eq!(sdr.pixel(Point::new(0, 0)), Some([0, 255, 0, 255]));
        assert!(kept.is_none());
    }

    #[test]
    fn a_capture_without_hdr_uses_the_compositors_frame_alone() {
        let (sdr, kept) = sdr_and_hdr(
            Some(&EditPipeline::new()),
            plain_frame(),
            None,
            ToneMapOptions::default(),
        )
        .unwrap();
        assert_eq!(sdr.pixel(Point::new(0, 0)), Some([0, 255, 0, 255]));
        assert!(kept.is_none());
    }

    /// A frame the tests can recognise by value, standing in for the compositor's
    /// own 8-bit capture.
    fn plain_frame() -> Frame {
        Frame::solid(Size::new(2, 1), [0, 255, 0, 255]).unwrap()
    }

    #[test]
    fn only_targets_ending_in_one_output_keep_an_hdr_half() {
        // The HDR half must be one output's own 10-bit buffer, so a composed
        // desktop, a reconstructed window and a scrolling stitch — none of
        // which has a single such buffer behind it — keep none.  `window pick`
        // is a refuser here because it takes its HDR half from its own second
        // capture, after the click picked the window.
        let window = CaptureTarget::ActiveWindow {
            pixel_detect: false,
            no_blend: false,
        };
        let keepers = [
            CaptureTarget::RegionFixed(Rect::new(0, 0, 4, 4)),
            CaptureTarget::RegionInteractive,
            CaptureTarget::Monitor("DP-1".into()),
            window,
        ];
        for target in &keepers {
            assert!(
                target_wants_hdr(target),
                "{target:?} should keep an HDR half"
            );
        }

        let refusers = [
            CaptureTarget::All,
            CaptureTarget::WindowPick {
                pixel_detect: false,
                no_blend: false,
            },
            CaptureTarget::LongShot {
                region: None,
                options: crate::longshot::LongShotOptions::default(),
                inject: crate::inject::Prefer::Auto,
            },
        ];
        for target in &refusers {
            assert!(!target_wants_hdr(target), "{target:?} should not");
        }
    }

    /// A release that did not press Pin hands nothing over, and the caller has
    /// to read that as "the capture is still to be written".
    ///
    /// `pin_from_editor` answers `false` for an ordinary OK, which is what tells
    /// the caller apart from the one release that really did put the image on
    /// the screen.  Reading any non-error release as a handoff skipped the write
    /// entirely: an interactive region capture through the editor exited 0 and
    /// saved nothing at all.
    #[test]
    fn a_release_without_the_pin_button_leaves_the_capture_to_be_written() {
        let (scene, hdr_outputs) = hdr_scene();
        let request = cli::Request {
            target: CaptureTarget::RegionInteractive,
            destination: cli::Destination::File(PathBuf::from("/dev/null")),
            cursor: false,
            compression: crate::model::PngCompression::default(),
            element_fallback: crate::element::Fallback::Lines,
            hdr_format: crate::output::HdrFormat::default(),
            tone_map: crate::model::hdr::ToneMapOptions::default(),
        };
        // The editor's ordinary answer: a selection, no `pin` flag.  The
        // composite is absent because this session rendered nothing, which is
        // the shape a cancelled render has too -- the difference is the status.
        let answer = br#"{"status":"ok","selection":{"x":0,"y":0,"width":8,"height":8}}"#;
        assert!(
            !pin_from_editor(&scene, &hdr_outputs, &request, answer, None).unwrap(),
            "an OK without Pin must leave the capture for the destination"
        );
    }
}

#[cfg(test)]
mod pixel_probe {
    use super::*;

    /// Every window `hyprctl clients` reports, as a candidate the detector can
    /// be pointed at.  Deliberately not the provider: that one is filtered to
    /// the visible workspace, and the point here is to look at any window.
    fn compositor_windows_for_probe() -> Vec<capture::WindowCandidate> {
        let output = match std::process::Command::new("hyprctl")
            .args(["clients", "-j"])
            .output()
        {
            Ok(output) => output,
            Err(_) => return Vec::new(),
        };
        let clients = match serde_json::from_slice::<serde_json::Value>(&output.stdout)
            .ok()
            .and_then(|value| value.as_array().cloned())
        {
            Some(clients) => clients,
            None => return Vec::new(),
        };
        clients
            .iter()
            .filter_map(|client| {
                let at = client.get("at")?.as_array()?;
                let size = client.get("size")?.as_array()?;
                let class = client.get("class").and_then(|v| v.as_str()).unwrap_or("");
                let title = client.get("title").and_then(|v| v.as_str()).unwrap_or("");
                Some(capture::WindowCandidate {
                    geometry: Rect::new(
                        i32::try_from(at.first()?.as_i64()?).ok()?,
                        i32::try_from(at.get(1)?.as_i64()?).ok()?,
                        u32::try_from(size.first()?.as_u64()?).ok()?,
                        u32::try_from(size.get(1)?.as_u64()?).ok()?,
                    ),
                    label: format!("{class} — {title}"),
                    app_id: class.to_string(),
                    title: title.to_string(),
                    pid: client.get("pid").and_then(|v| v.as_i64()).unwrap_or(0) as i32,
                    handle: None,
                })
            })
            .collect()
    }

    /// Runs the element detector against the live desktop, for tuning it.
    ///
    /// `cargo test -- --ignored pixel_probe --nocapture` with `VSHOT_PIXEL_DEBUG=1`
    /// prints what the detector found inside every window the compositor lists.
    /// It captures the screen, so it is ignored by default and belongs to the
    /// same family as the other live-desktop checks.
    #[test]
    #[ignore = "captures the live desktop"]
    fn the_live_desktop_through_the_element_detector() {
        let mut wayland = match WaylandSession::connect() {
            Ok(wayland) => wayland,
            Err(error) => {
                println!("no wayland connection: {error}");
                return;
            }
        };
        let output_infos = match wayland.output_infos() {
            Ok(output_infos) => output_infos,
            Err(error) => {
                println!("no topology: {error}");
                return;
            }
        };
        let mut capture = match Capturer::connect() {
            Ok(capture) => capture,
            Err(error) => {
                println!("no capture: {error}");
                return;
            }
        };
        let scene = capture_scene(&mut capture, &output_infos, false).expect("scene");
        // Every window the compositor knows, not only the ones on the visible
        // workspace: the detector is what is being looked at here, and it does
        // not care which workspace a window is on.
        let windows = compositor_windows_for_probe();
        println!("\n{} window(s) on the live desktop", windows.len());
        // Dump each window as a PNG beside the analysis, so what the detector
        // sees can be looked at rather than guessed at.
        let dump = std::env::var_os("VSHOT_PIXEL_DUMP").map(std::path::PathBuf::from);
        for window in &windows {
            if let Some(directory) = &dump {
                let _ = std::fs::create_dir_all(directory);
                if let Ok(crop) = scene.crop(window.geometry) {
                    if let Ok(png) = crop.encode_png(None, model::PngCompression::default()) {
                        let name = format!(
                            "{}-{}.png",
                            window.app_id.replace('/', "_"),
                            window.geometry.size.width
                        );
                        let _ = std::fs::write(directory.join(name), png);
                    }
                }
            }
            let request = element::ElementRequest {
                window,
                scene: &scene,
                fallback: crate::element::Fallback::Lines,
            };
            let elements = element::elements_of(&request);
            match elements {
                Some(nodes) => {
                    fn count(node: &selection_region::RegionNode) -> usize {
                        1 + node.children.iter().map(count).sum::<usize>()
                    }
                    let total: usize = nodes.iter().map(count).sum();
                    println!(
                        "\n{:?}: {} top-level, {} total",
                        window.label,
                        nodes.len(),
                        total
                    );
                    fn dump(node: &selection_region::RegionNode, depth: usize) {
                        if depth > 4 {
                            return;
                        }
                        println!(
                            "{}  {}x{}+{}+{}",
                            "  ".repeat(depth),
                            node.rect.size.width,
                            node.rect.size.height,
                            node.rect.left(),
                            node.rect.top()
                        );
                        for child in &node.children {
                            dump(child, depth + 1);
                        }
                    }
                    for node in nodes.iter().take(6) {
                        dump(node, 0);
                    }
                }
                None => println!("\n{:?}: no elements", window.label),
            }
        }
    }
}

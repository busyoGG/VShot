// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Recording one window's own pixels on a Plasma/KWin session:
//! `vshot record window` where the wlroots route cannot work.
//!
//! The wlroots route ([`super::window`]) is built on two staging protocols —
//! `ext_foreign_toplevel_list_v1` to name the windows, `ext_image_copy_capture_v1`
//! to copy one of them — and a Plasma session speaks neither.  What it does
//! speak is `org.kde.KWin.ScreenShot2`, whose `CaptureWindow(handle, …)` takes
//! a window by its internal `QUuid` and renders that window itself.  This
//! module is that route: the same shape of loop, the same encoder, the same
//! soundtrack, over KWin's own D-Bus call instead of the two protocols.
//!
//! Three things are different from the wlroots loop, and each is why this is
//! its own module rather than a branch inside [`super::window`]:
//!
//! * **The frames are CPU pixels.**  ScreenShot2 writes premultiplied BGRA
//!   through a pipe; there is no dma-buf and no zero-copy chain.  The recorder
//!   is opened for the software (packed RGBA) path, which is the compatibility
//!   path the encoder already has.
//!
//! * **The call is pull, not push.**  Every capture is one D-Bus round trip
//!   plus a pipe full of pixels, so the loop samples at the requested rate
//!   rather than waiting for the compositor to say something changed.  A
//!   monitor recording samples the same way.
//!
//! * **The window is named by a handle, not a protocol object.**  The handle
//!   comes from KWin's scripting probe (`internalId`), which is also where the
//!   class, title and pid come from — so `--app-audio` and `--follow` work the
//!   same way they do on the other compositors, matched on the same two
//!   strings.

use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use crate::capture::window::{
    kwin_active_row, kwin_rows, CompositorWindowProvider, KwinRow, ProcessWindowProvider,
    WindowCandidate,
};
use crate::capture::window_copy::{Follow, Name};
use crate::error::{Result, VshotError};
use crate::geometry::Size;
use crate::model::Frame;

use super::avcodec::{AudioSink, Recorder, VideoSink};
use super::{debug_enabled, prepare_output_path, RecordRequest, WindowTarget};

/// How often the focused window is asked for while a `--follow` recording
/// runs.  Same quarter second as the wlroots loop, and for the same reason:
/// the question is a process — the scripting probe — so it is not something to
/// pay at the frame rate.
const FOCUS_POLL: Duration = Duration::from_millis(250);

/// How many frames in a row the capture may fail before the recording gives
/// up.  A hiccup — a window mid-resize, a compositor busy redrawing it — is
/// dropped; a run of them is a session that has stopped working.
const MAX_CONSECUTIVE_ERRORS: u32 = 10;

/// The canvas a KWin window recording is opened for: the window's own pixel
/// size, rounded up to even.
///
/// The hardware encoders work in NV12, whose chroma plane is half-resolution
/// in both axes, so a frame with an odd width or height has no NV12 form — an
/// odd-sized canvas crashes inside libavutil's conversion rather than failing
/// an encode.  The wlroots route never meets this because a compositor's
/// buffer constraints are even by construction; `ScreenShot2` hands back the
/// window's *client* geometry, which is not.  An odd window is one pixel wider
/// and taller here than the window itself, and the fit path pads the extra
/// column and row, so the window still lands in the file whole.
pub(super) fn even_canvas(size: Size) -> (u32, u32) {
    let width = if size.width.is_multiple_of(2) {
        size.width
    } else {
        size.width + 1
    };
    let height = if size.height.is_multiple_of(2) {
        size.height
    } else {
        size.height + 1
    };
    (width, height)
}

/// Runs a KWin window recording to completion.
pub(super) fn run(request: &RecordRequest, target: &WindowTarget) -> Result<std::path::PathBuf> {
    let (mut capture, mut window, follow, first) = open_window_capture(request, target)?;
    let (width, height) = even_canvas(first.size());

    // --- the soundtrack ---------------------------------------------------
    let app_audio = if request.app_audio {
        super::open_app_audio(&window.as_name())?
    } else {
        None
    };
    let mic = super::open_soundtrack_with(request.mic.as_ref(), app_audio)?;
    let mic_format = mic.format();

    let path = prepare_output_path(request)?;
    let backend = request.encoder_backend.resolve();
    let mut recorder = match mic_format {
        Some(format) => {
            Recorder::start_mic(&path, width, height, request.encoder, format, backend)?
        }
        None => Recorder::start(&path, width, height, request.encoder, backend)?,
    };
    if debug_enabled() {
        eprintln!(
            "vshot: recording {width}x{height} at {} fps with {} ({}) through libavcodec {} \
             and libavformat's MP4 muxer, over KWin's ScreenShot2",
            request.fps,
            request.encoder.word(),
            backend.word(),
            super::avcodec::libavcodec_version()
        );
    }

    let interrupted = super::install_stop_handler()?;
    super::write_pid_file()?;
    let mut mic = mic;
    mic.arm();
    let outcome = loop_over(
        &mut capture,
        &mut recorder,
        request,
        &follow,
        &mut window,
        first,
        &mut mic,
        &interrupted,
        |_sink| Ok(false),
    );
    let _ = std::fs::remove_file(super::pid_file());

    let finish = recorder.finish();
    if let Err(error) = finish {
        if outcome.is_ok() {
            return Err(error);
        }
        eprintln!("vshot: the recording could not be finished: {error}");
    }
    super::report_outcome(
        outcome.map(|()| (recorder.frames() as usize, recorder.seconds())),
        &path,
    )
}

/// Connects, resolves which window to capture and takes its first frame: the
/// shared head of a KWin window session, so a replay's ring gets the same
/// window and the same starting shape a recording does.
///
/// Returns the connected capture, the window it is aimed at, the `--follow`
/// set, and that first frame — whose size is the size the encoder (or the
/// ring) has to be opened for.  The window is captured once here because the
/// canvas comes from the window's own pixels, which only a capture reports.
pub(super) fn open_window_capture(
    request: &RecordRequest,
    target: &WindowTarget,
) -> Result<(crate::capture::Capturer, Window, Follow, Frame)> {
    let mut capture = crate::capture::Capturer::connect()?;
    if !matches!(capture, crate::capture::Capturer::Kwin(_)) {
        return Err(VshotError::Recording(
            "the KWin window route needs KWin's ScreenShot2 service, and this session's \
             capture backend is not KWin's"
                .into(),
        ));
    }
    let rows = kwin_rows(&crate::capture::window::ProcessWindowRunner)?;
    if rows.is_empty() {
        return Err(VshotError::Recording(
            "KWin lists no windows to record".into(),
        ));
    }
    let follow = Follow::new(request.follow.clone());
    let selected = resolve_start(target, &follow, &rows)?;
    let window = Window {
        handle: rows[selected].handle.clone(),
        app_id: rows[selected].app_id.clone(),
        title: rows[selected].title.clone(),
    };
    if window.handle.is_empty() {
        return Err(VshotError::Recording(
            "KWin reported no window id for the window to record, so it cannot be aimed at; \
             this KWin is too old for `CaptureWindow`"
                .into(),
        ));
    }
    if debug_enabled() {
        eprintln!(
            "vshot: recording the window `{}` (app_id `{}`, title `{}`, handle {})",
            window.label(),
            window.app_id,
            window.title,
            window.handle
        );
    }
    let first = capture_window(&mut capture, &window, request.cursor)?;
    Ok((capture, window, follow, first))
}

/// The window a KWin recording is of.  The handle is the one KWin's
/// `CaptureWindow` takes; the labels are what `--follow`, `--app-audio` and
/// the messages match on.
#[derive(Clone, Debug)]
pub(super) struct Window {
    handle: String,
    app_id: String,
    title: String,
}

impl Window {
    fn label(&self) -> String {
        crate::capture::window::join_label(&self.app_id, &self.title)
    }

    /// The window as the toplevel-style [`Name`] the shared audio and follow
    /// code speaks: the same app id and title, no protocol identifier.
    pub(super) fn as_name(&self) -> Name {
        Name {
            app_id: self.app_id.clone(),
            title: self.title.clone(),
            identifier: String::new(),
        }
    }
}

/// Which window a KWin window recording starts on, from its target and its
/// `--follow` list.  Mirrors [`super::window::resolve_start`]: with `--follow`
/// the focus decides, so the target is not resolved at all.
fn resolve_start(target: &WindowTarget, follow: &Follow, rows: &[KwinRow]) -> Result<usize> {
    if follow.is_empty() {
        return resolve_window(target, rows);
    }
    let focused =
        kwin_active_row(&crate::capture::window::ProcessWindowRunner)?.ok_or_else(|| {
            VshotError::Recording(
                "`--follow` needs KWin to say which window has the focus, and it cannot".into(),
            )
        })?;
    let names: Vec<Name> = rows.iter().map(name_of).collect();
    let borrowed: Vec<&Name> = names.iter().collect();
    follow.start(&borrowed, Some((&focused.app_id, &focused.title)))
}

/// Which row a target names.
fn resolve_window(target: &WindowTarget, rows: &[KwinRow]) -> Result<usize> {
    let names: Vec<Name> = rows.iter().map(name_of).collect();
    let borrowed: Vec<&Name> = names.iter().collect();
    match target {
        WindowTarget::Filter(filter) => crate::capture::window_copy::select(&borrowed, filter),
        WindowTarget::Active => {
            let focused = kwin_active_row(&crate::capture::window::ProcessWindowRunner)?
                .ok_or_else(|| VshotError::Recording("KWin reports no focused window".into()))?;
            crate::capture::window_copy::match_description(
                &borrowed,
                &focused.app_id,
                &focused.title,
            )
        }
        WindowTarget::Pick => pick(rows),
    }
}

fn name_of(row: &KwinRow) -> Name {
    Name {
        app_id: row.app_id.clone(),
        title: row.title.clone(),
        identifier: String::new(),
    }
}

/// Asks the user which window to record, through the same picker the wlroots
/// route shows.  The candidates are KWin's own window list — which now carries
/// class, title and handle — and the click is resolved against that list, so
/// the picked window is the one the capture is aimed at by its handle.
fn pick(rows: &[KwinRow]) -> Result<usize> {
    let candidates: Vec<WindowCandidate> = rows
        .iter()
        .map(|row| WindowCandidate {
            geometry: row.rect(),
            label: crate::capture::window::join_label(&row.app_id, &row.title),
            app_id: row.app_id.clone(),
            title: row.title.clone(),
            handle: (!row.handle.is_empty()).then(|| row.handle.clone()),
        })
        .collect();
    let scene = super::window::picker_scene_standalone()?;
    let picked = crate::qt_overlay::pick_window(&scene, &candidates, || {
        ProcessWindowProvider.windows().ok()
    })?;
    let handle = picked
        .point
        .and_then(|point| ProcessWindowProvider.candidate_at(point))
        .and_then(|candidate| candidate.handle)
        .or_else(|| {
            candidates
                .iter()
                .find(|candidate| candidate.geometry == picked.rect)
                .and_then(|candidate| candidate.handle.clone())
        })
        .ok_or_else(|| {
            VshotError::Recording("the picked window is no longer in KWin's window list".into())
        })?;
    rows.iter()
        .position(|row| row.handle == handle)
        .ok_or_else(|| {
            VshotError::Recording("the picked window is no longer in KWin's window list".into())
        })
}

/// One capture of the window, as a software frame: KWin renders into a pipe
/// and the pixels arrive premultiplied BGRA, converted to RGBA where they
/// land.
fn capture_window(
    capture: &mut crate::capture::Capturer,
    window: &Window,
    cursor: bool,
) -> Result<Frame> {
    match capture.capture_window(&window.handle, cursor) {
        Some(result) => result.map(|(frame, _scale)| frame),
        None => Err(VshotError::Recording(
            "this session's capture backend cannot capture a window by name".into(),
        )),
    }
}

/// The frame loop.  Samples the window at the requested rate and follows the
/// focus between the `--follow` windows, exactly as the wlroots loop does; the
/// only difference is where the pixels come from.  Generic over the sinks so a
/// replay's ring drives it the same way a recording's file does, and taking
/// the same per-frame `control` hook so a replay can serve its control socket
/// at a frame boundary.
#[allow(clippy::too_many_arguments)] // the session's own shape
pub(super) fn loop_over<S: VideoSink + AudioSink>(
    capture: &mut crate::capture::Capturer,
    recorder: &mut S,
    request: &RecordRequest,
    follow: &Follow,
    window: &mut Window,
    first: Frame,
    mic: &mut super::pipewire_audio::Soundtrack,
    interrupted: &AtomicBool,
    mut control: impl FnMut(&mut S) -> Result<bool>,
) -> Result<()> {
    let started = Instant::now();
    let interval = request.frame_interval();
    let mut next_frame_at = started;
    let mut next_focus_check = (!follow.is_empty()).then(|| started + FOCUS_POLL);
    let mut timeline_ms = 0u64;
    let mut covered_us = 0u64;
    let mut last_frame_at = started;
    let mut consecutive_errors = 0u32;
    // The last frame encoded, kept so its tail interval can be sent as its
    // duration when the loop stops (see the end of the loop).
    let mut last_frame: Option<Frame> = None;
    // The input size the encoder is currently fitted from.  The canvas never
    // changes, but the window's own size can, and `resize_fit` is only worth
    // calling when the input size actually changed — calling it every frame
    // would spam the log for a window that is simply a different size than the
    // file.  It starts at zero so the very first frame is fitted: the canvas
    // is [`even_canvas`], which differs from the window whenever one of its
    // sides is odd.
    let mut fitted_to = (0u32, 0u32);
    let mut pending = Some(first);
    // The first frame covers the time from the start of the recording to its
    // arrival, so it is the one frame measured from `started`.
    let mut delivered_any = false;

    // A capture the compositor *refused* is retried rather than counted as a
    // dropped frame — see [`super::Refusals`].  KWin's refusal is a permission
    // check that a Plasma session can fail transiently even for a client it
    // just authorized.
    let mut refusals = super::Refusals::default();
    loop {
        if interrupted.load(Ordering::Relaxed) {
            break;
        }
        // A control line (a replay's save/stop) is served at a frame boundary,
        // so the session's state is consistent when it is handled.
        if control(recorder)? {
            break;
        }
        if let Some(deadline) = next_focus_check {
            if Instant::now() >= deadline {
                next_focus_check = Some(Instant::now() + FOCUS_POLL);
                if let Some(frame) = follow_focus(capture, recorder, request, follow, window, mic) {
                    // The switch already captured the new window's first
                    // frame; using it here saves a round trip and means the
                    // switch shows up in the file at once.
                    pending = Some(frame);
                    fitted_to = (0, 0);
                }
            }
        }
        if let Some(seconds) = request.duration {
            if started.elapsed() >= Duration::from_secs(seconds) {
                break;
            }
        }
        let now = Instant::now();
        if now < next_frame_at {
            super::sleep_interruptible(next_frame_at - now, interrupted);
            continue;
        }
        let frame = match pending.take() {
            Some(frame) => frame,
            None => match capture_window(capture, window, request.cursor) {
                Ok(frame) => frame,
                // The window went away: the file is finished properly and
                // this is why it ends where it does.
                Err(VshotError::WindowClosed(reason)) => {
                    eprintln!("vshot: the recording ended: {reason}");
                    break;
                }
                // A refusal is KWin declining to be asked, which comes and goes
                // with its desktop-file database; anything else is a hiccup.
                Err(VshotError::ScreenshotDenied(explanation)) => {
                    if !refusals.refused(&explanation) {
                        return Err(VshotError::ScreenshotDenied(explanation));
                    }
                    // The soundtrack keeps up even while the picture does not:
                    // a refusal is forgiven for ten seconds, and the audio ring
                    // behind the loop holds four, so a run of them would leave
                    // the file's audio behind its picture.
                    super::pump_soundtrack(mic, recorder)?;
                    // A refused or failed frame is not a hole in the timeline:
                    // the interval it covered belongs to the frame before it,
                    // which was still on screen (see `cast::cover`).
                    next_frame_at = Instant::now() + interval;
                    continue;
                }
                Err(error) => {
                    consecutive_errors += 1;
                    if consecutive_errors >= MAX_CONSECUTIVE_ERRORS {
                        return Err(VshotError::Recording(format!(
                            "giving up after {consecutive_errors} frames in a row failed: {error}"
                        )));
                    }
                    eprintln!("vshot: dropping a frame: {error}");
                    // A run of dropped frames is a picture that is not moving,
                    // which is exactly when the audio ring behind it fills up.
                    super::pump_soundtrack(mic, recorder)?;
                    // A refused or failed frame is not a hole in the timeline:
                    // the interval it covered belongs to the frame before it,
                    // which was still on screen (see `cast::cover`).
                    next_frame_at = Instant::now() + interval;
                    continue;
                }
            },
        };
        consecutive_errors = 0;
        refusals.delivered();
        let now = Instant::now();
        if !delivered_any {
            last_frame_at = started;
        }
        delivered_any = true;
        let duration_ms = {
            covered_us += now.saturating_duration_since(last_frame_at).as_micros() as u64;
            last_frame_at = now;
            let due_ms = covered_us / 1000;
            let step = due_ms.saturating_sub(timeline_ms);
            timeline_ms = timeline_ms.max(due_ms);
            u32::try_from(step).unwrap_or(1).max(1)
        };
        let encode_started = Instant::now();
        // The window can change size mid-recording; one MP4 holds one frame
        // size, so the new size is fitted into the canvas the file was opened
        // with.  `resize_fit` records the input size the software path fits
        // from, and the next frame is fitted into the same canvas.
        let this_size = (frame.size().width, frame.size().height);
        if this_size != fitted_to {
            recorder.resize_fit(this_size.0, this_size.1, 0)?;
            let (canvas_width, canvas_height) = VideoSink::canvas(recorder);
            eprintln!(
                "vshot: the window is now {}x{}; fitting it into the recording's {canvas_width}x\
                 {canvas_height} canvas",
                this_size.0, this_size.1,
            );
            fitted_to = this_size;
        }
        recorder.frame_rgba(frame.pixels(), duration_ms)?;
        if debug_enabled() {
            eprintln!(
                "vshot: encoded {}x{} in {:.1}ms (on screen {duration_ms}ms)",
                frame.size().width,
                frame.size().height,
                encode_started.elapsed().as_secs_f64() * 1000.0
            );
        }
        last_frame = Some(frame);
        super::pump_soundtrack(mic, recorder)?;
        next_frame_at = now + interval;
    }

    // The last interval — the time the last frame was on screen before the
    // stop signal — has not been written yet, because a frame's duration is
    // only known once the *next* frame arrives.  Re-sending the last frame
    // with that interval as its duration is what puts that time in the file,
    // exactly as the wlroots loop does.  The soundtrack of the tail is queued
    // before the frame goes out again: `finish` flushes the audio encoder
    // after the video's, so the samples have to be in by then.
    if !mic.is_empty() {
        super::pump_soundtrack(mic, recorder)?;
    }
    if let Some(frame) = last_frame {
        let now = Instant::now();
        covered_us += now.saturating_duration_since(last_frame_at).as_micros() as u64;
        let due_ms = covered_us / 1000;
        let step = due_ms.saturating_sub(timeline_ms).max(1);
        if let Ok(step) = u32::try_from(step) {
            recorder.frame_rgba(frame.pixels(), step)?;
        }
    }
    Ok(())
}

/// Moves the capture to the followed window the focus just landed on, if any.
/// A failed switch leaves the recording on the window it was already on.
///
/// On a successful switch the new window's first frame is returned, so the
/// loop can use it instead of spending another D-Bus round trip: the switch
/// shows up in the file immediately, and the frame is fitted into the file's
/// canvas by the loop, exactly like any other frame of a window that changed
/// size.
fn follow_focus<S: VideoSink + AudioSink>(
    capture: &mut crate::capture::Capturer,
    recorder: &mut S,
    request: &RecordRequest,
    follow: &Follow,
    window: &mut Window,
    mic: &mut super::pipewire_audio::Soundtrack,
) -> Option<Frame> {
    let focused = match kwin_active_row(&crate::capture::window::ProcessWindowRunner) {
        Ok(Some(row)) => row,
        Ok(None) => return None,
        Err(error) => {
            if debug_enabled() {
                eprintln!("vshot: the focused window could not be read ({error}); staying put");
            }
            return None;
        }
    };
    let rows = match kwin_rows(&crate::capture::window::ProcessWindowRunner) {
        Ok(rows) => rows,
        Err(error) => {
            eprintln!("vshot: KWin's window list could not be re-read ({error}); staying put");
            return None;
        }
    };
    let names: Vec<Name> = rows.iter().map(name_of).collect();
    let borrowed: Vec<&Name> = names.iter().collect();
    let index = follow.retarget(
        &borrowed,
        &focused.app_id,
        &focused.title,
        &window.as_name(),
    )?;
    let row = &rows[index];
    if row.handle.is_empty() || row.handle == window.handle {
        return None;
    }
    let target = Window {
        handle: row.handle.clone(),
        app_id: row.app_id.clone(),
        title: row.title.clone(),
    };
    // The switch is a capture on the new handle; the file was already opened
    // for a canvas, and the new window's frames are fitted into it on the next
    // encode.
    match capture_window(capture, &target, request.cursor) {
        Ok(frame) => {
            let label = target.label();
            *window = target;
            eprintln!("vshot: the focus moved to `{label}`; following it");
            if request.app_audio {
                match super::app_audio_node(&window.as_name()) {
                    Ok(Some(node)) => match super::pipewire_audio::Mic::open(Some(&node)) {
                        Ok(opened) => {
                            if let Err(error) = mic.set_app(Some(opened), recorder) {
                                eprintln!(
                                    "vshot: the new window's audio could not be mixed in: {error}"
                                );
                            }
                        }
                        Err(error) => {
                            eprintln!("vshot: the new window's audio could not be opened: {error}")
                        }
                    },
                    Ok(None) => {}
                    Err(error) => {
                        eprintln!("vshot: the new window's audio could not be found: {error}")
                    }
                }
            }
            Some(frame)
        }
        Err(error) => {
            eprintln!(
                "vshot: the switch to `{}` failed ({error}); staying on `{}`",
                target.label(),
                window.label()
            );
            None
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::capture::window::KwinRow;

    fn row(x: i32, y: i32, w: u32, h: u32, app_id: &str, title: &str, handle: &str) -> KwinRow {
        KwinRow {
            x,
            y,
            width: w,
            height: h,
            pid: 100,
            app_id: app_id.to_owned(),
            title: title.to_owned(),
            handle: handle.to_owned(),
        }
    }

    /// The canvas is the window's size, rounded up to even on each side that
    /// needs it.  NV12 has no odd form, and `ScreenShot2` hands back the
    /// window's client geometry, which can be odd — the live KWin check saw a
    /// 941x768 window, whose width is the odd one.
    #[test]
    fn the_canvas_is_rounded_up_to_even() {
        assert_eq!(even_canvas(Size::new(941, 768)), (942, 768));
        assert_eq!(even_canvas(Size::new(940, 769)), (940, 770));
        assert_eq!(even_canvas(Size::new(1920, 1080)), (1920, 1080));
        // A one-pixel window is not a special case: it rounds to two.
        assert_eq!(even_canvas(Size::new(1, 1)), (2, 2));
    }

    #[test]
    fn a_filter_target_picks_the_matching_window() {
        let rows = vec![
            row(
                0,
                0,
                100,
                100,
                "kitty",
                "Hello",
                "{11111111-1111-1111-1111-111111111111}",
            ),
            row(
                0,
                0,
                100,
                100,
                "firefox",
                "News",
                "{22222222-2222-2222-2222-222222222222}",
            ),
        ];
        let index = resolve_window(&WindowTarget::Filter("firefox".into()), &rows).unwrap();
        assert_eq!(index, 1);
        // A substring of either the app id or the title matches too.
        assert_eq!(
            resolve_window(&WindowTarget::Filter("new".into()), &rows).unwrap(),
            1
        );
        // Nothing matches: an error, not a wrong window.
        assert!(resolve_window(&WindowTarget::Filter("nothing".into()), &rows).is_err());
    }

    #[test]
    fn a_window_names_itself_with_its_labels() {
        let window = Window {
            handle: "{11111111-1111-1111-1111-111111111111}".into(),
            app_id: "kitty".into(),
            title: "Hello".into(),
        };
        assert_eq!(window.label(), "kitty — Hello");
        assert_eq!(window.as_name().app_id, "kitty");
        assert_eq!(window.as_name().title, "Hello");
    }
}

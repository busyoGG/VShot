//! Recording one window's own pixels: `vshot record window`.
//!
//! This is a different thing from recording the screen area a window covers,
//! and the difference is the point of the whole module.  A screen recording of
//! the rectangle a window sits in shows whatever is *there*: the window's
//! neighbours wherever they overlap it, nothing at all where the window has
//! been dragged off the screen, and a frozen picture of where the window used
//! to be once it moves.  A window recording asks the compositor for the
//! window's own pixels, so an occluded window records whole, a window that
//! moves records whole, and what is behind it never appears.
//!
//! The route is `ext_image_copy_capture_v1` with a source built from the
//! window's `ext_foreign_toplevel_handle_v1` — the two staging protocols that
//! exist for exactly this.  The frames arrive as dma-bufs the client allocates,
//! which the encoder imports directly (the same zero-copy chain
//! `record monitor` uses, minus the screencopy half).  [`crate::capture::
//! window_copy`] is the protocol client; this module is the recording loop
//! around it.
//!
//! Three things about this protocol shape the loop:
//!
//! * **The compositor decides when to copy.**  After the first frame it may
//!   hold the copy until the window's content changes, so a still window
//!   produces no frames at all.  The loop therefore treats a frame that did not
//!   arrive as [`Capture::Idle`] and keeps going, rather than as a failure: the
//!   request stays in flight and the frame arrives when there is one.  The
//!   recording's timeline is wallclock either way — a still window becomes a
//!   long-duration frame, which is exactly what was on screen.
//!
//! * **A window can change size, a recording cannot.**  The encoder is opened
//!   for one frame size, and the compositor resends its constraints when the
//!   window is resized.  Frames of a new size would be refused mid-file, so the
//!   recording ends cleanly at the resize with a message saying so, and the
//!   file up to that point is complete.
//!
//! * **The window can go away.**  Closing it stops the session, which ends the
//!   recording the same way — the file is finished properly rather than left
//!   without its trailer.

use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use crate::capture::window::{CompositorWindowProvider, ProcessWindowProvider};
use crate::capture::window_copy::{self, Capture, Name, WindowCapture};
use crate::error::{Result, VshotError};

use super::avcodec::Recorder;
use super::{debug_enabled, prepare_output_path, RecordRequest, WindowTarget};

/// How long one wait for a frame lasts before the loop looks around: at the
/// stop signal, at the `--duration` deadline, and at whether a frame is
/// overdue.  A frame that arrives inside the window is used immediately; one
/// that does not stays in flight.
const POLL_WINDOW: Duration = Duration::from_millis(250);

/// How long a session may go without its *first* frame before the recording
/// gives up on it.  A window on a monitor that is off, disabled or
/// disconnected never gets one — its output never commits, and the compositor
/// never copies — and a recording that would otherwise sit silently forever
/// has to say so instead.
const FIRST_FRAME_TIMEOUT: Duration = Duration::from_secs(10);

/// Runs a window recording to completion.
pub(super) fn run(request: &RecordRequest, target: &WindowTarget) -> Result<std::path::PathBuf> {
    // --- which window -----------------------------------------------------
    let mut capture = WindowCapture::connect()?;
    let toplevels = capture.toplevels()?;
    if toplevels.is_empty() {
        return Err(VshotError::Recording(
            "the compositor lists no windows to record".into(),
        ));
    }
    let names: Vec<&Name> = toplevels.iter().map(|toplevel| &toplevel.name).collect();
    let index = match target {
        WindowTarget::Filter(filter) => window_copy::select(&names, filter)?,
        WindowTarget::Active => match ProcessWindowProvider.active_window() {
            Ok(active) => window_copy::match_description(&names, &active.app_id, &active.title)?,
            // The compositor's active-window query is a separate mechanism
            // from the toplevel list, and on some sessions it is not there
            // (KWin's probe reports geometry only).  Picking is the honest
            // answer then: the user knows which window they mean.
            Err(error) => {
                return Err(VshotError::Recording(format!(
                    "the focused window could not be resolved ({error}); name the window \
                     (`vshot record window NAME`) or pick it (`vshot record window --pick`)"
                )))
            }
        },
        WindowTarget::Pick => pick(&names)?,
    };
    let toplevel = &toplevels[index];
    if debug_enabled() {
        eprintln!(
            "vshot: recording the window `{}` (app_id `{}`, title `{}`)",
            toplevel.label(),
            toplevel.name.app_id,
            toplevel.name.title
        );
    }

    // --- the session and its shape ----------------------------------------
    let shape = capture.start(toplevel)?;
    if debug_enabled() {
        eprintln!(
            "vshot: window capture {}x{} (fourcc 0x{:08x})",
            shape.width, shape.height, shape.fourcc
        );
    }

    // --- the output, encoder and muxer ------------------------------------
    // The microphone, when one was asked for: opened before the encoder,
    // because the AAC encoder's rate and channel count come from the
    // negotiation and the MP4's audio stream is declared in its header.
    let mic = super::open_microphone(request)?;
    let path = prepare_output_path(request)?;
    let mut recorder = match &mic {
        Some(mic) => Recorder::start_dmabuf_mic(
            &path,
            shape.width,
            shape.height,
            request.encoder,
            shape.fourcc,
            mic.format(),
        )?,
        None => Recorder::start_dmabuf(
            &path,
            shape.width,
            shape.height,
            request.encoder,
            shape.fourcc,
        )?,
    };
    if debug_enabled() {
        eprintln!(
            "vshot: recording {width}x{height} with {} through libavcodec {version} and \
             libavformat's MP4 muxer",
            request.encoder.word(),
            width = shape.width,
            height = shape.height,
            version = super::avcodec::libavcodec_version()
        );
        if recorder.audio_channels() > 0 {
            eprintln!(
                "vshot: microphone soundtrack: {} Hz, {} channel(s), AAC",
                recorder.audio_rate(),
                recorder.audio_channels()
            );
        }
    }

    let interrupted = super::install_stop_handler()?;
    super::write_pid_file()?;
    // The microphone has been running through the setup; arming keeps the
    // samples from here on, so the soundtrack starts where the recording
    // does.
    if let Some(mic) = &mic {
        mic.arm();
    }
    let outcome = loop_over(
        &mut capture,
        &mut recorder,
        request,
        mic.as_ref(),
        &interrupted,
    );
    let _ = std::fs::remove_file(super::pid_file());

    // The trailer has to go in even when the loop failed: a file that exists
    // is finished properly, and the failure is reported next to it.
    let finish = recorder.finish();
    if let Err(error) = finish {
        if outcome.is_ok() {
            return Err(error);
        }
        eprintln!("vshot: the recording could not be finished: {error}");
    }
    super::report_outcome(
        outcome.map(|_| (recorder.frames() as usize, recorder.seconds())),
        &path,
    )
}

/// The frame loop.  Returns once the recording has run its course; the caller
/// finishes the file either way.
///
/// The loop aims for the requested frame rate, the way the screen loop does,
/// with one difference: this protocol's compositor holds a copy until the
/// window's content changes, so the loop asks and waits rather than samples.
/// A still window therefore yields long-duration frames, and the file plays
/// back at the pace the window actually changed.
fn loop_over(
    capture: &mut WindowCapture,
    recorder: &mut Recorder,
    request: &RecordRequest,
    mic: Option<&super::pipewire_audio::Mic>,
    interrupted: &AtomicBool,
) -> Result<()> {
    let started = Instant::now();
    let interval = request.frame_interval();
    // The microphone's buffer, reused across frames.
    let mut mic_samples: Vec<f32> = Vec::new();
    let mut timeline_ms = 0u64;
    let mut covered_us = 0u64;
    let mut last_frame_at = started;
    // The slot the next frame is wanted in.  A frame that arrives inside its
    // slot keeps it; one that arrives late resets the clock rather than
    // sprinting to catch up, exactly like the screen loop.
    let mut next_frame_at = started;
    // Consecutive frames the compositor refused.  One is a dropped frame; a
    // run of them is a session that has stopped working.
    let mut consecutive_errors = 0u32;

    loop {
        if interrupted.load(Ordering::Relaxed) {
            break;
        }
        if let Some(seconds) = request.duration {
            if started.elapsed() >= Duration::from_secs(seconds) {
                break;
            }
        }
        // The first frame is what says the session works at all: a window
        // whose output never commits produces nothing, and waiting forever
        // would look like a recording that simply never stops.
        if !capture.has_frames() && capture.first_frame_wait() >= FIRST_FRAME_TIMEOUT {
            return Err(VshotError::Recording(format!(
                "the compositor did not send a frame for this window within {} seconds; it may \
                 be on an output that is off, disabled or disconnected",
                FIRST_FRAME_TIMEOUT.as_secs()
            )));
        }
        // Pace: sleep until this frame's slot.  A slot already past means the
        // window is changing faster than the rate asked for, and the next
        // frame goes out immediately.
        let now = Instant::now();
        if now < next_frame_at {
            sleep_interruptible(next_frame_at - now, interrupted);
            continue;
        }
        let remaining = match request.duration {
            Some(seconds) => Duration::from_secs(seconds).saturating_sub(started.elapsed()),
            None => Duration::from_secs(u64::MAX / 2),
        };
        let wait = POLL_WINDOW.min(remaining.max(Duration::from_millis(1)));
        let had_frames = capture.has_frames();
        let frame = match capture.grab(wait, interrupted) {
            Ok(Capture::Frame(frame)) => frame,
            Ok(Capture::Idle) => {
                if debug_enabled() {
                    eprintln!(
                        "vshot: no frame yet ({:.1}s into the recording)",
                        started.elapsed().as_secs_f64()
                    );
                }
                continue;
            }
            Ok(Capture::Interrupted) => break,
            Ok(Capture::Ended(reason)) => {
                // A window that closed or resized ends the recording rather
                // than failing it: what was recorded is real and the file is
                // finished properly.  Say why on stderr, because a recording
                // that stops on its own has to explain itself.
                eprintln!("vshot: the recording ended: {reason}");
                break;
            }
            Err(error) => {
                consecutive_errors += 1;
                if consecutive_errors >= 10 {
                    return Err(VshotError::Recording(format!(
                        "giving up after {consecutive_errors} frames in a row failed: {error}"
                    )));
                }
                eprintln!("vshot: dropping a frame: {error}");
                continue;
            }
        };
        consecutive_errors = 0;
        let now = Instant::now();
        if !had_frames {
            // The first frame covers the time from the start of the recording
            // to its arrival: the window was on screen for all of it.
            last_frame_at = started;
        }
        let duration_ms = {
            covered_us += now.saturating_duration_since(last_frame_at).as_micros() as u64;
            last_frame_at = now;
            let due_ms = covered_us / 1000;
            let step = due_ms.saturating_sub(timeline_ms);
            timeline_ms = timeline_ms.max(due_ms);
            u32::try_from(step).unwrap_or(1).max(1)
        };
        let encode_started = Instant::now();
        recorder.frame_dmabuf(
            frame.fd,
            frame.fourcc,
            frame.modifier,
            frame.offset as i32,
            frame.stride as i32,
            duration_ms,
        )?;
        // The soundtrack for the interval this frame covered.
        if let Some(mic) = mic {
            super::pump_microphone(mic, recorder, &mut mic_samples)?;
        }
        if debug_enabled() {
            eprintln!(
                "vshot: frame {}: {}x{} muxed in {:.1}ms (on screen {duration_ms}ms)",
                recorder.frames(),
                frame.width,
                frame.height,
                encode_started.elapsed().as_secs_f64() * 1000.0
            );
        }
        next_frame_at = now + interval;
    }

    // The last interval — the time the last frame was on screen before the
    // stop signal — has not been written yet, because a frame's duration is
    // only known once the *next* frame arrives.  Re-sending the last frame
    // with that interval as its duration is what puts that time in the file.
    // The soundtrack of the tail interval, queued before the last frame is
    // sent again: `finish` flushes the audio encoder after the video's, so
    // the samples have to be in by then.
    if let Some(mic) = mic {
        super::pump_microphone(mic, recorder, &mut mic_samples)?;
    }
    if let Some(frame) = capture.last_frame() {
        let now = Instant::now();
        covered_us += now.saturating_duration_since(last_frame_at).as_micros() as u64;
        let due_ms = covered_us / 1000;
        let step = due_ms.saturating_sub(timeline_ms).max(1);
        if let Ok(step) = u32::try_from(step) {
            recorder.frame_dmabuf(
                frame.fd,
                frame.fourcc,
                frame.modifier,
                frame.offset as i32,
                frame.stride as i32,
                step,
            )?;
        }
    }
    Ok(())
}

/// `nanosleep` in slices, so a stop signal is noticed within a few
/// milliseconds even for a long interval.  The screen loop has its own copy
/// for the same reason; this one is not shared because that one is private to
/// the module that owns the signal flag's meaning there.
fn sleep_interruptible(duration: Duration, interrupted: &AtomicBool) {
    let deadline = Instant::now() + duration;
    while Instant::now() < deadline {
        if interrupted.load(Ordering::Relaxed) {
            return;
        }
        let remaining = deadline.saturating_duration_since(Instant::now());
        std::thread::sleep(remaining.min(Duration::from_millis(5)));
    }
}

/// Asks the user which window to record, through the same picker `vshot
/// window pick` shows.
///
/// The picker runs on the live desktop and needs the windows' rectangles to
/// highlight, which the compositor's window list has.  What it returns is a
/// click; that click is resolved against the window list again — the desktop
/// can have changed under the picker — and the resulting window is matched
/// into the foreign-toplevel list by its app id and title, which is the one
/// description both lists share.
fn pick(names: &[&Name]) -> Result<usize> {
    let candidates = ProcessWindowProvider.windows().map_err(|error| {
        VshotError::Recording(format!(
            "picking a window needs the compositor's window list, and it could not be read \
             ({error}); name the window instead (`vshot record window NAME`)"
        ))
    })?;
    if candidates.is_empty() {
        return Err(VshotError::Recording(
            "the compositor's window list is empty, so there is nothing to pick".into(),
        ));
    }
    // The picker draws over a frozen picture of the desktop; for picking a
    // window to record, a picture of each output is enough, and it is what
    // `vshot window pick` hands over as well.
    let scene = picker_scene()?;
    let picked = crate::qt_overlay::pick_window(&scene, &candidates, || {
        ProcessWindowProvider.windows().ok()
    })?;
    let candidate = picked
        .point
        .and_then(|point| ProcessWindowProvider.candidate_at(point))
        .or_else(|| {
            candidates
                .iter()
                .find(|candidate| candidate.geometry == picked.rect)
                .cloned()
        })
        .ok_or_else(|| {
            VshotError::Recording(
                "the picked window is no longer in the compositor's window list".into(),
            )
        })?;
    window_copy::match_description(names, &candidate.app_id, &candidate.title)
}

/// The desktop as the picker's backdrop: every output captured once, composed
/// at its logical position.  This is what `vshot window pick` hands over too,
/// and the picker only draws over it — the window that gets recorded is
/// decided by the click, not by the pixels.
fn picker_scene() -> Result<crate::model::SceneSnapshot> {
    let wayland = crate::wayland::WaylandSession::connect()?;
    let topology = wayland.output_infos()?;
    let mut capture = crate::capture::Capturer::connect()?;
    let mut outputs = Vec::with_capacity(topology.len());
    for info in &topology {
        let frame = capture.capture_output(&info.name, false)?;
        outputs.push(crate::model::OutputSnapshot::new(
            info.global_id,
            info.name.clone(),
            info.geometry,
            info.scale,
            frame,
        )?);
    }
    crate::model::SceneSnapshot::from_outputs(outputs)
}

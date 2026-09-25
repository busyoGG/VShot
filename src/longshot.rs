// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Scrolling capture: keep the wheel going, grab the region as it moves, and
//! stitch the frames into one tall image.
//!
//! The page is scrolled as a stream rather than a scroll at a time.  A wheel
//! goes out every `INJECT_INTERVAL` and frames are grabbed on their own, much
//! shorter schedule, without ever waiting for the page to come to rest —
//! waiting is what made the capture stop and start.  Applications animate a
//! wheel event, so what the grabs see is a page in motion: nearly every frame
//! overlaps the previous one by all but a small part, and those small,
//! well-overlapping advances are the ones the stitcher can place reliably.
//!
//! How far one notch moves is the application's business — a terminal moves a
//! few rows, a browser most of a screen — so the wheel is not fitted to a
//! target: the same number of notches goes out at the same rate from the first
//! frame to the last, and the advance the stitcher measures is simply what the
//! page did.  The one correction is downwards: a frame that moved further than
//! the stitcher can measure halves the wheel, because rows that cannot be
//! placed are rows the finished image would have lost.
//!
//! Scrolling stops when several wheels in a row move nothing (the page is at
//! its end), when the height, frame or time limit is reached, or when the user
//! says so — Enter keeps what has been stitched, Esc throws it away.  One
//! unproductive frame is deliberately not enough: a lazily loading page, a page
//! whose animation has not started yet, and a wheel the region never received
//! look just like the end of the content, so vshot keeps going before believing
//! it.

use std::time::{Duration, Instant};

use crate::capture::hypr_cursor;
use crate::capture::Capturer;
use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect, Size};
use crate::inject::Injector;
use crate::model::Frame;
use crate::qt_overlay;
use crate::stitch::{StitchStep, Stitcher};
use crate::wayland::topology::OutputInfo;

/// How long to let the region picker leave the screen before the first frame
/// is grabbed: the compositor unmaps that surface asynchronously, and a frame
/// taken too early would have the picker stitched into it.
const OVERLAY_SETTLE: Duration = Duration::from_millis(200);

/// How often a wheel is sent while the capture runs.  The page keeps moving
/// between two of these, so this is the rate at which the capture travels; how
/// often it looks is [`GRAB_INTERVAL`], which is far shorter.
const INJECT_INTERVAL: Duration = Duration::from_millis(120);

/// Shortest time between two grabs.  A display does not commit frames faster
/// than this, so grabbing quicker would only read the same picture again — and
/// every frame costs a match against what has been stitched so far.
const GRAB_INTERVAL: Duration = Duration::from_millis(20);

/// How many wheels in a row may move nothing before the page counts as
/// finished.  A count rather than a stretch of time on purpose: a frame of a
/// large region costs a good part of a second to match, so a wall-clock window
/// would expire while the page was still getting round to moving.  What the
/// question really is, is "did the wheel do anything", and that is what is
/// counted — several notches' worth, so a stalled lazy-load is not mistaken for
/// the end of the content.
const IDLE_NOTCHES_TO_STOP: u32 = 6;

/// How many frames in a row may be unplaceable — the page moved past everything
/// the stitcher can match, even at one notch at a time — before there is
/// nothing left to try and the capture gives up rather than lose rows.
const DROPPED_STREAK_TO_STOP: u32 = 5;

/// How often the progress is pushed to the hint overlay.
const STATUS_INTERVAL: Duration = Duration::from_millis(200);

/// How long the final state of the hint bar stays up before it is closed.  The
/// reason a capture stopped is how a short result explains itself, and closing
/// the bar in the same breath would hide it.
const FINAL_STATUS_LINGER: Duration = Duration::from_millis(1200);

/// What `vshot long` was asked for.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct LongShotOptions {
    /// Wheel notches sent at a time.
    pub notches: u32,
    /// Hard limit on the stitched image's height, in pixels.
    pub max_height: u32,
    /// Hard limit on how many frames are grabbed.
    pub max_frames: u32,
    /// Hard limit on the whole capture.
    pub timeout: Duration,
    /// Rows at the top of the frame that are not matched: sticky headers.
    pub ignore_top: u32,
}

impl Default for LongShotOptions {
    fn default() -> Self {
        Self {
            notches: 1,
            max_height: 30_000,
            // A stream takes several frames per wheel, so the ceiling has to be
            // well above what one frame per scroll would have needed: this one
            // is the time limit expressed at roughly 50 frames a second.
            max_frames: 6_000,
            timeout: Duration::from_secs(120),
            ignore_top: 0,
        }
    }
}

/// Outcome of a capture, for the caller to report.
pub struct LongShotResult {
    pub frame: Frame,
    /// Device pixels per logical pixel of the capture.
    pub density: u32,
    /// Rows appended after the first frame.  Zero means not one scroll moved
    /// anything, which the caller reports differently from a short capture.
    pub appended: u32,
    /// Name of the output the region was captured from.
    pub output: String,
    /// Frames grabbed, including the initial one.
    pub frames: u32,
    /// Why the loop stopped, for the message the user gets.
    pub stop: StopReason,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StopReason {
    /// Several wheels in a row moved nothing.
    AtTheEnd,
    /// One notch outran what the stitcher can measure, so there is no smaller
    /// scroll left to try.
    TooFast,
    /// The height limit was hit.
    HeightLimit,
    /// The frame limit was hit.
    FrameLimit,
    /// The time limit was hit.
    TimeLimit,
    /// The user pressed Enter.
    UserDone,
    /// The hint overlay went away on its own.
    HintLost,
}

/// Why the loop stopped, as one line: the hint overlay's last update and the
/// report vshot prints when the capture is done both read this.
pub fn stop_reason(reason: StopReason) -> &'static str {
    match reason {
        StopReason::AtTheEnd => "the page ended",
        StopReason::TooFast => "one scroll jumps further than can be measured",
        StopReason::HeightLimit => "height limit reached",
        StopReason::FrameLimit => "frame limit reached",
        StopReason::TimeLimit => "time limit reached",
        StopReason::UserDone => "stopped by the user",
        StopReason::HintLost => "the hint overlay went away",
    }
}

/// Scrolls `region` and stitches what comes out of it.
pub fn run(
    capture: &mut Capturer,
    outputs: &[OutputInfo],
    region: Rect,
    injector: &mut Injector,
    options: &LongShotOptions,
) -> Result<LongShotResult> {
    let output = output_containing(outputs, region)?;
    let output_name = output.name.clone();
    let density = output.scale.max(1);
    // Everything below works in the frames' own pixels, which are the output's
    // pixels: the selected region is a logical rectangle, but what comes back
    // from the compositor is scaled by the output.
    let pixel_width = region.size.width * density;
    let pixel_height = region.size.height * density;
    let mut stitcher = Stitcher::new(
        pixel_width,
        pixel_height,
        options.ignore_top * density,
        options.max_height,
    )?;
    let mut hint = qt_overlay::start_hint_session(region, outputs)?;

    let started = Instant::now();
    let mut notches = i32::try_from(options.notches.max(1)).unwrap_or(i32::MAX);
    let mut frames = 0u32;
    let mut dropped_streak = 0u32;
    let mut last_status = Instant::now() - STATUS_INTERVAL;

    // The wheel goes to whatever the pointer is over, so put it inside the
    // region before the first scroll — and then leave it alone.  The virtual
    // pointer shares the cursor with the real one, so parking it again before
    // every wheel would fight the user for the hint bar's buttons; the price is
    // that moving the mouse away mid-capture scrolls whatever it lands on.
    // With the uinput backend this is a no-op: that one owns a wheel, not a
    // pointer.
    let center = Point::new(
        region.origin.x + (region.size.width / 2) as i32,
        region.origin.y + (region.size.height / 2) as i32,
    );
    // The parking below is a pointer event, and this session's plugin paints the
    // pointer into the frames it composites after one (see `hypr_cursor`), so it
    // is held off until every frame this stitch is made of has been read.
    let _cursors_suspended = hypr_cursor::suspend();
    injector.move_pointer(center)?;

    // The region picker's surface leaves the screen asynchronously; a frame
    // grabbed too early would have the picker stitched into it.
    std::thread::sleep(OVERLAY_SETTLE);

    // The first frame is the top of the page; nothing has scrolled yet.
    let first = grab(capture, output, region)?;
    // A compositor may render at a scale `wl_output` cannot express — 1.5 on a
    // fractional-scale setup — and then the frame is not the one the stitcher
    // was built for.  Saying so here beats a byte count mismatch on the second
    // frame, which would give no hint of what actually went wrong.
    let expected = Size::new(pixel_width, pixel_height);
    if first.size() != expected {
        let actual = first.size();
        let logical = f64::from(region.size.width.max(1));
        return Err(VshotError::LongShotRegion(format!(
            "the compositor returned a {}x{} frame for a {}x{} selection, which is a scale of \
             {:.2}: a scrolling capture needs the output's whole-number scale",
            actual.width,
            actual.height,
            pixel_width,
            pixel_height,
            f64::from(actual.width) / logical,
        )));
    }
    push_frame(&mut stitcher, &first)?;
    frames += 1;

    // Two clocks, deliberately independent: the wheel goes out on its own
    // schedule, and frames come back on the compositor's.  Between two wheels
    // the page keeps moving, so one notch becomes several frames whose advances
    // are small enough to place — which is the whole point of scrolling in a
    // stream instead of a scroll at a time.
    let mut next_inject = Instant::now();
    // Wheels sent since the page last moved.  A count, not a deadline: matching
    // a frame of a large region costs a good part of a second, so a wall-clock
    // window would expire while the page was still getting round to moving.
    let mut notches_since_movement = 0u32;

    let stop = loop {
        if let Some(event) = hint.poll() {
            match event {
                qt_overlay::HintEvent::Done => break StopReason::UserDone,
                qt_overlay::HintEvent::Cancelled => {
                    // Dropping the session takes the overlay off the screen;
                    // the stitched image is thrown away with it.
                    drop(hint);
                    return Err(VshotError::SelectionCancelled);
                }
                qt_overlay::HintEvent::Closed => break StopReason::HintLost,
            }
        }
        if started.elapsed() >= options.timeout {
            break StopReason::TimeLimit;
        }
        if frames >= options.max_frames {
            break StopReason::FrameLimit;
        }
        if notches_since_movement >= IDLE_NOTCHES_TO_STOP {
            break StopReason::AtTheEnd;
        }

        let now = Instant::now();
        if now >= next_inject {
            injector.scroll(notches)?;
            next_inject = now + INJECT_INTERVAL;
            notches_since_movement += 1;
        }

        let grabbed = Instant::now();
        frames += 1;
        let frame = grab(capture, output, region)?;
        match push_frame(&mut stitcher, &frame)? {
            StitchStep::Seam { .. } => {
                notches_since_movement = 0;
                dropped_streak = 0;
            }
            StitchStep::Idle => {
                // Nothing moved between these two frames: either the page is
                // still on its way, or it has nowhere left to go.  Which one it
                // is only shows once the wheel has been sent a few more times,
                // so the wheel simply keeps going and the count decides.
            }
            StitchStep::Dropped => {
                // The page moved further than the strip search can follow.  The
                // frame is not accepted — the next one is measured against the
                // last accepted frame again — and the wheel is halved so that
                // the next frames land inside the search window.  The page did
                // move, though, so this is not the end of the content.
                notches_since_movement = 0;
                dropped_streak += 1;
                if notches > 1 {
                    notches = (notches / 2).max(1);
                } else if dropped_streak >= DROPPED_STREAK_TO_STOP {
                    // One notch at a time is already more than can be measured,
                    // so there is no smaller scroll to try.
                    break StopReason::TooFast;
                }
            }
            StitchStep::Limit => break StopReason::HeightLimit,
        }

        if last_status.elapsed() >= STATUS_INTERVAL {
            hint.status(stitcher.stitched_height(), frames, "")?;
            last_status = Instant::now();
        }

        // Frames come back as fast as the compositor hands them over, but not
        // faster than a display can have committed a new one: grabbing quicker
        // would read the same picture twice and pay for a match each time.
        if let Some(rest) = GRAB_INTERVAL.checked_sub(grabbed.elapsed()) {
            std::thread::sleep(rest);
        }
    };

    hint.status(stitcher.stitched_height(), frames, stop_reason(stop))?;
    if stop != StopReason::UserDone {
        // Leave the verdict on screen: a capture that stopped short says why
        // here, and closing the bar immediately would hide that.
        std::thread::sleep(FINAL_STATUS_LINGER);
    }
    hint.close()?;
    let appended = stitcher.stitched_height().saturating_sub(pixel_height);
    Ok(LongShotResult {
        frame: stitcher.into_frame()?,
        density,
        appended,
        output: output_name,
        frames,
        stop,
    })
}

/// The output that fully contains `region`: a scrolling capture stitches one
/// scroll container, which never spans two monitors.
fn output_containing(outputs: &[OutputInfo], region: Rect) -> Result<&OutputInfo> {
    outputs
        .iter()
        .find(|output| region.intersection(output.geometry) == Some(region))
        .ok_or_else(|| {
            VshotError::LongShotRegion("the selection has to sit inside a single monitor".into())
        })
}

/// One frame of the region, at the output's own pixels.
///
/// The picker works in desktop coordinates while a capture speaks output-local
/// ones, so the rectangle is moved onto its output first.  A backend that can
/// copy just that rectangle does so; the ones that cannot cut it out of a
/// full-screen frame, which is the same picture at a higher price.
fn grab(capture: &mut Capturer, output: &OutputInfo, region: Rect) -> Result<Frame> {
    let local = Rect::new(
        region.origin.x - output.geometry.origin.x,
        region.origin.y - output.geometry.origin.y,
        region.size.width,
        region.size.height,
    );
    let frame = capture.capture_region(&output.name, local, output.scale.max(1), false)?;
    dump_frame(&frame);
    Ok(frame)
}

/// Development aid: with `VSHOT_LONG_DEBUG_DIR=<dir>` every grabbed frame is
/// written there, so a stitched image can be checked against the frames it was
/// built from.  Nothing is written unless the variable is set, and the
/// directory is created on demand — asking for a dump should not also require
/// knowing to `mkdir` first.
fn debug_directory() -> Option<std::path::PathBuf> {
    let directory = std::env::var_os("VSHOT_LONG_DEBUG_DIR")?;
    let directory = std::path::PathBuf::from(directory);
    std::fs::create_dir_all(&directory).ok()?;
    Some(directory)
}

fn dump_frame(frame: &Frame) {
    use std::sync::atomic::{AtomicU32, Ordering};

    let Some(directory) = debug_directory() else {
        return;
    };
    static NEXT: AtomicU32 = AtomicU32::new(0);
    let index = NEXT.fetch_add(1, Ordering::Relaxed);
    let path = directory.join(format!("grab-{index:04}.png"));
    if let Ok(bytes) = frame.to_png() {
        let _ = std::fs::write(path, bytes);
    }
}

/// Development aid, same switch as [`dump_frame`]: every decision the stitcher
/// made is appended to `steps.log`, so a run can be replayed off-line.
fn dump_step(step: StitchStep) {
    use std::io::Write;

    let Some(directory) = debug_directory() else {
        return;
    };
    let path = directory.join("steps.log");
    if let Ok(mut file) = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
    {
        let _ = writeln!(file, "{step:?}");
    }
}

/// Feeds one frame to the stitcher, recording what it decided.
fn push_frame(stitcher: &mut Stitcher, frame: &Frame) -> Result<StitchStep> {
    let step = stitcher.push(frame)?;
    dump_step(step);
    Ok(step)
}

/// The logical bounding box of every output: the coordinate space a virtual
/// pointer's absolute positions are expressed in.
pub fn desktop_bounds(outputs: &[OutputInfo]) -> Result<Rect> {
    let mut bounds: Option<Rect> = None;
    for output in outputs {
        let geometry = output.geometry;
        bounds = Some(match bounds {
            None => geometry,
            Some(current) => {
                let left = current.origin.x.min(geometry.origin.x);
                let top = current.origin.y.min(geometry.origin.y);
                let right = current.right()?.max(geometry.right()?);
                let bottom = current.bottom()?.max(geometry.bottom()?);
                Rect::new(
                    left,
                    top,
                    u32::try_from(right - left).map_err(|_| {
                        VshotError::IncompleteTopology("the desktop bounds do not fit".into())
                    })?,
                    u32::try_from(bottom - top).map_err(|_| {
                        VshotError::IncompleteTopology("the desktop bounds do not fit".into())
                    })?,
                )
            }
        });
    }
    bounds.ok_or_else(|| VshotError::IncompleteTopology("no outputs to scroll on".into()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::geometry::Size;
    use wayland_client::protocol::wl_output::Transform;

    fn output(name: &str, x: i32, y: i32, width: u32, height: u32, scale: u32) -> OutputInfo {
        OutputInfo {
            global_id: 1,
            name: name.into(),
            geometry: Rect::new(x, y, width, height),
            pixel_size: Size::new(width * scale, height * scale),
            scale,
            transform: Transform::Normal,
        }
    }

    #[test]
    fn the_region_has_to_sit_inside_one_output() {
        let outputs = vec![
            output("DP-1", 0, 0, 1920, 1080, 1),
            output("DP-2", 1920, 0, 3840, 2160, 2),
        ];
        let inside = Rect::new(100, 100, 800, 600);
        assert_eq!(output_containing(&outputs, inside).unwrap().name, "DP-1");
        let spanning = Rect::new(1800, 100, 400, 600);
        assert!(output_containing(&outputs, spanning).is_err());
    }

    #[test]
    fn the_desktop_bounds_cover_every_output() {
        let outputs = vec![
            output("DP-1", 0, 0, 1920, 1080, 1),
            output("DP-2", 1920, 0, 1920, 1080, 2),
        ];
        let bounds = desktop_bounds(&outputs).unwrap();
        assert_eq!(bounds.origin.x, 0);
        assert_eq!(bounds.origin.y, 0);
        assert_eq!(bounds.size.width, 3840);
        assert_eq!(bounds.size.height, 1080);
    }

    #[test]
    fn the_defaults_are_the_documented_ones() {
        let options = LongShotOptions::default();
        assert_eq!(options.notches, 1);
        assert_eq!(options.max_height, 30_000);
        assert_eq!(options.ignore_top, 0);
        // A stream takes several frames per wheel, so the frame ceiling has to
        // allow far more than one frame per scroll would have needed.
        assert!(options.max_frames > 1_000);
        // A frame of a plausible viewport is small next to the limit.
        assert!(options.max_height > Size::new(1920, 1080).height);
    }
}

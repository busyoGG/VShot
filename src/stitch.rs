// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Stitching the frames of a scrolling capture into one tall image.
//!
//! Every frame is the same viewport, one scroll further down than the last.
//! The overlap between two consecutive frames is what makes the stitched image
//! possible: matching a strip of the new frame against the previous one gives
//! the distance the content travelled, and the rows at the bottom of the new
//! frame are the only ones that were not on screen before.
//!
//! Each frame is aligned against **the previous frame**, not against the image
//! built so far.  Consecutive frames overlap by a whole viewport minus one
//! scroll step, so the match is unambiguous; aligning against the growing
//! image would compare against pixels stitched many steps ago and let rounding
//! mistakes accumulate.
//!
//! The top and the bottom of a frame are usually not the page at all: a title
//! bar, a sticky header, a status line.  Those rows show the same picture in
//! every frame, so a match built on them scores a perfect zero at offset zero
//! and reads as "nothing moved" whatever the page did.  They are kept out of
//! the match, and out of the growing image too: they belong in the result once,
//! at the top (or bottom) of it, not once per frame.

use crate::error::{Result, VshotError};
use crate::geometry::Size;
use crate::model::Frame;

/// Horizontal subsampling used for matching: one sample every this many
/// pixels.  Scroll offsets are vertical, so throwing away horizontal detail
/// costs nothing and makes the search several times cheaper.
const SUBSAMPLE: u32 = 4;

/// Slack for the "this match sits against the end of the search window" test:
/// the true offset may be a few rows past the largest one that can be
/// measured, so only offsets this close to the limit count as running out of
/// window.
const NEAR_LIMIT_ROWS: u32 = 4;

/// How far two probes may differ and still be read as the same scroll.  The
/// offsets are whole rows of the same picture, so probes that really saw the
/// same scroll agree exactly; the slack is only there for the odd row of
/// antialiasing at a boundary.
const SEAM_TOLERANCE: u32 = 2;

/// Rows of the new frame a probe is built from, at most.  This is also the
/// distance at which two offsets stop overlapping each other enough to count as
/// neighbours when the match is judged.
const MAX_STRIP_ROWS: u32 = 96;

/// Probe lengths, as a divisor of the strip: a shorter probe costs evidence but
/// reaches further down the frame, so the same scroll is measured on the second
/// or third try even when it outran the strip.
const PROBE_DIVISORS: [u32; 3] = [1, 2, 4];

/// Rows the probe may grow to for the second opinion [`Stitcher::decide`] takes
/// on an ambiguous match, see [`Stitcher::recheck`].
const MAX_RECHECK_ROWS: usize = MAX_STRIP_ROWS as usize * 2;

/// Fewest rows an offset has to be checkable with to be worth measuring.  Below
/// this the margin a match has over the offsets far from it stops meaning
/// anything.
const MIN_PROBE_ROWS: u32 = 8;

/// Mean per-sample difference (0-255) at or above which a row counts as moving
/// at all.
const ACTIVITY_COST: f64 = 2.0;

/// How much of the page's own median motion a row needs to be treated as
/// content.  Chrome that the page shows through — a translucent title bar over
/// a browser window — changes, but far less than the page underneath it, and
/// the ratio is what tells the two apart without a constant that only fits one
/// screen.
const ACTIVITY_FRACTION: f64 = 0.75;

/// Average per-sample difference above which a match is not trusted at all.
///
/// This has to sit well above "pixel for pixel identical", because a live page
/// does not repeat itself exactly: measured on a browser page, the *correct*
/// offset scores around 8-13 — animations, lazy-loaded blocks and sub-pixel
/// scroll positions all show up as a few grey levels.  At 12 the true offset
/// was sometimes rejected and the frame dropped, which is why this is 25 and
/// the real decision is made by [`RIVAL_MARGIN`] instead.
const MISMATCH_COST: f64 = 25.0;

/// How much better the winning offset has to be than the best offset far away
/// from it.  A page built from similar blocks (a chat log, a table of
/// contents) matches several offsets nearly as well; without this margin the
/// winner would be luck rather than alignment.
const RIVAL_MARGIN: f64 = 3.0;

/// How many frames in a row may fail to align before the reference frame is
/// given up on and the newest one takes its place.
///
/// Holding on to the reference is what keeps a rejected frame's scroll from
/// being lost: the next frame is measured against it and the distance covers
/// both scrolls.  But that only helps while the page is still close enough for
/// the wider search to reach, and a page that has moved further than that never
/// comes back — so after a few the rows in between are written off rather than
/// ending the capture's ability to measure anything at all.
const MAX_STALE_FRAMES: u32 = 3;

/// What one frame did to the stitched image.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StitchStep {
    /// `advance` rows were appended: the content had moved up that far since
    /// the last accepted frame.  The first frame reports the whole viewport.
    Seam { advance: u32 },
    /// The page is where it already was: that is what most of the frame says,
    /// and a few rows changing is a decoration, not a scroll.
    Idle,
    /// The scroll ran past everything that can be checked: the frame was
    /// dropped, and the scroll that produced it is worth undoing and retrying
    /// smaller.
    Dropped,
    /// The stitched image reached its height limit and is finished.
    Limit,
}

/// Grayscale, horizontally subsampled copy of one frame.
struct GrayFrame {
    stride: u32,
    data: Vec<u8>,
}

impl GrayFrame {
    fn of(rgba: &[u8], width: u32, height: u32) -> Self {
        let stride = width.div_ceil(SUBSAMPLE);
        let row_bytes = width as usize * 4;
        let mut data = vec![0u8; stride as usize * height as usize];
        for y in 0..height as usize {
            let row = &rgba[y * row_bytes..(y + 1) * row_bytes];
            for x in 0..stride as usize {
                let pixel = &row[x * SUBSAMPLE as usize * 4..];
                let luma = (u32::from(pixel[0]) * 299
                    + u32::from(pixel[1]) * 587
                    + u32::from(pixel[2]) * 114)
                    / 1000;
                data[y * stride as usize + x] = luma as u8;
            }
        }
        Self { stride, data }
    }

    fn row(&self, index: u32) -> &[u8] {
        let start = index as usize * self.stride as usize;
        &self.data[start..start + self.stride as usize]
    }

    fn height(&self) -> u32 {
        (self.data.len() / self.stride as usize) as u32
    }

    /// Mean per-sample difference between this frame's row `index` and the
    /// other frame's row of the same number.
    fn activity(&self, index: u32, other: &GrayFrame) -> f64 {
        let total: u32 = self
            .row(index)
            .iter()
            .zip(other.row(index))
            .map(|(left, right)| u32::from(left.abs_diff(*right)))
            .sum();
        f64::from(total) / f64::from(self.stride)
    }
}

/// Where the page sits inside one frame, measured against the frame before it.
struct Bands {
    /// One past the last row of the page: everything from here down is chrome
    /// that never moves, and it is left out of the image until the very end.
    bottom: u32,
    /// The rows that moved, top first.  These are the page, and the only rows
    /// a probe may be built from.
    rows: Vec<u32>,
}

impl Bands {
    /// The row above `bottom` starts the bottom chrome; the rows before the
    /// first one in `rows` are the top chrome.  Neither is named: all that is
    /// needed of the top is that its rows stay out of `rows`, and the bottom
    /// only matters because the rows appended from a frame are counted up from
    /// it.
    ///
    /// `known_bottom` is where the page stopped on an earlier frame, and it
    /// wins over anything above it measured here.  How far down the page
    /// reaches is a property of the window, not of one frame, and a stretch of
    /// page whose rows repeat what is above them — a uniform block, a slow
    /// background fade, a blank tail — reads as "did not change" and would
    /// move the edge up.  Counting the appended rows up from that edge is what
    /// puts rows into the image that are already there.
    fn between(
        current: &GrayFrame,
        previous: &GrayFrame,
        ignore_top: u32,
        known_bottom: Option<u32>,
    ) -> Bands {
        let height = current.height();
        let activity: Vec<f64> = (0..height)
            .map(|index| {
                if index < ignore_top {
                    0.0
                } else {
                    current.activity(index, previous)
                }
            })
            .collect();
        // The bottom chrome is the run of rows at the end of the frame that did
        // not change at all.  Only the absolute threshold decides it: a row that
        // merely changed less than the rest is still the page, and treating it
        // as chrome would cut real content off the end of the image.
        let mut bottom = height;
        while bottom > ignore_top && activity[bottom as usize - 1] < ACTIVITY_COST {
            bottom -= 1;
        }
        if let Some(known) = known_bottom {
            bottom = bottom.max(known.min(height));
        }
        // Which of the rows above it a probe may be built from: those that
        // moved at all, and — of those — the ones that moved about as much as
        // the page does.  The relative threshold is what counts out a
        // translucent bar that follows the page a little: because it is also
        // where it already is, its own best match is offset zero, so keeping it
        // would pull every measurement towards "nothing moved".
        let moved: Vec<(u32, f64)> = (ignore_top..bottom)
            .map(|index| (index, activity[index as usize]))
            .filter(|(_, value)| *value >= ACTIVITY_COST)
            .collect();
        let mut values: Vec<f64> = moved.iter().map(|(_, value)| *value).collect();
        values.sort_by(f64::total_cmp);
        let floor = values
            .get(values.len() / 2)
            .map_or(0.0, |median| ACTIVITY_FRACTION * median);
        let rows: Vec<u32> = moved
            .into_iter()
            .filter(|(_, value)| *value >= floor)
            .map(|(index, _)| index)
            .collect();
        Bands { bottom, rows }
    }
}

/// Builds one tall image out of the frames of a scrolling capture.
pub struct Stitcher {
    width: u32,
    height: u32,
    /// Rows at the top of every frame left out of the match: a sticky header
    /// or a fixed toolbar does not move, and matching on it would report that
    /// the page never scrolled.
    ignore_top: u32,
    max_height: u32,
    /// Height of the stitched image so far.
    rows: u32,
    /// Where the page stopped on the last frame that scrolled, in rows from the
    /// top of a frame.  It only ever grows, see [`Bands::between`]: the edge is
    /// a property of the window, and a single frame that appears to end above
    /// it is rows of page that repeat what came before.
    page_bottom: Option<u32>,
    /// RGBA8 rows stitched so far, top first.
    pixels: Vec<u8>,
    previous: Option<GrayFrame>,
    /// How many pushes in a row have failed to align, see [`MAX_STALE_FRAMES`].
    stale: u32,
    /// Whether the first frame's bottom chrome has been taken back off again.
    trimmed: bool,
    /// Bottom chrome of the last frame that scrolled, appended by
    /// [`Stitcher::into_frame`] so it shows in the result once, at the bottom.
    footer: Vec<u8>,
}

impl Stitcher {
    /// `width`/`height` describe one frame (the viewport that was selected);
    /// every frame pushed must match, and the result is at most `max_height`
    /// rows tall.
    pub fn new(width: u32, height: u32, ignore_top: u32, max_height: u32) -> Result<Self> {
        if width == 0 || height == 0 {
            return Err(VshotError::LongShotStitch(
                "the selected region has no pixels".into(),
            ));
        }
        if ignore_top >= height {
            return Err(VshotError::LongShotStitch(format!(
                "`--ignore-top {ignore_top}` is not smaller than the frame height {height}"
            )));
        }
        if max_height < height {
            return Err(VshotError::LongShotStitch(format!(
                "the height limit {max_height} is below one frame of {height} rows"
            )));
        }
        Ok(Self {
            width,
            height,
            ignore_top,
            max_height,
            rows: 0,
            pixels: Vec::new(),
            previous: None,
            stale: 0,
            trimmed: false,
            footer: Vec::new(),
            page_bottom: None,
        })
    }

    /// Height of the stitched image so far, in rows.
    pub fn stitched_height(&self) -> u32 {
        self.rows
    }

    /// Every frame of the capture: the first one seeds the image, the rest are
    /// aligned against the last frame that was accepted.
    ///
    /// A frame that comes back as anything but [`StitchStep::Seam`] appends
    /// nothing and does not become the reference.  The caller scrolls on from
    /// wherever the page is, so the next frame is measured against the last
    /// accepted one and covers both scrolls — the rejected frame's rows are not
    /// lost, only measured later.  [`MAX_STALE_FRAMES`] says what happens when
    /// that keeps failing.
    pub fn push(&mut self, frame: &Frame) -> Result<StitchStep> {
        let bytes = frame.pixels();
        let expected = self.width as usize * self.height as usize * 4;
        if bytes.len() != expected {
            return Err(VshotError::LongShotStitch(format!(
                "frame is {} bytes, expected {expected} for {}x{}",
                bytes.len(),
                self.width,
                self.height
            )));
        }
        let gray = GrayFrame::of(bytes, self.width, self.height);
        let Some(previous) = self.previous.as_ref() else {
            // The first frame is the top of the page: all of it is new.  How
            // much of its bottom is chrome rather than page cannot be told
            // until a second frame arrives, so it goes in whole and is trimmed
            // back when the page is first measured.
            self.pixels.extend_from_slice(bytes);
            self.rows = self.height;
            self.previous = Some(gray);
            return Ok(StitchStep::Seam {
                advance: self.height,
            });
        };

        let bands = Bands::between(&gray, previous, self.ignore_top, self.page_bottom);
        let step = self.decide(previous, &gray, &bands);
        if let StitchStep::Seam { advance } = step {
            // The page bottom is what the window gives, not what this one frame
            // shows: it is already the lower of the two, so it is also the one
            // the next frame is measured against.
            self.page_bottom = Some(bands.bottom);
            let stride = self.width as usize * 4;
            // The page has been measured for the first time, so the first
            // frame's bottom chrome can be taken back off — otherwise it would
            // sit in the middle of the image.  Only possible while the image is
            // still that one frame.
            if !self.trimmed && self.rows == self.height {
                self.trimmed = true;
                if bands.bottom < self.height {
                    self.pixels.truncate(bands.bottom as usize * stride);
                    self.rows = bands.bottom;
                }
            }
            if self.rows + advance > self.max_height {
                return Ok(StitchStep::Limit);
            }
            // The rows worth appending are the page rows the previous frame did
            // not show: counted up from where the bottom chrome starts, not from
            // the bottom of the frame, which is chrome in every frame.
            let start = (bands.bottom - advance) as usize * stride;
            let end = bands.bottom as usize * stride;
            self.pixels.extend_from_slice(&bytes[start..end]);
            self.rows += advance;
            self.footer.clear();
            if !bands.rows.is_empty() && bands.bottom < self.height {
                self.footer = bytes[bands.bottom as usize * stride..].to_vec();
            }
        }
        if matches!(step, StitchStep::Seam { .. }) {
            self.previous = Some(gray);
            self.stale = 0;
        } else {
            // A frame that was not accepted does not become the reference: the
            // next one is measured against the last frame that *was*, so the
            // scroll this frame came from is not lost — it is measured later,
            // over a longer distance, and the rows it passed over go into the
            // image with the next accepted frame.
            //
            // That only works while the page is still near the reference.  A run
            // of rejected frames means it is not, and measuring against a page
            // that is no longer on screen can never succeed, so after a few the
            // reference is moved up anyway.  The rows in between are lost, which
            // is the lesser evil next to a capture that cannot measure anything
            // again.
            self.stale += 1;
            if self.stale >= MAX_STALE_FRAMES {
                self.previous = Some(gray);
                self.stale = 0;
            }
        }
        Ok(step)
    }

    /// Asks the same question again with a longer probe than [`strip_rows`]
    /// allows, and answers with the offset that settles it.
    ///
    /// [`decide`] reaches for this when its longest probe could not separate
    /// its answer from a rival far away: the page carries the same rows again a
    /// few hundred rows down — a table, a chat log, a run of identical list
    /// items — and a probe shorter than the repeat matches both occurrences
    /// about equally well.  A longer one tells them apart.
    ///
    /// The probe stops short of the bottom of the frame, because the rows it
    /// covers are exactly the rows the search cannot then look at: a probe that
    /// reached all the way down would leave a window too small to contain the
    /// offset being rechecked.  Since `rows` is in frame order, the longest
    /// probe that still leaves room is a partition point.
    ///
    /// [`strip_rows`]: Self::strip_rows
    /// [`decide`]: Self::decide
    fn recheck(
        &self,
        previous: &GrayFrame,
        current: &GrayFrame,
        rows: &[u32],
        bottom: u32,
        answer: u32,
    ) -> Option<u32> {
        let needed = answer.saturating_add(NEAR_LIMIT_ROWS + 1);
        let limit = bottom.checked_sub(1 + needed)?;
        let probe = rows
            .partition_point(|row| *row <= limit)
            .min(MAX_RECHECK_ROWS);
        if probe <= self.strip_rows() as usize {
            return None;
        }
        let sub = &rows[..probe];
        let max_advance = bottom - 1 - sub[probe - 1];
        seam_of(self.best_shift(previous, current, sub, max_advance))
    }

    /// What the new frame did, measured against the frame before it.
    ///
    /// The probe is made of the page rows — top first, so that it sits as high
    /// in the frame as the page allows and leaves the most room below it to
    /// match against — and it comes in a few lengths.  Length is evidence, and
    /// on a frame that changed without scrolling — a repaint, a blinking
    /// cursor, a video — a short probe is where a coincidence comes from: a few
    /// dozen rows will line up at some far-away offset by luck, score better
    /// than the truth, and report a scroll that never happened.  So the probes
    /// are asked in order of length and the first one that could see the whole
    /// scroll decides.  If it cannot place the page, the page did not move;
    /// the shorter probes only get to veto an answer that is already there.
    fn decide(&self, previous: &GrayFrame, current: &GrayFrame, bands: &Bands) -> StitchStep {
        if bands.rows.len() < self.strip_rows() as usize {
            // Fewer rows moved than one probe is long: there is nothing to match
            // with.  This is the "a cursor blinked" case, and it is deliberately
            // an absolute size rather than a share of the viewport.  A share of
            // the viewport quietly lost rows near the end of a page: past the
            // last line there is nothing but background, and a page's own blank
            // rows do not move either, so a page that is still scrolling can have
            // far fewer moving rows than the viewport has rows — and the rows
            // being appended are exactly the ones in that shrinking band.  A
            // decoration that moves in place needs no guard here: it matches at
            // offset zero, which [`seam_of`] refuses.
            return StitchStep::Idle;
        }
        let strip = self.strip_rows();
        // The first probe, kept for the one verdict that needs a probe rather
        // than a witness: whether the page ran past what can be measured.
        let mut longest: Option<(Shift, u32)> = None;
        // The distance the page travelled, once a probe has earned the right to
        // say so.
        let mut answer: Option<u32> = None;
        let mut previous_probe = 0;
        for divisor in PROBE_DIVISORS {
            // A probe is a fixed set of rows for the whole search, so a band
            // that is in every frame but never matches — the translucent title
            // bar of a window whose content shows through it, an animation that
            // is not the page — adds the same amount to every offset instead of
            // favouring the ones that happen to use fewest of its rows.
            let probe = (strip / divisor)
                .max(MIN_PROBE_ROWS)
                .min(bands.rows.len() as u32);
            if probe == previous_probe {
                continue;
            }
            previous_probe = probe;
            let rows = &bands.rows[..probe as usize];
            // Every row the probe is matched against has to land on the page in
            // the previous frame, above its bottom chrome: that is what caps the
            // scroll this can measure, and scrolling past it is answered by the
            // next probe down.
            let max_advance = bands.bottom - 1 - rows[probe as usize - 1];
            if max_advance == 0 {
                continue;
            }
            let shift = self.best_shift(previous, current, rows, max_advance);
            if longest.is_none() {
                longest = Some((shift, max_advance));
            }

            let Some(advance) = answer else {
                // No witness yet.  An offset pressed against the end of the
                // window is not one: it is what a probe reports when the scroll
                // went past everything it could check, and only a shorter probe
                // reaches far enough to say where the page actually is.
                if shift.advance + NEAR_LIMIT_ROWS >= max_advance {
                    continue;
                }
                match seam_of(shift) {
                    Some(advance) => {
                        answer = Some(advance);
                        continue;
                    }
                    // The longest probe that could see the whole scroll could
                    // not place the page.  Before writing the frame off as "the
                    // page did not move", ask once more with a longer probe: the
                    // usual reason for this shape is a page that has the same
                    // rows again further down, and it takes more rows than the
                    // repeat to tell the two occurrences apart.
                    None => match self.recheck(
                        previous,
                        current,
                        &bands.rows,
                        bands.bottom,
                        shift.advance,
                    ) {
                        Some(advance) => {
                            answer = Some(advance);
                            continue;
                        }
                        // Whatever the shorter probes find, a frame this unclear
                        // is not worth appending rows on a guess — and the page
                        // not moving is what this looks like.
                        None => return StitchStep::Idle,
                    },
                }
            };
            // A witness is in hand: a shorter probe that placed the page
            // somewhere else is the shape a coincidence makes, and there is no
            // way to tell it from the truth.  One that could not place it at
            // all proves nothing and is ignored.
            if seam_of(shift).is_some_and(|value| value.abs_diff(advance) > SEAM_TOLERANCE) {
                return StitchStep::Idle;
            }
        }
        if let Some(advance) = answer {
            return StitchStep::Seam { advance };
        }
        // Every probe ran into the end of its own window without exception:
        // the page really did outrun what can be measured, and the caller
        // should try again with a smaller scroll.
        match longest {
            Some((shift, max_advance)) if shift.advance + NEAR_LIMIT_ROWS >= max_advance => {
                StitchStep::Dropped
            }
            _ => StitchStep::Idle,
        }
    }

    /// The stitched image, with the bottom chrome of the last frame it saw put
    /// back at the end — the one place it belongs.
    pub fn into_frame(mut self) -> Result<Frame> {
        let stride = self.width as usize * 4;
        let footer_rows = (self.footer.len() / stride) as u32;
        if footer_rows > 0 && self.rows + footer_rows <= self.max_height {
            self.pixels.extend_from_slice(&self.footer);
            self.rows += footer_rows;
        }
        Frame::new(Size::new(self.width, self.rows), self.pixels)
    }

    /// Rows the probe is built from at most: a fixed fraction of the frame, so
    /// that the search stays cheap on a tall selection.
    fn strip_rows(&self) -> u32 {
        let available = self.height - self.ignore_top;
        (available / 4).clamp(1, MAX_STRIP_ROWS)
    }

    /// Finds the offset with the smallest average difference over `rows`,
    /// trying every offset up to `max_advance` and exiting early once a
    /// candidate is already worse than the best one found.
    ///
    /// It deliberately does not do a coarse pass first: on real content the
    /// difference at a wrong offset is a large random number, so which coarse
    /// candidate happens to look best is luck, and refining around that winner
    /// can miss the true offset entirely.  The early exit is what keeps the
    /// full search cheap — a wrong offset blows past the cutoff within a few
    /// rows, so only the candidates nearest the truth are measured in full.
    fn best_shift(
        &self,
        previous: &GrayFrame,
        current: &GrayFrame,
        moving: &[u32],
        max_advance: u32,
    ) -> Shift {
        let mut best = Shift {
            advance: 0,
            cost: f64::INFINITY,
            rival: f64::INFINITY,
        };
        let far = self.strip_rows();
        for advance in 0..=max_advance {
            // An offset that is already worse than the winner by more than the
            // margin can neither win nor count as a rival, so the search may
            // stop measuring it.  The margin has to be part of that cutoff: with
            // the winner's cost alone, an offset that is *nearly* as good would
            // be cut off before it could be scored, and the ambiguity it stands
            // for would go unseen.
            let Some(cost) =
                self.strip_cost(previous, current, moving, advance, best.cost + RIVAL_MARGIN)
            else {
                continue;
            };
            // Only offsets more than a strip away say anything about how
            // ambiguous this match is: its immediate neighbours overlap the
            // winner almost entirely and are expected to score like it.
            let away = advance.abs_diff(best.advance) > far;
            if cost < best.cost {
                if away {
                    best.rival = best.rival.min(best.cost);
                }
                best.cost = cost;
                best.advance = advance;
            } else if away {
                best.rival = best.rival.min(cost);
            }
        }
        best
    }

    /// Average absolute difference between the given rows of the new frame and
    /// the previous frame's rows `advance` further down.  `None` means the
    /// running total already exceeded `cutoff`.
    fn strip_cost(
        &self,
        previous: &GrayFrame,
        current: &GrayFrame,
        rows: &[u32],
        advance: u32,
        cutoff: f64,
    ) -> Option<f64> {
        let samples = rows.len() as u64 * u64::from(current.stride);
        let max_total = if cutoff.is_finite() {
            (cutoff * samples as f64) as u64
        } else {
            u64::MAX
        };
        let mut total: u64 = 0;
        for row in rows {
            let a = current.row(*row);
            let b = previous.row(row + advance);
            // Summed into a u32 first so that the byte-wise absolute
            // differences can be vectorized: with a u64 accumulator alone the
            // compiler cannot use the byte-SAD instruction, and the search runs
            // 3.5x slower for it.  A row of samples cannot come near a u32 — 255
            // per sample against 4 billion — so nothing is lost on the way.
            let row_total: u32 = a
                .iter()
                .zip(b)
                .map(|(left, right)| u32::from(left.abs_diff(*right)))
                .sum();
            total += u64::from(row_total);
            if total > max_total {
                return None;
            }
        }
        Some(total as f64 / samples as f64)
    }
}

#[derive(Clone, Copy)]
struct Shift {
    advance: u32,
    cost: f64,
    /// Best cost at an offset at least one probe away from `advance`.
    rival: f64,
}

/// The offset a match has to be trusted at, if any.
///
/// The absolute cost of the winner is only a coarse gate: a live page never
/// repeats itself exactly, so the true offset can score ten grey levels while
/// every wrong offset scores eighty.  What separates a real alignment from a
/// guess is the *margin* over the best far-away offset.
fn seam_of(shift: Shift) -> Option<u32> {
    if shift.advance == 0 {
        // Zero is not a seam: it would append nothing while telling the caller
        // the scroll was measured.
        return None;
    }
    if shift.cost > MISMATCH_COST {
        return None;
    }
    if shift.rival < shift.cost + RIVAL_MARGIN {
        // Something far away matches about as well, so this probe does not say
        // where the page is: picking between the two would be a coin flip.
        return None;
    }
    Some(shift.advance)
}

#[cfg(test)]
mod tests {
    use super::*;

    const W: u32 = 40;
    const H: u32 = 30;

    /// Deterministic pseudo-random RGBA content, so a "different page" is
    /// really different and a shifted window is really a shift.
    fn noise(seed: u32, rows: u32) -> Vec<u8> {
        let mut state = seed | 1;
        let mut bytes = Vec::with_capacity(W as usize * rows as usize * 4);
        for index in 0..W as usize * rows as usize {
            state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            let value = (state >> 24) as u8;
            bytes.push(value);
            bytes.push(value.wrapping_add((index % 61) as u8));
            bytes.push(value.wrapping_mul(3));
            bytes.push(255);
        }
        bytes
    }

    /// The frame a user would see after scrolling `top` rows into `source`.
    fn frame_at(source: &[u8], top: u32) -> Frame {
        let stride = W as usize * 4;
        let start = top as usize * stride;
        let end = start + H as usize * stride;
        Frame::new(Size::new(W, H), source[start..end].to_vec()).unwrap()
    }

    fn stitched(stitcher: Stitcher) -> Vec<u8> {
        stitcher.into_frame().unwrap().pixels().to_vec()
    }

    fn frame_rows(frame: &Frame, from: u32) -> Vec<u8> {
        let stride = W as usize * 4;
        frame.pixels()[from as usize * stride..].to_vec()
    }

    /// A page of `blocks` blocks, each `block_rows` tall and identical to all
    /// the others: a table, a chat log, a run of the same list item.
    fn repeating_page(seed: u32, blocks: u32, block_rows: u32) -> Vec<u8> {
        let block = noise(seed, block_rows);
        let mut bytes = Vec::with_capacity(block.len() * blocks as usize);
        for _ in 0..blocks {
            bytes.extend_from_slice(&block);
        }
        bytes
    }

    #[test]
    fn a_page_of_repeating_blocks_still_reports_its_scroll() {
        // Every block is the same, so a probe shorter than a block matches the
        // block after next exactly as well as it matches the true offset: the
        // match looks ambiguous even though it is not.  Only the longer probe
        // separates the two, and the scroll has to be reported rather than
        // written off as "the page did not move" — losing it drops those rows
        // from the image.
        let source = repeating_page(41, 6, 60);
        let mut stitcher = Stitcher::new(W, 200, 0, 10_000).unwrap();
        assert_eq!(
            stitcher.push(&tall_frame(&source, 200, 0)).unwrap(),
            StitchStep::Seam { advance: 200 }
        );
        assert_eq!(
            stitcher.push(&tall_frame(&source, 200, 5)).unwrap(),
            StitchStep::Seam { advance: 5 }
        );
        let stride = W as usize * 4;
        assert_eq!(stitched(stitcher), source[..205 * stride].to_vec());
    }

    #[test]
    fn appends_only_the_rows_that_were_not_there_before() {
        let source = noise(7, 200);
        let mut stitcher = Stitcher::new(W, H, 0, 10_000).unwrap();
        assert_eq!(
            stitcher.push(&frame_at(&source, 0)).unwrap(),
            StitchStep::Seam { advance: H }
        );
        assert_eq!(stitcher.stitched_height(), H);
        assert_eq!(
            stitcher.push(&frame_at(&source, 12)).unwrap(),
            StitchStep::Seam { advance: 12 }
        );
        assert_eq!(stitcher.stitched_height(), H + 12);

        // The stitched image is exactly the source window, as one image.
        let image = stitched(stitcher);
        let stride = W as usize * 4;
        let expected = &source[..(H as usize + 12) * stride];
        assert_eq!(image, expected);
    }

    #[test]
    fn several_frames_stitch_without_drift() {
        let source = noise(11, 400);
        let mut stitcher = Stitcher::new(W, H, 0, 10_000).unwrap();
        let mut top = 0;
        for _ in 0..12 {
            stitcher.push(&frame_at(&source, top)).unwrap();
            top += 9;
        }
        assert_eq!(stitcher.stitched_height(), H + 9 * 11);
        let stride = W as usize * 4;
        assert_eq!(
            stitched(stitcher),
            source[..(H as usize + 99) * stride].to_vec()
        );
    }

    /// `noise` whose rows from `repeat_from` down are a faint copy of the rows
    /// `period` above them: a large uniform block, or a background that fades
    /// too slowly to register as changed between two frames.
    fn noise_with_faint_tail(seed: u32, rows: u32, repeat_from: u32, period: u32) -> Vec<u8> {
        let mut bytes = noise(seed, rows);
        let stride = W as usize * 4;
        for row in repeat_from as usize..rows as usize {
            for column in 0..stride {
                let above = bytes[(row - period as usize) * stride + column];
                bytes[row * stride + column] = above ^ 1;
            }
        }
        bytes
    }

    #[test]
    fn a_page_whose_tail_almost_repeats_is_not_duplicated_at_the_seam() {
        // The last rows of the frame are a faint copy of the rows a scroll
        // above them, so they read as "did not change" and the page looks like
        // it ends higher than it does.  Appending the rows counted up from that
        // edge puts rows into the image that are already there — a repeat at
        // the seam — so the page bottom measured before it has to win.
        let source = noise_with_faint_tail(37, 200, 60, 6);
        let mut stitcher = Stitcher::new(W, H, 0, 10_000).unwrap();
        let mut top = 0;
        for _ in 0..7 {
            stitcher.push(&frame_at(&source, top)).unwrap();
            top += 6;
        }
        let stride = W as usize * 4;
        // Seven frames six rows apart: one frame plus six scrolls.
        assert_eq!(
            stitched(stitcher),
            source[..(H as usize + 36) * stride].to_vec()
        );
    }

    #[test]
    fn a_repeated_frame_is_idle() {
        let source = noise(3, 100);
        let frame = frame_at(&source, 0);
        let mut stitcher = Stitcher::new(W, H, 0, 10_000).unwrap();
        stitcher.push(&frame).unwrap();
        assert_eq!(stitcher.push(&frame).unwrap(), StitchStep::Idle);
        assert_eq!(stitcher.stitched_height(), H);
    }

    #[test]
    fn a_completely_different_page_does_not_fake_a_seam() {
        let first = noise(5, 60);
        let other = noise(999, 60);
        let mut stitcher = Stitcher::new(W, H, 0, 10_000).unwrap();
        stitcher.push(&frame_at(&first, 0)).unwrap();
        let step = stitcher.push(&frame_at(&other, 0)).unwrap();
        assert!(
            matches!(step, StitchStep::Idle | StitchStep::Dropped),
            "{step:?}"
        );
        assert_eq!(
            stitcher.stitched_height(),
            H,
            "a rejected frame appends nothing"
        );
    }

    #[test]
    fn a_scroll_beyond_the_measurable_overlap_is_dropped() {
        let source = noise(13, 200);
        let mut stitcher = Stitcher::new(W, H, 0, 10_000).unwrap();
        stitcher.push(&frame_at(&source, 0)).unwrap();
        // The strip is H/4 = 7 rows and must fit below the offset, so the
        // largest measurable scroll is 30 - 7 = 23 rows.
        let step = stitcher.push(&frame_at(&source, 26)).unwrap();
        // Which of the two "cannot align this" answers comes back depends on
        // what the unmatched strip happened to resemble; both mean the same
        // thing to the caller — nothing was appended, and the frame is not
        // worth measuring against.
        assert!(
            matches!(step, StitchStep::Idle | StitchStep::Dropped),
            "{step:?}"
        );
        assert_eq!(stitcher.stitched_height(), H);
    }

    #[test]
    fn ignore_top_skips_a_sticky_header() {
        // A header that never moves, on top of content that does.
        let source = noise(17, 200);
        let header = noise(23, 6);
        let frame_at_with_header = |top: u32| {
            let mut bytes = header.clone();
            let stride = W as usize * 4;
            let start = (top + 6) as usize * stride;
            bytes.extend_from_slice(&source[start..start + (H as usize - 6) * stride]);
            Frame::new(Size::new(W, H), bytes).unwrap()
        };
        let mut stitcher = Stitcher::new(W, H, 6, 10_000).unwrap();
        stitcher.push(&frame_at_with_header(0)).unwrap();
        // Without --ignore-top the frozen header would pin the best match at
        // zero; with it the moving content decides.
        assert_eq!(
            stitcher.push(&frame_at_with_header(10)).unwrap(),
            StitchStep::Seam { advance: 10 }
        );
    }

    #[test]
    fn the_height_limit_finishes_the_image() {
        let source = noise(19, 200);
        let mut stitcher = Stitcher::new(W, H, 0, H + 8).unwrap();
        stitcher.push(&frame_at(&source, 0)).unwrap();
        assert_eq!(
            stitcher.push(&frame_at(&source, 14)).unwrap(),
            StitchStep::Limit
        );
        assert_eq!(stitcher.stitched_height(), H);
    }

    /// A `height`-row frame showing `source` from row `top` down.
    fn tall_frame(source: &[u8], height: u32, top: u32) -> Frame {
        let stride = W as usize * 4;
        let start = top as usize * stride;
        Frame::new(
            Size::new(W, height),
            source[start..start + height as usize * stride].to_vec(),
        )
        .unwrap()
    }

    #[test]
    fn a_fixed_band_at_the_top_does_not_hide_the_scroll() {
        // A 60-row title bar that never moves, above content that does.  A
        // probe that includes the band matches perfectly at offset zero and
        // used to report that nothing had scrolled at all.
        const TALL: u32 = 200;
        let source = noise(43, 400);
        let band = noise(51, 60);
        let stride = W as usize * 4;
        let frame = |top: u32| {
            let mut bytes = band.clone();
            let start = (top + 60) as usize * stride;
            bytes.extend_from_slice(&source[start..start + (TALL as usize - 60) * stride]);
            Frame::new(Size::new(W, TALL), bytes).unwrap()
        };
        let mut stitcher = Stitcher::new(W, TALL, 0, 10_000).unwrap();
        stitcher.push(&frame(0)).unwrap();
        assert_eq!(
            stitcher.push(&frame(30)).unwrap(),
            StitchStep::Seam { advance: 30 }
        );
        assert_eq!(stitcher.stitched_height(), TALL + 30);

        // The band is in the image once, at the top, and the content under it
        // continues without a gap.
        let image = stitched(stitcher);
        assert_eq!(&image[..60 * stride], &band[..]);
        assert_eq!(&image[60 * stride..], &source[60 * stride..230 * stride]);
    }

    #[test]
    fn a_fixed_band_at_the_bottom_is_stitched_in_once_at_the_bottom() {
        // A status line that never moves, under content that does.  It belongs
        // in the result once, at the bottom: appending whole frames backwards
        // from the bottom of the frame would put one copy of it after every
        // scroll, and leave the first frame's copy in the middle of the image.
        const TALL: u32 = 200;
        const BAR: u32 = 40;
        let source = noise(79, 400);
        let bar = noise(83, BAR);
        let stride = W as usize * 4;
        let frame = |top: u32| {
            let mut bytes = Vec::new();
            let start = top as usize * stride;
            let page = source[start..start + (TALL as usize - BAR as usize) * stride].to_vec();
            bytes.extend_from_slice(&page);
            bytes.extend_from_slice(&bar);
            Frame::new(Size::new(W, TALL), bytes).unwrap()
        };
        let mut stitcher = Stitcher::new(W, TALL, 0, 10_000).unwrap();
        stitcher.push(&frame(0)).unwrap();
        assert_eq!(
            stitcher.push(&frame(30)).unwrap(),
            StitchStep::Seam { advance: 30 }
        );
        // One frame of page without the bar, plus the 30 rows that are new.
        assert_eq!(stitcher.stitched_height(), TALL - BAR + 30);

        let image = stitched(stitcher);
        let page_rows = (TALL - BAR) as usize;
        assert_eq!(
            &image[..(page_rows + 30) * stride],
            &source[..(page_rows + 30) * stride],
            "the page runs on without the bar breaking it"
        );
        assert_eq!(
            &image[(page_rows + 30) * stride..],
            &bar[..],
            "one copy of the bar, at the bottom"
        );
    }

    #[test]
    fn a_scroll_that_outran_the_long_probe_falls_back_to_a_shorter_one() {
        // 160 rows of advance into a 200-row viewport.  The 50-row probe can
        // only see 150 of them, and its best offset ends up pressed against
        // that limit — which is what "the scroll went past what I can check"
        // looks like, and an offset like that says nothing about where the page
        // is.  The shorter probes reach further and measure the real distance,
        // which is what keeps a long scroll from being a gap in the image.
        const TALL: u32 = 200;
        let source = sloped(400);
        let mut stitcher = Stitcher::new(W, TALL, 0, 10_000).unwrap();
        stitcher.push(&tall_frame(&source, TALL, 0)).unwrap();
        assert_eq!(
            stitcher.push(&tall_frame(&source, TALL, 160)).unwrap(),
            StitchStep::Seam { advance: 160 }
        );
        assert_eq!(stitcher.stitched_height(), TALL + 160);
        let stride = W as usize * 4;
        assert_eq!(stitched(stitcher), source[..360 * stride].to_vec());
    }

    #[test]
    fn a_rejected_frame_is_not_the_reference_for_the_next_one() {
        // The caller rewinds the page after a rejected frame, so the next frame
        // is taken at the same place as the last accepted one.  The stitcher has
        // to still be holding that one: measuring against the rejected frame
        // would read the rewind itself as a scroll in the other direction, and
        // every frame after it would be off by the same amount.
        const TALL: u32 = 200;
        let source = noise(87, 400);
        let mut stitcher = Stitcher::new(W, TALL, 0, 10_000).unwrap();
        stitcher.push(&tall_frame(&source, TALL, 0)).unwrap();
        // A scroll no probe can follow, so this frame is not accepted.
        let step = stitcher.push(&tall_frame(&source, TALL, 190)).unwrap();
        assert!(
            !matches!(step, StitchStep::Seam { .. }),
            "a frame that outran every probe must not append rows: {step:?}"
        );
        // Back where the accepted frame was taken, and measured against it.
        assert_eq!(
            stitcher.push(&tall_frame(&source, TALL, 30)).unwrap(),
            StitchStep::Seam { advance: 30 }
        );
        assert_eq!(stitcher.stitched_height(), TALL + 30);
    }

    /// Rows whose content changes steadily down the frame, so a shift of a few
    /// rows still nearly matches at offset zero.
    fn sloped(rows: u32) -> Vec<u8> {
        let mut bytes = Vec::with_capacity(W as usize * rows as usize * 4);
        for y in 0..rows {
            let value = 20u8.saturating_add((y * 230 / rows) as u8);
            for _ in 0..W as usize {
                bytes.extend_from_slice(&[value, value, value, 255]);
            }
        }
        bytes
    }

    #[test]
    fn a_frame_that_only_moved_backwards_is_not_dropped() {
        // Scroll offsets are searched downwards only, so a frame that shows
        // content from *above* the reference — what the caller's rewind leaves
        // behind — has no match to find.  What must not happen is it coming back
        // as dropped: that scrolls the page back again and produces the same
        // frame, and a page that cannot scroll any further rattles between two
        // positions for ever.
        const TALL: u32 = 200;
        let source = sloped(500);
        let mut stitcher = Stitcher::new(W, TALL, 0, 10_000).unwrap();
        stitcher.push(&tall_frame(&source, TALL, 120)).unwrap();
        let step = stitcher.push(&tall_frame(&source, TALL, 80)).unwrap();
        assert_ne!(step, StitchStep::Dropped, "往回滚的帧不能触发回滚");
        assert_eq!(step, StitchStep::Idle);
        assert_eq!(stitcher.stitched_height(), TALL);
    }

    #[test]
    fn the_reference_moves_on_after_a_run_of_rejected_frames() {
        // Three scrolls, each far past anything a probe can follow.  Holding on
        // to the original reference is what keeps a rejected scroll from being
        // lost, but once the page is further away than the search can reach,
        // measuring against it can never succeed again: the reference has to
        // move up, or the capture stops measuring anything at all.
        const TALL: u32 = 200;
        let source = noise(101, 800);
        let mut stitcher = Stitcher::new(W, TALL, 0, 10_000).unwrap();
        stitcher.push(&tall_frame(&source, TALL, 0)).unwrap();
        for top in [190u32, 380, 570] {
            let step = stitcher.push(&tall_frame(&source, TALL, top)).unwrap();
            assert!(
                !matches!(step, StitchStep::Seam { .. }),
                "top {top} is past anything that can be measured: {step:?}"
            );
        }
        // This one is close to the last of them, which is now the reference.
        assert_eq!(
            stitcher.push(&tall_frame(&source, TALL, 600)).unwrap(),
            StitchStep::Seam { advance: 30 }
        );
    }

    #[test]
    fn a_band_that_barely_changes_is_not_the_page() {
        // A translucent title bar: it sits still, and it follows the page just
        // enough that its rows differ a little from frame to frame.  A probe
        // that counts those rows as content matches them almost perfectly at
        // offset zero — the bar is where it already is — and stops seeing the
        // scroll underneath.
        const TALL: u32 = 250;
        const BAND: u32 = 90;
        let source = noise(71, 400);
        let bar = noise(73, BAND);
        let stride = W as usize * 4;
        let frame = |top: u32, faint: u8| {
            let mut bytes = bar.clone();
            for pixel in bytes.chunks_exact_mut(4) {
                pixel[0] = pixel[0].saturating_add(faint);
            }
            let start = (top + BAND) as usize * stride;
            bytes.extend_from_slice(
                &source[start..start + (TALL as usize - BAND as usize) * stride],
            );
            Frame::new(Size::new(W, TALL), bytes).unwrap()
        };
        let mut stitcher = Stitcher::new(W, TALL, 0, 10_000).unwrap();
        stitcher.push(&frame(0, 0)).unwrap();
        assert_eq!(
            stitcher.push(&frame(60, 3)).unwrap(),
            StitchStep::Seam { advance: 60 }
        );
        assert_eq!(stitcher.stitched_height(), TALL + 60);
    }

    #[test]
    fn a_change_in_place_appends_nothing() {
        // The upper half repainted with different content and no scroll.  The
        // offset zero must never come back as a seam, because appending nothing
        // while reporting a measured scroll would stall the capture.
        let source = noise(61, 100);
        let replacement = noise(67, 100);
        let stride = W as usize * 4;
        let first = frame_at(&source, 0);
        let mut stitcher = Stitcher::new(W, H, 0, 10_000).unwrap();
        stitcher.push(&first).unwrap();
        let mut bytes = first.pixels().to_vec();
        bytes[..(H as usize / 2) * stride]
            .copy_from_slice(&replacement[..(H as usize / 2) * stride]);
        let repainted = Frame::new(Size::new(W, H), bytes).unwrap();
        let step = stitcher.push(&repainted).unwrap();
        assert!(!matches!(step, StitchStep::Seam { .. }), "{step:?}");
        assert_eq!(stitcher.stitched_height(), H);
    }

    #[test]
    fn a_page_that_redraws_without_scrolling_never_reports_a_seam() {
        // The failure this guards against, seen on a real capture: a frame that
        // did not scroll at all, in which a short probe lined up by chance at a
        // far-away offset and scored *better* than the truth did.  Taking that
        // reading appended hundreds of rows that were never on screen — the
        // stitched image repeated itself.  Every seed here redraws a band in
        // place; none of them may come back as a scroll.
        const TALL: u32 = 200;
        const BAND: u32 = 60;
        let stride = W as usize * 4;
        for seed in 0..32u32 {
            let source = noise(seed * 2 + 1, 400);
            let mut stitcher = Stitcher::new(W, TALL, 0, 10_000).unwrap();
            stitcher.push(&tall_frame(&source, TALL, 0)).unwrap();
            // Same page, with a band of it painted over where it stands.
            let mut bytes = source[..TALL as usize * stride].to_vec();
            let band = noise(seed * 2 + 2, BAND);
            let at = 40 * stride;
            bytes[at..at + band.len()].copy_from_slice(&band);
            let repainted = Frame::new(Size::new(W, TALL), bytes).unwrap();
            let step = stitcher.push(&repainted).unwrap();
            assert!(
                !matches!(step, StitchStep::Seam { .. }),
                "seed {seed}: a band repainted in place is not a scroll, got {step:?}"
            );
            assert_eq!(stitcher.stitched_height(), TALL, "seed {seed}");
        }
    }

    #[test]
    fn a_frame_of_the_wrong_size_is_rejected() {
        let source = noise(29, 120);
        let mut stitcher = Stitcher::new(W, H, 0, 10_000).unwrap();
        stitcher.push(&frame_at(&source, 0)).unwrap();
        let other = Frame::new(
            Size::new(W, H + 1),
            vec![0; W as usize * (H as usize + 1) * 4],
        )
        .unwrap();
        assert!(stitcher.push(&other).is_err());
    }

    #[test]
    fn the_result_row_count_matches_what_was_appended() {
        let source = noise(31, 300);
        let mut stitcher = Stitcher::new(W, H, 0, 10_000).unwrap();
        let mut expected = 0;
        let mut top = 0;
        for _ in 0..5 {
            if let StitchStep::Seam { advance } = stitcher.push(&frame_at(&source, top)).unwrap() {
                expected += advance;
            }
            top += 7;
        }
        assert_eq!(stitcher.stitched_height(), expected);
        let image = stitcher.into_frame().unwrap();
        assert_eq!(image.pixels().len(), W as usize * expected as usize * 4);
    }

    #[test]
    fn frame_rows_helper_reads_the_bottom_rows() {
        let source = noise(37, 100);
        let frame = frame_at(&source, 0);
        let rows = frame_rows(&frame, H - 4);
        assert_eq!(rows.len(), 4 * W as usize * 4);
        let stride = W as usize * 4;
        assert_eq!(
            rows,
            source[(H as usize - 4) * stride..H as usize * stride].to_vec()
        );
    }

    /// A frame with a little noise on every pixel: the stand-in for a page that
    /// animates or loads while it scrolls, where the *correct* offset no longer
    /// scores zero.
    fn speckled(frame: &Frame, amplitude: u8) -> Frame {
        let mut state = 0x1234_5678u32;
        let mut bytes = frame.pixels().to_vec();
        for chunk in bytes.chunks_exact_mut(4) {
            state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            let span = i32::from(amplitude) * 2 + 1;
            let delta = (state >> 24) as i32 % span - i32::from(amplitude);
            chunk[0] = (i32::from(chunk[0]) + delta).clamp(0, 255) as u8;
        }
        Frame::new(Size::new(W, H), bytes).unwrap()
    }

    #[test]
    fn a_page_that_changes_a_little_still_aligns() {
        // The true offset scores a few grey levels instead of zero, which is
        // what a live browser page looks like; it must still be followed,
        // because every wrong offset scores far worse.
        let source = noise(41, 200);
        let mut stitcher = Stitcher::new(W, H, 0, 10_000).unwrap();
        stitcher.push(&frame_at(&source, 0)).unwrap();
        let second = speckled(&frame_at(&source, 9), 12);
        assert_eq!(
            stitcher.push(&second).unwrap(),
            StitchStep::Seam { advance: 9 }
        );
        assert_eq!(stitcher.stitched_height(), H + 9);
    }

    #[test]
    fn a_page_of_similar_blocks_is_not_guessed_at() {
        // Every 12 rows repeat the same two rows, so many offsets match almost
        // equally well: the stitcher has to refuse rather than pick one.
        let mut bytes = Vec::with_capacity(W as usize * H as usize * 4);
        for y in 0..H as usize {
            let value = if y % 12 < 6 { 30u8 } else { 200u8 };
            for x in 0..W as usize {
                bytes.extend_from_slice(&[value, value, value, 255]);
                let _ = x;
            }
        }
        let repeated = Frame::new(Size::new(W, H), bytes).unwrap();
        let mut stitcher = Stitcher::new(W, H, 0, 10_000).unwrap();
        stitcher.push(&repeated).unwrap();
        let step = stitcher.push(&repeated).unwrap();
        // Either it recognizes "nothing moved" or it refuses; what it must not
        // do is claim a scroll that it cannot see.
        assert!(
            matches!(step, StitchStep::Idle | StitchStep::Dropped),
            "{step:?}"
        );
        assert_eq!(stitcher.stitched_height(), H);
    }
}

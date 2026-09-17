//! Pixel fallback for window capture. When no compositor metadata provider
//! answers (Hyprland, Sway, or the KWin scripting probe), or when `--pixel`
//! asks for it, the window is detected from the captured frame itself.
//!
//! Detection never looks across a monitor seam: every output is analysed on
//! its **own** frame, in its own device pixels.  The composed scene is no
//! basis for this — it stretches the lower-density outputs to the highest
//! scale, so a 1080p output arrives upscaled and thin features are already
//! smeared, and a flood fill seeded from the scene's edges walks through
//! another monitor's wallpaper and hands back "the whole desktop" as one
//! candidate.
//!
//! Ring detection is the primary detector on a compositor that strokes
//! windows: a border is a band a few device pixels wide, uniform along its
//! length, with a sharp change on **both** sides — that holds for a flat
//! inactive colour and for a gradient (`col.active_border = gradient … 45deg`)
//! equally, which is why the test is geometric rather than colour-based.  Four
//! such lines that close on each other are a window, and the band is left out
//! of the crop because the compositor's own geometry leaves it out.  The ring's
//! mean saturation is the focus signal: measured on a real Hyprland desktop,
//! the focused window's stroke read saturation 162 and its neighbour's 0.8.
//!
//! The second detector is background segmentation, for borderless windows: the
//! wallpaper and soft shadows are traced from the output's edges with a
//! locally-smooth flood fill, and large non-background components are window
//! candidates.  On a dark, busy desktop this is the weak detector — the flood
//! labels the wallpaper unreachable-by-smoothness as *content*, so two tiled
//! windows merge into one candidate covering the output.  It is still what a
//! compositor without borders leaves you with.
//!
//! A third, colour-histogram outline fit runs only when neither of those finds
//! anything, and a final rung degrades to the whole output — one fullscreen
//! window or a bare desktop.  `--pixel` reports which rung answered.
//!
//! A cursor position, when known, disambiguates candidates within a rung; the
//! largest candidate wins otherwise.  Truly seamless borderless tiling with
//! several windows carries no pixel signal and ends in a clear error.
//!
//! Interactive window picking runs the same detectors but keeps every
//! candidate instead of the best one ([`detect_window_candidates`]), so the
//! user can hover the window they mean.

use std::cmp::Reverse;
use std::collections::VecDeque;

use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect, Size};
use crate::model::{Frame, OutputSnapshot, SceneSnapshot};

use super::window::{ActiveWindow, WindowSource};

/// Pixel budget of an analysis frame; detection runs on a downscaled copy to
/// keep its cost predictable, but the borders and gaps that separate windows
/// are a few device pixels wide, so the budget is set by *area* rather than by
/// the long side: a 1080p output is analysed 1:1 and a 4K one at 2:1.
const MAX_ANALYSIS_PIXELS: u64 = 4_000_000;
/// Neighbor channel delta that marks both pixels as content edges.
const EDGE_DIFF: i32 = 24;
/// Sum-of-channel distance for a pixel to join a candidate outline color.
const COLOR_TOLERANCE: i32 = 45;
/// Smallest outline window side, as a percent of the analysis frame.
const OUTLINE_MIN_DIM_PCT: u32 = 15;
/// Smallest share of the output an accent outline must cover to be believed
/// as a window rather than as a rectangle of matching colors in its content.
const OUTLINE_MIN_AREA_PCT: u64 = 25;
/// Outline band coverage and interior emptiness thresholds.
const OUTLINE_COVERAGE: f64 = 0.62;
const OUTLINE_HOLLOW_MAX: f64 = 0.35;
/// Probed border thickness range, in analysis pixels.
const MAX_BORDER: u32 = 8;
/// Channel delta between neighbouring pixels that opens or closes a border
/// band: the compositor draws its stroke against both a wallpaper and a window,
/// so both sides of the band change by more than this.
const BAND_JUMP: i32 = 20;
/// How much a band's interior may wander, as a share of the jumps that delimit
/// it: a border is a strip that changes far less than the edges around it.
///
/// The share is relative on purpose, because an absolute limit read a gradient
/// stroke as no border at all.  A border the compositor paints as a ramp —
/// niri's `active-gradient … angle=45` — drifts along its own thickness, and
/// where the ramp is steep (measured 17 across four analysis pixels) it exceeded
/// both of the absolute limits this used to carry: the drift from the band's
/// first pixel, and the cap on a single step, which aborted the band outright.
/// Frames where the ramp happened to be gentle passed, so `window active --pixel`
/// found the window on some frames and fell through to the whole output on
/// others.
///
/// Against the delimiting jumps the same ramp is unambiguous: 17 of a 205-pixel
/// jump.  A flat stroke keeps its interior at 0, so it reads as a border just as
/// before, and what separates a border from two neighbouring edges is still the
/// contrast at its sides rather than any absolute constant.
const BAND_RAMP: i32 = 4;
/// Share of the output a band run must span to be a window edge.
const RING_LINE_MIN_PCT: u64 = 12;
/// Two edge candidates belong to the same window when they overlap this much.
const RING_OVERLAP_PCT: u64 = 60;
/// Smallest ring side, as a percent of the output.
const RING_MIN_DIM_PCT: u32 = 12;
/// Share of the ring's other dimension a side line has to span to close it.
/// A window's border runs the whole side — rounded corners only trim a corner
/// radius — while what sits inside a window is shorter: measured on a real
/// kitty window, its side borders spanned 95% and 96% of the ring height while
/// a scrollbar inside it spanned 13% and used to be mistaken for the right
/// border, cropping 14 logical pixels off the window.
const RING_SIDE_PCT: u64 = 60;
/// Mean ring saturation that reads as the compositor's *focused* stroke rather
/// than its inactive one.  Measured 162 against 0.8 on a real desktop, so this
/// sits far below any plausible confusion.
const RING_VIVID: i32 = 48;
/// Pixels read per stroke when a ring's saturation is measured; the stride
/// grows with the ring so the cost stays flat.
const SAMPLES_PER_STROKE: u32 = 64;
/// Rings kept per output.  A frame that is itself a grid of thin rectangles —
/// a photo of a screen, a wall of nested panels — pairs its edge lines into
/// thousands of rectangles; only the most vivid handful can be the
/// compositor's focus stroke.
const MAX_RINGS: usize = 32;
/// Neighbor channel delta a locally-smooth background flood may cross.
const SEG_JOIN_DIFF: i32 = 10;
/// Smallest segmentation candidate: area percent and per-dimension percent.
const SEG_MIN_AREA_PCT: u64 = 4;
const SEG_MIN_DIM_PCT: u32 = 20;

/// How much a candidate is trusted, best first.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
enum Tier {
    /// Four border bands that close on each other, stroked in the compositor's
    /// *focus* colour: the focused window, read straight off its border.
    Ring,
    /// A region the background flood fill separated from its output — and, on
    /// the same rung, a ring whose stroke is gray, which is a rectangle of the
    /// right shape but no evidence of focus.
    Segment,
    /// A closed accent outline big enough to be a window.
    Outline,
    /// The whole output, for an output that reads as one surface.
    WholeOutput,
}

/// A window candidate in global logical coordinates, with the trust its
/// detector deserves.
struct Candidate {
    tier: Tier,
    geometry: Rect,
    under_cursor: bool,
    /// The candidate sits on the output the pointer is on.
    on_cursor_output: bool,
    /// A focused border stroke: vivid where its neighbours are gray.  Ranked
    /// ahead of the pointer's output because a coloured ring says *this* window
    /// has keyboard focus, which is what `window active` asks.
    focused: bool,
    area: u64,
}

/// Detects the active window from the captured scene. Returns global logical
/// geometry suitable for `SceneSnapshot::crop`.
pub fn detect_active_window(scene: &SceneSnapshot, cursor: Option<Point>) -> Result<ActiveWindow> {
    // Which output the pointer is on.  A frame without metadata carries no
    // focus signal of its own — a compositor that strokes the focused window in
    // a colour can be read, one that paints it gray (or hides the border behind
    // a fullscreen window) cannot — so "the window you are pointing at" is the
    // best available reading of *active*.
    let home = cursor.and_then(|point| {
        scene
            .outputs()
            .iter()
            .find(|output| output.geometry.contains(point))
            .map(|output| output.global_id)
    });
    let mut pool: Vec<Candidate> = Vec::new();
    for output in scene.outputs() {
        let factor = analysis_factor(output.frame.size());
        let analysis = AnalysisFrame::sample(&output.frame, factor);
        let local_cursor = cursor.and_then(|point| to_local(&analysis, factor, output, point));
        pool.extend(
            output_candidates(&output.name, &analysis, local_cursor)
                .into_iter()
                .filter_map(|(tier, focused, rect)| {
                    Some((tier, focused, to_global(output, factor, rect)?))
                })
                .map(|(tier, focused, geometry)| Candidate {
                    tier,
                    focused,
                    under_cursor: cursor.is_some_and(|point| geometry.contains(point)),
                    on_cursor_output: home.is_some_and(|id| id == output.global_id),
                    area: area(geometry),
                    geometry,
                }),
        );
    }
    // The best rung of one output's ladder beats a lower rung of another's — a
    // degraded "everything on this screen" crop never outranks a real window
    // elsewhere, which is what keeps a bare desktop under the pointer from
    // hiding the window on the next monitor.  Within a rung, a focused stroke
    // wins, then the pointer's output, then the window under the pointer, and
    // area breaks the rest.
    pool.sort_by_key(|candidate| {
        (
            candidate.tier,
            !candidate.focused,
            !candidate.on_cursor_output,
            !candidate.under_cursor,
            Reverse(candidate.area),
        )
    });
    let geometry = pool
        .first()
        .map(|candidate| candidate.geometry)
        .ok_or_else(|| {
            VshotError::ActiveWindowUnavailable(
                "pixel fallback found no accent outline and no separable window region \
                 (borderless windows with no gaps or shadows carry no pixel signal)"
                    .into(),
            )
        })?;
    Ok(ActiveWindow {
        geometry,
        source: WindowSource::Pixel,
    })
}

/// Every window candidate both detectors can see, on every output, in global
/// logical coordinates, largest first.
///
/// Interactive picking shows these while the pointer moves, so it wants the
/// whole set rather than one detector's best guess — the user, not the
/// heuristic, decides between overlapping candidates.  An empty result means
/// the outputs carry no window signal at all (seamless borderless tiling).
///
/// Largest first doubles as the stacking order the picker reads: it takes the
/// *last* candidate under the pointer, so of two overlapping regions the
/// innermost — the smaller, which is where the stronger signal is — wins.  The
/// pixels cannot say which window is on top; nothing about a screenshot does.
pub fn detect_window_candidates(scene: &SceneSnapshot) -> Vec<Rect> {
    let mut logical: Vec<Rect> = Vec::new();
    for output in scene.outputs() {
        let factor = analysis_factor(output.frame.size());
        let analysis = AnalysisFrame::sample(&output.frame, factor);
        let segments = segment_components(&analysis);
        let mut rects: Vec<Rect> = ring_candidates(&analysis)
            .iter()
            .map(|ring| ring.rect)
            .collect();
        rects.extend(segments.candidates.iter().copied());
        rects.extend(
            outline_candidates(&analysis)
                .into_iter()
                .filter(|rect| window_sized(*rect, &analysis)),
        );
        rects.extend(segments.minor);
        logical.extend(
            rects
                .into_iter()
                .filter_map(|rect| to_global(output, factor, rect)),
        );
    }
    logical.sort_by_key(|rect| Reverse(area(*rect)));
    logical.dedup();
    logical
}

/// The one output's candidates, as the rungs of a ladder: whatever the
/// strongest detector that found something yields, each with the "this is the
/// focused stroke" flag its rung can claim.
///
/// Only a *vivid* ring reaches the [`Tier::Ring`] rung.  A gray one is the
/// compositor's inactive stroke — or a rectangle in the desktop picture, which
/// looks the same — and it says nothing about which window is focused, so it
/// takes its place among the segmented regions instead of outranking a real
/// window on another output.
fn output_candidates(
    name: &str,
    analysis: &AnalysisFrame,
    cursor: Option<Point>,
) -> Vec<(Tier, bool, Rect)> {
    let rings = ring_candidates(analysis);
    let mut ladder: Vec<(Tier, bool, Rect)> = rings
        .iter()
        .filter(|ring| ring.focused())
        .map(|ring| (Tier::Ring, ring.focused(), ring.rect))
        .collect();
    let segments = ladder.is_empty().then(|| segment_components(analysis));
    if let Some(segments) = &segments {
        ladder.extend(
            rings
                .iter()
                .filter(|ring| !ring.focused())
                .map(|ring| (Tier::Segment, false, ring.rect)),
        );
        ladder.extend(
            segments
                .candidates
                .iter()
                .map(|rect| (Tier::Segment, false, *rect)),
        );
        if ladder.is_empty() {
            // A lone small window is still a valid answer; several are ambiguous
            // unless the cursor settles them.
            ladder.extend(
                segments
                    .lone_minor(cursor)
                    .map(|rect| (Tier::Segment, false, rect)),
            );
        }
    }
    if ladder.is_empty() {
        ladder.extend(
            outline_candidates(analysis)
                .into_iter()
                .filter(|rect| window_sized(*rect, analysis))
                .map(|rect| (Tier::Outline, false, rect)),
        );
    }
    if ladder.is_empty() {
        ladder.extend(
            segments
                .as_ref()
                .and_then(|segments| segments.degraded_whole_output(analysis))
                .map(|rect| (Tier::WholeOutput, false, rect)),
        );
    }
    if debug_enabled() {
        eprintln!(
            "vshot: pixel detection on {name}: analysis {}x{} -> {} ring(s), {} segment candidate(s), {} minor, bg {}%",
            analysis.width,
            analysis.height,
            rings.len(),
            segments.as_ref().map_or(0, |segments| segments.candidates.len()),
            segments.as_ref().map_or(0, |segments| segments.minor.len()),
            segments.as_ref().map_or(100, |segments| segments.background_percent),
        );
        for ring in &rings {
            eprintln!(
                "vshot:   ring {:?} saturation {}{}",
                ring.rect,
                ring.saturation,
                if ring.focused() {
                    " (focused stroke)"
                } else {
                    ""
                }
            );
        }
        if let Some(segments) = &segments {
            for rect in &segments.candidates {
                eprintln!("vshot:   segment {rect:?}");
            }
        }
        for (tier, _, rect) in &ladder {
            eprintln!("vshot:   answers {tier:?} {rect:?}");
        }
    }
    ladder
}

/// Development aid: `VSHOT_PIXEL_DEBUG=1` reports, per output, what each rung
/// of the ladder saw — so a wrong crop can be traced to the detector that
/// offered it without rebuilding the analysis by hand.
fn debug_enabled() -> bool {
    std::env::var_os("VSHOT_PIXEL_DEBUG").is_some()
}

/// A window read off its border: the rectangle the stroke encloses — the whole
/// window, stroke included — and how vivid that stroke is.
struct Ring {
    rect: Rect,
    saturation: i32,
}

impl Ring {
    /// Whether this ring is the compositor's focus stroke rather than its
    /// inactive one.
    fn focused(&self) -> bool {
        self.saturation >= RING_VIVID
    }
}

/// Thin constant bands in one analysis frame, per direction.  A non-zero entry
/// marks the first row (or column) of a band and holds its thickness: a
/// compositor's border is exactly that — a few device pixels wide, uniform
/// along its length, with a sharp change on both sides.
struct Bands {
    /// Indexed `y * width + x`: the band whose first row is `y`.
    horizontal: Vec<u8>,
    /// Indexed `x * height + y`: the band whose first column is `x`.
    vertical: Vec<u8>,
}

fn bands(frame: &AnalysisFrame) -> Bands {
    let pixels = (frame.width * frame.height) as usize;
    let mut bands = Bands {
        horizontal: vec![0u8; pixels],
        vertical: vec![0u8; pixels],
    };
    for x in 0..frame.width {
        mark_bands(
            frame.height as usize,
            x as usize,
            frame.width as usize,
            &|y| frame.get(x, y as u32),
            &mut bands.horizontal,
        );
    }
    for y in 0..frame.height {
        mark_bands(
            frame.width as usize,
            y as usize,
            frame.height as usize,
            &|x| frame.get(x as u32, y),
            &mut bands.vertical,
        );
    }
    bands
}

/// Walks one line of the frame — a column for horizontal bands, a row for
/// vertical ones — and marks every thin band it starts.  `offset` and `stride`
/// place the marks in the direction's own index space.
fn mark_bands(
    length: usize,
    offset: usize,
    stride: usize,
    pixel_at: &dyn Fn(usize) -> Option<[u8; 4]>,
    out: &mut [u8],
) {
    if length < 3 {
        return;
    }
    let line: Vec<[u8; 4]> = (0..length)
        .map(|at| pixel_at(at).unwrap_or([0, 0, 0, 0]))
        .collect();
    let jump = |at: usize| channel_delta(line[at], line[at + 1]);
    let mut start = 1usize;
    while start + 1 < length {
        let opening = jump(start - 1);
        if opening < BAND_JUMP {
            start += 1;
            continue;
        }
        // The band opened at `start`; find where it closes.
        let mut thickness = 0usize;
        for t in 1..=MAX_BORDER as usize {
            if start + t >= length {
                break;
            }
            let closing = jump(start + t - 1);
            if closing < BAND_JUMP {
                continue;
            }
            // Neither the band's rows nor any single step inside it may change
            // by as much as a fraction of the jumps that open and close it: a
            // border is what changes far less than the edges around it, whether
            // it is painted flat or as a ramp.
            let limit = opening.min(closing) / BAND_RAMP;
            let ramps = line[start..start + t]
                .iter()
                .all(|pixel| channel_delta(*pixel, line[start]) <= limit);
            let calm = (1..t).all(|i| jump(start + i - 1) <= limit);
            if ramps && calm {
                thickness = t;
                break;
            }
        }
        if thickness == 0 {
            start += 1;
            continue;
        }
        out[offset + start * stride] = thickness as u8;
        start += thickness;
    }
}

/// A long run of band starts along one row or column: a window edge candidate.
struct Line {
    /// The row (for horizontal edges) or column (for vertical ones) it sits on.
    at: u32,
    from: u32,
    to: u32,
    thickness: u32,
    horizontal: bool,
}

impl Line {
    fn span(&self) -> u32 {
        self.to - self.from
    }
}

/// Every long run of band starts along a row or column: a window edge candidate.
///
/// A row yields **every** qualifying run rather than its longest one.  Two
/// tiled windows share the rows their top and bottom edges sit on, so a row
/// holds one run per window; keeping only the longest silently discards the
/// narrower window's edge, and with it the whole window — measured on a real
/// niri desktop, `window active --pixel` returned the entire output for all ten
/// frames whenever the focused window was the narrower of the two.
///
/// Keeping them all does not by itself make a wrong answer possible: an extra
/// run is only an extra *line*, and a ring still needs four sides that close on
/// each other with a matched stroke width ([`ring_candidates`]).
fn edge_lines(frame: &AnalysisFrame, bands: &Bands) -> Vec<Line> {
    let mut lines = Vec::new();
    for horizontal in [true, false] {
        let (outer, inner) = if horizontal {
            (frame.height, frame.width)
        } else {
            (frame.width, frame.height)
        };
        let min_run = u64::from(inner) * RING_LINE_MIN_PCT / 100;
        for at in 0..outer {
            let thickness_at = |along: u32| {
                if horizontal {
                    bands.horizontal[(at * frame.width + along) as usize]
                } else {
                    bands.vertical[(at * frame.height + along) as usize]
                }
            };
            let mut along = 0u32;
            while along < inner {
                if thickness_at(along) == 0 {
                    along += 1;
                    continue;
                }
                let from = along;
                let mut counts = [0u32; MAX_BORDER as usize + 1];
                while along < inner && thickness_at(along) > 0 {
                    counts[usize::from(thickness_at(along).min(MAX_BORDER as u8))] += 1;
                    along += 1;
                }
                let to = along - 1;
                if u64::from(to - from) < min_run {
                    continue;
                }
                let thickness = counts
                    .iter()
                    .enumerate()
                    .skip(1)
                    .max_by_key(|(_, count)| *count)
                    .map_or(1, |(thickness, _)| thickness as u32);
                lines.push(Line {
                    at,
                    from,
                    to,
                    thickness,
                    horizontal,
                });
            }
        }
    }
    lines
}

/// Windows read off border bands: two long edge lines facing each other, closed
/// by two more.  A page is full of rectangles, so all four sides have to be
/// there, and they have to be the compositor's thin constant band.  The
/// rectangle handed back spans the stroke's outer edge on all four sides.
fn ring_candidates(frame: &AnalysisFrame) -> Vec<Ring> {
    let bands = bands(frame);
    let lines = edge_lines(frame, &bands);
    let min_side = frame.width.min(frame.height) * RING_MIN_DIM_PCT / 100;
    let mut rings: Vec<Ring> = Vec::new();
    let horizontals: Vec<&Line> = lines.iter().filter(|line| line.horizontal).collect();
    let verticals: Vec<&Line> = lines.iter().filter(|line| !line.horizontal).collect();
    for (index, top) in horizontals.iter().enumerate() {
        for bottom in &horizontals[index + 1..] {
            if bottom.at <= top.at + min_side {
                continue;
            }
            if overlap(top, bottom).is_none() {
                continue;
            }
            let height = bottom.at - top.at;
            let tolerance = (height / 8).max(MAX_BORDER + 2);
            // A side of the ring is a long line, not whatever short vertical
            // bands the window's own content happens to contain.
            let side_span = min_side.max((u64::from(height) * RING_SIDE_PCT / 100) as u32);
            // Each horizontal edge finds its own corners, and the two edges have
            // to agree on which borders they found: a line that runs on past the
            // ring belongs to a neighbouring window sharing the row, not to this
            // window's top edge.
            let corners = |line: &Line| {
                let left = closer(
                    &verticals, line.from, top.at, bottom.at, side_span, tolerance,
                )?;
                let right = closer(&verticals, line.to, top.at, bottom.at, side_span, tolerance)?;
                Some((left, right))
            };
            let Some((left, right)) = corners(top) else {
                continue;
            };
            if corners(bottom).is_none_or(|(bottom_left, bottom_right)| {
                bottom_left.at != left.at || bottom_right.at != right.at
            }) {
                continue;
            }
            if right.at <= left.at + left.thickness + min_side {
                continue;
            }
            // All four sides are one compositor stroke, so they are the same
            // width.  This is what keeps a rectangle a *window* now that every
            // run of a row is a line: rows of unrelated content pair off into
            // rectangles too, and the widest of those would otherwise outrank
            // the real window on area alone (measured: a wallpaper seam paired
            // with a window's bottom edge produced a taller rectangle that won
            // on every frame).
            let sides = [
                top.thickness,
                bottom.thickness,
                left.thickness,
                right.thickness,
            ];
            if sides.iter().max() != sides.iter().min() {
                continue;
            }
            // The ring *is* the compositor's stroke, and the screenshot wants
            // the window as it looks on screen — stroke included — so the crop
            // starts at the stroke's outer edge rather than inside it.
            let rect = Rect::new(
                left.at as i32,
                top.at as i32,
                right.at + right.thickness - left.at,
                bottom.at + bottom.thickness - top.at,
            );
            let saturation = ring_saturation(frame, left, top, right, bottom);
            if rings.iter().any(|ring: &Ring| ring.rect == rect) {
                continue;
            }
            if rings.len() < MAX_RINGS {
                rings.push(Ring { rect, saturation });
                continue;
            }
            // Full: the least vivid ring gives way, so the frame's best
            // candidates survive however many rectangles it offers.
            let weakest = rings
                .iter()
                .enumerate()
                .min_by_key(|(_, ring)| ring.saturation)
                .map(|(index, _)| index)
                .filter(|weakest| rings[*weakest].saturation < saturation);
            if let Some(weakest) = weakest {
                rings[weakest] = Ring { rect, saturation };
            }
        }
    }
    rings.sort_by_key(|ring| Reverse(ring.saturation));
    rings
}

/// The x range two horizontal edge lines share, when they share enough of it.
fn overlap(top: &Line, bottom: &Line) -> Option<(u32, u32)> {
    let from = top.from.max(bottom.from);
    let to = top.to.min(bottom.to);
    if to <= from {
        return None;
    }
    let smaller = top.span().min(bottom.span());
    (u64::from(to - from) * 100 >= u64::from(smaller) * RING_OVERLAP_PCT).then_some((from, to))
}

/// The vertical edge line nearest `x` that runs along the given rows.
///
/// "Nearest" rather than "at": a window's corners are rounded, so a horizontal
/// edge line stops short of the vertical border it belongs to by roughly the
/// corner radius.  The tolerance is therefore a share of the ring's own height —
/// the true border is always nearer to the line's end than a neighbour window's
/// border is, because that distance is the corner radius versus the window gap.
/// What the window *contains* is nearer still, so a line only counts as a side
/// when it spans `min_span` of the ring — the length of a whole side.
fn closer<'a>(
    verticals: &[&'a Line],
    x: u32,
    from: u32,
    to: u32,
    min_span: u32,
    tolerance: u32,
) -> Option<&'a Line> {
    let mut found: Option<(&Line, u32)> = None;
    for line in verticals {
        if line.span() < min_span {
            continue;
        }
        if line.from > to || line.to < from {
            continue;
        }
        let distance = i64::from(line.at).abs_diff(i64::from(x)) as u32;
        if distance > tolerance {
            continue;
        }
        if found.is_none_or(|(_best, best_distance)| distance < best_distance) {
            found = Some((line, distance));
        }
    }
    found.map(|(line, _)| line)
}

/// Mean saturation along the four bands of a ring, sampled inside the ring's
/// own bounds so a neighbour's gray border sharing the row cannot dilute it.
///
/// The sample stride grows with the ring: a long stroke is no more informative
/// than a short one, and one frame can offer thousands of rings — a picture of
/// a desktop, a UI of nested panels — so the cost of reading one has to stay
/// flat rather than follow its perimeter.
fn ring_saturation(
    frame: &AnalysisFrame,
    left: &Line,
    top: &Line,
    right: &Line,
    bottom: &Line,
) -> i32 {
    let across = (right.at.saturating_sub(left.at)).max(1);
    let down = (bottom.at.saturating_sub(top.at)).max(1);
    let step_x = (across / SAMPLES_PER_STROKE).max(1) as usize;
    let step_y = (down / SAMPLES_PER_STROKE).max(1) as usize;
    let mut sum = 0u64;
    let mut samples = 0u64;
    let mut sample = |x: u32, y: u32| {
        if let Some(pixel) = frame.get(x, y) {
            sum += u64::from(saturation(pixel).max(0) as u32);
            samples += 1;
        }
    };
    for x in (left.at..right.at).step_by(step_x) {
        for y in top.at..top.at + top.thickness {
            sample(x, y);
        }
        for y in bottom.at..bottom.at + bottom.thickness {
            sample(x, y);
        }
    }
    for y in (top.at..bottom.at).step_by(step_y) {
        for x in left.at..left.at + left.thickness {
            sample(x, y);
        }
        for x in right.at..right.at + right.thickness {
            sample(x, y);
        }
    }
    if samples == 0 {
        return 0;
    }
    (sum / samples) as i32
}

/// Downsampling factor that keeps an output inside [`MAX_ANALYSIS_PIXELS`].
fn analysis_factor(size: Size) -> u32 {
    let mut factor = 1u32;
    while u64::from(size.width.div_ceil(factor)) * u64::from(size.height.div_ceil(factor))
        > MAX_ANALYSIS_PIXELS
    {
        factor += 1;
    }
    factor
}

/// Downscaled RGBA copy of one output's frame. Pixel `(x, y)` samples the
/// source block starting at `(x * factor, y * factor)`.
struct AnalysisFrame {
    width: u32,
    height: u32,
    pixels: Vec<[u8; 4]>,
}

impl AnalysisFrame {
    fn sample(frame: &Frame, factor: u32) -> Self {
        let source = frame.size();
        let width = (source.width / factor).max(1);
        let height = (source.height / factor).max(1);
        let mut pixels = Vec::with_capacity((width * height) as usize);
        for y in 0..height {
            let sy = (y * factor).min(source.height - 1);
            for x in 0..width {
                let sx = (x * factor).min(source.width - 1);
                pixels.push(
                    frame
                        .pixel(Point::new(sx as i32, sy as i32))
                        .unwrap_or([0, 0, 0, 0]),
                );
            }
        }
        Self {
            width,
            height,
            pixels,
        }
    }

    fn get(&self, x: u32, y: u32) -> Option<[u8; 4]> {
        if x >= self.width || y >= self.height {
            return None;
        }
        self.pixels.get((y * self.width + x) as usize).copied()
    }

    fn mask_index(&self, x: u32, y: u32) -> usize {
        (y * self.width + x) as usize
    }

    fn area(&self) -> u64 {
        u64::from(self.width) * u64::from(self.height)
    }
}

fn channel_delta(a: [u8; 4], b: [u8; 4]) -> i32 {
    (i32::from(a[0]) - i32::from(b[0]))
        .abs()
        .max((i32::from(a[1]) - i32::from(b[1])).abs())
        .max((i32::from(a[2]) - i32::from(b[2])).abs())
}

fn saturation(color: [u8; 4]) -> i32 {
    let max = color[0].max(color[1]).max(color[2]);
    let min = color[0].min(color[1]).min(color[2]);
    i32::from(max) - i32::from(min)
}

/// Sum-of-channels distance, the tolerance every color mask test uses.
fn color_distance(a: [u8; 4], b: [u8; 4]) -> i32 {
    (i32::from(a[0]) - i32::from(b[0])).abs()
        + (i32::from(a[1]) - i32::from(b[1])).abs()
        + (i32::from(a[2]) - i32::from(b[2])).abs()
}

fn area(rect: Rect) -> u64 {
    u64::from(rect.size.width) * u64::from(rect.size.height)
}

/// Converts a global logical point into one output's analysis coordinates;
/// `None` when the point is not on that output.
fn to_local(
    analysis: &AnalysisFrame,
    factor: u32,
    output: &OutputSnapshot,
    point: Point,
) -> Option<Point> {
    let bounds = output.geometry;
    if !bounds.contains(point) {
        return None;
    }
    let local = Point::new(
        ((i64::from(point.x) - i64::from(bounds.left())) * i64::from(output.scale)
            / i64::from(factor)) as i32,
        ((i64::from(point.y) - i64::from(bounds.top())) * i64::from(output.scale)
            / i64::from(factor)) as i32,
    );
    (local.x < analysis.width as i32 && local.y < analysis.height as i32).then_some(local)
}

/// Converts an analysis-space rect into the output's global logical geometry,
/// clamped to the output; `None` when nothing usable survives the clamping.
fn to_global(output: &OutputSnapshot, factor: u32, rect: Rect) -> Option<Rect> {
    let bounds = output.geometry;
    let scale = i64::from(output.scale);
    let source = output.frame.size();
    let device_x = i64::from(rect.origin.x) * i64::from(factor);
    let device_y = i64::from(rect.origin.y) * i64::from(factor);
    let device_width =
        (i64::from(rect.size.width) * i64::from(factor)).min(i64::from(source.width));
    let device_height =
        (i64::from(rect.size.height) * i64::from(factor)).min(i64::from(source.height));
    let round_div = |value: i64| i32::try_from((value + scale / 2) / scale).unwrap_or(0);
    let geometry = Rect::new(
        bounds.left() + round_div(device_x.clamp(0, i64::from(source.width))),
        bounds.top() + round_div(device_y.clamp(0, i64::from(source.height))),
        round_div(device_width).max(1) as u32,
        round_div(device_height).max(1) as u32,
    );
    // Keep the crop inside the output even after rounding.
    let geometry = geometry.clamp_to(bounds)?;
    (!geometry.is_empty()).then_some(geometry)
}

/// Every closed accent outline the frame yields, across all plausible border
/// colors.
fn outline_candidates(frame: &AnalysisFrame) -> Vec<Rect> {
    let histogram = Histogram::collect_edge_colors(frame);
    let mut fits: Vec<Rect> = Vec::new();
    for color in histogram.take_candidates(8) {
        fits.extend(fit_outlines_for_color(frame, color));
    }
    fits
}

/// Does an outline fit describe a window rather than a block of page content?
///
/// A frame is full of rectangles drawn in one consistent color — toolbars,
/// cards, images with a border — and a fit is only a window candidate once it
/// covers a window-sized share of the output.
fn window_sized(rect: Rect, frame: &AnalysisFrame) -> bool {
    area(rect) * 100 >= frame.area() * OUTLINE_MIN_AREA_PCT
}

/// Quantized-color histogram over edge pixels; both pixels of every differing
/// pair are recorded, so border pixels count against content and wallpaper.
struct Histogram {
    counts: [u32; 4096],
    sums: [[u64; 3]; 4096],
}

impl Histogram {
    fn collect_edge_colors(frame: &AnalysisFrame) -> Self {
        let mut histogram = Histogram {
            counts: [0; 4096],
            sums: [[0; 3]; 4096],
        };
        for y in 0..frame.height {
            for x in 0..frame.width {
                let Some(pixel) = frame.get(x, y) else {
                    continue;
                };
                if pixel[3] == 0 {
                    continue;
                }
                for (nx, ny) in [(x + 1, y), (x, y + 1)] {
                    let Some(neighbor) = frame.get(nx, ny) else {
                        continue;
                    };
                    if neighbor[3] == 0 || channel_delta(pixel, neighbor) < EDGE_DIFF {
                        continue;
                    }
                    histogram.record(pixel);
                    histogram.record(neighbor);
                }
            }
        }
        histogram
    }

    fn record(&mut self, pixel: [u8; 4]) {
        let bucket = bucket_of(pixel);
        self.counts[bucket] += 1;
        self.sums[bucket][0] += u64::from(pixel[0]);
        self.sums[bucket][1] += u64::from(pixel[1]);
        self.sums[bucket][2] += u64::from(pixel[2]);
    }

    /// Colors ranked by `count x saturation`, capped at `limit`.
    fn take_candidates(&self, limit: usize) -> Vec<[u8; 4]> {
        let mut ranked: Vec<(u64, usize)> = self
            .counts
            .iter()
            .enumerate()
            .filter(|(_, count)| **count > 0)
            .map(|(bucket, count)| {
                let score = u64::from(*count)
                    * u64::try_from(24 + saturation(self.average(bucket))).unwrap_or(24);
                (score, bucket)
            })
            .collect();
        ranked.sort_by(|a, b| b.0.cmp(&a.0).then(a.1.cmp(&b.1)));
        ranked
            .into_iter()
            .take(limit)
            .map(|(_, bucket)| self.average(bucket))
            .collect()
    }

    fn average(&self, bucket: usize) -> [u8; 4] {
        let count = self.counts[bucket].max(1);
        [
            (self.sums[bucket][0] / u64::from(count)) as u8,
            (self.sums[bucket][1] / u64::from(count)) as u8,
            (self.sums[bucket][2] / u64::from(count)) as u8,
            255,
        ]
    }
}

fn bucket_of(pixel: [u8; 4]) -> usize {
    ((usize::from(pixel[0]) >> 4) << 8)
        | ((usize::from(pixel[1]) >> 4) << 4)
        | (usize::from(pixel[2]) >> 4)
}

/// Builds the color mask and scans its connected components for every fitted
/// outline of that border color, in scan order.
fn fit_outlines_for_color(frame: &AnalysisFrame, color: [u8; 4]) -> Vec<Rect> {
    let mask: Vec<bool> = frame
        .pixels
        .iter()
        .map(|pixel| pixel[3] != 0 && color_distance(*pixel, color) <= COLOR_TOLERANCE)
        .collect();
    let min_width = frame.width * OUTLINE_MIN_DIM_PCT / 100;
    let min_height = frame.height * OUTLINE_MIN_DIM_PCT / 100;

    let mut visited = vec![false; frame.pixels.len()];
    let mut fits: Vec<Rect> = Vec::new();
    for y in 0..frame.height {
        for x in 0..frame.width {
            let index = frame.mask_index(x, y);
            if !mask[index] || visited[index] {
                continue;
            }
            let bbox = component_bbox(frame, &mask, &mut visited, x, y);
            if bbox.size.width < min_width || bbox.size.height < min_height {
                continue;
            }
            if let Some(rect) = fit_outline_in_bbox(&|x, y| mask[frame.mask_index(x, y)], bbox) {
                fits.push(rect);
            }
        }
    }
    fits
}

/// 4-connected bounding box of the `mask` component containing `(x, y)`;
/// marks every walked pixel in `visited`.
fn component_bbox(
    frame: &AnalysisFrame,
    mask: &[bool],
    visited: &mut [bool],
    start_x: u32,
    start_y: u32,
) -> Rect {
    let mut queue = VecDeque::new();
    queue.push_back((start_x, start_y));
    visited[frame.mask_index(start_x, start_y)] = true;
    let mut min_x = start_x;
    let mut max_x = start_x;
    let mut min_y = start_y;
    let mut max_y = start_y;
    while let Some((x, y)) = queue.pop_front() {
        min_x = min_x.min(x);
        max_x = max_x.max(x);
        min_y = min_y.min(y);
        max_y = max_y.max(y);
        for (nx, ny) in [
            (x.wrapping_sub(1), y),
            (x + 1, y),
            (x, y.wrapping_sub(1)),
            (x, y + 1),
        ] {
            if nx >= frame.width || ny >= frame.height {
                continue;
            }
            let index = frame.mask_index(nx, ny);
            if mask[index] && !visited[index] {
                visited[index] = true;
                queue.push_back((nx, ny));
            }
        }
    }
    Rect::new(
        min_x as i32,
        min_y as i32,
        max_x - min_x + 1,
        max_y - min_y + 1,
    )
}

/// Tries border depths 1..=MAX_BORDER inside `bbox`: the band inside depth `t`
/// must be almost fully `matches`-colored while the interior beyond stays
/// mostly empty.  Scoring naturally selects the smallest interior that is still
/// empty, i.e. the true border thickness.
fn fit_outline_in_bbox(matches: &impl Fn(u32, u32) -> bool, bbox: Rect) -> Option<Rect> {
    let left = bbox.left();
    let top = bbox.top();
    let right = left + bbox.size.width as i32;
    let bottom = top + bbox.size.height as i32;
    let mut best: Option<(f64, Rect)> = None;
    for t in 1..=MAX_BORDER {
        let mut band_total = 0u64;
        let mut band_hit = 0u64;
        let mut inner_total = 0u64;
        let mut inner_hit = 0u64;
        for y in top..bottom {
            for x in left..right {
                let dx = (x - left).min(right - 1 - x);
                let dy = (y - top).min(bottom - 1 - y);
                let hit = matches(x as u32, y as u32);
                if dx.min(dy) < t as i32 {
                    band_total += 1;
                    band_hit += u64::from(hit);
                } else {
                    inner_total += 1;
                    inner_hit += u64::from(hit);
                }
            }
        }
        if band_total == 0 {
            continue;
        }
        let coverage = band_hit as f64 / band_total as f64;
        let hollow = if inner_total > 0 {
            inner_hit as f64 / inner_total as f64
        } else {
            0.0
        };
        if coverage < OUTLINE_COVERAGE || hollow > OUTLINE_HOLLOW_MAX {
            continue;
        }
        let rect = Rect::new(
            left + t as i32,
            top + t as i32,
            bbox.size.width.saturating_sub(2 * t).max(1),
            bbox.size.height.saturating_sub(2 * t).max(1),
        );
        if rect.size.width < 8 || rect.size.height < 8 {
            continue;
        }
        let score = coverage - 0.5 * hollow;
        if best.is_none_or(|(current, _)| score > current) {
            best = Some((score, rect));
        }
    }
    best.map(|(_, rect)| rect)
}

/// What the background flood fill left over, split by confidence.
struct Segments {
    /// Components that pass the size thresholds.
    candidates: Vec<Rect>,
    /// Smaller components: a lone one is still a window, several are ambiguous.
    minor: Vec<Rect>,
    /// Percentage of the frame the flood fill reached from its edges.
    background_percent: u64,
    /// The frame reads as one uniform surface, with no internal seams.
    uniform: bool,
}

impl Segments {
    fn lone_minor(&self, cursor: Option<Point>) -> Option<Rect> {
        match self.minor.len() {
            1 => self.minor.first().copied(),
            0 => None,
            _ => cursor
                .and_then(|point| self.minor.iter().copied().find(|rect| rect.contains(point))),
        }
    }

    /// The whole output, but only for an essentially uniform one: one
    /// fullscreen window or a bare desktop.  Internal edges — a seam between
    /// seamlessly tiled windows, text — mean several windows we cannot split,
    /// and guessing "everything" there would crop unrelated windows together.
    fn degraded_whole_output(&self, frame: &AnalysisFrame) -> Option<Rect> {
        ((self.background_percent > 97 && self.uniform) || self.background_percent < 50)
            .then(|| Rect::new(0, 0, frame.width, frame.height))
    }
}

/// Runs the background flood fill and groups what it left into window
/// candidates.
fn segment_components(frame: &AnalysisFrame) -> Segments {
    let total = frame.pixels.len();
    let mut background = vec![false; total];
    let mut queue = VecDeque::new();
    for x in 0..frame.width {
        seed_background(frame, &mut background, &mut queue, x, 0);
        seed_background(frame, &mut background, &mut queue, x, frame.height - 1);
    }
    for y in 0..frame.height {
        seed_background(frame, &mut background, &mut queue, 0, y);
        seed_background(frame, &mut background, &mut queue, frame.width - 1, y);
    }
    while let Some(index) = queue.pop_front() {
        let x = (index as u32) % frame.width;
        let y = (index as u32) / frame.width;
        let pixel = frame.pixels[index];
        for (nx, ny) in [
            (x.wrapping_sub(1), y),
            (x + 1, y),
            (x, y.wrapping_sub(1)),
            (x, y + 1),
        ] {
            if nx >= frame.width || ny >= frame.height {
                continue;
            }
            let neighbor_index = frame.mask_index(nx, ny);
            if background[neighbor_index] {
                continue;
            }
            let neighbor = frame.pixels[neighbor_index];
            if neighbor[3] == 0 || pixel[3] == 0 || channel_delta(pixel, neighbor) <= SEG_JOIN_DIFF
            {
                background[neighbor_index] = true;
                queue.push_back(neighbor_index);
            }
        }
    }
    let background_pixels = background.iter().filter(|seed| **seed).count();

    let min_width = frame.width * SEG_MIN_DIM_PCT / 100;
    let min_height = frame.height * SEG_MIN_DIM_PCT / 100;
    let min_area = u64::from(frame.width) * u64::from(frame.height) * SEG_MIN_AREA_PCT / 100;
    let foreground: Vec<bool> = background.iter().map(|seed| !seed).collect();
    let mut assigned = vec![false; total];
    let mut candidates: Vec<Rect> = Vec::new();
    let mut minor: Vec<Rect> = Vec::new();
    let minor_min_area = u64::from(frame.width) * u64::from(frame.height) / 200;
    for y in 0..frame.height {
        for x in 0..frame.width {
            let index = frame.mask_index(x, y);
            if !foreground[index] || assigned[index] {
                continue;
            }
            let bbox = component_bbox(frame, &foreground, &mut assigned, x, y);
            let component_area = area(bbox);
            if bbox.size.width < min_width
                || bbox.size.height < min_height
                || component_area < min_area
            {
                // A lone small window is still a valid answer; several small
                // ones stay ambiguous unless the cursor settles it. Text
                // blocks (a few percent of one dimension) never qualify.
                if component_area >= minor_min_area
                    && bbox.size.width >= frame.width / 12
                    && bbox.size.height >= frame.height / 12
                {
                    minor.push(bbox);
                }
                continue;
            }
            candidates.push(bbox);
        }
    }

    let edge_pairs = Histogram::collect_edge_colors(frame)
        .counts
        .iter()
        .map(|count| u64::from(*count))
        .sum::<u64>()
        / 2;
    Segments {
        candidates,
        minor,
        background_percent: background_pixels as u64 * 100 / total as u64,
        uniform: edge_pairs * 1000 < u64::from(frame.width) * u64::from(frame.height),
    }
}

fn seed_background(
    frame: &AnalysisFrame,
    background: &mut [bool],
    queue: &mut VecDeque<usize>,
    x: u32,
    y: u32,
) {
    let index = frame.mask_index(x, y);
    if !background[index] {
        background[index] = true;
        queue.push_back(index);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::geometry::Size;
    use crate::model::{OutputSnapshot, SceneSnapshot};

    /// Solid-color canvas with rect fills, convertible to a one-output scene.
    struct Canvas {
        width: u32,
        height: u32,
        pixels: Vec<u8>,
    }

    impl Canvas {
        fn new(width: u32, height: u32, rgba: [u8; 4]) -> Self {
            let mut pixels = Vec::with_capacity((width * height * 4) as usize);
            for _ in 0..width * height {
                pixels.extend_from_slice(&rgba);
            }
            Self {
                width,
                height,
                pixels,
            }
        }

        fn fill(&mut self, x: i32, y: i32, width: u32, height: u32, rgba: [u8; 4]) {
            for row in y..(y + height as i32) {
                for column in x..(x + width as i32) {
                    if column < 0
                        || row < 0
                        || column >= self.width as i32
                        || row >= self.height as i32
                    {
                        continue;
                    }
                    let index = ((row as u32 * self.width + column as u32) * 4) as usize;
                    self.pixels[index..index + 4].copy_from_slice(&rgba);
                }
            }
        }

        fn scene(&self) -> SceneSnapshot {
            SceneSnapshot::from_outputs(vec![OutputSnapshot::new(
                1,
                "test",
                Rect::new(0, 0, self.width, self.height),
                1,
                Frame::new(Size::new(self.width, self.height), self.pixels.clone()).unwrap(),
            )
            .unwrap()])
            .unwrap()
        }
    }

    #[test]
    fn ring_reads_the_bordered_window_and_keeps_its_border() {
        let mut canvas = Canvas::new(400, 300, [40, 60, 80, 255]);
        // Content first, then a 3px accent border drawn over its rim.
        canvas.fill(100, 80, 200, 140, [200, 200, 200, 255]);
        canvas.fill(100, 80, 200, 3, [255, 140, 0, 255]);
        canvas.fill(100, 217, 200, 3, [255, 140, 0, 255]);
        canvas.fill(100, 80, 3, 140, [255, 140, 0, 255]);
        canvas.fill(297, 80, 3, 140, [255, 140, 0, 255]);
        let window = detect_active_window(&canvas.scene(), None).unwrap();
        assert_eq!(window.source, WindowSource::Pixel);
        // The window as it looks, stroke included — not the content inside it.
        assert_eq!(window.geometry, Rect::new(100, 80, 200, 140));
    }

    #[test]
    fn ring_reads_a_gradient_stroke_without_needing_a_flat_band() {
        // niri strokes the focused window with `active-gradient … angle=45`, so
        // the border ramps along its own thickness instead of holding one
        // colour.  Absolute limits on that interior — a fixed spread from the
        // band's first pixel, and a fixed cap on a single step — both broke
        // where the ramp was steep, and the frame's phase decides whether that
        // happened: `window active --pixel` found the window on some frames and
        // fell through to the whole output on others.
        let mut canvas = Canvas::new(400, 300, [40, 60, 80, 255]);
        canvas.fill(100, 80, 200, 140, [200, 200, 200, 255]);
        // A 4px ramp drifting 40 in total, 13–14 per step: rejected by both
        // absolute limits, and plainly a border against the 100+ jumps at its
        // sides.
        let ramp: [[u8; 4]; 4] = [
            [255, 0, 0, 255],
            [255, 13, 0, 255],
            [255, 27, 0, 255],
            [255, 40, 0, 255],
        ];
        for (t, color) in ramp.iter().enumerate() {
            let t = t as i32;
            canvas.fill(100, 80 + t, 200, 1, *color);
            canvas.fill(100, 219 - t, 200, 1, ramp[3 - t as usize]);
            canvas.fill(100 + t, 80, 1, 140, *color);
            canvas.fill(299 - t, 80, 1, 140, ramp[3 - t as usize]);
        }
        let window = detect_active_window(&canvas.scene(), None).unwrap();
        assert_eq!(window.source, WindowSource::Pixel);
        assert_eq!(window.geometry, Rect::new(100, 80, 200, 140));
    }

    #[test]
    fn ring_reads_the_stroke_of_a_narrow_window_beside_a_wider_one() {
        // Two tiled windows share their top and bottom rows.  A row keeps only
        // its *longest* run, so the wider neighbour's edge is the one that
        // survives; the narrower window still has to be read off its own sides,
        // which is what the ring's corner pairing does.
        let mut canvas = Canvas::new(400, 300, [40, 60, 80, 255]);
        canvas.fill(10, 40, 80, 220, [210, 210, 210, 255]);
        canvas.fill(110, 40, 250, 220, [200, 200, 200, 255]);
        // Strokes: the narrow window's is vivid, the wide one's is gray.
        let (narrow, wide) = ([255, 140, 0, 255], [90, 90, 90, 255]);
        for (x, y, width, height, stroke) in [(10, 40, 80, 220, narrow), (110, 40, 250, 220, wide)]
        {
            canvas.fill(x, y, width, 3, stroke);
            canvas.fill(x, y + height as i32 - 3, width, 3, stroke);
            canvas.fill(x, y, 3, height, stroke);
            canvas.fill(x + width as i32 - 3, y, 3, height, stroke);
        }
        // The vivid stroke marks the focused window even though the wide
        // neighbour's edge is the longer line on the shared rows.
        let window = detect_active_window(&canvas.scene(), None).unwrap();
        assert_eq!(window.geometry, Rect::new(10, 40, 80, 220));
    }

    #[test]
    fn ring_prefers_the_cursor_containing_window() {
        let mut canvas = Canvas::new(400, 300, [40, 60, 80, 255]);
        for (x, y, color) in [(30, 30, [255, 0, 0, 255]), (240, 170, [0, 90, 255, 255])] {
            canvas.fill(x, y, 120, 90, [210, 210, 210, 255]);
            canvas.fill(x, y, 120, 2, color);
            canvas.fill(x, y + 88, 120, 2, color);
            canvas.fill(x, y, 2, 90, color);
            canvas.fill(x + 118, y, 2, 90, color);
        }
        // Both strokes are vivid, so neither window wins on colour and the
        // pointer settles it.
        let window = detect_active_window(&canvas.scene(), Some(Point::new(300, 210))).unwrap();
        assert_eq!(window.geometry, Rect::new(240, 170, 120, 90));
    }

    #[test]
    fn borderless_windows_segment_with_cursor_disambiguation() {
        let mut canvas = Canvas::new(400, 300, [30, 30, 30, 255]);
        canvas.fill(150, 60, 210, 180, [220, 220, 220, 255]);
        canvas.fill(10, 10, 90, 90, [90, 90, 90, 255]);
        // The cursor picks window A even though B is a valid candidate too.
        let window = detect_active_window(&canvas.scene(), Some(Point::new(250, 150))).unwrap();
        assert_eq!(window.geometry, Rect::new(150, 60, 210, 180));
        // B is above the size thresholds as well, so the cursor over B wins.
        let window = detect_active_window(&canvas.scene(), Some(Point::new(40, 40))).unwrap();
        assert_eq!(window.geometry, Rect::new(10, 10, 90, 90));
        // Without a cursor the larger window wins.
        let window = detect_active_window(&canvas.scene(), None).unwrap();
        assert_eq!(window.geometry, Rect::new(150, 60, 210, 180));
    }

    #[test]
    fn lone_small_borderless_window_is_accepted() {
        let mut canvas = Canvas::new(400, 300, [30, 30, 30, 255]);
        canvas.fill(20, 20, 50, 40, [180, 180, 180, 255]);
        let window = detect_active_window(&canvas.scene(), None).unwrap();
        assert_eq!(window.geometry, Rect::new(20, 20, 50, 40));
    }

    #[test]
    fn uniform_fullscreen_content_degrades_to_the_whole_frame() {
        let canvas = Canvas::new(400, 300, [50, 90, 130, 255]);
        let window = detect_active_window(&canvas.scene(), None).unwrap();
        assert_eq!(window.geometry, Rect::new(0, 0, 400, 300));
    }

    #[test]
    fn seamless_tiling_of_several_windows_reports_failure() {
        let mut canvas = Canvas::new(400, 300, [100, 100, 100, 255]);
        canvas.fill(200, 0, 200, 300, [200, 200, 200, 255]);
        let error = detect_active_window(&canvas.scene(), None).unwrap_err();
        assert!(error.to_string().contains("pixel fallback"), "{error}");
    }

    /// Two windows on a dark wallpaper, each stroked by the compositor: the
    /// focused one in orange, the other in gray.  The stroke is read off its
    /// border bands, so the crop is the window's own rectangle, and the vivid
    /// stroke settles which of the two is active.
    #[test]
    fn ring_reads_the_stroked_window_and_prefers_the_vivid_stroke() {
        let mut canvas = Canvas::new(400, 300, [40, 60, 80, 255]);
        let windows: [(i32, i32, u32, u32, [u8; 4]); 2] = [
            (20, 40, 160, 220, [255, 140, 0, 255]),
            (220, 60, 160, 180, [90, 90, 90, 255]),
        ];
        for (x, y, width, height, stroke) in windows {
            canvas.fill(x, y, width, height, [200, 200, 200, 255]);
            canvas.fill(x, y, width, 3, stroke);
            canvas.fill(x, y + height as i32 - 3, width, 3, stroke);
            canvas.fill(x, y, 3, height, stroke);
            canvas.fill(x + width as i32 - 3, y, 3, height, stroke);
        }
        // Only the vivid stroke is a ring the ladder will use; it wins without
        // any pointer help.
        assert_eq!(
            detect_active_window(&canvas.scene(), None)
                .unwrap()
                .geometry,
            Rect::new(20, 40, 160, 220)
        );
        // A vivid stroke says *this* window is focused, so it outranks the
        // window merely under the pointer — but the pointer still has to be
        // able to pick that window by hand, which is what the candidates below
        // are for.
        assert_eq!(
            detect_active_window(&canvas.scene(), Some(Point::new(300, 150)))
                .unwrap()
                .geometry,
            Rect::new(20, 40, 160, 220)
        );
        // Picking is offered both windows, stroke included.
        let candidates = detect_window_candidates(&canvas.scene());
        assert!(
            candidates.contains(&Rect::new(20, 40, 160, 220)),
            "{candidates:?}"
        );
        assert!(
            candidates.contains(&Rect::new(220, 60, 160, 180)),
            "{candidates:?}"
        );
    }

    /// A gray ring is a rectangle of the right shape, not evidence of focus.
    /// Read as a border it used to outrank a real window on the other output —
    /// which is how a rectangle in the desktop picture on an idle monitor could
    /// answer for the window the pointer was on.
    #[test]
    fn a_gray_ring_does_not_outrank_another_outputs_window() {
        let mut left = Canvas::new(400, 300, [40, 60, 80, 255]);
        left.fill(20, 40, 160, 220, [200, 200, 200, 255]);
        for (x, y, width, height) in [(20, 40, 160, 3), (20, 257, 160, 3), (20, 40, 3, 220)] {
            left.fill(x, y, width, height, [90, 90, 90, 255]);
        }
        left.fill(177, 40, 3, 220, [90, 90, 90, 255]);
        let mut right = Canvas::new(400, 300, [30, 30, 30, 255]);
        right.fill(150, 60, 210, 180, [220, 220, 220, 255]);
        let scene = SceneSnapshot::from_outputs(vec![
            OutputSnapshot::new(
                1,
                "left",
                Rect::new(0, 0, 400, 300),
                1,
                Frame::new(Size::new(400, 300), left.pixels.clone()).unwrap(),
            )
            .unwrap(),
            OutputSnapshot::new(
                2,
                "right",
                Rect::new(400, 0, 400, 300),
                1,
                Frame::new(Size::new(400, 300), right.pixels.clone()).unwrap(),
            )
            .unwrap(),
        ])
        .unwrap();
        // The pointer is on the right output, whose window carries no stroke.
        assert_eq!(
            detect_active_window(&scene, Some(Point::new(600, 150)))
                .unwrap()
                .geometry,
            Rect::new(550, 60, 210, 180)
        );
        // The gray-stroked window is still offered for picking by hand.
        let candidates = detect_window_candidates(&scene);
        assert!(
            candidates.contains(&Rect::new(20, 40, 160, 220)),
            "{candidates:?}"
        );
    }

    #[test]
    fn detection_never_spans_outputs() {
        // A scale-1 output beside a scale-2 one, each with a window on a plain
        // wallpaper.  Detecting on the composed scene used to walk the flood
        // fill across the monitor seam and hand back one candidate covering
        // the whole desktop.
        let mut left = Canvas::new(400, 300, [30, 30, 30, 255]);
        left.fill(20, 20, 180, 120, [220, 220, 220, 255]);
        let mut right = Canvas::new(800, 600, [30, 30, 30, 255]);
        // Inset from the frame's edges: the background flood is seeded from
        // every edge pixel, so a window that touches one is absorbed with it.
        right.fill(150, 100, 500, 400, [210, 210, 210, 255]);
        let scene = SceneSnapshot::from_outputs(vec![
            OutputSnapshot::new(
                1,
                "left",
                Rect::new(0, 0, 400, 300),
                1,
                Frame::new(Size::new(400, 300), left.pixels.clone()).unwrap(),
            )
            .unwrap(),
            OutputSnapshot::new(
                2,
                "right",
                Rect::new(400, 0, 400, 300),
                2,
                Frame::new(Size::new(800, 600), right.pixels.clone()).unwrap(),
            )
            .unwrap(),
        ])
        .unwrap();

        // The pointer decides which output is meant — the right window is the
        // bigger one, so the left answer can only come from the pointer's
        // output — and each answer is in that output's own coordinates: the
        // scale-2 window maps back to half its device size.
        assert_eq!(
            detect_active_window(&scene, Some(Point::new(100, 80)))
                .unwrap()
                .geometry,
            Rect::new(20, 20, 180, 120)
        );
        assert_eq!(
            detect_active_window(&scene, Some(Point::new(600, 150)))
                .unwrap()
                .geometry,
            Rect::new(475, 50, 250, 200)
        );
        // Without a pointer to place the intent, the bigger window wins.
        assert_eq!(
            detect_active_window(&scene, None).unwrap().geometry,
            Rect::new(475, 50, 250, 200)
        );
        // Nothing offered for picking crosses the seam either.
        assert!(detect_window_candidates(&scene)
            .iter()
            .all(|rect| rect.size.width <= 400));
    }

    #[test]
    fn candidates_cover_every_window_for_picking() {
        let mut canvas = Canvas::new(400, 300, [30, 30, 30, 255]);
        canvas.fill(150, 60, 210, 180, [220, 220, 220, 255]);
        canvas.fill(10, 10, 90, 90, [90, 90, 90, 255]);
        // Largest first, so hovering can simply take the last candidate that
        // contains the pointer (the innermost window wins).
        assert_eq!(
            detect_window_candidates(&canvas.scene()),
            vec![Rect::new(150, 60, 210, 180), Rect::new(10, 10, 90, 90)]
        );
    }

    #[test]
    fn candidates_stay_empty_without_a_pixel_signal() {
        // Seamless borderless tiling: nothing to offer, honestly empty.
        let mut canvas = Canvas::new(400, 300, [100, 100, 100, 255]);
        canvas.fill(200, 0, 200, 300, [200, 200, 200, 255]);
        assert!(detect_window_candidates(&canvas.scene()).is_empty());
    }

    #[test]
    fn detection_maps_through_the_scene_scale() {
        // One scale-2 output: logical 200x150, device 400x300. The window is
        // painted in device pixels and must map back to logical ones.
        let mut device = Canvas::new(400, 300, [30, 30, 30, 255]);
        device.fill(100, 80, 200, 140, [220, 220, 220, 255]);
        let scene = SceneSnapshot::from_outputs(vec![OutputSnapshot::new(
            1,
            "hidpi",
            Rect::new(0, 0, 200, 150),
            2,
            Frame::new(Size::new(400, 300), device.pixels.clone()).unwrap(),
        )
        .unwrap()])
        .unwrap();
        let window = detect_active_window(&scene, None).unwrap();
        assert_eq!(window.geometry, Rect::new(50, 40, 100, 70));
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Reading a window by the lines drawn in it.
//!
//! This is the port of the prototype's Gen14.  The idea is that an interface's
//! structure is *drawn*: a divider is a run of pixels that is long one way and
//! thin the other, and the rectangles those dividers enclose are its panes.
//! Finding them takes four steps.
//!
//! **1. Edges.**  Where neighbouring pixels differ by [`EDGE_JUMP`] or more in
//! luminance.  Measured on a real window, the dividers between its panes answer
//! 60 to 75 while a themed background's own texture answers under 4.
//!
//! **2. Runs.**  Each edge pixel is measured along both axes.  A divider is
//! *long one way and thin the other* — measured, a real divider is 1376 long
//! and 2 wide, while a glyph stroke is 9 long and 17 wide.  Two orders of
//! magnitude apart, and it needs no morphological operation, which matters:
//! opening a window to remove text also removes the dividers, because a
//! divider and a stroke are both thin.  That was tried and measured — a
//! divider's luminance went from 70 to 21.
//!
//! **3. Segments.**  Runs that survive are grouped into segments, and segments
//! of the same line are merged across the gaps left by the content drawn over
//! them.  The gap test is a colour test, not a length one: a line broken by
//! text has that text's colour in the gap, while two genuinely different lines
//! have each other's background in theirs.
//!
//! **4. Cut.**  The segments divide the window by recursive cutting, and the
//! recursion *is* the tree: the two halves of a cut are siblings and the region
//! that was cut is their parent.  A cut is allowed when the line covers enough
//! of the region, measured with the corners excused — a rounded corner bends
//! the border away, so a line legitimately stops short of the region's ends.
//!
//! What it cannot do: an interface that draws no dividers at all and separates
//! its panes by colour alone has no lines to find.  [`super::components`]
//! answers for those.

use crate::geometry::Rect;
use crate::selection_region::RegionNode;

use super::{leaf, Analysis};

/// A change of this much luminance between neighbouring pixels is an edge.
const EDGE_JUMP: f32 = 10.0;

/// The most pixels an edge may be thick across and still be part of a line.
///
/// This is the definition of "thin" the whole reader rests on.
const MAX_WIDTH: u32 = 3;

/// The fewest pixels an edge must run to be part of a line.
const MIN_LENGTH: u32 = 16;

/// How far apart two runs may be and still be the same line's corner.
const CORNER_REACH: u32 = 6;

/// The least a line must cover of a region before it may cut it.
///
/// Not 1.0: a divider interrupted by the content it separates is still the
/// divider.  Measured on an editor, its tab strip's dividers cover 65% of the
/// strip.
const MIN_COVERAGE: f64 = 0.7;

/// How much of each end of a region a line may miss and still count.
///
/// A rounded corner bends the border away from the corner, so a line stops
/// short of the region's ends by the corner radius.  Excusing a fixed slack at
/// each end is what lets a rounded rectangle be recognized without fitting
/// arcs to it.  A break in the *middle* still counts against the line, so this
/// cannot be used by a fragment to pass itself off as a divider.
const CORNER_SLACK: u32 = 12;

/// The smallest a region may be and still be worth cutting.
///
/// The region's own size, not its children's: a tab strip is 31 pixels tall and
/// its dividers are real, so judging "can this be cut" by whether the halves
/// would be large enough left every tab strip whole.
const MIN_CUTTABLE: u32 = 8;

/// The smallest region worth offering.
const MIN_REGION_EDGE: u32 = 24;

/// How many cuts one window may take.
const MAX_CUTS: usize = 512;

/// How deep the recursion may go.
const MAX_DEPTH: u32 = 24;

/// A gap shorter than this is merged without asking what colour it is.
///
/// Text sitting on a line breaks it by the width of its glyphs — a few pixels —
/// and in that gap is the *text's* colour, which says nothing about whether the
/// line continues.  Asking would reject the merge.
const SHORT_GAP: u32 = 8;

/// The longest gap that may still be bridged.
///
/// Only a sanity bound; the colour test below is what decides.  Measured on an
/// editor, a divider broken by its breadcrumb had a gap 88 pixels long whose
/// colour matched both sides exactly — the same line, plainly.
const MAX_MERGE_GAP: u32 = 400;

/// How close a gap's colour must be to the line's own.
const GAP_TOLERANCE: f32 = 6.0;

/// One line segment: which way it runs, where, and between what.
#[derive(Clone, Copy, Debug)]
struct Segment {
    vertical: bool,
    /// The x of a vertical segment, the y of a horizontal one.
    at: u32,
    start: u32,
    end: u32,
}

impl Segment {
    fn length(self) -> u32 {
        self.end.saturating_sub(self.start) + 1
    }
}

/// The regions the lines divide the window into, as a tree.
pub(super) fn regions(analysis: &Analysis) -> Vec<RegionNode> {
    let segments = segments(analysis);
    if segments.is_empty() {
        return Vec::new();
    }
    let root = cut(&segments, analysis.width, analysis.height);
    let root = prune(root);
    // Nothing was cut: the window is the only region there is, which is "no
    // elements", not "one element".  Judged here, in the reader's own pixels —
    // the space every constant above is measured in — and before the one
    // conversion to global coordinates below.
    if root.children.is_empty()
        && root.rect.origin.x == 0
        && root.rect.origin.y == 0
        && root.rect.size.width == analysis.width
        && root.rect.size.height == analysis.height
    {
        return Vec::new();
    }
    vec![root.into_global(analysis)]
}

/// The luminance of every pixel, which is what all the passes read.
fn luma(analysis: &Analysis) -> Vec<f32> {
    analysis
        .pixels
        .iter()
        .map(|pixel| super::luminance(*pixel))
        .collect()
}

/// Where neighbouring pixels differ enough to be an edge.
fn edge_maps(analysis: &Analysis) -> (Vec<bool>, Vec<bool>) {
    let values = luma(analysis);
    let (width, height) = (analysis.width as usize, analysis.height as usize);
    let mut vertical = vec![false; width * height];
    let mut horizontal = vec![false; width * height];
    for y in 0..height {
        for x in 0..width {
            let index = y * width + x;
            if x > 0 {
                vertical[index] = (values[index] - values[index - 1]).abs() >= EDGE_JUMP;
            }
            if y > 0 {
                horizontal[index] = (values[index] - values[index - width]).abs() >= EDGE_JUMP;
            }
        }
    }
    (vertical, horizontal)
}

/// How far the edge at each pixel runs along each axis.
fn run_lengths(analysis: &Analysis, edges: &[bool]) -> (Vec<u32>, Vec<u32>) {
    let (width, height) = (analysis.width as usize, analysis.height as usize);
    let mut vertical = vec![0u32; width * height];
    let mut horizontal = vec![0u32; width * height];

    for x in 0..width {
        let mut start = None;
        for y in 0..=height {
            let on = y < height && edges[y * width + x];
            match (on, start) {
                (true, None) => start = Some(y),
                (false, Some(from)) => {
                    for row in from..y {
                        vertical[row * width + x] = (y - from) as u32;
                    }
                    start = None;
                }
                _ => {}
            }
        }
    }
    for y in 0..height {
        let mut start = None;
        for x in 0..=width {
            let on = x < width && edges[y * width + x];
            match (on, start) {
                (true, None) => start = Some(x),
                (false, Some(from)) => {
                    for column in from..x {
                        horizontal[y * width + column] = (x - from) as u32;
                    }
                    start = None;
                }
                _ => {}
            }
        }
    }
    (vertical, horizontal)
}

/// The line segments in the window.
fn segments(analysis: &Analysis) -> Vec<Segment> {
    let (vertical_edges, horizontal_edges) = edge_maps(analysis);
    let mut any = vertical_edges.clone();
    for (index, value) in horizontal_edges.iter().enumerate() {
        any[index] |= *value;
    }
    let (run_v, run_h) = run_lengths(analysis, &any);
    let (width, height) = (analysis.width, analysis.height);

    // A line is long one way and thin the other.  This is the whole test, and
    // it is why text is excluded without any morphological operation.
    let mut vertical = Vec::new();
    let mut horizontal = Vec::new();
    for y in 0..height {
        for x in 0..width {
            let index = (y * width + x) as usize;
            if run_v[index] >= MIN_LENGTH && run_h[index] <= MAX_WIDTH {
                vertical.push((x, y));
            }
            if run_h[index] >= MIN_LENGTH && run_v[index] <= MAX_WIDTH {
                horizontal.push((y, x));
            }
        }
    }

    // Runs of those pixels become segments.  Every run is collected, however
    // short — filtering by length here would drop the fragments a rounded
    // corner leaves, and they are needed to bridge the corner.
    let mut out = Vec::new();
    out.extend(group(collect(&vertical, width, height, true)));
    out.extend(group(collect(&horizontal, width, height, false)));
    merge_gaps(analysis, out)
}

/// Group the marked pixels into runs along their own axis.
fn collect(points: &[(u32, u32)], width: u32, height: u32, vertical: bool) -> Vec<Segment> {
    let mut marked = vec![false; (width * height) as usize];
    for (a, b) in points {
        let (x, y) = if vertical { (*a, *b) } else { (*b, *a) };
        marked[(y * width + x) as usize] = true;
    }
    let mut out = Vec::new();
    if vertical {
        for x in 0..width {
            let mut start = None;
            for y in 0..=height {
                let on = y < height && marked[(y * width + x) as usize];
                match (on, start) {
                    (true, None) => start = Some(y),
                    (false, Some(from)) => {
                        out.push(Segment {
                            vertical: true,
                            at: x,
                            start: from,
                            end: y - 1,
                        });
                        start = None;
                    }
                    _ => {}
                }
            }
        }
    } else {
        for y in 0..height {
            let mut start = None;
            for x in 0..=width {
                let on = x < width && marked[(y * width + x) as usize];
                match (on, start) {
                    (true, None) => start = Some(x),
                    (false, Some(from)) => {
                        out.push(Segment {
                            vertical: false,
                            at: y,
                            start: from,
                            end: x - 1,
                        });
                        start = None;
                    }
                    _ => {}
                }
            }
        }
    }
    out
}

/// Join runs that are the same line's two ends around a rounded corner, then
/// keep the ones long enough to be a line.
fn group(mut items: Vec<Segment>) -> Vec<Segment> {
    items.sort_by_key(|segment| (segment.at, segment.start));
    let mut merged: Vec<Segment> = Vec::new();
    for item in items {
        let mut placed = false;
        for kept in merged.iter_mut() {
            if kept.at.abs_diff(item.at) > CORNER_REACH {
                continue;
            }
            if item.start > kept.end + CORNER_REACH || item.end + CORNER_REACH < kept.start {
                continue;
            }
            kept.at = item.at;
            kept.start = kept.start.min(item.start);
            kept.end = kept.end.max(item.end);
            placed = true;
            break;
        }
        if !placed {
            merged.push(item);
        }
    }
    merged.retain(|segment| segment.length() >= MIN_LENGTH);
    merged
}

/// Merge segments of the same line that content drew over.
///
/// The test is the colour in the gap.  A line broken by text has that text's
/// colour there; two genuinely different lines have each other's background.
/// Measured on an editor: a divider broken by its breadcrumb had a gap 88
/// pixels long whose colour matched both sides exactly, while two different
/// lines 161 pixels apart had a gap colour 12 levels off.
fn merge_gaps(analysis: &Analysis, segments: Vec<Segment>) -> Vec<Segment> {
    let (verticals, horizontals): (Vec<Segment>, Vec<Segment>) =
        segments.into_iter().partition(|segment| segment.vertical);
    let mut merged = merge_axis(analysis, verticals, true);
    merged.extend(merge_axis(analysis, horizontals, false));
    merged
}

fn merge_axis(analysis: &Analysis, items: Vec<Segment>, vertical: bool) -> Vec<Segment> {
    let mut working = items;
    working.sort_by_key(|segment| (segment.at, segment.start));
    let mut changed = true;
    while changed {
        changed = false;
        let mut out: Vec<Segment> = Vec::new();
        for item in working {
            let mut placed = false;
            for kept in out.iter_mut() {
                if kept.at.abs_diff(item.at) > 2 {
                    continue;
                }
                let (first, second) = if kept.start <= item.start {
                    (*kept, item)
                } else {
                    (item, *kept)
                };
                let gap_start = first.end + 1;
                let gap_end = second.start.saturating_sub(1);
                let gap_length = if gap_end < gap_start {
                    0
                } else {
                    gap_end - gap_start + 1
                };
                let joins = gap_end < gap_start
                    || gap_length <= 2
                    || (gap_length <= SHORT_GAP)
                    || (gap_length <= MAX_MERGE_GAP
                        && same_colour_across(
                            analysis, vertical, kept.at, first, second, gap_start, gap_end,
                        ));
                if joins {
                    kept.start = kept.start.min(item.start);
                    kept.end = kept.end.max(item.end);
                    placed = true;
                    changed = true;
                    break;
                }
            }
            if !placed {
                out.push(item);
            }
        }
        working = out;
    }
    working
}

/// Whether the gap between two segments has the colour of the line's own sides.
fn same_colour_across(
    analysis: &Analysis,
    vertical: bool,
    at: u32,
    first: Segment,
    second: Segment,
    gap_start: u32,
    gap_end: u32,
) -> bool {
    let flank = |segment: Segment| -> Option<f32> {
        band_mean(analysis, vertical, at, segment.start, segment.end)
    };
    let Some(left) = flank(first) else {
        return false;
    };
    let Some(right) = flank(second) else {
        return false;
    };
    let Some(gap) = band_mean(analysis, vertical, at, gap_start, gap_end) else {
        return false;
    };
    (gap - left).abs() <= GAP_TOLERANCE && (gap - right).abs() <= GAP_TOLERANCE
}

/// The mean luminance of a band just beside a segment.
fn band_mean(analysis: &Analysis, vertical: bool, at: u32, from: u32, to: u32) -> Option<f32> {
    if to < from {
        return None;
    }
    let offset = 2u32;
    let (width, height) = (analysis.width, analysis.height);
    let mut sum = 0f32;
    let mut count = 0u32;
    for step in from..=to {
        let (x, y) = if vertical {
            (at.saturating_sub(offset).min(width - 1), step)
        } else {
            (step, at.saturating_sub(offset).min(height - 1))
        };
        if x >= width || y >= height {
            continue;
        }
        sum += super::luminance(analysis.at(x, y));
        count += 1;
    }
    (count > 0).then(|| sum / count as f32)
}

/// Cut the window along its lines, keeping the recursion as the tree.
fn cut(segments: &[Segment], width: u32, height: u32) -> RegionNode {
    let mut budget = MAX_CUTS;
    step(segments, Rect::new(0, 0, width, height), 0, &mut budget)
}

fn step(segments: &[Segment], area: Rect, depth: u32, budget: &mut usize) -> RegionNode {
    let node = leaf(area);
    if *budget == 0 || depth > MAX_DEPTH {
        return node;
    }
    let (x0, y0) = (area.origin.x as u32, area.origin.y as u32);
    let x1 = x0 + area.size.width;
    let y1 = y0 + area.size.height;
    if area.size.width < 2 * MIN_CUTTABLE || area.size.height < 2 * MIN_CUTTABLE {
        return node;
    }

    let mut best: Option<(f64, Segment)> = None;
    for segment in segments {
        let (lo, hi, from, to) = if segment.vertical {
            (x0, x1, y0, y1)
        } else {
            (y0, y1, x0, x1)
        };
        if !(lo + MIN_CUTTABLE <= segment.at && segment.at <= hi - MIN_CUTTABLE) {
            continue;
        }
        let covered = coverage(segments, segment.vertical, segment.at, from, to);
        if covered < MIN_COVERAGE {
            continue;
        }
        if best.is_none_or(|(score, _)| covered > score) {
            best = Some((covered, *segment));
        }
    }

    let Some((_, cut_at)) = best else {
        return node;
    };
    *budget -= 1;
    let children = if cut_at.vertical {
        vec![
            step(
                segments,
                Rect::new(x0 as i32, y0 as i32, cut_at.at - x0, y1 - y0),
                depth + 1,
                budget,
            ),
            step(
                segments,
                Rect::new(cut_at.at as i32, y0 as i32, x1 - cut_at.at, y1 - y0),
                depth + 1,
                budget,
            ),
        ]
    } else {
        vec![
            step(
                segments,
                Rect::new(x0 as i32, y0 as i32, x1 - x0, cut_at.at - y0),
                depth + 1,
                budget,
            ),
            step(
                segments,
                Rect::new(x0 as i32, cut_at.at as i32, x1 - x0, y1 - cut_at.at),
                depth + 1,
                budget,
            ),
        ]
    };
    RegionNode::leaf(
        crate::selection_region::RegionKind::Element,
        area,
        super::label_for(area),
    )
    .with_children(children)
}

/// How much of `[from, to]` a line at `at` covers, with the corners excused.
fn coverage(segments: &[Segment], vertical: bool, at: u32, from: u32, to: u32) -> f64 {
    let mut inner_from = from + CORNER_SLACK;
    let mut inner_to = to.saturating_sub(CORNER_SLACK);
    if inner_to <= inner_from {
        // Too narrow to excuse a corner: the whole span counts.
        inner_from = from;
        inner_to = to;
    }
    let mut best = 0u32;
    for segment in segments {
        if segment.vertical != vertical || segment.at.abs_diff(at) > 2 {
            continue;
        }
        let overlap = segment
            .end
            .min(inner_to)
            .saturating_sub(segment.start.max(inner_from));
        if segment.end >= inner_from && segment.start <= inner_to {
            best = best.max(overlap + 1);
        }
    }
    let span = inner_to.saturating_sub(inner_from) + 1;
    f64::from(best) / f64::from(span.max(1))
}

/// Drop fragments, and dissolve nodes that group nothing.
///
/// Cutting leaves a few pixel-sized pieces behind — antialiasing, the edge of
/// an icon.  They are not regions.  A node with one child is dissolved because
/// it says nothing: it and its child are the same piece of the window.
///
/// Everything here stays in the reader's own pixels.  The size test is the one
/// the prototype applied to the image it was handed, so it is measured in the
/// same pixels the rest of this file works in — a region is small or not small
/// on its own, not after it has been rescaled to logical ones.  Converting to
/// global coordinates is [`regions`]'s single, final step.
fn prune(node: RegionNode) -> RegionNode {
    let rect = node.rect;
    let mut kept = Vec::new();
    for child in node.children {
        let child = prune(child);
        if child.rect.size.width < MIN_REGION_EDGE || child.rect.size.height < MIN_REGION_EDGE {
            continue;
        }
        kept.push(child);
    }
    // A node with one child says nothing — it and its child are the same piece
    // of the window — so the child takes its place.
    match kept.len() {
        0 => leaf(rect),
        1 => kept.into_iter().next().expect("one child"),
        _ => leaf(rect).with_children(kept),
    }
}

/// Converts the tree's local rectangles to global logical ones.
///
/// Applied exactly once, to the pruned tree, by [`regions`]: the whole reader
/// works in window-local pixels, and only the picker cares where the window is
/// on the screen.  It is not idempotent — a logical pixel is `scale` device
/// ones, so converting a converted rect divides it again — which is why it may
/// not be applied per level as the recursion unwinds.
trait IntoGlobal {
    fn into_global(self, analysis: &Analysis) -> RegionNode;
}

impl IntoGlobal for RegionNode {
    fn into_global(self, analysis: &Analysis) -> RegionNode {
        let rect = analysis.to_global(self.rect).unwrap_or(self.rect);
        let children = self
            .children
            .into_iter()
            .map(|child| child.into_global(analysis))
            .collect();
        RegionNode::leaf(
            crate::selection_region::RegionKind::Element,
            rect,
            super::label_for(rect),
        )
        .with_children(children)
    }
}

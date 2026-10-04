// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Reading a window by the colour regions in it.
//!
//! This is the pixel source as it stood before the line reader was written, and
//! it is kept beside it because the two fail on different interfaces.  It finds
//! a pane as *an area of one colour* rather than as *the space between drawn
//! lines*, so it answers for an interface that draws no lines at all — a chat
//! client's sidebar against its conversation, which the line reader cannot see
//! because there is nothing there to see.
//!
//! Three passes, coarse to fine:
//!
//! * **Recursive XY-cut** finds the structure.  A pane is bounded by lines that
//!   cross it, and cutting at the strongest such line, over and over, recovers
//!   the layout.  The lines here are *colour steps* rather than drawn dividers,
//!   which is what makes it work on a window that draws none.
//! * **Connected components** finds the controls: a button is a small area of
//!   one colour inside a differently-coloured surround.
//! * **Containment** arranges the boxes into the tree the picker walks.
//!
//! It is not registered by default — [`super::Pixels`] is, and it reads lines —
//! but it is one line in `element::sources` away from being asked first, and
//! that is deliberate: which of the two reads an interface better is a question
//! about the interface, not about the code.

use crate::geometry::Rect;
use crate::selection_region::RegionNode;

use super::{leaf, Analysis};

/// Two colours are "the same" for a component when every channel is within
/// this.  Loose enough to absorb a gradient or a subtle texture, tight enough
/// that a border line is not swallowed.
const SAME_COLOR: i32 = 12;

/// How much a line's centre must differ from its surround, in luminance, to
/// count as a line.
const LINE_THRESHOLD: f32 = 16.0;

/// How much the two sides of a colour step must differ, in luminance.
const BLOCK_THRESHOLD: f32 = 2.0;

/// The smallest a *control* may be and still be offered.
const MIN_CONTROL_EDGE: u32 = 24;

/// How large a region has to be before its colour steps are trusted.
const BLOCK_MIN_SPAN: u32 = 200;

/// How far either side of a colour step the means are taken.
const BLOCK_REACH: u32 = 24;

/// The widest a line may be and still be a line.
const MAX_LINE_WIDTH: u32 = 4;

/// How much of a line has to run through a region before it divides it.
const CUT_FRACTION: f64 = 0.65;

/// The smallest region worth offering.
const MIN_REGION_EDGE: u32 = 200;

/// How many regions one window may be divided into.
const MAX_REGIONS: usize = 256;

/// The regions this reader finds, as a tree.
pub(super) fn regions(analysis: &Analysis) -> Vec<RegionNode> {
    let rects = candidates(analysis);
    nest(analysis, rects)
}

/// Every region the window's pixels yield, in analysis coordinates.
fn candidates(analysis: &Analysis) -> Vec<Rect> {
    let edges = edges(analysis);
    let mut regions = Vec::new();
    cut(
        analysis,
        &edges,
        0,
        0,
        analysis.width,
        analysis.height,
        0,
        &mut regions,
    );

    // Two kinds, held apart because they are worth different sizes: a region
    // is a pane and has to be big to be worth offering, while a control is a
    // button and is *supposed* to be small.
    let mut found: Vec<(Rect, bool)> = regions.iter().map(|rect| (*rect, false)).collect();
    // Controls are looked for over the *whole window*, not inside each region:
    // a control may straddle two of them — a toolbar button sitting on the line
    // between the toolbar and the page — and searching region by region would
    // find half a button in each and neither half would be the button.
    for control in controls_in(analysis, Rect::new(0, 0, analysis.width, analysis.height)) {
        found.push((control, true));
    }
    found
        .into_iter()
        .filter(|(rect, _)| {
            // The window itself is not an element inside it.
            !(rect.size.width == analysis.width && rect.size.height == analysis.height)
        })
        .filter_map(|(rect, is_control)| {
            let floor = if is_control {
                MIN_CONTROL_EDGE
            } else {
                MIN_REGION_EDGE
            };
            (rect.size.width >= floor && rect.size.height >= floor)
                .then(|| analysis.to_global(rect))
                .flatten()
        })
        .collect()
}

/// The controls inside one region: connected areas of a single colour.
///
/// A four-way flood fill, seeded from every unvisited pixel, joining a
/// neighbour when its colour is within [`SAME_COLOR`] of the seed.  A component
/// that does not fill its own bounding box is dropped — it is a scatter of
/// similar pixels, which is what a gradient or a photograph produces, not a
/// rectangle anyone could point at.
fn controls_in(analysis: &Analysis, region: Rect) -> Vec<Rect> {
    let x0 = region.origin.x.max(0) as u32;
    let y0 = region.origin.y.max(0) as u32;
    let x1 = (x0 + region.size.width).min(analysis.width);
    let y1 = (y0 + region.size.height).min(analysis.height);
    if x1 <= x0 || y1 <= y0 {
        return Vec::new();
    }
    let width = x1 - x0;
    let height = y1 - y0;
    let mut seen = vec![false; (width * height) as usize];
    let index = |x: u32, y: u32| ((y - y0) * width + (x - x0)) as usize;

    let mut rects = Vec::new();
    let mut stack: Vec<(u32, u32)> = Vec::new();
    for sy in y0..y1 {
        for sx in x0..x1 {
            if seen[index(sx, sy)] {
                continue;
            }
            let seed = analysis.at(sx, sy);
            seen[index(sx, sy)] = true;
            stack.clear();
            stack.push((sx, sy));
            let (mut min_x, mut min_y) = (sx, sy);
            let (mut max_x, mut max_y) = (sx, sy);
            let mut members = 0u32;

            while let Some((x, y)) = stack.pop() {
                min_x = min_x.min(x);
                min_y = min_y.min(y);
                max_x = max_x.max(x);
                max_y = max_y.max(y);
                members += 1;
                let mut push = |nx: u32, ny: u32, stack: &mut Vec<(u32, u32)>| {
                    let at = index(nx, ny);
                    if !seen[at] && close_enough(analysis.at(nx, ny), seed) {
                        seen[at] = true;
                        stack.push((nx, ny));
                    }
                };
                if x > x0 {
                    push(x - 1, y, &mut stack);
                }
                if x + 1 < x1 {
                    push(x + 1, y, &mut stack);
                }
                if y > y0 {
                    push(x, y - 1, &mut stack);
                }
                if y + 1 < y1 {
                    push(x, y + 1, &mut stack);
                }
            }

            let box_width = max_x - min_x + 1;
            let box_height = max_y - min_y + 1;
            let box_area = u64::from(box_width) * u64::from(box_height);
            if u64::from(members) * 100 < box_area * 88 {
                continue;
            }
            // A component touching the region's edge is the region's own
            // background, not a control in it: whatever colour the pane is
            // painted, it runs to the pane's border.
            let touches_edge = min_x <= x0 || min_y <= y0 || max_x + 1 >= x1 || max_y + 1 >= y1;
            if touches_edge {
                continue;
            }
            rects.push(Rect::new(min_x as i32, min_y as i32, box_width, box_height));
        }
    }
    rects
}

/// Whether two colours are the same for a component.
fn close_enough(a: [u8; 4], b: [u8; 4]) -> bool {
    let channel = |i: usize| (i32::from(a[i]) - i32::from(b[i])).abs();
    channel(0) <= SAME_COLOR
        && channel(1) <= SAME_COLOR
        && channel(2) <= SAME_COLOR
        && channel(3) <= SAME_COLOR
}

/// Splits a region at its strongest divider and recurses, or records it.
fn cut(
    analysis: &Analysis,
    edges: &Edges,
    x0: u32,
    y0: u32,
    x1: u32,
    y1: u32,
    depth: u32,
    out: &mut Vec<Rect>,
) {
    if out.len() >= MAX_REGIONS {
        return;
    }
    if x1.saturating_sub(x0) < MIN_REGION_EDGE * 2 || y1.saturating_sub(y0) < MIN_REGION_EDGE * 2 {
        out.push(Rect::new(x0 as i32, y0 as i32, x1 - x0, y1 - y0));
        return;
    }
    // A colour step is only trusted in a region large enough to be a pane: in a
    // small one the step is as likely to be a control's own edge.
    let blocks = (x1 - x0) >= BLOCK_MIN_SPAN && (y1 - y0) >= BLOCK_MIN_SPAN;
    match edges.strongest(x0, y0, x1, y1, blocks) {
        Some((Axis::Vertical, at)) => {
            cut(analysis, edges, x0, y0, at, y1, depth + 1, out);
            cut(analysis, edges, at, y0, x1, y1, depth + 1, out);
        }
        Some((Axis::Horizontal, at)) => {
            cut(analysis, edges, x0, y0, x1, at, depth + 1, out);
            cut(analysis, edges, x0, at, x1, y1, depth + 1, out);
        }
        None => out.push(Rect::new(x0 as i32, y0 as i32, x1 - x0, y1 - y0)),
    }
}

/// Which way a cut runs.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Axis {
    Vertical,
    Horizontal,
}

/// Where a line runs, as a response map per axis.
struct Edges {
    width: u32,
    vertical: Vec<bool>,
    horizontal: Vec<bool>,
    block_vertical: Vec<bool>,
    block_horizontal: Vec<bool>,
}

impl Edges {
    /// The strongest line crossing the region.
    fn strongest(&self, x0: u32, y0: u32, x1: u32, y1: u32, blocks: bool) -> Option<(Axis, u32)> {
        let height = f64::from(y1 - y0).max(1.0);
        let width = f64::from(x1 - x0).max(1.0);
        let mut best: Option<(f64, Axis, u32)> = None;

        for x in (x0 + MIN_REGION_EDGE)..(x1.saturating_sub(MIN_REGION_EDGE)) {
            let fraction = self.column_strength(x, y0, y1, blocks) / height;
            if fraction < CUT_FRACTION {
                continue;
            }
            if best.is_none_or(|(score, _, _)| fraction > score) {
                best = Some((fraction, Axis::Vertical, x));
            }
        }
        for y in (y0 + MIN_REGION_EDGE)..(y1.saturating_sub(MIN_REGION_EDGE)) {
            let fraction = self.row_strength(y, x0, x1, blocks) / width;
            if fraction < CUT_FRACTION {
                continue;
            }
            if best.is_none_or(|(score, _, _)| fraction > score) {
                best = Some((fraction, Axis::Horizontal, y));
            }
        }
        best.map(|(_, axis, at)| (axis, at))
    }

    fn column_strength(&self, x: u32, y0: u32, y1: u32, blocks: bool) -> f64 {
        if x >= self.width {
            return 0.0;
        }
        let mut count = 0u32;
        for y in y0..y1 {
            let index = (y * self.width + x) as usize;
            let hit = self.vertical[index] || (blocks && self.block_vertical[index]);
            if hit {
                count += 1;
            }
        }
        f64::from(count)
    }

    fn row_strength(&self, y: u32, x0: u32, x1: u32, blocks: bool) -> f64 {
        let mut count = 0u32;
        for x in x0..x1.min(self.width) {
            let index = (y * self.width + x) as usize;
            let hit = self.horizontal[index] || (blocks && self.block_horizontal[index]);
            if hit {
                count += 1;
            }
        }
        f64::from(count)
    }
}

/// Where a line runs, as a response map for each axis.
///
/// The response is a convolution: a centre band flanked by two bands of the
/// opposite sign, which answers to "a run of one colour with a different colour
/// on both sides" — a matched filter for a line.  Kept as maps so a region can
/// ask about its own slice, which is what lets a line be found inside a pane
/// where it does not cross the window.
fn edges(analysis: &Analysis) -> Edges {
    let width = analysis.width as usize;
    let height = analysis.height as usize;
    let luma: Vec<f32> = analysis
        .pixels
        .iter()
        .map(|pixel| super::luminance(*pixel))
        .collect();

    let mut vertical = vec![false; width * height];
    let mut block_vertical = vec![false; width * height];
    let mut row = vec![0f32; width];
    let mut prefix = vec![0f32; width + 1];
    for y in 0..height {
        row.copy_from_slice(&luma[y * width..(y + 1) * width]);
        running_sum(&row, &mut prefix);
        for x in 0..width {
            let index = y * width + x;
            let at = x as u32;
            vertical[index] = matched_filter(&prefix, at, analysis.width);
            block_vertical[index] = block_step(&prefix, at, analysis.width);
        }
    }

    let mut horizontal = vec![false; width * height];
    let mut block_horizontal = vec![false; width * height];
    let mut column = vec![0f32; height];
    let mut down = vec![0f32; height + 1];
    for x in 0..width {
        for y in 0..height {
            column[y] = luma[y * width + x];
        }
        running_sum(&column, &mut down);
        for y in 0..height {
            let index = y * width + x;
            let at = y as u32;
            horizontal[index] = matched_filter(&down, at, analysis.height);
            block_horizontal[index] = block_step(&down, at, analysis.height);
        }
    }

    Edges {
        width: analysis.width,
        vertical,
        horizontal,
        block_vertical,
        block_horizontal,
    }
}

/// Prefix sums of one line, so a band's mean is two lookups.
fn running_sum(line: &[f32], prefix: &mut [f32]) {
    prefix[0] = 0.0;
    for (index, value) in line.iter().enumerate() {
        prefix[index + 1] = prefix[index] + value;
    }
}

/// The matched filter at one position: `min(|centre − left|, |centre − right|)`,
/// over every width a line might be.
///
/// The *minimum* of the two sides rather than the nearer one: a line has a
/// different colour on both sides, while a glyph stroke has background on one
/// side and more glyph on the other.  Taking the minimum is what tells them
/// apart.
fn matched_filter(prefix: &[f32], at: u32, span: u32) -> bool {
    for width in 1..=MAX_LINE_WIDTH {
        if at < 3 * width {
            // The window has not opened yet, and it only widens.
            return false;
        }
        if at + 3 * width > span {
            continue;
        }
        let centre_sum = prefix[(at + width) as usize] - prefix[(at - width) as usize];
        let left_sum = prefix[(at - width) as usize] - prefix[(at - 3 * width) as usize];
        let right_sum = prefix[(at + 3 * width) as usize] - prefix[(at + width) as usize];
        let two_w = (2 * width) as f32;
        let centre = centre_sum / two_w;
        let left = left_sum / two_w;
        let right = right_sum / two_w;
        if (centre - left).abs().min((centre - right).abs()) >= LINE_THRESHOLD {
            return true;
        }
    }
    false
}

/// Whether a colour step runs through one position.
fn block_step(prefix: &[f32], at: u32, span: u32) -> bool {
    if at < BLOCK_REACH || at + BLOCK_REACH > span {
        return false;
    }
    let reach = BLOCK_REACH as f32;
    let before = (prefix[at as usize] - prefix[(at - BLOCK_REACH) as usize]) / reach;
    let after = (prefix[(at + BLOCK_REACH) as usize] - prefix[at as usize]) / reach;
    (before - after).abs() >= BLOCK_THRESHOLD
}

/// Arranges regions into the tree the picker walks.
///
/// A region inside another is that region's child, so the wheel can climb from
/// a control to the pane holding it.  Regions are nested by area, largest
/// outward: each is placed under the smallest already placed that contains it,
/// or at the top when none does.
fn nest(analysis: &Analysis, rects: Vec<Rect>) -> Vec<RegionNode> {
    let mut rects = rects;
    rects.sort_by_key(|rect| {
        std::cmp::Reverse(u64::from(rect.size.width) * u64::from(rect.size.height))
    });
    rects.dedup();
    if rects.is_empty() {
        return Vec::new();
    }

    let nodes: Vec<RegionNode> = rects.iter().map(|rect| leaf(*rect)).collect();
    let area = |rect: Rect| u64::from(rect.size.width) * u64::from(rect.size.height);
    let mut parent: Vec<Option<usize>> = vec![None; nodes.len()];
    for index in 0..nodes.len() {
        let rect = nodes[index].rect;
        let mut best: Option<usize> = None;
        for other in 0..index {
            if !contains(nodes[other].rect, rect) {
                continue;
            }
            if best.is_none_or(|current| area(nodes[other].rect) < area(nodes[current].rect)) {
                best = Some(other);
            }
        }
        parent[index] = best;
    }

    let mut children: Vec<Vec<usize>> = vec![Vec::new(); nodes.len()];
    let mut roots: Vec<usize> = Vec::new();
    for (index, owner) in parent.iter().enumerate() {
        match owner {
            Some(owner) => children[*owner].push(index),
            None => roots.push(index),
        }
    }
    fn build(index: usize, nodes: &[RegionNode], children: &[Vec<usize>]) -> RegionNode {
        let kids: Vec<RegionNode> = children[index]
            .iter()
            .map(|child| build(*child, nodes, children))
            .collect();
        nodes[index].clone().with_children(kids)
    }
    let roots: Vec<RegionNode> = roots
        .iter()
        .map(|root| build(*root, &nodes, &children))
        .collect();
    // The window itself is the top of the path the wheel climbs: several
    // sibling panes have no common parent otherwise.
    let window = leaf(Rect::new(0, 0, analysis.width, analysis.height));
    let mut sorted = roots;
    sorted.sort_by_key(|node| (node.rect.origin.y, node.rect.origin.x));
    vec![window.with_children(sorted)]
}

/// Whether `outer` contains `inner` — not merely overlaps it.
fn contains(outer: Rect, inner: Rect) -> bool {
    const SLACK: i32 = 2;
    outer != inner
        && outer.left() - SLACK <= inner.left()
        && outer.top() - SLACK <= inner.top()
        && outer.right().map(|r| r + SLACK).unwrap_or(i32::MAX) >= inner.right().unwrap_or(i32::MIN)
        && outer.bottom().map(|b| b + SLACK).unwrap_or(i32::MAX)
            >= inner.bottom().unwrap_or(i32::MIN)
}

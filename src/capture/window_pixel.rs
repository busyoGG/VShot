//! Pixel fallback for active-window capture. When no compositor metadata
//! provider answers (Hyprland, Sway, or the KWin scripting probe), the
//! focused window is detected from the captured frame itself.
//!
//! Three detectors run in order, all on a downscaled analysis copy of the
//! composed scene:
//!
//! 1. Accent outline: tiling compositors stroke the focused window with a
//!    distinct border color, so a closed rectangle outline of one consistent
//!    color is a near-certain active-window marker. Edge pixels are
//!    histogrammed by quantized color and the most promising colors are
//!    fitted with rectangle outlines.
//! 2. Background segmentation (borderless windows): the wallpaper and soft
//!    shadows are traced from the frame edges with a locally-smooth flood
//!    fill; large non-background components are window candidates, a lone
//!    small component is still accepted, and uniform or content-dominated
//!    frames degrade to the whole frame.
//!
//! A cursor position, when known, disambiguates multiple candidates; the
//! largest candidate wins otherwise. Truly seamless borderless tiling with
//! several windows carries no pixel signal and ends in a clear error.

use std::collections::VecDeque;

use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect};
use crate::model::{Frame, SceneSnapshot};

use super::window::{ActiveWindow, WindowSource};

/// Longest analysis-frame side; detection runs on a downscaled copy for
/// predictable cost on 4K+ scenes.
const MAX_ANALYSIS_DIM: u32 = 1024;
/// Neighbor channel delta that marks both pixels as content edges.
const EDGE_DIFF: i32 = 24;
/// Sum-of-channel distance for a pixel to join a candidate outline color.
const COLOR_TOLERANCE: i32 = 45;
/// Smallest outline window side, as a percent of the analysis frame.
const OUTLINE_MIN_DIM_PCT: u32 = 15;
/// Outline band coverage and interior emptiness thresholds.
const OUTLINE_COVERAGE: f64 = 0.62;
const OUTLINE_HOLLOW_MAX: f64 = 0.35;
/// Probed border thickness range, in analysis pixels.
const MAX_BORDER: u32 = 6;
/// Neighbor channel delta a locally-smooth background flood may cross.
const SEG_JOIN_DIFF: i32 = 10;
/// Smallest segmentation candidate: area percent and per-dimension percent.
const SEG_MIN_AREA_PCT: u64 = 4;
const SEG_MIN_DIM_PCT: u32 = 20;

/// Detects the active window from the captured scene. Returns global logical
/// geometry suitable for `SceneSnapshot::crop`.
pub fn detect_active_window(scene: &SceneSnapshot, cursor: Option<Point>) -> Result<ActiveWindow> {
    let frame = scene.frame();
    let size = frame.size();
    let factor = size
        .width
        .max(size.height)
        .div_ceil(MAX_ANALYSIS_DIM)
        .max(1);
    let analysis = AnalysisFrame::sample(frame, factor);
    let cursor_analysis = cursor.and_then(|point| to_analysis(scene, factor, point));

    let rect = detect_outline(&analysis, cursor_analysis)
        .or_else(|| detect_segment(&analysis, cursor_analysis))
        .ok_or_else(|| {
            VshotError::ActiveWindowUnavailable(
                "pixel fallback found no accent outline and no separable window region \
                 (borderless windows with no gaps or shadows carry no pixel signal)"
                    .into(),
            )
        })?;
    map_to_logical(scene, factor, rect)
}

/// Downscaled RGBA copy of the composed scene frame. Pixel `(x, y)` samples
/// the source block starting at `(x * factor, y * factor)`.
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

fn area(rect: Rect) -> u64 {
    u64::from(rect.size.width) * u64::from(rect.size.height)
}

/// Converts a global logical point into analysis coordinates; `None` when the
/// point lies outside the scene.
fn to_analysis(scene: &SceneSnapshot, factor: u32, point: Point) -> Option<Point> {
    let bounds = scene.bounds();
    let device_x = (i64::from(point.x) - i64::from(bounds.left())) * i64::from(scene.scale());
    let device_y = (i64::from(point.y) - i64::from(bounds.top())) * i64::from(scene.scale());
    if device_x < 0 || device_y < 0 {
        return None;
    }
    Some(Point::new(
        (device_x / i64::from(factor)) as i32,
        (device_y / i64::from(factor)) as i32,
    ))
}

/// Converts an analysis-space rect back to global logical coordinates.
fn map_to_logical(scene: &SceneSnapshot, factor: u32, rect: Rect) -> Result<ActiveWindow> {
    let bounds = scene.bounds();
    let scale = i64::from(scene.scale());
    let frame_device_width = i64::from(bounds.size.width) * scale;
    let frame_device_height = i64::from(bounds.size.height) * scale;
    let device_x = i64::from(rect.origin.x) * i64::from(factor);
    let device_y = i64::from(rect.origin.y) * i64::from(factor);
    let device_width = i64::from(rect.size.width) * i64::from(factor);
    let device_height = i64::from(rect.size.height) * i64::from(factor);
    let round_div = |value: i64| i32::try_from((value + scale / 2) / scale).unwrap_or(0);
    let mut geometry = Rect::new(
        bounds.left() + round_div(device_x.clamp(0, frame_device_width)),
        bounds.top() + round_div(device_y.clamp(0, frame_device_height)),
        round_div(device_width.min(frame_device_width)).max(1) as u32,
        round_div(device_height.min(frame_device_height)).max(1) as u32,
    );
    // Keep the crop inside the scene even after rounding.
    geometry = geometry.clamp_to(bounds).ok_or_else(|| {
        VshotError::ActiveWindowUnavailable("pixel fallback geometry fell outside the scene".into())
    })?;
    if geometry.is_empty() {
        return Err(VshotError::ActiveWindowUnavailable(
            "pixel fallback produced an empty window rectangle".into(),
        ));
    }
    Ok(ActiveWindow {
        geometry,
        source: WindowSource::Pixel,
    })
}

/// Ranks candidates: a candidate containing the cursor always wins, the
/// larger area wins otherwise.
fn pick_candidate(candidates: Vec<Rect>, cursor: Option<Point>) -> Option<Rect> {
    fn rank(rect: Rect, cursor: Option<Point>) -> (u8, u64) {
        (
            u8::from(cursor.is_some_and(|point| rect.contains(point))),
            area(rect),
        )
    }
    candidates
        .into_iter()
        .max_by_key(|rect| rank(*rect, cursor))
}

/// Phase 1: find a closed rectangle outline drawn in one consistent accent
/// color and return the window rect inside it.
fn detect_outline(frame: &AnalysisFrame, cursor: Option<Point>) -> Option<Rect> {
    let histogram = Histogram::collect_edge_colors(frame);
    let mut fits: Vec<Rect> = Vec::new();
    for color in histogram.take_candidates(8) {
        if let Some(rect) = fit_outline_for_color(frame, color, cursor) {
            // A candidate containing the cursor ends the search immediately.
            if cursor.is_some_and(|point| rect.contains(point)) {
                return Some(rect);
            }
            fits.push(rect);
        }
    }
    pick_candidate(fits, cursor)
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

/// Builds the color mask and scans its connected components for a fitted
/// outline. Returns the best fit, preferring cursor containment then area.
fn fit_outline_for_color(
    frame: &AnalysisFrame,
    color: [u8; 4],
    cursor: Option<Point>,
) -> Option<Rect> {
    let mask: Vec<bool> = frame
        .pixels
        .iter()
        .map(|pixel| {
            pixel[3] != 0
                && (i32::from(pixel[0]) - i32::from(color[0])).abs()
                    + (i32::from(pixel[1]) - i32::from(color[1])).abs()
                    + (i32::from(pixel[2]) - i32::from(color[2])).abs()
                    <= COLOR_TOLERANCE
        })
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
            if let Some(rect) = fit_outline_in_bbox(frame, &mask, bbox) {
                if cursor.is_some_and(|point| rect.contains(point)) {
                    return Some(rect);
                }
                fits.push(rect);
            }
        }
    }
    pick_candidate(fits, cursor)
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

/// Tries border depths 1..=MAX_BORDER: the band inside depth `t` must be
/// almost fully mask-colored while the interior beyond stays mostly empty.
/// Scoring naturally selects the smallest interior that is still empty, i.e.
/// the true border thickness.
fn fit_outline_in_bbox(frame: &AnalysisFrame, mask: &[bool], bbox: Rect) -> Option<Rect> {
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
                let hit = mask[frame.mask_index(x as u32, y as u32)];
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

/// Phase 2: flood background regions from the frame edges (locally-smooth
/// growth so gradient wallpapers and soft shadows are absorbed), then treat
/// large non-background components as windows.
fn detect_segment(frame: &AnalysisFrame, cursor: Option<Point>) -> Option<Rect> {
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
    // Selection ladder: threshold-passing windows first (cursor-containing
    // wins, largest otherwise), then a lone minor component (one small
    // floating window; several need the cursor to disambiguate), then the
    // degraded whole-frame cases, otherwise an honest None.
    match pick_candidate(candidates, cursor) {
        Some(rect) => Some(rect),
        None => {
            let minor_pick = match minor.len() {
                1 => minor.first().copied(),
                0 => None,
                _ => {
                    cursor.and_then(|point| minor.iter().copied().find(|rect| rect.contains(point)))
                }
            };
            minor_pick.or_else(|| {
                let background_fraction = background_pixels as u64 * 100 / total as u64;
                // The whole-frame degradation only makes sense for an
                // essentially uniform frame (one fullscreen window or a bare
                // desktop). Internal edges — a seam between seamlessly tiled
                // windows, text — mean several windows we cannot split.
                let edge_pairs = Histogram::collect_edge_colors(frame)
                    .counts
                    .iter()
                    .map(|count| u64::from(*count))
                    .sum::<u64>()
                    / 2;
                let uniform = edge_pairs * 1000 < u64::from(frame.width) * u64::from(frame.height);
                ((background_fraction > 97 && uniform) || background_fraction < 50)
                    .then(|| Rect::new(0, 0, frame.width, frame.height))
            })
        }
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
    fn outline_detects_bordered_window_and_insets_the_border() {
        let mut canvas = Canvas::new(400, 300, [40, 60, 80, 255]);
        // Content first, then a 3px accent border drawn over its rim.
        canvas.fill(100, 80, 200, 140, [200, 200, 200, 255]);
        canvas.fill(100, 80, 200, 3, [255, 140, 0, 255]);
        canvas.fill(100, 217, 200, 3, [255, 140, 0, 255]);
        canvas.fill(100, 80, 3, 140, [255, 140, 0, 255]);
        canvas.fill(297, 80, 3, 140, [255, 140, 0, 255]);
        let window = detect_active_window(&canvas.scene(), None).unwrap();
        assert_eq!(window.source, WindowSource::Pixel);
        assert_eq!(window.geometry, Rect::new(103, 83, 194, 134));
    }

    #[test]
    fn outline_prefers_the_cursor_containing_window() {
        let mut canvas = Canvas::new(400, 300, [40, 60, 80, 255]);
        for (x, y, color) in [(30, 30, [255, 0, 0, 255]), (240, 170, [0, 90, 255, 255])] {
            canvas.fill(x, y, 120, 90, [210, 210, 210, 255]);
            canvas.fill(x, y, 120, 2, color);
            canvas.fill(x, y + 88, 120, 2, color);
            canvas.fill(x, y, 2, 90, color);
            canvas.fill(x + 118, y, 2, 90, color);
        }
        let window = detect_active_window(&canvas.scene(), Some(Point::new(300, 210))).unwrap();
        assert_eq!(window.geometry, Rect::new(242, 172, 116, 86));
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

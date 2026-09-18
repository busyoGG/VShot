//! Finds a window's own render on a captured frame.
//!
//! niri hands a window screenshot over rendered with its alpha channel: a
//! translucent window comes out translucent, with none of the wallpaper that
//! was showing through it.  The screen, however, *did* show that wallpaper —
//! and the frame vshot captures with screencopy holds exactly that composite.
//! This module finds *where* on the frame the render sits, so the caller can
//! crop the frame instead and output the window the way the screen actually
//! showed it, wallpaper and all.
//!
//! The search is a template match over the render's own pixels.  At the true
//! position a sampled frame pixel equals the render's colour scaled by its
//! alpha (the screen is `render · α + background · (1−α)`, and the background
//! is the only unknown); anywhere else, window content and background
//! disagree.  Only pixels that carry structure are sampled — a window's own
//! flat background verifies a position almost anywhere, which is how a capture
//! once landed on the window next door; see [`collect_structure_samples`].  A
//! coarse pass at quarter resolution narrows the position down, a
//! full-resolution pass refines it, and the match ratio decides whether the
//! position is trusted at all.

use crate::geometry::{Rect, Size};
use crate::model::Frame;

/// Samples the coarse pass scores a position with.  Small enough that a 4K
/// search stays well under a second, large enough that random background
/// cannot clear the match ratio by luck.
const COARSE_SAMPLES: usize = 160;
/// Samples the refinement pass scores with, at full resolution.
const REFINE_SAMPLES: usize = 1200;
/// Pixel stride of the coarse position sweep.
const COARSE_STEP: u32 = 4;
/// Full-resolution positions tried around the coarse winner, per axis.
const REFINE_RADIUS: i32 = 4;
/// A sampled pixel counts as matching when every channel is this close to the
/// render's premultiplied colour — the screen pixel is `render + background ·
/// (1−α)`, so the render's premultiplied share is the floor and the unknown
/// background can add up to `(255−α)` per channel on top, with noise allowed.
const MATCH_SLACK: i32 = 24;
/// A sampled pixel participates at all when its alpha is at least this: below
/// it the background dominates the screen pixel and says nothing about where
/// the window is.
const MIN_SAMPLE_ALPHA: u8 = 150;
/// How far a pixel has to differ from a neighbour before it counts as
/// *structure* — something that pins a position down.  A window's own flat
/// background verifies a position almost anywhere on a desktop of the same
/// colour, so sampling it is what once put a capture on the window next door;
/// see [`collect_structure_samples`].
const MIN_STRUCTURE_DELTA: u8 = 8;
/// Cap on how many structured pixels are held while collecting, so a busy
/// render cannot make the collection itself the cost.  Hitting it halves the
/// kept set and doubles the stride from there, which keeps the samples spread
/// over the render instead of clustered on its busiest corner.
const MAX_STRUCTURED_PIXELS: usize = 16384;
/// Fraction of participating samples that must match for a position to be
/// trusted.
const MIN_MATCH_RATIO: f64 = 0.6;
/// With fewer usable samples than this the render says nothing about where it
/// sits (a fully translucent or near-empty render cannot be located).
const MIN_SAMPLES: usize = 64;

struct Sample {
    /// Position inside the window render, in render pixels.
    x: u32,
    y: u32,
    /// The render's colour at that position, premultiplied by its alpha — the
    /// form niri writes and the form the screen composites with.
    color: [u8; 4],
}

/// Lays an even grid over the render and keeps the pixels opaque enough to
/// verify a position, roughly `target` of them.
///
/// This is the sampler that ignores structure entirely, and it is what
/// [`locate_window`] falls back to for a render that has none (a plain solid
/// window, which says nothing about where it sits either way).  It is also
/// what [`renders_agree`] compares with: that question is "did the content
/// move on", and a flat area changing colour answers it just as well.
///
/// The step is taken from the render's *area*, so the samples spread over both
/// axes at once.  A step that walked the rows with a single stride and stopped
/// once it had collected `target` samples would spend the whole budget on the
/// first few rows of a tall render and leave the vertical position barely
/// constrained: measured on a 1880×2112 render, the refinement's 1200 samples
/// landed on three rows four pixels apart, and the located rectangle settled
/// two pixels right and four down — far enough to crop the left and top border
/// off the capture.
fn collect_samples(window: &Frame, target: usize) -> Vec<Sample> {
    let Size { width, height } = window.size();
    if width == 0 || height == 0 {
        return Vec::new();
    }
    let area = u64::from(width) * u64::from(height);
    let stride = ((area as f64 / target.max(1) as f64).sqrt().ceil() as u64).max(2) as u32;
    let pixels = window.pixels();
    let width = width as usize;
    let mut samples = Vec::new();
    let mut y = 0;
    while y < height {
        let mut x = 0;
        while x < width as u32 {
            let index = (y as usize * width + x as usize) * 4;
            if let Some(pixel) = pixels.get(index..index + 4) {
                if pixel[3] >= MIN_SAMPLE_ALPHA {
                    // niri's render arrives premultiplied (every pixel obeys
                    // RGB ≤ alpha), so the colour is stored as it will be
                    // composited, not rescaled to full opacity.
                    samples.push(Sample {
                        x,
                        y,
                        color: pixel.try_into().unwrap(),
                    });
                }
            }
            x += stride;
        }
        y += stride;
    }
    samples
}

/// Does the pixel at `(x, y)` differ from any neighbour by more than
/// [`MIN_STRUCTURE_DELTA`] on some channel?  Edges are marked on both sides —
/// the bright and the dark pixel of a stroke both answer true — so a stroke one
/// pixel wide is not missed.
fn has_structure(pixels: &[u8], width: usize, height: usize, x: usize, y: usize) -> bool {
    let index = (y * width + x) * 4;
    let Some(pixel) = pixels.get(index..index + 4) else {
        return false;
    };
    let neighbours = [
        x.checked_sub(1).map(|nx| (nx, y)),
        (x + 1 < width).then_some((x + 1, y)),
        y.checked_sub(1).map(|ny| (x, ny)),
        (y + 1 < height).then_some((x, y + 1)),
    ];
    neighbours.iter().flatten().any(|&(nx, ny)| {
        let other = (ny * width + nx) * 4;
        pixels.get(other..other + 4).is_some_and(|other| {
            (0..3).any(|channel| other[channel].abs_diff(pixel[channel]) > MIN_STRUCTURE_DELTA)
        })
    })
}

/// The pixels that actually pin a position down: opaque ones that carry
/// structure.
///
/// Sampling by opacity alone is what once put a capture on the window next
/// door.  A terminal is mostly one flat colour with a little text in it —
/// measured on a live 1880×2024 kitty render, 99.68% of its pixels were the
/// single background colour — and a flat colour matches almost anywhere on a
/// desktop of a similar shade.  The coarse grid's stride is taken from the
/// render's area (78 pixels on that render, 155 on it in another run), so all
/// its samples landed on the flat background, none on the text: the true
/// position scored 180/182 while a point 850 device pixels away scored
/// 182/182 and won.  Structure is what makes the difference: the same two
/// windows then score 160/160 at the truth and under 70/160 at the decoy.
///
/// The pixels are collected in one pass and then thinned by an even stride, so
/// the samples still spread over the render's whole extent.  A render with too
/// little structure to place is not a failure here: it falls back to
/// [`collect_samples`] and lets the match ratio decide, rather than refusing a
/// window the old sampler could still locate.
fn collect_structure_samples(window: &Frame, target: usize) -> Vec<Sample> {
    let Size { width, height } = window.size();
    if width == 0 || height == 0 {
        return Vec::new();
    }
    let pixels = window.pixels();
    let (width, height) = (width as usize, height as usize);
    let mut kept: Vec<(u32, u32)> = Vec::new();
    let mut seen: u64 = 0;
    let mut stride: u64 = 1;
    for y in 0..height {
        for x in 0..width {
            let index = (y * width + x) * 4;
            if pixels
                .get(index + 3)
                .is_none_or(|alpha| *alpha < MIN_SAMPLE_ALPHA)
                || !has_structure(pixels, width, height, x, y)
            {
                continue;
            }
            if seen.is_multiple_of(stride) {
                kept.push((x as u32, y as u32));
                if kept.len() >= MAX_STRUCTURED_PIXELS {
                    // A busy render: thin what is held so far and take every
                    // other one from here on.  The set stays spread over the
                    // render, which random sampling would not guarantee.
                    kept = kept.iter().copied().step_by(2).collect();
                    stride = stride.saturating_mul(2);
                }
            }
            seen += 1;
        }
    }
    if kept.len() < MIN_SAMPLES {
        return collect_samples(window, target);
    }
    let step = kept.len().div_ceil(target.max(1));
    kept.iter()
        .step_by(step)
        .map(|&(x, y)| {
            let index = (y as usize * width + x as usize) * 4;
            Sample {
                x,
                y,
                color: pixels[index..index + 4].try_into().unwrap(),
            }
        })
        .collect()
}

/// How many of `samples` agree with the frame when the render is placed at
/// `(origin_x, origin_y)`.
fn matches_at(
    frame: &Frame,
    frame_pixels: &[u8],
    samples: &[Sample],
    origin_x: i64,
    origin_y: i64,
) -> usize {
    let frame_width = frame.size().width as usize;
    let frame_height = frame.size().height as usize;
    let mut matched = 0;
    for sample in samples {
        let fx = origin_x + i64::from(sample.x);
        let fy = origin_y + i64::from(sample.y);
        if fx < 0 || fy < 0 {
            continue;
        }
        let (fx, fy) = (fx as usize, fy as usize);
        if fx >= frame_width || fy >= frame_height {
            continue;
        }
        let index = (fy * frame_width + fx) * 4;
        // The screen pixel is `render + background · (1−α)` with the render
        // premultiplied, so the render's own colour is the *floor*: the
        // unknown background can only add, up to `(255−α)` per channel.  (A
        // straight-alpha reading would scale the expectation down and let a
        // position that is 40% wrong outscore the true one — measured on a
        // translucent kitty, the true position matched 25% of samples that
        // way and the premultiplied model matches 100%.)
        //
        // The floor is a hard bound, not a slack-clamped one: the only way a
        // screen pixel falls below the render's premultiplied colour is
        // compression noise or an overwrite by something on top, and both are
        // reasons to distrust the sample rather than to widen the interval —
        // a wide interval is what let the match drift two pixels off on a
        // noisy background.
        let span = (255 - i32::from(sample.color[3])) + MATCH_SLACK;
        let within = [0, 1, 2].iter().all(|&channel| {
            let rendered = i32::from(frame_pixels[index + channel]);
            let premultiplied = i32::from(sample.color[channel]);
            rendered >= premultiplied && rendered - premultiplied <= span
        });
        if within {
            matched += 1;
        }
    }
    matched
}

/// Sweeps every `step`-th position, scoring with `samples`, and returns the
/// best `(x, y, matched)`.
fn best_position(
    frame: &Frame,
    frame_pixels: &[u8],
    samples: &[Sample],
    window: &Frame,
    step: u32,
) -> Option<(i64, i64, usize)> {
    let last_x = i64::from(frame.size().width) - i64::from(window.size().width);
    let last_y = i64::from(frame.size().height) - i64::from(window.size().height);
    if last_x < 0 || last_y < 0 {
        return None;
    }
    let mut best: Option<(usize, i64, i64)> = None;
    let mut y = 0;
    while y <= last_y {
        let mut x = 0;
        while x <= last_x {
            let matched = matches_at(frame, frame_pixels, samples, x, y);
            if best.is_none_or(|(best_matched, _, _)| matched > best_matched) {
                best = Some((matched, x, y));
            }
            x += i64::from(step);
        }
        y += i64::from(step);
    }
    best.map(|(matched, x, y)| (x, y, matched))
}

/// The rectangle on `frame` where `window`'s render sits, or `None` when no
/// position matches well enough to be trusted.
pub fn locate_window(window: &Frame, frame: &Frame) -> Option<Rect> {
    if window.size().width == 0
        || window.size().height == 0
        || window.size().width > frame.size().width
        || window.size().height > frame.size().height
    {
        return None;
    }
    let frame_pixels = frame.pixels();

    // Coarse: a sweep every [`COARSE_STEP`] narrows the neighbourhood.  Both
    // passes sample structure, not just opacity — see
    // [`collect_structure_samples`] for the neighbour window that opacity
    // sampling put a capture on.
    //
    // The coarse pass narrows, it does not judge: the verdict is the
    // refinement's, at full resolution.  It used to require `MIN_MATCH_RATIO`
    // as well, which is what structure sampling breaks — its samples are sharp,
    // so a coarse grid point two pixels off the truth scores well under the
    // ratio (measured on a 60×40 texture pasted at (210,130): the truth scores
    // 153/153, the grid point (208,132) only 70/153).  Judging there rejected
    // windows the old sampler could still locate, so the gate is gone and only
    // "nothing anywhere matched" stops the search.
    let coarse = collect_structure_samples(window, COARSE_SAMPLES);
    if coarse.len() < MIN_SAMPLES {
        return None;
    }
    let (coarse_x, coarse_y, coarse_matched) =
        best_position(frame, frame_pixels, &coarse, window, COARSE_STEP)?;
    if coarse_matched == 0 {
        return None;
    }

    // Refine: full-resolution samples around the coarse winner.
    let refine = collect_structure_samples(window, REFINE_SAMPLES);
    let mut best: Option<(usize, i64, i64)> = None;
    for dy in -REFINE_RADIUS..=REFINE_RADIUS {
        for dx in -REFINE_RADIUS..=REFINE_RADIUS {
            let x = coarse_x + i64::from(dx);
            let y = coarse_y + i64::from(dy);
            if x < 0 || y < 0 {
                continue;
            }
            let matched = matches_at(frame, frame_pixels, &refine, x, y);
            if best.is_none_or(|(best_matched, _, _)| matched > best_matched) {
                best = Some((matched, x, y));
            }
        }
    }
    let (matched, x, y) = best?;
    if (matched as f64 / refine.len() as f64) < MIN_MATCH_RATIO {
        return None;
    }
    Some(Rect::new(
        x.try_into().ok()?,
        y.try_into().ok()?,
        window.size().width,
        window.size().height,
    ))
}

/// Whether `window`'s render sits at `(origin_x, origin_y)` on `frame`, by the
/// same sample-and-ratio test [`locate_window`] judges a searched position
/// with.
///
/// This is the check a position that was *stated* rather than searched for
/// needs.  A floating window's position comes from niri's IPC, which is a
/// different moment from the grab: a window dragged or animated between the two
/// would otherwise be cropped where it used to be — silently, since the
/// arithmetic succeeds on stale numbers just as well as on current ones.
///
/// The ratio is taken over the samples that fall inside `frame`, so a window
/// hanging off the edge is still judged on the part that is there (which is the
/// part a crop can use).
///
/// `None` when the position cannot be checked at all: too little structure in
/// the render to judge with, or too little of it inside the frame.  The caller
/// must not read that as agreement — it is the absence of evidence.
pub fn matches_at_position(
    window: &Frame,
    frame: &Frame,
    origin_x: i32,
    origin_y: i32,
) -> Option<bool> {
    let (width, height) = (window.size().width, window.size().height);
    if width == 0 || height == 0 {
        return None;
    }
    let samples = collect_structure_samples(window, REFINE_SAMPLES);
    if samples.len() < MIN_SAMPLES {
        return None;
    }
    let (frame_width, frame_height) = (
        i64::from(frame.size().width),
        i64::from(frame.size().height),
    );
    let (origin_x, origin_y) = (i64::from(origin_x), i64::from(origin_y));
    let in_frame = samples
        .iter()
        .filter(|sample| {
            let x = origin_x + i64::from(sample.x);
            let y = origin_y + i64::from(sample.y);
            x >= 0 && y >= 0 && x < frame_width && y < frame_height
        })
        .count();
    if in_frame < MIN_SAMPLES {
        return None;
    }
    let matched = matches_at(frame, frame.pixels(), &samples, origin_x, origin_y);
    Some((matched as f64 / in_frame as f64) >= MIN_MATCH_RATIO)
}

/// Whether two renders of one window show the same picture: the same size,
/// and the same colour on every sampled opaque pixel.  Both renders come from
/// niri rendering the same window into a buffer, so a window whose content has
/// not moved on encodes byte for byte the same — the comparison is exact.
///
/// This is how the caller tells *why* a match failed: a window whose own
/// pixels moved on between the render and the grab (a video, an animation)
/// made the template stale, and a fresh render is worth another try — while a
/// render that is current but cannot be verified (nearly invisible, off the
/// output's edge, invisible workspace) is a positional dead end, and no retry
/// will ever help.
pub fn renders_agree(a: &Frame, b: &Frame) -> bool {
    if a.size() != b.size() {
        return false;
    }
    let samples = collect_samples(a, REFINE_SAMPLES);
    if samples.len() < MIN_SAMPLES {
        // Nothing verifiable in either render; the sizes agreeing is all the
        // stability that can be stated.
        return true;
    }
    let pixels = b.pixels();
    let width = a.size().width as usize;
    samples.iter().all(|sample| {
        let index = (sample.y as usize * width + sample.x as usize) * 4;
        pixels
            .get(index..index + 4)
            .is_some_and(|pixel| pixel == sample.color)
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::geometry::Point;

    /// A deterministic noisy background, so a wrong position cannot resemble
    /// the window by accident.
    fn noise_frame(width: u32, height: u32) -> Frame {
        let mut pixels = Vec::with_capacity(width as usize * height as usize * 4);
        for y in 0..height {
            for x in 0..width {
                let value = ((x * 7 + y * 13) % 251) as u8;
                pixels.extend_from_slice(&[
                    value,
                    value.wrapping_mul(3),
                    value.wrapping_add(9),
                    255,
                ]);
            }
        }
        Frame::new(Size::new(width, height), pixels).unwrap()
    }

    /// A textured window render: every pixel distinct, so only the true
    /// position can match.  The colour is premultiplied by the alpha, the form
    /// niri writes `screenshot-window` output in — measured against a live
    /// niri, every pixel of such a render obeys RGB ≤ alpha.
    fn textured_window(width: u32, height: u32, alpha: u8) -> Frame {
        let mut pixels = Vec::with_capacity(width as usize * height as usize * 4);
        for y in 0..height {
            for x in 0..width {
                let scale = |v: u32| ((v * u32::from(alpha) + 127) / 255) as u8;
                pixels.extend_from_slice(&[
                    scale(x * 5 + 11),
                    scale(y * 9 + 23),
                    scale(x * 3 + y * 4 + 41),
                    alpha,
                ]);
            }
        }
        Frame::new(Size::new(width, height), pixels).unwrap()
    }

    /// A screen frame with `window` composited over it the way a compositor
    /// does in premultiplied space: `screen = render + background · (1−α)`,
    /// matching what niri actually puts on the screen for a render it wrote
    /// premultiplied.  The original background stays fully recoverable — this
    /// is a paste onto an opaque frame, so the output alpha stays 255.
    fn composited(background: &Frame, window: &Frame, origin: Point) -> Frame {
        let mut pixels = background.pixels().to_vec();
        let width = background.size().width as usize;
        for y in 0..window.size().height {
            for x in 0..window.size().width {
                let Some(color) = window.pixel(Point::new(x as i32, y as i32)) else {
                    continue;
                };
                let alpha = i32::from(color[3]);
                if alpha == 0 {
                    continue;
                }
                let px = i64::from(origin.x) + i64::from(x);
                let py = i64::from(origin.y) + i64::from(y);
                if px < 0 || py < 0 {
                    continue;
                }
                let (px, py) = (px as usize, py as usize);
                let index = (py * width + px) * 4;
                for channel in 0..3 {
                    let rendered = i32::from(pixels[index + channel]);
                    let premultiplied = i32::from(color[channel]);
                    pixels[index + channel] =
                        (premultiplied + rendered * (255 - alpha) / 255).min(255) as u8;
                }
            }
        }
        Frame::new(background.size(), pixels).unwrap()
    }

    #[test]
    fn an_opaque_window_is_found_where_it_was_pasted() {
        let frame = noise_frame(400, 300);
        let window = textured_window(60, 40, 255);
        let screen = composited(&frame, &window, Point::new(123, 87));
        assert_eq!(
            locate_window(&window, &screen),
            Some(Rect::new(123, 87, 60, 40))
        );
    }

    #[test]
    fn a_translucent_window_is_found_through_its_background() {
        // 80% opacity: the screen pixel is four fifths window, one fifth
        // background — exactly the mix the match has to tolerate.
        let frame = noise_frame(400, 300);
        let window = textured_window(60, 40, 204);
        let screen = composited(&frame, &window, Point::new(210, 130));
        assert_eq!(
            locate_window(&window, &screen),
            Some(Rect::new(210, 130, 60, 40))
        );
    }

    #[test]
    fn a_wrong_position_never_outranks_the_true_one() {
        // The regression behind the premultiplied model: with the render read
        // as straight alpha, a translucent window's expected colours came out
        // a full background-share too dark, the true position scored far below
        // the trust threshold, and somewhere on a busy desktop an area 40%
        // wrong cleared it — vshot cropped the wrong part of the screen.  With
        // the premultiplied reading the true position scores at or near 100%,
        // so anything past the threshold has to be the window itself.
        let frame = noise_frame(400, 300);
        let window = textured_window(60, 40, 204);
        let screen = composited(&frame, &window, Point::new(210, 130));
        // Somewhere else entirely must not match.
        assert_eq!(
            locate_window(&window, &screen),
            Some(Rect::new(210, 130, 60, 40))
        );
    }

    /// A window that is almost entirely one flat colour with a single
    /// structured block in it — a terminal showing a little text on a solid
    /// background, measured on a live kitty render at 99.68% flat.
    ///
    /// The block's position is what makes this a regression frame: the coarse
    /// pass strides its sample grid by the render's area (a stride of 28 on a
    /// 400×300 render), and the block sits *between* those grid lines, so an
    /// opacity-sampling search samples the flat background alone and locates
    /// the window wherever that colour happens to match.
    fn mostly_flat_window(width: u32, height: u32) -> Frame {
        let mut pixels = vec![0u8; width as usize * height as usize * 4];
        for y in 0..height {
            for x in 0..width {
                let index = (y as usize * width as usize + x as usize) * 4;
                // Premultiplied: RGB ≤ alpha, the form niri writes.
                pixels[index..index + 4].copy_from_slice(&[200, 194, 180, 204]);
            }
        }
        // A striped block at x=100..105, y=40..60 — off the coarse grid.
        for y in 40..60u32 {
            for x in 100..105u32 {
                let index = (y as usize * width as usize + x as usize) * 4;
                let color: [u8; 4] = match (x * 37 + y * 11) % 3 {
                    0 => [40, 40, 40, 255],
                    1 => [220, 220, 220, 255],
                    _ => [30, 90, 200, 255],
                };
                pixels[index..index + 4].copy_from_slice(&color);
            }
        }
        Frame::new(Size::new(width, height), pixels).unwrap()
    }

    #[test]
    fn a_flat_window_is_not_located_where_its_background_happens_to_match() {
        // The regression behind a capture of the window *next door*, seen on a
        // live niri with two kitty windows side by side: the right-hand
        // window's render is 99.68% one flat colour, that colour also covers
        // most of the desktop and of its neighbour, so the true position scored
        // 180/182 coarse samples while a position 850 device pixels away scored
        // 182/182 — and the sweep takes the first best, so the window was
        // captured from the wrong rectangle.  Sampling structure instead of
        // opacity scores this frame 100% at the truth and well under 70% on any
        // decoy.
        //
        // The desktop here is the same hue as the window's background, and
        // differs from it enough that a flat-background match would still pass:
        // that is what makes the frame a regression rather than a formality.
        let mut background = vec![0u8; 900 * 700 * 4];
        for pixel in background.chunks_exact_mut(4) {
            pixel.copy_from_slice(&[204, 198, 184, 255]);
        }
        let frame = Frame::new(Size::new(900, 700), background).unwrap();
        let window = mostly_flat_window(400, 300);
        let screen = composited(&frame, &window, Point::new(500, 300));
        assert_eq!(
            locate_window(&window, &screen),
            Some(Rect::new(500, 300, 400, 300)),
            "the structured block has to decide the position, not the flat background"
        );
    }

    #[test]
    fn a_render_with_nothing_structured_falls_back_to_the_opaque_sampler() {
        // A plain solid window has no structure to sample, and refusing to
        // locate it would be a worse answer than the old sampler's: the
        // fallback keeps that behaviour, and the match ratio still decides.
        let window = Frame::solid(Size::new(60, 40), [30, 30, 200, 204]).unwrap();
        let samples = collect_structure_samples(&window, COARSE_SAMPLES);
        assert!(
            !samples.is_empty(),
            "a structured-less render still samples its opaque pixels"
        );
    }

    #[test]
    fn samples_spread_over_both_axes_of_a_tall_render() {
        // The regression behind a two-pixel-right, four-pixel-down location on
        // a real 1880×2112 window: a single-stride row walk spent the whole
        // sample budget on the top few rows, so the vertical position was
        // barely constrained and the located rectangle settled off the true
        // one — cropping the left and top border out of the capture.  The
        // samples have to reach the bottom of the render, not just its top.
        let window = textured_window(1880, 2112, 255);
        let samples = collect_samples(&window, REFINE_SAMPLES);
        let deepest = samples.iter().map(|sample| sample.y).max().unwrap();
        assert!(
            deepest > window.size().height / 2,
            "samples stop at y={deepest} of {}",
            window.size().height
        );
        let widest = samples.iter().map(|sample| sample.x).max().unwrap();
        assert!(
            widest > window.size().width / 2,
            "samples stop at x={widest} of {}",
            window.size().width
        );
    }

    #[test]
    fn a_tall_window_is_found_exactly_where_it_was_pasted() {
        // The end-to-end form of the same guard: with the samples collapsed
        // onto the top rows this window located a few pixels away from the
        // truth, which is what put the crop out of step with the border.
        let frame = noise_frame(900, 1200);
        let window = textured_window(300, 900, 255);
        let screen = composited(&frame, &window, Point::new(120, 240));
        assert_eq!(
            locate_window(&window, &screen),
            Some(Rect::new(120, 240, 300, 900))
        );
    }

    #[test]
    fn a_window_that_is_not_on_the_frame_is_not_found() {
        let frame = noise_frame(400, 300);
        let window = textured_window(60, 40, 255);
        assert_eq!(locate_window(&window, &frame), None);
    }

    #[test]
    fn a_render_too_translucent_to_verify_is_reported_as_unlocatable() {
        // Alpha 100: the background dominates every pixel, and no position can
        // be verified — the caller is told so instead of being handed a guess.
        let frame = noise_frame(400, 300);
        let window = textured_window(60, 40, 100);
        assert_eq!(locate_window(&window, &frame), None);
    }

    #[test]
    fn a_window_larger_than_the_frame_is_not_found() {
        let frame = noise_frame(100, 80);
        let window = textured_window(120, 90, 255);
        assert_eq!(locate_window(&window, &frame), None);
    }

    #[test]
    fn two_renders_of_unchanged_content_agree() {
        let window = textured_window(60, 40, 204);
        assert!(renders_agree(&window, &window.clone()));
    }

    #[test]
    fn a_changed_pixel_disagrees() {
        // The window's content moved on between the two renders: the very
        // first sampled pixel (top left is always on the sample grid) carries
        // another colour, and stability must not be claimed.
        let a = textured_window(60, 40, 204);
        let mut pixels = a.pixels().to_vec();
        pixels[0..3].copy_from_slice(&[1, 2, 3]);
        let b = Frame::new(a.size(), pixels).unwrap();
        assert!(!renders_agree(&a, &b));
    }

    #[test]
    fn a_resize_is_not_stability() {
        let a = textured_window(60, 40, 204);
        let b = textured_window(61, 40, 204);
        assert!(!renders_agree(&a, &b));
    }

    #[test]
    fn a_render_with_nothing_to_verify_is_called_stable_when_its_size_holds() {
        // Alpha 100: no sample qualifies, so there is nothing to compare —
        // sizes agreeing is the whole verdict, and the caller treats it as the
        // positional dead end it is.
        let a = textured_window(60, 40, 100);
        assert!(renders_agree(&a, &a.clone()));
    }
}

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
//! disagree.  A coarse pass at quarter resolution narrows the position down, a
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

    // Coarse: a quarter-resolution sweep finds the neighbourhood.
    let coarse = collect_samples(window, COARSE_SAMPLES);
    if coarse.len() < MIN_SAMPLES {
        return None;
    }
    let (coarse_x, coarse_y, coarse_matched) =
        best_position(frame, frame_pixels, &coarse, window, COARSE_STEP)?;
    if (coarse_matched as f64 / coarse.len() as f64) < MIN_MATCH_RATIO {
        return None;
    }

    // Refine: full-resolution samples around the coarse winner.
    let refine = collect_samples(window, REFINE_SAMPLES);
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

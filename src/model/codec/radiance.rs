// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Radiance RGBE: the exact, archival HDR format.
//!
//! RGBE holds the light as one shared exponent and three 8-bit mantissas, so it
//! is lossless in the sense that matters for a container — nothing is thrown
//! away to compression — while the values themselves are quantized to about a
//! 0.4 % relative step.  It has no colorimetry of its own, so the gamut is
//! declared in a `PRIMARIES=` header, and, because the frame's `1.0` is the SDR
//! white it was captured against rather than a fixed level, in a
//! `REFERENCE_NITS=` header beside it.
//!
//! Readers that matter elsewhere (ffmpeg, ImageMagick) ignore both headers and
//! assume Rec.709, which is why the SDR half written beside a Radiance file is
//! the one that is converted for them.

use crate::error::{Result, VshotError};
use crate::geometry::Size;
use crate::model::codec::{HdrCodec, HdrImage};
use crate::model::hdr::{HdrFrame, Primaries, D65};
use crate::parallel::collect_rows;

/// The Radiance codec.
pub struct Radiance;

impl HdrCodec for Radiance {
    fn extension(&self) -> &'static str {
        "hdr"
    }

    fn name(&self) -> &'static str {
        "hdr"
    }

    fn encode(&self, image: &HdrImage) -> Result<Vec<u8>> {
        Ok(encode(&image.frame, image.white()))
    }

    fn decode(&self, bytes: &[u8], fallback_nits: f32) -> Result<HdrImage> {
        decode(bytes, fallback_nits, "<memory>")
    }
}

/// Writes `frame` as a Radiance RGBE file whose `1.0` is `reference_nits`.
///
/// The pixels go out in the frame's own primaries, unconverted: a wide-gamut
/// capture stays wide-gamut, and the header is what says so.
pub fn encode(frame: &HdrFrame, reference_nits: f32) -> Vec<u8> {
    let width = frame.size().width;
    let height = frame.size().height;
    let mut out = Vec::new();
    out.extend_from_slice(b"#?RADIANCE\n");
    out.extend_from_slice(b"FORMAT=32-bit_rle_rgbe\n");
    let [r, g, b] = frame.primaries().chromaticities();
    out.extend_from_slice(
        format!(
            "PRIMARIES={:.6} {:.6} {:.6} {:.6} {:.6} {:.6} {:.6} {:.6}\n",
            r.0, r.1, g.0, g.1, b.0, b.1, D65.0, D65.1
        )
        .as_bytes(),
    );
    // The light a code of 1.0 stands for.  Non-standard, like `PRIMARIES=`, and
    // read by this program and nothing else; without it a decode cannot know
    // whether the frame's white was 203 cd/m² or the 100 of some other output,
    // and would show the picture at the wrong brightness.
    out.extend_from_slice(format!("REFERENCE_NITS={reference_nits:.2}\n").as_bytes());
    out.extend_from_slice(b"\n");
    out.extend_from_slice(format!("-Y {height} +X {width}\n").as_bytes());
    let rle = (8..=0x7fff).contains(&width);
    // Scanlines are independent — a run never crosses one — so each is encoded
    // on its own and they are laid down in order.  The chunks the encoder is
    // split into are whole rows, and every row in one gets its own scanline:
    // emitting a chunk as a single scanline would shift every row after the
    // first.
    for rows in collect_rows(frame.pixels(), width as usize, |chunk| {
        let mut bytes = Vec::new();
        for scanline in chunk.chunks(width as usize) {
            encode_scanline(scanline, width, rle, &mut bytes);
        }
        bytes
    }) {
        out.extend_from_slice(&rows);
    }
    out
}

/// Reads a Radiance RGBE file back into the currency.
///
/// `fallback_nits` is the reference white to read the file at when it carries no
/// `REFERENCE_NITS=` header of its own — every Radiance file written by anything
/// but this program.  `origin` names the file in errors; it is passed rather
/// than read off a path so that the byte form and the path form report the same
/// way.
pub fn decode(bytes: &[u8], fallback_nits: f32, origin: &str) -> Result<HdrImage> {
    let header = parse_header(bytes, fallback_nits, origin)?;
    let size = Size::new(header.width, header.height);
    let expected = size
        .area()
        .map_err(|_| bad(origin, "image size is out of range"))?;
    let mut pixels = vec![[0.0f32; 4]; expected];
    let mut at = header.data_offset;
    let rle = (8..=0x7fff).contains(&header.width);
    for row in 0..header.height as usize {
        if rle {
            at = decode_rle_scanline(bytes, at, header.width as usize, origin, |index, rgbe| {
                pixels[row * header.width as usize + index] = rgbe_to_pixel(rgbe);
            })?;
        } else {
            let stride = header.width as usize * 4;
            let end = at + stride;
            let Some(scanline) = bytes.get(at..end) else {
                return Err(bad(origin, "the pixel data ends early"));
            };
            for (index, rgbe) in scanline.as_chunks::<4>().0.iter().enumerate() {
                pixels[row * header.width as usize + index] = rgbe_to_pixel(*rgbe);
            }
            at = end;
        }
    }
    let frame = HdrFrame::in_primaries(size, pixels, header.primaries)?;
    Ok(HdrImage::new(frame, header.reference_nits))
}

/// The header's answers: what the gamut and white are, how big the image is,
/// and where its pixels start.
struct Header {
    primaries: Primaries,
    reference_nits: f32,
    width: u32,
    height: u32,
    data_offset: usize,
}

/// Reads the header up to and including the resolution line.
///
/// The gamut and the reference white are both optional, because a Radiance file
/// from anywhere but this program has neither: `PRIMARIES=` is a VShot
/// extension and `REFERENCE_NITS=` is one of its own, so a foreign file is read
/// as Rec.709 at `fallback_nits` — the assumptions every other Radiance reader
/// makes, with the white the user's own setting names — rather than refused.
fn parse_header(bytes: &[u8], fallback_nits: f32, origin: &str) -> Result<Header> {
    let mut primaries = None;
    let mut reference_nits = None;
    let mut at = 0usize;
    let mut resolution = None;
    // Line by line over the bytes rather than over a decoded string: the header
    // is text and the pixels that follow it are not, so decoding the whole file
    // as UTF-8 would refuse every valid one.
    while at < bytes.len() {
        let end = match bytes[at..].iter().position(|byte| *byte == b'\n') {
            Some(offset) => at + offset + 1,
            None => bytes.len(),
        };
        let line = std::str::from_utf8(&bytes[at..end])
            .map_err(|_| bad(origin, "not a Radiance file: the header is not text"))?;
        let trimmed = line.trim_end_matches(['\n', '\r']);
        let next = end;
        if trimmed.is_empty() {
            at = next;
            continue;
        }
        if let Some(values) = trimmed.strip_prefix("PRIMARIES=") {
            primaries = parse_primaries(values);
            at = next;
            continue;
        }
        if let Some(value) = trimmed.strip_prefix("REFERENCE_NITS=") {
            reference_nits = value.trim().parse::<f32>().ok().filter(|n| *n > 0.0);
            at = next;
            continue;
        }
        if trimmed.starts_with("-Y ") {
            resolution = Some((trimmed.to_string(), next));
            break;
        }
        at = next;
    }
    let Some((resolution, data_offset)) = resolution else {
        return Err(bad(
            origin,
            "no `-Y height +X width` resolution line; only that orientation is read",
        ));
    };
    let (width, height) = parse_resolution(&resolution)
        .ok_or_else(|| bad(origin, "the resolution line is malformed"))?;
    Ok(Header {
        primaries: primaries.unwrap_or(Primaries::Bt709),
        reference_nits: reference_nits
            .unwrap_or_else(|| crate::model::hdr::clamp_reference_nits(fallback_nits)),
        width,
        height,
        data_offset,
    })
}

/// `-Y height +X width`, the orientation every writer of this format emits.
fn parse_resolution(line: &str) -> Option<(u32, u32)> {
    let mut parts = line.split_whitespace();
    if parts.next()? != "-Y" {
        return None;
    }
    let height = parts.next()?.parse::<u32>().ok()?;
    if parts.next()? != "+X" {
        return None;
    }
    let width = parts.next()?.parse::<u32>().ok()?;
    Some((width, height))
}

/// The eight chromaticity coordinates `PRIMARIES=` carries: the three primaries
/// and the white, the same order the encoder writes them in.
fn parse_primaries(values: &str) -> Option<Primaries> {
    let numbers: Vec<f32> = values
        .split_whitespace()
        .filter_map(|value| value.parse::<f32>().ok())
        .collect();
    if numbers.len() < 6 {
        return None;
    }
    Some(Primaries::from_chromaticities(
        (numbers[0], numbers[1]),
        (numbers[2], numbers[3]),
        (numbers[4], numbers[5]),
    ))
}

/// Decodes one run-length scanline, calling `put` for each pixel in order.
///
/// Returns the offset just past the scanline.  The four component planes are
/// stored one after another, each run-length encoded on its own, so the pixels
/// are only assembled once all four have been read.
fn decode_rle_scanline(
    bytes: &[u8],
    at: usize,
    width: usize,
    origin: &str,
    mut put: impl FnMut(usize, [u8; 4]),
) -> Result<usize> {
    let Some(prefix) = bytes.get(at..at + 4) else {
        return Err(bad(origin, "a scanline header is missing"));
    };
    if prefix[0] != 2 || prefix[1] != 2 || (prefix[2] & 0x80) != 0 {
        return Err(bad(origin, "a scanline is not in run-length form"));
    }
    let declared = ((prefix[2] as usize) << 8) | prefix[3] as usize;
    if declared != width {
        return Err(bad(origin, "a scanline's width does not match the header"));
    }
    let mut at = at + 4;
    let mut planes = vec![vec![0u8; width]; 4];
    for plane in planes.iter_mut() {
        let mut index = 0;
        while index < width {
            let Some(&count) = bytes.get(at) else {
                return Err(bad(origin, "a scanline's data ends early"));
            };
            at += 1;
            if count > 128 {
                let run = count as usize - 128;
                let Some(&value) = bytes.get(at) else {
                    return Err(bad(origin, "a run has no value"));
                };
                at += 1;
                if index + run > width {
                    return Err(bad(origin, "a run overruns its scanline"));
                }
                plane[index..index + run].fill(value);
                index += run;
            } else {
                let run = count as usize;
                let Some(literals) = bytes.get(at..at + run) else {
                    return Err(bad(origin, "a literal run is cut short"));
                };
                at += run;
                if index + run > width {
                    return Err(bad(origin, "a literal run overruns its scanline"));
                }
                plane[index..index + run].copy_from_slice(literals);
                index += run;
            }
        }
    }
    for index in 0..width {
        put(
            index,
            [
                planes[0][index],
                planes[1][index],
                planes[2][index],
                planes[3][index],
            ],
        );
    }
    Ok(at)
}

/// One RGBE quad back to linear light where `1.0` is the file's reference white,
/// which is the scale the encoder wrote it in.
fn rgbe_to_pixel(rgbe: [u8; 4]) -> [f32; 4] {
    let [r, g, b] = rgbe_to_rgb(rgbe);
    [r, g, b, 1.0]
}

/// The inverse of [`to_rgbe`]: mantissas scaled by the shared exponent.
fn rgbe_to_rgb(rgbe: [u8; 4]) -> [f32; 3] {
    let exponent = rgbe[3];
    if exponent == 0 {
        return [0.0, 0.0, 0.0];
    }
    let scale = 2f32.powi(exponent as i32 - 128 - 8);
    [
        f32::from(rgbe[0]) * scale,
        f32::from(rgbe[1]) * scale,
        f32::from(rgbe[2]) * scale,
    ]
}

/// Appends one Radiance scanline: the run-length form when the width allows it,
/// which is every width but a tiny one, and the flat form otherwise.
fn encode_scanline(scanline: &[[f32; 4]], width: u32, rle: bool, out: &mut Vec<u8>) {
    if rle {
        out.extend_from_slice(&[2, 2, (width >> 8) as u8, (width & 0xff) as u8]);
        // Four component planes, each run-length encoded on its own.
        for channel in 0..4 {
            let plane: Vec<u8> = scanline
                .iter()
                .map(|pixel| to_rgbe(*pixel)[channel])
                .collect();
            encode_rle_plane(&plane, out);
        }
    } else {
        for pixel in scanline {
            out.extend_from_slice(&to_rgbe(*pixel));
        }
    }
}

/// Linear RGB to Radiance RGBE: one shared exponent, three 8-bit mantissas.
fn to_rgbe(pixel: [f32; 4]) -> [u8; 4] {
    let r = pixel[0].max(0.0);
    let g = pixel[1].max(0.0);
    let b = pixel[2].max(0.0);
    let peak = r.max(g).max(b);
    if !peak.is_finite() || peak < 1.0e-32 {
        return [0, 0, 0, 0];
    }
    // v = mantissa * 2^exponent with mantissa in [0.5, 1).
    let exponent = peak.log2().floor() as i32 + 1;
    let mantissa = peak / 2f32.powi(exponent);
    let scale = mantissa * 256.0 / peak;
    [
        (r * scale).clamp(0.0, 255.0) as u8,
        (g * scale).clamp(0.0, 255.0) as u8,
        (b * scale).clamp(0.0, 255.0) as u8,
        (exponent + 128).clamp(0, 255) as u8,
    ]
}

/// One Radiance run-length plane: runs of four or more bytes are stored as
/// `(128 + count, byte)`, everything else as a literal `(count, bytes...)`.
fn encode_rle_plane(plane: &[u8], out: &mut Vec<u8>) {
    let mut index = 0;
    while index < plane.len() {
        let mut run = 1;
        while index + run < plane.len() && plane[index + run] == plane[index] && run < 127 {
            run += 1;
        }
        if run >= 4 {
            out.push(128 + run as u8);
            out.push(plane[index]);
            index += run;
            continue;
        }
        // Gather literals until a run of four starts.
        let start = index;
        while index < plane.len() {
            if index + 3 < plane.len()
                && plane[index] == plane[index + 1]
                && plane[index] == plane[index + 2]
                && plane[index] == plane[index + 3]
            {
                break;
            }
            index += 1;
            if index - start == 128 {
                break;
            }
        }
        out.push((index - start) as u8);
        out.extend_from_slice(&plane[start..index]);
    }
}

fn bad(origin: &str, reason: &str) -> VshotError {
    VshotError::HdrDecode {
        path: origin.into(),
        reason: reason.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::codec::HdrCodec;
    use crate::model::hdr::REFERENCE_WHITE_NITS;

    fn frame(size: Size, pixels: Vec<[f32; 4]>, primaries: Primaries) -> HdrFrame {
        HdrFrame::in_primaries(size, pixels, primaries).expect("frame")
    }

    #[test]
    fn a_radiance_file_round_trips_its_light() {
        let pixels: Vec<[f32; 4]> = (0..16)
            .map(|index| {
                let value = index as f32 / 16.0;
                [value, value * 2.0, value * 0.5, 1.0]
            })
            .collect();
        let image = HdrImage::new(
            frame(Size::new(4, 4), pixels.clone(), Primaries::Bt709),
            203.0,
        );
        let bytes = Radiance.encode(&image).expect("encode");
        let back = Radiance
            .decode(&bytes, REFERENCE_WHITE_NITS)
            .expect("decode");
        assert_eq!(back.frame.size(), image.frame.size());
        assert_eq!(back.frame.primaries(), Primaries::Bt709);
        assert!((back.reference_nits - 203.0).abs() < 0.01, "{back:?}");
        for (before, after) in pixels.iter().zip(back.frame.pixels()) {
            for channel in 0..3 {
                let delta = (before[channel] - after[channel]).abs();
                // RGBE's 8-bit mantissa is a relative step, so the error scales
                // with the value; a quarter of the value is far looser than the
                // format's own ~0.4 % and still catches a wrong scale.
                assert!(
                    delta <= before[channel] * 0.05 + 1.0e-3,
                    "{before:?} vs {after:?}"
                );
            }
        }
    }

    #[test]
    fn the_gamut_and_the_reference_white_survive_the_round_trip() {
        let image = HdrImage::new(
            frame(
                Size::new(8, 1),
                vec![[0.5, 0.5, 0.5, 1.0]; 8],
                Primaries::DisplayP3,
            ),
            100.0,
        );
        let bytes = Radiance.encode(&image).expect("encode");
        let text = String::from_utf8_lossy(&bytes);
        assert!(text.contains("PRIMARIES="), "no gamut header");
        assert!(text.contains("REFERENCE_NITS=100.00"), "no white header");
        let back = Radiance
            .decode(&bytes, REFERENCE_WHITE_NITS)
            .expect("decode");
        assert_eq!(back.frame.primaries(), Primaries::DisplayP3);
        assert!((back.reference_nits - 100.0).abs() < 0.01);
    }

    #[test]
    fn a_file_without_the_vshot_headers_reads_as_bt709_at_the_reference() {
        // What any other Radiance writer produces: no gamut, no white.
        let mut bytes = Vec::new();
        bytes.extend_from_slice(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n");
        // Two flat pixels, the non-run-length form (width below 8).
        bytes.extend_from_slice(&[128, 128, 128, 129]);
        bytes.extend_from_slice(&[64, 64, 64, 130]);
        let image = Radiance
            .decode(&bytes, REFERENCE_WHITE_NITS)
            .expect("decode");
        assert_eq!(image.frame.primaries(), Primaries::Bt709);
        assert!((image.reference_nits - REFERENCE_WHITE_NITS).abs() < 0.01);
        // And the same file read for a display whose white is another level is
        // read at that level: the header is absent, so the caller's answer is
        // the only one there is.
        let brighter = Radiance.decode(&bytes, 300.0).expect("decode");
        assert!((brighter.reference_nits - 300.0).abs() < 0.01);
    }

    #[test]
    fn a_file_that_is_not_radiance_is_refused() {
        assert!(decode(b"\x89PNG\r\n\x1a\n", REFERENCE_WHITE_NITS, "x").is_err());
        assert!(decode(b"#?RADIANCE\nno resolution\n", REFERENCE_WHITE_NITS, "x").is_err());
    }

    #[test]
    fn a_truncated_file_is_refused_rather_than_read_short() {
        let image = HdrImage::new(
            frame(
                Size::new(16, 2),
                vec![[0.25, 0.25, 0.25, 1.0]; 32],
                Primaries::Bt709,
            ),
            203.0,
        );
        let bytes = Radiance.encode(&image).expect("encode");
        let cut = &bytes[..bytes.len() - 8];
        assert!(decode(cut, REFERENCE_WHITE_NITS, "x").is_err());
    }
}

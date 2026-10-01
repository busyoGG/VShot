// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Writing an HDR frame as a ten-bit AVIF.
//!
//! AVIF is the HDR half's default format because it is the one an HDR file can
//! carry *and* state: it is ten bits per channel, it can be BT.2020 with the PQ
//! transfer, and the `colr` box names all three of those in the CICP fields a
//! reader already understands.  Radiance RGBE, the other format here, holds the
//! light but has no colorimetry at all — its gamut is a non-standard header most
//! readers ignore (see [`HdrFrame::encode_radiance`]).
//!
//! The cost is that AVIF is lossy.  That is what the choice is for: AVIF is
//! small and universally readable, Radiance is exact and read by few.
//!
//! The pixels go through the standard path for HDR stills: the frame's PQ codes
//! in BT.2020, converted to the non-constant-luminance YCbCr of BT.2020 with the
//! matrix applied to the *PQ signal* (which is what CICP says a matrix
//! coefficient applies to), at full range and 4:4:4.  The gamut is not converted
//! — a BT.2020 AVIF is where a wide-gamut capture belongs.

use rav1e::prelude::{
    ChromaSamplePosition, ChromaSampling, ColorDescription, ColorPrimaries, Config, EncoderConfig,
    EncoderStatus, FrameType, MatrixCoefficients, PixelRange, SpeedSettings,
    TransferCharacteristics,
};

use crate::error::{Result, VshotError};
use crate::model::hdr::HdrFrame;

/// How hard the encoder works: a rav1e speed preset, 0 (slowest, best) to 10.
///
/// Nine, not rav1e's own default of six, and the reason is the latency a user
/// feels.  Measured on a 2560x1440 capture of a desktop with text and a pinned
/// picture on it: about 2.5 s of encoding, 3.0 s for the whole command, against
/// 0.5 s for the Radiance path.  Preset ten cuts that to 2.4 s but makes the
/// file about 40 % bigger, which is the wrong trade for a still.
///
/// This build runs rav1e without its hand-written SIMD (the `avif-asm` feature
/// turns that on, and needs `nasm` at build time), which is most of why the
/// number is seconds rather than a fraction of one.
const SPEED: u8 = 9;

/// The base quantizer, on rav1e's 0-255 log scale: 66, which is the quality
/// `ravif` — the crate `image` uses for AVIF — calls 90 out of 100.
///
/// A screenshot is mostly flat colour and hard edges, which this holds well,
/// while an archival capture is not something to compress to the last byte: the
/// Radiance option exists for a caller who wants no loss at all.
const QUANTIZER: usize = 66;

/// Encodes `frame` as a ten-bit PQ / BT.2020 AVIF.
///
/// `reference_nits` is the light the frame's `1.0` stands for, the same value
/// [`HdrFrame::to_rgb10_pq`] takes: PQ codes are absolute, so the frame has to
/// say what its white is before it can be encoded.
pub fn encode(frame: &HdrFrame, reference_nits: f32) -> Result<Vec<u8>> {
    let width = frame.size().width as usize;
    let height = frame.size().height as usize;
    let planes = to_ycbcr(frame.to_rgb10_pq(reference_nits));

    let config = Config::new().with_encoder_config(EncoderConfig {
        width,
        height,
        bit_depth: 10,
        chroma_sampling: ChromaSampling::Cs444,
        chroma_sample_position: ChromaSamplePosition::Unknown,
        pixel_range: PixelRange::Full,
        // The CICP triple the `colr` box below repeats: a reader that looks at
        // the AV1 sequence header and one that looks at the container must agree,
        // or the image is shown with the wrong transfer function.
        color_description: Some(ColorDescription {
            color_primaries: ColorPrimaries::BT2020,
            transfer_characteristics: TransferCharacteristics::SMPTE2084,
            matrix_coefficients: MatrixCoefficients::BT2020NCL,
        }),
        still_picture: true,
        enable_timing_info: false,
        quantizer: QUANTIZER,
        min_quantizer: QUANTIZER as u8,
        speed_settings: SpeedSettings::from_preset(SPEED),
        ..EncoderConfig::default()
    });
    let av1 = encode_av1(config, width, height, &planes)?;

    // The container: the same CICP triple as the bitstream, full range, and the
    // depth.  `Aviffy` defaults to 4:4:4, which is what was encoded.
    let mut avif = avif_serialize::Aviffy::new();
    avif.set_color_primaries(avif_serialize::constants::ColorPrimaries::Bt2020)
        .set_transfer_characteristics(avif_serialize::constants::TransferCharacteristics::Smpte2084)
        .set_matrix_coefficients(avif_serialize::constants::MatrixCoefficients::Bt2020Ncl)
        .set_full_color_range(true)
        .set_chroma_subsampling((false, false));
    Ok(avif.to_vec(&av1, None, width as u32, height as u32, 10))
}

/// Runs rav1e over one still frame and answers the AV1 payload the AVIF
/// container wraps.
fn encode_av1(config: Config, width: usize, height: usize, planes: &[[u16; 3]]) -> Result<Vec<u8>> {
    let mut context = config
        .new_context::<u16>()
        .map_err(|error| VshotError::AvifEncode(error.to_string()))?;
    let mut frame = context.new_frame();
    {
        // The three planes, in the order AV1 reads them: Y, Cb, Cr.
        let mut plane_iter = frame.planes.iter_mut();
        let mut luma = plane_iter
            .next()
            .expect("a frame has a luma plane")
            .mut_slice(Default::default());
        let mut blue = plane_iter
            .next()
            .expect("Cs444 has a Cb plane")
            .mut_slice(Default::default());
        let mut red = plane_iter
            .next()
            .expect("Cs444 has a Cr plane")
            .mut_slice(Default::default());
        for (row, ((luma, blue), red)) in luma
            .rows_iter_mut()
            .zip(blue.rows_iter_mut())
            .zip(red.rows_iter_mut())
            .take(height)
            .enumerate()
        {
            let luma = &mut luma[..width];
            let blue = &mut blue[..width];
            let red = &mut red[..width];
            for (index, ((luma, blue), red)) in luma.iter_mut().zip(blue).zip(red).enumerate() {
                let pixel = planes[row * width + index];
                *luma = pixel[0];
                *blue = pixel[1];
                *red = pixel[2];
            }
        }
    }
    context
        .send_frame(frame)
        .map_err(|error| VshotError::AvifEncode(error.to_string()))?;
    context.flush();

    let mut out = Vec::new();
    loop {
        match context.receive_packet() {
            Ok(mut packet) => {
                // A still is one frame and it is a key frame; anything else is
                // not part of the image this function is asked for.
                if packet.frame_type == FrameType::KEY {
                    out.append(&mut packet.data);
                }
            }
            Err(EncoderStatus::Encoded | EncoderStatus::LimitReached) => break,
            Err(error) => return Err(VshotError::AvifEncode(error.to_string())),
        }
    }
    if out.is_empty() {
        return Err(VshotError::AvifEncode(
            "the encoder produced no key frame".into(),
        ));
    }
    Ok(out)
}

/// The BT.2020 non-constant-luminance matrix, applied to the PQ signal.
///
/// These are the coefficients the AV1/AVIF `matrix_coefficients = 9` field
/// stands for, so a reader undoes exactly this.  Chroma is offset to sit in
/// `[0, 1]` as full range wants.
fn to_ycbcr(rgb10: Vec<u32>) -> Vec<[u16; 3]> {
    // BT.2020 NCL, full range.
    const KR: f32 = 0.2627;
    const KB: f32 = 0.0593;
    const KG: f32 = 1.0 - KR - KB;
    const CB_DIVISOR: f32 = 2.0 * (1.0 - KB);
    const CR_DIVISOR: f32 = 2.0 * (1.0 - KR);

    let code = |value: f32| -> u16 { (value * 1023.0).round().clamp(0.0, 1023.0) as u16 };
    rgb10
        .into_iter()
        .map(|word| {
            let r = ((word >> 20) & 0x3ff) as f32 / 1023.0;
            let g = ((word >> 10) & 0x3ff) as f32 / 1023.0;
            let b = (word & 0x3ff) as f32 / 1023.0;
            let luma = KR * r + KG * g + KB * b;
            [
                code(luma),
                code((b - luma) / CB_DIVISOR + 0.5),
                code((r - luma) / CR_DIVISOR + 0.5),
            ]
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::geometry::Size;
    use crate::model::hdr::{Primaries, Transfer, REFERENCE_WHITE_NITS};

    /// A frame from ten-bit PQ words, the way a capture arrives.
    fn frame(words: &[u32], width: u32, height: u32) -> HdrFrame {
        HdrFrame::from_rgb10(
            words,
            Size::new(width, height),
            Transfer::Pq,
            Primaries::Bt2020,
            false,
            REFERENCE_WHITE_NITS,
        )
        .unwrap()
    }

    /// The CICP triple an AVIF declares, read back out of its `colr` box: after
    /// the `nclx` type come primaries and transfer as big-endian `u16`s, the
    /// matrix as one more, and the full-range flag in the last byte.
    fn cicp(bytes: &[u8]) -> Option<[u8; 7]> {
        let at = bytes.windows(4).position(|window| window == b"nclx")?;
        let values = bytes.get(at + 4..at + 11)?;
        let mut found = [0u8; 7];
        found.copy_from_slice(values);
        Some(found)
    }

    #[test]
    fn an_avif_is_written_with_the_hdr_colour_description() {
        // A 16x16 PQ ramp, which is the smallest rav1e accepts.
        let words: Vec<u32> = (0..256)
            .map(|index| {
                let value = (index % 16) * 64;
                (3 << 30) | (value << 20) | (value << 10) | value
            })
            .collect();
        let bytes = encode(&frame(&words, 16, 16), REFERENCE_WHITE_NITS).unwrap();
        // An ISOBMFF file with an AVIF brand.
        assert_eq!(&bytes[4..8], b"ftyp", "not an ISOBMFF file");
        assert!(
            bytes.windows(4).any(|window| window == b"avif"),
            "no avif brand"
        );
        // BT.2020 primaries (9), PQ transfer (16), BT.2020 NCL matrix (9), and
        // the full-range flag in the top bit of the last byte.
        assert_eq!(cicp(&bytes), Some([0, 9, 0, 16, 0, 9, 0x80]));
    }

    #[test]
    fn the_pq_signal_survives_the_colour_transform() {
        // Neutrals are the one thing the matrix must not move: with rows that
        // sum to one, a grey triple stays grey and its chroma lands mid-scale.
        let words = [0x3fff_ffffu32, 0x0000_0000];
        let planes = to_ycbcr(words.to_vec());
        assert_eq!(planes.len(), 2);
        // White: luma full, chroma neutral.
        assert_eq!(planes[0], [1023, 512, 512]);
        // Black: the same neutrality at the other end.
        assert_eq!(planes[1], [0, 512, 512]);
    }
}

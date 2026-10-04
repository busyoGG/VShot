// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Colour management: the conversions between the ways a colour can be written
//! and the one way this program holds it.
//!
//! The pipeline's own currency is linear light in a frame's primaries, where
//! `1.0` is the SDR white the picture was made against (see
//! [`crate::model::hdr::REFERENCE_WHITE_NITS`]).  Everything a file or a
//! compositor hands over is some other spelling of the same colour: a transfer
//! curve's code, a YCbCr triple, a ten-bit integer in a limited range.  This
//! module is where those spellings are read and written, so a codec declares
//! *which* curve and *which* matrix its bytes are in and the arithmetic lives in
//! one place rather than in each format.
//!
//! Two things follow from putting it here.  A codec that only needs to name its
//! colour description does not have to know how PQ is evaluated; and a transfer
//! curve, a matrix coefficient set or a range that turns out to be needed by a
//! second format is already written.

// Both directions of every conversion are here, because a colour description is
// symmetric and a format that reads one needs the half a format that writes one
// does not.  `Transfer::to_code` is the case in point: nothing this program
// writes is HLG or sRGB, so nothing in the program calls it, but a transfer
// curve with only a decode is half a curve.
#![allow(dead_code)]

pub use crate::model::hdr::Transfer;

use crate::model::hdr::{
    hlg_ootf, pq_oetf, srgb_oetf, HLG_PEAK_NITS, PQ_PEAK_NITS, REFERENCE_WHITE_NITS,
};

/// The reference white an absent or nonsensical one falls back to: BT.2408's.
fn reference(reference_nits: f32) -> f32 {
    if reference_nits.is_finite() && reference_nits > 0.0 {
        reference_nits
    } else {
        REFERENCE_WHITE_NITS
    }
}

impl Transfer {
    /// One channel of a `0..1` code to linear light on the frame's scale, where
    /// `1.0` is `reference_nits`.
    ///
    /// This is the per-channel half of a decode.  HLG is the exception — its
    /// opto-optical transfer needs the pixel's own luma — and goes through
    /// [`Self::to_linear_rgb`] instead.
    pub fn to_linear(self, code: f32, reference_nits: f32) -> f32 {
        crate::model::hdr::decode_transfer(code, self, reference_nits)
    }

    /// Linear light back to a `0..1` code: the inverse of [`Self::to_linear`],
    /// and what an absolute encoding needs before it can write a sample.
    ///
    /// `None` for HLG.  Its signal is scene-referred — turning display light
    /// back into one needs the inverse of the opto-optical transfer, which takes
    /// the pixel's own luma and so has no per-channel form.  Nothing in this
    /// program writes HLG (every file it produces is PQ or a linear-light
    /// container), so rather than approximate the missing half this says it is
    /// missing.
    pub fn to_code(self, linear: f32, reference_nits: f32) -> Option<f32> {
        let reference = reference(reference_nits);
        Some(match self {
            Transfer::Linear => linear,
            Transfer::Srgb => srgb_oetf(linear),
            // PQ's codes are absolute, so the frame's relative light is scaled
            // back to the 10 000 cd/m² the curve is defined over.
            Transfer::Pq => pq_oetf(linear * reference / PQ_PEAK_NITS),
            Transfer::Hlg => return None,
        })
    }

    /// A triple of `0..1` codes to linear light, applying the opto-optical
    /// transfer where the curve needs the whole pixel.
    ///
    /// This is the decode a codec wants: it has three samples and wants the
    /// colour, and whether the curve is per-channel is the curve's business.
    pub fn to_linear_rgb(self, codes: [f32; 3], reference_nits: f32) -> [f32; 3] {
        let rgb = codes.map(|code| self.to_linear(code, reference_nits));
        if self == Transfer::Hlg {
            hlg_ootf(rgb, reference_nits)
        } else {
            rgb
        }
    }

    /// The nominal peak of the curve in cd/m², for a reader that has to scale
    /// what it decodes.  `None` for the curves whose scale is the frame's own
    /// reference white.
    pub fn nominal_peak_nits(self) -> Option<f32> {
        match self {
            Transfer::Pq => Some(PQ_PEAK_NITS),
            Transfer::Hlg => Some(HLG_PEAK_NITS),
            Transfer::Linear | Transfer::Srgb => None,
        }
    }
}

/// How a YCbCr triple's samples sit in their integer range.
///
/// Full range uses the whole code space and is what a file written here uses;
/// limited (or "video") range keeps the head- and foot-room broadcast expects,
/// and is what most AV1 in the wild carries.  Reading a limited-range image as
/// full — or the reverse — is not a subtle error: it lifts black to a visible
/// grey and crushes the top of the range.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ColorRange {
    Full,
    Limited,
}

impl ColorRange {
    /// The normalized value of a raw code of `bits` bits: luma on `0..1`, chroma
    /// on `-0.5..0.5`, which is the form [`MatrixCoefficients::to_rgb`] takes.
    pub fn normalize(self, code: f32, bits: u32, luma: bool) -> f32 {
        let max = ((1u32 << bits) - 1) as f32;
        match self {
            ColorRange::Full => {
                let value = code / max;
                if luma {
                    value
                } else {
                    value - 0.5
                }
            }
            // BT.2100's narrow range, scaled to the bit depth: luma in
            // 16..235, chroma in 16..240 of an 8-bit code.
            ColorRange::Limited => {
                let step = 2f32.powi(bits as i32 - 8);
                if luma {
                    (code - 16.0 * step) / (219.0 * step)
                } else {
                    (code - 128.0 * step) / (224.0 * step)
                }
            }
        }
    }

    /// The raw code a normalized value is written as: the inverse of
    /// [`Self::normalize`], clamped to the range the codes have.
    pub fn code(self, value: f32, bits: u32, luma: bool) -> f32 {
        let max = ((1u32 << bits) - 1) as f32;
        match self {
            ColorRange::Full => {
                let value = if luma { value } else { value + 0.5 };
                (value * max).round().clamp(0.0, max)
            }
            ColorRange::Limited => {
                let step = 2f32.powi(bits as i32 - 8);
                let code = if luma {
                    16.0 * step + value * 219.0 * step
                } else {
                    128.0 * step + value * 224.0 * step
                };
                code.round().clamp(0.0, max)
            }
        }
    }
}

/// The CICP matrix coefficients a YCbCr triple was built with.
///
/// Only the sets this program can meet are named.  The two constant-luminance
/// variants are here because the enum a decoder hands over names them, and they
/// are *not* interchangeable with the non-constant-luminance ones: they carry
/// chroma as a ratio of luma, so reading one as the other gives wrong colour
/// wherever luma is not mid-scale.  Rather than approximate them, a decode that
/// meets one says so and is refused.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MatrixCoefficients {
    /// No transform: the three planes are already R, G and B.
    Identity,
    /// Rec. ITU-R BT.709's Kr/Kb, also used by sRGB and BT.601-derived content
    /// that declares itself as 709.
    Bt709,
    /// BT.2020 non-constant-luminance — the pair an HDR still is written with.
    Bt2020Ncl,
    /// BT.2020 constant-luminance, which this program refuses rather than
    /// approximates.
    Bt2020Cl,
}

impl MatrixCoefficients {
    /// The `Kr` and `Kb` weights the matrix is built from.
    ///
    /// `None` for the two sets that are not this family: identity has no
    /// weights, and constant luminance moves chroma into a different domain
    /// entirely.
    pub fn weights(self) -> Option<(f32, f32)> {
        match self {
            MatrixCoefficients::Identity | MatrixCoefficients::Bt2020Cl => None,
            MatrixCoefficients::Bt709 => Some((0.2126, 0.0722)),
            MatrixCoefficients::Bt2020Ncl => Some((0.2627, 0.0593)),
        }
    }

    /// A luma-and-chroma triple, normalized as [`ColorRange::normalize`] leaves
    /// it, back to RGB on `0..1`.
    ///
    /// `None` for the sets this program does not decode: the caller turns that
    /// into an error that names the matrix rather than showing the wrong
    /// colours.
    pub fn to_rgb(self, y: f32, cb: f32, cr: f32) -> Option<[f32; 3]> {
        let (kr, kb) = self.weights()?;
        let kg = 1.0 - kr - kb;
        Some([
            y + 2.0 * (1.0 - kr) * cr,
            y - 2.0 * kb * (1.0 - kb) / kg * cb - 2.0 * kr * (1.0 - kr) / kg * cr,
            y + 2.0 * (1.0 - kb) * cb,
        ])
    }

    /// RGB on `0..1` to the luma-and-chroma triple [`Self::to_rgb`] inverts,
    /// normalized the same way.
    pub fn from_rgb(self, rgb: [f32; 3]) -> Option<[f32; 3]> {
        let (kr, kb) = self.weights()?;
        let kg = 1.0 - kr - kb;
        let y = kr * rgb[0] + kg * rgb[1] + kb * rgb[2];
        Some([
            y,
            (rgb[2] - y) / (2.0 * (1.0 - kb)),
            (rgb[0] - y) / (2.0 * (1.0 - kr)),
        ])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The normalized triples the matrix is built for round-trip through it.
    #[test]
    fn the_matrix_and_its_inverse_agree() {
        for matrix in [MatrixCoefficients::Bt709, MatrixCoefficients::Bt2020Ncl] {
            for rgb in [
                [0.0, 0.0, 0.0],
                [1.0, 1.0, 1.0],
                [0.25, 0.5, 0.75],
                [1.0, 0.0, 0.0],
            ] {
                let ycc = matrix.from_rgb(rgb).expect("a matrix with weights");
                let back = matrix
                    .to_rgb(ycc[0], ycc[1], ycc[2])
                    .expect("a matrix with weights");
                for channel in 0..3 {
                    assert!(
                        (back[channel] - rgb[channel]).abs() < 1.0e-5,
                        "{matrix:?} {rgb:?} -> {ycc:?} -> {back:?}"
                    );
                }
            }
        }
    }

    /// A neutral is the one thing the matrix must not move: the rows sum to one,
    /// so a grey triple comes back grey with its chroma mid-scale.
    #[test]
    fn a_neutral_stays_neutral() {
        let ycc = MatrixCoefficients::Bt2020Ncl
            .from_rgb([0.5, 0.5, 0.5])
            .expect("weights");
        assert!((ycc[0] - 0.5).abs() < 1.0e-6, "{ycc:?}");
        assert!(ycc[1].abs() < 1.0e-6 && ycc[2].abs() < 1.0e-6, "{ycc:?}");
    }

    /// The sets that are not this family say so rather than answering with a
    /// plausible-looking wrong colour.
    #[test]
    fn the_matrices_this_program_does_not_decode_are_refused() {
        for matrix in [MatrixCoefficients::Identity, MatrixCoefficients::Bt2020Cl] {
            assert!(matrix.weights().is_none());
            assert!(matrix.to_rgb(0.5, 0.0, 0.0).is_none());
            assert!(matrix.from_rgb([0.5, 0.5, 0.5]).is_none());
        }
    }

    /// The two ranges differ by where black and white sit, which is the whole
    /// reason a decode has to know which one it has.
    #[test]
    fn the_ranges_place_black_and_white_where_they_belong() {
        // Ten bits, the depth an HDR still is written at.
        assert!((ColorRange::Full.normalize(0.0, 10, true) - 0.0).abs() < 1.0e-6);
        assert!((ColorRange::Full.normalize(1023.0, 10, true) - 1.0).abs() < 1.0e-6);
        // Limited range's black is code 64 and its white 940 at ten bits.
        assert!(ColorRange::Limited.normalize(64.0, 10, true).abs() < 1.0e-6);
        assert!((ColorRange::Limited.normalize(940.0, 10, true) - 1.0).abs() < 1.0e-6);
        // Chroma sits on -0.5..0.5 in both, so mid-scale is zero either way.
        assert!(ColorRange::Full.normalize(512.0, 10, false).abs() < 1.0e-3);
        assert!(ColorRange::Limited.normalize(512.0, 10, false).abs() < 1.0e-3);
    }

    #[test]
    fn a_code_survives_the_range_it_was_written_in() {
        for range in [ColorRange::Full, ColorRange::Limited] {
            for value in [0.0f32, 0.25, 0.5, 0.75, 1.0] {
                let code = range.code(value, 10, true);
                let back = range.normalize(code, 10, true);
                assert!(
                    (back - value).abs() < 2.0e-3,
                    "{range:?} {value} -> {code} -> {back}"
                );
            }
        }
    }

    /// A transfer curve and its inverse agree, and PQ's is anchored where the
    /// standard puts it: a code of 0.5 is a little over 92 cd/m².
    #[test]
    fn the_transfer_curves_round_trip_and_pq_is_absolute() {
        for transfer in [Transfer::Linear, Transfer::Srgb, Transfer::Pq] {
            for value in [0.0f32, 0.1, 0.5, 1.0] {
                let code = transfer
                    .to_code(value, REFERENCE_WHITE_NITS)
                    .expect("a curve with an inverse");
                let back = transfer.to_linear(code, REFERENCE_WHITE_NITS);
                assert!(
                    (back - value).abs() < 1.0e-3,
                    "{transfer:?} {value} -> {code} -> {back}"
                );
            }
        }
        // The frame's scale is relative to its reference white, so a code of 0.5
        // is a fixed absolute luminance: 92.2 cd/m², whatever the white is.
        let half = Transfer::Pq.to_linear(0.5, REFERENCE_WHITE_NITS) * REFERENCE_WHITE_NITS;
        assert!((half - 92.2).abs() < 1.0, "{half} cd/m² at PQ code 0.5");
        // And the reference white itself lands on the code the standard gives
        // it: 203 cd/m² is a little under 0.58.
        let white = Transfer::Pq
            .to_code(1.0, REFERENCE_WHITE_NITS)
            .expect("PQ has an inverse");
        assert!((white - 0.580).abs() < 0.01, "203 cd/m² at code {white}");
    }

    /// HLG decodes but does not encode: the missing half says so rather than
    /// answering with a per-channel approximation of it.
    #[test]
    fn hlg_has_no_encode_direction() {
        assert!(Transfer::Hlg.to_code(0.5, REFERENCE_WHITE_NITS).is_none());
        // The decode is there, and BT.2100's reference white is the property it
        // is built around: a 75 % signal lands on 203 cd/m² of a 1 000-nit
        // system.
        let light = Transfer::Hlg.to_linear_rgb([0.75, 0.75, 0.75], REFERENCE_WHITE_NITS);
        assert!((light[0] - 1.0).abs() < 0.02, "{light:?}");
    }
}

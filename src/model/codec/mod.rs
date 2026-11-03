// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Reading and writing the HDR half's file formats.
//!
//! Every format is one module behind one trait ([`HdrCodec`]), and every format
//! reads and writes one currency ([`HdrImage`]).  A capture's second file, a pin
//! read back off disk, and the private file the daemon hands its surface helper
//! are all the same value here, so "encode what was decoded" is a round trip
//! through one type rather than a conversion between four.
//!
//! The currency is deliberately format-agnostic: the transfer function has
//! already been resolved to linear light and the bit depth to `f32`, so nothing
//! downstream — the tone map, the compositor, the cropper — has to know which
//! format the pixels arrived in.  What that costs is the source's own codes: a
//! decode re-encodes rather than copying, so a round trip through a lossy format
//! pays that format's error once more.

// The trait is an interface, not a menu: every codec implements every half of
// it, and the registry offers what a *caller* needs — the codec for a name, the
// codec for a suffix, the codec for a file's own bytes.  A build that reaches
// for one of those and not the others is the ordinary case, not an omission, and
// the same rule already holds for `geometry`, `edit` and `model::frame`.
#![allow(dead_code)]

use std::path::Path;

// At least one format has to be there, on each side.  A build with none is not
// a smaller build, it is a broken one: `HdrFormat::default` has no codec to
// name and the first capture would panic rather than write anything, and the
// SDR half is the one file a capture is never without.  Saying so here turns
// that into a build error that names the features.
#[cfg(not(any(feature = "avif", feature = "radiance")))]
compile_error!(
    "vshot needs at least one HDR codec: enable the `avif` or `radiance` feature \
     (both are on by default), or build with the default features"
);

#[cfg(not(feature = "png"))]
compile_error!(
    "vshot needs the `png` codec: it is the SDR half of every capture, and the \
     feature is on by default"
);

use crate::error::{Result, VshotError};
use crate::geometry::Size;
use crate::model::hdr::{HdrFrame, Primaries, Transfer, REFERENCE_WHITE_NITS};

pub mod params;
pub mod radiance;

#[cfg(feature = "avif")]
pub mod avif;

#[cfg(feature = "png")]
pub mod png;

pub use params::{ParamKind, ParamSpec, ParamValue, ParamValues};

/// One HDR image as it travels between a codec and the rest of the program.
///
/// The frame is linear light in its own primaries, where `1.0` is the SDR white
/// the picture was made against; `reference_nits` is that white in cd/m².  The
/// two travel together because a codec needs both: an absolute encoding (PQ)
/// cannot be written without being told what the frame's `1.0` stands for, and a
/// decode is what produces that answer.
///
/// This is [`crate::output::HdrHalf`]'s shape, promoted out of the output path
/// so that reading a file and writing one meet on the same value.
#[derive(Clone, Debug, PartialEq)]
pub struct HdrImage {
    pub frame: HdrFrame,
    pub reference_nits: f32,
}

impl HdrImage {
    pub fn new(frame: HdrFrame, reference_nits: f32) -> Self {
        Self {
            frame,
            reference_nits,
        }
    }

    /// Decodes packed ten-bit codes of `transfer` into the currency, the way a
    /// codec's own decode does before it has a frame.
    pub fn from_rgb10(
        words: &[u32],
        size: Size,
        transfer: Transfer,
        primaries: Primaries,
        alpha: bool,
        reference_nits: f32,
    ) -> Result<Self> {
        Ok(Self {
            frame: HdrFrame::from_rgb10(words, size, transfer, primaries, alpha, reference_nits)?,
            reference_nits,
        })
    }

    /// The white this image's `1.0` stands for, falling back to BT.2408's
    /// reference when the source did not name one.
    pub fn white(&self) -> f32 {
        if self.reference_nits.is_finite() && self.reference_nits > 0.0 {
            self.reference_nits
        } else {
            REFERENCE_WHITE_NITS
        }
    }
}

/// One file format an HDR image can be written in and read back from.
///
/// `encode`/`decode` work on bytes and `encode_path`/`decode_path` on files;
/// the path forms have defaults built on the byte forms, so a codec that has
/// nothing special to do with a file implements only the pair.
pub trait HdrCodec: Sync {
    /// The suffix this format's files carry.
    fn extension(&self) -> &'static str;

    /// The name `--hdr-format` and the config file use for it.
    fn name(&self) -> &'static str;

    /// The parameters this format accepts, in the order a window should list
    /// them.  A format with nothing to tune — Radiance — declares none.
    fn specs(&self) -> &'static [ParamSpec] {
        &[]
    }

    /// Encodes `image` in this format.
    fn encode(&self, image: &HdrImage) -> Result<Vec<u8>>;

    /// The same, at the settings the caller asked for.
    ///
    /// The default ignores them and encodes at the format's own defaults, which
    /// is what a codec with no parameters wants and what a codec that has them
    /// overrides.  Having both means a caller that has no settings to pass —
    /// a test, or a path that never grew any — still has a way in.
    fn encode_with(&self, image: &HdrImage, values: &ParamValues) -> Result<Vec<u8>> {
        let _ = values;
        self.encode(image)
    }

    /// Decodes `bytes` back into the currency.
    ///
    /// `fallback_nits` is the reference white to read a file at when it names
    /// none of its own — a Radiance file from another writer, or an AVIF with
    /// no `vshot.refwhite01` box.  It is the `--hdr-reference-white` setting:
    /// without it the file would be read at BT.2408's 203 cd/m², which is the
    /// right guess for a broadcast file and the wrong one for a capture taken
    /// against a brighter or dimmer output.
    fn decode(&self, bytes: &[u8], fallback_nits: f32) -> Result<HdrImage>;

    fn encode_path(&self, image: &HdrImage, path: &Path) -> Result<()> {
        let bytes = self.encode(image)?;
        std::fs::write(path, bytes).map_err(|source| VshotError::WriteFile {
            path: path.to_path_buf(),
            source,
        })
    }

    fn decode_path(&self, path: &Path, fallback_nits: f32) -> Result<HdrImage> {
        let bytes = std::fs::read(path).map_err(|source| VshotError::HdrDecode {
            path: path.to_path_buf(),
            reason: format!("cannot read the image: {source}"),
        })?;
        self.decode(&bytes, fallback_nits)
    }
}

/// Which format a file is, from its bytes alone.
///
/// Used where a caller has a file and no idea what wrote it — a pin asked to
/// show whatever it was handed — so it must answer without a name to go on.
pub fn detect(bytes: &[u8]) -> Option<&'static dyn HdrCodec> {
    // Radiance names itself in its first line.
    if bytes.starts_with(b"#?RADIANCE") || bytes.starts_with(b"#?RGBE") {
        #[cfg(feature = "radiance")]
        return Some(&radiance::Radiance);
    }
    // AVIF is ISOBMFF: an `ftyp` box whose brand is one of the AVIF ones.
    if bytes.len() >= 12 && &bytes[4..8] == b"ftyp" && is_avif_brand(&bytes[8..12]) {
        #[cfg(feature = "avif")]
        return Some(&avif::Avif);
    }
    None
}

/// Which format a path's suffix names, for a caller that has a file name and
/// wants the codec for it rather than a guess from the bytes.
pub fn from_extension(path: &Path) -> Option<&'static dyn HdrCodec> {
    match path.extension()?.to_str()? {
        #[cfg(feature = "avif")]
        "avif" => Some(&avif::Avif),
        #[cfg(feature = "radiance")]
        "hdr" => Some(&radiance::Radiance),
        _ => None,
    }
}

/// The four-byte major brand of an AVIF file: `avif` for a still, `avis` for a
/// sequence.  Both are this program's own output, so both are accepted.
fn is_avif_brand(brand: &[u8]) -> bool {
    brand == b"avif" || brand == b"avis"
}

/// The formats this build can read and write, in the order the help lists them.
// The pushes are feature-gated, which a `vec![]` literal cannot express: an
// attribute on an element is not stable.  The clippy lint that wants the macro
// has no way to see that.
#[allow(clippy::vec_init_then_push)]
pub fn codecs() -> Vec<&'static dyn HdrCodec> {
    let mut found: Vec<&'static dyn HdrCodec> = Vec::new();
    #[cfg(feature = "avif")]
    found.push(&avif::Avif);
    #[cfg(feature = "radiance")]
    found.push(&radiance::Radiance);
    found
}

/// The codec for one of the names `--hdr-format` accepts.
pub fn by_name(name: &str) -> Option<&'static dyn HdrCodec> {
    codecs().into_iter().find(|codec| codec.name() == name)
}

/// The names this build accepts, for an error that has to say what it wanted.
pub fn names() -> Vec<&'static str> {
    codecs().into_iter().map(|codec| codec.name()).collect()
}

/// One file format the *SDR* half can be written in.
///
/// The HDR side's mirror image, and deliberately a separate trait rather than
/// one trait over both: the two halves carry different values (an `HdrFrame`
/// against a `Frame`), they answer different questions (`mime` matters here
/// because the clipboard is one of this half's destinations, and the HDR half
/// has no clipboard), and the HDR half's reference white has no counterpart on
/// this side at all.
pub trait SdrCodec: Sync {
    /// The suffix this format's files carry.
    fn extension(&self) -> &'static str;

    /// The name `--sdr-format` and the config file use for it.
    fn name(&self) -> &'static str;

    /// The type the clipboard carries this format under.
    fn mime(&self) -> &'static str;

    /// The parameters this format accepts, in the order a window should list
    /// them.
    fn specs(&self) -> &'static [ParamSpec];

    /// Encodes `frame` in this format, at the settings the caller asked for.
    ///
    /// `density` is the device density to stamp into the file, where the format
    /// has somewhere to put one; `None` leaves it out.
    fn encode(
        &self,
        frame: &crate::model::Frame,
        density: Option<u32>,
        values: &ParamValues,
    ) -> Result<Vec<u8>>;
}

/// The SDR formats this build can read and write, in the order the help lists
/// them.
#[allow(clippy::vec_init_then_push)] // the push is feature-gated; see `codecs`
pub fn sdr_codecs() -> Vec<&'static dyn SdrCodec> {
    let mut found: Vec<&'static dyn SdrCodec> = Vec::new();
    #[cfg(feature = "png")]
    found.push(&png::Png);
    found
}

/// The codec for one of the names `--sdr-format` accepts.
pub fn sdr_by_name(name: &str) -> Option<&'static dyn SdrCodec> {
    sdr_codecs().into_iter().find(|codec| codec.name() == name)
}

/// The SDR names this build accepts, for an error that has to say what it
/// wanted.
pub fn sdr_names() -> Vec<&'static str> {
    sdr_codecs().into_iter().map(|codec| codec.name()).collect()
}

/// The whole registry as JSON: what this build can write, and what each of
/// those formats will let a caller tune.
///
/// This is what `vshot formats --json` prints, and it is the only thing the
/// settings window knows about formats — which is the point of the registry.
/// Add a parameter to a codec and the window grows a row for it without the
/// window being touched, and a build without a codec never offers it.
///
/// The strings are built by hand, the way `vshot ocr --json` builds its own:
/// this is a handful of fixed keys, and the JSON it has to match is one this
/// program writes and reads back rather than a general-purpose document.
pub fn describe_json() -> String {
    let mut out = String::from("{\"sdr\":[");
    for (index, codec) in sdr_codecs().into_iter().enumerate() {
        if index > 0 {
            out.push(',');
        }
        out.push_str(&format!(
            "{{\"name\":{},\"extension\":{},\"params\":[",
            quote(codec.name()),
            quote(codec.extension())
        ));
        out.push_str(&params_json(codec.specs()));
        out.push_str("]}");
    }
    out.push_str("],\"hdr\":[");
    for (index, codec) in codecs().into_iter().enumerate() {
        if index > 0 {
            out.push(',');
        }
        out.push_str(&format!(
            "{{\"name\":{},\"extension\":{},\"params\":[",
            quote(codec.name()),
            quote(codec.extension())
        ));
        out.push_str(&params_json(codec.specs()));
        out.push_str("]}");
    }
    out.push_str("]}");
    out
}

fn params_json(specs: &[ParamSpec]) -> String {
    let mut out = String::new();
    for (index, spec) in specs.iter().enumerate() {
        if index > 0 {
            out.push(',');
        }
        out.push_str(&format!(
            "{{\"name\":{},\"label\":{},\"hint\":{},",
            quote(spec.name),
            quote(spec.label),
            quote(spec.hint)
        ));
        out.push_str(&match spec.kind {
            ParamKind::Choice { values, default } => format!(
                "\"kind\":\"choice\",\"values\":[{}],\"default\":{}",
                values
                    .iter()
                    .map(|value| quote(value))
                    .collect::<Vec<_>>()
                    .join(","),
                quote(default)
            ),
            ParamKind::Integer {
                min,
                max,
                step,
                default,
            } => format!(
                "\"kind\":\"integer\",\"min\":{min},\"max\":{max},\"step\":{step},\
                 \"default\":{default}"
            ),
            ParamKind::Number {
                min,
                max,
                step,
                decimals,
                default,
            } => format!(
                "\"kind\":\"number\",\"min\":{min},\"max\":{max},\"step\":{step},\
                 \"decimals\":{decimals},\"default\":{default}"
            ),
        });
        out.push('}');
    }
    out
}

fn value_json(value: &ParamValue) -> String {
    match value {
        ParamValue::Text(text) => quote(text),
        ParamValue::Integer(number) => number.to_string(),
        ParamValue::Number(number) => number.to_string(),
    }
}

/// A JSON string literal, escaped the way the format requires.  The values
/// here are codec names and labels rather than user input, but a label with a
/// quote in it would still be a document this program could not read back.
fn quote(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    out.push('"');
    for character in text.chars() {
        match character {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            character if (character as u32) < 0x20 => {
                out.push_str(&format!("\\u{:04x}", character as u32));
            }
            character => out.push(character),
        }
    }
    out.push('"');
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_built_codec_round_trips_through_its_own_name_and_extension() {
        for codec in codecs() {
            assert_eq!(by_name(codec.name()).map(|c| c.name()), Some(codec.name()));
            assert_eq!(
                from_extension(Path::new(&format!("x.{}", codec.extension()))).map(|c| c.name()),
                Some(codec.name())
            );
        }
    }

    #[test]
    fn a_radiance_file_is_recognized_by_its_magic() {
        let detection = detect(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n");
        #[cfg(feature = "radiance")]
        assert_eq!(detection.map(|codec| codec.name()), Some("hdr"));
        #[cfg(not(feature = "radiance"))]
        assert!(detection.is_none());
    }

    #[test]
    fn a_file_that_is_neither_is_not_claimed() {
        assert!(detect(b"\x89PNG\r\n\x1a\n").is_none());
        assert!(detect(b"VSHTPQ02").is_none());
        assert!(detect(b"").is_none());
    }

    #[test]
    fn the_names_are_the_ones_the_help_lists() {
        // The list is built from the enabled codecs, so this only pins that it
        // is non-empty and made of the names `parse` accepts.
        let names = names();
        assert!(!names.is_empty(), "a build with no codec cannot write HDR");
        for name in names {
            assert!(by_name(name).is_some());
        }
    }

    /// The registry's outward description is what the settings window builds
    /// its rows from, so it has to be JSON, and it has to name exactly the
    /// codecs this build has.
    #[test]
    fn the_description_is_json_covering_every_built_codec() {
        let described: serde_json::Value =
            serde_json::from_str(&describe_json()).expect("the registry describes itself as JSON");
        for (half, built) in [("sdr", sdr_names()), ("hdr", names())] {
            let listed: Vec<&str> = described[half]
                .as_array()
                .expect("both halves are lists")
                .iter()
                .map(|codec| codec["name"].as_str().expect("a name"))
                .collect();
            assert_eq!(listed, built, "the {half} half lists what was compiled in");
        }
    }

    /// Every declared parameter survives the trip out, with the range a
    /// control has to be bounded by.
    #[test]
    fn every_parameter_reaches_the_description() {
        let described: serde_json::Value = serde_json::from_str(&describe_json()).unwrap();
        for half in ["sdr", "hdr"] {
            for codec in described[half].as_array().unwrap() {
                let name = codec["name"].as_str().unwrap();
                let specs = if half == "sdr" {
                    sdr_by_name(name).unwrap().specs()
                } else {
                    by_name(name).unwrap().specs()
                };
                let params = codec["params"].as_array().unwrap();
                assert_eq!(params.len(), specs.len(), "{name}'s parameters");
                for (param, spec) in params.iter().zip(specs) {
                    assert_eq!(param["name"], spec.name);
                    assert!(
                        param["default"].is_string() || param["default"].is_number(),
                        "{name}.{} has a default",
                        spec.name
                    );
                }
            }
        }
    }
}

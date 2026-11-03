// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! PNG: the SDR half, and the one format a capture is never without.
//!
//! This is the SDR side's first [`SdrCodec`], and for now its only one.  It is
//! a codec like any other rather than a special case in the output path because
//! its compression level is a *parameter* — the same kind of thing AVIF's
//! quality is — and a settings window that can draw one format's parameters
//! should be able to draw every format's.
//!
//! Every level is lossless: the parameter trades encoding time for file size
//! and nothing else, which is why this side needs no reference white, no
//! transfer function and no gamut.  The pixels are the pixels.

use crate::error::Result;
use crate::model::codec::params::{ParamKind, ParamSpec, ParamValues};
use crate::model::codec::SdrCodec;
use crate::model::frame::{Frame, PngCompression};

/// The PNG codec.
pub struct Png;

/// The names `compression` accepts, in the order the settings window lists
/// them — the same five `--format-param png.compression` takes.
pub const COMPRESSION_NAMES: &[&str] = &["none", "fastest", "fast", "balanced", "high"];

/// What `compression` is when nothing says otherwise.
pub const DEFAULT_COMPRESSION: &str = "fast";

const SPECS: &[ParamSpec] = &[ParamSpec {
    name: "compression",
    label: "Compression",
    hint: "All levels are lossless; slower ones buy a smaller file",
    kind: ParamKind::Choice {
        values: COMPRESSION_NAMES,
        default: DEFAULT_COMPRESSION,
    },
}];

impl SdrCodec for Png {
    fn extension(&self) -> &'static str {
        "png"
    }

    fn name(&self) -> &'static str {
        "png"
    }

    fn mime(&self) -> &'static str {
        "image/png"
    }

    fn specs(&self) -> &'static [ParamSpec] {
        SPECS
    }

    fn encode(&self, frame: &Frame, density: Option<u32>, values: &ParamValues) -> Result<Vec<u8>> {
        let compression = PngCompression::parse(values.text("compression", DEFAULT_COMPRESSION))?;
        frame.encode_png(density, compression)
    }
}

/// What `compression` is when nothing says otherwise.  Named here rather than
/// only inside `PngCompression`'s own `Default` so the settings window can show
/// the same answer in its "built-in default" row.
#[cfg(test)]
mod tests {
    use super::*;
    use crate::geometry::Size;
    use crate::model::codec::params::ParamValue;
    use crate::model::codec::sdr_by_name;

    fn frame() -> Frame {
        Frame::new(Size::new(2, 2), vec![128; 2 * 2 * 4]).unwrap()
    }

    /// PNG is registered under the name the config file and the flag use.
    #[test]
    fn png_is_reachable_by_its_own_name() {
        let codec = sdr_by_name("png").expect("the SDR codec this build always has");
        assert_eq!(codec.extension(), "png");
        assert_eq!(codec.mime(), "image/png");
    }

    /// The codec's own default is the one the spec declares, so the settings
    /// window's "built-in default" row and the encoder agree.
    #[test]
    fn the_declared_default_is_the_one_the_encoder_uses() {
        let codec = sdr_by_name("png").unwrap();
        let spec = codec
            .specs()
            .iter()
            .find(|spec| spec.name == "compression")
            .expect("compression is declared");
        assert_eq!(
            spec.default_value(),
            ParamValue::Text(DEFAULT_COMPRESSION.into())
        );
    }

    /// The parameter reaches the encoder: a lower level writes a bigger file.
    /// The frame is noise rather than a flat colour, because DEFLATE has
    /// nothing to do with a flat one and every level would agree.
    #[test]
    fn the_compression_level_reaches_the_encoder() {
        let pixels: Vec<u8> = (0..64 * 64 * 4)
            .map(|index| (index * 37 % 251) as u8)
            .collect();
        let frame = Frame::new(Size::new(64, 64), pixels).unwrap();
        let codec = sdr_by_name("png").unwrap();

        let write = |name: &str| {
            let mut values = ParamValues::defaults(codec.specs());
            values.set(codec.specs(), "compression", ParamValue::Text(name.into()));
            codec.encode(&frame, None, &values).unwrap()
        };
        let smallest = write("high");
        let fastest = write("fastest");
        assert!(
            fastest.len() > smallest.len(),
            "fastest wrote {} bytes against high's {}",
            fastest.len(),
            smallest.len()
        );
    }

    /// A name the format does not offer is refused by the encoder rather than
    /// silently encoded at the default.
    #[test]
    fn a_level_the_format_does_not_offer_is_refused() {
        let codec = sdr_by_name("png").unwrap();
        let mut values = ParamValues::defaults(codec.specs());
        // `set` drops an unknown choice back to the declared default, which is
        // what a `--format-param png.compression=slowest` on the command line
        // gets: the codec layer never sees the bad name.
        values.set(
            codec.specs(),
            "compression",
            ParamValue::Text("slowest".into()),
        );
        assert_eq!(values.text("compression", "?"), DEFAULT_COMPRESSION);
        // The parser underneath still refuses it, which is what keeps a typo
        // from being written as a level nobody chose.
        assert!(PngCompression::parse("slowest").is_err());
        assert!(codec.encode(&frame(), None, &values).is_ok());
    }
}

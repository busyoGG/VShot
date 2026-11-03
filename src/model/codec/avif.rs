// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Reading and writing an HDR frame as a ten-bit AVIF.
//!
//! AVIF is the HDR half's default format because it is the one an HDR file can
//! carry *and* state: it is ten bits per channel, it can be BT.2020 with the PQ
//! transfer, and the `colr` box names all three of those in the CICP fields a
//! reader already understands.  Radiance RGBE, the other format here, holds the
//! light but has no colorimetry at all — its gamut is a non-standard header most
//! readers ignore (see [`crate::model::codec::radiance`]).
//!
//! The cost is that AVIF is lossy.  That is what the choice is for: AVIF is
//! small and universally readable, Radiance is exact and read by few.
//!
//! The pixels go through the standard path for HDR stills: the frame's PQ codes
//! in BT.2020, converted to the non-constant-luminance YCbCr of BT.2020 with the
//! matrix applied to the *PQ signal* (which is what CICP says a matrix
//! coefficient applies to), at full range and 4:4:4.  The gamut is not converted
//! — a BT.2020 AVIF is where a wide-gamut capture belongs.
//!
//! Reading one back is the same path in reverse, over the AV1 decoder and the
//! container: `dav1d` for the payload and `mp4parse` for the boxes.  Both are
//! behind this crate's `avif` feature, because they are most of the build time
//! the HDR path adds.  `image`'s own `avif-native` feature is the same two
//! crates plus its own YUV handling, but it hands back an RGB image and drops
//! the CICP triple — the one thing a decode here cannot do without, since the
//! triple is what says the samples are PQ and not sRGB.

use std::path::Path;

use dav1d::{
    BitsPerComponent, Decoder, Error as Dav1dError, Picture, PixelLayout, PlanarImageComponent,
};
use rav1e::prelude::{
    ChromaSamplePosition, ChromaSampling, ColorDescription, ColorPrimaries, Config, EncoderConfig,
    EncoderStatus, FrameType, MatrixCoefficients as Rav1eMatrix, PixelRange, SpeedSettings,
    TransferCharacteristics,
};

use crate::error::{Result, VshotError};
use crate::geometry::Size;
use crate::model::codec::params::{ParamKind, ParamSpec, ParamValues};
use crate::model::codec::{HdrCodec, HdrImage};
use crate::model::color::{ColorRange, MatrixCoefficients};
use crate::model::hdr::{HdrFrame, Primaries, Transfer, REFERENCE_WHITE_NITS};

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
pub const DEFAULT_SPEED: i64 = 9;

/// The base quantizer, on rav1e's 0-255 log scale: 66, which is the quality
/// `ravif` — the crate `image` uses for AVIF — calls 90 out of 100.
///
/// A screenshot is mostly flat colour and hard edges, which this holds well,
/// while an archival capture is not something to compress to the last byte: the
/// Radiance option exists for a caller who wants no loss at all.
///
/// Named as the *quality* the settings window shows, not as the quantizer it
/// becomes: the two are inversely related and non-linear, and a user has a
/// feeling for "90 out of 100" and none at all for "quantizer 66".
pub const DEFAULT_QUALITY: i64 = 90;

/// The parameters this format accepts, and the range each one lives in.
///
/// `quality` is the number `ravif` calls quality, not the quantizer: it is
/// mapped to rav1e's 0-255 log scale by [`quality_to_quantizer`], which is the
/// same curve `ravif` uses, so a value here means what it means in every other
/// AVIF encoder.
const SPECS: &[ParamSpec] = &[
    ParamSpec {
        name: "quality",
        label: "Quality",
        hint: "Higher keeps more of the picture and writes a bigger file; 90 is \
               about what other AVIF encoders call quality 90",
        kind: ParamKind::Integer {
            min: 1,
            max: 100,
            step: 1,
            default: DEFAULT_QUALITY,
        },
    },
    ParamSpec {
        name: "speed",
        label: "Speed",
        hint: "How hard the encoder works, 0 (slowest, smallest) to 10. This is \
               most of the time an HDR capture takes",
        kind: ParamKind::Integer {
            min: 0,
            max: 10,
            step: 1,
            default: DEFAULT_SPEED,
        },
    },
];

/// The depth both halves of the codec work at.
const BITS: u32 = 10;

/// The box this program writes its reference white in: a `uuid` box inside
/// `meta` whose payload is the light a code of 1.0 stands for, as a big-endian
/// `f32`.
///
/// AVIF has no field for it.  The nearest thing in the format is `mdcv`, whose
/// minimum-luminance field describes a *display* rather than the file's own
/// scale, and a reader that understood the box would take a value put there for
/// something it is not.  A `uuid` box is the extension point ISO 14496-12
/// provides for exactly this: a reader that does not know the UUID skips the box
/// and shows the file with BT.2408's reference white, which is the right answer
/// for everything that did not come from here.
const REFERENCE_WHITE_UUID: [u8; 16] = *b"vshot.refwhite01";

/// The AVIF codec.
pub struct Avif;

impl HdrCodec for Avif {
    fn extension(&self) -> &'static str {
        "avif"
    }

    fn name(&self) -> &'static str {
        "avif"
    }

    fn specs(&self) -> &'static [ParamSpec] {
        SPECS
    }

    fn encode(&self, image: &HdrImage) -> Result<Vec<u8>> {
        encode(&image.frame, image.white())
    }

    fn encode_with(&self, image: &HdrImage, values: &ParamValues) -> Result<Vec<u8>> {
        encode_at(
            &image.frame,
            image.white(),
            values.integer("quality", DEFAULT_QUALITY),
            values.integer("speed", DEFAULT_SPEED),
        )
    }

    fn decode(&self, bytes: &[u8], fallback_nits: f32) -> Result<HdrImage> {
        decode(bytes, fallback_nits, "<memory>")
    }

    fn decode_path(&self, path: &Path, fallback_nits: f32) -> Result<HdrImage> {
        // The path form names the file for the error; the byte form has only
        // `<memory>` to offer, and a pin that failed on a file should say which.
        let bytes = std::fs::read(path).map_err(|source| VshotError::HdrDecode {
            path: path.to_path_buf(),
            reason: format!("cannot read the image: {source}"),
        })?;
        decode(&bytes, fallback_nits, &path.display().to_string())
    }
}

/// Encodes `frame` as a ten-bit PQ / BT.2020 AVIF at this format's own defaults.
///
/// `reference_nits` is the light the frame's `1.0` stands for, the same value
/// [`HdrFrame::to_rgb10_pq`] takes: PQ codes are absolute, so the frame has to
/// say what its white is before it can be encoded.
pub fn encode(frame: &HdrFrame, reference_nits: f32) -> Result<Vec<u8>> {
    encode_at(frame, reference_nits, DEFAULT_QUALITY, DEFAULT_SPEED)
}

/// The same, at the quality and speed a caller asked for.
///
/// `quality` is on the 0-100 scale every other AVIF encoder uses, `speed` is
/// rav1e's own 0-10 preset.  Both are expected to be in range — [`SPECS`] is
/// what the config file and the settings window are bounded by — but they are
/// clamped here too, because this is also reachable from a caller that built
/// its values some other way.
pub fn encode_at(
    frame: &HdrFrame,
    reference_nits: f32,
    quality: i64,
    speed: i64,
) -> Result<Vec<u8>> {
    let width = frame.size().width as usize;
    let height = frame.size().height as usize;
    let planes = to_ycbcr(frame.to_rgb10_pq(reference_nits));
    let quantizer = quality_to_quantizer(quality.clamp(1, 100));

    let config = Config::new().with_encoder_config(EncoderConfig {
        width,
        height,
        bit_depth: BITS as usize,
        chroma_sampling: ChromaSampling::Cs444,
        chroma_sample_position: ChromaSamplePosition::Unknown,
        pixel_range: PixelRange::Full,
        // The CICP triple the `colr` box below repeats: a reader that looks at
        // the AV1 sequence header and one that looks at the container must agree,
        // or the image is shown with the wrong transfer function.
        color_description: Some(ColorDescription {
            color_primaries: ColorPrimaries::BT2020,
            transfer_characteristics: TransferCharacteristics::SMPTE2084,
            matrix_coefficients: Rav1eMatrix::BT2020NCL,
        }),
        still_picture: true,
        enable_timing_info: false,
        quantizer,
        min_quantizer: quantizer as u8,
        speed_settings: SpeedSettings::from_preset(speed.clamp(0, 10) as u8),
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
    Ok(with_reference_white(
        avif.to_vec(&av1, None, width as u32, height as u32, BITS as u8),
        white_or_reference(reference_nits),
    ))
}

/// The 0-100 quality every AVIF encoder speaks, as rav1e's 0-255 log quantizer.
///
/// The same piecewise curve `ravif` uses, so a quality set here means what it
/// means in `avifenc` or in `image`: the two are not proportional, and reading
/// a quality as a quantizer would put 90 at nearly-lossless instead of at the
/// setting everyone else calls 90.
fn quality_to_quantizer(quality: i64) -> usize {
    let q = quality as f64 / 100.0;
    let x = if q >= 0.82 {
        (1.0 - q) * 2.6
    } else if q > 0.25 {
        q.mul_add(-0.5, 1.0 - 0.125)
    } else {
        1.0 - q
    };
    (x * 255.0).round().clamp(0.0, 255.0) as usize
}

/// The white to write into the file, with the same fallback the rest of the
/// pipeline uses for a value that never arrived.
fn white_or_reference(reference_nits: f32) -> f32 {
    if reference_nits.is_finite() && reference_nits > 0.0 {
        reference_nits
    } else {
        REFERENCE_WHITE_NITS
    }
}

/// Reads a ten-bit AVIF back into the currency.
///
/// `origin` names the file in errors.
///
/// Only the shape this program writes is read: a still image item, 4:4:4, ten
/// or twelve bits, non-constant-luminance BT.709 or BT.2020, in either range.
/// That is deliberately narrower than AVIF, because the alternative to refusing
/// an unhandled shape is showing it with the wrong colour — an image whose CICP
/// triple says something this side cannot undo is not better displayed as a
/// guess.  The SDR half sits beside every HDR file this program writes, and it
/// is the one to look at when the HDR one is refused.
pub fn decode(bytes: &[u8], fallback_nits: f32, origin: &str) -> Result<HdrImage> {
    let mut input = std::io::Cursor::new(bytes);
    let context = mp4parse::read_avif(&mut input, mp4parse::ParseStrictness::Permissive)
        .map_err(|error| bad(origin, &format!("the container could not be read: {error}")))?;
    let payload = context
        .primary_item_coded_data()
        .ok_or_else(|| bad(origin, "there is no primary image in the file"))?;
    let decoded = decode_av1(payload, origin)?;

    let size = Size::new(decoded.width, decoded.height);
    if size
        .area()
        .map_err(|_| bad(origin, "the image size is out of range"))?
        != decoded.codes.len()
    {
        return Err(bad(origin, "the decoded planes disagree about the size"));
    }
    let matrix = match decoded.matrix {
        1 => MatrixCoefficients::Bt709,
        9 => MatrixCoefficients::Bt2020Ncl,
        other => {
            return Err(bad(
                origin,
                &format!(
                    "the image's chroma matrix (CICP {other}) is not one this program decodes"
                ),
            ))
        }
    };
    let primaries = match decoded.primaries {
        1 => Primaries::Bt709,
        9 => Primaries::Bt2020,
        12 => Primaries::DisplayP3,
        other => {
            return Err(bad(
                origin,
                &format!("the image's primaries (CICP {other}) are not one this program decodes"),
            ))
        }
    };
    let transfer = match decoded.transfer {
        8 => Transfer::Linear,
        13 => Transfer::Srgb,
        16 => Transfer::Pq,
        18 => Transfer::Hlg,
        other => {
            return Err(bad(
                origin,
                &format!(
                    "the image's transfer function (CICP {other}) is not one this program decodes"
                ),
            ))
        }
    };
    // PQ's codes are absolute, so the reference white does not change what the
    // file shows — it says where the frame's `1.0` sits, which is what the tone
    // map and the SDR half are built against.  AVIF has no field for it, so the
    // program writes its own box; a file from anywhere else falls back to the
    // setting, which is what a capture taken against an output of another white
    // needs to be read back at its own brightness.
    let reference_nits = reference_white(bytes)
        .unwrap_or_else(|| crate::model::hdr::clamp_reference_nits(fallback_nits));

    let mut pixels = vec![[0.0f32; 4]; decoded.codes.len()];
    for (destination, codes) in pixels.iter_mut().zip(decoded.codes) {
        // The range's codes to a normalized luma and chroma, the matrix to
        // R'G'B' on 0..1, and the transfer curve to linear light — the three
        // steps a codec's decode is, in the order they have to happen.
        let rgb = matrix
            .to_rgb(
                decoded.range.normalize(codes[0], decoded.bits, true),
                decoded.range.normalize(codes[1], decoded.bits, false),
                decoded.range.normalize(codes[2], decoded.bits, false),
            )
            .expect("a matrix with weights, checked above");
        let linear = transfer.to_linear_rgb(rgb, reference_nits);
        *destination = [linear[0], linear[1], linear[2], 1.0];
    }
    let frame = HdrFrame::in_primaries(size, pixels, primaries)?;
    Ok(HdrImage::new(frame, reference_nits))
}

/// What the AV1 decoder handed back, before the colour description has been
/// applied: the three planes' raw codes and the four CICP fields that say how to
/// read them.
struct Decoded {
    width: u32,
    height: u32,
    /// The bits the codes are in, which is what the range scales by.
    bits: u32,
    /// One `[y, cb, cr]` per pixel, row-major, as raw codes.
    codes: Vec<[f32; 3]>,
    range: ColorRange,
    matrix: u16,
    primaries: u16,
    transfer: u16,
}

/// Decodes the AV1 payload of a still image item.
///
/// The sequence header carries its own CICP triple, and it is the one to believe
/// over the container's: it is what the decoder itself read, and what its own
/// range handling was applied against.  The two agree in every file this program
/// writes, and where a foreign file disagrees the bitstream is the one a
/// standards-compliant player shows.
fn decode_av1(payload: &[u8], origin: &str) -> Result<Decoded> {
    let mut decoder = Decoder::new()
        .map_err(|error| bad(origin, &format!("the AV1 decoder could not start: {error}")))?;
    match decoder.send_data(payload.to_vec(), None, None, None) {
        Ok(()) | Err(Dav1dError::Again) => {}
        Err(error) => return Err(bad(origin, &format!("the AV1 data was refused: {error}"))),
    }
    // Pump until the frame comes out.  A still is one frame, but dav1d runs
    // frame threading and holds pictures back until the pipeline is pushed
    // along: the first call after the data answers `Again` and a later one
    // produces the picture.  `flush` is the wrong tool — it clears the decoder
    // state and takes the frame with it, which is how a decode of a file this
    // program had just written came back with "the payload holds no frame".
    let limit = decoder.get_frame_delay().unwrap_or(0).saturating_add(2);
    let mut picture = None;
    for _ in 0..limit {
        // Returns immediately when there is nothing pending, so calling it here
        // is what keeps a payload too large for dav1d's input buffer moving.
        match decoder.send_pending_data() {
            Ok(()) | Err(Dav1dError::Again) => {}
            Err(error) => return Err(bad(origin, &format!("the AV1 data was refused: {error}"))),
        }
        match decoder.get_picture() {
            Ok(found) => {
                picture = Some(found);
                break;
            }
            Err(Dav1dError::Again) => {}
            Err(error) => {
                return Err(bad(
                    origin,
                    &format!("the AV1 frame could not be decoded: {error}"),
                ))
            }
        }
    }
    let Some(picture) = picture else {
        return Err(bad(origin, "the AV1 payload holds no frame"));
    };
    read_picture(&picture, origin)
}

/// Copies the three planes out of a decoded picture.
fn read_picture(picture: &Picture, origin: &str) -> Result<Decoded> {
    if picture.pixel_layout() != PixelLayout::I444 {
        return Err(bad(
            origin,
            "the image is not 4:4:4, which is the only layout this program writes",
        ));
    }
    let Some(BitsPerComponent(bits)) = picture.bits_per_component() else {
        return Err(bad(
            origin,
            "the image's bit depth is not one dav1d reports",
        ));
    };
    let bits = bits as u32;
    // Samples wider than a byte are stored as sixteen-bit words; ten-bit AV1
    // still uses two bytes each.
    let sample_bytes = if bits > 8 { 2 } else { 1 };
    let width = picture.width() as usize;
    let height = picture.height() as usize;

    let mut planes: [Vec<f32>; 3] = [
        Vec::with_capacity(width * height),
        Vec::with_capacity(width * height),
        Vec::with_capacity(width * height),
    ];
    for (plane, component) in planes.iter_mut().zip([
        PlanarImageComponent::Y,
        PlanarImageComponent::U,
        PlanarImageComponent::V,
    ]) {
        // dav1d's stride is in bytes, not samples, and it is padded well past
        // the picture's width — 256 for a 16-pixel image — so a row is only the
        // first `width * sample_bytes` of it.
        let stride = picture.stride(component) as usize;
        let data = picture.plane(component);
        let data: &[u8] = data.as_ref();
        for row in 0..height {
            let start = row * stride;
            for column in 0..width {
                let at = start + column * sample_bytes;
                // dav1d hands high-bit-depth samples in the low bits of the
                // sixteen-bit word, not shifted up, so a ten-bit code comes out
                // as 0..1023 and is used as it stands.
                let code = if sample_bytes == 2 {
                    let Some(bytes) = data.get(at..at + 2) else {
                        return Err(bad(origin, "a plane is shorter than its geometry says"));
                    };
                    f32::from(u16::from_ne_bytes([bytes[0], bytes[1]]))
                } else {
                    let Some(&byte) = data.get(at) else {
                        return Err(bad(origin, "a plane is shorter than its geometry says"));
                    };
                    f32::from(byte)
                };
                plane.push(code);
            }
        }
    }
    let mut codes = vec![[0.0f32; 3]; width * height];
    for (index, triple) in codes.iter_mut().enumerate() {
        *triple = [planes[0][index], planes[1][index], planes[2][index]];
    }

    Ok(Decoded {
        width: picture.width(),
        height: picture.height(),
        bits,
        codes,
        range: match picture.color_range() {
            dav1d::pixel::YUVRange::Full => ColorRange::Full,
            dav1d::pixel::YUVRange::Limited => ColorRange::Limited,
        },
        matrix: picture.matrix_coefficients() as u16,
        primaries: picture.color_primaries() as u16,
        transfer: picture.transfer_characteristic() as u16,
    })
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
    let matrix = MatrixCoefficients::Bt2020Ncl;
    let range = ColorRange::Full;
    let code = |value: f32| -> u16 { range.code(value, BITS, false) as u16 };
    let luma = |value: f32| -> u16 { range.code(value, BITS, true) as u16 };
    rgb10
        .into_iter()
        .map(|word| {
            let rgb = [
                ((word >> 20) & 0x3ff) as f32 / 1023.0,
                ((word >> 10) & 0x3ff) as f32 / 1023.0,
                (word & 0x3ff) as f32 / 1023.0,
            ];
            let [y, cb, cr] = matrix.from_rgb(rgb).expect("a matrix with weights");
            [luma(y), code(cb), code(cr)]
        })
        .collect()
}

/// The `[start, size]` of a top-level ISOBMFF box, walking the file's own sizes
/// rather than assuming where anything sits.
fn top_level_box(bytes: &[u8], name: &[u8; 4]) -> Option<(usize, usize)> {
    let mut at = 0usize;
    while at + 8 <= bytes.len() {
        let size = u32::from_be_bytes(bytes[at..at + 4].try_into().ok()?) as usize;
        if size < 8 || at + size > bytes.len() {
            return None;
        }
        if &bytes[at + 4..at + 8] == name {
            return Some((at, size));
        }
        at += size;
    }
    None
}

/// Puts the reference white into the file as a `uuid` box at the end of the
/// media data, growing `mdat`'s own size to cover it.
///
/// `meta` is where ISOBMFF puts per-image metadata and where this box would
/// naturally go, but it cannot: `iloc` names the AV1 payload by *absolute file
/// offset*, so a box inserted inside `meta` shifts the payload by its own size
/// and every offset in the file becomes wrong — the reader then fails with
/// `IlocNotFound` on a file it wrote itself.  Growing `mdat` instead leaves
/// every offset that matters exactly where it was: the payload still starts
/// where `iloc` says, the item still ends where its length says, and the box
/// sits in the padding after it.  A reader that does not know the UUID never
/// looks inside the media data at all.
fn with_reference_white(mut avif: Vec<u8>, reference_nits: f32) -> Vec<u8> {
    let Some((at, size)) = top_level_box(&avif, b"mdat") else {
        return avif;
    };
    // Only the 32-bit size form is grown; a file large enough for the 64-bit one
    // is not something this writes, and rewriting the wrong field would corrupt
    // it rather than lose a header.
    if at + size != avif.len()
        || u32::from_be_bytes(avif[at..at + 4].try_into().unwrap()) != size as u32
    {
        return avif;
    }
    let mut box_bytes = Vec::with_capacity(28);
    box_bytes.extend_from_slice(&28u32.to_be_bytes());
    box_bytes.extend_from_slice(b"uuid");
    box_bytes.extend_from_slice(&REFERENCE_WHITE_UUID);
    box_bytes.extend_from_slice(&reference_nits.to_be_bytes());
    avif.extend_from_slice(&box_bytes);
    avif[at..at + 4].copy_from_slice(&((size + box_bytes.len()) as u32).to_be_bytes());
    avif
}

/// The reference white this program wrote into `bytes`, if it wrote one.
fn reference_white(bytes: &[u8]) -> Option<f32> {
    let (at, size) = top_level_box(bytes, b"mdat")?;
    // The payload, past `mdat`'s own header.
    let mdat = bytes.get(at + 8..at + size)?;
    let magic = mdat
        .windows(REFERENCE_WHITE_UUID.len())
        .position(|window| window == REFERENCE_WHITE_UUID)?;
    let value = mdat.get(magic + 16..magic + 20)?;
    let found = f32::from_be_bytes(value.try_into().ok()?);
    (found.is_finite() && found > 0.0).then_some(found)
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
    use crate::model::codec::params::ParamValue;
    use crate::model::hdr::REFERENCE_WHITE_NITS;

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

    /// A 16x16 PQ ramp — the smallest rav1e accepts — as the words a capture
    /// hands over.
    fn ramp() -> Vec<u32> {
        (0..256)
            .map(|index| {
                let value = (index % 16) * 64;
                (3 << 30) | (value << 20) | (value << 10) | value
            })
            .collect()
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
        let bytes = encode(&frame(&ramp(), 16, 16), REFERENCE_WHITE_NITS).unwrap();
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

    /// The reference white rides in the file and comes back out, which is what
    /// lets a pin show the same light the capture did.
    #[test]
    fn the_reference_white_is_written_and_read_back() {
        for white in [100.0f32, 203.0, 350.0] {
            let bytes = encode(&frame(&ramp(), 16, 16), white).unwrap();
            let found = reference_white(&bytes).expect("the box this program wrote");
            assert!((found - white).abs() < 0.01, "{white} came back as {found}");
        }
    }

    /// A file from anywhere else has no such box, and reads as BT.2408's
    /// reference rather than as a guess.
    #[test]
    fn a_file_without_the_box_has_no_reference_white() {
        assert!(reference_white(b"not an isobmff file").is_none());
        assert!(reference_white(&[]).is_none());
        // The file this program writes *with* the box still parses as AVIF:
        // an unknown UUID is skipped rather than refused.
        let bytes = encode(&frame(&ramp(), 16, 16), 203.0).unwrap();
        let mut cursor = std::io::Cursor::new(&bytes);
        assert!(
            mp4parse::read_avif(&mut cursor, mp4parse::ParseStrictness::Permissive).is_ok(),
            "the uuid box made the container unreadable"
        );
    }

    /// The whole round trip: what the encoder wrote is what the decoder reads,
    /// to within the quantizer's loss.  This is the check that the two halves of
    /// the codec — including the chroma matrix, the range and the ten-bit
    /// unpacking — are inverses of each other.
    #[test]
    fn an_avif_round_trips_its_light() {
        let source = frame(&ramp(), 16, 16);
        let bytes = encode(&source, REFERENCE_WHITE_NITS).unwrap();
        let back = Avif
            .decode(&bytes, REFERENCE_WHITE_NITS)
            .expect("the file this program wrote");
        assert_eq!(back.frame.size(), source.size());
        assert_eq!(back.frame.primaries(), Primaries::Bt2020);
        assert!((back.reference_nits - REFERENCE_WHITE_NITS).abs() < 0.01);
        // PQ is a steep curve at the bottom and the ramp's first steps are close
        // together in light, so the comparison is against the source's own
        // values with the quantizer's slack rather than an absolute tolerance.
        for (index, (before, after)) in source.pixels().iter().zip(back.frame.pixels()).enumerate()
        {
            for channel in 0..3 {
                let delta = (before[channel] - after[channel]).abs();
                assert!(
                    delta <= before[channel] * 0.15 + 0.02,
                    "pixel {index} channel {channel}: {before:?} vs {after:?}"
                );
            }
        }
    }

    /// A file that is not an AVIF is refused rather than read as something else.
    #[test]
    fn a_file_that_is_not_avif_is_refused() {
        assert!(Avif
            .decode(b"\x89PNG\r\n\x1a\n", REFERENCE_WHITE_NITS)
            .is_err());
        assert!(Avif.decode(b"", REFERENCE_WHITE_NITS).is_err());
        // Truncated after the container: the payload cannot be decoded.
        let bytes = encode(&frame(&ramp(), 16, 16), REFERENCE_WHITE_NITS).unwrap();
        assert!(Avif
            .decode(&bytes[..bytes.len() / 2], REFERENCE_WHITE_NITS)
            .is_err());
    }

    /// The declared parameters are the ones the encoder reads, so a value the
    /// settings window shows is a value that changes the file.
    #[test]
    fn the_declared_defaults_are_the_ones_the_encoder_uses() {
        let specs = Avif.specs();
        let quality = specs.iter().find(|spec| spec.name == "quality").unwrap();
        let speed = specs.iter().find(|spec| spec.name == "speed").unwrap();
        assert_eq!(
            quality.default_value(),
            ParamValue::Integer(DEFAULT_QUALITY)
        );
        assert_eq!(speed.default_value(), ParamValue::Integer(DEFAULT_SPEED));
        // The encode that takes no settings is the one the defaults describe.
        let image = HdrImage::new(frame(&ramp(), 16, 16), REFERENCE_WHITE_NITS);
        assert_eq!(
            Avif.encode(&image).unwrap(),
            Avif.encode_with(&image, &ParamValues::defaults(specs))
                .unwrap()
        );
    }

    /// Quality reaches rav1e's quantizer, on the curve every other AVIF encoder
    /// uses: a higher quality is a lower quantizer, and quality 90 is the 66
    /// that `ravif` calls 90.
    #[test]
    fn quality_maps_onto_the_quantizer_the_way_ravif_maps_it() {
        assert_eq!(quality_to_quantizer(100), 0);
        assert_eq!(quality_to_quantizer(90), 66, "what ravif calls quality 90");
        // Monotone: more quality is never a coarser quantizer.
        let mut previous = quality_to_quantizer(1);
        for quality in 2..=100 {
            let quantizer = quality_to_quantizer(quality);
            assert!(quantizer <= previous, "quality {quality} went backwards");
            previous = quantizer;
        }
        // The ends of the scale are the ones the curve is meant to reach.
        assert!(
            quality_to_quantizer(1) > 200,
            "quality 1 is the very lossy end"
        );
    }

    /// The parameter really is what changes the file: a low quality writes a
    /// smaller one than a high quality, on content with something to lose.
    #[test]
    fn a_lower_quality_writes_a_smaller_file() {
        // A gradient rather than the flat ramp: AVIF has nothing to quantize
        // away in a picture that is already uniform.
        let words: Vec<u32> = (0..64 * 64)
            .map(|index| {
                let x = (index % 64) * 4;
                let y = (index / 64) * 4;
                (3 << 30) | (x << 20) | (y << 10) | ((x + y) / 2)
            })
            .collect();
        let image = HdrImage::new(frame(&words, 64, 64), REFERENCE_WHITE_NITS);
        let specs = Avif.specs();
        let at = |quality: i64| {
            let mut values = ParamValues::defaults(specs);
            values.set(specs, "quality", ParamValue::Integer(quality));
            Avif.encode_with(&image, &values).unwrap()
        };
        let low = at(20);
        let high = at(95);
        assert!(
            low.len() < high.len(),
            "quality 20 wrote {} bytes against quality 95's {}",
            low.len(),
            high.len()
        );
    }
}

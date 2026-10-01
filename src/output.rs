// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

use std::fs::File;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use chrono::{DateTime, Local};

use crate::cli::Destination;
use crate::error::{Result, VshotError};
use crate::geometry::Point;
use crate::model::{Frame, HdrFrame, PngCompression};

/// How the HDR half of a capture is written.
///
/// Both options hold the light; they differ in whether a reader can be told
/// what the light *means*.  AVIF carries its own colour description, so a
/// BT.2020 PQ image is stated as one and any reader that understands AVIF
/// shows it correctly — at the cost of being lossy.  Radiance RGBE is exact,
/// but its gamut lives in a `PRIMARIES=` header that the readers that matter
/// here (ffmpeg, ImageMagick) ignore, so a wide-gamut capture looks
/// over-saturated in them.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum HdrFormat {
    /// Ten-bit AVIF, BT.2020 with the PQ transfer, declared in the file's CICP
    /// fields.  The default: small, lossy, and readable everywhere.
    #[default]
    Avif,
    /// Radiance RGBE, the pixels as captured in the output's own primaries.
    /// Exact and archival; the gamut is a non-standard header.
    Radiance,
}

impl HdrFormat {
    /// Parses the `--hdr-format` value.  The names are the file extensions, so
    /// a value copied out of `vshot --help` names the file it produces.
    pub fn parse(value: &str) -> Result<Self> {
        match value {
            "avif" => Ok(Self::Avif),
            "hdr" => Ok(Self::Radiance),
            other => Err(VshotError::InvalidDestination(format!(
                "`--hdr-format {other}` is not one of avif, hdr"
            ))),
        }
    }

    /// The suffix the HDR half's file carries.
    pub const fn extension(self) -> &'static str {
        match self {
            Self::Avif => "avif",
            Self::Radiance => "hdr",
        }
    }

    /// Encodes the HDR half in this format.
    ///
    /// `reference_nits` is the light the frame's `1.0` stands for.  Radiance
    /// holds linear light and needs nothing more; AVIF is PQ, whose codes are
    /// absolute, so it has to be told what the frame's white is.
    pub fn encode(self, frame: &HdrFrame, reference_nits: f32) -> Result<Vec<u8>> {
        match self {
            Self::Avif => crate::model::avif::encode(frame, reference_nits),
            Self::Radiance => Ok(frame.encode_radiance()),
        }
    }
}

/// One capture's HDR half, with the light its `1.0` stands for: the output's own
/// SDR white.  Kept together because both consumers need both — the second file
/// is encoded from the frame, while a pinned surface takes it re-encoded at that
/// white, which is what turns the frame back into the absolute codes the panel
/// shows.
#[derive(Clone, Debug)]
pub struct HdrHalf {
    pub frame: HdrFrame,
    pub reference_nits: f32,
}

/// Writes a captured frame to `destination`. `density` is the frame's device
/// pixels per logical pixel (the scale of the output it came from); every PNG
/// written to a file, stdout or the clipboard carries it as its physical
/// resolution, and the pin destination states it in the request, so the
/// on-screen size and the sharpness of the capture survive wherever the image
/// goes next. `compression` is the PNG encoder's level, which costs time and
/// buys size; it does not apply to the pin destination, whose PNG only travels
/// to the daemon through a temp file.
///
/// `hdr` is the capture's HDR half when the source carried HDR content.  The
/// SDR half is the ordinary PNG at `destination`; the HDR half is written to
/// the same path with its extension replaced by `format`'s — `shot.png` beside
/// `shot.avif` — so the two images of one capture sit together and differ only
/// in their suffix.  The first is the SDR view of the content, the second the
/// HDR content itself.  Only a file destination names that second path: stdout
/// and the clipboard carry one image, and the pin daemon carries the HDR half
/// out of band, as the PQ codes a surface of its own reads (see the `Pin` arm
/// below).
pub fn write_frame_with_hdr(
    frame: &Frame,
    hdr: Option<&HdrHalf>,
    destination: &Destination,
    density: u32,
    compression: PngCompression,
    format: HdrFormat,
    // Where the capture came from on the desktop, in global logical pixels,
    // when it came from a place at all.  Only the pin destination uses it: the
    // daemon puts the pin back exactly there instead of in the middle of an
    // output.  Every other destination has no position to keep, and a
    // composed or synthetic image never had one.
    pin_origin: Option<Point>,
    // The marks the editing session left on the image, when it left any.  Only
    // the pin destination uses them: they are what lets the pin be reopened for
    // editing on the user's own marks rather than on the pixels they were
    // flattened into.  `None` for a capture nothing was drawn on, which is
    // every route that never opened an editor.
    pin_marks: Option<&serde_json::Value>,
    // The capture `frame` was flattened from, before any mark was drawn on it,
    // sent beside `pin_marks` so the daemon can keep the picture the marks
    // belong to.  `None` whenever `pin_marks` is.
    pin_base: Option<&[u8]>,
) -> Result<()> {
    match destination {
        Destination::File(path) => {
            let path = expand_output_path(path, Local::now());
            create_parent_directories(&path)?;
            write_capture_files(&path, frame, hdr, density, compression, format)?;
            copy_file_to_clipboard(&path)
        }
        Destination::Stdout => io::stdout()
            .write_all(&frame.encode_png(Some(density), compression)?)
            .map_err(|source| VshotError::WriteFile {
                path: "stdout".into(),
                source,
            }),
        Destination::Clipboard => copy_to_clipboard(&frame.encode_png(Some(density), compression)?),
        // The daemon is told the density outright, so the bytes it loads need
        // no declaration of their own, and they only have to survive the trip
        // through the temp file: the default (fastest useful) level is right.
        //
        // An HDR pinch travels in two files: the SDR PNG, which the daemon loads
        // as it always did -- for the pin's size, its fallback and whatever
        // edits it holds -- and the PQ codes, which the daemon hands to a
        // surface of its own so the image reaches the panel as the light it
        // stands for rather than as the SDR view of it.
        Destination::Pin => {
            // The codes are written in the capture's own primaries, which are
            // the output's: the surface that shows them carries that same
            // description, so what it declares and what the words hold agree.
            let pq = hdr.map(|half| crate::pin::PqPin {
                words: half
                    .frame
                    .to_rgb10_pq_in(half.frame.primaries(), half.reference_nits),
                width: half.frame.size().width,
                height: half.frame.size().height,
                reference_nits: half.reference_nits,
                primaries: half.frame.primaries(),
            });
            crate::pin::pin_png(
                &frame.to_png()?,
                density,
                pin_origin,
                pq.as_ref(),
                pin_marks,
                pin_base,
                // The editor's own handoff, and only that: a pin carrying the
                // marks of the session that just drew them.  Every other route
                // here has nobody drawing the capture, so there is no gap to
                // close and no reason to make the daemon wait for a frame.
                pin_marks.is_some(),
            )
        }
    }
}

/// Writes the SDR PNG and, when there is one, the HDR image beside it.  Split
/// out so the offline test can exercise the pairing without `wl-copy`.
fn write_capture_files(
    path: &Path,
    frame: &Frame,
    hdr: Option<&HdrHalf>,
    density: u32,
    compression: PngCompression,
    format: HdrFormat,
) -> Result<()> {
    // The SDR half is the destination the user named and the HDR half sits
    // beside it.  When the destination already ends in the HDR format's own
    // suffix the two names would be the same file, so the named path is taken
    // for the HDR half and the PNG goes to its sibling — the pair still differs
    // only in its suffix, and neither image overwrites the other.
    let (png_path, hdr_path) = match hdr {
        Some(_) if hdr_sibling_path(path, format) == path => {
            (png_sibling_path(path), path.to_path_buf())
        }
        _ => (path.to_path_buf(), hdr_sibling_path(path, format)),
    };
    write_file(&png_path, &frame.encode_png(Some(density), compression)?)?;
    if let Some(hdr) = hdr {
        write_file(&hdr_path, &format.encode(&hdr.frame, hdr.reference_nits)?)?;
    }
    Ok(())
}

/// The HDR half's path: the same file name with the extension replaced, so the
/// two images of one capture sit together and differ only in their suffix.
fn hdr_sibling_path(path: &Path, format: HdrFormat) -> PathBuf {
    let mut sibling = path.to_path_buf();
    sibling.set_extension(format.extension());
    sibling
}

/// The SDR half's path, for a destination that named the HDR one outright.
fn png_sibling_path(path: &Path) -> PathBuf {
    let mut sibling = path.to_path_buf();
    sibling.set_extension("png");
    sibling
}

fn expand_output_path(path: &Path, now: DateTime<Local>) -> PathBuf {
    let pattern = path.to_string_lossy();
    PathBuf::from(now.format(&pattern).to_string())
}

/// Makes the directory an `--output` path names, so a pattern can place its file
/// in a directory that only comes into being as the pattern expands.
///
/// A strftime pattern is a path *recipe*, not a path: `Screenshots/%Y%m/shot.png`
/// names a different directory every month, and the user cannot pre-create next
/// month's.  So the directory is made here rather than left to `File::create`,
/// whose "No such file or directory" says nothing about which part was missing.
/// This is the rule the recording paths already follow for vshot's own videos
/// directory, applied to every `-o` path.
pub fn create_parent_directories(path: &Path) -> Result<()> {
    // A bare file name has no directory to make; an empty parent (`/shot.png`)
    // is the root and already exists.
    let Some(directory) = path.parent().filter(|dir| !dir.as_os_str().is_empty()) else {
        return Ok(());
    };
    if directory.is_dir() {
        return Ok(());
    }
    std::fs::create_dir_all(directory).map_err(|source| VshotError::CreateOutputDirectory {
        path: directory.to_path_buf(),
        source,
    })
}

fn write_file(path: &Path, bytes: &[u8]) -> Result<()> {
    let mut file = File::create(path).map_err(|source| VshotError::WriteFile {
        path: path.to_path_buf(),
        source,
    })?;
    file.write_all(bytes)
        .map_err(|source| VshotError::WriteFile {
            path: path.to_path_buf(),
            source,
        })?;
    file.flush().map_err(|source| VshotError::WriteFile {
        path: path.to_path_buf(),
        source,
    })
}

fn copy_to_clipboard(bytes: &[u8]) -> Result<()> {
    copy_bytes_to_clipboard(bytes, "image/png")
}

/// Puts recognized text on the clipboard.  `text/plain` is what a paste into
/// an editor asks for; `wl-copy` keeps serving it until the next copy.
pub fn copy_text_to_clipboard(text: &str) -> Result<()> {
    copy_bytes_to_clipboard(text.as_bytes(), "text/plain")
}

/// Puts a PNG that is already encoded on the clipboard, for a caller whose
/// result arrived as bytes rather than as a `Frame` (the translate overlay's
/// composited image, which the Qt helper wrote to a file).
pub fn copy_png_to_clipboard(bytes: &[u8]) -> Result<()> {
    copy_to_clipboard(bytes)
}

/// Expands a `--output` path's strftime pattern with the current time, the way
/// `write_frame` does.  It exists for the translate overlay, which has to hand
/// the helper an absolute path to write to before any `Frame` exists.
pub fn expanded_output_path(path: &Path) -> PathBuf {
    expand_output_path(path, Local::now())
}

fn copy_file_to_clipboard(path: &Path) -> Result<()> {
    let absolute = path.canonicalize().map_err(|source| {
        VshotError::Clipboard(format!(
            "failed to resolve output file {} for the clipboard: {source}",
            path.display()
        ))
    })?;
    let uri = format!("{}\n", file_uri(&absolute));
    copy_bytes_to_clipboard(uri.as_bytes(), "text/uri-list")
}

fn copy_bytes_to_clipboard(bytes: &[u8], mime_type: &str) -> Result<()> {
    let mut child = Command::new("wl-copy")
        .arg("--type")
        .arg(mime_type)
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|source| VshotError::CommandIo {
            program: "wl-copy".into(),
            source,
        })?;
    let mut stdin = child
        .stdin
        .take()
        .ok_or_else(|| VshotError::Clipboard("wl-copy stdin was not available".into()))?;
    stdin.write_all(bytes).map_err(|source| {
        VshotError::Clipboard(format!("failed to send {mime_type} to wl-copy: {source}"))
    })?;
    drop(stdin);
    let status = child.wait().map_err(|source| VshotError::CommandIo {
        program: "wl-copy".into(),
        source,
    })?;
    if !status.success() {
        return Err(VshotError::Clipboard(format!(
            "wl-copy exited with {status}"
        )));
    }
    Ok(())
}

fn file_uri(path: &Path) -> String {
    let mut uri = String::from("file://");
    for &byte in path.to_string_lossy().as_bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'.' | b'_' | b'~' | b'/') {
            uri.push(byte as char);
        } else {
            uri.push_str(&format!("%{byte:02X}"));
        }
    }
    uri
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::geometry::Size;

    #[test]
    fn frame_png_is_nonempty() {
        let frame = Frame::solid(Size::new(1, 1), [1, 2, 3, 255]).unwrap();
        let mut path = std::env::temp_dir();
        path.push(format!(
            "vshot-output-test-{}-{}.png",
            std::process::id(),
            unique_suffix()
        ));
        write_file(&path, &frame.to_png().unwrap()).unwrap();
        let bytes = std::fs::read(&path).unwrap();
        let _ = std::fs::remove_file(path);
        assert!(bytes.starts_with(b"\x89PNG\r\n\x1a\n"));
    }

    #[test]
    fn output_path_keeps_prefix_and_suffix_around_time_format() {
        let now = Local::now();
        let path = expand_output_path(Path::new("shots/vshot-%Y%m%d-%H%M%S.final.png"), now);
        let expected_date = now.format("%Y%m%d-%H%M%S").to_string();
        assert_eq!(
            path,
            PathBuf::from(format!("shots/vshot-{expected_date}.final.png"))
        );
    }

    /// The directory a strftime pattern names comes into being with the pattern.
    ///
    /// `Screenshots/%Y%m/shot.png` names a different directory every month, and
    /// the user cannot pre-create next month's — so the write has to make it.
    /// Before this, such a pattern failed with a bare "No such file or
    /// directory" that named neither the missing part nor the way out.
    #[test]
    fn a_pattern_whose_directory_does_not_exist_yet_is_still_writable() {
        let mut root = std::env::temp_dir();
        root.push(format!(
            "vshot-output-dir-test-{}-{}",
            std::process::id(),
            unique_suffix()
        ));
        // Two levels that do not exist, the shape `Screenshots/%Y%m/` has.
        let path = expand_output_path(&root.join("%Y%m/shot.png"), Local::now());
        assert!(
            !path.parent().unwrap().exists(),
            "the fixture starts absent"
        );

        create_parent_directories(&path).unwrap();
        assert!(
            path.parent().unwrap().is_dir(),
            "the month directory is made"
        );

        let frame = Frame::solid(Size::new(1, 1), [1, 2, 3, 255]).unwrap();
        write_file(&path, &frame.to_png().unwrap()).unwrap();
        assert!(std::fs::read(&path).unwrap().starts_with(b"\x89PNG"));

        // A second write into the same month is not an error: an existing
        // directory is left alone rather than refused.
        create_parent_directories(&path).unwrap();
        let _ = std::fs::remove_dir_all(&root);
    }

    /// A bare file name has no directory to make, and the root is not made
    /// either — it is already there.
    #[test]
    fn a_path_with_no_directory_is_left_alone() {
        create_parent_directories(Path::new("shot.png")).unwrap();
        create_parent_directories(Path::new("/shot.png")).unwrap();
    }

    #[test]
    fn output_path_preserves_escaped_percent() {
        let now = Local::now();
        let path = expand_output_path(Path::new("vshot-%%-%Y.png"), now);
        assert!(path.to_string_lossy().starts_with("vshot-%-"));
        assert!(path.to_string_lossy().ends_with(".png"));
    }

    #[test]
    fn the_hdr_format_parses_the_names_the_help_lists() {
        assert_eq!(HdrFormat::parse("avif").unwrap(), HdrFormat::Avif);
        assert_eq!(HdrFormat::parse("hdr").unwrap(), HdrFormat::Radiance);
        // A typo is refused rather than silently falling back to a format the
        // user did not ask for, and the message names the flag.
        let error = HdrFormat::parse("avifs").unwrap_err().to_string();
        assert!(error.contains("--hdr-format"), "{error}");
        // The default is the one the setting documents.
        assert_eq!(HdrFormat::default(), HdrFormat::Avif);
        assert_eq!(HdrFormat::Avif.extension(), "avif");
        assert_eq!(HdrFormat::Radiance.extension(), "hdr");
    }

    #[test]
    fn the_hdr_sibling_only_changes_the_suffix() {
        assert_eq!(
            hdr_sibling_path(Path::new("shots/vshot-2026.png"), HdrFormat::Radiance),
            PathBuf::from("shots/vshot-2026.hdr")
        );
        // The AVIF default names its own suffix, and the rest of the name is
        // untouched either way.
        assert_eq!(
            hdr_sibling_path(Path::new("shots/vshot-2026.png"), HdrFormat::Avif),
            PathBuf::from("shots/vshot-2026.avif")
        );
        // A path with more than one dot keeps everything but the last part.
        assert_eq!(
            hdr_sibling_path(Path::new("a.final.png"), HdrFormat::Radiance),
            PathBuf::from("a.final.hdr")
        );
        // No extension at all: the HDR half still gets one.
        assert_eq!(
            hdr_sibling_path(Path::new("shot"), HdrFormat::Avif),
            PathBuf::from("shot.avif")
        );
    }

    #[test]
    fn an_hdr_capture_writes_both_files_with_the_same_stem() {
        let frame = Frame::solid(Size::new(2, 2), [10, 20, 30, 255]).unwrap();
        let hdr = HdrHalf {
            frame: HdrFrame::new(Size::new(2, 2), vec![[4.0, 2.0, 1.0, 1.0]; 4]).unwrap(),
            reference_nits: 203.0,
        };
        let mut path = std::env::temp_dir();
        path.push(format!(
            "vshot-hdr-output-test-{}-{}.png",
            std::process::id(),
            unique_suffix()
        ));
        write_capture_files(
            &path,
            &frame,
            Some(&hdr),
            1,
            PngCompression::default(),
            HdrFormat::Radiance,
        )
        .unwrap();
        let png = std::fs::read(&path).unwrap();
        let radiance = std::fs::read(hdr_sibling_path(&path, HdrFormat::Radiance)).unwrap();
        let _ = std::fs::remove_file(&path);
        let _ = std::fs::remove_file(hdr_sibling_path(&path, HdrFormat::Radiance));
        assert!(
            png.starts_with(b"\x89PNG\r\n\x1a\n"),
            "the SDR half is a PNG"
        );
        assert!(
            radiance.starts_with(b"#?RADIANCE"),
            "the HDR half is Radiance"
        );
        assert_eq!(path.extension().unwrap(), "png");
        assert_eq!(
            hdr_sibling_path(&path, HdrFormat::Radiance)
                .extension()
                .unwrap(),
            "hdr"
        );
    }

    #[test]
    fn the_default_hdr_format_writes_an_avif_beside_the_png() {
        // The pair a capture writes by default: a PNG and an AVIF of the same
        // stem, the AVIF an ISOBMFF file declaring BT.2020 PQ.
        let frame = Frame::solid(Size::new(2, 2), [10, 20, 30, 255]).unwrap();
        let hdr = HdrHalf {
            frame: HdrFrame::new(Size::new(16, 16), vec![[4.0, 2.0, 1.0, 1.0]; 256]).unwrap(),
            reference_nits: 203.0,
        };
        let mut path = std::env::temp_dir();
        path.push(format!(
            "vshot-avif-output-test-{}-{}.png",
            std::process::id(),
            unique_suffix()
        ));
        write_capture_files(
            &path,
            &frame,
            Some(&hdr),
            1,
            PngCompression::default(),
            HdrFormat::Avif,
        )
        .unwrap();
        let avif_path = hdr_sibling_path(&path, HdrFormat::Avif);
        let avif = std::fs::read(&avif_path).unwrap();
        let _ = std::fs::remove_file(&path);
        let _ = std::fs::remove_file(&avif_path);
        assert_eq!(&avif[4..8], b"ftyp", "the HDR half is an ISOBMFF file");
        assert!(avif.windows(4).any(|window| window == b"avif"));
    }

    #[test]
    fn a_destination_named_for_the_hdr_half_does_not_overwrite_the_png() {
        // A destination that already ends in the HDR format's suffix names the
        // HDR half; the PNG goes beside it rather than to the same path, so
        // neither file is lost.
        let frame = Frame::solid(Size::new(2, 2), [10, 20, 30, 255]).unwrap();
        let hdr = HdrHalf {
            frame: HdrFrame::new(Size::new(2, 2), vec![[4.0, 2.0, 1.0, 1.0]; 4]).unwrap(),
            reference_nits: 203.0,
        };
        let mut path = std::env::temp_dir();
        path.push(format!(
            "vshot-hdr-destination-test-{}-{}.hdr",
            std::process::id(),
            unique_suffix()
        ));
        write_capture_files(
            &path,
            &frame,
            Some(&hdr),
            1,
            PngCompression::default(),
            HdrFormat::Radiance,
        )
        .unwrap();
        let png_path = png_sibling_path(&path);
        let png = std::fs::read(&png_path).unwrap();
        let radiance = std::fs::read(&path).unwrap();
        let _ = std::fs::remove_file(&path);
        let _ = std::fs::remove_file(&png_path);
        assert!(
            png.starts_with(b"\x89PNG\r\n\x1a\n"),
            "the SDR half is a PNG"
        );
        assert!(
            radiance.starts_with(b"#?RADIANCE"),
            "the HDR half is Radiance"
        );
        assert_eq!(png_path.extension().unwrap(), "png");
    }

    #[test]
    fn a_capture_without_hdr_writes_the_sdr_file_alone() {
        let frame = Frame::solid(Size::new(1, 1), [1, 2, 3, 255]).unwrap();
        let mut path = std::env::temp_dir();
        path.push(format!(
            "vshot-sdr-output-test-{}-{}.png",
            std::process::id(),
            unique_suffix()
        ));
        write_capture_files(
            &path,
            &frame,
            None,
            1,
            PngCompression::default(),
            HdrFormat::Avif,
        )
        .unwrap();
        assert!(path.exists());
        assert!(!hdr_sibling_path(&path, HdrFormat::Avif).exists());
        assert!(!hdr_sibling_path(&path, HdrFormat::Radiance).exists());
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn file_uri_percent_encodes_path_bytes() {
        assert_eq!(
            file_uri(Path::new("/tmp/vshot image#%.png")),
            "file:///tmp/vshot%20image%23%25.png"
        );
    }

    fn unique_suffix() -> u128 {
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    }
}

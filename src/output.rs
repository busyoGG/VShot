use std::fs::File;
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use chrono::{DateTime, Local};

use crate::cli::Destination;
use crate::error::{Result, VshotError};
use crate::model::{Frame, PngCompression};

/// Writes a captured frame to `destination`. `density` is the frame's device
/// pixels per logical pixel (the scale of the output it came from); every PNG
/// written to a file, stdout or the clipboard carries it as its physical
/// resolution, and the pin destination states it in the request, so the
/// on-screen size and the sharpness of the capture survive wherever the image
/// goes next. `compression` is the PNG encoder's level, which costs time and
/// buys size; it does not apply to the pin destination, whose PNG only travels
/// to the daemon through a temp file.
pub fn write_frame(
    frame: &Frame,
    destination: &Destination,
    density: u32,
    compression: PngCompression,
) -> Result<()> {
    match destination {
        Destination::File(path) => {
            let path = expand_output_path(path, Local::now());
            write_file(&path, &frame.encode_png(Some(density), compression)?)?;
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
        Destination::Pin => crate::pin::pin_png(&frame.to_png()?, density),
    }
}

fn expand_output_path(path: &Path, now: DateTime<Local>) -> PathBuf {
    let pattern = path.to_string_lossy();
    PathBuf::from(now.format(&pattern).to_string())
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

    #[test]
    fn output_path_preserves_escaped_percent() {
        let path = expand_output_path(Path::new("vshot-%%-%Y.png"), Local::now());
        assert!(path.to_string_lossy().starts_with("vshot-%-"));
        assert!(path.to_string_lossy().ends_with(".png"));
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

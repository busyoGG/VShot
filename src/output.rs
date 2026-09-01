use std::fs::File;
use std::io::{self, Write};
use std::path::Path;
use std::process::{Command, Stdio};

use crate::cli::Destination;
use crate::error::{Result, VshotError};
use crate::model::Frame;

pub fn write_frame(frame: &Frame, destination: &Destination) -> Result<()> {
    let png = frame.to_png()?;
    match destination {
        Destination::File(path) => write_file(path, &png),
        Destination::Stdout => {
            io::stdout()
                .write_all(&png)
                .map_err(|source| VshotError::WriteFile {
                    path: "stdout".into(),
                    source,
                })
        }
        Destination::Clipboard => copy_to_clipboard(&png),
    }
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
    let mut child = Command::new("wl-copy")
        .arg("--type")
        .arg("image/png")
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
        VshotError::Clipboard(format!("failed to send PNG to wl-copy: {source}"))
    })?;
    drop(stdin);
    let output = child
        .wait_with_output()
        .map_err(|source| VshotError::CommandIo {
            program: "wl-copy".into(),
            source,
        })?;
    if !output.status.success() {
        return Err(VshotError::Clipboard(format!(
            "wl-copy exited with {}: {}",
            output.status,
            String::from_utf8_lossy(&output.stderr).trim()
        )));
    }
    Ok(())
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

    fn unique_suffix() -> u128 {
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    }
}

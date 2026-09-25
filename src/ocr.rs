// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Text recognition, for `vshot ocr` and the annotation editor's text tool.
//!
//! Two engines can do the work, and the config file picks which:
//!
//! - `builtin` (the default) runs PaddleOCR's own PP-OCR models — the ONNX
//!   conversions of them — through ONNX Runtime on the CPU, in this process.
//!   No network, no GPU, no extra service: the models are read from disk and
//!   the engine is linked in.
//! - `external` hands the image to a command of the user's choosing and reads
//!   its answer back.  This is the escape hatch for anyone who wants a GPU
//!   (an ONNX Runtime build with ROCm, CUDA or MIGraphX, a Python rapidocr
//!   with the ROCm wheels, a service on another machine), because vshot itself
//!   never links a GPU runtime: doing so would drag a gigabyte of driver
//!   packages into a screenshot tool's dependency list.
//!
//! Both produce the same thing: the lines of text in reading order.

use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use crate::config::OcrDefaults;
use crate::error::{Result, VshotError};

/// Where the models live.  The package installs them here; a source checkout
/// falls back to the copy in the tree (see `model_directory`).
const INSTALLED_MODEL_DIR: &str = "/usr/share/vshot/models";

/// The three files a built-in run needs, in the names the package installs.
const DETECTION_MODEL: &str = "det.onnx";
const RECOGNITION_MODEL: &str = "rec.onnx";
const CHARACTER_DICT: &str = "dict.txt";

/// One recognized line: its text and where it sat in the image.
#[derive(Clone, Debug, PartialEq)]
pub struct OcrLine {
    pub text: String,
    /// Top-left corner and size in pixels of the image handed to `recognize`.
    pub rect: crate::geometry::Rect,
}

/// Which engine does the work, resolved from the config file.
#[derive(Clone, Debug, PartialEq)]
pub enum Engine {
    /// In-process ONNX Runtime on the CPU.
    Builtin,
    /// A command that reads a PNG on stdin and writes text on stdout.
    External(ExternalEngine),
}

/// How to talk to an external OCR program.
#[derive(Clone, Debug, PartialEq)]
pub struct ExternalEngine {
    /// The program and its arguments.  The image path is appended as the last
    /// argument unless `stdin` is set, in which case the PNG goes to stdin.
    pub command: Vec<String>,
    /// Feed the PNG on stdin instead of naming a file on the command line.
    pub stdin: bool,
    /// How long to wait before giving up, in seconds.
    pub timeout: u64,
}

impl ExternalEngine {
    fn program(&self) -> Result<&str> {
        self.command
            .first()
            .map(String::as_str)
            .ok_or_else(|| VshotError::Ocr("the external OCR command is empty".into()))
    }
}

/// Resolves the engine from the config file.  An unusable `external` entry is
/// an error rather than a silent fallback: a user who configured a GPU engine
/// wants to hear that it did not run, not get CPU output they did not ask for.
pub fn engine_from_config(defaults: &OcrDefaults) -> Result<Engine> {
    match defaults.engine.as_deref() {
        None | Some("builtin") => Ok(Engine::Builtin),
        Some("external") => {
            let external = defaults.external.as_ref().ok_or_else(|| {
                VshotError::Ocr(
                    "ocr.engine is \"external\" but no ocr.external.command is configured".into(),
                )
            })?;
            let command = external.command.clone().unwrap_or_default();
            if command.is_empty() {
                return Err(VshotError::Ocr("ocr.external.command is empty".into()));
            }
            Ok(Engine::External(ExternalEngine {
                command,
                stdin: external.stdin.unwrap_or(false),
                timeout: external.timeout.unwrap_or(30).max(1),
            }))
        }
        Some(other) => Err(VshotError::Ocr(format!(
            "unknown ocr engine `{other}`: expected `builtin` or `external`"
        ))),
    }
}

/// Where the models are: `$VSHOT_OCR_MODELS` when it names a complete set (the
/// override the README documents), then the installed directory, then the
/// first `models/` directory above the executable, which covers a package that
/// ships them beside the binary, a tarball, and a source checkout
/// (`target/release/vshot` and `target/release/deps/vshot-<hash>`, both a few
/// levels below the tree).  `None` when none of them is complete, which the
/// caller reports with the paths it tried.
pub fn model_directory() -> Option<PathBuf> {
    let mut candidates: Vec<PathBuf> = Vec::new();
    if let Some(override_directory) = std::env::var_os("VSHOT_OCR_MODELS") {
        if !override_directory.is_empty() {
            candidates.push(PathBuf::from(override_directory));
        }
    }
    candidates.push(PathBuf::from(INSTALLED_MODEL_DIR));
    if let Ok(exe) = std::env::current_exe() {
        let mut directory = exe.parent().map(Path::to_path_buf);
        // Four levels is enough for `target/release/deps/` and then some; the
        // walk stops early anyway once it leaves the filesystem root.
        for _ in 0..4 {
            let Some(current) = directory else {
                break;
            };
            candidates.push(current.join("models"));
            candidates.push(current.join("share/vshot/models"));
            directory = current.parent().map(Path::to_path_buf);
        }
    }
    candidates.into_iter().find(|directory| {
        directory.join(DETECTION_MODEL).is_file()
            && directory.join(RECOGNITION_MODEL).is_file()
            && directory.join(CHARACTER_DICT).is_file()
    })
}

/// The built-in engine, built once per run.
///
/// Loading the models costs a few hundred milliseconds, so a caller that
/// recognizes several images keeps one of these rather than rebuilding it.
pub struct Builtin {
    ocr: oar_ocr::prelude::OAROCR,
}

impl Builtin {
    /// Loads the models from `directory`.
    pub fn load(directory: &Path) -> Result<Self> {
        let ocr = oar_ocr::prelude::OAROCRBuilder::new(
            directory.join(DETECTION_MODEL),
            directory.join(RECOGNITION_MODEL),
            directory.join(CHARACTER_DICT),
        )
        .build()
        .map_err(|error| VshotError::Ocr(format!("failed to load the OCR models: {error}")))?;
        Ok(Self { ocr })
    }

    /// Recognizes the text in `frame`.
    pub fn recognize(&self, frame: &crate::model::Frame) -> Result<Vec<OcrLine>> {
        let png = frame.encode_png(None, crate::model::PngCompression::Fastest)?;
        let image = oar_ocr::utils::image::load_image_from_memory(&png)
            .map_err(|error| VshotError::Ocr(format!("cannot read the image to OCR: {error}")))?;
        let results = self
            .ocr
            .predict(vec![image])
            .map_err(|error| VshotError::Ocr(format!("OCR failed: {error}")))?;
        let Some(result) = results.into_iter().next() else {
            return Ok(Vec::new());
        };
        Ok(lines_from_regions(&result.text_regions))
    }
}

/// Turns the engine's regions into lines, in reading order.
///
/// The detector returns regions in no particular order, so they are sorted by
/// their vertical centre and then by their left edge: a page of text comes out
/// as it reads rather than as the detector happened to find it.  The sort is
/// deliberately not a full layout analysis — columns would need one, and a
/// screenshot's text is overwhelmingly a single column.
fn lines_from_regions(regions: &[oar_ocr::prelude::TextRegion]) -> Vec<OcrLine> {
    let mut lines: Vec<OcrLine> = regions
        .iter()
        .filter_map(|region| {
            let (text, _confidence) = region.text_with_confidence()?;
            if text.trim().is_empty() {
                return None;
            }
            let rect = &region.bounding_box;
            Some(OcrLine {
                text: text.to_string(),
                rect: crate::geometry::Rect::new(
                    rect.x_min().round() as i32,
                    rect.y_min().round() as i32,
                    (rect.x_max() - rect.x_min()).max(0.0).round() as u32,
                    (rect.y_max() - rect.y_min()).max(0.0).round() as u32,
                ),
            })
        })
        .collect();
    lines.sort_by_key(|line| {
        let center = i64::from(line.rect.origin.y) + i64::from(line.rect.size.height) / 2;
        (center, i64::from(line.rect.origin.x))
    });
    lines
}

/// Runs an external OCR program over `frame`.
///
/// The image is written to a temporary PNG (or piped on stdin) and the
/// program's stdout is the answer, one line of text per line.  Nothing about
/// the program is assumed beyond that: it may be a Python script using the
/// ROCm wheels, a shell wrapper around a service, or another build of vshot.
pub fn recognize_external(
    engine: &ExternalEngine,
    frame: &crate::model::Frame,
) -> Result<Vec<OcrLine>> {
    let png = frame.encode_png(None, crate::model::PngCompression::Fastest)?;
    let mut file = tempfile::Builder::new()
        .prefix("vshot-ocr-")
        .suffix(".png")
        .tempfile()
        .map_err(|error| VshotError::Ocr(format!("cannot create a temporary image: {error}")))?;
    file.write_all(&png)
        .map_err(|error| VshotError::Ocr(format!("cannot write the temporary image: {error}")))?;
    file.flush()
        .map_err(|error| VshotError::Ocr(format!("cannot write the temporary image: {error}")))?;

    let program = engine.program()?;
    let mut command = Command::new(program);
    command.args(&engine.command[1..]);
    if engine.stdin {
        command.stdin(Stdio::piped());
    } else {
        command.arg(file.path());
    }
    command.stdout(Stdio::piped()).stderr(Stdio::piped());

    let mut child = command.spawn().map_err(|error| {
        VshotError::Ocr(format!(
            "cannot run the external OCR command `{program}`: {error}"
        ))
    })?;
    if engine.stdin {
        if let Some(mut stdin) = child.stdin.take() {
            // A program that exits without reading all of stdin closes the
            // pipe; that is its business, not an error here.
            let _ = stdin.write_all(&png);
        }
    }

    // `wait_with_output` has no timeout, so the wait is done by hand with the
    // deadline checked between polls: an external engine that hangs must not
    // hang the capture that asked for it.
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(engine.timeout);
    let output = loop {
        match child.try_wait() {
            Ok(Some(_)) => break child.wait_with_output(),
            Ok(None) => {
                if std::time::Instant::now() >= deadline {
                    let _ = child.kill();
                    let _ = child.wait();
                    return Err(VshotError::Ocr(format!(
                        "the external OCR command `{program}` did not finish within {} seconds",
                        engine.timeout
                    )));
                }
                std::thread::sleep(std::time::Duration::from_millis(20));
            }
            Err(error) => {
                return Err(VshotError::Ocr(format!(
                    "cannot wait for the external OCR command `{program}`: {error}"
                )))
            }
        }
    };
    let output = output.map_err(|error| {
        VshotError::Ocr(format!("cannot read the output of `{program}`: {error}"))
    })?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let stderr = stderr.trim();
        return Err(VshotError::Ocr(if stderr.is_empty() {
            format!(
                "the external OCR command `{program}` exited with {}",
                output.status
            )
        } else {
            format!(
                "the external OCR command `{program}` exited with {}: {stderr}",
                output.status
            )
        }));
    }
    let text = String::from_utf8_lossy(&output.stdout);
    Ok(text
        .lines()
        .filter(|line| !line.trim().is_empty())
        .map(|line| OcrLine {
            text: line.to_string(),
            // An external engine reports text, not geometry, so the rect is
            // empty: there is nothing to place, only something to read.
            rect: crate::geometry::Rect::new(0, 0, 0, 0),
        })
        .collect())
}

/// Recognizes text in `frame` with whichever engine the config file picks.
pub fn recognize(frame: &crate::model::Frame) -> Result<Vec<OcrLine>> {
    let defaults = crate::config::load().ocr;
    match engine_from_config(&defaults)? {
        Engine::Builtin => {
            let directory = model_directory().ok_or_else(|| {
                VshotError::Ocr(format!(
                    "the OCR models are not installed: looked for {DETECTION_MODEL}, \
                     {RECOGNITION_MODEL} and {CHARACTER_DICT} in {INSTALLED_MODEL_DIR} and \
                     beside the executable"
                ))
            })?;
            Builtin::load(&directory)?.recognize(frame)
        }
        Engine::External(engine) => recognize_external(&engine, frame),
    }
}

/// The recognized lines joined into the text a caller prints or copies.
///
/// The lines are joined and nothing is added at the end.  A trailing newline
/// here would be a stray blank line for every caller that copies the text
/// somewhere (the clipboard, or a `| wl-copy` pipeline), and it is not this
/// function's to add: `write_ocr_text` adds one back for stdout, which is the
/// only destination where a line without an end looks wrong.
pub fn join_lines(lines: &[OcrLine]) -> String {
    lines
        .iter()
        .map(|line| line.text.as_str())
        .collect::<Vec<_>>()
        .join("\n")
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::{OcrDefaults, OcrExternalDefaults};

    fn external(command: &[&str]) -> OcrDefaults {
        OcrDefaults {
            engine: Some("external".into()),
            external: Some(OcrExternalDefaults {
                command: Some(command.iter().map(|part| (*part).to_string()).collect()),
                stdin: None,
                timeout: None,
            }),
            notify: None,
        }
    }

    #[test]
    fn the_default_engine_is_the_built_in_one() {
        assert_eq!(
            engine_from_config(&OcrDefaults::default()).unwrap(),
            Engine::Builtin
        );
        let named = OcrDefaults {
            engine: Some("builtin".into()),
            external: None,
            notify: None,
        };
        assert_eq!(engine_from_config(&named).unwrap(), Engine::Builtin);
    }

    #[test]
    fn an_external_engine_keeps_its_command_and_defaults_its_timeout() {
        let engine = engine_from_config(&external(&["my-ocr", "--json"])).unwrap();
        match engine {
            Engine::External(engine) => {
                assert_eq!(engine.command, ["my-ocr", "--json"]);
                assert!(!engine.stdin);
                assert_eq!(engine.timeout, 30);
            }
            other => panic!("expected an external engine, got {other:?}"),
        }
    }

    #[test]
    fn an_external_engine_without_a_command_is_an_error() {
        // Asking for an external engine and forgetting the command must not
        // quietly produce CPU output: the user asked for something specific.
        let missing = OcrDefaults {
            engine: Some("external".into()),
            external: None,
            notify: None,
        };
        assert!(matches!(
            engine_from_config(&missing),
            Err(VshotError::Ocr(_))
        ));
        let empty = external(&[]);
        assert!(matches!(
            engine_from_config(&empty),
            Err(VshotError::Ocr(_))
        ));
    }

    #[test]
    fn an_unknown_engine_name_is_reported_rather_than_ignored() {
        let typo = OcrDefaults {
            engine: Some("gpu".into()),
            external: None,
            notify: None,
        };
        let error = engine_from_config(&typo).unwrap_err();
        assert!(error.to_string().contains("gpu"), "{error}");
    }

    #[test]
    fn lines_are_joined_without_a_trailing_newline() {
        // The end of the text is the end of the last line: anything a caller
        // copies out is what was recognized, with no blank line appended.
        let lines = vec![
            OcrLine {
                text: "first".into(),
                rect: crate::geometry::Rect::new(0, 0, 10, 10),
            },
            OcrLine {
                text: "second".into(),
                rect: crate::geometry::Rect::new(0, 20, 10, 10),
            },
        ];
        assert_eq!(join_lines(&lines), "first\nsecond");
        assert_eq!(join_lines(&[]), "");
    }

    #[test]
    fn an_external_engine_is_run_and_its_stdout_becomes_the_lines() {
        // `/bin/cat` with no file reads stdin, so it echoes the PNG back; the
        // point is that the plumbing runs a real program and reads its stdout.
        // A shell that prints fixed text is the more useful shape, and it also
        // proves the arguments arrive.
        let defaults = external(&["/bin/sh", "-c", "printf 'alpha\\nbeta\\n'"]);
        let Engine::External(engine) = engine_from_config(&defaults).unwrap() else {
            panic!("expected an external engine");
        };
        let frame =
            crate::model::Frame::solid(crate::geometry::Size::new(4, 4), [0, 0, 0, 255]).unwrap();
        let lines = recognize_external(&engine, &frame).unwrap();
        assert_eq!(
            lines
                .iter()
                .map(|line| line.text.as_str())
                .collect::<Vec<_>>(),
            ["alpha", "beta"]
        );
    }

    #[test]
    fn an_external_engine_that_fails_reports_its_stderr() {
        let defaults = external(&["/bin/sh", "-c", "echo 'no GPU here' >&2; exit 3"]);
        let Engine::External(engine) = engine_from_config(&defaults).unwrap() else {
            panic!("expected an external engine");
        };
        let frame =
            crate::model::Frame::solid(crate::geometry::Size::new(4, 4), [0, 0, 0, 255]).unwrap();
        let error = recognize_external(&engine, &frame).unwrap_err();
        let text = error.to_string();
        assert!(text.contains("no GPU here"), "{text}");
        assert!(text.contains('3'), "{text}");
    }

    #[test]
    fn an_external_engine_that_never_finishes_is_killed() {
        let defaults = external(&["/bin/sh", "-c", "sleep 30"]);
        let Engine::External(mut engine) = engine_from_config(&defaults).unwrap() else {
            panic!("expected an external engine");
        };
        engine.timeout = 1;
        let frame =
            crate::model::Frame::solid(crate::geometry::Size::new(4, 4), [0, 0, 0, 255]).unwrap();
        let started = std::time::Instant::now();
        let error = recognize_external(&engine, &frame).unwrap_err();
        assert!(error.to_string().contains("did not finish"), "{error}");
        // It gave up on the deadline rather than waiting out the sleep.
        assert!(started.elapsed() < std::time::Duration::from_secs(10));
    }

    #[test]
    fn a_missing_program_is_reported_by_name() {
        let defaults = external(&["/nonexistent/ocr-program"]);
        let Engine::External(engine) = engine_from_config(&defaults).unwrap() else {
            panic!("expected an external engine");
        };
        let frame =
            crate::model::Frame::solid(crate::geometry::Size::new(4, 4), [0, 0, 0, 255]).unwrap();
        let error = recognize_external(&engine, &frame).unwrap_err();
        assert!(error.to_string().contains("ocr-program"), "{error}");
    }

    /// The built-in engine against the real models.
    ///
    /// Ignored by default because it needs the 30 MB of weights on disk, which
    /// a plain `cargo test` has no way to fetch.  Point `VSHOT_OCR_MODELS` at
    /// a directory holding `det.onnx`, `rec.onnx` and `dict.txt` and run it
    /// with `--ignored`; without the variable the in-tree `models/` directory
    /// is used, which is where the PKGBUILD's sources land in a checkout.
    ///
    /// What it pins is the part no unit test can: that the models load, that
    /// the pipeline is wired the right way round, and that the characters come
    /// back in one piece.  A screenshot's text is drawn, not typed, so the
    /// frame is drawn with the same software renderer the editor uses.
    #[test]
    #[ignore = "needs the PP-OCR models on disk; see the comment above"]
    fn the_built_in_engine_reads_drawn_text() {
        let directory = std::env::var_os("VSHOT_OCR_MODELS")
            .map(std::path::PathBuf::from)
            .or_else(model_directory)
            .expect("the models have to be reachable, see the comment above");
        let engine = Builtin::load(&directory).expect("the models load");

        // Drawn in the built-in 5x7 glyph font, which the OCR has to read off
        // a 4x upscale: the glyphs are 5x7 cells, too small to recognize at
        // their native size.
        let mut frame =
            crate::model::Frame::solid(crate::geometry::Size::new(360, 60), [255, 255, 255, 255])
                .unwrap();
        frame
            .draw_text(
                crate::geometry::Point::new(12, 14),
                "HELLO",
                [0, 0, 0, 255],
                4,
            )
            .unwrap();

        let lines = engine.recognize(&frame).expect("recognition runs");
        let text = lines
            .iter()
            .map(|line| line.text.as_str())
            .collect::<Vec<_>>()
            .join(" ");
        assert!(
            text.to_uppercase().contains("HELLO"),
            "the drawn text has to come back, got {text:?}"
        );
        // A recognized line carries where it was, so the editor can place it.
        let first = lines.first().expect("at least one line");
        assert!(
            first.rect.size.width > 0 && first.rect.size.height > 0,
            "{first:?}"
        );
    }
}

//! Remembered defaults, read from the same `config.json` the Qt helper writes.
//!
//! The file is shared with the helper, which owns the `editor` section — the
//! style the user last left the annotation editor in.  This side reads only
//! the `cli` section, which the helper leaves alone.
//!
//! A config file is a convenience, never a requirement: a missing, unreadable,
//! or malformed file yields the built-in defaults, and a bad value inside a
//! valid file falls back field by field.  Nothing here can fail a capture.

use std::path::PathBuf;

use serde::Deserialize;

use crate::model::PngCompression;

/// The defaults a command-line flag falls back to when it is not given.
///
/// The JSON keys are the flag names, so `--png-compression` is written
/// `"png-compression"` and `--max-height` is `"max-height"`; that is what a
/// user editing the file by hand would reach for, and it matches the Qt side's
/// camelCase habit of naming things as they appear in the UI.
///
/// Unknown keys are ignored rather than rejected: this is a file the user may
/// edit by hand and that a newer vshot may write, and refusing the whole
/// section over one unrecognized name would silently drop every default in it.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default, rename_all = "kebab-case")]
pub struct CliDefaults {
    /// `--png-compression`, one of `none` / `fastest` / `fast` / `balanced` /
    /// `high`.
    pub png_compression: Option<String>,
    /// `monitor`'s output name when none is given; `current` means the output
    /// under the pointer.
    pub monitor: Option<String>,
    pub long: LongDefaults,
    pub pin: PinDefaults,
    pub ocr: OcrDefaults,
}

/// Text recognition, used by `vshot ocr` and the editor's text tool.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default, rename_all = "kebab-case")]
pub struct OcrDefaults {
    /// `builtin` (the default) or `external`.
    pub engine: Option<String>,
    /// Only read when `engine` is `external`.
    pub external: Option<OcrExternalDefaults>,
}

/// How to reach an OCR program the user runs themselves.
///
/// This is the way to use a GPU without vshot linking a GPU runtime: the
/// program may be anything that reads a PNG and writes text, and vshot only
/// has to start it and read its stdout.  A user with a ROCm or CUDA build of
/// ONNX Runtime — or a machine elsewhere on the network — points this at it.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default, rename_all = "kebab-case")]
pub struct OcrExternalDefaults {
    /// The program and its arguments, as an array.  The image's path is
    /// appended unless `stdin` is set.
    pub command: Option<Vec<String>>,
    /// Send the PNG on stdin instead of naming a file on the command line.
    pub stdin: Option<bool>,
    /// Seconds to wait before giving up.  Defaults to 30.
    pub timeout: Option<u64>,
}

/// Defaults for `vshot long`.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default, rename_all = "kebab-case")]
pub struct LongDefaults {
    pub notches: Option<u32>,
    pub max_height: Option<u32>,
    pub max_frames: Option<u32>,
    pub timeout: Option<u64>,
    pub ignore_top: Option<u32>,
    /// Scroll backend: `auto` / `wlr` / `portal` / `uinput`.
    pub inject: Option<String>,
}

/// Defaults for `vshot pin`.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct PinDefaults {
    /// `--density` for every pinned image.
    pub density: Option<u32>,
}

/// The whole file, as far as this side is concerned.  The helper's `editor`
/// section is deliberately not modelled: it is the helper's to read and write,
/// and an unknown section must not make the file "malformed" here.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
struct ConfigFile {
    cli: CliDefaults,
}

/// The path of the config file: `$XDG_CONFIG_HOME/vshot/config.json`, falling
/// back to `$HOME/.config/vshot/config.json`.  `None` when neither variable is
/// set, in which case there is nothing to read.
pub fn config_path() -> Option<PathBuf> {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .filter(|path| !path.as_os_str().is_empty())
        .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")))?;
    Some(base.join("vshot").join("config.json"))
}

/// Reads the remembered command-line defaults.  Every failure — no file, no
/// path, unreadable, unparseable — is the built-in default.
pub fn load() -> CliDefaults {
    let Some(path) = config_path() else {
        return CliDefaults::default();
    };
    let Ok(text) = std::fs::read_to_string(&path) else {
        return CliDefaults::default();
    };
    serde_json::from_str::<ConfigFile>(&text)
        .map(|file| file.cli)
        .unwrap_or_default()
}

/// The compression the config file remembers, or `None` when it says nothing
/// usable.  The caller keeps its own built-in default.
pub fn compression_default() -> Option<PngCompression> {
    parse_compression(load().png_compression.as_deref()?)
}

/// The names are the ones `--png-compression` accepts, so a value copied out
/// of `vshot --help` works in the file unchanged.
fn parse_compression(name: &str) -> Option<PngCompression> {
    match name {
        "none" => Some(PngCompression::None),
        "fastest" => Some(PngCompression::Fastest),
        "fast" => Some(PngCompression::Fast),
        "balanced" => Some(PngCompression::Balanced),
        "high" => Some(PngCompression::High),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_compression_name_the_cli_accepts_is_parsed() {
        for name in ["none", "fastest", "fast", "balanced", "high"] {
            assert!(parse_compression(name).is_some(), "{name} must parse");
        }
        // An unknown name is a typo in a hand-edited file: ignore it and let
        // the built-in default stand.
        assert!(parse_compression("slowest").is_none());
    }

    #[test]
    fn a_config_without_the_cli_section_reads_as_defaults() {
        let file: ConfigFile = serde_json::from_str(r##"{"editor":{"color":"#ff0000"}}"##)
            .expect("an editor-only file is valid");
        assert!(file.cli.png_compression.is_none());
        assert!(file.cli.long.notches.is_none());
        assert!(file.cli.pin.density.is_none());
    }

    #[test]
    fn cli_values_are_read_field_by_field() {
        let file: ConfigFile = serde_json::from_str(
            r#"{"cli":{"png-compression":"high","long":{"notches":3,"max-height":1000}}}"#,
        )
        .expect("a cli section parses");
        assert_eq!(file.cli.png_compression.as_deref(), Some("high"));
        assert_eq!(file.cli.long.notches, Some(3));
        assert_eq!(file.cli.long.max_height, Some(1000));
        // Absent fields stay absent rather than becoming zero, so the caller's
        // own default is what stands.
        assert!(file.cli.long.timeout.is_none());
        assert!(file.cli.monitor.is_none());
    }
}

#[cfg(test)]
mod path_tests {
    use super::*;

    #[test]
    fn the_config_path_honours_xdg_config_home() {
        // SAFETY: single-threaded test body, and the variable is restored
        // before returning.
        let saved = std::env::var_os("XDG_CONFIG_HOME");
        std::env::set_var("XDG_CONFIG_HOME", "/tmp/vshot-cfg-probe");
        assert_eq!(
            config_path(),
            Some(PathBuf::from("/tmp/vshot-cfg-probe/vshot/config.json"))
        );
        match saved {
            Some(value) => std::env::set_var("XDG_CONFIG_HOME", value),
            None => std::env::remove_var("XDG_CONFIG_HOME"),
        }
    }
}

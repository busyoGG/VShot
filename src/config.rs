// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

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
    pub record: RecordDefaults,
    pub replay: ReplayDefaults,
}

/// Defaults for `vshot record`.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default, rename_all = "kebab-case")]
pub struct RecordDefaults {
    /// The microphone to record when `--mic` is given without a name, or
    /// when the command line says neither `--mic` nor `--no-mic`: an empty
    /// string is the session's default source, a name is a PipeWire node.
    /// `null` (the default) means a recording is silent.
    pub mic: Option<String>,
    /// The codec `--encoder` falls back to: `h264` (the built-in default),
    /// `hevc` or `av1`.
    pub encoder: Option<String>,
    /// The hardware encoder `--encoder-backend` falls back to: `auto` (the
    /// built-in default, VAAPI where it opens else NVENC), `vaapi` or
    /// `nvenc`.
    pub encoder_backend: Option<String>,
    /// The frame rate `--fps` falls back to, 1-240.
    pub fps: Option<u32>,
    /// Whether a recording goes through the desktop portal without
    /// `--portal`.  `null` (the default) keeps the compositor's own
    /// protocols; `--no-portal` overrides a remembered `true`.
    pub portal: Option<bool>,
    /// The windows a `record window` moves between without `--follow`: the
    /// names are the same ones the flag takes, and the fallback is read only
    /// when the target is a plain `window` (no NAME, no `--pick`) — a
    /// remembered follow must not turn a `record monitor` into an error, so
    /// every other target ignores it.  `null` (the default) means whatever the
    /// focus is on stays recorded; `--no-follow` turns a remembered list off
    /// for one recording.
    pub follow: Option<Vec<String>>,
    /// Whether a finished recording raises a desktop notification.  A
    /// recording started from a keybinding ends with nothing on screen to
    /// mark the moment, so the notification is its receipt; that is why the
    /// absent key means yes rather than no.  Like `ocr.notify` this is a
    /// settings-window and hand-edit key with no flag of its own, and only
    /// `false` is ever written.
    pub notify: Option<bool>,
}

/// Defaults for `vshot replay`: the memory-replay settings the flags fall back
/// to when they are not given.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default, rename_all = "kebab-case")]
pub struct ReplayDefaults {
    /// How many seconds of history the ring keeps; `--window` overrides it.
    /// 30 unless the file says otherwise.
    pub window: Option<u64>,
    /// The codec `--encoder` falls back to for a replay: `h264` (the built-in
    /// default), `hevc` or `av1`.
    pub encoder: Option<String>,
    /// The hardware encoder `--encoder-backend` falls back to for a replay:
    /// `auto` (the default), `vaapi` or `nvenc`.
    pub encoder_backend: Option<String>,
    /// The frame rate `--fps` falls back to, 1-240.
    pub fps: Option<u32>,
    /// The key-frame distance in seconds (1-10): a smaller value makes a save
    /// start closer to the requested edge at the cost of a bigger ring.
    pub gop: Option<u64>,
    /// The microphone the replay keeps in the ring, if any: an empty string is
    /// the session's default source, a name is a PipeWire node.  `null` (the
    /// default) means the replay is silent.
    pub mic: Option<String>,
    /// Whether a replay uses the desktop portal instead of the compositor's
    /// own protocols, as `record --portal` does.
    pub portal: Option<bool>,
    /// The windows a `replay start window` moves between without `--follow`,
    /// read on the same terms as `record.follow`: only a plain `window` target
    /// falls back to them, and `--no-follow` turns a remembered list off for
    /// one session.
    pub follow: Option<Vec<String>>,
    /// Where a triggered save lands; strftime is expanded.  The videos
    /// directory with a timestamped name when unset.
    pub save_dir: Option<String>,
    /// Whether a save raises a desktop notification.  Absent means yes.
    pub notify: Option<bool>,
}

/// Text recognition, used by `vshot ocr` and the editor's text tool.
#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default, rename_all = "kebab-case")]
pub struct OcrDefaults {
    /// `builtin` (the default) or `external`.
    pub engine: Option<String>,
    /// Only read when `engine` is `external`.
    pub external: Option<OcrExternalDefaults>,
    /// Whether a finished recognition raises a desktop notification.  Absent
    /// means yes; the settings window writes it out.
    pub notify: Option<bool>,
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
    fn the_ocr_section_carries_the_notification_switch() {
        let file: ConfigFile = serde_json::from_str(
            r#"{"cli":{"ocr":{"notify":false,"engine":"external","external":{"command":["x"]}}}}"#,
        )
        .expect("an ocr section parses");
        assert_eq!(file.cli.ocr.notify, Some(false));
        // The switch does not displace the rest of the section.
        assert_eq!(file.cli.ocr.engine.as_deref(), Some("external"));
        // Absent is not "off": the caller's own default -- on -- is what
        // stands, which is what a file written before this key existed says.
        let silent: ConfigFile =
            serde_json::from_str(r#"{"cli":{"ocr":{"engine":"builtin"}}}"#).unwrap();
        assert_eq!(silent.cli.ocr.notify, None);
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
        // The microphone is off unless the file says otherwise: `null` and
        // an absent key are the same silence, an empty string is the
        // session's default source, and a name is that input.
        assert!(file.cli.record.mic.is_none());
        let file: ConfigFile =
            serde_json::from_str(r#"{"cli":{"record":{"mic":""}}}"#).expect("an empty name parses");
        assert_eq!(file.cli.record.mic.as_deref(), Some(""));
        let file: ConfigFile =
            serde_json::from_str(r#"{"cli":{"record":{"mic":"55"}}}"#).expect("a serial parses");
        assert_eq!(file.cli.record.mic.as_deref(), Some("55"));
    }

    #[test]
    fn the_record_section_carries_the_encoder_fps_and_portal() {
        let file: ConfigFile = serde_json::from_str(
            r#"{"cli":{"record":{"encoder":"hevc","fps":120,"portal":true,"mic":"alsa_input.x"}}}"#,
        )
        .expect("a record section parses");
        assert_eq!(file.cli.record.encoder.as_deref(), Some("hevc"));
        assert_eq!(file.cli.record.fps, Some(120));
        assert_eq!(file.cli.record.portal, Some(true));
        assert_eq!(file.cli.record.mic.as_deref(), Some("alsa_input.x"));

        // Absent is absent: the caller's own default is what stands.
        let bare: ConfigFile = serde_json::from_str(r#"{"cli":{"record":{}}}"#)
            .expect("an empty record section parses");
        assert!(bare.cli.record.encoder.is_none());
        assert!(bare.cli.record.fps.is_none());
        assert!(bare.cli.record.portal.is_none());
        assert!(bare.cli.record.mic.is_none());
        assert!(bare.cli.record.follow.is_none());
    }

    #[test]
    fn the_record_section_remembers_the_windows_to_follow() {
        // The follow list is the one value here that is not a scalar: the
        // settings window writes several windows at once, and a session that
        // read only the first would follow the wrong one of them.
        let file: ConfigFile =
            serde_json::from_str(r#"{"cli":{"record":{"follow":["game","chat"]}}}"#)
                .expect("a follow list parses");
        assert_eq!(
            file.cli.record.follow.as_deref(),
            Some(["game".to_owned(), "chat".to_owned()].as_slice())
        );
        // An empty list is a value the file can carry, and it means the same
        // thing as the absent key: nothing to follow.
        let empty: ConfigFile = serde_json::from_str(r#"{"cli":{"record":{"follow":[]}}}"#)
            .expect("an empty list parses");
        assert_eq!(empty.cli.record.follow.as_deref(), Some([].as_slice()));
    }

    #[test]
    fn the_replay_section_carries_its_own_follow_and_save_places() {
        let file: ConfigFile = serde_json::from_str(
            r#"{"cli":{"replay":{"follow":["game"],"save-dir":"/tmp/clips","notify":false}}}"#,
        )
        .expect("a replay section parses");
        assert_eq!(
            file.cli.replay.follow.as_deref(),
            Some(["game".to_owned()].as_slice())
        );
        assert_eq!(file.cli.replay.save_dir.as_deref(), Some("/tmp/clips"));
        assert_eq!(file.cli.replay.notify, Some(false));
        // The recording side's follow is its own field: a replay list must not
        // appear as one.
        assert!(file.cli.record.follow.is_none());
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

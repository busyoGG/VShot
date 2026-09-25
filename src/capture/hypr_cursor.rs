// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Keeps `hypr-dynamic-cursors`' software cursor out of captured frames.
//!
//! While the cursor is magnified — a shake, or the plugin's magnify dispatcher
//! — the plugin holds the compositor's software-cursor lock, so the pointer is
//! drawn into the frame the compositor composites.  That frame is the one
//! `wlr-screencopy` hands out, so the pointer lands in every capture of it,
//! `--cursor` or not.  The lock outlives the magnification on an output whose
//! hardware cursor buffer has failed once: the plugin marks it failed and never
//! tries that output again.
//!
//! Switching the plugin off is what releases the lock — the compositor then
//! goes back to its own hardware cursor, which never was the problem.  Only a
//! pointer event makes it re-evaluate that: the move below is what puts the
//! pointer back on the hardware path and repaints the rectangle the software
//! cursor had been occupying.  With the plugin off for the whole capture,
//! nothing can drag the pointer into the frames being read.

use std::process::Command;
use std::time::Duration;

use super::active_output::Session;

/// The plugin's switch, as its author names it.
const ENABLED: &str = "plugin:dynamic_cursors:enabled";

/// How long the compositor is given to finish the repaint the pointer move
/// asks for, before the first capture starts reading frames.  One frame at
/// 60 Hz is enough in practice; this is two, so a busy session has room.
const SETTLE: Duration = Duration::from_millis(32);

/// Switches the plugin off for as long as the returned value lives, when this
/// session is a Hyprland one that has it enabled.  `None` everywhere else, and
/// whenever a step fails: a capture that cannot ask for this is exactly the
/// capture that happened before this module existed.
pub(crate) fn suspend() -> Option<Suspension> {
    if Session::detect() != Session::Hyprland {
        return None;
    }
    if plugin_enabled() != Some(true) {
        return None;
    }
    // Read the position before touching anything: moving the pointer somewhere
    // else is not an option, so a position we cannot read is a reason to leave
    // the session alone.
    let (x, y) = cursor_position()?;
    if !set_enabled(false) {
        return None;
    }
    // The move is what makes the compositor drop the software cursor.  If it
    // does not go through, put the plugin back and leave the capture to what
    // it would have done anyway.
    if !move_pointer(x, y) {
        set_enabled(true);
        return None;
    }
    std::thread::sleep(SETTLE);
    Some(Suspension)
}

/// Puts the plugin back the way it was found.
///
/// Best effort by design: the plugin's own default is on, so leaving it off is
/// the worse of the two failures, and a compositor that stopped answering is
/// not something a capture can fix.
pub(crate) struct Suspension;

impl Drop for Suspension {
    fn drop(&mut self) {
        set_enabled(true);
    }
}

/// `true` only for a plugin that is loaded and switched on.  `None` means it
/// is not loaded: `hyprctl` answers "no such option" then, not a value.
fn plugin_enabled() -> Option<bool> {
    parse_plugin_enabled(&run_hyprctl(&["-j", "getoption", ENABLED])?)
}

fn parse_plugin_enabled(answer: &str) -> Option<bool> {
    let value: serde_json::Value = serde_json::from_str(answer).ok()?;
    value.get("bool")?.as_bool()
}

/// The pointer's position, in the global logical coordinates a move takes.
fn cursor_position() -> Option<(i32, i32)> {
    parse_cursor_position(&run_hyprctl(&["cursorpos"])?)
}

fn parse_cursor_position(answer: &str) -> Option<(i32, i32)> {
    let (x, y) = answer.trim().split_once(',')?;
    Some((x.trim().parse().ok()?, y.trim().parse().ok()?))
}

/// Switches the plugin on or off.  Hyprland has two spellings for this and
/// which one answers is the version's: a Lua config takes `eval`, the older
/// hyprlang ones take `keyword`.  Both are tried, the Lua one first because it
/// is the current one.
fn set_enabled(enabled: bool) -> bool {
    let script =
        format!("hl.config({{ plugin = {{ dynamic_cursors = {{ enabled = {enabled} }} }} }})");
    if run_hyprctl(&["eval", &script]).as_deref() == Some("ok") {
        return true;
    }
    let value = if enabled { "true" } else { "false" };
    run_hyprctl(&["keyword", ENABLED, value])
        .is_some_and(|answer| !answer.contains("unknown request"))
}

/// Moves the pointer to where it already is: the compositor sees a pointer
/// event, re-checks which cursor backend this output is on, and repaints.
fn move_pointer(x: i32, y: i32) -> bool {
    let script = format!("hl.dispatch(hl.dsp.cursor.move({{ x = {x}, y = {y} }}))");
    if run_hyprctl(&["eval", &script]).as_deref() == Some("ok") {
        return true;
    }
    run_hyprctl(&["dispatch", "movecursor", &x.to_string(), &y.to_string()]).is_some()
}

/// `None` on anything that is not a successful `hyprctl` run — a missing
/// binary, a non-zero exit, a compositor that is not on this display.  All of
/// them mean the same thing here: this session is not one to touch.
fn run_hyprctl(args: &[&str]) -> Option<String> {
    let output = Command::new("hyprctl").args(args).output().ok()?;
    if !output.status.success() {
        return None;
    }
    Some(String::from_utf8_lossy(&output.stdout).trim().to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_loaded_plugin_reports_its_switch() {
        // What `hyprctl -j getoption plugin:dynamic_cursors:enabled` prints.
        let on = r#"{"option": "plugin:dynamic_cursors:enabled", "bool": true, "set": true }"#;
        let off = r#"{"option": "plugin:dynamic_cursors:enabled", "bool": false, "set": false }"#;
        assert_eq!(parse_plugin_enabled(on), Some(true));
        assert_eq!(parse_plugin_enabled(off), Some(false));
    }

    #[test]
    fn an_unloaded_plugin_is_not_an_answer() {
        // `hyprctl` prints this instead of a value, and it is not JSON.
        assert_eq!(parse_plugin_enabled("no such option"), None);
    }

    #[test]
    fn the_pointer_position_comes_from_hyprctls_own_line() {
        assert_eq!(parse_cursor_position("600, 500\n"), Some((600, 500)));
        // The layout's own origin, where negative coordinates are normal.
        assert_eq!(parse_cursor_position("-1920, -80"), Some((-1920, -80)));
        assert_eq!(parse_cursor_position("not a position"), None);
        assert_eq!(parse_cursor_position("600"), None);
    }
}

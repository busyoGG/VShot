// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! The process id behind a window, for per-application audio capture.
//!
//! `record --app-audio` records the sound one application makes, and the way
//! to reach "that application" is its process: PipeWire tags every playback
//! stream with `application.process.id`, which is the pid of the process that
//! opened it.  So the chain is `window → pid → PipeWire node`, and this module
//! is the first hop.
//!
//! A window's pid is not part of the Wayland protocols vshot records through.
//! `ext_foreign_toplevel_handle_v1` — the one description every compositor
//! shares, and the one a window capture is built on — carries only `app_id`,
//! `title` and an opaque `identifier`; there is no process field anywhere in
//! it, and no protocol vshot binds has one.  The pid comes from the
//! compositor's own IPC instead, which is why this is a per-compositor affair:
//!
//! * **Hyprland** answers `hyprctl clients -j` with a `pid` per client.
//! * **niri** answers `niri msg --json focused-window` / `pick-window` with a
//!   `pid` on the window object.
//! * **KWin** has none of this on the scripting interface vshot already uses
//!   for geometry, and the D-Bus screen-shot service returns pixels only.
//! * **Sway** would answer `swaymsg -t get_tree`, but a Sway session has no
//!   `ext_foreign_toplevel` either, so it is out of scope for window capture
//!   altogether.
//!
//! The lookup is keyed on the same description the toplevel list uses (`app_id`
//! and `title`), so the window the pid belongs to is the window the recording
//! is of.  Where no compositor can answer, [`pid_for`] returns `None` and the
//! caller says so rather than recording the wrong application's sound.

use serde_json::Value;

use crate::error::{Result, VshotError};

use super::active_output::Session;
use super::window::{join_label, ProcessWindowRunner, WindowCommand, WindowCommandRunner};
use super::window_copy::Name;

/// The pid of the process that owns the window described by `name`, or `None`
/// when this session's compositor cannot say.
///
/// `name` is the window's entry in the foreign-toplevel list — the same one
/// the capture is built on — so the window this answers for is the window
/// being recorded.
pub fn pid_for(name: &Name) -> Result<Option<i32>> {
    pid_for_session(Session::detect(), &ProcessWindowRunner, name)
}

fn pid_for_session<R: WindowCommandRunner>(
    session: Session,
    runner: &R,
    name: &Name,
) -> Result<Option<i32>> {
    match session {
        Session::Hyprland => hyprland_pid(runner, name),
        Session::Niri => niri_pid(runner, name),
        // Sway has no ext_foreign_toplevel, so a window recording never runs
        // on it; KWin's scripting interface reports geometry only.  Neither
        // is an error here — the caller falls back to the microphone or to
        // silence, and says which.
        _ => Ok(None),
    }
}

/// Hyprland: `hyprctl clients -j` lists every client with its `pid`, `class`
/// and `title`.  The window is matched the way the recording matched it — by
/// app id, then title — and among several clients that share both (two windows
/// of one application with the same title) the first is taken; the alternative
/// is refusing, which would leave `--app-audio` unusable on a desktop that
/// happens to have two.
fn hyprland_pid<R: WindowCommandRunner>(runner: &R, name: &Name) -> Result<Option<i32>> {
    let command = WindowCommand::hyprland_clients();
    let output = runner.run(&command)?;
    if !output.status.success() {
        return Err(VshotError::Recording(format!(
            "`hyprctl clients` exited with {}",
            output.status
        )));
    }
    Ok(parse_hyprland_pid(&output.stdout, name))
}

fn parse_hyprland_pid(bytes: &[u8], name: &Name) -> Option<i32> {
    let value: Value = serde_json::from_slice(bytes).ok()?;
    let clients = value.as_array()?;
    // Two passes: an exact class+title match first, then title alone (the
    // same preference `window_copy::match_description` uses), so a window
    // whose class the compositor spelled differently still resolves.
    let class_of = |client: &Value| {
        client
            .get("class")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_owned()
    };
    let title_of = |client: &Value| {
        client
            .get("title")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_owned()
    };
    let pid_of = |client: &Value| client.get("pid").and_then(Value::as_i64);

    let exact = clients
        .iter()
        .find(|client| class_of(client) == name.app_id && title_of(client) == name.title);
    let chosen = exact.or_else(|| {
        clients
            .iter()
            .find(|client| title_of(client) == name.title && !name.title.is_empty())
    });
    chosen
        .and_then(pid_of)
        .and_then(|pid| i32::try_from(pid).ok())
}

/// niri: `niri msg --json focused-window` names the focused window (with its
/// pid); `niri msg --json pick-window` lets the user point at one.  A window
/// recording resolved its target already, so the pid is read from whichever of
/// the two matches that target's app id and title.
fn niri_pid<R: WindowCommandRunner>(runner: &R, name: &Name) -> Result<Option<i32>> {
    // The focused window answers the common case (`record window active`).
    let focused = run_json(runner, &["msg", "--json", "focused-window"])?;
    if let Some(pid) = focused.as_ref().and_then(|value| niri_pid_of(value, name)) {
        return Ok(Some(pid));
    }
    // Otherwise ask niri for its window list and match by app id and title —
    // `niri msg --json windows` is one entry per window, each with a pid.
    let windows = run_json(runner, &["msg", "--json", "windows"])?;
    let Some(list) = windows.as_ref().and_then(Value::as_array) else {
        return Ok(None);
    };
    let class_of = |value: &Value| {
        value
            .get("app_id")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_owned()
    };
    let title_of = |value: &Value| {
        value
            .get("title")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_owned()
    };
    let exact = list
        .iter()
        .find(|value| class_of(value) == name.app_id && title_of(value) == name.title);
    let chosen = exact.or_else(|| {
        list.iter()
            .find(|value| title_of(value) == name.title && !name.title.is_empty())
    });
    Ok(chosen
        .and_then(|value| value.get("pid").and_then(Value::as_i64))
        .and_then(|pid| i32::try_from(pid).ok()))
}

/// One niri window object's pid, when the object is the window `name` names.
fn niri_pid_of(value: &Value, name: &Name) -> Option<i32> {
    let app_id = value.get("app_id").and_then(Value::as_str).unwrap_or("");
    let title = value.get("title").and_then(Value::as_str).unwrap_or("");
    // A focused window matches on title alone when the app id is unset; when
    // both are known they both have to agree, which is what keeps a second
    // window of the same application from being mistaken for the target.
    let matches = if name.app_id.is_empty() {
        !name.title.is_empty() && title == name.title
    } else {
        app_id == name.app_id && title == name.title
    };
    if !matches {
        return None;
    }
    value
        .get("pid")
        .and_then(Value::as_i64)
        .and_then(|pid| i32::try_from(pid).ok())
}

fn run_json<R: WindowCommandRunner>(runner: &R, args: &[&str]) -> Result<Option<Value>> {
    let command = WindowCommand {
        program: std::ffi::OsString::from("niri"),
        args: args.iter().map(std::ffi::OsString::from).collect(),
    };
    let output = runner.run(&command)?;
    if !output.status.success() {
        // A niri that does not answer this subcommand (an older release) is
        // not a failure of the recording; it just cannot give a pid.
        return Ok(None);
    }
    Ok(serde_json::from_slice(&output.stdout).ok())
}

/// The label a compositor's window list shows for the window `name` names,
/// for a diagnostic that has to point at the window the pid could not be
/// found for.
pub fn describe(name: &Name) -> String {
    let label = join_label(&name.app_id, &name.title);
    if label.is_empty() {
        "(the compositor reports no app id or title)".to_owned()
    } else {
        label
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::collections::HashMap;
    use std::os::unix::process::ExitStatusExt;
    use std::process::{ExitStatus, Output};

    /// A runner that answers from a fixed table, keyed by the command line.
    struct ScriptedRunner {
        replies: RefCell<HashMap<String, Vec<u8>>>,
    }

    impl ScriptedRunner {
        fn new() -> Self {
            Self {
                replies: RefCell::new(HashMap::new()),
            }
        }

        fn replying(self, args: &str, bytes: &[u8]) -> Self {
            self.replies
                .borrow_mut()
                .insert(args.to_owned(), bytes.to_vec());
            self
        }
    }

    impl WindowCommandRunner for ScriptedRunner {
        fn run(&self, command: &WindowCommand) -> Result<Output> {
            let key = command
                .args
                .iter()
                .map(|arg| arg.to_string_lossy().into_owned())
                .collect::<Vec<_>>()
                .join(" ");
            match self.replies.borrow().get(&key) {
                Some(bytes) => Ok(Output {
                    status: ExitStatus::from_raw(0),
                    stdout: bytes.clone(),
                    stderr: Vec::new(),
                }),
                None => Ok(Output {
                    status: ExitStatus::from_raw(256),
                    stdout: Vec::new(),
                    stderr: b"not scripted".to_vec(),
                }),
            }
        }
    }

    fn name(app_id: &str, title: &str) -> Name {
        Name {
            app_id: app_id.to_owned(),
            title: title.to_owned(),
            identifier: String::new(),
        }
    }

    const CLIENTS: &str = r#"[
        {"class":"kitty","title":"a","pid":111,"at":[0,0],"size":[100,100]},
        {"class":"vivaldi-stable","title":"News","pid":222,"at":[0,0],"size":[100,100]},
        {"class":"vivaldi-stable","title":"Mail","pid":333,"at":[0,0],"size":[100,100]}
    ]"#;

    #[test]
    fn hyprland_matches_on_class_and_title() {
        let runner = ScriptedRunner::new().replying("clients -j", CLIENTS.as_bytes());
        let found = parse_hyprland_pid(CLIENTS.as_bytes(), &name("vivaldi-stable", "Mail"));
        assert_eq!(found, Some(333));
        assert_eq!(
            parse_hyprland_pid(CLIENTS.as_bytes(), &name("kitty", "a")),
            Some(111)
        );
        let _ = runner;
    }

    #[test]
    fn hyprland_falls_back_to_title_when_the_class_differs() {
        // The compositor can report a class the toplevel list spells
        // differently; the title is the second chance.
        let found = parse_hyprland_pid(CLIENTS.as_bytes(), &name("other", "News"));
        assert_eq!(found, Some(222));
    }

    #[test]
    fn hyprland_returns_none_when_no_window_matches() {
        assert_eq!(
            parse_hyprland_pid(CLIENTS.as_bytes(), &name("x", "y")),
            None
        );
        assert_eq!(parse_hyprland_pid(b"[]", &name("x", "y")), None);
        assert_eq!(parse_hyprland_pid(b"not json", &name("x", "y")), None);
    }

    const FOCUSED: &str = r#"{
        "id": 12, "title": "t w", "app_id": "Alacritty", "pid": 4242,
        "workspace_id": 6, "is_focused": true, "is_floating": false,
        "layout": {"tile_size": [800.0, 600.0]}
    }"#;

    const NIRI_WINDOWS: &str = r#"[
        {"id": 12, "title": "t w", "app_id": "Alacritty", "pid": 4242},
        {"id": 13, "title": "browser", "app_id": "firefox", "pid": 9000}
    ]"#;

    #[test]
    fn niri_reads_the_focused_windows_pid() {
        let runner = ScriptedRunner::new()
            .replying("msg --json focused-window", FOCUSED.as_bytes())
            .replying("msg --json windows", NIRI_WINDOWS.as_bytes());
        let pid = niri_pid(&runner, &name("Alacritty", "t w")).expect("a lookup");
        assert_eq!(pid, Some(4242));
    }

    #[test]
    fn niri_falls_back_to_the_window_list_for_an_unfocused_window() {
        let runner = ScriptedRunner::new()
            .replying("msg --json focused-window", FOCUSED.as_bytes())
            .replying("msg --json windows", NIRI_WINDOWS.as_bytes());
        let pid = niri_pid(&runner, &name("firefox", "browser")).expect("a lookup");
        assert_eq!(pid, Some(9000));
    }

    #[test]
    fn niri_says_nothing_when_no_window_matches() {
        let runner = ScriptedRunner::new()
            .replying("msg --json focused-window", FOCUSED.as_bytes())
            .replying("msg --json windows", NIRI_WINDOWS.as_bytes());
        let pid = niri_pid(&runner, &name("nobody", "nothing")).expect("a lookup");
        assert_eq!(pid, None);
    }

    #[test]
    fn a_session_without_a_pid_source_answers_none() {
        // KWin and Sway have no pid to give; that is a `None`, not an error.
        let runner = ScriptedRunner::new();
        let pid = pid_for_session(Session::KWin, &runner, &name("a", "b")).expect("no error");
        assert_eq!(pid, None);
    }

    #[test]
    fn describe_names_the_window_for_a_diagnostic() {
        assert_eq!(describe(&name("kitty", "a shell")), "kitty — a shell");
        assert!(!describe(&name("", "")).is_empty());
    }
}

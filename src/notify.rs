// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Desktop notifications, for the one result that arrives with nothing on
//! screen to show for it.
//!
//! The text recognizer is the only caller.  `vshot ocr` puts its answer on the
//! clipboard or on stdout, and a keybinding shows neither, so a recognition
//! that finished, one that failed, and one that never ran all look the same
//! until the user pastes.  A notification is what tells them apart.
//!
//! It goes over the session bus to whatever daemon owns
//! `org.freedesktop.Notifications` — the desktop's own, not ours — which is why
//! nothing here can fail the command: the text is already where it was asked to
//! go, and a session with no notification daemon is a complete session that
//! gets the text without the note.  A failure is reported on stderr, where a
//! run from a terminal can act on it and a keybinding loses it.

use std::collections::HashMap;

use zbus::blocking::{Connection, Proxy};
use zbus::zvariant::Value;

use crate::config;

/// The name the desktop knows vshot by, and the icon to draw it with.  Both are
/// the strings the installed desktop file and `vshot.svg` carry, so a daemon
/// showing "which application is this" finds the same program the notification
/// came from.
const APP_NAME: &str = "vshot";
const APP_ICON: &str = "vshot";

const SERVICE: &str = "org.freedesktop.Notifications";
const PATH: &str = "/org/freedesktop/Notifications";
const INTERFACE: &str = "org.freedesktop.Notifications";

/// How long a notification stays up, in milliseconds: long enough to read a
/// short line of recognized text, short enough not to have to be dismissed.
const EXPIRE_MS: i32 = 4000;

/// The most text a notification carries.  The clipboard holds the whole thing;
/// this is the glance that says it looks like what was asked for.
const BODY_LIMIT: usize = 160;

/// Says that the text is ready, having just been sent to `destination`.
pub fn ocr_finished(text: &str, to_clipboard: bool) {
    let chinese = crate::cli_i18n::prefers_chinese();
    if text.trim().is_empty() {
        // An empty result is a result: saying "finished" over an unchanged
        // clipboard would leave the user pasting whatever was there before.
        send(
            if chinese {
                "没识别到文字"
            } else {
                "No text found"
            },
            "",
        );
        return;
    }
    // The body is a preview rather than the text itself, so the summary is
    // where the destination is stated -- and it is the half that matters when
    // the body is cut short.
    let summary = match (chinese, to_clipboard) {
        (true, true) => "已复制到剪贴板",
        (true, false) => "取字完成",
        (false, true) => "Copied to the clipboard",
        (false, false) => "Text recognized",
    };
    send(summary, &preview(text));
}

/// Says that the recognition itself failed, with the reason it gives.
pub fn ocr_failed(reason: &str) {
    let chinese = crate::cli_i18n::prefers_chinese();
    send(
        if chinese {
            "取字失败"
        } else {
            "Text recognition failed"
        },
        &preview(reason),
    );
}

/// Says that a recording was written, with its size in frames and seconds.
///
/// A recording runs from a keybinding and ends on a signal, so nothing on
/// screen marks the moment it stopped or where the file went; the
/// notification is the receipt.  It is not part of the result — a session
/// with no notification daemon still gets its file — so a failure to send
/// only reaches stderr.
pub fn recording_finished(path: &std::path::Path, frames: usize, seconds: f64) {
    let chinese = crate::cli_i18n::prefers_chinese();
    let summary = if chinese {
        "录制完成"
    } else {
        "Recording saved"
    };
    let body = format!("{} — {} frames, {:.1}s", path.display(), frames, seconds);
    send(summary, &preview(&body));
}

/// Says that a recording could not start or stopped short, with the reason.
pub fn recording_failed(reason: &str) {
    let chinese = crate::cli_i18n::prefers_chinese();
    send(
        if chinese {
            "录制失败"
        } else {
            "Recording failed"
        },
        &preview(reason),
    );
}

/// Sends a notification, unless the user turned them off (`cli.ocr.notify`).
fn send(summary: &str, body: &str) {
    if !notifications_enabled() {
        return;
    }
    if let Err(error) = post(summary, body) {
        eprintln!("vshot: could not send a notification: {error}");
    }
}

/// Whether the config file asks for notifications.  Absent means yes: the
/// notification is the point of the feature, and it is one switch away from
/// off in the settings window.
fn notifications_enabled() -> bool {
    config::load().ocr.notify.unwrap_or(true)
}

fn post(summary: &str, body: &str) -> zbus::Result<()> {
    let connection = Connection::session()?;
    let proxy = Proxy::new(&connection, SERVICE, PATH, INTERFACE)?;
    // No actions and no hints: the daemon's defaults are what the rest of the
    // desktop already shows, and vshot has nothing to add to them.
    let hints: HashMap<&str, Value<'_>> = HashMap::new();
    let _: u32 = proxy.call(
        "Notify",
        &(
            APP_NAME,
            0u32,
            APP_ICON,
            summary,
            body,
            Vec::<&str>::new(),
            hints,
            EXPIRE_MS,
        ),
    )?;
    Ok(())
}

/// One line out of any text, cut to what a notification can show.
///
/// Recognized text arrives with a newline between its lines and a reason
/// arrives as one sentence; a daemon draws whatever it is handed, so the lines
/// are folded into one and the whole is cut to [`BODY_LIMIT`] — by characters,
/// which is what a reader counts, not bytes.
fn preview(text: &str) -> String {
    let folded = text.split_whitespace().collect::<Vec<_>>().join(" ");
    if folded.chars().count() <= BODY_LIMIT {
        return folded;
    }
    let mut cut: String = folded.chars().take(BODY_LIMIT).collect();
    cut.push('…');
    cut
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_preview_folds_the_lines_into_one() {
        assert_eq!(preview("first\nsecond\n\nthird"), "first second third");
    }

    #[test]
    fn a_preview_is_cut_by_characters_not_bytes() {
        // Chinese text is three bytes a character: a byte-wise cut would split
        // one in half and hand the daemon a string that is not valid UTF-8.
        let long = "好".repeat(BODY_LIMIT + 10);
        let cut = preview(&long);
        assert_eq!(cut.chars().count(), BODY_LIMIT + 1);
        assert!(cut.ends_with('…'), "{cut}");
        assert_eq!(
            cut.chars().filter(|character| *character == '好').count(),
            BODY_LIMIT
        );
    }

    #[test]
    fn a_short_preview_is_left_alone() {
        assert_eq!(preview("HELLO WORLD"), "HELLO WORLD");
        assert_eq!(preview(""), "");
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

use std::path::{Path, PathBuf};

use thiserror::Error;

#[derive(Debug, Error)]
pub enum VshotError {
    #[error("invalid geometry: {0}")]
    InvalidGeometry(String),
    #[error("invalid output destination: {0}")]
    InvalidDestination(String),
    #[error(
        "no output destination supplied; use --output PATH, --output -, --clipboard, or --pin"
    )]
    MissingDestination,
    #[error("--geometry and interactive region selection are mutually exclusive")]
    ConflictingRegionSelection,
    #[error("failed to execute `{program}`: {source}")]
    CommandIo {
        program: String,
        #[source]
        source: std::io::Error,
    },
    #[error("failed to decode PNG from {origin}: {message}")]
    PngDecode { origin: String, message: String },
    #[error("failed to encode PNG: {0}")]
    PngEncode(String),
    #[error("failed to write {path}: {source}")]
    WriteFile {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("Wayland connection failed: {0}")]
    WaylandConnection(String),
    #[error("Wayland capability `{0}` is required for strict freeze mode")]
    MissingCapability(String),
    #[error("Wayland protocol error: {0}")]
    WaylandProtocol(String),
    /// No capture backend can serve the request.  The message is a complete
    /// sentence because both backends' diagnoses are composed into it.
    #[error("no usable screen capture backend: {0}")]
    NoCaptureBackend(String),
    #[error("KWin ScreenShot2 failed: {0}")]
    KwinScreenShot(String),
    #[error("niri's window screenshot failed: {0}")]
    NiriScreenshot(String),
    #[error("KDE denied the screenshot: {0}")]
    ScreenshotDenied(String),
    #[error("screen capture timed out before the compositor returned a frame")]
    CaptureTimeout,
    #[error("frozen overlay was not ready before the timeout")]
    OverlayTimeout,
    #[error("current monitor could not be determined from pointer input before the timeout")]
    CurrentOutputTimeout,
    #[error("output topology is incomplete: {0}")]
    IncompleteTopology(String),
    #[error("output topology changed while taking the screenshot")]
    TopologyChanged,
    #[error("unsupported output mapping: {0}")]
    UnsupportedOutput(String),
    #[error("unsupported text: {0}")]
    UnsupportedText(String),
    #[error("active window is unavailable: {0}")]
    ActiveWindowUnavailable(String),
    /// Interactive window picking found nothing to offer: no compositor
    /// reported any window and the pixel fallback saw no window either.
    #[error("no window could be picked: {0}")]
    WindowPickUnavailable(String),
    #[error("interactive selection cancelled")]
    SelectionCancelled,
    #[error("interactive selection failed: {0}")]
    Selection(String),
    #[error("Wayland clipboard failed: {0}")]
    Clipboard(String),
    #[error("pin failed: {0}")]
    Pin(String),
    #[error("text recognition failed: {0}")]
    Ocr(String),
    /// Scrolling capture: the frames could not be stitched, or what was asked
    /// of the stitcher makes no sense (a region too small, an ignore-top that
    /// covers the whole frame).
    #[error("long screenshot failed: {0}")]
    LongShotStitch(String),
    /// Scrolling capture: no way to drive the wheel on this compositor.
    #[error("long screenshot cannot scroll the page: {0}")]
    LongShotInjection(String),
    /// Scrolling capture: the selection is not usable for a long screenshot.
    #[error("long screenshot region is unusable: {0}")]
    LongShotRegion(String),
    /// Screen recording or replay: the encoder, the frame loop, the muxer or
    /// the control channel failed.  The message is a complete sentence because
    /// the causes span several layers (a missing VAAPI driver, a rejected
    /// surface size, a write that could not keep up, no session to talk to).
    #[error("recording failed: {0}")]
    Recording(String),
    /// A complete sentence that needs no prefix: a replay session's own error
    /// relayed to the client that triggered it is already worded for the user,
    /// so wrapping it again would read as "recording failed: recording
    /// failed: ...".
    #[error("{0}")]
    Bare(String),
}

pub type Result<T> = std::result::Result<T, VshotError>;

impl VshotError {
    /// Whether the failure is about a Wayland compositor — no connection, a
    /// protocol that is missing, a capture that was refused.  Those are the
    /// errors that say nothing without knowing *which* compositor was asked,
    /// so they are the ones that get [`wayland_display_note`] appended.
    pub fn is_display_failure(&self) -> bool {
        matches!(
            self,
            Self::WaylandConnection(_)
                | Self::WaylandProtocol(_)
                | Self::MissingCapability(_)
                | Self::NoCaptureBackend(_)
                | Self::KwinScreenShot(_)
                | Self::ScreenshotDenied(_)
        )
    }
}

/// Names the Wayland display this process is on, for an error that cannot be
/// read without it.
///
/// `libwayland` falls back to the name `wayland-0` when `WAYLAND_DISPLAY` is
/// unset, and a plain tty or ssh shell has no such variable: those runs land on
/// whatever holds the default name.  One runtime directory can hold several
/// compositors at once — a nested KDE started from a tty takes `wayland-0`
/// while the session's own Hyprland sits on `wayland-1` — so "`seat pointer` is
/// required" is an unreadable complaint when the display being asked is not
/// even the one on screen.  `None` when there is nothing to add: the single
/// display that exists is the one we used.
pub fn wayland_display_note() -> Option<String> {
    display_note(
        std::env::var("WAYLAND_DISPLAY").ok().as_deref(),
        std::env::var_os("XDG_RUNTIME_DIR")
            .map(PathBuf::from)
            .as_deref(),
    )
}

fn display_note(display: Option<&str>, runtime: Option<&Path>) -> Option<String> {
    let names: Vec<String> = std::fs::read_dir(runtime?)
        .ok()?
        .flatten()
        .filter_map(|entry| entry.file_name().into_string().ok())
        .filter(|name| name.starts_with("wayland-") && !name.ends_with(".lock"))
        .collect();
    let current = display.unwrap_or("wayland-0");
    let exists = names.iter().any(|name| name == current);
    let mut others: Vec<&str> = names
        .iter()
        .filter(|name| name.as_str() != current)
        .map(String::as_str)
        .collect();
    others.sort_unstable();
    if exists && others.is_empty() {
        return None;
    }
    let mut note = match display {
        Some(name) => format!("this run is on Wayland display `{name}`"),
        None => format!("`WAYLAND_DISPLAY` is unset, so libwayland used the default `{current}`"),
    };
    if !exists {
        note.push_str(", which does not exist");
    }
    if !others.is_empty() {
        note.push_str("; other displays on this machine: ");
        note.push_str(
            &others
                .iter()
                .map(|name| format!("`{name}`"))
                .collect::<Vec<_>>()
                .join(", "),
        );
    }
    Some(note)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn runtime_with(names: &[&str]) -> tempfile::TempDir {
        let directory = tempfile::tempdir().expect("temp dir");
        for name in names {
            std::fs::write(directory.path().join(name), b"").expect("socket stand-in");
        }
        directory
    }

    #[test]
    fn the_note_names_the_display_and_its_neighbours() {
        let runtime = runtime_with(&["wayland-0", "wayland-0.lock", "wayland-1"]);
        let note = display_note(Some("wayland-0"), Some(runtime.path())).expect("a note");
        assert!(note.contains("`wayland-0`"), "{note}");
        assert!(note.contains("`wayland-1`"), "{note}");
        assert!(!note.contains("does not exist"), "{note}");
    }

    #[test]
    fn an_unset_display_is_reported_as_the_default_that_was_used() {
        let runtime = runtime_with(&["wayland-1"]);
        let note = display_note(None, Some(runtime.path())).expect("a note");
        assert!(note.contains("WAYLAND_DISPLAY"), "{note}");
        assert!(note.contains("default `wayland-0`"), "{note}");
        assert!(note.contains("does not exist"), "{note}");
        assert!(note.contains("`wayland-1`"), "{note}");
    }

    #[test]
    fn a_missing_display_is_named_even_when_it_is_the_only_one() {
        let runtime = runtime_with(&[]);
        let note = display_note(Some("wayland-1"), Some(runtime.path())).expect("a note");
        assert!(
            note.contains("`wayland-1`") && note.contains("does not exist"),
            "{note}"
        );
    }

    #[test]
    fn the_only_display_needs_no_note() {
        let runtime = runtime_with(&["wayland-0"]);
        assert!(display_note(Some("wayland-0"), Some(runtime.path())).is_none());
        assert!(display_note(Some("wayland-0"), None).is_none());
    }
}

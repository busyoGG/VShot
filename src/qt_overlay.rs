// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

use std::fs::OpenOptions;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};

use serde::{Deserialize, Serialize};
use tempfile::TempDir;

use crate::capture::WindowCandidate;
use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect};
use crate::model::SceneSnapshot;
use crate::wayland::topology::OutputInfo;

const MAX_HELPER_ERROR_BYTES: usize = 512;
const HELPER_NAME: &str = "vshot-qt-ui";

/// Where the helper may live relative to the `vshot` executable: next to it
/// (installed layouts) or in `build-qt/` one and two levels up (in-tree
/// `cargo build` + `cmake -B build-qt` development layouts).
fn helper_candidates(exe_directory: Option<&Path>) -> Vec<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(directory) = exe_directory {
        candidates.push(directory.join(HELPER_NAME));
        candidates.push(directory.join("..").join("build-qt").join(HELPER_NAME));
        candidates.push(
            directory
                .join("..")
                .join("..")
                .join("build-qt")
                .join(HELPER_NAME),
        );
    }
    candidates
}

pub(crate) struct HelperLookup {
    pub(crate) path: PathBuf,
    pub(crate) searched: Vec<String>,
}

pub(crate) fn helper_program() -> Result<HelperLookup> {
    if let Some(path) = std::env::var_os("VSHOT_QT_HELPER") {
        if path.is_empty() {
            return Err(VshotError::Selection(
                "VSHOT_QT_HELPER is set but empty".into(),
            ));
        }
        let path = PathBuf::from(path);
        if !path.is_file() {
            return Err(VshotError::Selection(format!(
                "VSHOT_QT_HELPER points at missing executable `{}`",
                path.display()
            )));
        }
        return Ok(HelperLookup {
            path,
            searched: Vec::new(),
        });
    }

    let exe_directory = std::env::current_exe()
        .ok()
        .and_then(|executable| executable.parent().map(|directory| directory.to_path_buf()));
    let mut searched = Vec::new();
    for candidate in helper_candidates(exe_directory.as_deref()) {
        if candidate.is_file() {
            return Ok(HelperLookup {
                path: candidate,
                searched,
            });
        }
        searched.push(candidate.display().to_string());
    }
    Ok(HelperLookup {
        path: PathBuf::from(HELPER_NAME),
        searched,
    })
}

fn helper_spawn_error(helper: &HelperLookup, error: std::io::Error) -> VshotError {
    if error.kind() == std::io::ErrorKind::NotFound {
        let searched = if helper.searched.is_empty() {
            String::new()
        } else {
            format!(" (searched {})", helper.searched.join(", "))
        };
        VshotError::Selection(format!(
            "Qt helper `{}` was not found next to vshot, in `build-qt/`, or on \
             PATH{searched}; build it with `cmake -S . -B build-qt && cmake --build \
             build-qt` or point VSHOT_QT_HELPER at the executable",
            helper.path.display()
        ))
    } else {
        VshotError::Selection(format!(
            "failed to start Qt helper `{}`: {error}",
            helper.path.display()
        ))
    }
}

fn helper_exit_error(status: std::process::ExitStatus, stderr: &[u8]) -> VshotError {
    let detail = compact_error(stderr);
    VshotError::Selection(if detail.is_empty() {
        format!("Qt helper exited with {status}")
    } else {
        format!("Qt helper exited with {status}: {detail}")
    })
}

/// Runs the Qt helper's settings window and waits for it to be closed.
///
/// The window is an ordinary toplevel with no session behind it: it reads and
/// writes the shared config file by itself.  Its stdio is inherited rather than
/// piped, because nothing here consumes a result and a failure worth reading
/// (no compositor, no config directory) lands on stderr while the window is
/// still open.
pub(crate) fn run_settings() -> Result<()> {
    let helper = helper_program()?;
    let status = Command::new(&helper.path)
        .arg("--settings")
        .stdin(Stdio::null())
        .spawn()
        .map_err(|error| helper_spawn_error(&helper, error))?
        .wait()
        .map_err(|error| {
            VshotError::Selection(format!(
                "failed to wait for Qt helper `{}`: {error}",
                helper.path.display()
            ))
        })?;
    if !status.success() {
        return Err(VshotError::Selection(format!(
            "the settings window exited with {status}"
        )));
    }
    Ok(())
}

/// What a helper run handed back: its result JSON, and the rendered capture
/// when it sent one.
pub(crate) struct HelperOutput {
    pub(crate) json: Vec<u8>,
    pub(crate) composite: Option<RenderedCapture>,
}

/// The helper's render: the capture with the marks on it, and the marks alone.
///
/// Two images rather than one because the HDR half cannot take a flattened
/// picture -- it is opaque everywhere, so compositing it would replace the light
/// instead of marking it.  The layer is what goes onto the HDR half; the
/// flattened one is the SDR result verbatim.
pub(crate) struct RenderedCapture {
    pub(crate) composite: crate::model::Frame,
    pub(crate) marks: crate::model::Frame,
}

/// What a caller does when the editor asks to be let go: the session's result
/// JSON and the capture it rendered, for the caller to put where it belongs
/// before the editor's surface comes down.  See [`HelperRequest::Release`].
///
/// The render is borrowed rather than moved: a caller whose destination the
/// command line named still has to write it after the handoff has put the pin
/// on the screen, and the two need the same pixels.
pub(crate) type ReleaseHandler<'a> =
    dyn FnMut(&[u8], Option<&RenderedCapture>) -> Result<String> + 'a;

/// A request the helper makes of the CLI while a session is open, as one line
/// of JSON on the helper's stdout.  Anything that is not one of these is the
/// session's own result, which ends the dialogue.
#[derive(Debug, Deserialize)]
#[serde(tag = "request", rename_all = "lowercase")]
enum HelperRequest {
    /// The picker wants the windows it should highlight, again.
    Candidates,
    /// The keyboard walked its cursor and wants the real pointer moved there,
    /// in global logical pixels.
    Pointer { x: i32, y: i32 },
    /// The pin editor has drawn everything it is going to draw and is asking to
    /// be let go.  It does not stop on its own when its work is finished: it
    /// keeps its surface mapped -- showing the same picture the pin is about to
    /// show -- until this is answered, which happens only after the pin daemon
    /// has said the pin's own frame is on the screen.  Without the wait the two
    /// pictures would not overlap and the marks would blink out between them.
    ///
    /// The CLI answers with the pin's answer once it has one, so the editor
    /// exits knowing the handoff landed; see `run_helper_dialogue`.
    Release,
}

/// Runs the Qt helper for an interactive session — one that may ask the CLI for
/// things while it is open — and collects its answer.
///
/// The helper talks back on its stdout, one JSON object per line: a request
/// (see [`HelperRequest`]) or, at the end, the session's own result.  The CLI
/// answers on the helper's stdin.  Only the sessions that run against a live
/// desktop need this — picking, which re-reads the window list as the pointer
/// travels, and any session whose keyboard cursor walks the real pointer — but
/// they all come through here, so a request added later needs no second runner.
fn run_interactive_helper(
    helper: &HelperLookup,
    session_path: &Path,
    scene: &SceneSnapshot,
    refresh: &impl Fn() -> Option<Vec<WindowCandidate>>,
    // Run when the helper asks to be let go; see [`HelperRequest::Release`].
    // The caller's own handoff, because it is the caller that knows what the
    // editor was drawing over and what has to be on the screen before it stops.
    release: &mut ReleaseHandler<'_>,
) -> Result<HelperOutput> {
    let frames: Vec<&crate::model::Frame> =
        scene.outputs().iter().map(|output| &output.frame).collect();
    run_helper_dialogue(
        helper,
        "--session",
        session_path,
        &frames,
        Some(scene.bounds()),
        refresh,
        release,
    )
}

/// One helper session, start to answer.
///
/// `sources` are the frames the helper reads before it starts — one per output
/// for a capture, the pinned image alone for the pin editor — and they travel
/// over the pixel channel rather than as files beside the session, because they
/// are the bulk of it.  `desktop` is the logical rect a pointer request is
/// expressed in, and `refresh` is what the picker re-reads as its pointer
/// travels.  `release` is run when the helper asks to be let go and its answer
/// is what the helper is told; see [`HelperRequest::Release`].
fn run_helper_dialogue(
    helper: &HelperLookup,
    flag: &str,
    session_path: &Path,
    sources: &[&crate::model::Frame],
    desktop: Option<crate::geometry::Rect>,
    refresh: &dyn Fn() -> Option<Vec<WindowCandidate>>,
    release: &mut ReleaseHandler<'_>,
) -> Result<HelperOutput> {
    let (parent, child_end) = crate::pixel_fd::PixelChannel::spawn_pair()?;
    let mut command = Command::new(&helper.path);
    command
        .arg(flag)
        .arg(session_path)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    crate::pixel_fd::with_channel(&mut command, &child_end);
    command.env(
        crate::pixel_fd::CHANNEL_ENV,
        crate::pixel_fd::CHILD_FD.to_string(),
    );
    let mut child = command
        .spawn()
        .map_err(|error| helper_spawn_error(helper, error))?;
    child_end.close();
    send_source_frames(&parent, sources)?;
    let mut requests = child
        .stdin
        .take()
        .ok_or_else(|| VshotError::Selection("the Qt helper has no request pipe".into()))?;
    let responses = child
        .stdout
        .take()
        .ok_or_else(|| VshotError::Selection("the Qt helper has no answer pipe".into()))?;
    let mut responses = BufReader::new(responses);

    // The pointer mover is opened lazily, on the first request that needs one:
    // a session where the user never touches the keyboard cursor must not pay
    // for a virtual-pointer connection, and on a compositor with no injection
    // backend at all the walk simply moves the editor's own cursor.
    let mut pointer: Option<crate::inject::Injector> = None;
    let mut json: Option<Vec<u8>> = None;
    let mut composite: Option<RenderedCapture> = None;
    let mut handoff: Option<VshotError> = None;
    let mut line = String::new();
    loop {
        line.clear();
        let read = responses.read_line(&mut line).map_err(|error| {
            VshotError::Selection(format!("failed to read the Qt helper's answer: {error}"))
        })?;
        if read == 0 {
            // The helper went away.  That is how every session ends: the ones
            // with nothing to hand over write their result and exit, and a pin
            // edit closes its stdout once the release below has been answered.
            break;
        }
        let text = line.trim();
        if text.is_empty() {
            continue;
        }
        let request: HelperRequest = match serde_json::from_str(text) {
            Ok(request) => request,
            // Not a request: the session's own result.  It is not the end of
            // the dialogue any more -- an editor writes it and then asks to be
            // let go -- so it is kept and the loop goes on.
            Err(_) => {
                let bytes = text.as_bytes().to_vec();
                // Read the render now rather than after the loop: the helper
                // sends the pixels before this line, and the handoff below
                // needs them while the helper is still on the screen.  A
                // session whose caller has already taken them off the channel
                // has nothing left here -- the pixels travel once.
                if composite.is_none() && json.is_none() {
                    composite = read_composite(&bytes, &parent)?;
                }
                json = Some(bytes);
                continue;
            }
        };
        let reply = match request {
            HelperRequest::Candidates => {
                let reply = CandidateReply {
                    candidates: refresh()
                        .map(|candidates| candidates.iter().map(QtCandidate::from).collect()),
                };
                serde_json::to_string(&reply).map_err(|error| {
                    VshotError::Selection(format!("failed to encode the candidate reply: {error}"))
                })?
            }
            HelperRequest::Pointer { x, y } => {
                if pointer.is_none() {
                    pointer = desktop.and_then(open_pointer_mover);
                }
                if let Some(injector) = pointer.as_mut() {
                    // Best effort: a pointer that cannot be moved is not a
                    // reason to abandon the capture the user is in the middle
                    // of, and the editor's own cursor has already moved.
                    let _ = injector.move_pointer(Point { x, y });
                }
                "{}".to_string()
            }
            HelperRequest::Release => {
                let result = json.as_deref().ok_or_else(|| {
                    VshotError::Selection(
                        "the Qt helper asked to be let go before it reported a result".into(),
                    )
                })?;
                // The render goes with it: the handoff is what puts these
                // pixels in the pin, and the editor is holding them up on the
                // screen until it is told they landed.
                match release(result, composite.as_ref()) {
                    Ok(reply) => reply,
                    Err(error) => {
                        // The handoff could not be made, but the helper is
                        // still told to stop: it has drawn everything it is
                        // going to draw and the session is over either way.
                        // The failure travels to the caller below.
                        handoff = Some(error);
                        "{}".to_string()
                    }
                }
            }
        };
        writeln!(requests, "{reply}")
            .and_then(|()| requests.flush())
            .map_err(|error| {
                VshotError::Selection(format!("failed to answer the Qt helper: {error}"))
            })?;
    }

    let output = child.wait_with_output().map_err(|error| {
        VshotError::Selection(format!("failed to collect Qt helper output: {error}"))
    })?;
    // A handoff that failed is reported before the exit status: the editor
    // stopped because it was told to, so its status says nothing about whether
    // the pin took the pixels.
    if let Some(error) = handoff {
        return Err(error);
    }
    if !output.status.success() {
        return Err(helper_exit_error(output.status, &output.stderr));
    }
    let json =
        json.ok_or_else(|| VshotError::Selection("the Qt helper closed without a result".into()))?;
    Ok(HelperOutput { json, composite })
}

/// A pointer mover for the desktop `desktop`, or `None` when this session has
/// no way to move one.
///
/// The compositor's own virtual-pointer protocol only.  The scrolling capture
/// prefers `Auto`, which falls back to the remote-desktop portal and then to
/// `/dev/uinput` — and the portal is wrong here in a way it is not wrong there:
/// opening it puts up a permission dialog, and a scroll is a thing the user
/// asked for by pressing the button that starts it, while a nudge of the
/// keyboard cursor is a keystroke.  A dialog appearing because an arrow key was
/// tapped, on a compositor that has no virtual pointer to offer, is worse than
/// the pointer not moving: the editor's own cursor has already moved, and the
/// loupe says where it went.
fn open_pointer_mover(desktop: crate::geometry::Rect) -> Option<crate::inject::Injector> {
    crate::inject::Injector::open(desktop, crate::inject::Prefer::Wlr).ok()
}

fn run_helper(
    helper: &HelperLookup,
    session_path: &Path,
    scene: &SceneSnapshot,
    release: &mut ReleaseHandler<'_>,
) -> Result<HelperOutput> {
    run_interactive_helper(helper, session_path, scene, &|| None, release)
}

/// Hands the helper every source frame over the pixel channel.
///
/// They are sent in the session's own output order, which is how the helper
/// pairs them with the outputs it parsed, and the header's kind keeps them from
/// being mistaken for a result.
fn send_source_frames(
    channel: &crate::pixel_fd::PixelChannel,
    sources: &[&crate::model::Frame],
) -> Result<()> {
    for frame in sources {
        let size = frame.size();
        channel.send(
            crate::pixel_fd::KIND_SOURCE,
            crate::pixel_fd::FORMAT_RGBA8888,
            size.width,
            size.height,
            frame.pixels(),
        )?;
    }
    Ok(())
}

pub(crate) fn parse_outcome_with_render(
    output: HelperOutput,
    bounds: Rect,
) -> Result<SelectionOutcome> {
    let mut outcome = parse_outcome(output.json, bounds)?;
    outcome.composite = output.composite;
    Ok(outcome)
}

fn read_frame(channel: &crate::pixel_fd::PixelChannel, what: &str) -> Result<crate::model::Frame> {
    let received = channel.receive()?;
    if received.kind != crate::pixel_fd::KIND_RESULT {
        return Err(VshotError::Selection(format!(
            "the Qt helper sent {what} as something other than a rendered image"
        )));
    }
    if received.format != crate::pixel_fd::FORMAT_RGBA8888 {
        return Err(VshotError::Selection(format!(
            "{what} arrived in format {} where RGBA8 was expected",
            received.format
        )));
    }
    crate::model::Frame::new(
        crate::geometry::Size::new(received.width, received.height),
        received.pixels().to_vec(),
    )
    .map_err(|error| VshotError::Selection(error.to_string()))
}

/// Reads the helper's render back when its answer says it sent one.
///
/// The flag in the JSON is what decides, rather than the channel's own
/// readability: a session that rendered nothing (a cancelled one, a picker)
/// leaves the channel silent, and blocking on it would hang the CLI.
fn read_composite(
    json: &[u8],
    channel: &crate::pixel_fd::PixelChannel,
) -> Result<Option<RenderedCapture>> {
    let result: QtResult = serde_json::from_slice(json).map_err(|error| {
        VshotError::Selection(format!("Qt helper returned invalid result JSON: {error}"))
    })?;
    if !result.composite {
        return Ok(None);
    }
    // The order the helper sends them in: the marks, then the flattened
    // capture.  Fixed, so no second header field is needed to tell them apart.
    let marks = read_frame(channel, "the marks")?;
    let composite = read_frame(channel, "the rendered capture")?;
    if marks.size() != composite.size() {
        return Err(VshotError::Selection(format!(
            "the helper sent marks of {}x{} over a capture of {}x{}",
            marks.size().width,
            marks.size().height,
            composite.size().width,
            composite.size().height
        )));
    }
    Ok(Some(RenderedCapture { composite, marks }))
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
struct WirePoint {
    x: i64,
    y: i64,
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
struct WireRect {
    x: i64,
    y: i64,
    width: u64,
    height: u64,
}

impl From<Rect> for WireRect {
    fn from(rect: Rect) -> Self {
        Self {
            x: i64::from(rect.left()),
            y: i64::from(rect.top()),
            width: u64::from(rect.size.width),
            height: u64::from(rect.size.height),
        }
    }
}

impl WireRect {
    fn into_rect(self, label: &str) -> Result<Rect> {
        let x = i32::try_from(self.x)
            .map_err(|_| VshotError::Selection(format!("{label} x is out of range")))?;
        let y = i32::try_from(self.y)
            .map_err(|_| VshotError::Selection(format!("{label} y is out of range")))?;
        let width = u32::try_from(self.width)
            .map_err(|_| VshotError::Selection(format!("{label} width is out of range")))?;
        let height = u32::try_from(self.height)
            .map_err(|_| VshotError::Selection(format!("{label} height is out of range")))?;
        if width == 0 || height == 0 {
            return Err(VshotError::Selection(format!("{label} must not be empty")));
        }
        let rect = Rect::new(x, y, width, height);
        rect.right()
            .map_err(|_| VshotError::Selection(format!("{label} right edge is out of range")))?;
        rect.bottom()
            .map_err(|_| VshotError::Selection(format!("{label} bottom edge is out of range")))?;
        Ok(rect)
    }
}

#[derive(Debug, Serialize)]
struct QtSession<'a> {
    version: u32,
    mode: &'a str,
    bounds: WireRect,
    // Pin-edit only: the editor surface rect (equal to `bounds`, but the
    // helper reads it as an explicit placement hint).
    #[serde(skip_serializing_if = "Option::is_none")]
    window: Option<WireRect>,
    // Pin-edit only: daemon socket the editor talks to while moving the pin.
    #[serde(skip_serializing_if = "Option::is_none")]
    socket: Option<String>,
    // Pin-edit only: id of the pinned image inside the daemon.
    #[serde(skip_serializing_if = "Option::is_none")]
    id: Option<u64>,
    // Pin-edit only: how wide the pin's own border is drawn, in logical pixels.
    // The border is centred on the image's edge, so it reaches half this far
    // outside the image on every side; the editor counts that band as part of
    // the pin, so a drag that starts on the rim moves the pin rather than
    // landing on the bare canvas beside it.
    #[serde(skip_serializing_if = "Option::is_none")]
    border_width: Option<u32>,
    // Pin-edit only: which part of the editor to open on, absent for the
    // ordinary annotation editor.
    #[serde(skip_serializing_if = "Option::is_none")]
    action: Option<&'a str>,
    // Window-pick only: what the pointer may snap to.
    #[serde(skip_serializing_if = "Option::is_none")]
    candidates: Option<Vec<QtCandidate>>,
    // Region only: a selection the user has already made (the picker resolved
    // one), which the session opens in editing state instead of waiting for a
    // drag.
    #[serde(skip_serializing_if = "Option::is_none")]
    selection: Option<WireRect>,
    // Region editing only: whether the editor offers the scrolling-capture
    // action.  Window editing reuses the same editor on a frame that has
    // nothing to scroll, so it leaves this out and the action stays away.
    #[serde(skip_serializing_if = "Option::is_none")]
    long_allowed: Option<bool>,
    // Marks already drawn on this frame, in the shape the helper itself emits
    // them, so re-entering an editing session opens on them instead of on a
    // blank canvas.  The session a first edit runs under has none; the one a
    // pin's second edit runs under carries the marks the first one committed,
    // which is what makes them editable again rather than merely visible.
    //
    // Opaque here on purpose: the daemon holds what the helper wrote verbatim
    // and hands it straight back, so the two ends of one protocol cannot drift
    // apart by this side re-spelling the marks it was given.
    #[serde(skip_serializing_if = "Option::is_none")]
    annotations: Option<serde_json::Value>,
    // Translate mode only: the language pair and provider the helper hands to
    // `vshot translate --stdin-ocr` while it works, so the overlay and the CLI
    // translate the same way.
    #[serde(skip_serializing_if = "Option::is_none")]
    translate: Option<QtTranslate<'a>>,
    // Translate mode only: absolute path the helper writes its composited PNG
    // to.  Optional like every other added field, so an older helper ignores
    // it and a newer one without it falls back to a temp file of its own.
    #[serde(skip_serializing_if = "Option::is_none")]
    result_path: Option<String>,
    // Pin-edit only: the bounding box of every output, which is the coordinate
    // space the CLI's injection backend expresses an absolute pointer position
    // in.  The session's own `bounds` is the pin, which is not the screen, so
    // the editor's keyboard-cursor walk would have the pointer land a fraction
    // of the way to where it belongs without this.
    #[serde(skip_serializing_if = "Option::is_none")]
    desktop: Option<WireRect>,
    outputs: Vec<QtOutput<'a>>,
}

/// The language pair and provider a translate session runs with, exactly the
/// three knobs the CLI takes.
#[derive(Debug, Serialize)]
struct QtTranslate<'a> {
    from: &'a str,
    to: &'a str,
    provider: &'a str,
}

/// One pickable window: the helper highlights the candidate on top at the
/// pointer — the last one in the list, which arrives bottom to top — and shows
/// its label in the size pill.
#[derive(Debug, Serialize)]
struct QtCandidate {
    x: i64,
    y: i64,
    width: u64,
    height: u64,
    #[serde(skip_serializing_if = "String::is_empty")]
    label: String,
}

/// The answer to the picker's refresh request: a fresh window list, or an
/// empty object when there is nothing to say.
#[derive(Debug, Serialize)]
struct CandidateReply {
    #[serde(skip_serializing_if = "Option::is_none")]
    candidates: Option<Vec<QtCandidate>>,
}

impl From<&WindowCandidate> for QtCandidate {
    fn from(candidate: &WindowCandidate) -> Self {
        Self {
            x: i64::from(candidate.geometry.left()),
            y: i64::from(candidate.geometry.top()),
            width: u64::from(candidate.geometry.size.width),
            height: u64::from(candidate.geometry.size.height),
            label: candidate.label.clone(),
        }
    }
}

#[derive(Debug, Serialize)]
struct QtOutput<'a> {
    id: u32,
    name: &'a str,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    // Overlay surface the helper maps onto: equal to the output rect for
    // region capture, the whole output for pin editing (so the toolbar can
    // float on the canvas beside the pinned image).
    surface: WireRect,
    // Device pixels per logical pixel of this output.  A real output's density
    // is a whole number; the pin editor's virtual output carries the zoom its
    // image is shown at, which is not (a 160-pixel pin across 176 logical
    // pixels is 0.909), so it is serialized as the number it is.
    scale: f64,
    pixel_width: u32,
    pixel_height: u32,
    path: String,
    // Set when VShot is showing this output's HDR half on a backdrop surface
    // below the overlay.  The helper then leaves the frozen frame out and
    // veils the backdrop instead, so what shows through the selection is the
    // real light rather than the SDR map of it.
    #[serde(default)]
    backdrop: bool,
}

#[derive(Debug, Deserialize)]
struct QtResult {
    status: String,
    selection: Option<WireRect>,
    // Window-pick only: where the pointer was when the click committed, so the
    // caller can resolve the click against the windows that exist by then.
    point: Option<WirePoint>,
    // The marks, in the shape a later session can hand straight back: the
    // editor builds them without the transient parts of a render (a label's
    // bitmap, a translation's pixels), so a daemon that stores them can reopen
    // the pin for editing instead of only showing the flattened result.  This
    // side never reads into them -- it stores them and hands them back -- so
    // they stay the wire's own JSON.
    #[serde(default)]
    marks: Option<serde_json::Value>,
    // Region editing only: the user pressed the toolbar's scrolling-capture
    // action, so the selection names a region to scroll and stitch rather
    // than a still to keep.
    #[serde(default)]
    long: bool,
    // Translate mode only: the finished translation, and the PNG the helper
    // composited it into.  Both optional, so an older helper that does not
    // send them still parses here and a newer one always does.
    translated_text: Option<String>,
    image_path: Option<String>,
    // Set when the user finished with the toolbar's Pin button: the image goes
    // to the screen instead of to the destination the command line asked for.
    pin: Option<bool>,
    // Set when the helper rendered the capture and sent it back over the pixel
    // channel.  The rendered image *is* the result -- Qt is the only renderer --
    // so a session that says this has its pixels waiting to be read.
    #[serde(default)]
    composite: bool,
}

/// What an editing session reported: the region to keep, and the session's two
/// other answers — a scrolling capture instead of a still, an image to pin
/// instead of to save.
///
/// Neither answer decides what the image *is*: they come back with the rest of
/// the session rather than as destinations of their own, so the CLI composes the
/// image once and only then chooses what to do with it.
pub(crate) struct SelectionOutcome {
    pub(crate) rect: Rect,
    /// True when the toolbar's scrolling-capture action was pressed: the
    /// region names something to scroll and stitch rather than a still, and the
    /// picture the stitch is made of does not exist yet.
    pub(crate) long: bool,
    /// True when the toolbar's Pin button was pressed.
    pub(crate) pin: bool,
    /// The helper's render, when it sent one over the pixel channel.  This is
    /// the image the CLI writes: it is not rebuilt from a description of the
    /// marks, because a second renderer is exactly what used to leave a
    /// committed mark half a pixel from its preview.
    pub(crate) composite: Option<RenderedCapture>,
    /// The marks as data, in the shape a later session can hand straight back.
    /// Carried only so the Pin destination can give them to the daemon, which
    /// stores them and reopens the pin for editing on them; every other
    /// destination flattens the render and is done.  `None` when the session
    /// drew nothing.
    pub(crate) marks: Option<serde_json::Value>,
}

pub fn select_and_edit(
    scene: &SceneSnapshot,
    backdrop: &[String],
    release: &mut ReleaseHandler<'_>,
) -> Result<SelectionOutcome> {
    let (_directory, session_path) = write_session(scene, "region", &[], None, true, backdrop)?;
    let helper = helper_program()?;
    let output = run_helper(&helper, &session_path, scene, release)?;
    parse_outcome_with_render(output, scene.bounds())
}

/// What a picking session reported: the window the click landed on, as the
/// candidate list it started with saw it, plus where the pointer was.  The
/// caller resolves the click against the windows that exist by capture time,
/// because the picker runs on a live desktop.
pub(crate) struct PickedWindow {
    pub(crate) rect: Rect,
    pub(crate) point: Option<Point>,
}

/// Interactive window picking.  The helper keeps the desktop live, highlights
/// the candidate on top at the pointer, and the first click ends the session:
/// picking only decides *what* to capture, never the pixels.
///
/// `refresh` is asked while the session is open for the windows to highlight,
/// because the desktop it runs on can change under it (a workspace switch, a
/// window that moved).  Returning `None` means "no fresh source", which leaves
/// the picker with the list it started from.
pub fn pick_window(
    scene: &SceneSnapshot,
    candidates: &[WindowCandidate],
    refresh: impl Fn() -> Option<Vec<WindowCandidate>>,
) -> Result<PickedWindow> {
    let (_directory, session_path) =
        write_session(scene, "window-pick", candidates, None, false, &[])?;
    let helper = helper_program()?;
    let output = run_interactive_helper(&helper, &session_path, scene, &refresh, &mut |_, _| {
        Ok("{}".to_string())
    })?;
    parse_picked_window(output.json, scene.bounds())
}

/// Re-opens a captured scene for annotation with `selection` already made, so
/// the window the user picked is edited on the frame that was captured after
/// the pick — not on the one the picking itself started from.
pub fn edit_selection(
    scene: &SceneSnapshot,
    selection: Rect,
    backdrop: &[String],
    release: &mut ReleaseHandler<'_>,
) -> Result<SelectionOutcome> {
    let (_directory, session_path) = write_session(
        scene,
        "region",
        &[],
        Some(selection.into()),
        false,
        backdrop,
    )?;
    let helper = helper_program()?;
    let output = run_helper(&helper, &session_path, scene, release)?;
    parse_outcome_with_render(output, scene.bounds())
}

/// Asks for a region without offering to annotate it.  Scrolling capture is
/// what this is for: the frame that gets stitched does not exist yet, so there
/// is nothing to mark up at selection time.
pub fn select_region(scene: &SceneSnapshot) -> Result<Rect> {
    let (_directory, session_path) = write_session(scene, "region-only", &[], None, false, &[])?;
    let helper = helper_program()?;
    let output = run_helper(&helper, &session_path, scene, &mut |_, _| {
        Ok("{}".to_string())
    })?;
    Ok(parse_outcome_with_render(output, scene.bounds())?.rect)
}

/// What a translate session reported: the finished translation, which the
/// caller prints or copies, and where the helper says it wrote the composited
/// PNG (normally the `result_path` it was handed).
pub(crate) struct TranslateOutcome {
    pub(crate) text: String,
    pub(crate) image_path: Option<PathBuf>,
}

/// Opens the Qt overlay in its translate mode and waits for it to finish.
///
/// The whole interaction lives on the helper's side: it frames a region,
/// recognizes it (`vshot ocr`), translates it (`vshot translate --stdin-ocr`)
/// and draws the translation over the original, then writes the composited PNG
/// to `result_path`.  This side only builds the session and reads the answer —
/// the same shape `select_region` has, one mode over.
pub(crate) fn translate_overlay(
    scene: &SceneSnapshot,
    from: &str,
    to: &str,
    provider: &str,
    result_path: &Path,
) -> Result<TranslateOutcome> {
    let translate = QtTranslate { from, to, provider };
    let (_directory, session_path) = write_session_full(
        scene,
        "translate",
        &[],
        None,
        false,
        Some(translate),
        Some(result_path.to_string_lossy().into_owned()),
        &[],
    )?;
    let helper = helper_program()?;
    let output = run_helper(&helper, &session_path, scene, &mut |_, _| {
        Ok("{}".to_string())
    })?;
    parse_translate_result(output.json)
}

/// Reads a translate session's answer: the translated text a `status: "ok"`
/// result carries.  A cancel is the same error every other overlay returns.
fn parse_translate_result(bytes: Vec<u8>) -> Result<TranslateOutcome> {
    let result: QtResult = serde_json::from_slice(&bytes).map_err(|error| {
        VshotError::Translate(format!("Qt helper returned invalid result JSON: {error}"))
    })?;
    match result.status.as_str() {
        "cancelled" => Err(VshotError::SelectionCancelled),
        "ok" => {
            let text = result.translated_text.ok_or_else(|| {
                VshotError::Translate("the Qt helper returned no translated text".into())
            })?;
            Ok(TranslateOutcome {
                text,
                image_path: result.image_path.map(PathBuf::from),
            })
        }
        status => Err(VshotError::Translate(format!(
            "Qt helper returned unknown status `{status}`"
        ))),
    }
}

/// What the hint overlay of a scrolling capture said.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum HintEvent {
    /// Enter: keep what has been stitched.
    Done,
    /// Esc (or the cancel button): throw it away.
    Cancelled,
    /// The helper went away without saying anything.
    Closed,
}

/// The overlay a scrolling capture keeps on screen while it works: it shows
/// how far the stitch has come and turns Esc/Enter into an answer.
///
/// It is deliberately not a full-screen session — the desktop stays live and
/// the CLI keeps grabbing real frames — and it has to stay outside the region
/// being captured, or the overlay itself would end up in the stitched image.
pub(crate) struct HintSession {
    child: Child,
    stdin: ChildStdin,
    events: std::sync::mpsc::Receiver<HintEvent>,
    _directory: TempDir,
}

impl HintSession {
    /// Pushes a progress update.  Failures are reported: a hint overlay that
    /// silently stopped updating means the user lost their way to stop the
    /// capture.
    pub(crate) fn status(&mut self, height: u32, frames: u32, note: &str) -> Result<()> {
        let status = QtHintStatus {
            height,
            frames,
            note,
        };
        let encoded = serde_json::to_string(&status).map_err(|error| {
            VshotError::Selection(format!("failed to encode the hint status: {error}"))
        })?;
        match writeln!(self.stdin, "{encoded}").and_then(|()| self.stdin.flush()) {
            Ok(()) => Ok(()),
            // The overlay may already be gone — the user just pressed Esc or
            // Enter — and every write after that fails with a broken pipe.
            // The answer itself is already on its way through the reader, so
            // this is an ending, not a failure.
            Err(error) if error.kind() == std::io::ErrorKind::BrokenPipe => Ok(()),
            Err(error) => Err(VshotError::Selection(format!(
                "failed to update the hint overlay: {error}"
            ))),
        }
    }

    /// Non-blocking: `None` means the user has not answered yet.
    pub(crate) fn poll(&self) -> Option<HintEvent> {
        self.events.try_recv().ok()
    }

    /// Waits for the overlay to leave the screen, so the caller can finish
    /// with the desktop without a stray surface in the way.
    pub(crate) fn close(self) -> Result<()> {
        // Closing the status pipe is the helper's cue that nothing else is
        // coming, so it can leave the screen before this returns.
        let HintSession { child, stdin, .. } = self;
        drop(stdin);
        let output = child.wait_with_output().map_err(|error| {
            VshotError::Selection(format!(
                "failed to collect the hint overlay's output: {error}"
            ))
        })?;
        if !output.status.success() {
            return Err(helper_exit_error(output.status, &output.stderr));
        }
        Ok(())
    }
}

/// One progress update, as the helper reads it.
#[derive(Debug, Serialize)]
struct QtHintStatus<'a> {
    height: u32,
    frames: u32,
    /// Short note for the overlay: empty while scrolling, `done` at the end.
    note: &'a str,
}

/// The helper's answer to a hint session: one of the two flags, then it exits.
#[derive(Debug, Deserialize)]
struct QtHintReply {
    #[serde(default)]
    done: bool,
    #[serde(default)]
    cancelled: bool,
}

/// Non-image session description for the hint overlay: the region being
/// captured (which the overlay must stay clear of) and where the outputs are
/// (so it can pick a corner to live in).
#[derive(Debug, Serialize)]
struct QtHintSession<'a> {
    version: u32,
    mode: &'a str,
    bounds: WireRect,
    outputs: Vec<QtHintOutput<'a>>,
}

#[derive(Debug, Serialize)]
struct QtHintOutput<'a> {
    id: u32,
    name: &'a str,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    surface: WireRect,
    scale: u32,
}

/// Starts the hint overlay of a scrolling capture.
pub(crate) fn start_hint_session(region: Rect, outputs: &[OutputInfo]) -> Result<HintSession> {
    let directory = tempfile::Builder::new()
        .prefix("vshot-long-")
        .tempdir_in("/dev/shm")
        .or_else(|_| tempfile::tempdir())
        .map_err(|error| {
            VshotError::Selection(format!(
                "failed to create the scroll session directory: {error}"
            ))
        })?;
    let session = QtHintSession {
        version: 1,
        mode: "long-shot",
        bounds: region.into(),
        outputs: outputs
            .iter()
            .map(|output| QtHintOutput {
                id: output.global_id,
                name: &output.name,
                x: output.geometry.left(),
                y: output.geometry.top(),
                width: output.geometry.size.width,
                height: output.geometry.size.height,
                surface: output.geometry.into(),
                scale: output.scale,
            })
            .collect(),
    };
    let session_path = directory.path().join("session.json");
    let encoded = serde_json::to_vec(&session).map_err(|error| {
        VshotError::Selection(format!("failed to encode the hint session JSON: {error}"))
    })?;
    write_private_file(&session_path, &encoded)?;

    let helper = helper_program()?;
    let mut child = Command::new(&helper.path)
        .arg("--session")
        .arg(&session_path)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| helper_spawn_error(&helper, error))?;
    let stdin = child
        .stdin
        .take()
        .ok_or_else(|| VshotError::Selection("the hint overlay has no status pipe".into()))?;
    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| VshotError::Selection("the hint overlay has no answer pipe".into()))?;

    // The answer arrives whenever the user presses a key, which is not
    // something the scrolling loop can wait on: a thread watches the helper's
    // stdout and the loop polls the channel.
    let (sender, events) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let reader = BufReader::new(stdout);
        for line in reader.lines() {
            let Ok(line) = line else { break };
            let Ok(reply) = serde_json::from_str::<QtHintReply>(line.trim()) else {
                continue;
            };
            if reply.done {
                let _ = sender.send(HintEvent::Done);
                return;
            }
            if reply.cancelled {
                let _ = sender.send(HintEvent::Cancelled);
                return;
            }
        }
        let _ = sender.send(HintEvent::Closed);
    });

    Ok(HintSession {
        child,
        stdin,
        events,
        _directory: directory,
    })
}

/// Pin-edit descriptor: the daemon pins one image; the helper edits it in
/// place, driving the real pin window and the daemon socket.
#[derive(Clone, Copy, Debug)]
pub(crate) struct PinEditSpec<'a> {
    /// The pin's pixels at their native resolution (RGBA8).
    pub(crate) frame: &'a crate::model::Frame,
    /// Real compositor output the editor surface lands on.
    pub(crate) output_name: &'a str,
    /// Global logical rect of the pinned image.
    pub(crate) window: Rect,
    /// Device pixels per logical pixel between `frame` and `window`: 1 for a
    /// plain pin, the output's density for a HiDPI-rendered text card, and the
    /// zoom the image is shown at for a pin the user has zoomed.  The last is
    /// not a whole number, which is why it is a ratio rather than a density.
    pub(crate) scale: crate::edit::Scale,
    /// Daemon socket the editor uses to move the pin live.
    pub(crate) socket: &'a Path,
    /// Id of this pin inside the daemon, echoed back in session JSON.
    pub(crate) pin_id: u64,
    /// Which part of the editor to open on, empty for the ordinary annotation
    /// editor the Space key opens. `"text"` opens it on the recognized text.
    pub(crate) action: &'a str,
    /// The marks already on the pin, exactly as the helper last reported them,
    /// so a second edit opens on them and they stay editable instead of having
    /// been baked into the pixels the first one committed.  `None` on a pin
    /// that has never been annotated.
    pub(crate) annotations: Option<&'a serde_json::Value>,
    /// How wide the pin's border is drawn, in logical pixels, so the editor can
    /// count the rim as part of the pin: the stroke is centred on the image's
    /// edge and reaches half this far outside it, and a drag there should move
    /// the pin rather than read as a click on the bare canvas.
    pub(crate) border_width: u32,
    /// Bounding box of every output, or `None` when the caller has no way to
    /// ask.  The editor's keyboard-cursor walk asks the CLI to move the real
    /// pointer, and a pointer position is expressed in this space; `window` is
    /// not it, because the pin is not the screen.  Left out, the pointer simply
    /// is not moved, which is what happens on a compositor with no injection
    /// backend anyway.
    pub(crate) desktop: Option<Rect>,
}

/// Serializes a pin-edit session: one virtual output whose geometry is the
/// editor window; the pin image is written raw next to the JSON.
pub(crate) fn write_pin_edit_session(spec: &PinEditSpec<'_>) -> Result<(TempDir, PathBuf)> {
    let directory = tempfile::Builder::new()
        .prefix("vshot-pin-edit-")
        .tempdir_in("/dev/shm")
        .or_else(|_| tempfile::tempdir())
        .map_err(|error| {
            VshotError::Pin(format!(
                "failed to create pin-edit session directory: {error}"
            ))
        })?;
    let session = QtSession {
        version: 1,
        mode: "pin-edit",
        bounds: spec.window.into(),
        window: Some(spec.window.into()),
        socket: Some(spec.socket.to_string_lossy().into_owned()),
        id: Some(spec.pin_id),
        border_width: Some(spec.border_width),
        action: (!spec.action.is_empty()).then_some(spec.action),
        candidates: None,
        selection: None,
        long_allowed: None,
        annotations: spec.annotations.cloned(),
        translate: None,
        result_path: None,
        desktop: spec.desktop.map(WireRect::from),
        outputs: vec![QtOutput {
            id: 0,
            name: spec.output_name,
            x: spec.window.left(),
            y: spec.window.top(),
            width: spec.window.size.width,
            height: spec.window.size.height,
            // The editor covers the pinned image, not the whole output: the
            // helper widens it to the screen the pin sits on so the toolbar
            // lives on the canvas beside the image.
            surface: spec.window.into(),
            scale: spec.scale.factor(),
            pixel_width: spec.frame.size().width,
            pixel_height: spec.frame.size().height,
            // The image travels over the pixel channel, not as a file beside
            // the session: it is a screenful of pixels, and the channel is what
            // the two sides already share for exactly that.
            path: String::new(),
            // A pinned image is its own SDR picture, with no frozen screen
            // behind it to show better.
            backdrop: false,
        }],
    };
    let session_path = directory.path().join("session.json");
    let encoded = serde_json::to_vec(&session).map_err(|error| {
        VshotError::Pin(format!("failed to encode pin-edit session JSON: {error}"))
    })?;
    write_private_file(&session_path, &encoded)?;
    Ok((directory, session_path))
}

/// Runs the Qt helper against a session file and collects its answer: the
/// result JSON and, when the helper rendered one, the capture it sent back.
///
/// The pin editor goes through here rather than [`run_helper`] because it is
/// started with a different flag, but it is the same dialogue: the editor walks
/// the real pointer with its keyboard cursor like any other session, so it asks
/// for a warp the same way, and it needs a stdin to be answered on.  Only the
/// scene differs -- a pinned image is not a frozen screen, so the desktop the
/// pointer request is expressed in is read from the session rather than from a
/// capture.
pub(crate) fn run_session(
    session_path: &Path,
    pin: &crate::model::Frame,
    handoff: &mut ReleaseHandler<'_>,
) -> Result<HelperOutput> {
    let helper = helper_program()?;
    run_pin_session(&helper, session_path, pin, handoff)
}

/// [`run_session`] against a helper that is already located, so the dialogue
/// itself can be driven by a stub.
///
/// `handoff` is what the editor's release is answered with.  It is the one part
/// of a pin edit that cannot happen inside the dialogue: the daemon is only told
/// the new pixels after the dialogue has returned, because it is the same
/// conversation -- and the same helper process -- that produces them.
fn run_pin_session(
    helper: &HelperLookup,
    session_path: &Path,
    pin: &crate::model::Frame,
    handoff: &mut ReleaseHandler<'_>,
) -> Result<HelperOutput> {
    // The desktop a pointer request is expressed in: the CLI subtracts this
    // origin and scales to its size.  Read from the session, which is the only
    // place that knows the layout -- the pin image itself is not the screen.
    // An unreadable one means the pointer simply is not moved, which is what
    // happens on a compositor with no injection backend anyway.
    let desktop = session_desktop(session_path).ok();
    run_helper_dialogue(
        helper,
        "--pin-edit",
        session_path,
        &[pin],
        desktop,
        &|| None,
        handoff,
    )
    .map_err(|error| match error {
        // The pin editor is the pin's own editor, so its failures are the
        // pin's: the message a user sees should name what they asked for.
        VshotError::Selection(message) => VshotError::Pin(message),
        other => other,
    })
}

/// The desktop rect a pin-edit session's pointer requests are expressed in.
///
/// The session's own `bounds` is the pin, which is not the screen; the daemon
/// writes the layout's bounding box as `desktop` for exactly this.  A session
/// without one is left with no pointer mover rather than a wrong rectangle: a
/// pointer moved to the wrong place is worse than one that did not move.
fn session_desktop(session_path: &Path) -> Result<crate::geometry::Rect> {
    let payload = std::fs::read(session_path).map_err(|source| {
        VshotError::Pin(format!(
            "failed to read pin-edit session {}: {source}",
            session_path.display()
        ))
    })?;
    let session: serde_json::Value = serde_json::from_slice(&payload)
        .map_err(|error| VshotError::Pin(format!("invalid pin-edit session JSON: {error}")))?;
    let desktop = session
        .get("desktop")
        .ok_or_else(|| VshotError::Pin("pin-edit session has no desktop rect".into()))?;
    let as_i64 = |key: &str| -> Result<i64> {
        desktop
            .get(key)
            .and_then(serde_json::Value::as_i64)
            .ok_or_else(|| VshotError::Pin(format!("pin-edit desktop field `{key}` is missing")))
    };
    let x = i32::try_from(as_i64("x")?)
        .map_err(|_| VshotError::Pin("pin-edit desktop x is out of range".into()))?;
    let y = i32::try_from(as_i64("y")?)
        .map_err(|_| VshotError::Pin("pin-edit desktop y is out of range".into()))?;
    let width = u32::try_from(as_i64("width")?)
        .map_err(|_| VshotError::Pin("pin-edit desktop width is out of range".into()))?;
    let height = u32::try_from(as_i64("height")?)
        .map_err(|_| VshotError::Pin("pin-edit desktop height is out of range".into()))?;
    Ok(crate::geometry::Rect::new(x, y, width, height))
}

/// Parses a pin-edit helper result: where the user left the image, and the
/// marks on it in the shape the daemon stores them.  The status `cancelled`
/// maps to `Ok(None)`.
pub(crate) struct EditedPin {
    pub(crate) selection: Rect,
    /// The marks as the session handed them over and the editor handed them
    /// back, verbatim.  This is what the daemon keeps, and what it hands to the
    /// next session: re-spelling them here would be a second place for the wire
    /// shape to drift, and the render form the helper also emits is not what a
    /// re-edit needs.
    pub(crate) marks: serde_json::Value,
}

pub(crate) fn parse_edit_result(bytes: Vec<u8>, window: Rect) -> Result<Option<EditedPin>> {
    let result: QtResult = serde_json::from_slice(&bytes).map_err(|error| {
        VshotError::Pin(format!("Qt helper returned invalid result JSON: {error}"))
    })?;
    match result.status.as_str() {
        "cancelled" => Ok(None),
        "ok" => {
            // The image may have been dragged anywhere over the screen the pin
            // sits on; only its position changes, never its size.
            let selection = result
                .selection
                .ok_or_else(|| VshotError::Pin("Qt result has no selection".into()))?
                .into_rect("Qt selection")?;
            if selection.size != window.size {
                return Err(VshotError::Pin(
                    "Qt helper resized the pin image; pin editing only moves it".into(),
                ));
            }
            if selection.size.width < 5 || selection.size.height < 5 {
                return Err(VshotError::Pin(
                    "Qt helper returned a pin selection smaller than 5x5".into(),
                ));
            }
            let marks = result
                .marks
                .unwrap_or_else(|| serde_json::Value::Array(Vec::new()));
            Ok(Some(EditedPin { selection, marks }))
        }
        status => Err(VshotError::Pin(format!(
            "Qt helper returned unknown status `{status}`"
        ))),
    }
}

/// Serializes a Qt editing session and writes it to a private directory,
/// returning the directory (kept alive by the caller) and the JSON's path.
///
/// `translate` and `result_path` are the translate mode's two additions; every
/// other mode passes `None` and the fields stay out of its document.
#[allow(clippy::too_many_arguments)] // the session document's own shape
fn write_session_full(
    scene: &SceneSnapshot,
    mode: &str,
    candidates: &[WindowCandidate],
    selection: Option<WireRect>,
    // Whether the editor offers the scrolling-capture action: only the plain
    // region session does, because the other modes have already decided what
    // they are for.
    long_allowed: bool,
    translate: Option<QtTranslate<'_>>,
    result_path: Option<String>,
    // The outputs whose frozen frame the helper should leave to an HDR backdrop
    // surface below it, rather than drawing itself.
    backdrop: &[String],
) -> Result<(TempDir, PathBuf)> {
    let directory = tempfile::Builder::new()
        .prefix("vshot-qt-")
        .tempdir_in("/dev/shm")
        .or_else(|_| tempfile::tempdir())
        .map_err(|error| {
            VshotError::Selection(format!(
                "failed to create private Qt session directory: {error}"
            ))
        })?;

    let mut outputs = Vec::with_capacity(scene.outputs().len());
    for output in scene.outputs() {
        outputs.push(QtOutput {
            id: output.global_id,
            name: &output.name,
            x: output.geometry.left(),
            y: output.geometry.top(),
            width: output.geometry.size.width,
            height: output.geometry.size.height,
            surface: output.geometry.into(),
            scale: f64::from(output.scale),
            pixel_width: output.frame.size().width,
            pixel_height: output.frame.size().height,
            // The frame itself travels over the pixel channel rather than as a
            // file beside the session: it is a screenful of pixels, and the
            // channel carries exactly that.
            path: String::new(),
            backdrop: backdrop.iter().any(|name| name == &output.name),
        });
    }

    let session = QtSession {
        version: 1,
        mode,
        bounds: scene.bounds().into(),
        window: None,
        socket: None,
        id: None,
        border_width: None,
        action: None,
        candidates: (!candidates.is_empty())
            .then(|| candidates.iter().map(QtCandidate::from).collect()),
        selection,
        long_allowed: long_allowed.then_some(true),
        annotations: None,
        translate,
        result_path,
        // A region session's bounds *are* the desktop's, so the walk's pointer
        // requests are already expressed in the space the CLI converts against.
        desktop: None,
        outputs,
    };
    let session_path = directory.path().join("session.json");
    let encoded = serde_json::to_vec(&session).map_err(|error| {
        VshotError::Selection(format!("failed to encode Qt session JSON: {error}"))
    })?;
    write_private_file(&session_path, &encoded)?;
    Ok((directory, session_path))
}

fn write_session(
    scene: &SceneSnapshot,
    mode: &str,
    candidates: &[WindowCandidate],
    selection: Option<WireRect>,
    long_allowed: bool,
    // The outputs whose frozen frame the helper should leave to an HDR backdrop
    // surface below it, rather than drawing itself.
    backdrop: &[String],
) -> Result<(TempDir, PathBuf)> {
    write_session_full(
        scene,
        mode,
        candidates,
        selection,
        long_allowed,
        None,
        None,
        backdrop,
    )
}

fn write_private_file(path: &Path, bytes: &[u8]) -> Result<()> {
    #[cfg(unix)]
    use std::os::unix::fs::OpenOptionsExt;

    let mut options = OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    options.mode(0o600);
    let mut file = options.open(path).map_err(|error| {
        VshotError::Selection(format!(
            "failed to create Qt session file {}: {error}",
            path.display()
        ))
    })?;
    file.write_all(bytes).map_err(|error| {
        VshotError::Selection(format!(
            "failed to write Qt session file {}: {error}",
            path.display()
        ))
    })?;
    file.flush().map_err(|error| {
        VshotError::Selection(format!(
            "failed to flush Qt session file {}: {error}",
            path.display()
        ))
    })
}

fn compact_error(bytes: &[u8]) -> String {
    let mut text = String::from_utf8_lossy(bytes).trim().to_owned();
    while text.len() > MAX_HELPER_ERROR_BYTES {
        text.pop();
    }
    text
}

/// Reads one editing session's answer.  Every mode that can edit goes through
/// here, so the region, the marks and the two answers that decide what happens
/// to the image all come back together.
fn parse_outcome(bytes: Vec<u8>, bounds: Rect) -> Result<SelectionOutcome> {
    let result: QtResult = serde_json::from_slice(&bytes).map_err(|error| {
        VshotError::Selection(format!("Qt helper returned invalid result JSON: {error}"))
    })?;
    match result.status.as_str() {
        "cancelled" => Err(VshotError::SelectionCancelled),
        "ok" => {
            let selection = result
                .selection
                .ok_or_else(|| VshotError::Selection("Qt result has no selection".into()))?
                .into_rect("Qt selection")?;
            let selection = selection.intersection(bounds).ok_or_else(|| {
                VshotError::Selection("Qt selection does not intersect the frozen scene".into())
            })?;
            if selection.size.width < 5 || selection.size.height < 5 {
                return Err(VshotError::Selection(
                    "Qt helper returned a selection smaller than 5x5".into(),
                ));
            }
            Ok(SelectionOutcome {
                rect: selection,
                long: result.long,
                pin: result.pin.unwrap_or(false),
                // Read off the channel by the caller, which is where the pixels
                // arrive: parsing the answer and reading the buffers are two
                // steps, and this one never blocks.
                composite: None,
                // An empty array is "nothing was drawn", the same as an absent
                // field: a pin made from an untouched capture has no marks to
                // reopen on, and handing the daemon an empty list would only
                // make it store one.
                marks: result.marks.filter(|marks| match marks {
                    serde_json::Value::Array(items) => !items.is_empty(),
                    _ => true,
                }),
            })
        }
        status => Err(VshotError::Selection(format!(
            "Qt helper returned unknown status `{status}`"
        ))),
    }
}

fn parse_picked_window(bytes: Vec<u8>, bounds: Rect) -> Result<PickedWindow> {
    let result: QtResult = serde_json::from_slice(&bytes).map_err(|error| {
        VshotError::Selection(format!("Qt helper returned invalid result JSON: {error}"))
    })?;
    match result.status.as_str() {
        "cancelled" => Err(VshotError::SelectionCancelled),
        "ok" => {
            let rect = result
                .selection
                .ok_or_else(|| VshotError::Selection("Qt result has no selection".into()))?
                .into_rect("Qt selection")?;
            let rect = rect.intersection(bounds).ok_or_else(|| {
                VshotError::Selection("Qt selection does not intersect the captured desktop".into())
            })?;
            let point = result
                .point
                .map(|point| parse_point(point, "picked point"))
                .transpose()?;
            Ok(PickedWindow { rect, point })
        }
        status => Err(VshotError::Selection(format!(
            "Qt helper returned unknown status `{status}`"
        ))),
    }
}

/// One `{x,y}` pair of the wire, as the point type the geometry uses.
fn parse_point(point: WirePoint, label: &str) -> Result<Point> {
    Ok(Point::new(
        i32::try_from(point.x)
            .map_err(|_| VshotError::Selection(format!("{label} x is out of range")))?,
        i32::try_from(point.y)
            .map_err(|_| VshotError::Selection(format!("{label} y is out of range")))?,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    // The helper talks to the CLI in one JSON object per line, and the session's
    // own result is whatever is left over.  So the split between "a request" and
    // "the result" is the whole protocol, and getting it wrong in either
    // direction is silent: a request read as a result ends the session early,
    // and a result read as a request leaves the CLI answering something that
    // was never asked.
    #[test]
    fn a_helpers_line_is_either_a_request_or_the_result() {
        let pointer: HelperRequest = serde_json::from_str(r#"{"request":"pointer","x":12,"y":34}"#)
            .expect("a pointer request");
        match pointer {
            HelperRequest::Pointer { x, y } => assert_eq!((x, y), (12, 34)),
            other => panic!("read as {other:?}"),
        }
        assert!(matches!(
            serde_json::from_str::<HelperRequest>(r#"{"request":"candidates"}"#).unwrap(),
            HelperRequest::Candidates
        ));

        // A result is not a request: it has no `request` field, or one this
        // build does not know, and either way it ends the dialogue rather than
        // being answered.
        for result in [
            r#"{"rect":{"x":0,"y":0,"width":10,"height":10}}"#,
            r#"{"request":"something-newer"}"#,
            r#"{}"#,
        ] {
            assert!(
                serde_json::from_str::<HelperRequest>(result).is_err(),
                "{result} was taken for a request"
            );
        }
    }

    // The whole dialogue, against a stub helper that plays the Qt side: it asks
    // the CLI for a pointer move and then reports a result, and what is asserted
    // is that the CLI answered the request on the helper's stdin *and* still
    // read the result that followed it.  Answering is the easy half; the half
    // that breaks is the loop ending on the request instead of on the result,
    // or the answer never being flushed and the helper waiting for it forever.
    #[test]
    fn the_cli_answers_a_pointer_request_and_still_reads_the_result() {
        // The stub answers nothing, so an EOF here means the CLI never
        // replied: it exits non-zero and the test fails on the helper's exit
        // status, which is the assertion that the request was answered rather
        // than merely received.
        let script = r#"
printf '%s\n' '{"request":"pointer","x":7,"y":9}'
read -r answer
[ "$answer" = "{}" ] || { printf 'unanswered: %s\n' "$answer" >&2; exit 1; }
printf '%s\n' '{"status":"ok","selection":{"x":1,"y":2,"width":30,"height":40}}'
"#;
        let directory = tempfile::tempdir().unwrap();
        let helper = directory.path().join("vshot-qt-ui");
        std::fs::write(&helper, script).unwrap();
        std::fs::set_permissions(&helper, std::os::unix::fs::PermissionsExt::from_mode(0o755))
            .unwrap();
        let session_path = directory.path().join("session.json");
        std::fs::write(&session_path, "{}").unwrap();

        let scene = test_scene();
        let helper_lookup = HelperLookup {
            path: helper,
            searched: Vec::new(),
        };
        let output = run_interactive_helper(
            &helper_lookup,
            &session_path,
            &scene,
            &|| None,
            &mut |_, _| Ok("{}".to_string()),
        )
        .expect("the dialogue runs to the end");
        let parsed = parse_outcome(output.json, scene.bounds()).expect("a result");
        assert_eq!(parsed.rect, Rect::new(1, 2, 30, 40));
    }

    // The pin editor walks the same keyboard cursor the region editor does, so
    // it asks for a pointer warp the same way -- which means its helper has to
    // be run with a stdin to be answered on, not the null the pin path used to
    // give it.  A stub that never gets an answer writes a short result and
    // exits non-zero, so the CLI's own error is the assertion.
    #[test]
    fn the_pin_editor_is_answered_on_its_request_pipe_too() {
        let script = r#"
printf '%s\n' '{"request":"pointer","x":41,"y":17}'
read -r answer
[ "$answer" = "{}" ] || { printf 'unanswered: %s\n' "$answer" >&2; exit 1; }
printf '%s\n' '{"status":"ok","selection":{"x":0,"y":0,"width":40,"height":40}}'
"#;
        let directory = tempfile::tempdir().unwrap();
        let helper = directory.path().join("vshot-qt-ui");
        std::fs::write(&helper, script).unwrap();
        std::fs::set_permissions(&helper, std::os::unix::fs::PermissionsExt::from_mode(0o755))
            .unwrap();
        // The daemon's own session shape, with the desktop it stamps for the
        // pointer request: `bounds` is the pin and is deliberately not it.
        let session_path = directory.path().join("session.json");
        std::fs::write(
            &session_path,
            br#"{"version":1,"mode":"pin-edit",
                 "bounds":{"x":900,"y":40,"width":40,"height":40},
                 "desktop":{"x":0,"y":0,"width":1920,"height":1080}}"#,
        )
        .unwrap();

        let pin =
            crate::model::Frame::new(crate::geometry::Size::new(40, 40), vec![0u8; 40 * 40 * 4])
                .unwrap();
        let helper_lookup = HelperLookup {
            path: helper,
            searched: Vec::new(),
        };
        let output = run_pin_session(&helper_lookup, &session_path, &pin, &mut |_, _| {
            Ok("{}".to_string())
        })
        .expect("the pin dialogue runs to the end");
        let edited = parse_edit_result(output.json, Rect::new(900, 40, 40, 40))
            .expect("a result")
            .expect("not cancelled");
        assert_eq!(edited.selection, Rect::new(0, 0, 40, 40));
    }

    // The release is the whole handoff in one exchange: the editor writes its
    // result and then asks to be let go, and the CLI runs the caller's handoff
    // with that result before answering.  Two things have to hold for the
    // marks not to blink out -- the result is read *before* the answer goes
    // back, and the answer goes back at all, or the editor waits on a line that
    // never comes and only its own backstop ends it.
    #[test]
    fn the_editor_is_let_go_only_after_its_result_was_taken() {
        let script = r#"
printf '%s\n' '{"status":"ok","selection":{"x":0,"y":0,"width":40,"height":40}}'
printf '%s\n' '{"request":"release"}'
read -r answer
[ "$answer" = "{}" ] || { printf 'unanswered: %s\n' "$answer" >&2; exit 1; }
"#;
        let directory = tempfile::tempdir().unwrap();
        let helper = directory.path().join("vshot-qt-ui");
        std::fs::write(&helper, script).unwrap();
        std::fs::set_permissions(&helper, std::os::unix::fs::PermissionsExt::from_mode(0o755))
            .unwrap();
        let session_path = directory.path().join("session.json");
        std::fs::write(
            &session_path,
            br#"{"version":1,"mode":"pin-edit",
                 "bounds":{"x":900,"y":40,"width":40,"height":40},
                 "desktop":{"x":0,"y":0,"width":1920,"height":1080}}"#,
        )
        .unwrap();
        let pin =
            crate::model::Frame::new(crate::geometry::Size::new(40, 40), vec![0u8; 40 * 40 * 4])
                .unwrap();
        let helper_lookup = HelperLookup {
            path: helper,
            searched: Vec::new(),
        };
        // What the handoff was given, and how many times: a session has one
        // release, and a second would mean the CLI answered twice.
        let mut taken: Vec<Vec<u8>> = Vec::new();
        let output = run_pin_session(&helper_lookup, &session_path, &pin, &mut |result, _| {
            taken.push(result.to_vec());
            Ok("{}".to_string())
        })
        .expect("the pin dialogue runs to the end");
        assert_eq!(taken.len(), 1, "the handoff ran once");
        assert_eq!(
            taken[0], output.json,
            "the handoff saw the very bytes the dialogue collected"
        );
        assert!(parse_edit_result(output.json, Rect::new(900, 40, 40, 40))
            .expect("a result")
            .is_some());
    }

    // A handoff that fails still lets the editor go: it has drawn everything it
    // is going to draw, and leaving it up would be a surface nobody will ever
    // take down.  The failure travels to the caller instead, which is the side
    // that can say what went wrong.
    #[test]
    fn a_failed_handoff_still_ends_the_editor() {
        let script = r#"
printf '%s\n' '{"status":"ok","selection":{"x":0,"y":0,"width":40,"height":40}}'
printf '%s\n' '{"request":"release"}'
read -r answer
[ "$answer" = "{}" ] || { printf 'unanswered: %s\n' "$answer" >&2; exit 1; }
"#;
        let directory = tempfile::tempdir().unwrap();
        let helper = directory.path().join("vshot-qt-ui");
        std::fs::write(&helper, script).unwrap();
        std::fs::set_permissions(&helper, std::os::unix::fs::PermissionsExt::from_mode(0o755))
            .unwrap();
        let session_path = directory.path().join("session.json");
        std::fs::write(
            &session_path,
            br#"{"bounds":{"x":900,"y":40,"width":40,"height":40},
                 "desktop":{"x":0,"y":0,"width":1920,"height":1080}}"#,
        )
        .unwrap();
        let pin =
            crate::model::Frame::new(crate::geometry::Size::new(40, 40), vec![0u8; 40 * 40 * 4])
                .unwrap();
        let helper_lookup = HelperLookup {
            path: helper,
            searched: Vec::new(),
        };
        let error = match run_pin_session(&helper_lookup, &session_path, &pin, &mut |_, _| {
            Err(VshotError::Pin("the pin daemon refused the frame".into()))
        }) {
            Err(error) => error,
            Ok(_) => panic!("a failed handoff is not reported as a finished session"),
        };
        assert!(
            matches!(&error, VshotError::Pin(message) if message.contains("refused the frame")),
            "reported as {error}"
        );
    }

    // The desktop a warp is expressed in is the layout's, and the session's own
    // `bounds` is the pin: reading the wrong one would put the pointer at a
    // fraction of where it belongs whenever the pin is not at the origin.
    #[test]
    fn the_pin_edit_desktop_is_read_apart_from_the_pin_bounds() {
        let directory = tempfile::tempdir().unwrap();
        let session_path = directory.path().join("session.json");
        std::fs::write(
            &session_path,
            br#"{"bounds":{"x":900,"y":40,"width":40,"height":40},
                 "desktop":{"x":-1920,"y":0,"width":3840,"height":1080}}"#,
        )
        .unwrap();
        assert_eq!(
            session_desktop(&session_path).unwrap(),
            Rect::new(-1920, 0, 3840, 1080)
        );

        // A session without one has no desktop, and that is what keeps a warp
        // from being sent against a rectangle that is really the pin.
        std::fs::write(
            &session_path,
            br#"{"bounds":{"x":900,"y":40,"width":40,"height":40}}"#,
        )
        .unwrap();
        assert!(session_desktop(&session_path).is_err());
    }

    /// One small output, enough for a session to be written about it.
    fn test_scene() -> SceneSnapshot {
        let frame =
            crate::model::Frame::new(crate::geometry::Size::new(64, 64), vec![0u8; 64 * 64 * 4])
                .unwrap();
        SceneSnapshot::from_outputs(vec![crate::model::OutputSnapshot::new(
            1,
            "TEST-1",
            Rect::new(0, 0, 64, 64),
            1,
            frame,
        )
        .unwrap()])
        .unwrap()
    }

    #[test]
    fn helper_candidates_cover_sibling_and_buildqt_layouts() {
        let candidates = helper_candidates(Some(Path::new("/opt/vshot/bin")));
        assert_eq!(
            candidates,
            vec![
                PathBuf::from("/opt/vshot/bin/vshot-qt-ui"),
                PathBuf::from("/opt/vshot/bin/../build-qt/vshot-qt-ui"),
                PathBuf::from("/opt/vshot/bin/../../build-qt/vshot-qt-ui"),
            ]
        );
        assert!(helper_candidates(None).is_empty());
    }

    // A pin the user has zoomed is annotated on an image that is not a whole
    // number of device pixels per logical pixel.  The session has to declare
    // that ratio and the frame's own pixel size together, or the editor's
    // dimension check refuses it and the pin cannot be edited at all.
    #[test]
    fn a_zoomed_pin_edit_session_declares_the_ratio_and_the_frames_own_size() {
        let frame =
            crate::model::Frame::new(crate::geometry::Size::new(160, 90), vec![0u8; 160 * 90 * 4])
                .unwrap();
        let window = Rect::new(300, 200, 176, 99);
        let scale = crate::edit::Scale::ratio(frame.size().width, window.size.width);
        let (_directory, path) = write_pin_edit_session(&PinEditSpec {
            frame: &frame,
            output_name: "DP-1",
            window,
            scale,
            socket: Path::new("/tmp/vshot-test.sock"),
            pin_id: 7,
            action: "",
            annotations: None,
            border_width: 0,
            desktop: Some(Rect::new(0, 0, 1920, 1080)),
        })
        .unwrap();

        let session: serde_json::Value =
            serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        let output = &session["outputs"][0];
        // The desktop the pointer walk converts against is written apart from
        // the pin's own bounds, because the pin is not the screen.
        assert_eq!(session["desktop"]["width"].as_u64(), Some(1920));
        assert_eq!(session["bounds"]["width"].as_u64(), Some(176));
        // 160 pixels across a 176-logical-pixel window is the 1.1x zoom the
        // wheel's first notch gives -- 0.909, not a whole number.
        let declared = output["scale"].as_f64().unwrap();
        assert!((declared - 160.0 / 176.0).abs() < 1e-12, "{declared}");
        // The frame is written at its own size, and the product of that size
        // and the ratio is the logical rect, so the editor's check passes.
        assert_eq!(output["pixel_width"].as_u64(), Some(160));
        assert_eq!(output["pixel_height"].as_u64(), Some(90));
        assert_eq!(output["width"].as_u64(), Some(176));
        assert_eq!(output["height"].as_u64(), Some(99));
        assert!((declared * 176.0 - 160.0).abs() < 1e-9);
        assert!((declared * 99.0 - 90.0).abs() < 1e-9);
    }

    // The daemon keeps the marks and the border width and hands both back on a
    // re-edit, so the session the helper reads has to carry them.  The marks
    // travel as the wire's own JSON, untouched, because re-spelling them here
    // would be a second place for that shape to drift; the border width is a
    // number the editor needs to know the pin's rim is part of the pin.
    #[test]
    fn a_pin_edit_session_carries_the_marks_and_the_border_width() {
        let frame = crate::model::Frame::new(
            crate::geometry::Size::new(240, 180),
            vec![0u8; 240 * 180 * 4],
        )
        .unwrap();
        let window = Rect::new(150, 150, 240, 180);
        let marks = serde_json::json!([
            {"kind": "shape", "tool": "rectangle", "rect": {"x": 10, "y": 12, "width": 40, "height": 30}},
        ]);
        let (_directory, path) = write_pin_edit_session(&PinEditSpec {
            frame: &frame,
            output_name: "DP-1",
            window,
            scale: crate::edit::Scale::ratio(240, 240),
            socket: Path::new("/tmp/vshot-test.sock"),
            pin_id: 3,
            action: "",
            annotations: Some(&marks),
            border_width: 6,
            desktop: None,
        })
        .unwrap();

        let session: serde_json::Value =
            serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        assert_eq!(session["annotations"], marks);
        assert_eq!(session["border_width"].as_u64(), Some(6));

        // A first edit has neither, and the fields are left out rather than
        // written as nulls: the helper reads an absent field as "no marks" and
        // "no border", which is what a pin nobody has annotated wants.
        let (_directory, bare) = write_pin_edit_session(&PinEditSpec {
            frame: &frame,
            output_name: "DP-1",
            window,
            scale: crate::edit::Scale::ratio(240, 240),
            socket: Path::new("/tmp/vshot-test.sock"),
            pin_id: 3,
            action: "",
            annotations: None,
            border_width: 0,
            desktop: None,
        })
        .unwrap();
        let session: serde_json::Value =
            serde_json::from_slice(&std::fs::read(&bare).unwrap()).unwrap();
        assert!(session.get("annotations").is_none());
        assert_eq!(session["border_width"].as_u64(), Some(0));
    }

    #[test]
    fn rejects_cancelled_and_invalid_selection() {
        assert!(matches!(
            parse_outcome(
                br#"{"status":"cancelled"}"#.to_vec(),
                Rect::new(0, 0, 20, 20)
            ),
            Err(VshotError::SelectionCancelled)
        ));
        assert!(parse_outcome(
            br#"{"status":"ok","selection":{"x":0,"y":0,"width":4,"height":5}}"#.to_vec(),
            Rect::new(0, 0, 20, 20),
        )
        .is_err());
    }

    #[test]
    fn session_wire_rect_preserves_negative_origin() {
        let rect = WireRect::from(Rect::new(-5, 7, 12, 13));
        assert_eq!(rect.into_rect("rect").unwrap(), Rect::new(-5, 7, 12, 13));
    }

    #[test]
    fn compacts_unicode_errors_without_splitting_utf8() {
        let text = "错误".repeat(MAX_HELPER_ERROR_BYTES);
        let compacted = compact_error(text.as_bytes());
        assert!(compacted.len() <= MAX_HELPER_ERROR_BYTES);
        assert!(std::str::from_utf8(compacted.as_bytes()).is_ok());
    }

    #[test]
    fn parses_a_translate_result() {
        let bytes =
            r#"{"status":"ok","action":"","translated_text":"你好","image_path":"/tmp/t.png"}"#
                .as_bytes()
                .to_vec();
        let outcome = parse_translate_result(bytes).unwrap();
        assert_eq!(outcome.text, "你好");
        assert_eq!(outcome.image_path.as_deref(), Some(Path::new("/tmp/t.png")));
        // A cancel is the same error every other overlay returns.
        assert!(matches!(
            parse_translate_result(br#"{"status":"cancelled"}"#.to_vec()),
            Err(VshotError::SelectionCancelled)
        ));
        // An `ok` without the text is an error, not an empty translation.
        assert!(matches!(
            parse_translate_result(br#"{"status":"ok"}"#.to_vec()),
            Err(VshotError::Translate(_))
        ));
    }

    #[test]
    fn a_translate_session_carries_its_languages_and_result_path() {
        // The wire shape the Qt helper parses: the language pair, provider and
        // destination path, under the new `translate` mode.
        fn session<'a>(
            translate: Option<QtTranslate<'a>>,
            result_path: Option<String>,
        ) -> QtSession<'a> {
            QtSession {
                version: 1,
                mode: "translate",
                bounds: Rect::new(0, 0, 10, 10).into(),
                window: None,
                socket: None,
                id: None,
                border_width: None,
                action: None,
                candidates: None,
                selection: None,
                long_allowed: None,
                annotations: None,
                translate,
                result_path,
                desktop: None,
                outputs: Vec::new(),
            }
        }
        let with = session(
            Some(QtTranslate {
                from: "auto",
                to: "zh-Hans",
                provider: "google",
            }),
            Some("/tmp/translated.png".to_owned()),
        );
        let parsed: serde_json::Value =
            serde_json::from_str(&serde_json::to_string(&with).unwrap()).unwrap();
        assert_eq!(parsed["mode"], "translate");
        assert_eq!(parsed["translate"]["from"], "auto");
        assert_eq!(parsed["translate"]["to"], "zh-Hans");
        assert_eq!(parsed["translate"]["provider"], "google");
        assert_eq!(parsed["result_path"], "/tmp/translated.png");
        // A mode that does not translate leaves both fields out, so an older
        // helper sees exactly the session it always saw.
        let without = session(None, None);
        let parsed: serde_json::Value =
            serde_json::from_str(&serde_json::to_string(&without).unwrap()).unwrap();
        assert!(parsed.get("translate").is_none());
        assert!(parsed.get("result_path").is_none());
    }
}

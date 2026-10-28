// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Vidow, over its own IPC.
//!
//! Vidow is a wlroots compositor with a scrolling tiling layout, and it answers
//! questions about itself on a unix socket: one line of JSON in, one line back
//! (`src/ipc.c` in its tree).  That socket is the only route to the things
//! vshot needs here — there is no `hyprctl`-style client to run.  Vidow does
//! build one (`vidowmsg`), but its meson marks that target `install: false`, so
//! a session running vidow from its build tree has the socket and no command to
//! speak to it with; this module speaks the protocol itself.
//!
//! The socket is named after the display vidow created:
//! `$XDG_RUNTIME_DIR/vidow-$WAYLAND_DISPLAY.sock`.  A client *on* vidow's
//! display therefore derives the path from its own `WAYLAND_DISPLAY`, which is
//! what makes [`owns_this_connection`] decisive the way niri's socket name is
//! (see [`super::niri::owns_this_connection`]): the name carrying this client's
//! display is what says the vidow answering is ours, and a vidow running nested
//! inside another compositor cannot be mistaken for it.  Vidow also hands
//! `VIDOW_SOCKET` to the processes its own `config.lua` starts — that is
//! checked first, because a hand-written `ipc_socket` in the config makes the
//! derived name wrong.
//!
//! What each query answers, and why vshot asks it:
//!
//! * `windows` — every window vidow manages, with its layout geometry, its
//!   `app_id` / `title`, its `pid`, the output it is on, and a `focused` flag.
//!   This is `window active` (the flag), the picker's list, and the pid
//!   `record --app-audio` needs.
//! * `pick-window X Y` — the window at a point, hit-tested by vidow itself
//!   (`wlr_scene_node_at`), which is the same answer the pointer gets.  A list
//!   walk cannot reproduce it: vidow's list is in creation order, not stacking
//!   order, and floating windows sit above tiled ones.
//! * `cursor` — the pointer's position in layout coordinates, for "which output
//!   is the user on" (the pin daemon asks, and cannot see the pointer itself).
//! * `outputs` — each output's layout rect, scale and `focused` flag, which is
//!   the rest of that answer.
//!
//! Coordinates are all in vidow's *layout* space — the same space the captured
//! scene is described in, and the same space the other compositors' probes
//! report windows in — so nothing here needs converting.

use std::ffi::{OsStr, OsString};
use std::io::{BufRead, BufReader, Read, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::time::Duration;

use serde_json::{json, Value};

use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect};

use super::active_output::ActiveOutput;
use super::window::{join_label, ActiveWindow, WindowCandidate, WindowCommandRunner, WindowSource};
use super::window_copy::Name;

/// How long one request may take.
///
/// Vidow answers on its own event loop, so a socket that accepts and then says
/// nothing means the compositor is stuck rendering or blocked on a client.
/// Waiting on it forever would leave a screenshot command hanging with no way
/// to say why, and every query here is one the compositor can answer from
/// state it already holds.
const REQUEST_TIMEOUT: Duration = Duration::from_secs(2);

/// The most of one reply this side will read.  Vidow caps a *request* line at
/// 1 MiB, and a one-shot reply is bounded by what was asked for — nothing asked
/// for here is a whole-session snapshot (`state`) — but the cap is what keeps a
/// broken or hostile answer from being read into memory without end.
const MAX_REPLY: u64 = 8 * 1024 * 1024;

/// Every socket this client could be talking to, in the order they are tried.
///
/// The order is the one that has to hold for a *client*: `VIDOW_SOCKET` when
/// vidow's own config started us (it is the only source that is right when the
/// config names an `ipc_socket` by hand), then the name derived from the
/// display we are on.
pub(crate) fn socket_candidates() -> Vec<PathBuf> {
    let explicit = std::env::var_os("VIDOW_SOCKET");
    let runtime = std::env::var_os("XDG_RUNTIME_DIR");
    let display = std::env::var_os("WAYLAND_DISPLAY");
    // SAFETY: `getuid` takes no arguments and touches no memory.
    let uid = unsafe { libc::getuid() };
    socket_candidates_from(
        explicit.as_deref(),
        runtime.as_deref(),
        display.as_deref(),
        uid,
    )
}

/// See [`socket_candidates`]; split out so the rule can be tested without
/// touching the environment of a test runner.
fn socket_candidates_from(
    explicit: Option<&OsStr>,
    runtime: Option<&OsStr>,
    display: Option<&OsStr>,
    uid: u32,
) -> Vec<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(explicit) = explicit.filter(|value| !value.is_empty()) {
        candidates.push(PathBuf::from(explicit));
    }
    match (
        runtime.filter(|value| !value.is_empty()),
        display.filter(|value| !value.is_empty()),
    ) {
        (Some(runtime), Some(display)) => {
            let mut name = OsString::from("vidow-");
            name.push(display);
            name.push(".sock");
            candidates.push(Path::new(runtime).join(name));
        }
        // Vidow's own fallback when it cannot name a runtime directory:
        // `/tmp/vidow-<uid>.sock` (`ipc_path_build`, `src/ipc.c`).
        _ => candidates.push(PathBuf::from(format!("/tmp/vidow-{uid}.sock"))),
    }
    candidates
}

/// The socket that answers, i.e. the vidow this client is connected to.
///
/// Each candidate is asked the cheapest question the protocol has; the first
/// that replies with `ok` is the compositor.  A stale `VIDOW_SOCKET` — a nested
/// vidow that has since exited, while the session's own vidow is still there —
/// therefore falls through to the derived name rather than taking the whole
/// session down with it.
pub(crate) fn live_socket() -> Option<PathBuf> {
    // The socket a session talks to does not move for the life of this
    // process, and the probe is a connect and a round trip.  Without the
    // cache every `window_at` during a pick -- and every `Session::detect`
    // behind it -- pays for it again; the picker calls this many times a
    // second while the pointer moves.
    static LIVE: std::sync::OnceLock<Option<PathBuf>> = std::sync::OnceLock::new();
    LIVE.get_or_init(|| {
        socket_candidates()
            .into_iter()
            .find(|path| ask_raw(path, &json!({ "cmd": "focused" })).is_ok())
    })
    .clone()
}

/// Is the vidow answering on this display the compositor of *this* session?
///
/// The same judgement [`super::niri::owns_this_connection`] makes, and it is
/// needed for the same reason: a compositor nested inside another one inherits
/// the outer session's desktop variables, so the socket is the only evidence
/// that is about the display we are actually on.
pub(crate) fn owns_this_connection() -> bool {
    live_socket().is_some()
}

/// One request to whichever socket answers, and its reply.
///
/// This is what [`WindowCommandRunner::ask`] calls in production, so the
/// vidow queries below are driven through a runner and can be tested without a
/// compositor.
pub(crate) fn ask_live(request: &Value) -> Result<Value> {
    let socket = live_socket().ok_or_else(|| {
        VshotError::VidowIpc(format!(
            "no vidow IPC socket answers for this display (tried {})",
            socket_candidates()
                .iter()
                .map(|path| path.display().to_string())
                .collect::<Vec<_>>()
                .join(", ")
        ))
    })?;
    ask_raw(&socket, request)
}

/// One request, one reply, over `socket`.
fn ask_raw(socket: &Path, request: &Value) -> Result<Value> {
    let mut line = serde_json::to_vec(request)
        .map_err(|error| VshotError::VidowIpc(format!("cannot encode {request}: {error}")))?;
    line.push(b'\n');
    let mut stream = UnixStream::connect(socket)
        .map_err(|source| VshotError::VidowIpc(format!("{}: {source}", socket.display())))?;
    stream
        .set_read_timeout(Some(REQUEST_TIMEOUT))
        .and_then(|()| stream.set_write_timeout(Some(REQUEST_TIMEOUT)))
        .map_err(|source| VshotError::VidowIpc(format!("{}: {source}", socket.display())))?;
    stream
        .write_all(&line)
        .and_then(|()| stream.flush())
        .map_err(|source| VshotError::VidowIpc(format!("{}: {source}", socket.display())))?;
    let mut reader = BufReader::new(stream).take(MAX_REPLY);
    let mut reply = Vec::new();
    reader
        .read_until(b'\n', &mut reply)
        .map_err(|source| VshotError::VidowIpc(format!("{}: {source}", socket.display())))?;
    if reply.is_empty() {
        return Err(VshotError::VidowIpc(format!(
            "{} closed without answering `{}`",
            socket.display(),
            command_of(request)
        )));
    }
    let value: Value = serde_json::from_slice(&reply).map_err(|error| {
        VshotError::VidowIpc(format!(
            "{} answered `{}` with invalid JSON: {error}",
            socket.display(),
            command_of(request)
        ))
    })?;
    // `{"ok":false,"error":"…"}` is vidow refusing, and its reason is the only
    // useful thing in the reply.
    if value.get("ok").and_then(Value::as_bool) != Some(true) {
        let reason = value
            .get("error")
            .and_then(Value::as_str)
            .unwrap_or("no reason given");
        return Err(VshotError::VidowIpc(format!(
            "vidow refused `{}`: {reason}",
            command_of(request)
        )));
    }
    Ok(value)
}

fn command_of(request: &Value) -> &str {
    request.get("cmd").and_then(Value::as_str).unwrap_or("?")
}

/// The output the user is on, asked the way the session probe in
/// [`super::active_output`] needs it: over the socket, with no runner in
/// between, because that file's other probes are bare commands too.
pub(crate) fn active_output() -> Option<ActiveOutput> {
    Vidow::new(&super::window::ProcessWindowRunner)
        .active_output()
        .ok()
        .flatten()
}

/// Vidow's own answers, through a runner so the parsing can be tested without
/// a compositor — the arrangement every other compositor's probe uses.
pub(crate) struct Vidow<'a, R: WindowCommandRunner> {
    runner: &'a R,
}

impl<'a, R: WindowCommandRunner> Vidow<'a, R> {
    pub(crate) fn new(runner: &'a R) -> Self {
        Self { runner }
    }

    fn ask(&self, command: &str, arguments: Value) -> Result<Value> {
        let mut request = json!({ "cmd": command });
        if let (Some(request), Some(arguments)) = (request.as_object_mut(), arguments.as_object()) {
            for (key, value) in arguments {
                request.insert(key.clone(), value.clone());
            }
        }
        let reply = self.runner.ask(&request)?;
        reply.get("result").cloned().ok_or_else(|| {
            VshotError::VidowIpc(format!(
                "vidow's `{command}` reply has no `result`: {reply}"
            ))
        })
    }

    /// The focused window, as `window active`, `--follow` and
    /// `record window active` want it.
    pub(crate) fn active_window(&self) -> Result<ActiveWindow> {
        parse_active_window(&self.ask("windows", json!({}))?)
    }

    /// Every window the user can point at, for the picker's list.
    pub(crate) fn windows(&self) -> Result<Vec<WindowCandidate>> {
        Ok(parse_windows(&self.ask("windows", json!({}))?))
    }

    /// The window at `point`, hit-tested by vidow itself.
    pub(crate) fn window_at(&self, point: Point) -> Result<Option<WindowCandidate>> {
        let result = self.ask("pick-window", json!({ "x": point.x, "y": point.y }))?;
        Ok(result.get("window").and_then(candidate_of))
    }

    /// The pid of the process behind `name`, for `record --app-audio`.
    pub(crate) fn pid_for(&self, name: &Name) -> Result<Option<i32>> {
        Ok(pid_in(&self.ask("windows", json!({}))?, name))
    }

    /// The output the user is on: the one under the pointer, else the focused
    /// one.
    pub(crate) fn active_output(&self) -> Result<Option<ActiveOutput>> {
        let cursor = parse_cursor(&self.ask("cursor", json!({}))?);
        let outputs = parse_outputs(&self.ask("outputs", json!({}))?);
        Ok(pick_output(&outputs, cursor))
    }
}

/// One window object, as the picker wants it: `None` for a window the user
/// cannot point at.
fn candidate_of(window: &Value) -> Option<WindowCandidate> {
    if !pointable(window) {
        return None;
    }
    Some(WindowCandidate {
        geometry: window_rect(window)?,
        label: join_label(app_id_of(window), title_of(window)),
        app_id: app_id_of(window).to_owned(),
        title: title_of(window).to_owned(),
        // Vidow's own window id is not a capture handle: a window capture goes
        // through `ext_image_copy_capture`'s toplevel source, which names the
        // window by its protocol handle (see `capture::window_copy`).
        handle: None,
    })
}

fn parse_windows(result: &Value) -> Vec<WindowCandidate> {
    result
        .as_array()
        .map(|windows| windows.iter().filter_map(candidate_of).collect())
        .unwrap_or_default()
}

/// The window vidow reports as focused.
///
/// Vidow marks the focused window on the window object itself (`"focused"`), so
/// the flag on the list *is* the answer — there is no second query to make, and
/// no way for the two to disagree.
///
/// A window with no focus at all is a state rather than a broken reply — the
/// desktop itself has the keyboard — and it gets the same sentence Hyprland's
/// empty reply gets, because the callers (`record window active`, a replay's
/// standby state) relay it to the user.
fn parse_active_window(result: &Value) -> Result<ActiveWindow> {
    let windows = result.as_array().ok_or_else(|| {
        VshotError::ActiveWindowUnavailable("vidow's window list is not an array".into())
    })?;
    let focused = windows
        .iter()
        .find(|window| window.get("focused").and_then(Value::as_bool) == Some(true));
    let Some(window) = focused else {
        return Err(VshotError::ActiveWindowUnavailable(
            "no window has the focus".into(),
        ));
    };
    let geometry = window_rect(window).ok_or_else(|| {
        VshotError::ActiveWindowUnavailable(format!(
            "vidow reports no geometry for the focused window {}",
            id_of(window).unwrap_or(0)
        ))
    })?;
    Ok(ActiveWindow {
        geometry: Some(geometry),
        source: WindowSource::Vidow,
        app_id: app_id_of(window).to_owned(),
        title: title_of(window).to_owned(),
        // Vidow's window id is what every command that aims at one window
        // takes (`{"cmd":"focus-window","id":3}`), so it is a name a later
        // query can be aimed at — which is what `handle` is for.
        handle: id_of(window).map(|id| id.to_string()),
    })
}

/// The pid of the window `name` describes.
///
/// The window is matched the way a window recording matched it — by app id and
/// title together, then by title alone, the same preference
/// [`super::window_pid`] applies on the other compositors — so the pid belongs
/// to the window being recorded.  Among several windows that share both (two
/// windows of one application with the same title) the first is taken; the
/// alternative is refusing, which would leave `--app-audio` unusable on a
/// desktop that happens to have two.
fn pid_in(result: &Value, name: &Name) -> Option<i32> {
    let windows = result.as_array()?;
    let pid_of = |window: &Value| {
        window
            .get("pid")
            .and_then(Value::as_i64)
            .and_then(|pid| i32::try_from(pid).ok())
            .filter(|pid| *pid > 0)
    };
    let exact = windows
        .iter()
        .find(|window| app_id_of(window) == name.app_id && title_of(window) == name.title);
    let chosen = exact.or_else(|| {
        windows
            .iter()
            .find(|window| !name.title.is_empty() && title_of(window) == name.title)
    });
    chosen.and_then(pid_of)
}

/// Whether the user can see and point at this window.
///
/// Vidow keeps a view in its list while it is unmapped, minimized, scrolled out
/// of the viewport (`visible` is false for a parked window), or a popup; none of
/// those is something a picker should offer or a screenshot should land on.
fn pointable(window: &Value) -> bool {
    let flag = |key: &str| window.get(key).and_then(Value::as_bool).unwrap_or(false);
    flag("mapped") && flag("visible") && !flag("minimized") && !flag("popup")
}

/// A window's geometry box: `{x, y, width, height}` in layout coordinates.
/// An empty box is no answer — there is nothing there to point at.
fn window_rect(window: &Value) -> Option<Rect> {
    let geometry = window.get("geometry")?;
    let number = |key: &str| geometry.get(key).and_then(Value::as_i64);
    let x = i32::try_from(number("x")?).ok()?;
    let y = i32::try_from(number("y")?).ok()?;
    let width = u32::try_from(number("width")?).ok()?;
    let height = u32::try_from(number("height")?).ok()?;
    (width > 0 && height > 0).then(|| Rect::new(x, y, width, height))
}

fn app_id_of(window: &Value) -> &str {
    window.get("app_id").and_then(Value::as_str).unwrap_or("")
}

fn title_of(window: &Value) -> &str {
    window.get("title").and_then(Value::as_str).unwrap_or("")
}

fn id_of(window: &Value) -> Option<u64> {
    window.get("id").and_then(Value::as_u64)
}

/// `cursor`'s position in layout coordinates.  Vidow reports it as a number
/// rather than an integer (it tracks a fractional pointer position), and a
/// point is what the layout rects are compared against.
fn parse_cursor(result: &Value) -> Option<Point> {
    let coordinate = |key: &str| {
        let value = result.get(key)?.as_f64()?;
        i32::try_from(value.round() as i64).ok()
    };
    Some(Point::new(coordinate("x")?, coordinate("y")?))
}

/// One output, as much of it as choosing the active one needs.
#[derive(Clone, Debug, Eq, PartialEq)]
struct VidowOutput {
    name: String,
    enabled: bool,
    focused: bool,
    /// The output's rect in the global layout, which is already logical — vidow
    /// reports the layout in logical coordinates and the mode size separately
    /// (`width` / `height` are physical), so nothing has to be divided by the
    /// scale the way Hyprland's monitor list does.
    rect: Rect,
}

fn parse_outputs(result: &Value) -> Vec<VidowOutput> {
    let Some(outputs) = result.as_array() else {
        return Vec::new();
    };
    outputs
        .iter()
        .filter_map(|output| {
            let name = output.get("name").and_then(Value::as_str)?.to_owned();
            let layout = output.get("layout")?;
            let number = |key: &str| layout.get(key).and_then(Value::as_i64);
            let rect = Rect::new(
                i32::try_from(number("x")?).ok()?,
                i32::try_from(number("y")?).ok()?,
                u32::try_from(number("width")?).ok()?,
                u32::try_from(number("height")?).ok()?,
            );
            Some(VidowOutput {
                name,
                enabled: output
                    .get("enabled")
                    .and_then(Value::as_bool)
                    .unwrap_or(false),
                focused: output
                    .get("focused")
                    .and_then(Value::as_bool)
                    .unwrap_or(false),
                rect,
            })
        })
        .collect()
}

/// The output under the pointer, else the focused one — the rule the Hyprland
/// probe applies, and for the same reason: a pin belongs where the user is
/// pointing at, and on a click-to-focus setup that is not necessarily the
/// output holding the keyboard focus.
fn pick_output(outputs: &[VidowOutput], cursor: Option<Point>) -> Option<ActiveOutput> {
    let usable = |output: &VidowOutput| {
        output.enabled && output.rect.size.width > 0 && output.rect.size.height > 0
    };
    cursor
        .and_then(|point| {
            outputs
                .iter()
                .find(|output| usable(output) && output.rect.contains(point))
        })
        .or_else(|| {
            outputs
                .iter()
                .find(|output| usable(output) && output.focused)
        })
        .map(|output| ActiveOutput {
            name: Some(output.name.clone()),
            rect: Some(output.rect),
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn window(id: u64, app_id: &str, title: &str, x: i64, y: i64, w: i64, h: i64) -> Value {
        json!({
            "id": id,
            "kind": "xdg",
            "title": title,
            "app_id": app_id,
            "pid": 4000 + id,
            "mapped": true,
            "visible": true,
            "minimized": false,
            "popup": false,
            "focused": false,
            "floating": false,
            "output": "DP-1",
            "workspace": "1",
            "geometry": { "x": x, "y": y, "width": w, "height": h },
        })
    }

    fn output(name: &str, x: i64, y: i64, w: i64, h: i64, focused: bool) -> Value {
        json!({
            "name": name,
            "enabled": true,
            "focused": focused,
            "scale": 2,
            "transform": "normal",
            "width": w * 2,
            "height": h * 2,
            "layout": { "x": x, "y": y, "width": w, "height": h },
            "usable_area": { "x": x, "y": y, "width": w, "height": h },
        })
    }

    /// The socket vidow would build for this client: the name carries the
    /// display we are on, which is what makes the guess safe to make.
    #[test]
    fn the_socket_is_named_after_the_display_we_are_on() {
        let candidates = socket_candidates_from(
            None,
            Some(OsStr::new("/run/user/1000")),
            Some(OsStr::new("wayland-1")),
            1000,
        );
        assert_eq!(
            candidates,
            [PathBuf::from("/run/user/1000/vidow-wayland-1.sock")]
        );
    }

    /// `VIDOW_SOCKET` comes first: vidow sets it for the processes its own
    /// config starts, and it is the only source that stays right when the
    /// config names the socket by hand.
    #[test]
    fn an_explicit_socket_is_tried_before_the_derived_name() {
        let candidates = socket_candidates_from(
            Some(OsStr::new("/tmp/hand-written.sock")),
            Some(OsStr::new("/run/user/1000")),
            Some(OsStr::new("wayland-1")),
            1000,
        );
        assert_eq!(
            candidates,
            [
                PathBuf::from("/tmp/hand-written.sock"),
                PathBuf::from("/run/user/1000/vidow-wayland-1.sock"),
            ]
        );
    }

    /// Vidow's own fallback when it has no runtime directory to name
    /// (`/tmp/vidow-<uid>.sock`), including the case of an empty variable: an
    /// exported-but-empty `XDG_RUNTIME_DIR` is not a directory.
    #[test]
    fn with_no_runtime_directory_the_uid_fallback_is_used() {
        let candidates = socket_candidates_from(None, None, Some(OsStr::new("wayland-1")), 1000);
        assert_eq!(candidates, [PathBuf::from("/tmp/vidow-1000.sock")]);
        let candidates =
            socket_candidates_from(None, Some(OsStr::new("")), Some(OsStr::new("wayland-1")), 7);
        assert_eq!(candidates, [PathBuf::from("/tmp/vidow-7.sock")]);
        // No display name either: vidow would not have created a socket, but
        // the fallback is still the only path worth trying.
        let candidates = socket_candidates_from(None, None, None, 7);
        assert_eq!(candidates, [PathBuf::from("/tmp/vidow-7.sock")]);
    }

    #[test]
    fn a_window_is_read_with_its_labels_and_geometry() {
        let list = json!([
            window(3, "Alacritty", "~/Dev", 100, 40, 800, 600),
            window(4, "mpv", "video.mp4", 1000, 40, 400, 300),
        ]);
        assert_eq!(
            parse_windows(&list),
            [
                WindowCandidate {
                    geometry: Rect::new(100, 40, 800, 600),
                    label: "Alacritty — ~/Dev".into(),
                    app_id: "Alacritty".into(),
                    title: "~/Dev".into(),
                    handle: None,
                },
                WindowCandidate {
                    geometry: Rect::new(1000, 40, 400, 300),
                    label: "mpv — video.mp4".into(),
                    app_id: "mpv".into(),
                    title: "video.mp4".into(),
                    handle: None,
                },
            ]
        );
    }

    /// The windows a picker must not offer: hidden, unmapped, minimized, a
    /// popup, or with nothing left to point at.
    #[test]
    fn windows_the_user_cannot_point_at_are_left_out() {
        let mut hidden = window(5, "a", "hidden", 0, 0, 100, 100);
        hidden["visible"] = json!(false);
        let mut minimized = window(6, "b", "minimized", 0, 0, 100, 100);
        minimized["minimized"] = json!(true);
        let mut unmapped = window(7, "c", "unmapped", 0, 0, 100, 100);
        unmapped["mapped"] = json!(false);
        let mut popup = window(8, "d", "popup", 0, 0, 100, 100);
        popup["popup"] = json!(true);
        let empty = window(9, "e", "empty", 10, 10, 0, 0);
        let list = json!([hidden, minimized, unmapped, popup, empty]);
        assert_eq!(parse_windows(&list), []);
        // A reply that is not a list at all is no window rather than an error:
        // the caller decides what a missing list means.
        assert_eq!(parse_windows(&json!({})), []);
    }

    /// `focused` on the window object is the answer, and its geometry and
    /// labels come with it — no second query, and no way for the two to
    /// disagree.
    #[test]
    fn the_focused_window_is_the_one_vidow_flags() {
        let mut focused = window(3, "Alacritty", "~/Dev", 100, 40, 800, 600);
        focused["focused"] = json!(true);
        let list = json!([window(4, "mpv", "video.mp4", 1000, 40, 400, 300), focused]);
        let active = parse_active_window(&list).unwrap();
        assert_eq!(active.source, WindowSource::Vidow);
        assert_eq!(active.geometry, Some(Rect::new(100, 40, 800, 600)));
        assert_eq!(active.app_id, "Alacritty");
        assert_eq!(active.title, "~/Dev");
        // Vidow's window id is what every command that aims at one window
        // takes, so it is carried as the handle.
        assert_eq!(active.handle.as_deref(), Some("3"));
    }

    /// A focused window that is scrolled out of the viewport is still the
    /// focused window — the focus is a property of the session, not of what is
    /// on screen — but a window with no box at all is not an answer.
    #[test]
    fn a_focused_window_with_no_geometry_is_not_an_answer() {
        let mut focused = window(3, "a", "a", 0, 0, 0, 0);
        focused["focused"] = json!(true);
        let error = parse_active_window(&json!([focused])).unwrap_err();
        assert!(format!("{error}").contains("no geometry"), "{error}");
    }

    #[test]
    fn no_focused_window_is_its_own_sentence() {
        let error = parse_active_window(&json!([window(3, "a", "a", 0, 0, 10, 10)])).unwrap_err();
        assert!(
            format!("{error}").contains("no window has the focus"),
            "{error}"
        );
        let error = parse_active_window(&json!({})).unwrap_err();
        assert!(format!("{error}").contains("not an array"), "{error}");
    }

    /// `pick-window` answers with the window under the point, or `null` when
    /// there is nothing there — the compositor's own hit test, which a list
    /// walk cannot reproduce (vidow's list is in creation order).
    #[test]
    fn picking_a_point_uses_vidows_own_hit_test() {
        let reply = json!({
            "window": window(4, "mpv", "video.mp4", 1000, 40, 400, 300),
            "surface_x": 12.5,
            "surface_y": 3.0,
        });
        let picked = reply.get("window").and_then(candidate_of).unwrap();
        assert_eq!(picked.geometry, Rect::new(1000, 40, 400, 300));
        assert_eq!(picked.label, "mpv — video.mp4");
        // A point over the desktop, a layer surface, or a window that cannot
        // be pointed at: no candidate, and that is not an error.
        assert!(json!({ "window": null })
            .get("window")
            .and_then(candidate_of)
            .is_none());
        let mut hidden = window(5, "a", "hidden", 0, 0, 10, 10);
        hidden["visible"] = json!(false);
        assert!(candidate_of(&hidden).is_none());
    }

    /// The pid is matched on the description a window recording matched the
    /// window with: app id and title together first, then title alone.
    #[test]
    fn a_windows_pid_is_found_by_the_labels_the_recording_used() {
        let list = json!([
            window(3, "Alacritty", "~/Dev", 0, 0, 100, 100),
            window(4, "mpv", "video.mp4", 0, 0, 100, 100),
        ]);
        let name = |app_id: &str, title: &str| Name {
            app_id: app_id.into(),
            title: title.into(),
            identifier: String::new(),
        };
        assert_eq!(pid_in(&list, &name("mpv", "video.mp4")), Some(4004));
        // The title alone still resolves a window whose app id the compositor
        // spelled differently.
        assert_eq!(pid_in(&list, &name("MPV", "video.mp4")), Some(4004));
        assert_eq!(pid_in(&list, &name("mpv", "nothing")), None);
        // A pid vidow reported as 0 is an unknown pid, not a pid.
        let mut unknown = window(5, "mpv", "video.mp4", 0, 0, 100, 100);
        unknown["pid"] = json!(0);
        assert_eq!(pid_in(&json!([unknown]), &name("mpv", "video.mp4")), None);
    }

    /// The pointer decides which output is the active one; the focused output
    /// is the fallback.
    #[test]
    fn the_output_under_the_pointer_wins_and_focus_is_the_fallback() {
        let outputs = parse_outputs(&json!([
            output("DP-1", 0, 0, 1920, 1080, false),
            output("DP-2", 1920, 0, 1920, 1080, true),
        ]));
        assert_eq!(outputs.len(), 2);
        // The pointer is on the left output while the keyboard focus is on the
        // right one: the pin belongs where the user is pointing.
        assert_eq!(
            pick_output(&outputs, Some(Point::new(400, 300))),
            Some(ActiveOutput {
                name: Some("DP-1".into()),
                rect: Some(Rect::new(0, 0, 1920, 1080)),
            })
        );
        // No pointer position to read: the focused output answers.
        assert_eq!(
            pick_output(&outputs, None),
            Some(ActiveOutput {
                name: Some("DP-2".into()),
                rect: Some(Rect::new(1920, 0, 1920, 1080)),
            })
        );
        // A pointer in the gap between two outputs, and no focused output:
        // no answer rather than a wrong one.
        let outputs = parse_outputs(&json!([output("DP-1", 0, 0, 1920, 1080, false)]));
        assert_eq!(pick_output(&outputs, Some(Point::new(4000, 300))), None);
    }

    /// A disabled output is not somewhere a pin can go, and vidow reports it
    /// with its geometry still in place.
    #[test]
    fn a_disabled_output_is_not_an_answer() {
        let mut off = output("DP-1", 0, 0, 1920, 1080, true);
        off["enabled"] = json!(false);
        let outputs = parse_outputs(&json!([off]));
        assert_eq!(pick_output(&outputs, None), None);
        assert_eq!(pick_output(&outputs, Some(Point::new(10, 10))), None);
    }

    #[test]
    fn the_pointer_position_is_read_as_a_point() {
        assert_eq!(
            parse_cursor(&json!({ "x": 100, "y": 100, "magnified": false })),
            Some(Point::new(100, 100))
        );
        // Fractional positions are rounded, not truncated, so a pointer just
        // past a boundary lands on the output the user sees it on.
        assert_eq!(
            parse_cursor(&json!({ "x": 1919.6, "y": -0.4 })),
            Some(Point::new(1920, 0))
        );
        assert_eq!(parse_cursor(&json!({ "x": 1 })), None);
    }

    /// A malformed output entry is skipped rather than taking the whole list
    /// with it: one output the compositor described oddly must not hide the
    /// others.
    #[test]
    fn a_malformed_output_does_not_hide_the_rest() {
        let outputs = parse_outputs(&json!([
            { "name": "DP-1" },
            output("DP-2", 0, 0, 1280, 720, true),
        ]));
        assert_eq!(outputs.len(), 1);
        assert_eq!(outputs[0].name, "DP-2");
    }

    /// Runs the queries against a live vidow, which is the only way to catch a
    /// command name or a field spelled wrong: a query vidow does not know looks
    /// exactly like a compositor with nothing to say, and the fallback routes
    /// then answer in its place without any error.
    ///
    /// Ignored by default because it needs one.  Run it with the environment of
    /// a vidow session — its display and its runtime directory — from inside
    /// that session, or pointed at a nested instance:
    ///
    /// ```text
    /// WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 \
    ///     cargo test -- --ignored --nocapture a_live_vidow
    /// ```
    #[test]
    #[ignore = "needs a live vidow session"]
    fn a_live_vidow_answers_its_own_queries() {
        let socket = live_socket().expect("some socket has to answer in a vidow session");
        eprintln!("vidow socket: {}", socket.display());
        assert_eq!(
            super::super::active_output::Session::detect(),
            super::super::active_output::Session::Vidow,
            "the socket naming this display is what settles the session"
        );
        let vidow = Vidow::new(&super::super::window::ProcessWindowRunner);
        let active = vidow.active_output().expect("the outputs query has to run");
        eprintln!("active output: {active:?}");
        assert!(
            active.is_some(),
            "a live vidow has an output for a pin to land on"
        );
        eprintln!("windows: {:?}", vidow.windows());
        eprintln!("focused window: {:?}", vidow.active_window());
        eprintln!(
            "window at (10, 10): {:?}",
            vidow.window_at(Point::new(10, 10))
        );
    }
}

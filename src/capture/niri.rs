//! Window capture on niri, over niri's own IPC.
//!
//! niri is the one compositor where the rectangle-based routes cannot work: its
//! IPC reports window *sizes* but no absolute position for a tiled window.
//! `WindowLayout::tile_pos_in_workspace_view` is filled only for floating
//! windows — the tiled path sets it to `None` on purpose (`src/layout/tile.rs`,
//! `src/layout/scrolling.rs` in niri's tree, which only fills
//! `pos_in_scrolling_layout`, a pair of 1-based column/tile *indices*), and no
//! request reports the scroll offset of a workspace's view.  So "find the rect
//! in the frozen scene" is a guess there, and it is a guess that cannot even be
//! attempted for a long page.
//!
//! niri offers the exact answer instead, and this module drives it:
//!
//! * `Request::FocusedWindow` / `Request::PickWindow` name a window (the
//!   focused one, or the one the user points at with niri's own crosshair
//!   picker);
//! * `Action::ScreenshotWindow { id, path }` makes niri draw that window itself
//!   and write a PNG to an absolute path.
//!
//! The PNG is what niri renders for the window — its surface tree, popups
//! included — at the output's physical scale, and it carries no density of its
//! own, so the caller states the density (see [`window_scale`]).  niri also
//! always puts that image on the clipboard as part of the same action; that is
//! niri's own behaviour and not something this side can turn off.

use std::collections::HashMap;
use std::ffi::OsString;
use std::io::ErrorKind;
use std::path::Path;
use std::process::Output;
use std::time::{Duration, Instant};

use serde_json::Value;

use crate::error::{Result, VshotError};
use crate::model::Frame;

use super::window::{join_label, WindowCommand, WindowCommandRunner};

/// How long niri may take to write the window's PNG.  The action itself returns
/// `Handled` at once and the encoding happens on a thread inside niri, so the
/// file is the only completion signal there is; a failure inside niri is
/// logged there and never reaches the IPC reply, which is why this has to end
/// in a timeout rather than an error of its own.
const SCREENSHOT_TIMEOUT: Duration = Duration::from_secs(5);
/// Wait between two reads of that file.
const POLL_INTERVAL: Duration = Duration::from_millis(25);

/// One window as niri's IPC describes it.  `workspace_id` is what places it on
/// an output: niri reports the workspace → output mapping separately.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct NiriWindow {
    pub id: u64,
    /// `app_id — title`, when niri reports either.
    pub label: String,
    pub workspace_id: Option<u64>,
}

/// The focused window, or `None` when niri has none (a layer-shell surface can
/// hold the focus).
pub fn focused_window<R: WindowCommandRunner>(runner: &R) -> Result<Option<NiriWindow>> {
    let output = run(runner, &command(&["msg", "--json", "focused-window"]))?;
    parse_window(&output.stdout, "focused-window")
}

/// Asks niri to run its own window picker and waits for the answer.  This
/// blocks until the user clicks a window or cancels (`None`); niri draws only a
/// crosshair while it runs.
pub fn pick_window<R: WindowCommandRunner>(runner: &R) -> Result<Option<NiriWindow>> {
    let output = run(runner, &command(&["msg", "--json", "pick-window"]))?;
    parse_window(&output.stdout, "pick-window")
}

/// The output the window sits on and the scale niri lays that output out at.
/// `None` when niri cannot place the window (a workspace on no output, or an
/// output that went away between the two queries).
pub fn window_scale<R: WindowCommandRunner>(
    runner: &R,
    window: &NiriWindow,
) -> Result<Option<(String, u32)>> {
    let Some(workspace_id) = window.workspace_id else {
        return Ok(None);
    };
    let workspaces = run(runner, &command(&["msg", "--json", "workspaces"]))?;
    let Some(name) = parse_workspace_outputs(&workspaces.stdout)?.remove(&workspace_id) else {
        return Ok(None);
    };
    let outputs = run(runner, &command(&["msg", "--json", "outputs"]))?;
    Ok(parse_output_scales(&outputs.stdout)?
        .remove(&name)
        .map(|scale| (name, scale)))
}

/// niri's own screenshot of one window, as a frame.
///
/// The file is written by niri asynchronously, so this reads it until it
/// decodes: a half-written PNG must never be handed on as a result.  `cursor`
/// asks niri to draw the pointer in, which only the builds that know the
/// `show-pointer` argument can do — see [`command_for_window`].
pub fn capture_window<R: WindowCommandRunner>(
    runner: &R,
    window: &NiriWindow,
    cursor: bool,
) -> Result<Frame> {
    capture_window_within(runner, window, cursor, SCREENSHOT_TIMEOUT)
}

/// [`capture_window`] with the wait for niri's file spelled out, so a test can
/// watch a niri that never writes without sitting out the real deadline.
fn capture_window_within<R: WindowCommandRunner>(
    runner: &R,
    window: &NiriWindow,
    cursor: bool,
    timeout: Duration,
) -> Result<Frame> {
    let file = tempfile::Builder::new()
        .prefix("vshot-niri-window-")
        .suffix(".png")
        .tempfile()
        .map_err(|error| VshotError::NiriScreenshot(format!("no temporary file: {error}")))?;
    let path = file.path().to_path_buf();

    let mut command = command_for_window(window.id, &path, cursor);
    // The first attempt is run raw rather than through `run`: a niri that
    // predates `--show-pointer` refuses the whole request, and that refusal is
    // the one failure worth retrying.
    let first = runner.run(&command)?;
    let output = if cursor && !first.status.success() && rejects_show_pointer(&first) {
        // Losing the pointer beats losing the screenshot.
        eprintln!(
            "vshot: this niri has no `--show-pointer` for screenshot-window, so the pointer \
             cannot be drawn into the window"
        );
        command = command_for_window(window.id, &path, false);
        runner.run(&command)?
    } else {
        first
    };
    if !output.status.success() {
        return Err(failed_action(&output));
    }

    wait_for_png(&path, timeout)
}

fn command(args: &[&str]) -> WindowCommand {
    WindowCommand {
        program: OsString::from("niri"),
        args: args.iter().map(OsString::from).collect(),
    }
}

/// `niri msg action screenshot-window` with the window's own id.  The path has
/// to be absolute — niri rejects anything else — and `--write-to-disk` is
/// stated rather than left to its default, because the media type in the
/// clipboard it writes anyway is nothing this side controls.
fn command_for_window(id: u64, path: &Path, cursor: bool) -> WindowCommand {
    let mut command = command(&[
        "msg",
        "action",
        "screenshot-window",
        "--id",
        &id.to_string(),
        "--write-to-disk",
        "true",
        "--path",
        &path.to_string_lossy(),
    ]);
    if cursor {
        command.args.push(OsString::from("--show-pointer"));
        command.args.push(OsString::from("true"));
    }
    command
}

/// Does this failure mean "this niri does not know `--show-pointer`"?  The
/// argument arrived after 25.11, and a build without it fails the request with
/// clap's "unexpected argument" complaint instead of ignoring it.
fn rejects_show_pointer(output: &Output) -> bool {
    let stderr = String::from_utf8_lossy(&output.stderr);
    stderr.contains("unexpected argument") && stderr.contains("show-pointer")
}

fn failed_action(output: &Output) -> VshotError {
    let stderr = String::from_utf8_lossy(&output.stderr);
    let detail = stderr.trim();
    VshotError::NiriScreenshot(if detail.is_empty() {
        format!(
            "`niri msg action screenshot-window` exited with {}",
            output.status
        )
    } else {
        detail.to_owned()
    })
}

/// Reads the screenshot until it is a whole PNG.  An empty or partly written
/// file fails to decode, which is exactly the state to wait through.
fn wait_for_png(path: &Path, timeout: Duration) -> Result<Frame> {
    let deadline = Instant::now() + timeout;
    loop {
        match std::fs::read(path) {
            Ok(bytes) if !bytes.is_empty() => {
                if let Ok(frame) = Frame::from_png(&bytes) {
                    return Ok(frame);
                }
            }
            Ok(_) => {}
            Err(error) if error.kind() == ErrorKind::NotFound => {}
            Err(error) => {
                return Err(VshotError::NiriScreenshot(format!(
                    "cannot read the screenshot niri wrote to {}: {error}",
                    path.display()
                )))
            }
        }
        if Instant::now() >= deadline {
            return Err(VshotError::NiriScreenshot(format!(
                "niri did not write a window screenshot to {} within {}s",
                path.display(),
                timeout.as_secs()
            )));
        }
        std::thread::sleep(POLL_INTERVAL);
    }
}

fn run<R: WindowCommandRunner>(runner: &R, command: &WindowCommand) -> Result<Output> {
    let output = runner.run(command)?;
    if !output.status.success() {
        return Err(VshotError::NiriScreenshot(command_failure(
            command, &output,
        )));
    }
    Ok(output)
}

fn command_failure(command: &WindowCommand, output: &Output) -> String {
    let stderr = String::from_utf8_lossy(&output.stderr);
    let detail = stderr.trim();
    if detail.is_empty() {
        format!(
            "`{}` exited with {}",
            command.program.to_string_lossy(),
            output.status
        )
    } else {
        detail.to_owned()
    }
}

/// One window out of niri's JSON, or `None` for the `null` that means "no
/// window" — nothing focused, or a pick the user cancelled.  Unknown fields are
/// ignored: niri adds them between releases and leaves the rest alone.
fn parse_window(bytes: &[u8], what: &str) -> Result<Option<NiriWindow>> {
    let value: Value = serde_json::from_slice(bytes).map_err(|error| {
        VshotError::NiriScreenshot(format!("niri's {what} reply is not JSON: {error}"))
    })?;
    if value.is_null() {
        return Ok(None);
    }
    let id = value.get("id").and_then(Value::as_u64).ok_or_else(|| {
        VshotError::NiriScreenshot(format!("niri's {what} reply names no window id"))
    })?;
    let app_id = value.get("app_id").and_then(Value::as_str).unwrap_or("");
    let title = value.get("title").and_then(Value::as_str).unwrap_or("");
    Ok(Some(NiriWindow {
        id,
        label: join_label(app_id, title),
        workspace_id: value.get("workspace_id").and_then(Value::as_u64),
    }))
}

/// Workspace id → the output it is on, from `niri msg --json workspaces`.
fn parse_workspace_outputs(bytes: &[u8]) -> Result<HashMap<u64, String>> {
    let values: Vec<Value> = serde_json::from_slice(bytes).map_err(|error| {
        VshotError::NiriScreenshot(format!("niri's workspace list is not JSON: {error}"))
    })?;
    let mut workspaces = HashMap::new();
    for value in values {
        let (Some(id), Some(name)) = (
            value.get("id").and_then(Value::as_u64),
            value.get("output").and_then(Value::as_str),
        ) else {
            continue;
        };
        workspaces.insert(id, name.to_owned());
    }
    Ok(workspaces)
}

/// Output name → the scale niri lays it out at, from
/// `niri msg --json outputs`.  niri states a double (`2`, or `1.25` on a
/// fractional-scale screen) while everything in vshot counts whole device
/// pixels per logical pixel, so it is rounded here the same way KWin's is
/// (`capture::kwin::result_scale`).
fn parse_output_scales(bytes: &[u8]) -> Result<HashMap<String, u32>> {
    let value: Value = serde_json::from_slice(bytes).map_err(|error| {
        VshotError::NiriScreenshot(format!("niri's output list is not JSON: {error}"))
    })?;
    let Some(outputs) = value.as_object() else {
        return Err(VshotError::NiriScreenshot(
            "niri's output list is not an object keyed by output name".into(),
        ));
    };
    let mut scales = HashMap::new();
    for (name, output) in outputs {
        let scale = output
            .get("logical")
            .and_then(|logical| logical.get("scale"))
            .and_then(Value::as_f64);
        // A disabled output carries no `logical` block at all.
        if let Some(scale) = scale.filter(|scale| *scale >= 1.0) {
            scales.insert(name.clone(), scale.round() as u32);
        }
    }
    Ok(scales)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::geometry::Size;
    use std::cell::RefCell;
    use std::ffi::OsStr;
    use std::os::unix::process::ExitStatusExt;
    use std::path::PathBuf;
    use std::process::ExitStatus;

    /// A focused tiled window, shaped exactly like niri's `Window` — notice
    /// `tile_pos_in_workspace_view: null`, which is what makes the rect route
    /// impossible.
    const FOCUSED_TILED: &str = r#"{
        "id": 12,
        "title": "t w",
        "app_id": "Alacritty",
        "pid": 4242,
        "workspace_id": 6,
        "is_focused": true,
        "is_floating": false,
        "is_urgent": false,
        "layout": {
            "pos_in_scrolling_layout": [1, 1],
            "tile_size": [800.0, 600.0],
            "window_size": [796, 596],
            "tile_pos_in_workspace_view": null,
            "window_offset_in_tile": [2.0, 2.0]
        },
        "focus_timestamp": {"secs": 1234, "nanos": 0}
    }"#;

    const FLOATING: &str = r#"{
        "id": 13,
        "title": "floating",
        "app_id": "kitty",
        "workspace_id": 6,
        "is_focused": false,
        "is_floating": true,
        "is_urgent": false,
        "layout": {
            "pos_in_scrolling_layout": null,
            "tile_size": [400.0, 300.0],
            "window_size": [400, 300],
            "tile_pos_in_workspace_view": [100.5, 200.0],
            "window_offset_in_tile": [0.0, 0.0]
        },
        "focus_timestamp": null
    }"#;

    const WORKSPACES: &str = r#"[
        {"id": 6, "idx": 1, "name": null, "output": "DP-2", "is_urgent": false,
         "is_active": true, "active_window_id": 12},
        {"id": 7, "idx": 2, "name": null, "output": "HDMI-A-1", "is_urgent": false,
         "is_active": false, "active_window_id": null}
    ]"#;

    const OUTPUTS: &str = r#"{
        "DP-2": {
            "name": "DP-2", "make": "Dell", "model": "U2720Q", "serial": "abc",
            "physical_size": [600, 340], "modes": [], "current_mode": 0,
            "is_custom_mode": false, "vrr_supported": false, "vrr_enabled": false,
            "logical": {"x": 0, "y": 0, "width": 2560, "height": 1440, "scale": 2.0,
                        "transform": "Normal"},
            "max_bpc": 10
        },
        "HDMI-A-1": {
            "name": "HDMI-A-1", "make": "Dell", "model": "U2415", "serial": "def",
            "physical_size": [520, 320], "modes": [], "current_mode": 0,
            "is_custom_mode": false, "vrr_supported": false, "vrr_enabled": false,
            "logical": {"x": 2560, "y": 0, "width": 1920, "height": 1200, "scale": 1.0,
                        "transform": "Normal"},
            "max_bpc": 8
        }
    }"#;

    #[test]
    fn parses_a_focused_tiled_window() {
        let window = parse_window(FOCUSED_TILED.as_bytes(), "focused-window")
            .unwrap()
            .expect("a window is focused");
        assert_eq!(window.id, 12);
        assert_eq!(window.label, "Alacritty — t w");
        assert_eq!(window.workspace_id, Some(6));
    }

    #[test]
    fn a_rounded_floating_window_is_read_the_same_way() {
        let window = parse_window(FLOATING.as_bytes(), "pick-window")
            .unwrap()
            .expect("a window was picked");
        assert_eq!(window.id, 13);
        assert_eq!(window.label, "kitty — floating");
    }

    #[test]
    fn no_window_is_an_answer_and_not_a_failure() {
        // `niri msg --json focused-window` prints `null` when a layer-shell
        // surface holds the focus, and `pick-window` prints it on Esc.
        assert_eq!(parse_window(b"null", "focused-window").unwrap(), None);
        assert_eq!(parse_window(b"null\n", "pick-window").unwrap(), None);
    }

    #[test]
    fn unknown_fields_are_ignored() {
        // niri adds fields between releases and keeps the old ones, so a reply
        // from a newer compositor must still parse.
        let json = r#"{"id": 21, "app_id": "foot", "title": "shell", "workspace_id": 6,
                       "is_window_cast_target": false, "something_new": {"nested": [1, 2]}}"#;
        let window = parse_window(json.as_bytes(), "focused-window")
            .unwrap()
            .expect("a window is focused");
        assert_eq!(window.id, 21);
        assert_eq!(window.label, "foot — shell");
    }

    #[test]
    fn a_window_without_an_id_is_a_failure() {
        let error = parse_window(b"{\"app_id\": \"foot\"}", "focused-window").unwrap_err();
        assert!(error.to_string().contains("no window id"), "{error}");
    }

    #[test]
    fn the_windows_output_comes_from_its_workspace() {
        let runner = ScriptedRunner::new()
            .replying("msg --json workspaces", WORKSPACES.as_bytes())
            .replying("msg --json outputs", OUTPUTS.as_bytes());
        let window = parse_window(FOCUSED_TILED.as_bytes(), "focused-window")
            .unwrap()
            .unwrap();
        assert_eq!(
            window_scale(&runner, &window).unwrap(),
            Some(("DP-2".to_owned(), 2))
        );

        let floating = parse_window(FLOATING.as_bytes(), "focused-window")
            .unwrap()
            .unwrap();
        assert_eq!(
            window_scale(&runner, &floating).unwrap(),
            Some(("DP-2".to_owned(), 2)),
            "the floating window is on the same workspace"
        );
    }

    #[test]
    fn a_fractional_scale_is_rounded_like_kwins() {
        let outputs = r#"{"eDP-1": {"logical": {"scale": 1.25}}}"#;
        assert_eq!(
            parse_output_scales(outputs.as_bytes())
                .unwrap()
                .get("eDP-1"),
            Some(&1)
        );
    }

    #[test]
    fn a_disabled_output_has_no_scale() {
        // niri leaves `logical` out for an output it is not driving.
        let outputs = r#"{"DP-3": {"name": "DP-3", "current_mode": null}}"#;
        assert!(parse_output_scales(outputs.as_bytes()).unwrap().is_empty());
    }

    #[test]
    fn a_window_on_no_workspace_cannot_be_placed() {
        let runner = ScriptedRunner::new();
        let window = NiriWindow {
            id: 5,
            label: String::new(),
            workspace_id: None,
        };
        assert_eq!(window_scale(&runner, &window).unwrap(), None);
        assert!(runner.asked().is_empty(), "nothing worth asking about");
    }

    #[test]
    fn the_screenshot_asks_for_the_id_and_an_absolute_path() {
        let runner = ScriptedRunner::new().writes_png();
        let window = NiriWindow {
            id: 12,
            label: String::new(),
            workspace_id: Some(6),
        };
        capture_window(&runner, &window, false).unwrap();

        let asked = runner.asked();
        assert_eq!(asked.len(), 1, "{asked:?}");
        assert_eq!(asked[0][0], "niri");
        let (path, head) = asked[0].split_last().expect("a path is always passed");
        assert_eq!(
            head[1..].to_vec(),
            vec![
                "msg",
                "action",
                "screenshot-window",
                "--id",
                "12",
                "--write-to-disk",
                "true",
                "--path"
            ]
        );
        assert!(
            path.starts_with('/'),
            "niri rejects a relative path: {path}"
        );
        assert!(path.ends_with(".png"), "{path}");
    }

    #[test]
    fn the_cursor_is_asked_for_only_when_it_is_wanted() {
        let runner = ScriptedRunner::new().writes_png();
        let window = NiriWindow {
            id: 12,
            label: String::new(),
            workspace_id: Some(6),
        };
        capture_window(&runner, &window, true).unwrap();
        assert!(runner.asked()[0].contains(&"--show-pointer".to_owned()));
    }

    #[test]
    fn a_niri_without_show_pointer_still_gives_its_picture() {
        // 25.11 has no `show-pointer` on this action, and clap refuses the
        // whole request rather than ignoring the argument.
        let runner = ScriptedRunner::new()
            .failing(
                "--show-pointer",
                "error: unexpected argument '--show-pointer' found",
            )
            .writes_png();
        let window = NiriWindow {
            id: 12,
            label: String::new(),
            workspace_id: Some(6),
        };
        let frame = capture_window(&runner, &window, true).unwrap();
        assert_eq!(frame.size(), Size::new(40, 30));

        let asked = runner.asked();
        assert_eq!(asked.len(), 2, "the pointer request is retried without it");
        assert!(!asked[1].contains(&"--show-pointer".to_owned()));
        assert!(asked[1].contains(&"--id".to_owned()));
    }

    #[test]
    fn a_half_written_screenshot_is_waited_through() {
        // niri encodes on a thread and writes the file in one go, so a reader
        // can catch it empty or cut in half; only a whole PNG may be returned.
        let runner = ScriptedRunner::new().writes_partial_png_then_the_rest();
        let window = NiriWindow {
            id: 12,
            label: String::new(),
            workspace_id: Some(6),
        };
        let frame = capture_window(&runner, &window, false).unwrap();
        assert_eq!(frame.size(), Size::new(40, 30));
    }

    #[test]
    fn a_niri_that_never_writes_says_so() {
        let runner = ScriptedRunner::new(); // succeeds, writes nothing
        let window = NiriWindow {
            id: 12,
            label: String::new(),
            workspace_id: Some(6),
        };
        let error =
            capture_window_within(&runner, &window, false, Duration::from_millis(50)).unwrap_err();
        assert!(error.to_string().contains("did not write"), "{error}");
    }

    #[test]
    fn a_failed_action_reports_niris_own_words() {
        let runner = ScriptedRunner::new().failing("", "error: window 99 does not exist");
        let window = NiriWindow {
            id: 99,
            label: String::new(),
            workspace_id: Some(6),
        };
        let error = capture_window(&runner, &window, false).unwrap_err();
        assert!(error.to_string().contains("does not exist"), "{error}");
    }

    /// A runner that answers with fixed JSON, records every command and can
    /// write a PNG into the `--path` it is given.
    struct ScriptedRunner {
        repl: RefCell<Vec<(&'static str, Vec<u8>)>>,
        fail_with: RefCell<Vec<(&'static str, String)>>,
        asked: RefCell<Vec<Vec<String>>>,
        png: RefCell<Option<Vec<u8>>>,
        write_half_first: bool,
    }

    impl ScriptedRunner {
        fn new() -> Self {
            Self {
                repl: RefCell::new(Vec::new()),
                fail_with: RefCell::new(Vec::new()),
                asked: RefCell::new(Vec::new()),
                png: None.into(),
                write_half_first: false,
            }
        }

        /// Answers the command matching `needle`, or any command when the
        /// needle is empty.
        fn replying(self, needle: &'static str, stdout: &[u8]) -> Self {
            self.repl.borrow_mut().push((needle, stdout.to_vec()));
            self
        }

        /// Fails the command matching `needle` with this stderr.
        fn failing(self, needle: &'static str, stderr: &'static str) -> Self {
            self.fail_with
                .borrow_mut()
                .push((needle, stderr.to_owned()));
            self
        }

        /// Succeeds and writes a whole PNG to the requested path.
        fn writes_png(self) -> Self {
            *self.png.borrow_mut() = Some(
                Frame::solid(Size::new(40, 30), [10, 20, 30, 255])
                    .unwrap()
                    .to_png()
                    .unwrap(),
            );
            self
        }

        /// Writes the first half, then the rest from another thread: the
        /// reader has to survive catching the file in between.
        fn writes_partial_png_then_the_rest(self) -> Self {
            let runner = self.writes_png();
            Self {
                write_half_first: true,
                ..runner
            }
        }

        fn asked(&self) -> Vec<Vec<String>> {
            self.asked.borrow().clone()
        }

        fn writes_to(&self, command: &WindowCommand) -> Option<PathBuf> {
            let needle = OsString::from("--path");
            let index = command.args.iter().position(|arg| *arg == needle)?;
            command.args.get(index + 1).map(PathBuf::from)
        }
    }

    impl WindowCommandRunner for ScriptedRunner {
        fn run(&self, command: &WindowCommand) -> Result<Output> {
            self.asked.borrow_mut().push(
                std::iter::once(command.program.to_string_lossy().into_owned())
                    .chain(
                        command
                            .args
                            .iter()
                            .map(|arg| arg.to_string_lossy().into_owned()),
                    )
                    .collect(),
            );
            for (needle, stderr) in self.fail_with.borrow().iter() {
                let line = command
                    .args
                    .join(OsStr::new(" "))
                    .to_string_lossy()
                    .into_owned();
                let matches = needle.is_empty() || line.contains(*needle);
                if matches {
                    return Ok(Output {
                        status: ExitStatus::from_raw(1 << 8),
                        stdout: Vec::new(),
                        stderr: stderr.clone().into_bytes(),
                    });
                }
            }
            let stdout = self
                .repl
                .borrow()
                .iter()
                .find(|(needle, _)| {
                    let line = command
                        .args
                        .join(OsStr::new(" "))
                        .to_string_lossy()
                        .into_owned();
                    needle.is_empty() || line.contains(*needle)
                })
                .map(|(_, stdout)| stdout.clone())
                .unwrap_or_default();

            if let (Some(png), Some(path)) = (self.png.borrow().clone(), self.writes_to(command)) {
                if self.write_half_first {
                    let half = png.len() / 2;
                    std::fs::write(&path, &png[..half]).unwrap();
                    let path = path.clone();
                    std::thread::spawn(move || {
                        std::thread::sleep(Duration::from_millis(60));
                        std::fs::write(&path, png).unwrap();
                    });
                } else {
                    std::fs::write(&path, &png).unwrap();
                }
            }

            Ok(Output {
                status: ExitStatus::from_raw(0),
                stdout,
                stderr: Vec::new(),
            })
        }
    }
}

use std::ffi::OsString;
use std::process::{Command, Output};

use serde_json::Value;

use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ActiveWindow {
    pub geometry: Rect,
    pub source: WindowSource,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum WindowSource {
    Hyprland,
    Sway,
    KWin,
    /// Detected from the captured frame (accent outline / segmentation).
    Pixel,
}

/// One window the compositor shows, with the label the user can recognise it
/// by. Interactive picking offers these; the geometry is in global logical
/// pixels, the same space the captured scene uses.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct WindowCandidate {
    pub geometry: Rect,
    /// `class — title` for Wayland-native windows, the closest available
    /// equivalent elsewhere, and empty when the compositor reports neither.
    pub label: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct WindowCommand {
    pub program: OsString,
    pub args: Vec<OsString>,
}

impl WindowCommand {
    fn hyprland() -> Self {
        Self {
            program: OsString::from("hyprctl"),
            args: vec![OsString::from("activewindow"), OsString::from("-j")],
        }
    }

    /// Every client Hyprland knows, so picking can offer the windows the user
    /// sees rather than only the focused one.
    fn hyprland_clients() -> Self {
        Self {
            program: OsString::from("hyprctl"),
            args: vec![OsString::from("clients"), OsString::from("-j")],
        }
    }

    /// The monitor list that goes with [`WindowCommand::hyprland_clients`] :
    /// which workspace each monitor shows, and where it is.
    fn hyprland_monitors() -> Self {
        Self {
            program: OsString::from("hyprctl"),
            args: vec![OsString::from("monitors"), OsString::from("-j")],
        }
    }

    fn sway() -> Self {
        Self {
            program: OsString::from("swaymsg"),
            args: vec![OsString::from("-t"), OsString::from("get_tree")],
        }
    }

    /// One-shot KWin scripting probe for KDE Plasma. KWin exposes no direct
    /// "active window geometry" DBus query, so the probe either uses
    /// `kdotool` when installed or loads a tiny script through
    /// `org.kde.kwin.Scripting` that logs the active window's frame
    /// geometry (`workspace.activeWindow` on Plasma 6, `activeClient` on
    /// Plasma 5) and reads the marked line back from the user journal.
    /// Everything is best-effort: on compositors without KWin the DBus name
    /// is absent and the probe fails in milliseconds.
    fn kwin() -> Self {
        kwin_probe("active")
    }

    /// The same probe in `list` mode: one line per window instead of the one
    /// focused window.
    fn kwin_list() -> Self {
        kwin_probe("list")
    }
}

fn kwin_probe(mode: &str) -> WindowCommand {
    // `bash -c SCRIPT vshot MODE` makes the script's `$1` the mode.
    WindowCommand {
        program: OsString::from("bash"),
        args: vec![
            OsString::from("-c"),
            OsString::from(KWIN_PROBE),
            OsString::from("vshot"),
            OsString::from(mode),
        ],
    }
}

/// See [`WindowCommand::kwin`]. `$1` selects `active` (the focused window) or
/// `list` (every window). Each result line is `x y width height` in global
/// logical pixels; any other outcome must exit non-zero or print nothing.
const KWIN_PROBE: &str = r##"
set -u
mode="${1:-active}"
marker="VSHOTMARK_$$"
run_limited() {
    if command -v timeout >/dev/null 2>&1; then
        timeout 3 "$@"
    else
        "$@"
    fi
}
tmp="$(mktemp "${TMPDIR:-/tmp}/vshot-kwin-XXXXXX.js")" || exit 3
trap 'rm -f "$tmp"' EXIT

# kdotool answers only the active-window question; listing goes through the
# scripting probe either way.
if [ "$mode" = active ] && command -v kdotool >/dev/null 2>&1; then
    # `getwindowgeometry` has no `--shell` form — kdotool 0.2.1 offers that only
    # for `getmouselocation`, and asking anyway fails the whole command — so its
    # labelled output is parsed instead:
    #
    #   Window {1d5f...}
    #     Position: 400.0702150216,167.68492081784424
    #     Geometry: 1066x709.9999999999989
    #
    # The fractions are kdotool's own arithmetic on whole logical pixels, so
    # they are rounded back.
    geo="$(run_limited kdotool getactivewindow getwindowgeometry 2>/dev/null)" || geo=""
    if [ -n "$geo" ] && command -v awk >/dev/null 2>&1; then
        geo="$(printf '%s\n' "$geo" | awk '
            /^[[:space:]]*Position:/ { split($2, at, ","); x = at[1]; y = at[2] }
            /^[[:space:]]*Geometry:/ { split($2, size, "x"); w = size[1]; h = size[2] }
            END {
                if (w > 0 && h > 0) {
                    printf "%d %d %d %d\n", int(x + 0.5), int(y + 0.5), int(w + 0.5), int(h + 0.5)
                }
            }
        ')"
        if [ -n "$geo" ]; then
            printf '%s\n' "$geo"
            exit 0
        fi
    fi
fi

if command -v gdbus >/dev/null 2>&1; then
    tool=gdbus
elif command -v dbus-send >/dev/null 2>&1; then
    tool=dbus-send
else
    exit 3
fi

if [ "$mode" = active ]; then
cat > "$tmp" <<EOF
const w = workspace.activeWindow || workspace.activeClient;
if (w) {
    const g = w.frameGeometry || w.geometry;
    if (g && g.width > 0 && g.height > 0) {
        console.info("$marker " + Math.round(g.x) + " " + Math.round(g.y)
            + " " + Math.round(g.width) + " " + Math.round(g.height));
    } else {
        console.info("$marker null");
    }
} else {
    console.info("$marker null");
}
EOF
else
cat > "$tmp" <<EOF
const windows = workspace.windowList ? workspace.windowList()
    : (workspace.clientList ? workspace.clientList() : []);
for (let index = 0; index < windows.length; ++index) {
    const w = windows[index];
    if (!w || w.deleted || w.hidden || w.minimized) {
        continue;
    }
    // Panels, docks and splash screens are not pickable windows.
    if (w.normalWindow === false) {
        continue;
    }
    const g = w.frameGeometry || w.geometry;
    if (!g || !(g.width > 0) || !(g.height > 0)) {
        continue;
    }
    console.info("$marker " + Math.round(g.x) + " " + Math.round(g.y)
        + " " + Math.round(g.width) + " " + Math.round(g.height));
}
EOF
fi

if [ "$tool" = gdbus ]; then
    run_limited gdbus call --session --dest org.kde.KWin --object-path /Scripting \
        --method org.kde.kwin.Scripting.loadScript "$tmp" "vshot$$" >/dev/null 2>&1 || exit 3
    run_limited gdbus call --session --dest org.kde.KWin --object-path /Scripting \
        --method org.kde.kwin.Scripting.start >/dev/null 2>&1 || exit 3
else
    run_limited dbus-send --session --print-reply=literal --dest=org.kde.KWin /Scripting \
        org.kde.kwin.Scripting.loadScript string:"$tmp" string:"vshot$$" >/dev/null 2>&1 || exit 3
    run_limited dbus-send --session --print-reply=literal --dest=org.kde.KWin /Scripting \
        org.kde.kwin.Scripting.start >/dev/null 2>&1 || exit 3
fi

# The active probe reports one window (or `null`); the list probe reports one
# line per window.  Both are collected the same way and the null lines dropped.
lines=""
if command -v journalctl >/dev/null 2>&1; then
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        sleep 0.06
        if [ "$mode" = active ]; then
            lines="$(journalctl --user -n 200 --output=cat 2>/dev/null \
                | grep -aF "$marker" | tail -n 1)"
        else
            lines="$(journalctl --user -n 200 --output=cat 2>/dev/null \
                | grep -aF "$marker" | tail -n 40)"
        fi
        [ -n "$lines" ] && break
    done
fi

# Best-effort cleanup of the loaded script; output was already captured.
if [ "$tool" = gdbus ]; then
    run_limited gdbus call --session --dest org.kde.KWin --object-path /Scripting \
        --method org.kde.kwin.Scripting.unloadScript "vshot$$" >/dev/null 2>&1
else
    run_limited dbus-send --session --print-reply=literal --dest=org.kde.KWin /Scripting \
        org.kde.kwin.Scripting.unloadScript string:"vshot$$" >/dev/null 2>&1
fi

[ -n "$lines" ] || exit 4
payload="$(printf '%s\n' "$lines" | while IFS= read -r entry; do
    window="${entry#* }"
    [ "$window" = "null" ] && continue
    printf '%s\n' "$window"
done)"
[ -n "$payload" ] || exit 4
printf '%s\n' "$payload"
"##;

pub trait WindowCommandRunner {
    fn run(&self, command: &WindowCommand) -> Result<Output>;
}

#[derive(Clone, Copy, Debug, Default)]
pub struct ProcessWindowRunner;

impl WindowCommandRunner for ProcessWindowRunner {
    fn run(&self, command: &WindowCommand) -> Result<Output> {
        Command::new(&command.program)
            .args(&command.args)
            .output()
            .map_err(|source| VshotError::CommandIo {
                program: command.program.to_string_lossy().into_owned(),
                source,
            })
    }
}

pub trait CompositorWindowProvider {
    fn active_window(&self) -> Result<ActiveWindow>;
    /// Every window the compositor shows, for interactive picking.  There is
    /// no pixel fallback here: the caller decides what a missing list means.
    fn windows(&self) -> Result<Vec<WindowCandidate>>;
}

#[derive(Clone, Copy, Debug, Default)]
pub struct ProcessWindowProvider;

impl CompositorWindowProvider for ProcessWindowProvider {
    fn active_window(&self) -> Result<ActiveWindow> {
        find_active_window(&ProcessWindowRunner)
    }

    fn windows(&self) -> Result<Vec<WindowCandidate>> {
        find_windows(&ProcessWindowRunner)
    }
}

impl ProcessWindowProvider {
    /// The window under `point` in the compositor's list *right now*, for
    /// resolving where a picking session's click landed.  Interactive picking
    /// runs on a live desktop, so the window list it started with may be stale
    /// by the time the click arrives — a window may have moved or the user may
    /// have switched workspace under it.
    ///
    /// The rule matches the picker's own: the smallest window containing the
    /// point wins, so pointing at overlapping windows takes the inner one.
    /// `None` means the compositor cannot answer (no window list, or nothing
    /// there), which is not an error: the caller keeps what the picker had.
    pub fn window_at(&self, point: Point) -> Option<Rect> {
        let windows = self.windows().ok()?;
        smallest_containing(windows.iter().map(|window| window.geometry), point)
    }
}

/// Smallest rect that contains `point`.
fn smallest_containing(rects: impl Iterator<Item = Rect>, point: Point) -> Option<Rect> {
    rects
        .filter(|rect| rect.contains(point))
        .min_by_key(|rect| u64::from(rect.size.width) * u64::from(rect.size.height))
}

/// Lists the windows the compositor shows, geometry in global logical pixels.
///
/// Interactive picking uses this to highlight what the pointer is over, so it
/// wants every window rather than the focused one — and it has to be honest
/// about not knowing: a compositor that reports no list is collected into the
/// error instead of being guessed at.
pub fn find_windows<R: WindowCommandRunner>(runner: &R) -> Result<Vec<WindowCandidate>> {
    let mut reasons: Vec<String> = Vec::new();
    match hyprland_windows(runner) {
        Ok(windows) if !windows.is_empty() => return Ok(windows),
        Ok(_) => reasons.push("Hyprland shows no window".into()),
        Err(error) => reasons.push(format!("Hyprland: {error}")),
    }
    match run_command(runner, &WindowCommand::sway()).and_then(|bytes| parse_sway_windows(&bytes)) {
        Ok(windows) if !windows.is_empty() => return Ok(windows),
        Ok(_) => reasons.push("Sway shows no window".into()),
        Err(error) => reasons.push(format!("Sway: {error}")),
    }
    match run_command(runner, &WindowCommand::kwin_list())
        .and_then(|bytes| parse_kwin_windows(&bytes))
    {
        Ok(windows) if !windows.is_empty() => return Ok(windows),
        Ok(_) => reasons.push("KWin shows no window".into()),
        Err(error) => reasons.push(format!("KWin: {error}")),
    }
    Err(VshotError::WindowPickUnavailable(format!(
        "no compositor reported a window list ({})",
        reasons.join("; ")
    )))
}

/// Hyprland's window list needs its monitor list as well: `hyprctl clients`
/// reports clients from every workspace, so the two queries are parsed
/// together.
fn hyprland_windows<R: WindowCommandRunner>(runner: &R) -> Result<Vec<WindowCandidate>> {
    let clients = run_command(runner, &WindowCommand::hyprland_clients())?;
    let monitors = run_command(runner, &WindowCommand::hyprland_monitors())?;
    parse_hyprland_windows(&clients, &monitors)
}

fn run_command<R: WindowCommandRunner>(runner: &R, command: &WindowCommand) -> Result<Vec<u8>> {
    let output = runner.run(command)?;
    if !output.status.success() {
        return Err(VshotError::WindowPickUnavailable(format!(
            "`{}` exited with {}",
            command.program.to_string_lossy(),
            output.status
        )));
    }
    Ok(output.stdout)
}

pub fn find_active_window<R: WindowCommandRunner>(runner: &R) -> Result<ActiveWindow> {
    let hyprland = runner.run(&WindowCommand::hyprland());
    if let Ok(output) = hyprland {
        if output.status.success() {
            if let Ok(window) = parse_hyprland_active_window(&output.stdout) {
                return Ok(window);
            }
        }
    }

    let sway = runner.run(&WindowCommand::sway());
    if let Ok(output) = sway {
        if output.status.success() {
            if let Ok(window) = parse_sway_active_window(&output.stdout) {
                return Ok(window);
            }
        }
    }

    let kwin = runner.run(&WindowCommand::kwin());
    if let Ok(output) = kwin {
        if output.status.success() {
            if let Ok(window) = parse_kwin_active_window(&output.stdout) {
                return Ok(window);
            }
        }
    }

    Err(VshotError::ActiveWindowUnavailable(
        "neither a valid Hyprland `hyprctl activewindow -j` result, a focused Sway tree node, \
         nor a KWin scripting probe was available"
            .into(),
    ))
}

pub fn parse_hyprland_active_window(bytes: &[u8]) -> Result<ActiveWindow> {
    let value: Value = serde_json::from_slice(bytes).map_err(|error| {
        VshotError::ActiveWindowUnavailable(format!("invalid Hyprland JSON: {error}"))
    })?;
    let at = pair_i32(&value, "at")?;
    let size = pair_u32(&value, "size")?;
    if size.0 == 0 || size.1 == 0 {
        return Err(VshotError::ActiveWindowUnavailable(
            "Hyprland active window has an empty size".into(),
        ));
    }
    Ok(ActiveWindow {
        geometry: Rect::new(at.0, at.1, size.0, size.1),
        source: WindowSource::Hyprland,
    })
}

pub fn parse_sway_active_window(bytes: &[u8]) -> Result<ActiveWindow> {
    let value: Value = serde_json::from_slice(bytes).map_err(|error| {
        VshotError::ActiveWindowUnavailable(format!("invalid Sway JSON: {error}"))
    })?;
    let node = focused_node(&value).ok_or_else(|| {
        VshotError::ActiveWindowUnavailable("Sway tree contains no focused node".into())
    })?;
    let rect = node.get("rect").ok_or_else(|| {
        VshotError::ActiveWindowUnavailable("focused Sway node has no rect".into())
    })?;
    let x = json_i32(rect, "x")?;
    let y = json_i32(rect, "y")?;
    let width = json_u32(rect, "width")?;
    let height = json_u32(rect, "height")?;
    if width == 0 || height == 0 {
        return Err(VshotError::ActiveWindowUnavailable(
            "focused Sway node has an empty rect".into(),
        ));
    }
    Ok(ActiveWindow {
        geometry: Rect::new(x, y, width, height),
        source: WindowSource::Sway,
    })
}

/// Parses the KWin scripting probe's `x y width height` output (global
/// logical pixels; x/y may be negative on multi-monitor layouts).
pub fn parse_kwin_active_window(bytes: &[u8]) -> Result<ActiveWindow> {
    let text = std::str::from_utf8(bytes).map_err(|error| {
        VshotError::ActiveWindowUnavailable(format!("invalid KWin probe output: {error}"))
    })?;
    let parts: Vec<&str> = text.split_whitespace().collect();
    if parts.len() != 4 {
        return Err(VshotError::ActiveWindowUnavailable(
            "KWin probe did not report x y width height".into(),
        ));
    }
    let parse_i32 = |value: &str| -> Result<i32> {
        value.parse::<i32>().map_err(|_| {
            VshotError::ActiveWindowUnavailable(format!(
                "KWin probe coordinate `{value}` is not an integer"
            ))
        })
    };
    let parse_u32 = |value: &str| -> Result<u32> {
        value.parse::<u32>().map_err(|_| {
            VshotError::ActiveWindowUnavailable(format!(
                "KWin probe dimension `{value}` is invalid"
            ))
        })
    };
    let x = parse_i32(parts[0])?;
    let y = parse_i32(parts[1])?;
    let width = parse_u32(parts[2])?;
    let height = parse_u32(parts[3])?;
    if width == 0 || height == 0 {
        return Err(VshotError::ActiveWindowUnavailable(
            "KWin probe reported an empty window size".into(),
        ));
    }
    Ok(ActiveWindow {
        geometry: Rect::new(x, y, width, height),
        source: WindowSource::KWin,
    })
}

/// `hyprctl clients -j` plus `hyprctl monitors -j`: the clients that are
/// really on screen, geometry in global logical pixels.
///
/// Hyprland lists clients from every workspace, and `visible` does not mean
/// "on a workspace that is being shown" — it is set for windows on hidden
/// workspaces too, and those keep a stale `at` from whenever they were last
/// laid out.  So the list is narrowed structurally instead:
///
/// * the client's workspace must be the one its monitor currently shows (the
///   active one, or an activated special workspace); pinned windows stay
///   visible across workspaces and are always kept;
/// * the client's rect must land inside that monitor — a stale rect usually
///   points at another monitor, which is how it gets caught.
///
/// Nothing here is a guess about which window the user means; it only removes
/// entries that cannot be the window under the pointer.
pub fn parse_hyprland_windows(clients: &[u8], monitors: &[u8]) -> Result<Vec<WindowCandidate>> {
    let clients: Value = serde_json::from_slice(clients).map_err(|error| {
        VshotError::WindowPickUnavailable(format!("invalid Hyprland client JSON: {error}"))
    })?;
    let monitors: Value = serde_json::from_slice(monitors).map_err(|error| {
        VshotError::WindowPickUnavailable(format!("invalid Hyprland monitor JSON: {error}"))
    })?;
    let clients = clients.as_array().ok_or_else(|| {
        VshotError::WindowPickUnavailable("Hyprland client list is not an array".into())
    })?;
    let monitors = monitors.as_array().ok_or_else(|| {
        VshotError::WindowPickUnavailable("Hyprland monitor list is not an array".into())
    })?;

    let mut windows = Vec::new();
    for client in clients {
        let Some(shown) = shown_workspaces(client, monitors) else {
            continue;
        };
        let (Ok(at), Ok(size)) = (pair_i32(client, "at"), pair_u32(client, "size")) else {
            continue;
        };
        if size.0 == 0 || size.1 == 0 {
            continue;
        }
        let geometry = Rect::new(at.0, at.1, size.0, size.1);
        let pinned = client.get("pinned").and_then(Value::as_bool) == Some(true);
        if !pinned && !shown.is_showing_client_workspace(client) {
            continue;
        }
        if !shown.contains(geometry) {
            continue;
        }
        windows.push(WindowCandidate {
            geometry,
            label: join_label(
                client.get("class").and_then(Value::as_str).unwrap_or(""),
                client.get("title").and_then(Value::as_str).unwrap_or(""),
            ),
        });
    }
    Ok(windows)
}

/// How far outside its monitor a reported rect may reach before it is treated
/// as stale.  Borders and gap rounding can push a real window a hair over the
/// edge; a rect left over from another workspace misses by far more.
const MONITOR_SLACK: i32 = 4;

/// The monitor a client sits on, reduced to what the filter needs: the
/// workspaces it currently shows and its logical bounds.
struct ShownMonitor {
    active: String,
    special: String,
    bounds: Rect,
}

impl ShownMonitor {
    fn is_showing_client_workspace(&self, client: &Value) -> bool {
        let name = workspace_identity(client.get("workspace"));
        !name.is_empty() && (name == self.active || name == self.special)
    }

    /// Does the window's rect land on this monitor?  Tiled windows are laid
    /// out inside it, so anything reaching outside comes from a stale `at`.
    fn contains(&self, geometry: Rect) -> bool {
        let (Ok(right), Ok(bottom)) = (geometry.right(), geometry.bottom()) else {
            return false;
        };
        let (Ok(bounds_right), Ok(bounds_bottom)) = (self.bounds.right(), self.bounds.bottom())
        else {
            return false;
        };
        geometry.left() >= self.bounds.left() - MONITOR_SLACK
            && geometry.top() >= self.bounds.top() - MONITOR_SLACK
            && right <= bounds_right + MONITOR_SLACK
            && bottom <= bounds_bottom + MONITOR_SLACK
    }
}

/// Resolves the monitor a client reports, or `None` when the monitor is not
/// in the list — an unmatched client cannot be validated and is dropped.
fn shown_workspaces(client: &Value, monitors: &[Value]) -> Option<ShownMonitor> {
    let id = client.get("monitor")?.as_u64()?;
    let monitor = monitors
        .iter()
        .find(|monitor| monitor.get("id").and_then(Value::as_u64) == Some(id))?;
    // `width`/`height` are native pixels: the bounds a rect is compared
    // against are logical, so they follow the scale.
    let scale = monitor.get("scale").and_then(Value::as_f64).unwrap_or(1.0);
    let scale = if scale > 0.0 { scale } else { 1.0 };
    let width = monitor.get("width").and_then(Value::as_u64)?;
    let height = monitor.get("height").and_then(Value::as_u64)?;
    let bounds = Rect::new(
        json_i32(monitor, "x").ok()?,
        json_i32(monitor, "y").ok()?,
        (width as f64 / scale).round() as u32,
        (height as f64 / scale).round() as u32,
    );
    Some(ShownMonitor {
        active: workspace_identity(monitor.get("activeWorkspace")),
        special: workspace_identity(monitor.get("specialWorkspace")),
        bounds,
    })
}

/// Workspace identity across the shapes Hyprland reports: newer builds name
/// the field `address`, older ones `id`, and `name` carries the display name
/// for both.
fn workspace_identity(workspace: Option<&Value>) -> String {
    let Some(workspace) = workspace else {
        return String::new();
    };
    for key in ["address", "name", "id"] {
        let value = workspace.get(key);
        if let Some(text) = value.and_then(Value::as_str) {
            if !text.is_empty() {
                return text.to_owned();
            }
        }
        if let Some(number) = value.and_then(Value::as_i64) {
            return number.to_string();
        }
    }
    String::new()
}

/// `swaymsg -t get_tree`: the leaves of the tree are the windows.  A leaf on a
/// hidden workspace is invisible and is therefore left out.
pub fn parse_sway_windows(bytes: &[u8]) -> Result<Vec<WindowCandidate>> {
    let value: Value = serde_json::from_slice(bytes).map_err(|error| {
        VshotError::WindowPickUnavailable(format!("invalid Sway tree JSON: {error}"))
    })?;
    let mut windows = Vec::new();
    collect_sway_windows(&value, true, &mut windows);
    Ok(windows)
}

fn collect_sway_windows(node: &Value, visible: bool, windows: &mut Vec<WindowCandidate>) {
    // Only workspaces carry a meaningful `visible`; a hidden one hides its
    // whole subtree.
    let visible = visible
        && !(node.get("type").and_then(Value::as_str) == Some("workspace")
            && node.get("visible").and_then(Value::as_bool) == Some(false));
    let mut children: Vec<&Value> = Vec::new();
    for key in ["nodes", "floating_nodes"] {
        if let Some(nodes) = node.get(key).and_then(Value::as_array) {
            children.extend(nodes.iter());
        }
    }
    if children.is_empty() {
        let Some(rect) = node.get("rect") else {
            return;
        };
        if !visible {
            return;
        }
        let (Ok(x), Ok(y), Ok(width), Ok(height)) = (
            json_i32(rect, "x"),
            json_i32(rect, "y"),
            json_u32(rect, "width"),
            json_u32(rect, "height"),
        ) else {
            return;
        };
        if width == 0 || height == 0 {
            return;
        }
        let class = node
            .get("app_id")
            .and_then(Value::as_str)
            .filter(|text| !text.is_empty())
            .or_else(|| {
                node.get("window_properties")
                    .and_then(|properties| properties.get("class"))
                    .and_then(Value::as_str)
            })
            .unwrap_or("");
        windows.push(WindowCandidate {
            geometry: Rect::new(x, y, width, height),
            label: join_label(
                class,
                node.get("name").and_then(Value::as_str).unwrap_or(""),
            ),
        });
        return;
    }
    for child in children {
        collect_sway_windows(child, visible, windows);
    }
}

/// The KWin probe in `list` mode: one `x y width height` line per window.  The
/// probe reports no titles, so every candidate is unlabelled.
pub fn parse_kwin_windows(bytes: &[u8]) -> Result<Vec<WindowCandidate>> {
    let text = std::str::from_utf8(bytes).map_err(|error| {
        VshotError::WindowPickUnavailable(format!("invalid KWin list output: {error}"))
    })?;
    let mut windows = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let Ok(window) = parse_kwin_active_window(line.as_bytes()) else {
            continue;
        };
        windows.push(WindowCandidate {
            geometry: window.geometry,
            label: String::new(),
        });
    }
    Ok(windows)
}

/// `class — title`, collapsing the cases where either side is missing or both
/// say the same thing.
fn join_label(class: &str, title: &str) -> String {
    let class = class.trim();
    let title = title.trim();
    match (
        class.is_empty(),
        title.is_empty(),
        class.eq_ignore_ascii_case(title),
    ) {
        (false, false, true) => class.to_owned(),
        (false, false, false) => format!("{class} — {title}"),
        (false, true, _) => class.to_owned(),
        (true, false, _) => title.to_owned(),
        (true, true, _) => String::new(),
    }
}

fn focused_node(value: &Value) -> Option<&Value> {
    if value.get("focused").and_then(Value::as_bool) == Some(true) {
        return Some(value);
    }
    value
        .get("nodes")
        .and_then(Value::as_array)
        .and_then(|nodes| nodes.iter().find_map(focused_node))
        .or_else(|| {
            value
                .get("floating_nodes")
                .and_then(Value::as_array)
                .and_then(|nodes| nodes.iter().find_map(focused_node))
        })
}

fn pair_i32(value: &Value, key: &str) -> Result<(i32, i32)> {
    let pair = value.get(key).and_then(Value::as_array).ok_or_else(|| {
        VshotError::ActiveWindowUnavailable(format!(
            "Hyprland field `{key}` is not a two-element array"
        ))
    })?;
    if pair.len() != 2 {
        return Err(VshotError::ActiveWindowUnavailable(format!(
            "Hyprland field `{key}` is not a two-element array"
        )));
    }
    let x = pair[0]
        .as_i64()
        .and_then(|number| i32::try_from(number).ok())
        .ok_or_else(|| {
            VshotError::ActiveWindowUnavailable(format!(
                "Hyprland field `{key}` contains a non-integer"
            ))
        })?;
    let y = pair[1]
        .as_i64()
        .and_then(|number| i32::try_from(number).ok())
        .ok_or_else(|| {
            VshotError::ActiveWindowUnavailable(format!(
                "Hyprland field `{key}` contains a non-integer"
            ))
        })?;
    Ok((x, y))
}

fn pair_u32(value: &Value, key: &str) -> Result<(u32, u32)> {
    let pair = value.get(key).and_then(Value::as_array).ok_or_else(|| {
        VshotError::ActiveWindowUnavailable(format!(
            "Hyprland field `{key}` is not a two-element array"
        ))
    })?;
    if pair.len() != 2 {
        return Err(VshotError::ActiveWindowUnavailable(format!(
            "Hyprland field `{key}` is not a two-element array"
        )));
    }
    let width = pair[0]
        .as_u64()
        .and_then(|number| u32::try_from(number).ok())
        .ok_or_else(|| {
            VshotError::ActiveWindowUnavailable(format!(
                "Hyprland field `{key}` contains an invalid width"
            ))
        })?;
    let height = pair[1]
        .as_u64()
        .and_then(|number| u32::try_from(number).ok())
        .ok_or_else(|| {
            VshotError::ActiveWindowUnavailable(format!(
                "Hyprland field `{key}` contains an invalid height"
            ))
        })?;
    Ok((width, height))
}

fn json_i32(value: &Value, key: &str) -> Result<i32> {
    value
        .get(key)
        .and_then(Value::as_i64)
        .and_then(|number| i32::try_from(number).ok())
        .ok_or_else(|| {
            VshotError::ActiveWindowUnavailable(format!(
                "Sway rect field `{key}` is not an integer"
            ))
        })
}

fn json_u32(value: &Value, key: &str) -> Result<u32> {
    value
        .get(key)
        .and_then(Value::as_u64)
        .and_then(|number| u32::try_from(number).ok())
        .ok_or_else(|| {
            VshotError::ActiveWindowUnavailable(format!(
                "Sway rect field `{key}` is not a non-negative integer"
            ))
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Resolving a click takes the smallest window under it, exactly like the
    /// picker's own hit test: pointing at overlapping windows means the inner
    /// one, and a point on bare desktop resolves to nothing.
    #[test]
    fn the_smallest_window_under_a_point_wins() {
        let windows = [
            Rect::new(0, 0, 1920, 1080),
            Rect::new(100, 100, 800, 600),
            Rect::new(800, 500, 400, 300),
        ];
        let resolved = |point| smallest_containing(windows.into_iter(), point);
        assert_eq!(
            resolved(Point::new(900, 700)),
            Some(Rect::new(800, 500, 400, 300))
        );
        assert_eq!(
            resolved(Point::new(400, 400)),
            Some(Rect::new(100, 100, 800, 600))
        );
        assert_eq!(
            resolved(Point::new(1900, 1000)),
            Some(Rect::new(0, 0, 1920, 1080))
        );
        assert_eq!(resolved(Point::new(2000, 100)), None);
        // Right and bottom edges belong to the next window, exactly as in the
        // picker's own hit test: (900, 400) is on the inner rect's right edge,
        // so only the full-screen window is left.
        assert_eq!(
            resolved(Point::new(900, 400)),
            Some(Rect::new(0, 0, 1920, 1080))
        );
        assert_eq!(resolved(Point::new(1900, 1100)), None);
    }

    #[test]
    fn parses_hyprland_active_window_geometry() {
        let json = br#"{"at":[-20,30],"size":[800,600],"class":"kitty"}"#;
        let window = parse_hyprland_active_window(json).unwrap();
        assert_eq!(window.geometry, Rect::new(-20, 30, 800, 600));
        assert_eq!(window.source, WindowSource::Hyprland);
    }

    #[test]
    fn finds_focused_sway_leaf_recursively() {
        let json = br#"{"nodes":[{"nodes":[],"focused":false},{"nodes":[{"focused":true,"rect":{"x":10,"y":20,"width":400,"height":300}}]}]}"#;
        let window = parse_sway_active_window(json).unwrap();
        assert_eq!(window.geometry, Rect::new(10, 20, 400, 300));
    }

    #[test]
    fn parses_kwin_probe_geometry() {
        let window = parse_kwin_active_window(b"1920 0 1600 900\n").unwrap();
        assert_eq!(window.geometry, Rect::new(1920, 0, 1600, 900));
        assert_eq!(window.source, WindowSource::KWin);
        let window = parse_kwin_active_window(b"-20 30 800 600").unwrap();
        assert_eq!(window.geometry, Rect::new(-20, 30, 800, 600));
    }

    #[test]
    fn lists_hyprland_clients_that_are_on_a_shown_workspace() {
        // DP-2 (id 0) is 3840x2160 at scale 2, so its logical area is
        // 1920..3840 and it shows workspace 1; DP-3 (id 1) shows workspace 3.
        let monitors = br#"[
            {"id":1,"name":"DP-3","x":0,"y":0,"width":1920,"height":1080,"scale":1.0,
             "activeWorkspace":{"address":"3","name":"3"},"specialWorkspace":{"name":""}},
            {"id":0,"name":"DP-2","x":1920,"y":0,"width":3840,"height":2160,"scale":2.0,
             "activeWorkspace":{"address":"1","name":"1"},"specialWorkspace":{"name":""}}
        ]"#;
        let clients = br#"[
            {"at":[1936,55],"size":[1893,1014],"monitor":0,"pinned":false,
             "workspace":{"address":"1","name":"1"},
             "class":"vivaldi-stable","title":"vivaldi-stable"},
            {"at":[11,55],"size":[1893,1014],"monitor":1,"pinned":false,
             "workspace":{"address":"3","name":"3"},
             "class":"vivaldi-stable","title":"vivaldi-stable"},
            {"at":[11,55],"size":[941,1014],"monitor":1,"pinned":false,
             "workspace":{"address":"2","name":"2"},
             "class":"discord","title":"Discord"},
            {"at":[979,55],"size":[941,1014],"monitor":0,"pinned":false,
             "workspace":{"address":"1","name":"1"},
             "class":"sparkle","title":"Sparkle"},
            {"at":[2407,55],"size":[946,1014],"monitor":0,"pinned":false,
             "workspace":{"address":"5","name":"5"},
             "class":"LACT","title":"LACT"},
            {"at":[0,0],"size":[800,600],"monitor":9,"pinned":false,
             "workspace":{"address":"1","name":"1"},
             "class":"orphan","title":"Orphan"},
            {"at":[100,100],"size":[0,0],"monitor":1,"pinned":false,
             "workspace":{"address":"3","name":"3"},
             "class":"empty","title":"Empty"}
        ]"#;
        let windows = parse_hyprland_windows(clients, monitors).unwrap();
        // The two windows the monitors are actually showing.  Discord is on
        // another workspace, sparkle and LACT are elsewhere too (and sparkle's
        // stale rect points off its own monitor), the orphan has no monitor
        // and the last client's size is empty.
        assert_eq!(windows.len(), 2);
        assert_eq!(windows[0].geometry, Rect::new(1936, 55, 1893, 1014));
        assert_eq!(windows[0].label, "vivaldi-stable");
        assert_eq!(windows[1].geometry, Rect::new(11, 55, 1893, 1014));
    }

    #[test]
    fn keeps_pinned_and_special_workspace_windows() {
        let monitors = br#"[{"id":0,"x":0,"y":0,"width":1920,"height":1080,"scale":1.0,
            "activeWorkspace":{"name":"1"},"specialWorkspace":{"name":"special:scratch"}}]"#;
        let clients = br#"[
            {"at":[10,10],"size":[100,100],"monitor":0,"pinned":true,
             "workspace":{"name":"4"},"class":"pinned","title":"Pinned"},
            {"at":[0,0],"size":[200,200],"monitor":0,"pinned":false,
             "workspace":{"name":"special:scratch"},"class":"scratch","title":"Scratch"}
        ]"#;
        let windows = parse_hyprland_windows(clients, monitors).unwrap();
        assert_eq!(windows.len(), 2, "{windows:?}");
        assert_eq!(windows[0].label, "pinned");
        assert_eq!(windows[1].label, "scratch");
    }

    #[test]
    fn drops_a_stale_rect_that_points_at_another_monitor() {
        let monitors = br#"[
            {"id":0,"x":1920,"y":0,"width":1920,"height":1080,"scale":1.0,
             "activeWorkspace":{"name":"1"},"specialWorkspace":{"name":""}}
        ]"#;
        // The workspace matches, but the rect lies left of the monitor: this
        // is the shape a hidden workspace leaves behind.
        let clients = br#"[{"at":[11,55],"size":[941,1014],"monitor":0,"pinned":false,
            "workspace":{"name":"1"},"class":"stale","title":"Stale"}]"#;
        assert!(parse_hyprland_windows(clients, monitors)
            .unwrap()
            .is_empty());
    }

    #[test]
    fn joins_window_labels_without_duplicating_them() {
        assert_eq!(join_label("kitty", "kitty"), "kitty");
        assert_eq!(join_label("kitty", "~/Dev"), "kitty — ~/Dev");
        assert_eq!(join_label("kitty", ""), "kitty");
        assert_eq!(join_label("", "~/Dev"), "~/Dev");
        assert_eq!(join_label("", ""), "");
    }

    #[test]
    fn lists_sway_leaves_and_hides_hidden_workspaces() {
        let json = br#"{"type":"root","nodes":[
            {"type":"output","nodes":[
                {"type":"workspace","visible":true,"nodes":[
                    {"type":"con","app_id":"foot","name":"shell","rect":{"x":10,"y":20,"width":400,"height":300}}
                ]},
                {"type":"workspace","visible":false,"nodes":[
                    {"type":"con","app_id":"vim","name":"edit","rect":{"x":0,"y":0,"width":800,"height":600}}
                ]}
            ]}
        ]}"#;
        let windows = parse_sway_windows(json).unwrap();
        assert_eq!(windows.len(), 1);
        assert_eq!(windows[0].geometry, Rect::new(10, 20, 400, 300));
        assert_eq!(windows[0].label, "foot — shell");
    }

    #[test]
    fn lists_kwin_probe_lines_and_ignores_junk() {
        let windows =
            parse_kwin_windows(b"1920 0 1600 900\n-20 30 800 600\nnot a window\n").unwrap();
        assert_eq!(windows.len(), 2);
        assert_eq!(windows[0].geometry, Rect::new(1920, 0, 1600, 900));
        assert_eq!(windows[1].geometry, Rect::new(-20, 30, 800, 600));
        assert!(windows[0].label.is_empty());
    }

    #[test]
    fn rejects_unreliable_window_data() {
        assert!(parse_hyprland_active_window(br#"{"at":[0,0],"size":[0,1]}"#).is_err());
        assert!(parse_sway_active_window(br#"{"nodes":[]}"#).is_err());
        assert!(parse_kwin_active_window(b"1 2 3").is_err());
        assert!(parse_kwin_active_window(b"a b c d").is_err());
        assert!(parse_kwin_active_window(b"0 0 0 500").is_err());
    }
}

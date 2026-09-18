use std::ffi::OsString;
use std::process::{Command, Output};

use serde_json::Value;

use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect};

use super::active_output::Session;

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

    /// KDE Plasma's window metadata, in two routes. `kdotool` comes first when
    /// it is installed: it drives the same KWin scripting interface, but gets
    /// its result back over D-Bus instead of through KWin's script logging, so
    /// it is the one route that does not need the journal. Otherwise the probe
    /// loads a tiny script through `org.kde.kwin.Scripting` that logs the
    /// active window's frame geometry (`workspace.activeWindow` on Plasma 6,
    /// `activeClient` on Plasma 5) and reads the marked line back from the user
    /// journal. Everything is best-effort: on compositors without KWin the DBus
    /// name is absent and the probe fails in milliseconds.
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
// Stacking order, bottom to top: the picker takes the last window under the
// pointer, so the order is what makes that window the one on top.  KWin's own
// hit test walks this same list from its end for the same reason.  It is a
// property, not a method; `windowList()` is creation order and only stands in
// for a KWin too old to have the stacking order at all.
let stacking = workspace.stackingOrder;
if (typeof stacking === "function") {
    stacking = stacking.call(workspace);
}
const windows = stacking && stacking.length ? stacking
    : (workspace.windowList ? workspace.windowList()
        : (workspace.clientList ? workspace.clientList() : []));
for (let index = 0; index < windows.length; ++index) {
    const w = windows[index];
    if (!w || w.deleted || w.hidden || w.minimized) {
        continue;
    }
    // Panels, docks and splash screens are not pickable windows.
    if (w.normalWindow === false) {
        continue;
    }
    // The picker's own overlay is a normal, full-screen window as far as KWin
    // is concerned (layer-shell surfaces are not special-cased here), and it
    // sits on top of the stack while the session is open.  Re-listing the
    // windows mid-session would otherwise offer the overlay itself as the
    // candidate under the pointer — a full-screen rectangle that wins every
    // hit test, so the highlight covers the whole desktop and no real window
    // can be picked.  Match on the class and on our own scope name.
    const klass = String(w.resourceClass || "");
    if (klass === "vshot-qt-ui" || klass.indexOf("vshot") === 0) {
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
    /// The rule matches the picker's own: the last window containing the point
    /// wins, and the list is ordered bottom to top, so that is the window on
    /// top — the one the compositor would hand the click to.
    /// `None` means the compositor cannot answer (no window list, or nothing
    /// there), which is not an error: the caller keeps what the picker had.
    pub fn window_at(&self, point: Point) -> Option<Rect> {
        let windows = self.windows().ok()?;
        topmost_containing(windows.iter().map(|window| window.geometry), point)
    }
}

/// The last rect that contains `point`.  The window list is ordered bottom to
/// top (see [`parse_hyprland_windows`]), so the last hit is the one on top.
fn topmost_containing(rects: impl Iterator<Item = Rect>, point: Point) -> Option<Rect> {
    rects.filter(|rect| rect.contains(point)).last()
}

/// A compositor that can be asked about windows.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Compositor {
    Hyprland,
    Sway,
    KWin,
}

const COMPOSITORS: [Compositor; 3] = [Compositor::Hyprland, Compositor::Sway, Compositor::KWin];

impl Compositor {
    fn name(self) -> &'static str {
        match self {
            Self::Hyprland => "Hyprland",
            Self::Sway => "Sway",
            Self::KWin => "KWin",
        }
    }

    fn session(self) -> Session {
        match self {
            Self::Hyprland => Session::Hyprland,
            Self::Sway => Session::Sway,
            Self::KWin => Session::KWin,
        }
    }

    fn active_window_command(self) -> WindowCommand {
        match self {
            Self::Hyprland => WindowCommand::hyprland(),
            Self::Sway => WindowCommand::sway(),
            Self::KWin => WindowCommand::kwin(),
        }
    }

    fn parse_active_window(self, stdout: &[u8]) -> Result<ActiveWindow> {
        match self {
            Self::Hyprland => parse_hyprland_active_window(stdout),
            Self::Sway => parse_sway_active_window(stdout),
            Self::KWin => parse_kwin_active_window(stdout),
        }
    }

    /// The focused window as this compositor reports it.
    fn active_window<R: WindowCommandRunner>(self, runner: &R) -> Result<ActiveWindow> {
        let command = self.active_window_command();
        let output = runner.run(&command)?;
        if !output.status.success() {
            return Err(VshotError::ActiveWindowUnavailable(match self {
                // The probe is a shell script, so "bash exited 4" would say
                // nothing about what actually went wrong.
                Self::KWin => format!(
                    "the KWin scripting probe exited with {} — it loads a script over D-Bus \
                     (`gdbus` or `dbus-send`) and reads the answer back from the user journal",
                    output.status
                ),
                _ => format!(
                    "`{}` exited with {}",
                    command.program.to_string_lossy(),
                    output.status
                ),
            }));
        }
        self.parse_active_window(&output.stdout)
    }

    /// The windows this compositor shows, as picking wants them.
    fn window_list<R: WindowCommandRunner>(self, runner: &R) -> Result<Vec<WindowCandidate>> {
        match self {
            Self::Hyprland => hyprland_windows(runner),
            Self::Sway => run_command(runner, &WindowCommand::sway())
                .and_then(|bytes| parse_sway_windows(&bytes)),
            Self::KWin => run_command(runner, &WindowCommand::kwin_list())
                .and_then(|bytes| parse_kwin_windows(&bytes)),
        }
    }
}

/// The compositors worth asking, mirroring [`Session`]: only the session's own
/// compositor is queried, because a second one running alongside answers for
/// itself. A Plasma session started from a Hyprland terminal keeps
/// `HYPRLAND_INSTANCE_SIGNATURE`, so `hyprctl` goes on answering and its
/// windows would be read as if they were on the KDE desktop.
///
/// A session whose compositor has neither query (niri) yields nothing to ask,
/// which is an honest answer: the caller falls back to the pixels.
fn compositors_for(session: Session) -> Vec<Compositor> {
    COMPOSITORS
        .into_iter()
        .filter(|compositor| session == Session::Unknown || compositor.session() == session)
        .collect()
}

/// Lists the windows the compositor shows, geometry in global logical pixels.
///
/// Interactive picking uses this to highlight what the pointer is over, so it
/// wants every window rather than the focused one — and it has to be honest
/// about not knowing: a compositor that reports no list is collected into the
/// error instead of being guessed at.
pub fn find_windows<R: WindowCommandRunner>(runner: &R) -> Result<Vec<WindowCandidate>> {
    find_windows_for(Session::detect(), runner)
}

fn find_windows_for<R: WindowCommandRunner>(
    session: Session,
    runner: &R,
) -> Result<Vec<WindowCandidate>> {
    let mut reasons: Vec<String> = Vec::new();
    for compositor in compositors_for(session) {
        match compositor.window_list(runner) {
            Ok(windows) if !windows.is_empty() => return Ok(windows),
            Ok(_) => reasons.push(format!("{} shows no window", compositor.name())),
            Err(error) => reasons.push(format!("{}: {error}", compositor.name())),
        }
    }
    if reasons.is_empty() {
        return Err(VshotError::WindowPickUnavailable(
            "this session's compositor has no window list to offer (only Hyprland, Sway and \
             KWin have one)"
                .into(),
        ));
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
    find_active_window_for(Session::detect(), runner)
}

fn find_active_window_for<R: WindowCommandRunner>(
    session: Session,
    runner: &R,
) -> Result<ActiveWindow> {
    let mut reasons: Vec<String> = Vec::new();
    for compositor in compositors_for(session) {
        match compositor.active_window(runner) {
            Ok(window) => return Ok(window),
            Err(error) => reasons.push(format!("{}: {error}", compositor.name())),
        }
    }
    if reasons.is_empty() {
        return Err(VshotError::ActiveWindowUnavailable(
            "this session's compositor has no active-window query (only Hyprland, Sway and KWin \
             have one)"
                .into(),
        ));
    }
    Err(VshotError::ActiveWindowUnavailable(format!(
        "no compositor reported an active window ({})",
        reasons.join("; ")
    )))
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
///
/// The list comes back in stacking order, bottom to top, because the picker
/// takes the *last* candidate under the pointer (see [`topmost_containing`]).
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

    // Each client lands in one of the three passes Hyprland both draws and
    // hit-tests in — tiled, floating, pinned floating — and the stable sort
    // keeps `hyprctl`'s own order inside a pass.  That order is the stacking
    // order: Hyprland keeps its window vector bottom to top and raises a
    // focused window to the end of it, and `hyprctl clients` reports that same
    // vector in that same order.  Without the three passes a floating window
    // would only outrank a tiled one when it happened to sit later in the
    // vector, which focusing it usually achieves but does not guarantee.
    let mut windows: Vec<(u8, WindowCandidate)> = Vec::new();
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
        let floating = client.get("floating").and_then(Value::as_bool) == Some(true);
        if !pinned && !shown.is_showing_client_workspace(client) {
            continue;
        }
        if !shown.contains(geometry) {
            continue;
        }
        // Pinning only lifts a window that floats too: Hyprland's pinned render
        // pass and its hit test both ask for the two flags together.
        let pass = match (floating, pinned) {
            (true, true) => 2,
            (true, false) => 1,
            (false, _) => 0,
        };
        windows.push((
            pass,
            WindowCandidate {
                geometry,
                label: join_label(
                    client.get("class").and_then(Value::as_str).unwrap_or(""),
                    client.get("title").and_then(Value::as_str).unwrap_or(""),
                ),
            },
        ));
    }
    windows.sort_by_key(|(pass, _)| *pass);
    Ok(windows.into_iter().map(|(_, window)| window).collect())
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
///
/// Floating containers are listed last, because sway hit-tests them before the
/// tiling tree and draws them above it, and the picker takes the last candidate
/// under the pointer (see [`parse_hyprland_windows`]).
pub fn parse_sway_windows(bytes: &[u8]) -> Result<Vec<WindowCandidate>> {
    let value: Value = serde_json::from_slice(bytes).map_err(|error| {
        VshotError::WindowPickUnavailable(format!("invalid Sway tree JSON: {error}"))
    })?;
    let mut tiled = Vec::new();
    let mut floating = Vec::new();
    collect_sway_windows(&value, true, false, &mut tiled, &mut floating);
    tiled.extend(floating);
    Ok(tiled)
}

/// `is_floating` is true for everything reached through a `floating_nodes`
/// array: the whole subtree floats, so it belongs in the floating list — and
/// sway keeps those in the order it raises them, the last one on top.
fn collect_sway_windows(
    node: &Value,
    visible: bool,
    is_floating: bool,
    tiled: &mut Vec<WindowCandidate>,
    floating: &mut Vec<WindowCandidate>,
) {
    // Only workspaces carry a meaningful `visible`; a hidden one hides its
    // whole subtree.
    let visible = visible
        && !(node.get("type").and_then(Value::as_str) == Some("workspace")
            && node.get("visible").and_then(Value::as_bool) == Some(false));
    let mut children: Vec<(&Value, bool)> = Vec::new();
    for key in ["nodes", "floating_nodes"] {
        let nested_floating = is_floating || key == "floating_nodes";
        if let Some(nodes) = node.get(key).and_then(Value::as_array) {
            children.extend(nodes.iter().map(|child| (child, nested_floating)));
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
        let candidate = WindowCandidate {
            geometry: Rect::new(x, y, width, height),
            label: join_label(
                class,
                node.get("name").and_then(Value::as_str).unwrap_or(""),
            ),
        };
        if is_floating {
            floating.push(candidate);
        } else {
            tiled.push(candidate);
        }
        return;
    }
    for (child, nested_floating) in children {
        collect_sway_windows(child, visible, nested_floating, tiled, floating);
    }
}

/// The KWin probe in `list` mode: one `x y width height` line per window, in
/// KWin's stacking order bottom to top, which the probe asks for by iterating
/// `workspace.stackingOrder`.  The probe reports no titles, so every candidate
/// is unlabelled.
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
/// say the same thing.  niri's window replies are labelled with it too.
pub(crate) fn join_label(class: &str, title: &str) -> String {
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
    use std::cell::RefCell;
    use std::os::unix::process::ExitStatusExt;
    use std::process::ExitStatus;

    use super::*;

    /// Resolving a click takes the window on top under it — the *last*
    /// candidate containing the point, because the list is ordered bottom to
    /// top, exactly like the picker's own hit test — and a point on bare
    /// desktop resolves to nothing.
    #[test]
    fn the_window_on_top_under_a_point_wins() {
        let desktop = Rect::new(0, 0, 1920, 1080);
        let inner = Rect::new(100, 100, 800, 600);
        let floating = Rect::new(800, 500, 400, 300);
        // Bottom to top: desktop, inner, floating.
        let windows = [desktop, inner, floating];
        let resolved = |point| topmost_containing(windows.into_iter(), point);
        assert_eq!(resolved(Point::new(900, 700)), Some(floating));
        assert_eq!(resolved(Point::new(400, 400)), Some(inner));
        assert_eq!(resolved(Point::new(1900, 1000)), Some(desktop));
        assert_eq!(resolved(Point::new(2000, 100)), None);
        // Right and bottom edges belong to the next window, exactly as in the
        // picker's own hit test: (900, 400) is on the inner rect's right edge,
        // so only the full-screen window is left.
        assert_eq!(resolved(Point::new(900, 400)), Some(desktop));
        assert_eq!(resolved(Point::new(1900, 1100)), None);
    }

    /// The rule is "on top", not "smallest".  This is the shape that used to
    /// resolve to the wrong window: a floating window larger than the tiled one
    /// it sits on, for which the old smallest-area rule handed back the tiled
    /// window underneath.
    #[test]
    fn a_floating_window_larger_than_the_one_under_it_still_wins() {
        let tiled = Rect::new(0, 0, 400, 300);
        let floating = Rect::new(100, 100, 800, 600);
        let resolved = |point| topmost_containing([tiled, floating].into_iter(), point);
        // Inside both: the larger one wins because it is the one on top.
        assert_eq!(resolved(Point::new(200, 200)), Some(floating));
        // Outside the floating window the tiled one is still what is there.
        assert_eq!(resolved(Point::new(50, 50)), Some(tiled));
        assert_eq!(resolved(Point::new(950, 700)), None);
    }

    /// Hyprland's own stacking, which the candidate order has to mirror: the
    /// tiled window first, then the floating one, then the pinned one.  The
    /// clients arrive in a different order, so only the sort can produce this.
    #[test]
    fn lists_hyprland_floating_and_pinned_windows_last() {
        let monitors = br#"[{"id":0,"x":0,"y":0,"width":1920,"height":1080,"scale":1.0,
            "activeWorkspace":{"name":"1"},"specialWorkspace":{"name":""}}]"#;
        let clients = br#"[
            {"at":[100,100],"size":[400,300],"monitor":0,"floating":false,"pinned":false,
             "workspace":{"name":"1"},"class":"tiled","title":"Tiled"},
            {"at":[200,200],"size":[200,100],"monitor":0,"floating":true,"pinned":true,
             "workspace":{"name":"1"},"class":"pinned","title":"Pinned"},
            {"at":[150,150],"size":[300,200],"monitor":0,"floating":true,"pinned":false,
             "workspace":{"name":"1"},"class":"floating","title":"Floating"}
        ]"#;
        let windows = parse_hyprland_windows(clients, monitors).unwrap();
        let labels: Vec<&str> = windows.iter().map(|window| window.label.as_str()).collect();
        assert_eq!(labels, ["tiled", "floating", "pinned"]);
        // A click inside all three lands on the pinned window, the topmost.
        assert_eq!(
            topmost_containing(
                windows.iter().map(|window| window.geometry),
                Point::new(250, 240)
            ),
            Some(Rect::new(200, 200, 200, 100))
        );
    }

    /// A pinned window that does not float is not lifted: Hyprland asks for
    /// both flags before it draws or hit-tests a window as pinned.
    #[test]
    fn lists_a_pinned_tiled_hyprland_window_as_tiled() {
        let monitors = br#"[{"id":0,"x":0,"y":0,"width":1920,"height":1080,"scale":1.0,
            "activeWorkspace":{"name":"1"},"specialWorkspace":{"name":""}}]"#;
        let clients = br#"[
            {"at":[0,0],"size":[800,600],"monitor":0,"floating":false,"pinned":true,
             "workspace":{"name":"1"},"class":"pinned-tile","title":"Pinned tile"}
        ]"#;
        let windows = parse_hyprland_windows(clients, monitors).unwrap();
        assert_eq!(windows.len(), 1);
        assert_eq!(windows[0].label, "pinned-tile — Pinned tile");
    }

    /// Sway hit-tests and draws floating containers above the tiling tree, so
    /// they come last in the list even though the tree visits them first.
    #[test]
    fn lists_floating_sway_containers_last() {
        let tree = br#"{"type":"root","nodes":[{"type":"output","nodes":[{"type":"workspace",
            "visible":true,"name":"1",
            "nodes":[{"type":"con","app_id":"tiled","rect":{"x":0,"y":0,"width":800,"height":600}}],
            "floating_nodes":[{"type":"floating_con","nodes":[{"type":"con","app_id":"float",
                "rect":{"x":100,"y":100,"width":300,"height":200}}]}]}]}]}"#;
        let windows = parse_sway_windows(tree).unwrap();
        assert_eq!(windows.len(), 2, "{windows:?}");
        assert_eq!(windows[0].label, "tiled");
        assert_eq!(windows[1].label, "float");
        assert_eq!(
            topmost_containing(
                windows.iter().map(|window| window.geometry),
                Point::new(200, 150)
            ),
            Some(Rect::new(100, 100, 300, 200))
        );
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

    /// The KWin probe has to skip vshot's own overlay.
    ///
    /// The picker re-lists the windows while its session is open, and the
    /// overlay it is drawing with is a full-screen, `normalWindow` surface on
    /// top of the stack — so it passed every other filter and became the
    /// candidate under the pointer, veiling the whole desktop instead of
    /// highlighting a window.  The fix is a class check in the probe script.
    ///
    /// This is a guard, not a behavioural test: the filter is JavaScript
    /// embedded in a shell script, so there is no JavaScript to run here.  What
    /// it catches is the realistic regression — someone refactoring the probe
    /// loop drops the check — and the live KDE check is what proves it works.
    #[test]
    fn the_kwin_probe_skips_vshots_own_overlay() {
        assert!(
            KWIN_PROBE.contains(r#"String(w.resourceClass || "")"#),
            "the probe is expected to read the window class"
        );
        assert!(
            KWIN_PROBE.contains(r#"indexOf("vshot") === 0"#),
            "and to skip windows whose class belongs to vshot"
        );
        // The filters that were already there must survive the change.
        assert!(KWIN_PROBE.contains("w.normalWindow === false"));
        assert!(KWIN_PROBE.contains("w.deleted"));
    }

    #[test]
    fn rejects_unreliable_window_data() {
        assert!(parse_hyprland_active_window(br#"{"at":[0,0],"size":[0,1]}"#).is_err());
        assert!(parse_sway_active_window(br#"{"nodes":[]}"#).is_err());
        assert!(parse_kwin_active_window(b"1 2 3").is_err());
        assert!(parse_kwin_active_window(b"a b c d").is_err());
        assert!(parse_kwin_active_window(b"0 0 0 500").is_err());
    }

    /// Answers every command the same way and remembers what it was asked, so a
    /// test can see which compositors a session consults.
    struct ScriptedRunner {
        status: ExitStatus,
        stdout: Vec<u8>,
        asked: RefCell<Vec<OsString>>,
    }

    impl ScriptedRunner {
        /// A runner whose every command fails, so the loop runs out of
        /// compositors and the calls can be counted.
        fn failing() -> Self {
            Self {
                status: ExitStatus::from_raw(1),
                stdout: Vec::new(),
                asked: RefCell::new(Vec::new()),
            }
        }

        /// A runner whose every command succeeds with this output.
        fn replying(stdout: &[u8]) -> Self {
            Self {
                status: ExitStatus::from_raw(0),
                stdout: stdout.to_vec(),
                asked: RefCell::new(Vec::new()),
            }
        }

        fn asked(&self) -> Vec<String> {
            self.asked
                .borrow()
                .iter()
                .map(|program| program.to_string_lossy().into_owned())
                .collect()
        }
    }

    impl WindowCommandRunner for ScriptedRunner {
        fn run(&self, command: &WindowCommand) -> Result<Output> {
            self.asked.borrow_mut().push(command.program.clone());
            Ok(Output {
                status: self.status,
                stdout: self.stdout.clone(),
                stderr: Vec::new(),
            })
        }
    }

    /// Only the session's own compositor is asked. A Plasma session started
    /// from a Hyprland terminal keeps `HYPRLAND_INSTANCE_SIGNATURE`, so
    /// `hyprctl` goes on answering — and its answer is a window on a desktop
    /// the user is not looking at.
    #[test]
    fn a_kde_session_asks_kwin_and_not_hyprland() {
        let runner = ScriptedRunner::failing();
        assert!(find_active_window_for(Session::KWin, &runner).is_err());
        assert_eq!(runner.asked(), ["bash"]);

        let runner = ScriptedRunner::failing();
        assert!(find_windows_for(Session::KWin, &runner).is_err());
        assert_eq!(runner.asked(), ["bash"]);
    }

    /// The KDE session's answer is read as the window: the probe runs, and its
    /// single line is the focused window.
    #[test]
    fn a_kde_session_reads_the_focused_window_from_the_probe() {
        let runner = ScriptedRunner::replying(b"1920 0 1600 900\n");
        let window = find_active_window_for(Session::KWin, &runner).unwrap();
        assert_eq!(window.geometry, Rect::new(1920, 0, 1600, 900));
        assert_eq!(window.source, WindowSource::KWin);
        assert_eq!(runner.asked(), ["bash"]);
    }

    /// With nothing in the environment naming a compositor every probe is worth
    /// trying, in the order the README gives.
    #[test]
    fn an_unknown_session_asks_every_compositor_in_order() {
        let runner = ScriptedRunner::failing();
        assert!(find_active_window_for(Session::Unknown, &runner).is_err());
        assert_eq!(runner.asked(), ["hyprctl", "swaymsg", "bash"]);

        // The window list goes through the same three: Hyprland's first query
        // (`hyprctl clients`) is already failing here, so its monitor list is
        // never reached.
        let runner = ScriptedRunner::failing();
        assert!(find_windows_for(Session::Unknown, &runner).is_err());
        assert_eq!(runner.asked(), ["hyprctl", "swaymsg", "bash"]);
    }

    #[test]
    fn only_the_sessions_compositor_is_offered_to_picking() {
        assert_eq!(compositors_for(Session::Hyprland), [Compositor::Hyprland]);
        assert_eq!(compositors_for(Session::Sway), [Compositor::Sway]);
        assert_eq!(compositors_for(Session::KWin), [Compositor::KWin]);
        assert_eq!(compositors_for(Session::Unknown), COMPOSITORS);
        // niri has no window list and no active-window geometry, so there is
        // nothing to ask and the caller falls back to the pixels.
        assert!(compositors_for(Session::Niri).is_empty());
    }

    /// A session whose compositor cannot answer says that, instead of claiming
    /// some compositor failed.
    #[test]
    fn a_compositor_without_window_queries_says_so() {
        let runner = ScriptedRunner::failing();
        let error = find_windows_for(Session::Niri, &runner).unwrap_err();
        assert!(
            format!("{error}").contains("no window list to offer"),
            "{error}"
        );
        let error = find_active_window_for(Session::Niri, &runner).unwrap_err();
        assert!(
            format!("{error}").contains("no active-window query"),
            "{error}"
        );
        assert!(runner.asked().is_empty());
    }
}

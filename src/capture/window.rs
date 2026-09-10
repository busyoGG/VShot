use std::ffi::OsString;
use std::process::{Command, Output};

use serde_json::Value;

use crate::error::{Result, VshotError};
use crate::geometry::Rect;

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
        Self {
            program: OsString::from("bash"),
            args: vec![OsString::from("-c"), OsString::from(KWIN_PROBE)],
        }
    }
}

/// See [`WindowCommand::kwin`]. Prints `x y width height` in global logical
/// pixels on success; any other outcome must exit non-zero or print nothing.
const KWIN_PROBE: &str = r##"
set -u
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

if command -v kdotool >/dev/null 2>&1; then
    geo="$(run_limited kdotool getactivewindow getwindowgeometry --shell 2>/dev/null)" || geo=""
    if [ -n "$geo" ]; then
        eval "$geo"
        if [ -n "${X:-}" ] && [ -n "${Y:-}" ] && [ -n "${WIDTH:-}" ] && [ -n "${HEIGHT:-}" ] \
            && [ "${WIDTH:-0}" -gt 0 ] 2>/dev/null && [ "${HEIGHT:-0}" -gt 0 ] 2>/dev/null; then
            printf '%s %s %s %s\n' "$X" "$Y" "$WIDTH" "$HEIGHT"
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

line=""
if command -v journalctl >/dev/null 2>&1; then
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
        sleep 0.06
        line="$(journalctl --user -n 200 --output=cat 2>/dev/null \
            | grep -aF "$marker" | tail -n 1)"
        [ -n "$line" ] && break
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

[ -n "$line" ] || exit 4
payload="${line#* }"
[ "$payload" = "null" ] && exit 4
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
}

#[derive(Clone, Copy, Debug, Default)]
pub struct ProcessWindowProvider;

impl CompositorWindowProvider for ProcessWindowProvider {
    fn active_window(&self) -> Result<ActiveWindow> {
        find_active_window(&ProcessWindowRunner)
    }
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
    fn rejects_unreliable_window_data() {
        assert!(parse_hyprland_active_window(br#"{"at":[0,0],"size":[0,1]}"#).is_err());
        assert!(parse_sway_active_window(br#"{"nodes":[]}"#).is_err());
        assert!(parse_kwin_active_window(b"1 2 3").is_err());
        assert!(parse_kwin_active_window(b"a b c d").is_err());
        assert!(parse_kwin_active_window(b"0 0 0 500").is_err());
    }
}

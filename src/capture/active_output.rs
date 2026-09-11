//! Which output the user is looking at.
//!
//! A pin belongs on the monitor the user is working on, not on whatever Qt
//! calls the primary one. The pin daemon renders every pin on every output, so
//! it cannot answer this: a windowless Qt process sees the pointer at (0, 0)
//! and has no notion of the focused output. The compositor does know, so the
//! CLI — which runs in the user's session, next to the focused window — asks it
//! and passes the answer along with the pin request.
//!
//! Detection is best-effort and never fatal: a compositor without a probe just
//! leaves the choice to the daemon, which falls back to the output Qt reports
//! as primary.

use std::process::Command;

use serde_json::Value;

use crate::geometry::Rect;

/// One probe, run as a subprocess. Split out from the running of it so the
/// parsing below can be tested without a compositor.
#[derive(Clone, Debug, Eq, PartialEq)]
struct OutputCommand {
    program: &'static str,
    args: &'static [&'static str],
}

impl OutputCommand {
    fn hyprland_monitors() -> Self {
        Self {
            program: "hyprctl",
            args: &["monitors", "-j"],
        }
    }

    fn sway_outputs() -> Self {
        Self {
            program: "swaymsg",
            args: &["-t", "get_outputs"],
        }
    }

    /// niri's IPC answers with the focused output directly. It is only
    /// reachable through `$NIRI_SOCKET`, which a session sets for its own
    /// clients, so the probe fails fast anywhere else.
    fn niri_focused_output() -> Self {
        Self {
            program: "niri",
            args: &["msg", "--json", "focused-output"],
        }
    }
}

/// All probes in the order they are tried; the first one that succeeds wins.
/// Hyprland advertises its outputs to Sway's IPC as well, so the Hyprland
/// probe has to be decisive: its "no monitor is focused" answer is a real
/// answer, not a reason to try something else.
fn probes() -> [OutputCommand; 3] {
    [
        OutputCommand::hyprland_monitors(),
        OutputCommand::sway_outputs(),
        OutputCommand::niri_focused_output(),
    ]
}

/// Global logical rect of the output the user is working on, if the compositor
/// reports one. The pointer decides when it can be read: a pin belongs where
/// the user is pointing at, and on a click-to-focus setup that is not
/// necessarily the output holding the keyboard focus. Focus is the fallback.
pub fn active_output() -> Option<Rect> {
    for probe in probes() {
        let output = Command::new(probe.program).args(probe.args).output();
        let Ok(output) = output else {
            continue;
        };
        if !output.status.success() {
            continue;
        }
        let parsed = match probe.program {
            "hyprctl" => hyprland_output(&output.stdout, cursor_position()),
            "swaymsg" => parse_sway_outputs(&output.stdout),
            _ => parse_niri_output(&output.stdout),
        };
        if parsed.is_some() {
            return parsed;
        }
    }
    None
}

/// Global logical pointer position, when the compositor reports one. The
/// daemon cannot see this for itself: a windowless process reads (0, 0).
fn cursor_position() -> Option<(i64, i64)> {
    let output = Command::new("hyprctl").arg("cursorpos").output().ok()?;
    if !output.status.success() {
        return None;
    }
    parse_cursor(&output.stdout)
}

/// The monitor under the pointer, else the focused one.
fn hyprland_output(monitors: &[u8], cursor: Option<(i64, i64)>) -> Option<Rect> {
    if let Some(cursor) = cursor {
        if let Some(rect) = monitor_rects(monitors)
            .into_iter()
            .find(|(_, rect)| contains(*rect, cursor))
            .map(|(_, rect)| rect)
        {
            return Some(rect);
        }
    }
    parse_hyprland_monitors(monitors)
}

fn contains(rect: Rect, point: (i64, i64)) -> bool {
    match (i32::try_from(point.0), i32::try_from(point.1)) {
        (Ok(x), Ok(y)) => rect.contains(crate::geometry::Point::new(x, y)),
        _ => false,
    }
}

/// `hyprctl cursorpos` prints `x, y` in global logical pixels.
fn parse_cursor(bytes: &[u8]) -> Option<(i64, i64)> {
    let text = std::str::from_utf8(bytes).ok()?;
    let (x, y) = text.trim().split_once(',')?;
    Some((x.trim().parse().ok()?, y.trim().parse().ok()?))
}

/// Reads the focused monitor's logical geometry out of `hyprctl monitors -j`.
fn parse_hyprland_monitors(bytes: &[u8]) -> Option<Rect> {
    monitor_rects(bytes)
        .into_iter()
        .find(|(focused, _)| *focused)
        .map(|(_, rect)| rect)
}

/// Every Hyprland monitor as `(focused, logical rect)`, in compositor order.
///
/// The array is ordered by the compositor's monitor id, which the entries also
/// carry, and `focused` marks the one holding the keyboard focus.
///
/// Hyprland reports a monitor's `x`/`y` in the logical layout while `width`
/// and `height` are the native mode size, so the size has to be divided by
/// `scale` before it can be compared with a Qt screen geometry or with a
/// pointer position.
fn monitor_rects(bytes: &[u8]) -> Vec<(bool, Rect)> {
    let Ok(monitors) = serde_json::from_slice::<Vec<Value>>(bytes) else {
        return Vec::new();
    };
    monitors
        .iter()
        .filter_map(|monitor| {
            let x = monitor.get("x").and_then(Value::as_i64)?;
            let y = monitor.get("y").and_then(Value::as_i64)?;
            let width = monitor.get("width").and_then(Value::as_u64)?;
            let height = monitor.get("height").and_then(Value::as_u64)?;
            let scale = monitor.get("scale").and_then(Value::as_f64).unwrap_or(1.0);
            if !scale.is_finite() || scale <= 0.0 {
                return None;
            }
            let focused = monitor.get("focused").and_then(Value::as_bool) == Some(true);
            let rect = rect(
                x,
                y,
                (width as f64 / scale).round() as u64,
                (height as f64 / scale).round() as u64,
            )?;
            Some((focused, rect))
        })
        .collect()
}

/// Reads the focused output's logical geometry out of `swaymsg -t
/// get_outputs`. Sway exposes the compositor's own hit-testing instead of a
/// rectangle, which is what "the monitor the user is on" really means; the
/// rect is only needed to hand the choice to the daemon.
fn parse_sway_outputs(bytes: &[u8]) -> Option<Rect> {
    let outputs: Vec<Value> = serde_json::from_slice(bytes).ok()?;
    let output = outputs
        .iter()
        .find(|output| output.get("focused").and_then(Value::as_bool) == Some(true))?;
    let geometry = output.get("rect")?;
    let x = geometry.get("x").and_then(Value::as_i64)?;
    let y = geometry.get("y").and_then(Value::as_i64)?;
    let width = geometry.get("width").and_then(Value::as_u64)?;
    let height = geometry.get("height").and_then(Value::as_u64)?;
    rect(x, y, width, height)
}

/// Reads the focused output's logical geometry out of `niri msg --json
/// focused-output`.
///
/// niri reports the geometry it uses for layout in `logical`, which is already
/// in the same logical space as a pointer position or a Qt screen geometry, so
/// unlike Hyprland nothing has to be divided by the scale. The scale is
/// deliberately left out of the rect for that reason. A disabled output has no
/// `logical` geometry and is rejected rather than guessed at.
fn parse_niri_output(bytes: &[u8]) -> Option<Rect> {
    let output: Value = serde_json::from_slice(bytes).ok()?;
    let logical = output.get("logical")?;
    let x = logical.get("x").and_then(Value::as_i64)?;
    let y = logical.get("y").and_then(Value::as_i64)?;
    let width = logical.get("width").and_then(Value::as_u64)?;
    let height = logical.get("height").and_then(Value::as_u64)?;
    rect(x, y, width, height)
}

fn rect(x: i64, y: i64, width: u64, height: u64) -> Option<Rect> {
    Some(Rect::new(
        i32::try_from(x).ok()?,
        i32::try_from(y).ok()?,
        u32::try_from(width).ok()?,
        u32::try_from(height).ok()?,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_the_focused_hyprland_monitor() {
        // The first entry is focused and is the non-zero origin one, so a
        // parser that ignored `focused` would report DP-3's 0,0 instead. Its
        // 3840x2160 mode at scale 2 is 1920x1080 of logical layout.
        let json = br#"[
            {"id":0,"name":"DP-2","x":1920,"y":0,"width":3840,"height":2160,
             "scale":2,"focused":true},
            {"id":1,"name":"DP-3","x":0,"y":0,"width":1920,"height":1080,
             "scale":1,"focused":false}
        ]"#;
        assert_eq!(
            parse_hyprland_monitors(json),
            Some(Rect::new(1920, 0, 1920, 1080))
        );
    }

    #[test]
    fn the_pointer_decides_the_output_before_the_focus_does() {
        let json = br#"[
            {"name":"DP-2","x":1920,"y":0,"width":3840,"height":2160,
             "scale":2,"focused":true},
            {"name":"DP-3","x":0,"y":0,"width":1920,"height":1080,
             "scale":1,"focused":false}
        ]"#;
        // Keyboard focus on the 4K monitor, pointer on the 1080p one: the pin
        // belongs where the user is pointing.
        assert_eq!(
            hyprland_output(json, Some((1666, 502))),
            Some(Rect::new(0, 0, 1920, 1080))
        );
        // Pointer over the 4K monitor, which starts at logical x = 1920.
        assert_eq!(
            hyprland_output(json, Some((2000, 10))),
            Some(Rect::new(1920, 0, 1920, 1080))
        );
        // A pointer the layout does not cover falls back to the focused
        // monitor rather than picking nothing.
        assert_eq!(
            hyprland_output(json, Some((-5000, -5000))),
            Some(Rect::new(1920, 0, 1920, 1080))
        );
        // No pointer reading at all: focus decides.
        assert_eq!(
            hyprland_output(json, None),
            Some(Rect::new(1920, 0, 1920, 1080))
        );
    }

    #[test]
    fn the_logical_right_and_bottom_edges_are_exclusive() {
        let json = br#"[{"name":"A","x":0,"y":0,"width":1920,"height":1080,
                        "scale":1,"focused":true}]"#;
        // The last pixel is inside; one past it is not, and then the focused
        // fallback answers instead of a bogus hit.
        assert_eq!(
            hyprland_output(json, Some((1919, 1079))),
            Some(Rect::new(0, 0, 1920, 1080))
        );
        assert_eq!(
            hyprland_output(json, Some((1920, 1079))),
            Some(Rect::new(0, 0, 1920, 1080))
        );
        // A negative-origin layout is hit-tested just the same.
        let json = br#"[{"name":"A","x":-1600,"y":0,"width":1600,"height":900,
                        "scale":1,"focused":false}]"#;
        assert_eq!(
            hyprland_output(json, Some((-1600, 5))),
            Some(Rect::new(-1600, 0, 1600, 900))
        );
    }

    #[test]
    fn cursor_output_is_parsed_or_rejected_whole() {
        assert_eq!(parse_cursor(b"1666, 502"), Some((1666, 502)));
        assert_eq!(parse_cursor(b" -12,-8\n"), Some((-12, -8)));
        assert_eq!(parse_cursor(b"1666"), None);
        assert_eq!(parse_cursor(b"x, y"), None);
        assert_eq!(parse_cursor(b""), None);
        assert_eq!(parse_cursor(&[0xff, 0xfe]), None);
    }

    #[test]
    fn a_broken_monitor_entry_does_not_hide_the_others() {
        // The first entry is unusable; the second still answers for the
        // pointer, so one odd monitor cannot break the lookup.
        let json = br#"[
            {"name":"BROKEN","x":0,"y":0,"scale":0,"focused":false},
            {"name":"GOOD","x":0,"y":0,"width":800,"height":600,
             "scale":1,"focused":true}
        ]"#;
        assert_eq!(
            hyprland_output(json, Some((10, 10))),
            Some(Rect::new(0, 0, 800, 600))
        );
    }

    #[test]
    fn hyprland_scale_is_applied_and_fractional_scales_survive() {
        let json = br#"[{"x":0,"y":0,"width":2560,"height":1440,"scale":1.25,"focused":true}]"#;
        assert_eq!(
            parse_hyprland_monitors(json),
            Some(Rect::new(0, 0, 2048, 1152))
        );
        // A missing scale means an unscaled output.
        let json = br#"[{"x":0,"y":0,"width":800,"height":600,"focused":true}]"#;
        assert_eq!(
            parse_hyprland_monitors(json),
            Some(Rect::new(0, 0, 800, 600))
        );
        // A nonsensical scale is not worth guessing at.
        let json = br#"[{"x":0,"y":0,"width":800,"height":600,"scale":0,"focused":true}]"#;
        assert_eq!(parse_hyprland_monitors(json), None);
    }

    #[test]
    fn hyprland_monitors_without_focus_pick_nothing() {
        let json = br#"[{"x":0,"y":0,"width":1920,"height":1080,"focused":false}]"#;
        assert_eq!(parse_hyprland_monitors(json), None);
    }

    #[test]
    fn finds_the_focused_sway_output() {
        let json = br#"[
            {"name":"HDMI-A-1","focused":false,"rect":{"x":0,"y":0,"width":1920,"height":1080}},
            {"name":"DP-1","focused":true,"rect":{"x":-1600,"y":0,"width":1600,"height":900}}
        ]"#;
        assert_eq!(
            parse_sway_outputs(json),
            Some(Rect::new(-1600, 0, 1600, 900))
        );
    }

    #[test]
    fn malformed_probe_output_is_not_a_failure() {
        assert_eq!(parse_hyprland_monitors(b"not json"), None);
        assert_eq!(parse_hyprland_monitors(br#"{"x":0}"#), None);
        assert_eq!(parse_sway_outputs(b""), None);
        assert_eq!(parse_sway_outputs(br#"[{"focused":true}]"#), None);
    }

    #[test]
    fn probe_commands_name_the_focused_output_queries() {
        let probes = probes();
        assert_eq!(probes[0].program, "hyprctl");
        assert_eq!(probes[0].args, ["monitors", "-j"]);
        assert_eq!(probes[1].program, "swaymsg");
        assert_eq!(probes[1].args, ["-t", "get_outputs"]);
        assert_eq!(probes[2].program, "niri");
        assert_eq!(probes[2].args, ["msg", "--json", "focused-output"]);
    }

    #[test]
    fn finds_the_focused_niri_output() {
        // `niri msg --json focused-output` for a 4K output at scale 2: `logical`
        // already holds the layout geometry, so its 1920-wide rect is used as
        // it stands rather than being worked out from the 3840x2160 mode.
        let json = br#"{
            "name":"DP-2","make":"Dell","model":"X","serial":null,
            "modes":[{"width":3840,"height":2160,"refresh_rate":60000,
                      "is_preferred":true}],
            "current_mode":0,"is_custom_mode":false,"vrr_supported":false,
            "vrr_enabled":false,
            "logical":{"x":1920,"y":0,"width":1920,"height":1080,"scale":2.0,
                       "transform":"Normal"}
        }"#;
        assert_eq!(
            parse_niri_output(json),
            Some(Rect::new(1920, 0, 1920, 1080))
        );
    }

    #[test]
    fn a_disabled_niri_output_is_not_an_answer() {
        // niri leaves `logical` unset when an output is disabled, and there is
        // no geometry to pin onto in that case.
        let json = br#"{"name":"DP-2","logical":null,"current_mode":null}"#;
        assert_eq!(parse_niri_output(json), None);
        assert_eq!(parse_niri_output(b""), None);
        assert_eq!(parse_niri_output(b"not json"), None);
        // A fractional scale is carried in `logical.scale` and deliberately
        // plays no part in the rect: `logical` is already logical.
        let json = br#"{"logical":{"x":0,"y":0,"width":1280,"height":720,
                                   "scale":1.5,"transform":"Normal"}}"#;
        assert_eq!(parse_niri_output(json), Some(Rect::new(0, 0, 1280, 720)));
    }
}

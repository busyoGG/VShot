//! Which output the user is looking at.
//!
//! A pin belongs on the monitor the user is working on, not on whatever Qt
//! calls the primary one. The pin daemon renders every pin on every output, so
//! it cannot answer this: a windowless Qt process sees the pointer at (0, 0)
//! and has no notion of the focused output. The compositor does know, so the
//! CLI — which runs in the user's session, next to the focused window — asks it
//! and passes the answer along with the pin request.
//!
//! The answer carries the output's *name* wherever the compositor has one,
//! because that is what Qt calls the matching screen, plus the output's global
//! logical rect for a daemon that can only match by geometry.
//!
//! Detection is best-effort and never fatal: a compositor without a probe just
//! leaves the choice to the daemon, which falls back to the output Qt reports
//! as primary. Only the session's own compositor is asked — see [`Session`] —
//! because a second compositor running alongside would answer for itself.

use std::process::Command;

use serde_json::Value;

use crate::geometry::Rect;

/// What a compositor says about the output the user is on: the name Qt knows
/// the output by, the rect it occupies in the global logical layout, or both.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct ActiveOutput {
    pub name: Option<String>,
    pub rect: Option<Rect>,
}

/// One compositor query, and how to read its reply.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Probe {
    Hyprland,
    Sway,
    Niri,
    /// KWin's own "which output is the user on". It is read through a D-Bus
    /// CLI, and the same query appears twice because `gdbus` and `dbus-send`
    /// come from different packages: either may be the only one installed.
    KWinGdbus,
    KWinDbusSend,
}

/// The compositor the session we are running in actually is.
///
/// A probe is only worth asking when its compositor is the one behind this
/// session, because more than one can be running at a time. A Plasma session
/// started from a Hyprland terminal keeps `HYPRLAND_INSTANCE_SIGNATURE` in its
/// environment, `hyprctl` goes on answering for an instance nobody is looking
/// at, and the pin follows *that* session's focus onto the wrong monitor. The
/// desktop variables the session sets say which one we are in.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Session {
    Hyprland,
    Sway,
    Niri,
    KWin,
    /// Nothing in the environment names a compositor, so every probe is worth
    /// trying: a foreign one fails on its own unless it is really there.
    Unknown,
}

impl Session {
    /// The session named by `XDG_CURRENT_DESKTOP` and `XDG_SESSION_DESKTOP`.
    /// Both hold a colon-separated list, and the first name we recognize wins.
    fn from_desktops(current: Option<&str>, session: Option<&str>) -> Self {
        for value in [current, session].into_iter().flatten() {
            for token in value.split(':') {
                let token = token.trim().to_ascii_lowercase();
                match token.as_str() {
                    "hyprland" => return Self::Hyprland,
                    "sway" => return Self::Sway,
                    "niri" => return Self::Niri,
                    "kde" | "plasma" => return Self::KWin,
                    _ => {}
                }
            }
        }
        Self::Unknown
    }

    fn detect() -> Self {
        let current = std::env::var("XDG_CURRENT_DESKTOP").ok();
        let session = std::env::var("XDG_SESSION_DESKTOP").ok();
        Self::from_desktops(current.as_deref(), session.as_deref())
    }
}

/// A probe as a subprocess. Split out from the running of it so the parsing
/// below can be tested without a compositor.
#[derive(Clone, Debug, Eq, PartialEq)]
struct OutputCommand {
    probe: Probe,
    program: &'static str,
    args: &'static [&'static str],
}

impl Probe {
    fn session(self) -> Session {
        match self {
            Probe::Hyprland => Session::Hyprland,
            Probe::Sway => Session::Sway,
            Probe::Niri => Session::Niri,
            Probe::KWinGdbus | Probe::KWinDbusSend => Session::KWin,
        }
    }

    fn command(self) -> OutputCommand {
        let (program, args) = match self {
            Probe::Hyprland => ("hyprctl", &["monitors", "-j"][..]),
            Probe::Sway => ("swaymsg", &["-t", "get_outputs"][..]),
            // niri's IPC answers with the focused output directly. It is only
            // reachable through `$NIRI_SOCKET`, which a session sets for its
            // own clients, so the probe fails fast anywhere else.
            Probe::Niri => ("niri", &["msg", "--json", "focused-output"][..]),
            Probe::KWinGdbus => (
                "gdbus",
                &[
                    "call",
                    "--session",
                    "--dest",
                    "org.kde.KWin",
                    "--object-path",
                    "/KWin",
                    "--method",
                    "org.kde.KWin.activeOutputName",
                ][..],
            ),
            Probe::KWinDbusSend => (
                "dbus-send",
                &[
                    "--session",
                    "--print-reply=literal",
                    "--dest=org.kde.KWin",
                    "/KWin",
                    "org.kde.KWin.activeOutputName",
                ][..],
            ),
        };
        OutputCommand {
            probe: self,
            program,
            args,
        }
    }

    fn parse(self, stdout: &[u8]) -> Option<ActiveOutput> {
        match self {
            Probe::Hyprland => hyprland_output(stdout, cursor_position()),
            Probe::Sway => parse_sway_outputs(stdout),
            Probe::Niri => parse_niri_output(stdout),
            Probe::KWinGdbus | Probe::KWinDbusSend => parse_kwin_output_name(stdout),
        }
    }
}

/// All probes in the order they are tried; the first one that answers wins.
///
/// Hyprland advertises its outputs to Sway's IPC as well, so the Hyprland probe
/// has to be decisive: its "no monitor is focused" answer is a real answer, not
/// a reason to try something else. KWin comes last — `org.kde.KWin` is on the
/// bus only under Plasma, where nothing else answers.
///
/// Only the probes of this session's own compositor are run; see [`Session`].
fn probes() -> Vec<Probe> {
    probes_for(Session::detect())
}

fn probes_for(session: Session) -> Vec<Probe> {
    [
        Probe::Hyprland,
        Probe::Sway,
        Probe::Niri,
        Probe::KWinGdbus,
        Probe::KWinDbusSend,
    ]
    .into_iter()
    .filter(|probe| session == Session::Unknown || probe.session() == session)
    .collect()
}

/// The output the user is working on, if the compositor reports one. The
/// pointer decides when it can be read: a pin belongs where the user is
/// pointing at, and on a click-to-focus setup that is not necessarily the
/// output holding the keyboard focus. Focus is the fallback.
pub fn active_output() -> Option<ActiveOutput> {
    for probe in probes() {
        let command = probe.command();
        let Ok(output) = Command::new(command.program).args(command.args).output() else {
            continue;
        };
        if !output.status.success() {
            continue;
        }
        if let Some(active) = probe.parse(&output.stdout) {
            return Some(active);
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

/// One monitor from `hyprctl monitors -j`.
#[derive(Clone, Debug, Eq, PartialEq)]
struct HyprlandMonitor {
    name: Option<String>,
    focused: bool,
    rect: Rect,
}

impl HyprlandMonitor {
    fn active(&self) -> ActiveOutput {
        ActiveOutput {
            name: self.name.clone(),
            rect: Some(self.rect),
        }
    }
}

/// The monitor under the pointer, else the focused one.
fn hyprland_output(monitors: &[u8], cursor: Option<(i64, i64)>) -> Option<ActiveOutput> {
    let monitors = parse_hyprland_monitors(monitors);
    cursor
        .and_then(|cursor| {
            monitors
                .iter()
                .find(|monitor| contains(monitor.rect, cursor))
        })
        .or_else(|| monitors.iter().find(|monitor| monitor.focused))
        .map(HyprlandMonitor::active)
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

/// Reads every monitor out of `hyprctl monitors -j`.
///
/// The array is ordered by the compositor's monitor id, which the entries also
/// carry, and `focused` marks the one holding the keyboard focus.
///
/// Hyprland reports a monitor's `x`/`y` in the logical layout while `width`
/// and `height` are the native mode size, so the size has to be divided by
/// `scale` before it can be compared with a Qt screen geometry or with a
/// pointer position.
fn parse_hyprland_monitors(bytes: &[u8]) -> Vec<HyprlandMonitor> {
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
            let name = monitor
                .get("name")
                .and_then(Value::as_str)
                .map(str::to_owned);
            let focused = monitor.get("focused").and_then(Value::as_bool) == Some(true);
            let rect = rect(
                x,
                y,
                (width as f64 / scale).round() as u64,
                (height as f64 / scale).round() as u64,
            )?;
            Some(HyprlandMonitor {
                name,
                focused,
                rect,
            })
        })
        .collect()
}

/// Reads the focused output out of `swaymsg -t get_outputs`. Sway exposes the
/// compositor's own hit-testing instead of a rectangle, which is what "the
/// monitor the user is on" really means; the rect only matters to a daemon that
/// matches screens by geometry.
fn parse_sway_outputs(bytes: &[u8]) -> Option<ActiveOutput> {
    let outputs: Vec<Value> = serde_json::from_slice(bytes).ok()?;
    let output = outputs
        .iter()
        .find(|output| output.get("focused").and_then(Value::as_bool) == Some(true))?;
    let name = output
        .get("name")
        .and_then(Value::as_str)
        .map(str::to_owned);
    let geometry = output.get("rect")?;
    let x = geometry.get("x").and_then(Value::as_i64)?;
    let y = geometry.get("y").and_then(Value::as_i64)?;
    let width = geometry.get("width").and_then(Value::as_u64)?;
    let height = geometry.get("height").and_then(Value::as_u64)?;
    Some(ActiveOutput {
        name,
        rect: rect(x, y, width, height),
    })
}

/// Reads the focused output out of `niri msg --json focused-output`.
///
/// niri reports the geometry it uses for layout in `logical`, which is already
/// in the same logical space as a pointer position or a Qt screen geometry, so
/// unlike Hyprland nothing has to be divided by the scale. The scale is
/// deliberately left out of the rect for that reason. A disabled output has no
/// `logical` geometry and is rejected rather than guessed at.
fn parse_niri_output(bytes: &[u8]) -> Option<ActiveOutput> {
    let output: Value = serde_json::from_slice(bytes).ok()?;
    let name = output
        .get("name")
        .and_then(Value::as_str)
        .map(str::to_owned);
    let logical = output.get("logical")?;
    let x = logical.get("x").and_then(Value::as_i64)?;
    let y = logical.get("y").and_then(Value::as_i64)?;
    let width = logical.get("width").and_then(Value::as_u64)?;
    let height = logical.get("height").and_then(Value::as_u64)?;
    Some(ActiveOutput {
        name,
        rect: rect(x, y, width, height),
    })
}

/// Reads the output name out of KWin's `activeOutputName` reply.
///
/// KWin is the only compositor here that answers "which output is the user on"
/// outright, and the name is the better half of the answer anyway: Qt names its
/// screens after the outputs, so the daemon can pick the screen outright instead
/// of comparing geometry. The reply is printed by whichever D-Bus CLI ran the
/// call — `('DP-2',)` from `gdbus`, ` DP-2` from `dbus-send --print-reply=literal`
/// — so the wrapper is stripped rather than one shape being parsed as the
/// format. KWin answers with an empty name when no output qualifies, which is
/// no answer at all rather than an output called "".
fn parse_kwin_output_name(bytes: &[u8]) -> Option<ActiveOutput> {
    let text = std::str::from_utf8(bytes).ok()?;
    let line = text.lines().next()?.trim();
    let line = line.strip_prefix('(').unwrap_or(line);
    let line = line.strip_suffix(')').unwrap_or(line);
    let line = line.strip_suffix(',').unwrap_or(line);
    let name = line
        .trim()
        .trim_matches(|character| character == '\'' || character == '"')
        .trim();
    if name.is_empty() {
        return None;
    }
    Some(ActiveOutput {
        name: Some(name.to_owned()),
        rect: None,
    })
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

    /// The answer for a compositor that names the output it picked.
    fn named(name: &str, rect: Rect) -> ActiveOutput {
        ActiveOutput {
            name: Some(name.to_owned()),
            rect: Some(rect),
        }
    }

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
            hyprland_output(json, None),
            Some(named("DP-2", Rect::new(1920, 0, 1920, 1080)))
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
        // belongs where the user is pointing, and it goes there under the name
        // Qt knows that output by.
        assert_eq!(
            hyprland_output(json, Some((1666, 502))),
            Some(named("DP-3", Rect::new(0, 0, 1920, 1080)))
        );
        // Pointer over the 4K monitor, which starts at logical x = 1920.
        assert_eq!(
            hyprland_output(json, Some((2000, 10))),
            Some(named("DP-2", Rect::new(1920, 0, 1920, 1080)))
        );
        // A pointer the layout does not cover falls back to the focused
        // monitor rather than picking nothing.
        assert_eq!(
            hyprland_output(json, Some((-5000, -5000))),
            Some(named("DP-2", Rect::new(1920, 0, 1920, 1080)))
        );
        // No pointer reading at all: focus decides.
        assert_eq!(
            hyprland_output(json, None),
            Some(named("DP-2", Rect::new(1920, 0, 1920, 1080)))
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
            Some(named("A", Rect::new(0, 0, 1920, 1080)))
        );
        assert_eq!(
            hyprland_output(json, Some((1920, 1079))),
            Some(named("A", Rect::new(0, 0, 1920, 1080)))
        );
        // A negative-origin layout is hit-tested just the same.
        let json = br#"[{"name":"A","x":-1600,"y":0,"width":1600,"height":900,
                        "scale":1,"focused":false}]"#;
        assert_eq!(
            hyprland_output(json, Some((-1600, 5))),
            Some(named("A", Rect::new(-1600, 0, 1600, 900)))
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
            Some(named("GOOD", Rect::new(0, 0, 800, 600)))
        );
    }

    #[test]
    fn hyprland_scale_is_applied_and_fractional_scales_survive() {
        let json = br#"[{"x":0,"y":0,"width":2560,"height":1440,"scale":1.25,"focused":true}]"#;
        assert_eq!(
            hyprland_output(json, None),
            Some(ActiveOutput {
                name: None,
                rect: Some(Rect::new(0, 0, 2048, 1152)),
            })
        );
        // A missing scale means an unscaled output, and a missing name is not
        // fatal: the rect still carries the answer.
        let json = br#"[{"x":0,"y":0,"width":800,"height":600,"focused":true}]"#;
        assert_eq!(
            hyprland_output(json, None),
            Some(ActiveOutput {
                name: None,
                rect: Some(Rect::new(0, 0, 800, 600)),
            })
        );
        // A nonsensical scale is not worth guessing at.
        let json = br#"[{"x":0,"y":0,"width":800,"height":600,"scale":0,"focused":true}]"#;
        assert_eq!(parse_hyprland_monitors(json), Vec::new());
    }

    #[test]
    fn hyprland_monitors_without_focus_pick_nothing() {
        let json = br#"[{"x":0,"y":0,"width":1920,"height":1080,"focused":false}]"#;
        assert_eq!(hyprland_output(json, None), None);
    }

    #[test]
    fn finds_the_focused_sway_output() {
        let json = br#"[
            {"name":"HDMI-A-1","focused":false,"rect":{"x":0,"y":0,"width":1920,"height":1080}},
            {"name":"DP-1","focused":true,"rect":{"x":-1600,"y":0,"width":1600,"height":900}}
        ]"#;
        assert_eq!(
            parse_sway_outputs(json),
            Some(named("DP-1", Rect::new(-1600, 0, 1600, 900)))
        );
    }

    #[test]
    fn malformed_probe_output_is_not_a_failure() {
        assert_eq!(parse_hyprland_monitors(b"not json"), Vec::new());
        assert_eq!(parse_hyprland_monitors(br#"{"x":0}"#), Vec::new());
        assert_eq!(parse_sway_outputs(b""), None);
        assert_eq!(parse_sway_outputs(br#"[{"focused":true}]"#), None);
    }

    #[test]
    fn probe_commands_name_the_focused_output_queries() {
        let commands = probes_for(Session::Unknown)
            .into_iter()
            .map(Probe::command)
            .collect::<Vec<_>>();
        assert_eq!(commands[0].program, "hyprctl");
        assert_eq!(commands[0].args, ["monitors", "-j"]);
        assert_eq!(commands[1].program, "swaymsg");
        assert_eq!(commands[1].args, ["-t", "get_outputs"]);
        assert_eq!(commands[2].program, "niri");
        assert_eq!(commands[2].args, ["msg", "--json", "focused-output"]);
        // KWin answers "which output is the user on" itself; both D-Bus CLIs
        // ask the same question so that either package alone is enough.
        assert_eq!(commands[3].program, "gdbus");
        assert_eq!(
            commands[3].args.last(),
            Some(&"org.kde.KWin.activeOutputName")
        );
        assert_eq!(commands[4].program, "dbus-send");
        assert_eq!(
            commands[4].args.last(),
            Some(&"org.kde.KWin.activeOutputName")
        );
    }

    #[test]
    fn the_desktop_variables_name_the_session_compositor() {
        let detect = Session::from_desktops;
        assert_eq!(detect(Some("KDE"), Some("KDE")), Session::KWin);
        assert_eq!(
            detect(Some("Hyprland"), Some("Hyprland")),
            Session::Hyprland
        );
        assert_eq!(detect(Some("sway"), None), Session::Sway);
        assert_eq!(detect(None, Some("niri")), Session::Niri);
        // The value is a colon-separated list and the case is not fixed.
        assert_eq!(detect(Some("KDE:GNOME"), None), Session::KWin);
        assert_eq!(detect(Some("kde"), None), Session::KWin);
        assert_eq!(detect(Some("PLASMA"), None), Session::KWin);
        // The first variable that names a compositor decides, so a stale
        // XDG_SESSION_DESKTOP cannot override XDG_CURRENT_DESKTOP.
        assert_eq!(detect(Some("KDE"), Some("Hyprland")), Session::KWin);
        // A session we know nothing about, or know only as something without a
        // probe, tries everything.
        assert_eq!(detect(Some("GNOME"), Some("gnome")), Session::Unknown);
        assert_eq!(detect(Some(""), Some("")), Session::Unknown);
        assert_eq!(detect(None, None), Session::Unknown);
    }

    #[test]
    fn only_the_sessions_own_compositor_is_asked() {
        // A Plasma session that inherited Hyprland's variables must not ask
        // hyprctl: the answer would be the focused monitor of a compositor
        // nobody is looking at.
        assert_eq!(
            probes_for(Session::KWin),
            vec![Probe::KWinGdbus, Probe::KWinDbusSend]
        );
        assert_eq!(probes_for(Session::Hyprland), vec![Probe::Hyprland]);
        assert_eq!(probes_for(Session::Sway), vec![Probe::Sway]);
        assert_eq!(probes_for(Session::Niri), vec![Probe::Niri]);
        assert_eq!(
            probes_for(Session::Unknown),
            vec![
                Probe::Hyprland,
                Probe::Sway,
                Probe::Niri,
                Probe::KWinGdbus,
                Probe::KWinDbusSend,
            ]
        );
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
            Some(named("DP-2", Rect::new(1920, 0, 1920, 1080)))
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
        let json = br#"{"name":"A","logical":{"x":0,"y":0,"width":1280,"height":720,
                                   "scale":1.5,"transform":"Normal"}}"#;
        assert_eq!(
            parse_niri_output(json),
            Some(named("A", Rect::new(0, 0, 1280, 720)))
        );
    }

    #[test]
    fn kwin_names_the_active_output() {
        // `gdbus call ... activeOutputName` wraps its reply in a tuple.
        assert_eq!(
            parse_kwin_output_name(b"('DP-2',)\n"),
            Some(ActiveOutput {
                name: Some("DP-2".into()),
                rect: None,
            })
        );
        // `dbus-send --print-reply=literal` prints the bare string, indented,
        // and without a trailing newline.
        assert_eq!(
            parse_kwin_output_name(b"   DP-2"),
            Some(ActiveOutput {
                name: Some("DP-2".into()),
                rect: None,
            })
        );
        // A double-quoted reply is just as readable.
        assert_eq!(
            parse_kwin_output_name(b"(\"HDMI-A-1\",)\n"),
            Some(ActiveOutput {
                name: Some("HDMI-A-1".into()),
                rect: None,
            })
        );
    }

    #[test]
    fn an_empty_kwin_name_is_not_an_answer() {
        // KWin reports an empty name when no output qualifies, and a name that
        // matches no screen would keep the daemon on the primary output — so it
        // has to fall through to the next probe instead.
        assert_eq!(parse_kwin_output_name(b"('',)\n"), None);
        assert_eq!(parse_kwin_output_name(b"   "), None);
        assert_eq!(parse_kwin_output_name(b"\n"), None);
        assert_eq!(parse_kwin_output_name(b""), None);
        assert_eq!(parse_kwin_output_name(&[0xff, 0xfe]), None);
    }

    /// Runs the probes against the session this test is started in, which is
    /// the only way to catch a query that is spelled wrong — a `--method` that
    /// does not exist looks exactly like a compositor with nothing to say, and
    /// the pin then lands on the primary output without any error.
    ///
    /// Ignored by default because it needs a live session: run it with
    /// `cargo test -- --ignored --nocapture a_live_session` inside the session
    /// to see what the probes answer there. Starting it with the session's
    /// variables removed (`env -u HYPRLAND_INSTANCE_SIGNATURE ...`) probes the
    /// fallbacks the way another compositor would see them.
    #[test]
    #[ignore = "needs a live session: Hyprland, Sway, niri or KWin"]
    fn a_live_session_reports_the_active_output() {
        let active = active_output().expect("some probe has to answer in a live session");
        eprintln!("active output: {active:?}");
        assert!(
            active.name.is_some() || active.rect.is_some(),
            "an answer has to carry a name, a rect, or both"
        );
        // A name is matched against Qt's screen names, so it has to be the
        // output's own name rather than a line of debug output.
        if let Some(name) = &active.name {
            assert!(!name.is_empty(), "an empty name is no name");
            assert!(
                !name.contains(char::is_whitespace),
                "`{name}` is not an output name"
            );
        }
    }
}

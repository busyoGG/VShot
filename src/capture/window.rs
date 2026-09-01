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
}

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

    Err(VshotError::ActiveWindowUnavailable(
        "neither a valid Hyprland `hyprctl activewindow -j` result nor a focused Sway tree node was available".into(),
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
    fn rejects_unreliable_window_data() {
        assert!(parse_hyprland_active_window(br#"{"at":[0,0],"size":[0,1]}"#).is_err());
        assert!(parse_sway_active_window(br#"{"nodes":[]}"#).is_err());
    }
}

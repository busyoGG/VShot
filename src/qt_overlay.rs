use std::fs::OpenOptions;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use serde::{Deserialize, Serialize};
use tempfile::TempDir;

use crate::edit::{ArrowStyle, LineDash, ShapeMask, TextBitmap, DEFAULT_MOSAIC_STRENGTH};
use crate::error::{Result, VshotError};
use crate::geometry::{Point, Rect};
use crate::model::SceneSnapshot;
use crate::wayland::input::{
    Annotation, EditorTool, DEFAULT_ANNOTATION_COLOR, DEFAULT_ANNOTATION_WIDTH, DEFAULT_TEXT_COLOR,
};

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

fn run_helper(helper: &HelperLookup, session_path: &Path) -> Result<Vec<u8>> {
    let child = Command::new(&helper.path)
        .arg("--session")
        .arg(session_path)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| {
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
        })?;

    let output = child.wait_with_output().map_err(|error| {
        VshotError::Selection(format!("failed to collect Qt helper output: {error}"))
    })?;
    if !output.status.success() {
        let detail = compact_error(&output.stderr);
        return Err(VshotError::Selection(if detail.is_empty() {
            format!("Qt helper exited with {}", output.status)
        } else {
            format!("Qt helper exited with {}: {detail}", output.status)
        }));
    }
    Ok(output.stdout)
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
    outputs: Vec<QtOutput<'a>>,
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
    scale: u32,
    pixel_width: u32,
    pixel_height: u32,
    path: String,
}

#[derive(Debug, Deserialize)]
struct QtResult {
    status: String,
    selection: Option<WireRect>,
    annotations: Option<Vec<QtAnnotation>>,
}

#[derive(Debug, Deserialize)]
struct QtAnnotation {
    kind: String,
    tool: Option<String>,
    rect: Option<WireRect>,
    points: Option<Vec<WirePoint>>,
    origin: Option<WirePoint>,
    text: Option<String>,
    scale: Option<u32>,
    color: Option<String>,
    width: Option<u32>,
    dash: Option<String>,
    size: Option<u32>,
    mask: Option<String>,
    arrow_style: Option<String>,
    strength: Option<u32>,
    // Font family used by the helper to rasterize the text label; empty or
    // missing means the helper's application default font.
    font: Option<String>,
    bitmap_width: Option<u32>,
    bitmap_height: Option<u32>,
    // Path to a raw RGBA8888 file next to the session JSON, mirroring how
    // the frozen output frames are handed over.
    bitmap: Option<String>,
}

pub fn select_and_edit(scene: &SceneSnapshot) -> Result<(Rect, Vec<Annotation>)> {
    let (_directory, session_path) = write_session(scene)?;
    let helper = helper_program()?;
    let output = run_helper(&helper, &session_path)?;
    parse_result(output, scene.bounds())
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
    /// Device pixels per logical pixel between `frame` and `window`: 1 for
    /// plain pins, the output's density for HiDPI-rendered text cards.
    pub(crate) scale: u32,
    /// Daemon socket the editor uses to move the pin live.
    pub(crate) socket: &'a Path,
    /// Id of this pin inside the daemon, echoed back in session JSON.
    pub(crate) pin_id: u64,
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
    let raw_path = directory.path().join("pin.rgba");
    write_private_file(&raw_path, spec.frame.pixels())?;

    let session = QtSession {
        version: 1,
        mode: "pin-edit",
        bounds: spec.window.into(),
        window: Some(spec.window.into()),
        socket: Some(spec.socket.to_string_lossy().into_owned()),
        id: Some(spec.pin_id),
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
            scale: spec.scale,
            pixel_width: spec.frame.size().width,
            pixel_height: spec.frame.size().height,
            path: raw_path.to_string_lossy().into_owned(),
        }],
    };
    let session_path = directory.path().join("session.json");
    let encoded = serde_json::to_vec(&session).map_err(|error| {
        VshotError::Pin(format!("failed to encode pin-edit session JSON: {error}"))
    })?;
    write_private_file(&session_path, &encoded)?;
    Ok((directory, session_path))
}

/// Runs the Qt helper against a session file and collects its stdout result.
pub(crate) fn run_session(session_path: &Path) -> Result<Vec<u8>> {
    let helper = helper_program()?;
    let child = Command::new(&helper.path)
        .arg("--pin-edit")
        .arg(session_path)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| {
            if error.kind() == std::io::ErrorKind::NotFound {
                VshotError::Pin(format!(
                    "Qt helper `{}` was not found; build it with `cmake -S . -B build-qt && \
                     cmake --build build-qt` or point VSHOT_QT_HELPER at the executable",
                    helper.path.display()
                ))
            } else {
                VshotError::Pin(format!(
                    "failed to start Qt helper `{}`: {error}",
                    helper.path.display()
                ))
            }
        })?;
    let output = child
        .wait_with_output()
        .map_err(|error| VshotError::Pin(format!("failed to collect Qt helper output: {error}")))?;
    if !output.status.success() {
        let detail = compact_error(&output.stderr);
        return Err(VshotError::Pin(if detail.is_empty() {
            format!("Qt helper exited with {}", output.status)
        } else {
            format!("Qt helper exited with {}: {detail}", output.status)
        }));
    }
    Ok(output.stdout)
}

/// Parses a pin-edit helper result: the selection (the pin image, where the
/// user left it) and the annotations drawn on it. The status `cancelled` maps
/// to `Ok(None)`.
pub(crate) fn parse_edit_result(
    bytes: Vec<u8>,
    window: Rect,
) -> Result<Option<(Rect, Vec<Annotation>)>> {
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
            let annotations = result
                .annotations
                .unwrap_or_default()
                .into_iter()
                .map(parse_annotation)
                .collect::<Result<Vec<_>>>()?;
            Ok(Some((selection, annotations)))
        }
        status => Err(VshotError::Pin(format!(
            "Qt helper returned unknown status `{status}`"
        ))),
    }
}

fn write_session(scene: &SceneSnapshot) -> Result<(TempDir, PathBuf)> {
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
        let raw_path = directory
            .path()
            .join(format!("output-{}.rgba", output.global_id));
        write_private_file(&raw_path, output.frame.pixels())?;
        outputs.push(QtOutput {
            id: output.global_id,
            name: &output.name,
            x: output.geometry.left(),
            y: output.geometry.top(),
            width: output.geometry.size.width,
            height: output.geometry.size.height,
            surface: output.geometry.into(),
            scale: output.scale,
            pixel_width: output.frame.size().width,
            pixel_height: output.frame.size().height,
            path: raw_path.to_string_lossy().into_owned(),
        });
    }

    let session = QtSession {
        version: 1,
        mode: "region",
        bounds: scene.bounds().into(),
        window: None,
        socket: None,
        id: None,
        outputs,
    };
    let session_path = directory.path().join("session.json");
    let encoded = serde_json::to_vec(&session).map_err(|error| {
        VshotError::Selection(format!("failed to encode Qt session JSON: {error}"))
    })?;
    write_private_file(&session_path, &encoded)?;
    Ok((directory, session_path))
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

fn parse_result(bytes: Vec<u8>, bounds: Rect) -> Result<(Rect, Vec<Annotation>)> {
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
            let annotations = result
                .annotations
                .unwrap_or_default()
                .into_iter()
                .map(parse_annotation)
                .collect::<Result<Vec<_>>>()?;
            Ok((selection, annotations))
        }
        status => Err(VshotError::Selection(format!(
            "Qt helper returned unknown status `{status}`"
        ))),
    }
}

fn parse_annotation(annotation: QtAnnotation) -> Result<Annotation> {
    let tool_name = annotation.tool.as_deref().unwrap_or_default();
    match annotation.kind.as_str() {
        "shape" => {
            let tool = parse_shape_tool(tool_name)?;
            let rect = annotation
                .rect
                .ok_or_else(|| VshotError::Selection("shape annotation has no rect".into()))?
                .into_rect("shape annotation rect")?;
            Ok(Annotation::Shape {
                tool,
                rect,
                color: parse_color(annotation.color.as_deref(), DEFAULT_ANNOTATION_COLOR)?,
                width: parse_width(annotation.width)?,
                dash: parse_dash(annotation.dash.as_deref())?,
                mask: parse_mask(annotation.mask.as_deref())?,
                strength: parse_strength(annotation.strength)?,
            })
        }
        "stroke" => {
            let tool = parse_stroke_tool(tool_name)?;
            let points = annotation
                .points
                .ok_or_else(|| VshotError::Selection("stroke annotation has no points".into()))?
                .into_iter()
                .map(parse_point)
                .collect::<Result<Vec<_>>>()?;
            if points.is_empty() {
                return Err(VshotError::Selection(
                    "stroke annotation must contain points".into(),
                ));
            }
            Ok(Annotation::Stroke {
                tool,
                points,
                color: parse_color(annotation.color.as_deref(), DEFAULT_ANNOTATION_COLOR)?,
                width: parse_width(annotation.width)?,
                dash: parse_dash(annotation.dash.as_deref())?,
                head: parse_head(annotation.size)?,
                arrow_style: parse_arrow_style(annotation.arrow_style.as_deref())?,
                strength: parse_strength(annotation.strength)?,
            })
        }
        "text" => {
            let origin = annotation
                .origin
                .ok_or_else(|| VshotError::Selection("text annotation has no origin".into()))?;
            let text = annotation
                .text
                .ok_or_else(|| VshotError::Selection("text annotation has no text".into()))?;
            let scale = parse_text_scale(annotation.scale)?;
            let bitmap = match (
                annotation.bitmap_width,
                annotation.bitmap_height,
                annotation.bitmap.as_deref(),
            ) {
                (None, None, None) => None,
                (Some(width), Some(height), Some(path)) => {
                    Some(read_text_bitmap(path, width, height)?)
                }
                _ => {
                    return Err(VshotError::Selection(
                        "text bitmap fields must be provided together".into(),
                    ))
                }
            };
            Ok(Annotation::Text {
                origin: parse_point(origin)?,
                text,
                scale,
                color: parse_color(annotation.color.as_deref(), DEFAULT_TEXT_COLOR)?,
                font: annotation.font.unwrap_or_default(),
                bitmap,
            })
        }
        kind => Err(VshotError::Selection(format!(
            "Qt helper returned unknown annotation kind `{kind}`"
        ))),
    }
}

/// Parses a `#RRGGBB` helper color into RGBA8 with full alpha.
fn parse_color(value: Option<&str>, default: [u8; 4]) -> Result<[u8; 4]> {
    let Some(hex) = value else {
        return Ok(default);
    };
    let hex = hex.strip_prefix('#').unwrap_or(hex);
    if hex.len() != 6 || !hex.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return Err(VshotError::Selection(format!(
            "Qt annotation color `{value:?}` is not a #RRGGBB value"
        )));
    }
    let channel = |range: std::ops::Range<usize>| {
        u8::from_str_radix(&hex[range], 16)
            .map_err(|_| VshotError::Selection("Qt annotation color is invalid".into()))
    };
    Ok([channel(0..2)?, channel(2..4)?, channel(4..6)?, 255])
}

/// Clamps a helper stroke width into a sane logical-pixel range.
fn parse_width(value: Option<u32>) -> Result<u32> {
    Ok(value.unwrap_or(DEFAULT_ANNOTATION_WIDTH).clamp(1, 64))
}

/// Parses the optional line style; missing fields mean a solid stroke.
fn parse_dash(value: Option<&str>) -> Result<LineDash> {
    let Some(value) = value else {
        return Ok(LineDash::Solid);
    };
    LineDash::parse(value).ok_or_else(|| {
        VshotError::Selection(format!(
            "Qt annotation dash `{value}` is not a known line style"
        ))
    })
}

/// Clamps the optional arrow head size multiplier.
fn parse_head(value: Option<u32>) -> Result<u32> {
    Ok(value.unwrap_or(1).clamp(1, 8))
}

/// Clamps the optional text scale to the helper's supported 1..=64 range.
fn parse_text_scale(value: Option<u32>) -> Result<u32> {
    Ok(value.unwrap_or(2).clamp(1, 64))
}

/// Loads a helper-rendered text bitmap and validates its pixel payload.
fn read_text_bitmap(path: &str, width: u32, height: u32) -> Result<TextBitmap> {
    const MAX_TEXT_BITMAP_PIXELS: u64 = 16 * 1024 * 1024;
    if width == 0 || height == 0 {
        return Err(VshotError::Selection(
            "text bitmap dimensions must be greater than zero".into(),
        ));
    }
    if u64::from(width) * u64::from(height) > MAX_TEXT_BITMAP_PIXELS {
        return Err(VshotError::Selection("text bitmap is too large".into()));
    }
    let expected = (width as usize)
        .checked_mul(height as usize)
        .and_then(|pixels| pixels.checked_mul(4))
        .ok_or_else(|| VshotError::Selection("text bitmap is too large".into()))?;
    let pixels = std::fs::read(path).map_err(|error| {
        VshotError::Selection(format!("failed to read text bitmap `{path}`: {error}"))
    })?;
    if pixels.len() != expected {
        return Err(VshotError::Selection(format!(
            "text bitmap `{path}` has {} bytes but {}x{} RGBA needs {expected}",
            pixels.len(),
            width,
            height
        )));
    }
    Ok(TextBitmap {
        width,
        height,
        pixels,
    })
}

/// Parses the optional arrow head shape; missing fields mean an open head.
fn parse_arrow_style(value: Option<&str>) -> Result<ArrowStyle> {
    let Some(value) = value else {
        return Ok(ArrowStyle::Open);
    };
    ArrowStyle::parse(value).ok_or_else(|| {
        VshotError::Selection(format!(
            "Qt annotation arrow_style `{value}` is not a known arrow style"
        ))
    })
}

/// Clamps the optional mosaic strength level; missing fields mean standard.
fn parse_strength(value: Option<u32>) -> Result<u32> {
    Ok(value.unwrap_or(DEFAULT_MOSAIC_STRENGTH).clamp(1, 3))
}

/// Parses the optional mosaic area shape; missing fields mean a rectangle.
fn parse_mask(value: Option<&str>) -> Result<ShapeMask> {
    let Some(value) = value else {
        return Ok(ShapeMask::Rect);
    };
    ShapeMask::parse(value).ok_or_else(|| {
        VshotError::Selection(format!(
            "Qt annotation mask `{value}` is not a known area shape"
        ))
    })
}

fn parse_point(point: WirePoint) -> Result<Point> {
    Ok(Point::new(
        i32::try_from(point.x)
            .map_err(|_| VshotError::Selection("annotation x is out of range".into()))?,
        i32::try_from(point.y)
            .map_err(|_| VshotError::Selection("annotation y is out of range".into()))?,
    ))
}

fn parse_shape_tool(name: &str) -> Result<EditorTool> {
    match name {
        "rectangle" => Ok(EditorTool::Rectangle),
        "ellipse" => Ok(EditorTool::Ellipse),
        "mosaic" => Ok(EditorTool::Mosaic),
        _ => Err(VshotError::Selection(format!(
            "unknown shape annotation tool `{name}`"
        ))),
    }
}

fn parse_stroke_tool(name: &str) -> Result<EditorTool> {
    match name {
        "arrow" => Ok(EditorTool::Arrow),
        "pen" => Ok(EditorTool::Pen),
        "draw" => Ok(EditorTool::Draw),
        "line" => Ok(EditorTool::Line),
        "mosaic" => Ok(EditorTool::Mosaic),
        "blur" => Ok(EditorTool::Blur),
        _ => Err(VshotError::Selection(format!(
            "unknown stroke annotation tool `{name}`"
        ))),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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

    #[test]
    fn parses_qt_result_with_annotations() {
        let bytes = br#"{"status":"ok","selection":{"x":-2,"y":3,"width":10,"height":8},"annotations":[{"kind":"shape","tool":"rectangle","rect":{"x":0,"y":1,"width":3,"height":4}},{"kind":"stroke","tool":"arrow","points":[{"x":0,"y":0},{"x":5,"y":6}]},{"kind":"text","origin":{"x":1,"y":2},"text":"A","scale":2}]}"#.to_vec();
        let (selection, annotations) = parse_result(bytes, Rect::new(-10, -10, 100, 100)).unwrap();
        assert_eq!(selection, Rect::new(-2, 3, 10, 8));
        assert_eq!(annotations.len(), 3);
        assert_eq!(annotations[0].tool(), EditorTool::Rectangle);
        assert_eq!(annotations[0].color(), DEFAULT_ANNOTATION_COLOR);
        assert_eq!(annotations[0].width(), DEFAULT_ANNOTATION_WIDTH);
        assert_eq!(annotations[1].tool(), EditorTool::Arrow);
        assert_eq!(annotations[1].arrow_style(), ArrowStyle::Open);
        assert_eq!(
            annotations[2].text_content().map(|value| value.0),
            Some("A")
        );
        assert_eq!(annotations[2].color(), DEFAULT_TEXT_COLOR);
    }

    #[test]
    fn parses_annotation_styles_from_wire() {
        let bytes = br##"{"status":"ok","selection":{"x":0,"y":0,"width":50,"height":50},"annotations":[{"kind":"stroke","tool":"pen","color":"#00FF88","width":4,"dash":"dotted","points":[{"x":0,"y":0},{"x":9,"y":9}]},{"kind":"stroke","tool":"arrow","arrow_style":"filled","points":[{"x":0,"y":0},{"x":9,"y":9}]},{"kind":"text","origin":{"x":1,"y":2},"text":"A","scale":3,"color":"#112233"}]}"##.to_vec();
        let (selection, annotations) = parse_result(bytes, Rect::new(0, 0, 100, 100)).unwrap();
        assert_eq!(selection, Rect::new(0, 0, 50, 50));
        assert_eq!(annotations[0].color(), [0, 255, 136, 255]);
        assert_eq!(annotations[0].width(), 4);
        assert_eq!(annotations[0].dash(), LineDash::Dotted);
        assert_eq!(annotations[1].arrow_style(), ArrowStyle::Filled);
        assert_eq!(annotations[2].color(), [17, 34, 51, 255]);
        assert!(matches!(
            parse_annotation(
                serde_json::from_value(serde_json::json!({
                    "kind": "stroke", "tool": "pen", "color": "nothex",
                    "points": [{"x": 0, "y": 0}]
                }))
                .unwrap()
            ),
            Err(VshotError::Selection(_))
        ));
        assert!(matches!(
            parse_annotation(
                serde_json::from_value(serde_json::json!({
                    "kind": "stroke", "tool": "pen",
                    "dash": "zigzag",
                    "points": [{"x": 0, "y": 0}]
                }))
                .unwrap()
            ),
            Err(VshotError::Selection(_))
        ));
    }

    #[test]
    fn parses_shape_styles_and_mosaic_masks_from_wire() {
        let bytes = br##"{"status":"ok","selection":{"x":0,"y":0,"width":50,"height":50},"annotations":[{"kind":"shape","tool":"rectangle","rect":{"x":0,"y":0,"width":8,"height":6},"dash":"dashed"},{"kind":"stroke","tool":"arrow","points":[{"x":0,"y":0},{"x":5,"y":6}],"size":3},{"kind":"shape","tool":"mosaic","rect":{"x":0,"y":0,"width":8,"height":6},"mask":"ellipse","strength":1}]}"##.to_vec();
        let (_, annotations) = parse_result(bytes, Rect::new(0, 0, 100, 100)).unwrap();
        assert_eq!(annotations.len(), 3);
        assert_eq!(annotations[0].dash(), LineDash::Dashed);
        assert_eq!(annotations[0].head(), 1);
        assert_eq!(annotations[0].strength(), DEFAULT_MOSAIC_STRENGTH);
        assert_eq!(annotations[1].head(), 3);
        assert_eq!(annotations[1].dash(), LineDash::Solid);
        assert_eq!(annotations[2].tool(), EditorTool::Mosaic);
        assert_eq!(annotations[2].mask(), ShapeMask::Ellipse);
        assert_eq!(annotations[2].strength(), 1);
        assert!(matches!(
            parse_annotation(
                serde_json::from_value(serde_json::json!({
                    "kind": "shape", "tool": "mosaic",
                    "rect": {"x": 0, "y": 0, "width": 2, "height": 2},
                    "mask": "triangle"
                }))
                .unwrap()
            ),
            Err(VshotError::Selection(_))
        ));
    }

    #[test]
    fn parses_arrow_style_and_clamps_text_scale() {
        let bytes = br#"{"status":"ok","selection":{"x":0,"y":0,"width":50,"height":50},"annotations":[{"kind":"stroke","tool":"arrow","arrow_style":"filled","points":[{"x":0,"y":0},{"x":5,"y":5}]},{"kind":"text","origin":{"x":1,"y":2},"text":"A","scale":99},{"kind":"text","origin":{"x":3,"y":4},"text":"B","scale":0}]}"#.to_vec();
        let (_, annotations) = parse_result(bytes, Rect::new(0, 0, 100, 100)).unwrap();
        assert_eq!(annotations[0].arrow_style(), ArrowStyle::Filled);
        assert_eq!(annotations[1].text_content().map(|value| value.2), Some(64));
        assert_eq!(annotations[2].text_content().map(|value| value.2), Some(1));
        assert_eq!(parse_text_scale(None).unwrap(), 2);
        assert!(parse_arrow_style(Some("double")).is_err());
    }

    #[test]
    fn parses_text_font_from_wire() {
        let bytes = serde_json::json!({
            "status": "ok",
            "selection": {"x": 0, "y": 0, "width": 50, "height": 50},
            "annotations": [{
                "kind": "text",
                "origin": {"x": 1, "y": 2},
                "text": "Hi",
                "scale": 3,
                "color": "#ffffff",
                "font": "Noto Sans"
            }]
        })
        .to_string()
        .into_bytes();
        let (_, annotations) = parse_result(bytes, Rect::new(0, 0, 100, 100)).unwrap();
        assert_eq!(
            annotations[0].text_content().map(|value| value.0),
            Some("Hi")
        );
        match &annotations[0] {
            Annotation::Text { font, .. } => assert_eq!(font, "Noto Sans"),
            other => panic!("expected text annotation, got {other:?}"),
        }
        // Missing font keeps the empty default.
        let legacy = br#"{"status":"ok","selection":{"x":0,"y":0,"width":50,"height":50},"annotations":[{"kind":"text","origin":{"x":1,"y":2},"text":"A","scale":2}]}"#;
        let (_, annotations) = parse_result(legacy.to_vec(), Rect::new(0, 0, 100, 100)).unwrap();
        match &annotations[0] {
            Annotation::Text { font, .. } => assert!(font.is_empty()),
            other => panic!("expected text annotation, got {other:?}"),
        }
    }

    #[test]
    fn parses_text_bitmap_from_session_file() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("text-0.rgba");
        std::fs::write(&path, [10u8, 20, 30, 255, 0, 0, 0, 0]).unwrap();
        let bytes = format!(
            r##"{{"status":"ok","selection":{{"x":0,"y":0,"width":50,"height":50}},"annotations":[{{"kind":"text","origin":{{"x":1,"y":2}},"text":"Hi","scale":3,"color":"#ffffff","bitmap_width":2,"bitmap_height":1,"bitmap":"{}"}}]}}"##,
            path.display()
        )
        .into_bytes();
        let (_, annotations) = parse_result(bytes, Rect::new(0, 0, 100, 100)).unwrap();
        let bitmap = annotations[0].text_bitmap().unwrap();
        assert_eq!((bitmap.width, bitmap.height), (2, 1));
        assert_eq!(bitmap.pixels, vec![10, 20, 30, 255, 0, 0, 0, 0]);
        // Legacy helpers without bitmaps still parse into the fallback path.
        let (_, legacy) = parse_result(
            br#"{"status":"ok","selection":{"x":0,"y":0,"width":50,"height":50},"annotations":[{"kind":"text","origin":{"x":1,"y":2},"text":"A","scale":2}]}"#
                .to_vec(),
            Rect::new(0, 0, 100, 100),
        )
        .unwrap();
        assert!(legacy[0].text_bitmap().is_none());
        // Payload/dimension mismatches and partial fields are rejected.
        assert!(read_text_bitmap(path.to_str().unwrap(), 3, 1).is_err());
        assert!(
            parse_result(
                br#"{"status":"ok","selection":{"x":0,"y":0,"width":50,"height":50},"annotations":[{"kind":"text","origin":{"x":1,"y":2},"text":"A","scale":2,"bitmap_width":2}]}"#
                    .to_vec(),
                Rect::new(0, 0, 100, 100),
            )
            .is_err()
        );
    }

    #[test]
    fn rejects_cancelled_and_invalid_selection() {
        assert!(matches!(
            parse_result(
                br#"{"status":"cancelled"}"#.to_vec(),
                Rect::new(0, 0, 20, 20)
            ),
            Err(VshotError::SelectionCancelled)
        ));
        assert!(parse_result(
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
}

use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};

use crate::error::{Result, VshotError};
use crate::qt_overlay::helper_program;

/// Budget for the daemon to come up after we start it ourselves.
const DAEMON_STARTUP: Duration = Duration::from_secs(3);
/// Budget for a single request/response round trip with a live daemon.
const IO_TIMEOUT: Duration = Duration::from_secs(5);

/// One request to the resident pin daemon; a single JSON object per
/// connection, matching the `--pin-server` protocol in `ui/pin_server.cpp`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[serde(tag = "cmd", rename_all = "lowercase")]
pub(crate) enum PinCommand {
    Add {
        path: PathBuf,
        /// Device pixels per logical pixel of the image, when it came from a
        /// capture: the scale of the output it was taken on. A pin shows the
        /// image at the logical size it had there. Bare files and clipboard
        /// images leave this out and the daemon works it out.
        #[serde(skip_serializing_if = "Option::is_none")]
        density: Option<u32>,
        /// Name of the output the user is on, when the compositor reports one.
        /// Qt names its screens after the outputs, so the daemon matches this
        /// against a screen directly; the rect below is what a compositor that
        /// names nothing has to offer instead. The daemon cannot work either out
        /// for itself: a windowless process sees the pointer at (0, 0).
        #[serde(skip_serializing_if = "Option::is_none")]
        output_name: Option<String>,
        /// Global logical rect of the output the user is on, when the
        /// compositor reports one.
        #[serde(skip_serializing_if = "Option::is_none")]
        output: Option<WireOutputRect>,
    },
    #[serde(rename = "add-clipboard")]
    AddClipboard {
        /// `vshot region --pin` styles the clipboard the same way: the capture
        /// brings its source density, everything else lets the daemon decide.
        #[serde(skip_serializing_if = "Option::is_none")]
        density: Option<u32>,
        #[serde(skip_serializing_if = "Option::is_none")]
        output_name: Option<String>,
        #[serde(skip_serializing_if = "Option::is_none")]
        output: Option<WireOutputRect>,
    },
    /// Repositions a pin, optionally replacing its pixels in the same round
    /// trip (pin editing writes the annotated image back where the user put it).
    Move {
        id: u64,
        x: i32,
        y: i32,
        #[serde(skip_serializing_if = "Option::is_none")]
        path: Option<PathBuf>,
    },
    Toggle,
    Show,
    Hide,
    Close,
    Quit,
    List,
}

/// An output's global logical rect on the wire.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
pub(crate) struct WireOutputRect {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

impl From<crate::geometry::Rect> for WireOutputRect {
    fn from(rect: crate::geometry::Rect) -> Self {
        Self {
            x: rect.origin.x,
            y: rect.origin.y,
            width: rect.size.width,
            height: rect.size.height,
        }
    }
}

#[derive(Clone, Debug, Deserialize)]
pub(crate) struct PinReply {
    pub ok: bool,
    #[serde(default)]
    pub error: Option<String>,
    #[serde(default)]
    pub count: Option<u64>,
    #[serde(default)]
    pub visible: Option<bool>,
}

impl PinReply {
    fn into_result(self) -> Result<Self> {
        if self.ok {
            return Ok(self);
        }
        Err(VshotError::Pin(format!(
            "pin daemon rejected the request: {}",
            self.error.as_deref().unwrap_or("unknown error")
        )))
    }
}

/// A parsed `vshot pin ...` request: pin these files and/or the clipboard
/// image, or run this one control command. Exactly one control flag is
/// allowed, and never alongside files or `--clipboard`.
#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) struct PinInvocation {
    pub files: Vec<PathBuf>,
    pub clipboard: bool,
    pub command: Option<PinCommand>,
    /// Device density to stamp on the pinned images, overriding what the
    /// daemon would work out for itself. `None` leaves it to the daemon.
    pub density: Option<u32>,
}

/// Environment fallback for `--density`, so a screenshot hotkey can hand the
/// source output's scale to every invocation without repeating the flag.
const DENSITY_ENV: &str = "VSHOT_PIN_DENSITY";

fn density_from_env() -> Result<Option<u32>> {
    let Some(raw) = std::env::var_os(DENSITY_ENV) else {
        return Ok(None);
    };
    let raw = raw.to_string_lossy().trim().to_owned();
    if raw.is_empty() {
        return Ok(None);
    }
    match raw.parse::<u32>() {
        Ok(value) if (1..=4).contains(&value) => Ok(Some(value)),
        _ => Err(VshotError::InvalidDestination(format!(
            "{DENSITY_ENV} must be a device density between 1 and 4, got `{raw}`"
        ))),
    }
}

impl PinInvocation {
    #[allow(clippy::too_many_arguments)] // one bool per control flag, all parsed from the CLI
    pub(crate) fn build(
        files: Vec<PathBuf>,
        clipboard: bool,
        toggle: bool,
        show: bool,
        hide: bool,
        close_all: bool,
        quit: bool,
        list: bool,
        density: Option<u32>,
    ) -> Result<Self> {
        let selected = [toggle, show, hide, close_all, quit, list]
            .into_iter()
            .filter(|flag| *flag)
            .count();
        if selected > 1 {
            return Err(VshotError::InvalidDestination(
                "the pin control flags --toggle/--show/--hide/--close-all/--quit/--list \
                 are mutually exclusive"
                    .into(),
            ));
        }
        if selected == 1 && (!files.is_empty() || clipboard) {
            return Err(VshotError::InvalidDestination(
                "cannot combine image files or --clipboard with a pin control flag".into(),
            ));
        }
        if selected == 1 && density.is_some() {
            return Err(VshotError::InvalidDestination(
                "--density only applies to images being pinned, not to a pin control flag".into(),
            ));
        }
        if selected == 0 && files.is_empty() && !clipboard {
            return Err(VshotError::InvalidDestination(
                "pin requires image files, --clipboard, or a control flag such as --toggle".into(),
            ));
        }
        // The flag wins over the environment, which wins over the config file;
        // the environment is the fallback for a hotkey that cannot pass flags,
        // and the config file is the fallback for one that cannot pass either.
        let density = match density {
            Some(value) => Some(value),
            None if !files.is_empty() || clipboard => match density_from_env()? {
                Some(value) => Some(value),
                None => crate::config::load().pin.density,
            },
            None => None,
        };
        let command = if !files.is_empty() || clipboard {
            None
        } else if quit {
            Some(PinCommand::Quit)
        } else if close_all {
            Some(PinCommand::Close)
        } else if list {
            Some(PinCommand::List)
        } else if show {
            Some(PinCommand::Show)
        } else if hide {
            Some(PinCommand::Hide)
        } else {
            Some(PinCommand::Toggle)
        };
        Ok(Self {
            files,
            clipboard,
            command,
            density,
        })
    }
}

/// Runs a pin invocation, printing the list summary for `--list`.
pub(crate) fn run(invocation: PinInvocation) -> Result<()> {
    let PinInvocation {
        files,
        clipboard,
        command,
        density,
    } = invocation;
    if let Some(command) = command {
        let list = matches!(command, PinCommand::List);
        let reply = execute(command)?;
        if list {
            println!(
                "{} pinned image(s), {}",
                reply.count.unwrap_or(0),
                if reply.visible.unwrap_or(true) {
                    "visible"
                } else {
                    "hidden"
                }
            );
        }
        return Ok(());
    }
    // Resolved once per invocation, and only when something is actually being
    // pinned: the probes are subprocesses, and a control flag does not need
    // them. `None` just lets the daemon pick, which is what happens on
    // compositors without a probe.
    let (output, output_name) = active_output_hints();
    for file in &files {
        let absolute = if file.is_absolute() {
            file.clone()
        } else {
            std::env::current_dir()
                .map_err(|error| VshotError::Pin(format!("failed to resolve cwd: {error}")))?
                .join(file)
        };
        // The override, when one was given, rides along; otherwise the daemon
        // sizes the image from what it can find out about it.
        execute(PinCommand::Add {
            path: absolute,
            density,
            output,
            output_name: output_name.clone(),
        })?;
    }
    if clipboard {
        execute(PinCommand::AddClipboard {
            density,
            output,
            output_name,
        })?;
    }
    Ok(())
}

/// The compositor's answer for "which output is the user on", in the two shapes
/// the daemon accepts: the output's name, which Qt can match against a screen
/// outright, and its global logical rect for the compositors and daemons that
/// only know geometry. Both are `None` when no probe could answer, which leaves
/// the choice to the daemon.
fn active_output_hints() -> (Option<WireOutputRect>, Option<String>) {
    let Some(active) = crate::capture::active_output::active_output() else {
        return (None, None);
    };
    (active.rect.map(WireOutputRect::from), active.name)
}

/// Where the daemon socket lives: `VSHOT_PIN_SOCKET` overrides everything
/// (isolated instances and tests); otherwise under `XDG_RUNTIME_DIR` when
/// set, falling back to `/tmp`. The uid suffix keeps concurrent users apart.
fn socket_path() -> PathBuf {
    if let Some(override_path) = std::env::var_os("VSHOT_PIN_SOCKET") {
        let path = PathBuf::from(override_path);
        if !path.as_os_str().is_empty() {
            return path;
        }
    }
    let runtime = std::env::var_os("XDG_RUNTIME_DIR")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp"));
    runtime.join(format!(
        "vshot-pin-{}.sock",
        rustix::process::getuid().as_raw()
    ))
}

fn connect_stream(path: &Path) -> std::io::Result<UnixStream> {
    let stream = UnixStream::connect(path)?;
    stream.set_read_timeout(Some(IO_TIMEOUT))?;
    stream.set_write_timeout(Some(IO_TIMEOUT))?;
    Ok(stream)
}

/// Whether the daemon should keep the terminal's stderr.
///
/// It normally drops every stream: it outlives the terminal the CLI ran in and
/// has nothing to say. The exception is a debug switch in its environment --
/// `VSHOT_PIN_DEBUG` and `VSHOT_PIN_FOCUS_DEBUG` send their trace to stderr,
/// and a trace nobody can read is worse than no trace at all, so the daemon
/// then writes into the terminal that started it.
fn keep_daemon_stderr() -> bool {
    const TRACES: [&str; 2] = ["VSHOT_PIN_DEBUG", "VSHOT_PIN_FOCUS_DEBUG"];
    TRACES.iter().any(|name| std::env::var_os(name).is_some())
}

/// Starts the resident daemon detached: no pipes are inherited, so the CLI
/// returns immediately while the surfaces live on.
fn spawn_daemon() -> Result<()> {
    let helper = helper_program()?;
    Command::new(&helper.path)
        .arg("--pin-server")
        .arg(socket_path())
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(if keep_daemon_stderr() {
            Stdio::inherit()
        } else {
            Stdio::null()
        })
        .spawn()
        .map_err(|source| {
            if source.kind() == std::io::ErrorKind::NotFound {
                VshotError::Pin(format!(
                    "Qt helper `{}` was not found; build it with `cmake -S . -B build-qt && \
                     cmake --build build-qt` or point VSHOT_QT_HELPER at the executable",
                    helper.path.display()
                ))
            } else {
                VshotError::CommandIo {
                    program: helper.path.display().to_string(),
                    source,
                }
            }
        })?;
    Ok(())
}

/// Sends one command, starting the daemon first when nothing owns the socket.
pub(crate) fn execute(command: PinCommand) -> Result<PinReply> {
    let path = socket_path();
    let mut payload = serde_json::to_vec(&command)
        .map_err(|error| VshotError::Pin(format!("failed to encode pin request: {error}")))?;
    // The daemon splits requests on the newline terminator.
    payload.push(b'\n');

    let mut stream = match connect_stream(&path) {
        Ok(stream) => stream,
        Err(first) => {
            if !matches!(
                first.kind(),
                std::io::ErrorKind::ConnectionRefused | std::io::ErrorKind::NotFound
            ) {
                return Err(VshotError::Pin(format!(
                    "failed to reach the pin socket {}: {first}",
                    path.display()
                )));
            }
            spawn_daemon()?;
            wait_for_daemon(&path, first)?
        }
    };
    stream
        .write_all(&payload)
        .map_err(|error| VshotError::Pin(format!("failed to send pin request: {error}")))?;
    read_reply(&mut stream)
}

fn wait_for_daemon(path: &Path, first: std::io::Error) -> Result<UnixStream> {
    let deadline = Instant::now() + DAEMON_STARTUP;
    loop {
        std::thread::sleep(Duration::from_millis(25));
        if let Ok(stream) = connect_stream(path) {
            return Ok(stream);
        }
        if Instant::now() >= deadline {
            return Err(VshotError::Pin(format!(
                "the pin daemon did not answer on {}: {first}",
                path.display()
            )));
        }
    }
}

fn read_reply(stream: &mut UnixStream) -> Result<PinReply> {
    let mut buffer = Vec::new();
    let mut chunk = [0u8; 1024];
    loop {
        let read = stream
            .read(&mut chunk)
            .map_err(|error| VshotError::Pin(format!("failed to read pin reply: {error}")))?;
        if read == 0 {
            break;
        }
        buffer.extend_from_slice(&chunk[..read]);
        if buffer.contains(&b'\n') {
            break;
        }
    }
    let reply: PinReply = serde_json::from_slice(&buffer)
        .map_err(|error| VshotError::Pin(format!("pin daemon returned invalid JSON: {error}")))?;
    reply.into_result()
}

/// Pins freshly encoded PNG bytes: written to a private temp file, handed to
/// the daemon (which loads them into memory), then unlinked right away, so a
/// `--pin` capture never leaves a file on the user's disk. `density` is the
/// capture's device pixels per logical pixel — the scale of the output it was
/// taken on — so the pin reappears at the size it had there.
pub(crate) fn pin_png(png: &[u8], density: u32) -> Result<()> {
    use std::os::unix::fs::OpenOptionsExt;

    let directory = tempfile::Builder::new()
        .prefix("vshot-pin-")
        .tempdir_in("/dev/shm")
        .or_else(|_| tempfile::tempdir())
        .map_err(|error| {
            VshotError::Pin(format!("failed to create pin temp directory: {error}"))
        })?;
    let path = directory.path().join("capture.png");
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&path)
        .map_err(|source| VshotError::WriteFile {
            path: path.clone(),
            source,
        })?;
    file.write_all(png)
        .map_err(|source| VshotError::WriteFile {
            path: path.clone(),
            source,
        })?;
    drop(file);
    // A capture is pinned where the user just made the selection; the
    // compositor still knows which output is focused.
    let (output, output_name) = active_output_hints();
    let result = execute(PinCommand::Add {
        path: path.clone(),
        density: Some(density.clamp(1, 4)),
        output,
        output_name,
    });
    // The daemon has copied the pixels by the time it replied; the temp file
    // is ours to remove even when the reply said no.
    let _ = std::fs::remove_file(&path);
    result.map(|_| ())
}

/// One interactive pin editing round: the Qt helper edits the pinned image,
/// the editor drives the live pin window over the daemon socket, and the
/// rendered result lands the pin where the user left it via the `move` command.
///
/// `session_path` points at the pin-edit session JSON the daemon wrote. The
/// blocking flow is intentional: the daemon spawns `vshot pin --apply` in the
/// background and simply waits for the edit to finish.
pub(crate) fn apply_edit(session_path: &Path) -> Result<()> {
    let payload = std::fs::read(session_path).map_err(|source| {
        VshotError::Pin(format!(
            "failed to read pin-edit session {}: {source}",
            session_path.display()
        ))
    })?;
    let session: serde_json::Value = serde_json::from_slice(&payload)
        .map_err(|error| VshotError::Pin(format!("invalid pin-edit session JSON: {error}")))?;
    let window = session
        .get("bounds")
        .ok_or_else(|| VshotError::Pin("pin-edit session has no bounds".into()))?;
    let window = parse_json_rect(window)?;
    let id = session
        .get("id")
        .and_then(serde_json::Value::as_u64)
        .ok_or_else(|| VshotError::Pin("pin-edit session has no pin id".into()))?;
    let output_name = session
        .pointer("/outputs/0/name")
        .and_then(serde_json::Value::as_str)
        .ok_or_else(|| VshotError::Pin("pin-edit session has no output name".into()))?
        .to_owned();
    let image_path = session
        .pointer("/outputs/0/path")
        .and_then(serde_json::Value::as_str)
        .ok_or_else(|| VshotError::Pin("pin-edit session has no image path".into()))?
        .to_owned();
    // The editor moves the real pin window instead of drawing its own copy, so
    // it needs a live daemon to talk to. The daemon always stamps this.
    let socket_path = session
        .get("socket")
        .and_then(serde_json::Value::as_str)
        .ok_or_else(|| VshotError::Pin("pin-edit session has no daemon socket".into()))?
        .to_owned();

    // The editor session and the rendered replacement share one private
    // directory; both are cleaned up when this function returns.
    let directory = session_path
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_else(std::env::temp_dir);

    let frame = crate::model::Frame::from_png(&std::fs::read(&image_path).map_err(|source| {
        VshotError::Pin(format!(
            "failed to read pinned image {image_path}: {source}"
        ))
    })?)?;

    let scale = pin_edit_scale(frame.size().width, window.size.width);
    let (_editor_dir, editor_session) =
        crate::qt_overlay::write_pin_edit_session(&crate::qt_overlay::PinEditSpec {
            frame: &frame,
            output_name: &output_name,
            window,
            scale,
            socket: Path::new(&socket_path),
            pin_id: id,
        })?;
    let output = crate::qt_overlay::run_session(&editor_session)?;
    let Some((selection, annotations)) = crate::qt_overlay::parse_edit_result(output, window)?
    else {
        // Cancelled. The editor moved the live pin while the user was
        // dragging, and that move stands: cancelling drops the annotations,
        // it does not undo where the user put the image. The pixels were
        // never replaced, so the pin keeps its original content.
        return Ok(());
    };

    // The editor reports the pin image's rect as the selection: the user may
    // have dragged the image to a new spot. Render the annotations over the
    // pin's own pixels and land the pin exactly there. The editor works in
    // screen-logical pixels; the ratio computed above maps them onto the pin's
    // device pixels — and it is also the scale the editor rasterized its text
    // bitmaps at, since that is the only scale its single output declares.
    let pipeline = crate::edit::pipeline_for_annotations(annotations, selection, scale, scale)?;
    let edited = pipeline.apply(crate::model::ImageDocument::new(frame))?;
    let png = edited.frame().to_png()?;

    let rendered_path = directory.join("pin-edited.png");
    let mut file = {
        use std::os::unix::fs::OpenOptionsExt;
        let mut options = std::fs::OpenOptions::new();
        options.write(true).create_new(true).mode(0o600);
        options
            .open(&rendered_path)
            .map_err(|source| VshotError::WriteFile {
                path: rendered_path.clone(),
                source,
            })?
    };
    file.write_all(&png)
        .map_err(|source| VshotError::WriteFile {
            path: rendered_path.clone(),
            source,
        })?;
    drop(file);

    let reply = execute(PinCommand::Move {
        id,
        x: selection.origin.x,
        y: selection.origin.y,
        path: Some(rendered_path.clone()),
    });
    let _ = std::fs::remove_file(&rendered_path);
    reply.map(|_| ())
}

/// Device pixels per logical pixel for a pin-edit round trip: the pin's
/// native width divided by its on-screen (logical) width. Plain pins and
/// captures are 1:1 logical; text cards render at the output's pixel
/// density (2x on HiDPI outputs). The renderer supports 1..=4.
fn pin_edit_scale(frame_width: u32, window_width: u32) -> u32 {
    if window_width == 0 {
        return 1;
    }
    (frame_width / window_width).clamp(1, 4)
}

fn parse_json_rect(value: &serde_json::Value) -> Result<crate::geometry::Rect> {
    use crate::geometry::Rect;
    let as_i64 = |key: &str| -> Result<i64> {
        value
            .get(key)
            .and_then(serde_json::Value::as_i64)
            .ok_or_else(|| VshotError::Pin(format!("pin-edit rect field `{key}` is missing")))
    };
    let x = i32::try_from(as_i64("x")?)
        .map_err(|_| VshotError::Pin("pin-edit rect x is out of range".into()))?;
    let y = i32::try_from(as_i64("y")?)
        .map_err(|_| VshotError::Pin("pin-edit rect y is out of range".into()))?;
    let width = u32::try_from(as_i64("width")?)
        .map_err(|_| VshotError::Pin("pin-edit rect width is out of range".into()))?;
    let height = u32::try_from(as_i64("height")?)
        .map_err(|_| VshotError::Pin("pin-edit rect height is out of range".into()))?;
    Ok(Rect::new(x, y, width, height))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn socket_path_names_the_uid_socket_and_honours_override() {
        // Keep both assertions in one test: env mutation is process-wide and
        // a leaked override would confuse a separate test.
        let path = socket_path();
        let name = path.file_name().unwrap().to_string_lossy().into_owned();
        assert!(name.starts_with("vshot-pin-"), "{name}");
        assert!(name.ends_with(".sock"), "{name}");
        std::env::set_var("VSHOT_PIN_SOCKET", "/tmp/custom-pin.sock");
        assert_eq!(socket_path(), PathBuf::from("/tmp/custom-pin.sock"));
        std::env::remove_var("VSHOT_PIN_SOCKET");
    }

    #[test]
    fn a_debug_switch_keeps_the_daemons_stderr() {
        // The trace goes to stderr, so dropping it would make the switch
        // useless: the daemon has to inherit the terminal that started it.
        std::env::remove_var("VSHOT_PIN_DEBUG");
        std::env::remove_var("VSHOT_PIN_FOCUS_DEBUG");
        assert!(!keep_daemon_stderr());
        std::env::set_var("VSHOT_PIN_FOCUS_DEBUG", "1");
        assert!(keep_daemon_stderr());
        std::env::remove_var("VSHOT_PIN_FOCUS_DEBUG");
        std::env::set_var("VSHOT_PIN_DEBUG", "1");
        assert!(keep_daemon_stderr());
        std::env::remove_var("VSHOT_PIN_DEBUG");
        assert!(!keep_daemon_stderr());
    }

    #[test]
    fn add_command_encodes_the_wire_shape() {
        let encoded = serde_json::to_vec(&PinCommand::Add {
            path: PathBuf::from("/tmp/x.png"),
            density: None,
            output: None,
            output_name: None,
        })
        .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert_eq!(value["cmd"], "add");
        assert_eq!(value["path"], "/tmp/x.png");
        // No probe result means no field at all: the daemon then picks.
        assert!(value.get("output").is_none(), "{value}");
        assert!(value.get("output_name").is_none(), "{value}");
        assert!(value.get("density").is_none(), "{value}");
        let encoded = serde_json::to_vec(&PinCommand::Add {
            path: PathBuf::from("/tmp/x.png"),
            density: Some(2),
            output: Some(WireOutputRect::from(crate::geometry::Rect::new(
                1920, 0, 3840, 2160,
            ))),
            output_name: Some("DP-2".into()),
        })
        .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert_eq!(value["output_name"], "DP-2");
        assert_eq!(value["output"]["x"], 1920);
        assert_eq!(value["output"]["width"], 3840);
        assert_eq!(value["density"], 2);
        // The name travels on its own too: KWin answers with the name of the
        // active output and no geometry at all.
        let encoded = serde_json::to_vec(&PinCommand::Add {
            path: PathBuf::from("/tmp/x.png"),
            density: None,
            output: None,
            output_name: Some("DP-2".into()),
        })
        .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert_eq!(value["output_name"], "DP-2");
        assert!(value.get("output").is_none(), "{value}");
        let encoded = serde_json::to_vec(&PinCommand::AddClipboard {
            density: None,
            output: None,
            output_name: None,
        })
        .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert_eq!(value, serde_json::json!({"cmd": "add-clipboard"}));
        let encoded = serde_json::to_vec(&PinCommand::AddClipboard {
            density: Some(2),
            output: None,
            output_name: Some("DP-3".into()),
        })
        .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert_eq!(value["cmd"], "add-clipboard");
        assert_eq!(value["density"], 2);
        assert_eq!(value["output_name"], "DP-3");
        let encoded = serde_json::to_vec(&PinCommand::Toggle).unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert_eq!(value, serde_json::json!({"cmd": "toggle"}));
    }

    #[test]
    fn pin_edit_scale_follows_the_display_density() {
        assert_eq!(pin_edit_scale(160, 160), 1);
        assert_eq!(pin_edit_scale(320, 160), 2);
        assert_eq!(pin_edit_scale(1120, 560), 2);
        // Zoomed-in pins (screen rect larger than the image) stay at 1x.
        assert_eq!(pin_edit_scale(80, 160), 1);
        // Degenerate window falls back to 1x; density is capped at 4.
        assert_eq!(pin_edit_scale(320, 0), 1);
        assert_eq!(pin_edit_scale(4000, 500), 4);
    }

    #[test]
    fn the_manual_density_overrides_and_the_environment_backs_it_up() {
        let build = |density| {
            PinInvocation::build(
                vec![PathBuf::from("a.png")],
                false,
                false,
                false,
                false,
                false,
                false,
                false,
                density,
            )
        };
        // Nothing stated: the daemon works the density out for itself.
        assert_eq!(build(None).unwrap().density, None);
        // The flag is the answer when given.
        assert_eq!(build(Some(3)).unwrap().density, Some(3));
        // The environment stands in for a hotkey that cannot pass flags.
        std::env::set_var("VSHOT_PIN_DENSITY", "2");
        assert_eq!(build(None).unwrap().density, Some(2));
        // ... but never over the flag.
        assert_eq!(build(Some(4)).unwrap().density, Some(4));
        // A control flag neither needs nor accepts a density.
        let error = PinInvocation::build(
            Vec::new(),
            false,
            true,
            false,
            false,
            false,
            false,
            false,
            Some(2),
        )
        .unwrap_err();
        assert!(error.to_string().contains("--density"), "{error}");
        // A malformed value is reported, not silently ignored.
        std::env::set_var("VSHOT_PIN_DENSITY", "banana");
        let error = build(None).unwrap_err();
        assert!(error.to_string().contains("VSHOT_PIN_DENSITY"), "{error}");
        std::env::remove_var("VSHOT_PIN_DENSITY");
    }

    #[test]
    fn a_capture_brings_its_source_density_and_files_leave_it_open() {
        let encoded = serde_json::to_vec(&PinCommand::Add {
            path: PathBuf::from("/tmp/x.png"),
            density: Some(2),
            output: None,
            output_name: None,
        })
        .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert_eq!(value["density"], 2);
        // No density stated: the field is absent and the daemon sizes the
        // image from what it can find out about it.
        let encoded = serde_json::to_vec(&PinCommand::Add {
            path: PathBuf::from("/tmp/x.png"),
            density: None,
            output: None,
            output_name: None,
        })
        .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert!(value.get("density").is_none(), "{value}");
    }

    #[test]
    fn error_replies_become_failures() {
        let reply: PinReply =
            serde_json::from_str(r#"{"ok":false,"error":"cannot load pin image"}"#).unwrap();
        let error = reply.into_result().unwrap_err();
        assert!(error.to_string().contains("cannot load pin image"));
        let reply: PinReply =
            serde_json::from_str(r#"{"ok":true,"count":3,"visible":false}"#).unwrap();
        let reply = reply.into_result().unwrap();
        assert_eq!(reply.count, Some(3));
        assert_eq!(reply.visible, Some(false));
    }
}

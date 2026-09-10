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
    },
    #[serde(rename = "add-clipboard")]
    AddClipboard,
    Replace {
        id: u64,
        path: PathBuf,
    },
    Toggle,
    Show,
    Hide,
    Close,
    Quit,
    List,
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
        if selected == 0 && files.is_empty() && !clipboard {
            return Err(VshotError::InvalidDestination(
                "pin requires image files, --clipboard, or a control flag such as --toggle".into(),
            ));
        }
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
        })
    }
}

/// Runs a pin invocation, printing the list summary for `--list`.
pub(crate) fn run(invocation: PinInvocation) -> Result<()> {
    let PinInvocation {
        files,
        clipboard,
        command,
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
    for file in &files {
        let absolute = if file.is_absolute() {
            file.clone()
        } else {
            std::env::current_dir()
                .map_err(|error| VshotError::Pin(format!("failed to resolve cwd: {error}")))?
                .join(file)
        };
        execute(PinCommand::Add { path: absolute })?;
    }
    if clipboard {
        execute(PinCommand::AddClipboard)?;
    }
    Ok(())
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

/// Starts the resident daemon detached: no pipes are inherited, so the CLI
/// returns immediately while the surfaces live on.
fn spawn_daemon() -> Result<()> {
    let helper = helper_program()?;
    Command::new(&helper.path)
        .arg("--pin-server")
        .arg(socket_path())
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
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
/// `--pin` capture never leaves a file on the user's disk.
pub(crate) fn pin_png(png: &[u8]) -> Result<()> {
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
    let result = execute(PinCommand::Add { path: path.clone() });
    // The daemon has copied the pixels by the time it replied; the temp file
    // is ours to remove even when the reply said no.
    let _ = std::fs::remove_file(&path);
    result.map(|_| ())
}

/// One interactive pin editing round: the Qt helper edits the pinned image,
/// and the rendered result replaces the pin via the `replace` command.
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
        })?;
    let output = crate::qt_overlay::run_session(&editor_session)?;
    let Some((selection, annotations)) = crate::qt_overlay::parse_edit_result(output, window)?
    else {
        // Cancelled: keep the pin as it was.
        return Ok(());
    };

    // Render the annotations over the pin's own pixels. The editor works in
    // window-logical pixels; the ratio computed above maps them onto the
    // pin's device pixels.
    let pipeline = crate::edit::pipeline_for_annotations(annotations, selection, scale)?;
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

    let reply = execute(PinCommand::Replace {
        id,
        path: rendered_path.clone(),
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
    fn add_command_encodes_the_wire_shape() {
        let encoded = serde_json::to_vec(&PinCommand::Add {
            path: PathBuf::from("/tmp/x.png"),
        })
        .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert_eq!(value["cmd"], "add");
        assert_eq!(value["path"], "/tmp/x.png");
        let encoded = serde_json::to_vec(&PinCommand::AddClipboard).unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert_eq!(value, serde_json::json!({"cmd": "add-clipboard"}));
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

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};

use crate::error::{Result, VshotError};
use crate::geometry::Size;
use crate::model::hdr::Transfer;
use crate::model::HdrFrame;
use crate::output::HdrHalf;
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
        /// The same capture's HDR half, when it has one: the PQ codes a
        /// ten-bit surface shows with no tone map in between.  The daemon
        /// copies the file out of the CLI's temp directory (which is gone by
        /// the time it replies) and hands the copy to the surface helper; a pin
        /// without it is shown from `path` alone, the way every pin used to be.
        #[serde(skip_serializing_if = "Option::is_none")]
        hdr: Option<PathBuf>,
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
        /// Where on the desktop the capture came from, in global logical
        /// pixels, when it came from a place at all.  A pin made from a capture
        /// lands back exactly there instead of in the middle of the output, so
        /// pinning a window over itself is seamless.  Bare files and clipboard
        /// images have no such place and leave this out.
        #[serde(skip_serializing_if = "Option::is_none")]
        at: Option<WirePoint>,
        /// The marks the pinned pixels carry, in the shape the editor reports
        /// them, when the pin came out of an editing session that had any.
        /// The daemon keeps them so the pin can be opened for editing again
        /// with the user's marks still on it: they are the marks as data, which
        /// the flattened pixels no longer are.  Absent for a pin made from a
        /// file, the clipboard or a capture nothing was drawn on.
        #[serde(skip_serializing_if = "Option::is_none")]
        annotations: Option<serde_json::Value>,
        /// The capture `path` was flattened from, before any mark was drawn on
        /// it: the pristine picture the marks belong to.  Only sent beside
        /// `annotations`, and it is what makes the marks editable a second
        /// time -- an editor handed the flattened pixels and the marks would
        /// draw every one of them again on top of its own baked copy, which is
        /// the duplicate the user sees.  The daemon shows `path` and edits
        /// `base`.
        #[serde(skip_serializing_if = "Option::is_none")]
        base: Option<PathBuf>,
        /// Ask for the reply to wait until the pin is really on the screen
        /// rather than merely applied.  Only a client that is about to stop
        /// drawing asks for it: the pin editor unmaps its own surface as soon
        /// as it has the answer, and without this it would do that over a
        /// picture the compositor has not drawn yet, which the user sees as the
        /// marks vanishing for a moment.  Absent means "answer as soon as the
        /// change is applied", which is what every other caller wants.
        #[serde(skip_serializing_if = "std::ops::Not::not")]
        ack: bool,
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
        /// A replacement HDR half for the new pixels, when the editor rendered
        /// them in HDR as well.  `None` keeps whatever the pin had, which is
        /// what a plain move wants; a pin whose pixels were replaced from the
        /// SDR editor sends neither and becomes an ordinary SDR pin.
        #[serde(skip_serializing_if = "Option::is_none")]
        hdr: Option<PathBuf>,
        /// The marks the new pixels carry, in the shape the editor reports
        /// them.  The daemon keeps them so the pin can be opened for editing
        /// again: they are the marks as data, which the flattened pixels above
        /// no longer are.  Absent on a plain move, which replaces nothing.
        #[serde(skip_serializing_if = "Option::is_none")]
        annotations: Option<serde_json::Value>,
        /// Ask for the reply to wait until the new pixels are on the screen;
        /// see `Add::ack`.  The pin editor's own handoff uses it.
        #[serde(skip_serializing_if = "std::ops::Not::not")]
        ack: bool,
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

/// A point on the desktop's global logical grid.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
pub(crate) struct WirePoint {
    pub x: i32,
    pub y: i32,
}

impl From<crate::geometry::Point> for WirePoint {
    fn from(point: crate::geometry::Point) -> Self {
        Self {
            x: point.x,
            y: point.y,
        }
    }
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
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct PinInvocation {
    pub files: Vec<PathBuf>,
    pub clipboard: bool,
    pub command: Option<PinCommand>,
    /// Device density to stamp on the pinned images, overriding what the
    /// daemon would work out for itself. `None` leaves it to the daemon.
    pub density: Option<u32>,
    /// The light in cd/m² an HDR half's `1.0` stands for, for the files that do
    /// not say themselves. Resolved once here rather than at the decode, so the
    /// flag, the config file and the built-in default are decided in one place.
    pub reference_nits: f32,
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
        reference_white: Option<f32>,
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
        // The flag wins, then the config file, then BT.2408's reference white.
        // Read off the output a capture came from is the first answer, and this
        // is the answer for the files that carry none: another writer's Radiance
        // file, or an AVIF with no VShot box naming one.
        let reference_nits = reference_white
            .or_else(crate::config::hdr_reference_white_default)
            .map(crate::model::hdr::clamp_reference_nits)
            .unwrap_or(crate::model::hdr::REFERENCE_WHITE_NITS);
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
            reference_nits,
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
        reference_nits,
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
        pin_file(
            absolute,
            density,
            reference_nits,
            output,
            output_name.clone(),
        )?;
    }
    if clipboard {
        // A capture puts its own file on the clipboard as a URI, so a clipboard
        // pin can be resolved to the file the program wrote and, beside it, the
        // HDR half.  That is the one case where the clipboard carries more than
        // the daemon can see for itself, and it is only taken when there really
        // is a half to find: everything else -- an image, text, a colour, a file
        // with no sibling -- is left to the daemon, which resolves the clipboard
        // the way it always has.
        match clipboard_source() {
            Some(path) => pin_file(path, density, reference_nits, output, output_name)?,
            None => {
                execute(PinCommand::AddClipboard {
                    density,
                    output,
                    output_name,
                })?;
            }
        }
    }
    Ok(())
}

/// Pins one image file, with the HDR half a capture wrote beside it when there
/// is one.  `density` is the override the command line or the config asked for;
/// `None` leaves the size to the daemon, which reads it off the image and the
/// source record.  `reference_nits` is the white to read a half at that names
/// none of its own.
fn pin_file(
    path: PathBuf,
    density: Option<u32>,
    reference_nits: f32,
    output: Option<WireOutputRect>,
    output_name: Option<String>,
) -> Result<()> {
    let half = match hdr_half_beside(&path, reference_nits) {
        Some(pq) => Some(PinHalf::write(&pq)?),
        None => None,
    };
    execute(PinCommand::Add {
        path,
        hdr: half.as_ref().map(|half| half.path.clone()),
        density,
        output,
        output_name,
        at: None,
        annotations: None,
        base: None,
        // Nothing is drawing this pin, so there is no handoff to make seamless
        // and nothing to wait for.
        ack: false,
    })
    .map(|_| ())
}

/// One pin's HDR half as it travels: the PQ file the daemon copies out of this
/// directory, and the directory itself, which has to outlive the round trip
/// because the daemon reads the file before it answers.
struct PinHalf {
    _directory: tempfile::TempDir,
    path: PathBuf,
}

impl PinHalf {
    fn write(pq: &PqPin) -> Result<Self> {
        let directory = pin_tempdir()?;
        let path = write_private_file(directory.path(), "capture.pq", &pq.encode())?;
        Ok(Self {
            _directory: directory,
            path,
        })
    }
}

/// The file a capture wrote beside `path`: the same stem with an HDR codec's
/// suffix.  `shot.png` and `shot.avif` are one capture's two halves, so this is
/// the name `write_capture_files` would have used for the second one.
///
/// A file that is itself an HDR half has no sibling: the suffix it already
/// carries is the one being looked for, and the file next to it with the other
/// codec's suffix is a different image, not this one's second half.
fn hdr_sibling_path(path: &Path) -> Option<PathBuf> {
    if crate::model::codec::from_extension(path).is_some() {
        return None;
    }
    let mut sibling = path.to_path_buf();
    for codec in crate::model::codec::codecs() {
        sibling.set_extension(codec.extension());
        if sibling.is_file() {
            return Some(sibling);
        }
    }
    None
}

/// The HDR half a capture wrote beside `path`, as the PQ codes a pin travels
/// with: the file read back through the same codec layer that wrote it, so
/// "encode then decode" is a round trip through one currency rather than a
/// conversion between two.
///
/// `None` when there is no sibling, and also when there is one this build
/// cannot read.  The sibling is found by name, and a file beside a PNG that
/// merely shares its stem is not necessarily this program's HDR half; the pin
/// then shows its SDR picture alone, which is what it did before there was an
/// HDR half to find.  A half the user *named* is a different matter -- the
/// daemon refuses that one rather than dropping it in silence -- but a guess is
/// not worth failing a pin over.
fn hdr_half_beside(path: &Path, reference_nits: f32) -> Option<PqPin> {
    let sibling = hdr_sibling_path(path)?;
    let codec = crate::model::codec::from_extension(&sibling)?;
    match codec.decode_path(&sibling, reference_nits) {
        Ok(image) => {
            // The codes go out in the file's own gamut and at the file's own
            // reference white, which is what makes the round trip exact: the
            // white cancels when the surface helper re-encodes, and the pin
            // editor decodes them back to the light the capture held.
            let white = image.white();
            Some(PqPin {
                words: image.frame.to_rgb10_pq_in(image.frame.primaries(), white),
                width: image.frame.size().width,
                height: image.frame.size().height,
                reference_nits: white,
                primaries: image.frame.primaries(),
            })
        }
        Err(error) => {
            if std::env::var_os("VSHOT_PIN_DEBUG").is_some() {
                eprintln!(
                    "vshot: pin: {} is not readable as an HDR half ({error}); \
                     the pin is the SDR picture alone",
                    sibling.display()
                );
            }
            None
        }
    }
}

/// The file the clipboard names, when it names one that has an HDR half beside
/// it.  Anything else is left to the daemon: it resolves the clipboard's image,
/// its text and its colours itself, and this exists only to recover the second
/// file of a capture this program wrote — `vshot region -o shot.png` leaves
/// `shot.png` on the clipboard as a URI, and `shot.avif` is the half the daemon
/// has no way to guess at.
///
/// The two shapes are the ones the daemon itself reads a path from, in its own
/// order: a `text/uri-list`, then a plain text that is nothing but a path.  A
/// clipboard holding only image bytes names no file, so there is nothing here to
/// resolve and the daemon is left to pin the pixels — which is what
/// `wl-copy < shot.png` leaves, and it is the one case where the second file
/// cannot be found from the clipboard at all.
fn clipboard_source() -> Option<PathBuf> {
    let listed = paste(&["--list-types"])?;
    let types = String::from_utf8_lossy(&listed);
    let offers = |wanted: &str| types.lines().any(|line| line.trim() == wanted);
    if offers("text/uri-list") {
        // `--no-newline` keeps the list byte-exact.
        if let Some(uris) = paste(&["--type", "text/uri-list", "--no-newline"]) {
            if let Some(path) = uri_list_path(&String::from_utf8_lossy(&uris)) {
                return Some(path);
            }
        }
    }
    // A plain text that is one line and names a file: the shape `wl-copy
    // /path/shot.png` leaves, and the one the daemon reads as a path too.
    for candidate in [
        "text/plain;charset=utf-8",
        "text/plain",
        "UTF8_STRING",
        "STRING",
        "TEXT",
    ] {
        if !offers(candidate) {
            continue;
        }
        let Some(bytes) = paste(&["--type", candidate, "--no-newline"]) else {
            continue;
        };
        let text = String::from_utf8_lossy(&bytes);
        let text = text.trim();
        if !text.is_empty() && !text.contains('\n') {
            let path = PathBuf::from(text);
            if hdr_sibling_path(&path).is_some() {
                return Some(path);
            }
        }
    }
    None
}

/// The first local file a `text/uri-list` names that has an HDR half beside it.
/// A list may carry several files, and `#` starts a comment in the format.
fn uri_list_path(list: &str) -> Option<PathBuf> {
    for line in list.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let Some(path) = file_uri_path(line) else {
            continue;
        };
        if hdr_sibling_path(&path).is_some() {
            return Some(path);
        }
    }
    None
}

/// Runs `wl-paste` with `args` and answers its bytes.
///
/// `None` when it is not installed, when the clipboard does not offer the type
/// that was asked for (which `wl-paste` reports by exiting non-zero), or when
/// it fails for any other reason.  All of those mean the same thing here: this
/// side cannot resolve the clipboard, so the daemon is left to.
fn paste(args: &[&str]) -> Option<Vec<u8>> {
    let output = Command::new("wl-paste").args(args).output().ok()?;
    output.status.success().then_some(output.stdout)
}

/// The local path a `file://` URI names.
///
/// `file:///a/b` is a path with an empty authority; `file://host/a/b` names a
/// host this side cannot read, and is refused rather than read as `/a/b`.
fn file_uri_path(uri: &str) -> Option<PathBuf> {
    let rest = uri.strip_prefix("file://")?.strip_prefix('/')?;
    Some(PathBuf::from(format!("/{}", percent_decode(rest)?)))
}

/// The bytes a percent-encoded URI component stands for, as text.  `None` on a
/// `%` that is not followed by two hex digits, or on bytes that are not UTF-8 —
/// a path this side cannot name is one it cannot read.
fn percent_decode(value: &str) -> Option<String> {
    let bytes = value.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut at = 0;
    while at < bytes.len() {
        if bytes[at] == b'%' {
            out.push(u8::from_str_radix(value.get(at + 1..at + 3)?, 16).ok()?);
            at += 3;
        } else {
            out.push(bytes[at]);
            at += 1;
        }
    }
    String::from_utf8(out).ok()
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

/// The HDR half of a capture as a pin travels: PQ-encoded words at the frame's
/// own device size, the exact shape a ten-bit surface buffer takes, with the
/// light a code of `1.0` stands for and the gamut the codes are written in.  The
/// white travels with the pixels because a PQ code alone does not say what light
/// it means, and both the surface helper and the pin editor have to read it
/// back; the gamut travels with them for the same reason, because a PQ signal is
/// relative to the primaries it is written against and a wide-gamut capture read
/// as BT.2020 shifts every colour.
pub(crate) struct PqPin {
    pub words: Vec<u32>,
    pub width: u32,
    pub height: u32,
    pub reference_nits: f32,
    /// The primaries the codes are in: the output's own, which is also the
    /// description the surface carries.  [`crate::model::hdr::Primaries::Bt2020`]
    /// only when the output really was BT.2020.
    pub primaries: crate::model::hdr::Primaries,
}

impl PqPin {
    /// The file body the surface helper reads: the magic, the size, the white,
    /// the gamut's six chromaticity coordinates as the protocol's millionth-unit
    /// integers, and the words in little-endian order.
    fn encode(&self) -> Vec<u8> {
        let mut bytes = Vec::with_capacity(44 + self.words.len() * 4);
        bytes.extend_from_slice(b"VSHTPQ02");
        bytes.extend_from_slice(&self.width.to_le_bytes());
        bytes.extend_from_slice(&self.height.to_le_bytes());
        bytes.extend_from_slice(&self.reference_nits.to_le_bytes());
        for (x, y) in self.primaries.chromaticities() {
            bytes.extend_from_slice(&((x * 1_000_000.0).round() as i32).to_le_bytes());
            bytes.extend_from_slice(&((y * 1_000_000.0).round() as i32).to_le_bytes());
        }
        for word in &self.words {
            bytes.extend_from_slice(&word.to_le_bytes());
        }
        bytes
    }
}

/// The pin half of a region session's handoff: the editor has finished drawing
/// and the capture it was drawing on has to be on the screen before it stops,
/// or the marks blink out between the editor's surface and the pin's.
///
/// Called from inside the helper dialogue, on the editor's release, which is
/// the only moment the editor is both still up and done drawing -- the pixels
/// it rendered reach this side as an argument rather than through the
/// dialogue's own return, which happens after the helper is gone.  `rendered`
/// is the editor's render, which is the SDR result outright: Qt is the only
/// annotation renderer, so there is nothing left to rasterize here.  The HDR
/// half is marked from the marks it sent beside it, because an opaque flattened
/// picture cannot be composited onto HDR -- it would replace the light instead
/// of marking it.
///
/// A session that did not ask to pin, or a cancelled one, pins nothing: the
/// answer is a release, not a pin, and the CLI's own path then does whatever
/// the command line asked for.
#[allow(clippy::too_many_arguments)]
pub(crate) fn hand_off_region_pin(
    // The capture as it was before the editor drew on it, which the daemon
    // keeps as the picture the marks belong to: a second edit then opens on the
    // user's marks instead of on the pixels they were flattened into.
    base: &crate::model::Frame,
    hdr: Option<&HdrHalf>,
    density: u32,
    rect: crate::geometry::Rect,
    rendered: &crate::qt_overlay::RenderedCapture,
    marks: Option<&serde_json::Value>,
    tone_map: crate::model::hdr::ToneMapOptions,
) -> Result<()> {
    // The editor reports the pin image's rect as the selection and the pin
    // lands back where the capture came from, exactly as a fixed-region pin
    // does.
    let at = Some(rect.origin);
    let sdr = &rendered.composite;
    let marked = match hdr {
        Some(HdrHalf {
            frame,
            reference_nits,
        }) => {
            let mut marked = frame.clone();
            marked.composite_srgb_layer(&rendered.marks)?;
            marked
                .carries_hdr(tone_map.hdr)
                .then_some((marked, *reference_nits))
        }
        None => None,
    };
    let pq = marked.map(|(frame, reference_nits)| PqPin {
        words: frame.to_rgb10_pq_in(frame.primaries(), reference_nits),
        width: frame.size().width,
        height: frame.size().height,
        reference_nits,
        primaries: frame.primaries(),
    });
    pin_png(
        &sdr.to_png()?,
        density,
        at,
        pq.as_ref(),
        marks,
        // The pin destination reads the base only when there are marks to
        // carry it for, and a session that drew nothing has none.
        match marks {
            Some(_) => Some(base.to_png()?),
            None => None,
        }
        .as_deref(),
        true,
    )
}

/// Pins an in-memory capture.  `at` is the global logical top-left of the
/// content the capture came from, when it came from the desktop at all: the pin
/// lands back exactly there, which is what makes pinning a window over itself
/// seamless.  `None` (a composed or synthetic image) lets the daemon place it.
/// `hdr` is the capture's HDR half, when the content really is HDR; the daemon
/// takes it and shows the pin on a surface of its own.  `marks` is what the
/// editing session drew, when there was one, so the pin opens for a second edit
/// on the user's marks instead of on the flattened pixels.  `handoff` asks the
/// daemon to answer only once the pin is on the screen: it is set when an
/// editing session is still drawing this capture and is about to stop.
pub(crate) fn pin_png(
    png: &[u8],
    density: u32,
    at: Option<crate::geometry::Point>,
    hdr: Option<&PqPin>,
    marks: Option<&serde_json::Value>,
    // The same capture before the marks were drawn on it.  Only sent when
    // `marks` is, and only a capture that came out of an editor has one.
    base_png: Option<&[u8]>,
    handoff: bool,
) -> Result<()> {
    let directory = pin_tempdir()?;
    let path = write_private_file(directory.path(), "capture.png", png)?;
    // The pristine picture the marks were drawn on, when there are marks.  It
    // travels as its own file because the daemon loads it later, per edit.
    let base_path = match (marks, base_png) {
        (Some(_), Some(base)) => Some(write_private_file(directory.path(), "base.png", base)?),
        _ => None,
    };
    // The helper reads this file by path, so it has to be a file and not a pipe:
    // the daemon copies it before this directory goes.
    let hdr_path = match hdr {
        Some(hdr) => Some(write_private_file(
            directory.path(),
            "capture.pq",
            &hdr.encode(),
        )?),
        None => None,
    };
    // A capture is pinned where the user just made the selection; the
    // compositor still knows which output is focused.
    let (output, output_name) = active_output_hints();
    let result = execute(PinCommand::Add {
        path: path.clone(),
        hdr: hdr_path.clone(),
        density: Some(density.clamp(1, 4)),
        output,
        output_name,
        at: at.map(WirePoint::from),
        annotations: marks.cloned(),
        base: base_path.clone(),
        // A capture pinned with the marks still drawn over it on the screen: the
        // editor's surface is about to go, and the reply is what tells it that
        // it may.  So it waits for the pin to be on the screen rather than for
        // the daemon to have applied it, and the marks do not blink out between
        // the two.  Without a session there is nothing drawing them, so nothing
        // to wait for.
        ack: handoff,
    });
    // The daemon has copied the pixels by the time it replied; the temp files
    // are ours to remove even when the reply said no.
    remove_pin_temp(&path);
    if let Some(base_path) = &base_path {
        remove_pin_temp(base_path);
    }
    if let Some(hdr_path) = &hdr_path {
        remove_pin_temp(hdr_path);
    }
    result.map(|_| ())
}

/// A directory for the files one pin travels through: private to this process,
/// and gone as soon as the request is answered.
///
/// `/dev/shm` when there is one, because a pinned capture is output-sized and
/// the daemon reads it before it replies; the system temp directory is the
/// fallback for a machine without shared memory.
fn pin_tempdir() -> Result<tempfile::TempDir> {
    tempfile::Builder::new()
        .prefix("vshot-pin-")
        .tempdir_in("/dev/shm")
        .or_else(|_| tempfile::tempdir())
        .map_err(|error| VshotError::Pin(format!("failed to create pin temp directory: {error}")))
}

/// Writes `bytes` into `directory` as a private file the pin daemon can read,
/// and answers the path.  Nothing here survives the request: the daemon copies
/// what it keeps, so the file only has to outlive a single round trip.
fn write_private_file(directory: &Path, name: &str, bytes: &[u8]) -> Result<PathBuf> {
    use std::os::unix::fs::OpenOptionsExt;

    let path = directory.join(name);
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&path)
        .map_err(|source| VshotError::WriteFile {
            path: path.clone(),
            source,
        })?;
    file.write_all(bytes)
        .map_err(|source| VshotError::WriteFile {
            path: path.clone(),
            source,
        })?;
    drop(file);
    Ok(path)
}

/// Removes one pin temp file, ignoring a failure: the whole directory goes with
/// the temp guard anyway, and a removal that fails must not turn a successful
/// pin into an error.
fn remove_pin_temp(path: &Path) {
    let _ = std::fs::remove_file(path);
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
    // The desktop the editor's keyboard-cursor walk expresses a pointer move
    // in.  The daemon stamps it because the session's own bounds are the pin,
    // which is not the screen; a session without one leaves the pointer
    // unmoved, which is the same outcome as a compositor with no injection
    // backend.
    let desktop = session.get("desktop").map(parse_json_rect).transpose()?;
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
    // Optional: which part of the editor to open on. Absent for the ordinary
    // annotation editor the Space key opens; the menu's `Recognize text…` row
    // asks for `"text"`.
    let action = session
        .get("action")
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default();

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

    // A pin that is shown in HDR carries a second file, and an edit has to land
    // on both halves or the pin would drop back to SDR the moment it is
    // annotated.  The PQ codes are decoded back to linear light here, the same
    // light the surface is showing, so the marks composite over what the user
    // sees on screen.
    let hdr_half = match session
        .pointer("/outputs/0/hdr")
        .and_then(serde_json::Value::as_str)
    {
        Some(hdr_path) => Some(decode_pq_half(Path::new(hdr_path))?),
        None => None,
    };

    // The marks the daemon holds, exactly as the editor last reported them, so
    // a second edit opens on them.  Read as JSON rather than re-parsed into
    // `Annotation`s: this side has no use for them beyond handing them back, and
    // re-spelling them would be a second place for the wire shape to drift.
    let marks = session.get("annotations").cloned();

    // The ratio between the pin's pixels and the rect it is shown in: the pin
    // editor's virtual output declares it as its scale, so the session's pixel
    // dimensions and its logical size agree exactly, and the editor's own
    // marks are drawn at the size the image is really shown at. A zoomed pin is
    // not a whole number of device pixels per logical pixel, which is why the
    // ratio is carried as it is rather than rounded: rounding a 1.1x zoom to 1
    // drew every mark at the wrong size and in the wrong place.
    let scale = crate::edit::Scale::ratio(frame.size().width, window.size.width);
    // How wide the daemon draws this pin's border. The rim is centred on the
    // image's edge, so half of it stands outside the image, and the editor has
    // to know that band to treat a drag on the rim as a drag on the pin. Absent
    // from a session an older daemon wrote, which means no border.
    let border_width = session
        .get("border_width")
        .and_then(serde_json::Value::as_u64)
        .unwrap_or(0) as u32;
    let (_editor_dir, editor_session) =
        crate::qt_overlay::write_pin_edit_session(&crate::qt_overlay::PinEditSpec {
            frame: &frame,
            output_name: &output_name,
            window,
            scale,
            socket: Path::new(&socket_path),
            pin_id: id,
            action,
            annotations: marks.as_ref(),
            border_width,
            desktop,
        })?;
    let mut hdr_half = hdr_half;
    // The handoff: everything from the editor's result to the daemon's answer
    // happens here, inside the dialogue, because the editor is holding the
    // picture up on the screen until it is told the pin has it.  The move
    // carries `ack`, so the daemon answers only once its own frame is
    // presented, and that answer is what the editor waits for.
    let mut released = false;
    let mut handoff_error: Option<VshotError> = None;
    let output = {
        let mut handoff = |result: &[u8],
                           composite: Option<&crate::qt_overlay::RenderedCapture>|
         -> Result<String> {
            released = true;
            if let Err(error) =
                apply_edit_result(result, composite, window, id, hdr_half.take(), &directory)
            {
                // The editor is still told to stop: it has drawn everything it
                // is going to draw and the session is over either way.  The
                // failure travels to the caller below.
                handoff_error = Some(error);
            }
            Ok("{}".to_string())
        };
        crate::qt_overlay::run_session(&editor_session, &frame, &mut handoff)?
    };
    if let Some(error) = handoff_error {
        return Err(error);
    }
    if released {
        return Ok(());
    }
    // The editor never asked to be let go: it wrote its result and exited,
    // which is a cancelled edit, or a helper that does not know the release.
    // Nothing was handed over while it was up, so the result is applied here
    // instead -- and a cancelled one applies nothing.
    apply_edit_result(
        &output.json,
        output.composite.as_ref(),
        window,
        id,
        hdr_half,
        &directory,
    )
}

/// The second half of one pin edit: the editor's result becomes the pin's new
/// pixels, and the daemon is told where they go.
///
/// Split out because it is called from the editor's release rather than after
/// the dialogue: the editor keeps drawing until this has landed, so it has to
/// happen while the helper is still running.  `half` is the pin's pristine HDR
/// capture, when it has one; it is consumed, because the marked HDR half
/// replaces it.
fn apply_edit_result(
    result: &[u8],
    composite: Option<&crate::qt_overlay::RenderedCapture>,
    window: crate::geometry::Rect,
    id: u64,
    half: Option<HdrHalf>,
    directory: &Path,
) -> Result<()> {
    let Some(edited) = crate::qt_overlay::parse_edit_result(result.to_vec(), window)? else {
        // Cancelled. The editor moved the live pin while the user was
        // dragging, and that move stands: cancelling drops the annotations,
        // it does not undo where the user put the image. The pixels were
        // never replaced, so the pin keeps its original content.
        return Ok(());
    };
    let selection = edited.selection;

    // The editor reports the pin image's rect as the selection: the user may
    // have dragged the image to a new spot. The pixels it hands back are the
    // whole answer for the SDR half -- Qt is the only renderer, so what the user
    // was looking at is what lands -- and the marks travel beside them for the
    // HDR half, which an opaque flattened picture cannot be composited onto.
    let annotations = edited.marks;

    // An HDR pin is annotated in HDR: the marks composite onto the HDR half in
    // linear light, exactly as a fresh capture with a Pin button is.  An SDR pin
    // keeps the plain path it always had.
    let (png, hdr_path) = match (half, composite) {
        (Some(half), Some(rendered)) => {
            let reference_nits = if half.reference_nits.is_finite() && half.reference_nits > 0.0 {
                half.reference_nits
            } else {
                crate::model::hdr::REFERENCE_WHITE_NITS
            };
            let mut annotated = half.frame;
            annotated.composite_srgb_layer(&rendered.marks)?;
            let png = rendered.composite.to_png()?;
            let pq = PqPin {
                words: annotated.to_rgb10_pq_in(annotated.primaries(), reference_nits),
                width: annotated.size().width,
                height: annotated.size().height,
                reference_nits,
                primaries: annotated.primaries(),
            };
            let rendered = write_private_file(directory, "pin-edited.pq", &pq.encode())?;
            (png, Some(rendered))
        }
        (_, Some(rendered)) => (rendered.composite.to_png()?, None),
        // The editor rendered nothing, which only happens when its own render
        // failed: it would have reported the failure through its status, so
        // there is nothing to replace the pin's pixels with.
        (_, None) => {
            return Err(VshotError::Pin(
                "the pin editor returned no rendered image".into(),
            ))
        }
    };

    let rendered_path = write_private_file(directory, "pin-edited.png", &png)?;

    let reply = execute(PinCommand::Move {
        id,
        x: selection.origin.x,
        y: selection.origin.y,
        path: Some(rendered_path.clone()),
        hdr: hdr_path.clone(),
        annotations: Some(annotations),
        // The editor is still drawing the marks the daemon is being handed, and
        // it stops as soon as this answers.  Waiting for the pin's own frame is
        // what makes the two pictures one: the compositor has drawn the pin
        // before the editor takes its copy away.
        ack: true,
    });
    let _ = std::fs::remove_file(&rendered_path);
    if let Some(hdr_path) = &hdr_path {
        let _ = std::fs::remove_file(hdr_path);
    }
    reply.map(|_| ())
}

/// Decodes one pin HDR file back to linear light: the same header the surface
/// helper reads, PQ over the gamut the file names, and the white the file names
/// as its own.
fn decode_pq_half(path: &Path) -> Result<HdrHalf> {
    let image = crate::pin_hdr::read_pq_file(path)?;
    Ok(HdrHalf {
        frame: HdrFrame::from_rgb10(
            &image.words,
            Size::new(image.width, image.height),
            Transfer::Pq,
            // The gamut the codes were written against, not a fixed BT.2020: a
            // pin taken from a P3-like output decodes to the light it holds
            // only when it is read back in that output's own primaries.
            image.primaries,
            // The words carry real alpha bits, the way a surface buffer does.
            true,
            image.reference_nits,
        )?,
        reference_nits: image.reference_nits,
    })
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
    use crate::model::hdr::Primaries;

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

    /// The sibling is the file `write_capture_files` would have written: the
    /// same stem with the codec's own suffix, and nothing else.
    #[cfg(feature = "avif")]
    #[test]
    fn the_hdr_half_is_found_beside_the_capture() {
        let directory = tempfile::tempdir().unwrap();
        let png = directory.path().join("shot.png");
        std::fs::write(&png, b"png").unwrap();
        assert!(hdr_sibling_path(&png).is_none(), "no half has been written");
        let avif = directory.path().join("shot.avif");
        std::fs::write(&avif, b"avif").unwrap();
        assert_eq!(hdr_sibling_path(&png), Some(avif.clone()));
        // A file that is already an HDR half has none: the suffix it carries is
        // the one being looked for, and the PNG beside it is the other half of
        // the same capture rather than a capture of its own.
        assert!(hdr_sibling_path(&avif).is_none());
        // The lookup is by stem, so any other file of the same stem finds the
        // half too -- the rule `write_capture_files` writes by, applied to a
        // name the user happened to pin instead of the PNG.
        let other = directory.path().join("shot.txt");
        std::fs::write(&other, b"notes").unwrap();
        assert_eq!(hdr_sibling_path(&other), Some(avif.clone()));
        // A different stem finds nothing: the half belongs to one capture.
        let unrelated = directory.path().join("elsewhere.png");
        std::fs::write(&unrelated, b"png").unwrap();
        assert!(hdr_sibling_path(&unrelated).is_none());
        // No extension at all still looks, the way `hdr_sibling_path` writes.
        let bare = directory.path().join("shot");
        std::fs::write(&bare, b"png").unwrap();
        assert_eq!(hdr_sibling_path(&bare), Some(avif));
    }

    /// The HDR half read back is the PQ codes the pin travels with: the same
    /// codes a capture's own pin carries, which is what makes the two pins show
    /// the same light.
    #[cfg(feature = "radiance")]
    #[test]
    fn the_hdr_half_is_read_back_as_the_codes_a_pin_carries() {
        use crate::geometry::Size;
        use crate::model::codec::HdrImage;

        let directory = tempfile::tempdir().unwrap();
        let png = directory.path().join("shot.png");
        std::fs::write(&png, b"png").unwrap();
        // A capture's HDR half: two pixels of a known light, written by the
        // codec layer the same way `write_capture_files` writes one.
        let source = HdrImage::new(
            HdrFrame::new(
                Size::new(2, 1),
                vec![[1.0, 0.5, 0.25, 1.0], [4.0, 2.0, 1.0, 1.0]],
            )
            .unwrap(),
            203.0,
        );
        let codec = crate::model::codec::by_name("hdr").expect("the radiance codec");
        codec
            .encode_path(&source, &directory.path().join("shot.hdr"))
            .unwrap();

        let half = hdr_half_beside(&png, crate::model::hdr::REFERENCE_WHITE_NITS)
            .expect("the half is found and read");
        assert_eq!((half.width, half.height), (2, 1));
        assert!((half.reference_nits - 203.0).abs() < 0.01);
        // The codes are what the capture's own pin would carry: the same frame
        // through the same encoder, so the two are byte for byte the same.
        let expected = source
            .frame
            .to_rgb10_pq_in(source.frame.primaries(), source.reference_nits);
        assert_eq!(half.words, expected);

        // A sibling that is not readable as an HDR half is not a failure: the
        // pin is the SDR picture alone, which is what it was before there was
        // anything to look for.
        std::fs::write(directory.path().join("shot.hdr"), b"not radiance").unwrap();
        assert!(hdr_half_beside(&png, crate::model::hdr::REFERENCE_WHITE_NITS).is_none());
    }

    /// A half that names no white of its own is read at the setting, and the
    /// codes it travels as are encoded against that same white — which is what
    /// makes the pin show the light the file holds rather than the light of a
    /// white the file never had.
    #[cfg(feature = "radiance")]
    #[test]
    fn a_half_with_no_white_of_its_own_is_read_at_the_setting() {
        use crate::geometry::Size;
        use crate::model::codec::HdrImage;

        let directory = tempfile::tempdir().unwrap();
        let png = directory.path().join("shot.png");
        std::fs::write(&png, b"png").unwrap();
        // What another program's Radiance writer produces: no `REFERENCE_NITS=`
        // and no `PRIMARIES=`, so both answers have to come from elsewhere.
        let mut bytes = Vec::new();
        bytes.extend_from_slice(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n");
        bytes.extend_from_slice(&[128, 128, 128, 129]);
        bytes.extend_from_slice(&[64, 64, 64, 130]);
        std::fs::write(directory.path().join("shot.hdr"), &bytes).unwrap();

        let half = hdr_half_beside(&png, 250.0).expect("the half is found and read");
        assert!(
            (half.reference_nits - 250.0).abs() < 0.01,
            "the setting is the white the file is read at, got {}",
            half.reference_nits
        );
        // The codes are absolute PQ, so the white does not merely label them: it
        // is what they were computed against.  A file read at a brighter white
        // is a dimmer picture, and the codes have to say so.
        let source = HdrImage::new(
            HdrFrame::new(
                Size::new(2, 1),
                vec![[1.0, 1.0, 1.0, 1.0], [1.0, 1.0, 1.0, 1.0]],
            )
            .unwrap(),
            250.0,
        );
        let expected = source.frame.to_rgb10_pq_in(source.frame.primaries(), 250.0);
        // The two RGBE pixels hold different light, so only the first is
        // compared: 128/128/128/129 is exactly 1.0, the file's own white.
        assert_eq!(
            half.words[0], expected[0],
            "1.0 in the file is the setting's white"
        );

        // And the setting is what decides it: the same file read at BT.2408's
        // reference is a different picture.
        let darker = hdr_half_beside(&png, crate::model::hdr::REFERENCE_WHITE_NITS)
            .expect("the half is found and read");
        assert!((darker.reference_nits - 203.0).abs() < 0.01);
        assert_ne!(darker.words[0], half.words[0]);
    }

    /// The two shapes a clipboard carries a path in, and the shapes it does
    /// not: a bare `file://` URI, a percent-encoded one, a plain path, and
    /// anything with no half beside it.
    #[cfg(feature = "radiance")]
    #[test]
    fn a_clipboard_path_resolves_only_when_a_half_is_beside_it() {
        assert_eq!(
            file_uri_path("file:///tmp/shot.png"),
            Some(PathBuf::from("/tmp/shot.png"))
        );
        // A percent-encoded path is decoded: a capture in a directory with a
        // space in its name still has to resolve.
        assert_eq!(
            file_uri_path("file:///tmp/my%20shots/shot.png"),
            Some(PathBuf::from("/tmp/my shots/shot.png"))
        );
        // A host this side cannot read is refused rather than read as a path on
        // this machine, and a truncated escape is not a path either.
        assert_eq!(file_uri_path("file://other/tmp/shot.png"), None);
        assert_eq!(file_uri_path("file:///tmp/shot%2.png"), None);
        assert_eq!(file_uri_path("/tmp/shot.png"), None);

        let directory = tempfile::tempdir().unwrap();
        let png = directory.path().join("shot.png");
        std::fs::write(&png, b"png").unwrap();
        let uri = format!("file://{}\n", png.display());
        // No half beside it: the daemon is left to resolve the clipboard, which
        // is what every clipboard without a capture behind it gets.
        assert_eq!(uri_list_path(&uri), None);
        std::fs::write(directory.path().join("shot.hdr"), b"hdr").unwrap();
        assert_eq!(uri_list_path(&uri), Some(png.clone()));
        // A comment and an empty line are skipped, and a list naming several
        // files resolves to the one that has a half.
        let list = format!("# a comment\n\nfile:///nonexistent.png\n{uri}");
        assert_eq!(uri_list_path(&list), Some(png));
    }

    // A pin made from an annotated capture carries the marks and the picture
    // they were drawn on, or the pin can only ever be edited once: a second
    // edit would be handed the flattening and draw every mark again on top of
    // its own baked copy.
    #[test]
    fn a_pin_carries_the_marks_and_the_base_they_were_drawn_on() {
        let marks = serde_json::json!([
            {"kind": "shape", "tool": "rect", "rect": {"x": 4, "y": 6, "width": 20, "height": 10}}
        ]);
        let encoded = serde_json::to_vec(&PinCommand::Add {
            path: PathBuf::from("/tmp/x.png"),
            hdr: None,
            density: Some(2),
            output: None,
            output_name: None,
            at: None,
            annotations: Some(marks.clone()),
            base: Some(PathBuf::from("/tmp/base.png")),
            ack: false,
        })
        .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert_eq!(value["annotations"], marks);
        assert_eq!(value["base"], "/tmp/base.png");

        // A capture nothing was drawn on carries neither, so the daemon opens
        // it blank rather than on a base that is not there.
        let encoded = serde_json::to_vec(&PinCommand::Add {
            path: PathBuf::from("/tmp/x.png"),
            hdr: None,
            density: None,
            output: None,
            output_name: None,
            at: None,
            annotations: None,
            base: None,
            ack: false,
        })
        .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert!(value.get("annotations").is_none(), "{value}");
        assert!(value.get("base").is_none(), "{value}");
    }

    #[test]
    fn add_command_encodes_the_wire_shape() {
        let encoded = serde_json::to_vec(&PinCommand::Add {
            path: PathBuf::from("/tmp/x.png"),
            hdr: None,
            density: None,
            output: None,
            output_name: None,
            at: None,
            annotations: None,
            base: None,
            ack: false,
        })
        .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert_eq!(value["cmd"], "add");
        assert_eq!(value["path"], "/tmp/x.png");
        // No probe result means no field at all: the daemon then picks.
        assert!(value.get("output").is_none(), "{value}");
        assert!(value.get("output_name").is_none(), "{value}");
        assert!(value.get("density").is_none(), "{value}");
        // An image with no place of its own says nothing about where it goes.
        assert!(value.get("at").is_none(), "{value}");
        let encoded = serde_json::to_vec(&PinCommand::Add {
            path: PathBuf::from("/tmp/x.png"),
            hdr: None,
            density: Some(2),
            output: Some(WireOutputRect::from(crate::geometry::Rect::new(
                1920, 0, 3840, 2160,
            ))),
            output_name: Some("DP-2".into()),
            at: None,
            annotations: None,
            base: None,
            ack: false,
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
            hdr: None,
            density: None,
            output: None,
            output_name: Some("DP-2".into()),
            at: None,
            annotations: None,
            base: None,
            ack: false,
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
    fn the_pin_edit_scale_is_the_ratio_the_image_is_shown_at() {
        use crate::edit::Scale;
        // A plain pin, and a text card rendered at an output's density: whole
        // numbers of device pixels per logical pixel, exactly as before.
        assert_eq!(Scale::ratio(160, 160).factor(), 1.0);
        assert_eq!(Scale::ratio(320, 160).factor(), 2.0);
        assert_eq!(Scale::ratio(1120, 560).factor(), 2.0);
        // A zoomed pin: the window is not a whole multiple of the image, and
        // truncating the ratio to 1 is what made the editor refuse the session.
        // The ratio is carried as it is, so the session's pixel dimensions and
        // its logical size agree and the marks land where they were drawn.
        assert!((Scale::ratio(160, 176).factor() - 0.909_09).abs() < 1e-4);
        // Zoomed out: fewer device pixels than logical ones, which no whole
        // number can express at all.
        assert_eq!(Scale::ratio(160, 320).factor(), 0.5);
        assert!((Scale::ratio(160, 144).factor() - 1.111_11).abs() < 1e-4);
        // Degenerate window falls back to 1 rather than dividing by zero.
        assert_eq!(Scale::ratio(320, 0).factor(), 1.0);
        assert_eq!(Scale::ratio(0, 160).factor(), 1.0);
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
                None,
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
            None,
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
            hdr: None,
            density: Some(2),
            output: None,
            output_name: None,
            at: Some(WirePoint { x: 100, y: 240 }),
            annotations: None,
            base: None,
            ack: false,
        })
        .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert_eq!(value["density"], 2);
        // The place the capture came from travels with it, so the pin can land
        // back on it.
        assert_eq!(value["at"]["x"], 100);
        assert_eq!(value["at"]["y"], 240);
        // No density stated: the field is absent and the daemon sizes the
        // image from what it can find out about it.
        let encoded = serde_json::to_vec(&PinCommand::Add {
            path: PathBuf::from("/tmp/x.png"),
            hdr: None,
            density: None,
            output: None,
            output_name: None,
            at: None,
            annotations: None,
            base: None,
            ack: false,
        })
        .unwrap();
        let value: serde_json::Value = serde_json::from_slice(&encoded).unwrap();
        assert!(value.get("density").is_none(), "{value}");
        assert!(value.get("at").is_none(), "{value}");
    }

    #[test]
    fn a_pq_pin_round_trips_through_the_surface_helpers_reader() {
        // The file a pin writes has to be exactly what the surface helper reads
        // and what the pin-edit path decodes: one header, then the words.
        let half = HdrFrame::new(
            Size::new(2, 1),
            vec![[1.0, 1.0, 1.0, 1.0], [4.0, 4.0, 4.0, 1.0]],
        )
        .unwrap();
        let pin = PqPin {
            words: half.to_rgb10_pq(crate::model::hdr::REFERENCE_WHITE_NITS),
            width: 2,
            height: 1,
            reference_nits: crate::model::hdr::REFERENCE_WHITE_NITS,
            primaries: Primaries::Bt2020,
        };
        let directory = tempfile::tempdir().unwrap();
        let path = write_private_file(directory.path(), "x.pq", &pin.encode()).unwrap();
        let image = crate::pin_hdr::read_pq_file(&path).unwrap();
        assert_eq!((image.width, image.height), (2, 1));
        assert_eq!(
            image.reference_nits,
            crate::model::hdr::REFERENCE_WHITE_NITS
        );
        assert_eq!(image.primaries, Primaries::Bt2020);
        assert_eq!(image.words, pin.words);
        // And the words decode back to the light they were encoded from: SDR
        // white stays white, and the bright pixel stays well above it.
        let decoded = decode_pq_half(&path).unwrap();
        let white = decoded.frame.pixel(0, 0).unwrap();
        assert!((white[0] - 1.0).abs() < 0.01, "{white:?}");
        let bright = decoded.frame.pixel(1, 0).unwrap();
        assert!((bright[0] - 4.0).abs() < 0.05, "{bright:?}");
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

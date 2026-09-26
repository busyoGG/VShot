// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

use std::path::PathBuf;

use clap::{error::ErrorKind, ArgGroup, CommandFactory, FromArgMatches, Parser, Subcommand};

use crate::error::{Result, VshotError};
use crate::geometry::{parse_geometry, Rect};
use crate::inject::Prefer;
use crate::longshot::LongShotOptions;
use crate::model::PngCompression;

#[derive(Debug, Parser)]
#[command(
    name = "vshot",
    version,
    about = "Strict-freeze Wayland screenshots for wlroots and KWin/Plasma",
    after_help = r#"vshot freezes the desktop once and captures from that still frame, so nothing
on screen moves while a selection is made.

Capture targets
  region, monitor, all, long   a screenshot of the frozen desktop
  window active, window pick   the focused window, or one you click
  record monitor|all|region|window
                               an MP4 on the GPU, one screen or one window
  record mics|stop             the audio inputs a recording could take
  replay start|save|status|stop
                               the last stretch of the screen kept in memory

Destination (every capture above goes to exactly one)
  -o, --output PATH   PNG at PATH, strftime-expanded
                      (shots/%Y%m%d-%H%M%S.png); its file URI is copied to the
                      clipboard afterwards, and `-` writes the PNG to stdout
  --clipboard         the PNG, copied to the clipboard
  --pin               the image pinned on screen, by the resident pin daemon

Shared modifiers
  -c, --cursor        draw the compositor cursor into the capture
  --png-compression   none|fastest|fast (default)|balanced|high, lossless

Compositors: wlroots sessions (Hyprland, Sway, labwc, niri) through
wlr-screencopy; KWin/Plasma through org.kde.KWin.ScreenShot2, granted only to a
client whose installed desktop file declares
`X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2` -- the package
installs one for /usr/bin/vshot, so a build run out of target/ cannot capture
under Plasma. niri reports no position for a tiled window over IPC, so `window
active` and `window pick` there use niri's own screenshot and crosshair.

`vshot pin` captures nothing: it drives the resident pin daemon, which pins
image files or whatever --clipboard holds, and exits once nothing is pinned.

Environment: VSHOT_QT_HELPER (which vshot-qt-ui), VSHOT_LANG (UI language),
VSHOT_PIXEL_DEBUG=1, VSHOT_SESSION_DEBUG=1, VSHOT_LONG_DEBUG_DIR=<dir>,
VSHOT_PIN_DEBUG=1, VSHOT_PIN_FOCUS_DEBUG=1, VSHOT_PIN_SOCKET,
VSHOT_PIN_DENSITY=N.

$XDG_CONFIG_HOME/vshot/config.json (or ~/.config/vshot/config.json) is
optional: `editor` is the annotation editor's style, `cli` supplies defaults for
flags not given here -- a command-line flag always wins. A malformed file falls
back to the built-in defaults.

Each subcommand keeps its own notes: `vshot <command> --help`."#,
    group = ArgGroup::new("destination")
        .args(["output", "clipboard", "pin"])
)]
pub struct Cli {
    #[command(subcommand)]
    pub command: Command,
    /// Include the compositor cursor in each native screencopy capture.
    #[arg(short = 'c', long = "cursor", global = true)]
    pub cursor: bool,
    /// Write the result to PATH, strftime-expanded (`-` writes to stdout): PNG
    /// bytes, with the file's URI copied to the clipboard. For `record` it is the
    /// video file: `-` is refused and `.mp4` is added when PATH has none.
    #[arg(
        short = 'o',
        long = "output",
        global = true,
        conflicts_with_all = ["clipboard", "pin"]
    )]
    pub output: Option<PathBuf>,
    /// Copy the result to the clipboard: PNG bytes, or `vshot ocr`'s text.
    #[arg(long, global = true, conflicts_with = "output")]
    pub clipboard: bool,
    /// Pin the captured image on screen instead of writing it anywhere.
    #[arg(
        long,
        global = true,
        conflicts_with_all = ["output", "clipboard"]
    )]
    pub pin: bool,
    /// PNG compression level for images written to a file, stdout or the
    /// clipboard: `none`, `fastest`, `fast` (the default), `balanced` or `high`,
    /// all lossless. `--pin` writes nothing to disk.
    #[arg(long = "png-compression", global = true, value_name = "LEVEL")]
    pub png_compression: Option<String>,
}

#[derive(Debug, Subcommand)]
pub enum Command {
    /// Select a region from the frozen desktop, optionally using fixed geometry.
    #[command(
        after_help = r#"Drag a rectangle on the frozen scene, adjust it with the eight handles or
the arrow keys (hold Shift for 10px steps) while a magnifier and a size readout
follow the pointer, then confirm with Enter, a double-click inside the selection
or the toolbar's OK. Esc or a right-click cancels; Esc inside a text box closes
only that box. The toolbar annotates with Rect, Ellipse, Arrow, Draw, Text and
Mosaic: Ctrl+Z / Ctrl+Y undo and redo, Delete removes the selected annotation,
Ctrl+V pastes a clipboard image. The final PNG is re-rendered from the
annotations, so it matches the preview.

VSHOT_QT_HELPER overrides which vshot-qt-ui is run, VSHOT_LANG its language."#
    )]
    Region {
        /// Fixed global geometry in `x,y widthxheight` form.
        #[arg(long, conflicts_with = "interactive", allow_hyphen_values = true)]
        geometry: Option<String>,
        /// Explicitly request pointer-driven selection; the default when geometry is omitted.
        #[arg(long, conflicts_with = "geometry")]
        interactive: bool,
    },
    /// Capture one monitor by name, or the monitor under the pointer with `current`.
    #[command(
        after_help = r#"The output is named from the frozen overlay's topology, so every capture needs
an output that answers `zxdg_output_manager_v1`. `current` reads the pointer, so
it needs a seat with a pointer capability -- a nested or virtual KWin has none;
name the output there."#
    )]
    Monitor {
        /// Output name, or `current` for the output under the pointer.
        name: Option<String>,
    },
    /// Capture the complete frozen desktop scene.
    #[command(
        after_help = r#"Every output is placed at its logical position, so a multi-monitor desktop is
composed as it is laid out and the gaps between outputs stay transparent."#
    )]
    All,
    /// Capture the focused window, or pick one interactively.
    #[command(
        after_help = r#"`active` resolves the focused window from compositor metadata where vshot can
query it (Hyprland, Sway, KWin/Plasma) and falls back to the captured pixels;
`pick` highlights the windows of the live desktop (the rest dimmed) and captures
the one you click. Picking re-captures the frame after the click, so the
annotations go on the frame captured after the pick."#
    )]
    Window {
        #[command(subcommand)]
        target: WindowTarget,
    },
    /// Capture a region that scrolls: scroll it automatically, grab it frame
    /// by frame, and stitch the frames into one tall image.
    #[command(
        after_help = r#"The region is picked interactively unless --geometry is given, then vshot
scrolls it with synthetic wheel events and stitches the frames it grabs into one
tall image. A hint bar shows for the duration: Enter, Space or a left click on it
finishes and keeps the image, Esc or a right-click cancels. The selection must
sit inside a single monitor. A capture ends when six wheels in a row move
nothing, or at --max-height, --max-frames or --timeout.

VSHOT_LONG_DEBUG_DIR=<dir> writes every grabbed frame as grab-NNNN.png and
appends each stitching decision to steps.log."#
    )]
    Long {
        /// Fixed global geometry in `x,y widthxheight` form; without it the
        /// region is picked interactively.
        #[arg(long, allow_hyphen_values = true)]
        geometry: Option<String>,
        /// Wheel notches sent at a time while the capture scrolls.
        #[arg(long)]
        notches: Option<u32>,
        /// Height limit of the stitched image, in pixels.
        #[arg(long = "max-height")]
        max_height: Option<u32>,
        /// Frame limit for one capture.
        #[arg(long = "max-frames")]
        max_frames: Option<u32>,
        /// Time limit for one capture, in seconds.
        #[arg(long)]
        timeout: Option<u64>,
        /// Rows at the top of every frame left out of the match, for sticky
        /// headers and fixed toolbars.
        #[arg(long = "ignore-top")]
        ignore_top: Option<u32>,
        /// Which wheel backend to use: `auto` (try them in order), `wlr` (the
        /// compositor's virtual pointer protocol), `portal` (the XDG
        /// RemoteDesktop portal) or `uinput` (`/dev/uinput`).
        #[arg(long)]
        inject: Option<String>,
    },

    /// Manage pinned images shown by the resident pin daemon; see --help for
    /// what can be pinned and how the pins behave.
    #[command(
        after_help = r#"Pins are owned by a resident daemon: the first `pin` starts it, and it exits by
itself once nothing is pinned any more. A pin is dragged to move it, the wheel
zooms about the image centre (0.1x-8x, the factor shown in its corner), a
double-click closes it, and the pin under the pointer carries the black outline
-- the one Space opens in the same editor as `vshot region`. Right-clicking a pin
opens its menu: a pinned color card offers its formats to copy, and every pin
offers `Save as…`, which writes the image out as a PNG.

Visibility control goes to the running daemon over its socket, so
--toggle/--show/--hide take effect at once and start no second daemon. Wayland
clients cannot receive global keys, so bind a hotkey in your compositor, e.g.
Hyprland:
    bind = SUPER, P, exec, vshot pin --toggle
    bind = SUPER SHIFT, P, exec, vshot pin --close-all

VSHOT_PIN_SOCKET overrides the daemon's socket, VSHOT_PIN_DENSITY=N the source
density of every pinned image, like --density. VSHOT_PIN_DEBUG=1 and
VSHOT_PIN_FOCUS_DEBUG=1 trace density decisions and surface focus to stderr."#
    )]
    Pin {
        /// Image files to pin (starts the daemon when it is not running).
        files: Vec<PathBuf>,
        /// Flip the visibility of every pin (default when no other flag is set).
        #[arg(long)]
        toggle: bool,
        /// Show all pins.
        #[arg(long)]
        show: bool,
        /// Hide all pins.
        #[arg(long)]
        hide: bool,
        /// Close every pin (the daemon stays resident).
        #[arg(long = "close-all")]
        close_all: bool,
        /// Quit the pin daemon.
        #[arg(long)]
        quit: bool,
        /// Report the pin count and visibility.
        #[arg(long)]
        list: bool,
        /// Device pixels per logical pixel of the pinned image (1-4), e.g. 2
        /// for a screenshot taken on a 2x output. vshot infers it from the
        /// capture, the image's own PNG density (96 DPI means 1x), the
        /// screenshot tool's record or the image size; this overrides that
        /// when the answer is wrong or unknown.
        #[arg(long, value_parser = clap::value_parser!(u32).range(1..=4))]
        density: Option<u32>,
        /// Internal: render one pin-edit session JSON written by the daemon
        /// and write the result back onto the pin. Not for interactive use.
        #[arg(long = "apply", hide = true, conflicts_with_all = ["toggle", "show", "hide", "close_all", "quit", "list", "density"])]
        apply: Option<PathBuf>,
    },

    /// Edit the remembered settings in a window: the annotation editor's style
    /// and the command-line defaults, both from the shared config file.
    #[command(
        after_help = r#"Opens a window over the same `$XDG_CONFIG_HOME/vshot/config.json` the annotation
editor and the CLI read. Saving writes the file, so the next run uses the chosen
style and the chosen defaults for flags that are not given. Nothing is captured,
and no compositor protocol beyond showing a window is needed."#
    )]
    Settings,

    /// Record the screen to an MP4, on the GPU.
    #[command(
        after_help = r#"Encoded on the GPU's media engine through libavcodec, loaded at run time: a
machine without ffmpeg still takes screenshots, and `record` alone reports what
is missing.

Targets: `monitor [NAME]` one output (a bare `record monitor` means `current`,
the output you are on), `all` every output composed, `region` one rectangle of
one output (--geometry, or dragged out on the frozen desktop), `window [NAME]`
one window's own pixels, not the area it covers. HEVC is also the answer for a
composed desktop wider than 4096 pixels, which this class of GPU cannot encode as
H.264.

A recording runs until it is stopped: `vshot record stop` sends the signal, or
Ctrl+C in the terminal that started it; either way the file is finished properly
(a seekable MP4 with its sample table written). `record stop` needs no display,
so it binds well:
    bind = SUPER SHIFT, R, exec, vshot record stop

The output path comes from the global -o/--output: strftime-expanded, defaulting
to vshot-%Y%m%d-%H%M%S.mp4 in the videos directory ($XDG_VIDEOS_DIR, else the one
xdg-user-dirs names, else ~/Videos), created when missing. Frames reach the
encoder without a CPU copy where the session speaks linux-dmabuf; the software
path is used otherwise and for `record all`.

--portal is not a silent fallback: the compositor's own picker chooses the
source, so a name or --pick does not decide it, and `record all --portal` is
refused because the portal hands over one stream. A screen cast only emits a
frame when the screen changes, so --fps there is a cap rather than the file's
rate. Needs libpipewire and xdg-desktop-portal; VSHOT_PORTAL_SHM=1 asks for
memory frames instead of dma-bufs.

A recording that never started leaves nothing behind; a process killed outright
leaves a file without its sample table.

Config keys: `cli.record` supplies `encoder`, `encoder-backend`, `fps`, `portal`,
`mic`, `follow` and `notify`; a flag always wins, and a remembered `follow` list
is read only for a bare `record window` (no NAME, no --pick).

VSHOT_RECORD_PIDFILE overrides the pid file `stop` reads, VSHOT_RECORD_DEBUG=1
traces each frame's stage, VSHOT_RECORD_NO_OVERLAY=1 forces the letterbox's
fallback composition instead of the GPU overlay."#
    )]
    Record {
        #[command(subcommand)]
        target: RecordTargetCommand,
        /// Frame rate the loop aims for, 1-240; the config's `cli.record.fps`
        /// when the flag is not given, else 60.
        #[arg(long, global = true, value_parser = clap::value_parser!(u32).range(1..=240))]
        fps: Option<u32>,
        /// Stop on its own after this many seconds.
        #[arg(long, global = true)]
        duration: Option<u64>,
        /// Record the microphone into the MP4 beside the video: a bare `--mic`
        /// takes the session's default source, a name another input (AAC), and
        /// `record mics` lists them. Without either flag `cli.record.mic`
        /// decides.
        #[arg(long, global = true, value_name = "DEVICE", num_args = 0..=1, default_missing_value = "")]
        mic: Option<String>,
        /// Do not record the microphone, even when `cli.record.mic` remembers
        /// one.
        #[arg(long = "no-mic", global = true, conflicts_with = "mic")]
        no_mic: bool,
        /// Record the window's own audio, summed with `--mic` into the one
        /// track. `record window` only, and not with --portal. The pid comes
        /// from the compositor: Hyprland, niri and KWin report it, Sway and
        /// labwc have none and say so.
        #[arg(long = "app-audio", global = true)]
        app_audio: bool,
        /// Follow the focus between windows: `--follow NAME`, repeated, names
        /// the windows the recording moves to. `record window` /
        /// `replay start window` only, never with a window NAME or `--pick`;
        /// without it, `cli.record.follow` / `cli.replay.follow`.
        #[arg(long = "follow", global = true, value_name = "NAME", action = clap::ArgAction::Append)]
        follow: Vec<String>,
        /// Do not follow the focus, even when `cli.record.follow` /
        /// `cli.replay.follow` remembers windows.
        #[arg(long = "no-follow", global = true, conflicts_with = "follow")]
        no_follow: bool,
        /// Video codec: h264 (default), hevc or av1, all on the GPU's media
        /// engine; the config's `cli.record.encoder` when the flag is not
        /// given.
        #[arg(
            long,
            global = true,
            value_parser = crate::record::avcodec::VideoCodec::ALL.map(|codec| codec.word())
        )]
        encoder: Option<String>,
        /// Hardware encoder: auto (default; VAAPI, else Vulkan, else NVENC),
        /// vaapi, vulkan or nvenc; `cli.record.encoder-backend` when the flag is
        /// not given. All encode on the GPU's media engine.
        #[arg(
            long,
            global = true,
            value_parser = crate::record::avcodec::EncoderBackend::ALL.map(|backend| backend.word())
        )]
        encoder_backend: Option<String>,
        /// Record through the XDG desktop portal
        /// (org.freedesktop.portal.ScreenCast) instead of the compositor's own
        /// protocols; `cli.record.portal` when the flag is not given. The
        /// picker chooses the source, so `record all --portal` is refused.
        #[arg(long, global = true)]
        portal: bool,
        /// Refuse the portal, even when `cli.record.portal` remembers it.
        #[arg(long = "no-portal", global = true, conflicts_with = "portal")]
        no_portal: bool,
    },

    /// Keep a rolling window of the screen in memory, and save it on demand.
    #[command(
        after_help = r#"A replay holds its last seconds in memory instead of writing them to a file:
`replay save` copies what the ring holds into an MP4 -- a stream copy, no
re-encode -- so the trigger costs almost nothing and nothing is written until it
is asked for. `--window` seconds is how far a save can reach back.

`replay start` runs the session, `replay save` asks it to write a file,
`replay status` prints how much history it holds, and `replay stop` ends it. The
control channel is a socket under $XDG_RUNTIME_DIR, so `save` and `stop` need no
display and work from a keybinding:
    bind = SUPER SHIFT, R, exec, vshot replay save

The target is what `record` records, from the same capture backends and the same
GPU encoder. `--fps` defaults to 30 (a recording defaults to 60), because a
replay is left running for long stretches.

A save starts at the newest key frame at or before `now - seconds`, so it holds
at least the seconds asked for; asking for more than the window holds gives
everything there is. `--background` detaches the session from the terminal.

Config keys: `cli.replay` supplies `window`, `encoder`, `encoder-backend`, `fps`,
`gop`, `mic`, `follow`, `portal`, `save-dir` and `notify`; a flag always wins, and
a remembered `follow` list is read only for a bare `replay start window`.

VSHOT_REPLAY_SOCKET overrides the control socket, VSHOT_REPLAY_PIDFILE the pid
file `replay stop` reads, and VSHOT_RECORD_DEBUG=1 traces each frame."#
    )]
    Replay {
        #[command(subcommand)]
        action: ReplayCommandLine,
        /// Seconds of history to keep in memory, 1-3600; the config's
        /// `cli.replay.window` when the flag is not given, else 30.
        #[arg(long, global = true, value_parser = clap::value_parser!(u64).range(1..=3600))]
        window: Option<u64>,
        /// Frame rate the loop aims for, 1-240; the config's `cli.replay.fps`
        /// when the flag is not given, else 30.
        #[arg(long, global = true, value_parser = clap::value_parser!(u32).range(1..=240))]
        fps: Option<u32>,
        /// Key-frame distance in seconds (1-10); `cli.replay.gop` when the flag
        /// is not given, else 1. Smaller values start a save closer to the
        /// requested edge.
        #[arg(long, global = true, value_parser = clap::value_parser!(u64).range(1..=10))]
        gop: Option<u64>,
        /// Video codec: h264 (default), hevc or av1; the config's
        /// `cli.replay.encoder` when the flag is not given.
        #[arg(
            long,
            global = true,
            value_parser = crate::record::avcodec::VideoCodec::ALL.map(|codec| codec.word())
        )]
        encoder: Option<String>,
        /// Hardware encoder: auto (default), vaapi, vulkan or nvenc;
        /// `cli.replay.encoder-backend` when the flag is not given. All encode
        /// on the GPU.
        #[arg(
            long,
            global = true,
            value_parser = crate::record::avcodec::EncoderBackend::ALL.map(|backend| backend.word())
        )]
        encoder_backend: Option<String>,
        /// Keep the microphone in the ring beside the video: a bare `--mic`
        /// takes the session's default source, and `record mics` lists them.
        /// Without either flag `cli.replay.mic` decides.
        #[arg(long, global = true, value_name = "DEVICE", num_args = 0..=1, default_missing_value = "")]
        mic: Option<String>,
        /// Do not keep the microphone, even when `cli.replay.mic` remembers one.
        #[arg(long = "no-mic", global = true, conflicts_with = "mic")]
        no_mic: bool,
        /// Keep the window's own application audio in the ring
        /// (`replay start window` only), summed with `--mic` into the one track.
        #[arg(long = "app-audio", global = true)]
        app_audio: bool,
        /// Follow the focus between windows, as `record --follow` does:
        /// `--follow NAME`, repeated. Only `replay start window`, never with a
        /// window NAME or `--pick`; without it, `cli.replay.follow`.
        #[arg(long = "follow", global = true, value_name = "NAME", action = clap::ArgAction::Append)]
        follow: Vec<String>,
        /// Do not follow the focus, even when the config's `cli.replay.follow`
        /// remembers windows to follow.
        #[arg(long = "no-follow", global = true, conflicts_with = "follow")]
        no_follow: bool,
        /// Directory a save lands in when `replay save` names no path;
        /// strftime-expanded. Defaults to the videos directory.
        #[arg(long, global = true, value_name = "DIR")]
        save_dir: Option<PathBuf>,
        /// Detach the session from the terminal (`replay start` only).
        #[arg(long, global = true)]
        background: bool,
    },

    /// Read the text out of a region of the screen.
    #[command(
        after_help = r#"Without --geometry the frozen scene is handed to the Qt overlay to frame the
text, as `vshot region` does but without the annotation editor: pick a rectangle,
press Enter, and its text comes back -- to stdout, or to the clipboard with
--clipboard. --input reads an image file instead.

Recognition runs PaddleOCR's PP-OCR models (the ONNX conversions) on ONNX
Runtime, on the CPU, in this process. The models are installed under
/usr/share/vshot/models and looked for beside the executable as well.

To use a GPU, point `ocr.engine` at an external program in
$XDG_CONFIG_HOME/vshot/config.json: it is handed a PNG and writes the text on
stdout. See the README's OCR section."#
    )]
    Ocr {
        /// Fixed global geometry in `x,y widthxheight` form.
        #[arg(long, conflicts_with = "interactive", allow_hyphen_values = true)]
        geometry: Option<String>,
        /// Explicitly request pointer-driven selection; the default when geometry is omitted.
        #[arg(long, conflicts_with = "geometry")]
        interactive: bool,
        /// Read an image from this file instead of capturing the screen; the
        /// annotation editor's text tool takes the same route.
        #[arg(long, value_name = "PATH", conflicts_with_all = ["geometry", "interactive"])]
        input: Option<PathBuf>,
    },
}

#[derive(Debug, Subcommand)]
pub enum RecordTargetCommand {
    /// Record one output by name, or the one you are on with `current`.
    #[command(
        after_help = r#"A bare `record monitor` means `current`: the output the compositor says you are
on -- the one under the pointer where it reports that, the focused output
otherwise. Nothing is frozen while recording, so the frames are live, and the
pointer never enters vshot: `current` is the compositor's answer, not the seat's."#
    )]
    Monitor {
        /// Output name; `current` (the default) is the output the compositor
        /// says you are on.
        #[arg(default_value = "current")]
        name: String,
    },
    /// Record the complete desktop: every output composed at its logical position.
    #[command(
        after_help = r#"Every output is composed at its logical position, frame by frame, so a
multi-monitor desktop becomes one video laid out as the desktop is."#
    )]
    All,
    /// Record one rectangle of the screen — a region, not a whole output.
    #[command(
        after_help = r#"The rectangle is in desktop logical coordinates (`x,y widthxheight`), as
`vshot region --geometry` takes, and it has to sit inside a single output: no
compositor call copies a region that spans two. Without --geometry the frozen
scene is handed to the Qt overlay and the rectangle is dragged out there; nothing
is opened or written until a rectangle is confirmed.

On a wlroots session the compositor renders just that rectangle into a dma-buf,
which reaches the encoder without a CPU copy, as `record monitor` does. A window
dragged inside the rectangle is recorded as it changes, but the rectangle itself
stays where it was drawn.

`--portal` records one stream the picker chooses, and the portal offers whole
screens, so with it the rectangle is not this command's own."#
    )]
    Region {
        /// Fixed global geometry in `x,y widthxheight` form; without it the
        /// rectangle is dragged out on the frozen desktop.
        #[arg(long, value_name = "GEOMETRY", allow_hyphen_values = true)]
        geometry: Option<String>,
        /// Explicitly request pointer-driven selection; the default when
        /// geometry is omitted.
        #[arg(long, conflicts_with = "geometry")]
        interactive: bool,
    },
    /// Record one window's own pixels — not the screen area it covers.
    #[command(
        after_help = r#"The compositor copies the window itself, so a covered window records whole, and
one dragged half off the screen still records whole; what is behind it never
appears.

The window is named by app id or title (the whole name first, else a
case-insensitive substring), picked with `--pick`, or the focused one when no
argument is given. This needs `ext_image_copy_capture_v1`; niri has no such
protocol and casts the window through its own screen-cast service
(`org.gnome.Mutter.ScreenCast`) instead, which needs no flag and shows no picker,
and `--follow` is not available on that route.

A window resized while recording is fitted into the recording's canvas (scaled
down, centred, letterboxed), because one MP4 holds one frame size. A window that
is closed ends the recording there, with the file finished properly; a window
whose output is off or disconnected never produces a frame, which is reported
after a few seconds."#
    )]
    Window {
        /// App id or title of the window; the focused window when omitted.
        #[arg(value_name = "NAME")]
        name: Option<String>,
        /// Pick the window to record by clicking it; a window on a hidden
        /// workspace can still be recorded, because its own pixels are
        /// captured.
        #[arg(long)]
        pick: bool,
    },
    /// List the audio inputs a recording could take: what `--mic` accepts.
    Mics,
    /// Stop the recording that is running.
    Stop,
}

/// What `vshot replay` was asked to do.
#[derive(Debug, Subcommand)]
pub enum ReplayCommandLine {
    /// Start a replay session, keeping the last `--window` seconds in memory.
    Start {
        #[command(subcommand)]
        target: ReplayTargetCommand,
    },
    /// Write the running session's history to a file.
    Save {
        /// Where to write it; a timestamped file in the videos directory (or
        /// `--save-dir`) when omitted.
        #[arg(value_name = "PATH")]
        path: Option<PathBuf>,
        /// How many seconds to take from the ring, 1-3600; the whole window
        /// when omitted.
        #[arg(long, value_parser = clap::value_parser!(u64).range(1..=3600))]
        seconds: Option<u64>,
    },
    /// Print how much history the running session holds.
    Status,
    /// End the replay session that is running.
    Stop,
}

/// What `vshot replay start` keeps in the ring: the same targets `record`
/// takes.
#[derive(Debug, Subcommand)]
pub enum ReplayTargetCommand {
    /// Replay one output by name, or the one you are on with `current`.
    #[command(
        after_help = r#"The choice `record monitor` makes: no name means `current`, the output the
compositor says you are on; give a name for another output. Nothing is frozen
during a replay either, so the frames are live."#
    )]
    Monitor {
        /// Output name; `current` (the default) is the output the compositor
        /// says you are on.
        #[arg(default_value = "current")]
        name: String,
    },
    /// Replay the complete desktop: every output composed at its logical position.
    #[command(after_help = r#"The composition `vshot all` uses, frame by frame."#)]
    All,
    /// Replay one rectangle of the screen — a region, not a whole output.
    #[command(
        after_help = r#"The rectangle is in desktop logical coordinates (`x,y widthxheight`) and has
to sit inside a single output. Without --geometry the frozen desktop goes to
the Qt overlay to drag it out there, and nothing is opened or written until a
rectangle is confirmed. On a wlroots session it takes the same zero-copy
dma-buf route as `record region`."#
    )]
    Region {
        /// Fixed global geometry in `x,y widthxheight` form; without it the
        /// rectangle is dragged out on the frozen desktop.
        #[arg(long, value_name = "GEOMETRY", allow_hyphen_values = true)]
        geometry: Option<String>,
        /// Explicitly request pointer-driven selection; the default when
        /// geometry is omitted.
        #[arg(long, conflicts_with = "geometry")]
        interactive: bool,
    },
    /// Replay one window's own pixels — not the screen area it covers.
    #[command(
        after_help = r#"The route `record window` takes: the compositor copies the window itself, so a
covered or half-off-screen window records whole. The window is named by app id
or title, picked with `--pick`, or -- with no argument -- the focused one. A
resize is fitted into the replay's canvas (scaled down, centred, letterboxed),
because a ring holds one frame size."#
    )]
    Window {
        /// App id or title of the window; the focused window when omitted.
        #[arg(value_name = "NAME")]
        name: Option<String>,
        /// Pick the window to replay by clicking it; a window on a hidden
        /// workspace can still be recorded, because its own pixels are
        /// captured.
        #[arg(long)]
        pick: bool,
    },
}

#[derive(Debug, Subcommand)]
pub enum WindowTarget {
    /// Capture the currently focused window: the compositor's own screenshot
    /// where it offers one, otherwise compositor metadata, otherwise pixel
    /// detection on the captured frame.
    #[command(
        after_help = r#"The window is taken from the compositor itself where it can draw one: KWin's
ScreenShot2 and niri's `screenshot-window` hand over the window's own pixels. On
niri a translucent window comes out with its alpha, so vshot locates that render
on a fresh capture of the window's output and crops the screen there; a match
failure re-renders and compares, retrying up to three times when the content
changed (a video, an animation) and falling back to niri's own translucent
picture when it did not. Otherwise the rectangle comes from compositor metadata
(Hyprland, Sway), and --pixel reads the border stroke off the captured frame
instead, falling back to background segmentation; borderless tiling has no pixel
signal and is reported as such. VSHOT_PIXEL_DEBUG=1 reports what each stage saw."#
    )]
    Active {
        /// Skip compositor metadata and detect the focused window from the
        /// captured pixels (border bands, then background segmentation); for
        /// compositors without metadata and for testing the detector.
        #[arg(long)]
        pixel: bool,
        /// On niri, take its own window render exactly as it hands it over
        /// instead of compositing it onto the captured background: a
        /// translucent window comes out with its alpha and nothing behind it,
        /// and niri's border is missing. The spare route for when locating the
        /// render on the screen misbehaves.
        #[arg(long = "no-blend", conflicts_with = "pixel")]
        no_blend: bool,
    },
    /// Pick a window on screen: hover to highlight, click to capture it.
    #[command(
        after_help = r#"The live desktop is shown with the candidate windows highlighted one at a time
and everything else dimmed; a left click captures the highlighted window, Esc or
a right-click cancels. The frame is grabbed again after the click, so the
captured window is the one visible then. Candidates come from the compositor's
window list unless --pixel is given.

On niri this is niri's own picker: its IPC reports no position for a tiled
window, so there is no rectangle to offer the overlay, and niri draws a crosshair
(no highlight). The clicked window's own screenshot is captured, which also means
no annotation editor afterwards; --pixel asks for the overlay back. A translucent
window comes out of niri with its alpha, so vshot locates that render on a fresh
capture of the window's output and crops the screen there, retrying with a fresh
render when the content changed, up to three times. `--no-blend` skips that
locating and takes niri's render as handed over."#
    )]
    Pick {
        /// Skip the compositor's window list and take the candidates from the
        /// captured pixels (border bands, then background segmentation); for
        /// compositors without a window-list query and for testing the
        /// detector. Borderless tiling has no pixel signal and is reported as
        /// such.
        #[arg(long)]
        pixel: bool,
        /// On niri, take its own window render exactly as it hands it over
        /// instead of compositing it onto the captured background — see
        /// `window active --no-blend`.
        #[arg(long = "no-blend", conflicts_with = "pixel")]
        no_blend: bool,
    },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Destination {
    File(PathBuf),
    Stdout,
    Clipboard,
    Pin,
}

/// What to capture, and with which knobs.
#[derive(Clone, Debug, PartialEq)]
pub enum CaptureTarget {
    RegionFixed(Rect),
    RegionInteractive,
    Monitor(String),
    All,
    ActiveWindow {
        /// Detect the window from pixels instead of compositor metadata.
        pixel_detect: bool,
        /// Take niri's own window render as it is instead of compositing it
        /// onto the captured background.
        no_blend: bool,
    },
    /// Interactive window picking: the compositor's window list when it has
    /// one, the captured pixels otherwise.
    WindowPick {
        /// Take the candidates from the pixels instead of the window list.
        pixel_detect: bool,
        /// Take niri's own window render as it is instead of compositing it
        /// onto the captured background.
        no_blend: bool,
    },
    /// Scrolling capture over an interactive (or fixed) region.
    LongShot {
        /// Fixed region; `None` asks the user to pick one.
        region: Option<Rect>,
        options: LongShotOptions,
        inject: Prefer,
    },
}

#[derive(Clone, Debug, PartialEq)]
pub struct Request {
    pub target: CaptureTarget,
    pub destination: Destination,
    pub cursor: bool,
    pub compression: PngCompression,
}

/// What `vshot` was asked to do.  Only the capture arm touches the Wayland
/// capture path; pin management talks to the daemon, and the settings window is
/// an ordinary toplevel over the config file.
#[derive(Clone, Debug, PartialEq)]
pub enum Action {
    Capture(Request),
    Pin(crate::pin::PinInvocation),
    /// Internal: render one pin-edit session and write the result back.
    PinApply(std::path::PathBuf),
    /// Show the settings window and wait for it to close.
    Settings,
    /// Read the text out of a region, to stdout or the clipboard.
    Ocr {
        /// Fixed region, `None` to frame it interactively, or a file to read.
        source: OcrSource,
        destination: OcrDestination,
    },
    /// Record the screen to a file; `Stop` ends a running recording.
    Record(RecordAction),
    /// Keep a rolling window of the screen in memory; `Save` writes it out.
    Replay(ReplayAction),
}

/// What `vshot record` was asked to do.
#[derive(Clone, Debug, PartialEq)]
pub enum RecordAction {
    Start(crate::record::RecordRequest),
    /// `record mics`: list the session's audio inputs, one per line.
    Mics,
    Stop,
}

/// What `vshot replay` was asked to do.
#[derive(Clone, Debug, PartialEq)]
pub enum ReplayAction {
    /// `replay start`: run the session (in the foreground, or detached with
    /// `--background`).
    Start {
        request: Box<crate::record::ReplayRequest>,
        background: bool,
    },
    /// `replay save`: write the running session's history out.
    Save {
        path: Option<PathBuf>,
        seconds: Option<u64>,
        /// The directory the session names the file in when `path` is absent,
        /// from `--save-dir`; the session's own remembered directory when
        /// this is `None`.
        save_dir: Option<PathBuf>,
    },
    /// `replay status`: print how much history the session holds.
    Status,
    /// `replay stop`: end the session.
    Stop,
}

/// Where `vshot ocr` gets its image.
#[derive(Clone, Debug, PartialEq)]
pub enum OcrSource {
    /// A region of the frozen screen, framed interactively.
    Screen,
    /// A fixed region of the frozen screen.
    Geometry(Rect),
    /// An image file on disk.
    File(PathBuf),
}

/// Where recognized text goes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OcrDestination {
    Stdout,
    Clipboard,
}

/// Refuses the options that only shape a session when one of the one-line
/// control shapes (`replay save`, `status`, `stop`) is given them.
///
/// Those three send a request to a session that is already running, so there
/// is nowhere to put a rate, a codec, a microphone or a follow list — and
/// accepting one silently would read as if it had been applied.  `options`
/// carries the name of each such option with whether the user actually gave
/// it, so only the ones that were typed are named.
fn refuse_session_options(shape: &str, options: &[(&str, bool)]) -> Result<()> {
    let named: Vec<&str> = options
        .iter()
        .filter(|(_, present)| *present)
        .map(|(name, _)| *name)
        .collect();
    if named.is_empty() {
        return Ok(());
    }
    Err(VshotError::InvalidDestination(format!(
        "{shape} does not take {}: {} a session, and only `replay start` starts one",
        named.join(", "),
        if named.len() == 1 {
            "it shapes"
        } else {
            "they shape"
        }
    )))
}

/// The parser, with the help output in the language `VSHOT_LANG` (or the
/// locale) asks for: the command tree is built as usual and then every help
/// string that has a translation is replaced, so `--help` and every
/// subcommand's help follow the same language as the Qt helper's UI.  clap's
/// own error wording stays English — those strings are clap's.
pub fn parse() -> Cli {
    let mut command = Cli::command();
    crate::cli_i18n::localize(&mut command);
    let matches = command.get_matches();
    Cli::from_arg_matches(&matches).unwrap_or_else(|error| error.exit())
}

impl Cli {
    /// Splits the CLI into the things `vshot` can do: capture to a destination,
    /// drive the pin daemon, render a pin-edit session, or show the settings
    /// window.
    pub fn parse_action(self) -> Result<Action> {
        if let Command::Record {
            target,
            fps,
            duration,
            encoder,
            encoder_backend,
            portal,
            no_portal,
            mic,
            no_mic,
            app_audio,
            follow,
            no_follow,
        } = &self.command
        {
            // A recording is a file, not an image: the screenshot
            // destinations have no meaning here, and `--pin` least of all.
            if self.pin || self.clipboard {
                return Err(VshotError::InvalidDestination(
                    "--clipboard and --pin do not apply to the record subcommand; a recording \
                     is written to a file"
                        .into(),
                ));
            }
            // The two read-only shapes take no recording options. The flags
            // are all global, so clap would accept them here; refusing them
            // explicitly is what keeps `record stop --fps 30` from looking
            // like it means something.
            let option_given = self.output.is_some()
                || fps.is_some()
                || duration.is_some()
                || encoder.is_some()
                || encoder_backend.is_some()
                || *portal
                || *no_portal
                || mic.is_some()
                || *no_mic
                || *app_audio
                || !follow.is_empty()
                || *no_follow;
            match target {
                RecordTargetCommand::Stop => {
                    if option_given {
                        return Err(VshotError::InvalidDestination(
                            "`record stop` takes no options: it signals the recording that is \
                             already running"
                                .into(),
                        ));
                    }
                    return Ok(Action::Record(RecordAction::Stop));
                }
                RecordTargetCommand::Mics => {
                    if option_given {
                        return Err(VshotError::InvalidDestination(
                            "`record mics` takes no options: it lists the audio inputs a \
                             recording could take"
                                .into(),
                        ));
                    }
                    return Ok(Action::Record(RecordAction::Mics));
                }
                _ => {}
            }
            // The portal: `--portal` is on, `--no-portal` is off, and
            // neither means the config's remembered default (off unless it
            // was written).
            let portal = if *no_portal {
                false
            } else if *portal {
                true
            } else {
                crate::record::default_portal()
            };
            // The portal hands over one stream per session and the compositor
            // decides which screen that is, so "the whole desktop" has no
            // portal shape: recording every output is the compositor's own
            // protocols' job.
            if portal && matches!(target, RecordTargetCommand::All) {
                return Err(VshotError::InvalidDestination(
                    "`--portal` records one stream, and the compositor chooses which screen it \
                     is; record a single screen instead (`vshot record monitor --portal`)"
                        .into(),
                ));
            }
            // The portal's picker offers whole screens or whole windows; a
            // rectangle of a screen is not something it can be asked for.
            // Refusing it beats recording a screen and calling it a region.
            if portal && matches!(target, RecordTargetCommand::Region { .. }) {
                return Err(VshotError::InvalidDestination(
                    "`--portal` records a whole screen or a whole window, not a rectangle of \
                     one; drop --portal to record a region, or record a screen instead"
                        .into(),
                ));
            }
            // `--app-audio` records one window's application, so it needs a
            // window to name that application.  A screen, the whole desktop or
            // a region has no single one, and picking the sound by anything
            // else would be a guess.
            if *app_audio && !matches!(target, RecordTargetCommand::Window { .. }) {
                return Err(VshotError::InvalidDestination(
                    "`--app-audio` records the sound of one window's application, so it needs a \
                     window: `vshot record window --app-audio`"
                        .into(),
                ));
            }
            // `--follow` moves a window recording between windows, so it needs
            // a window to move between: a screen or a region has no window to
            // follow the focus onto.
            if !follow.is_empty() && !matches!(target, RecordTargetCommand::Window { .. }) {
                return Err(VshotError::InvalidDestination(
                    "`--follow` moves a window recording between windows, so it needs a window: \
                     `vshot record window --follow NAME`"
                        .into(),
                ));
            }
            // A portal window recording never learns which window it got (the
            // compositor's picker decides, and the portal reports a stream, not
            // a window), so the application behind it cannot be named.
            if *app_audio && portal {
                return Err(VshotError::InvalidDestination(
                    "`--app-audio` needs the window's process, which a portal recording does not \
                     learn; drop --portal to record a window's own audio"
                        .into(),
                ));
            }
            let target = match target {
                RecordTargetCommand::Monitor { name } => {
                    if name.trim().is_empty() {
                        return Err(VshotError::InvalidDestination(
                            "monitor name cannot be empty".into(),
                        ));
                    }
                    crate::record::RecordTarget::Monitor(name.clone())
                }
                RecordTargetCommand::All => crate::record::RecordTarget::All,
                RecordTargetCommand::Region {
                    geometry,
                    interactive: _,
                } => {
                    let target = match geometry {
                        Some(geometry) => {
                            crate::record::RegionTarget::Fixed(parse_geometry(geometry)?)
                        }
                        None => crate::record::RegionTarget::Pick,
                    };
                    crate::record::RecordTarget::Region(target)
                }
                RecordTargetCommand::Window { name, pick } => {
                    // One way to say which window, not three that fight.
                    if *pick && name.is_some() {
                        return Err(VshotError::InvalidDestination(
                            "`record window` takes a window name or `--pick`, not both".into(),
                        ));
                    }
                    // `--follow` names the windows to move between, so a
                    // separate starting window would contradict it: the focus
                    // decides where a followed recording starts (see the
                    // window loop), and a NAME alongside it would say two
                    // different things about the same first frame.
                    if !follow.is_empty() && (name.is_some() || *pick) {
                        return Err(VshotError::InvalidDestination(
                            "`--follow` names the windows to move between, so it takes no window \
                             name or `--pick`: the focus decides which of them is recorded"
                                .into(),
                        ));
                    }
                    let target = if *pick {
                        crate::record::WindowTarget::Pick
                    } else {
                        match name.as_deref() {
                            Some(filter) if !filter.trim().is_empty() => {
                                crate::record::WindowTarget::Filter(filter.to_owned())
                            }
                            Some(_) => {
                                return Err(VshotError::InvalidDestination(
                                    "the window name cannot be empty".into(),
                                ))
                            }
                            None => crate::record::WindowTarget::Active,
                        }
                    };
                    crate::record::RecordTarget::Window(target)
                }
                // Both handled above, with the options they refuse.
                RecordTargetCommand::Stop | RecordTargetCommand::Mics => {
                    unreachable!("handled above")
                }
            };
            // The windows to follow, now that the target is known: an explicit
            // `--follow` list wins, `--no-follow` clears it, and a plain
            // `record window` — no NAME, no `--pick` — falls back to the
            // config's `record.follow`.  Any other target gets no list at all,
            // so a remembered follow cannot turn a `record monitor` into the
            // error `--follow` would be there.
            let follow = if *no_follow {
                Vec::new()
            } else if !follow.is_empty() {
                follow.clone()
            } else if matches!(
                target,
                crate::record::RecordTarget::Window(crate::record::WindowTarget::Active)
            ) {
                crate::record::default_follow()
            } else {
                Vec::new()
            };
            let encoder = match encoder.as_deref() {
                None => crate::record::default_encoder(),
                Some(word) => crate::record::avcodec::VideoCodec::parse(word).ok_or_else(|| {
                    VshotError::InvalidDestination(format!(
                        "unknown encoder `{word}`: pick h264, hevc or av1"
                    ))
                })?,
            };
            let encoder_backend = match encoder_backend.as_deref() {
                None => crate::record::default_encoder_backend(),
                Some(word) => {
                    crate::record::avcodec::EncoderBackend::parse(word).ok_or_else(|| {
                        VshotError::InvalidDestination(format!(
                            "unknown encoder backend `{word}`: pick auto, vaapi, vulkan or nvenc"
                        ))
                    })?
                }
            };
            // The microphone: `--mic` is on, `--no-mic` is off, and
            // neither means the config's remembered default (off unless it
            // was written).  An empty name is the default source.
            let mic = if *no_mic {
                None
            } else {
                match mic {
                    // `--mic` with no value fills an empty name (clap's
                    // `default_missing_value`), which means the default
                    // source, exactly like no flag at all... except that
                    // giving the flag is itself the decision, so the config
                    // does not get a vote.
                    Some(name) if name.trim().is_empty() => Some(crate::record::MicChoice::Default),
                    Some(name) => Some(crate::record::MicChoice::Device(name.clone())),
                    None => crate::record::default_mic(),
                }
            };
            return Ok(Action::Record(RecordAction::Start(
                crate::record::RecordRequest {
                    target,
                    output: self.output.clone(),
                    fps: fps.unwrap_or_else(crate::record::default_fps),
                    cursor: self.cursor,
                    duration: *duration,
                    encoder,
                    encoder_backend,
                    portal,
                    mic,
                    app_audio: *app_audio,
                    follow: follow.clone(),
                },
            )));
        }
        if let Command::Replay {
            action,
            window,
            fps,
            gop,
            encoder,
            encoder_backend,
            mic,
            no_mic,
            app_audio,
            follow,
            no_follow,
            save_dir,
            background,
        } = &self.command
        {
            // A replay is not a capture: the screenshot destinations have no
            // meaning, and a replay writes its file from the session, not the
            // command line that triggered it.
            if self.pin || self.clipboard {
                return Err(VshotError::InvalidDestination(
                    "--clipboard and --pin do not apply to the replay subcommand".into(),
                ));
            }
            // The options that only shape a session, with which of them the
            // user actually gave: the three one-line control shapes ask a
            // session that is already running and cannot apply any of them.
            let session_options: [(&str, bool); 11] = [
                ("--window", window.is_some()),
                ("--fps", fps.is_some()),
                ("--gop", gop.is_some()),
                ("--encoder", encoder.is_some()),
                ("--encoder-backend", encoder_backend.is_some()),
                ("--mic", mic.is_some()),
                ("--no-mic", *no_mic),
                ("--app-audio", *app_audio),
                ("--follow", !follow.is_empty()),
                ("--no-follow", *no_follow),
                // `--save-dir` is the one session option a save can use — the
                // path is resolved by the session, so the directory travels
                // with the request — and it is the last entry so a save can
                // leave it out of the list it refuses.
                ("--save-dir", save_dir.is_some()),
            ];
            // A save keeps `--save-dir`; the other two have no use for it.
            let for_save = &session_options[..session_options.len() - 1];
            return match action {
                ReplayCommandLine::Save { path, seconds } => {
                    if self.output.is_some() || *background {
                        return Err(VshotError::InvalidDestination(
                            "`replay save` takes a PATH argument, not --output or --background"
                                .into(),
                        ));
                    }
                    refuse_session_options("`replay save`", for_save)?;
                    Ok(Action::Replay(ReplayAction::Save {
                        path: path.clone(),
                        seconds: *seconds,
                        save_dir: save_dir.clone(),
                    }))
                }
                ReplayCommandLine::Status => {
                    if self.output.is_some() || *background {
                        return Err(VshotError::InvalidDestination(
                            "`replay status` takes no options".into(),
                        ));
                    }
                    refuse_session_options("`replay status`", &session_options)?;
                    Ok(Action::Replay(ReplayAction::Status))
                }
                ReplayCommandLine::Stop => {
                    if self.output.is_some() || *background {
                        return Err(VshotError::InvalidDestination(
                            "`replay stop` takes no options".into(),
                        ));
                    }
                    refuse_session_options("`replay stop`", &session_options)?;
                    Ok(Action::Replay(ReplayAction::Stop))
                }
                ReplayCommandLine::Start { target } => {
                    // `--background` belongs to `start` alone; the other three
                    // shapes are one-shot control lines.
                    let target = match target {
                        ReplayTargetCommand::Monitor { name } => {
                            if name.trim().is_empty() {
                                return Err(VshotError::InvalidDestination(
                                    "monitor name cannot be empty".into(),
                                ));
                            }
                            crate::record::RecordTarget::Monitor(name.clone())
                        }
                        ReplayTargetCommand::All => crate::record::RecordTarget::All,
                        ReplayTargetCommand::Region {
                            geometry,
                            interactive: _,
                        } => {
                            let target = match geometry {
                                Some(geometry) => {
                                    crate::record::RegionTarget::Fixed(parse_geometry(geometry)?)
                                }
                                None => crate::record::RegionTarget::Pick,
                            };
                            crate::record::RecordTarget::Region(target)
                        }
                        ReplayTargetCommand::Window { name, pick } => {
                            if *pick && name.is_some() {
                                return Err(VshotError::InvalidDestination(
                                    "`replay start window` takes a window name or `--pick`, not \
                                     both"
                                        .into(),
                                ));
                            }
                            let target = if *pick {
                                crate::record::WindowTarget::Pick
                            } else {
                                match name.as_deref() {
                                    Some(filter) if !filter.trim().is_empty() => {
                                        crate::record::WindowTarget::Filter(filter.to_owned())
                                    }
                                    Some(_) => {
                                        return Err(VshotError::InvalidDestination(
                                            "the window name cannot be empty".into(),
                                        ))
                                    }
                                    None => crate::record::WindowTarget::Active,
                                }
                            };
                            crate::record::RecordTarget::Window(target)
                        }
                    };
                    let encoder = match encoder.as_deref() {
                        None => crate::record::default_replay_encoder(),
                        Some(word) => {
                            crate::record::avcodec::VideoCodec::parse(word).ok_or_else(|| {
                                VshotError::InvalidDestination(format!(
                                    "unknown encoder `{word}`: pick h264, hevc or av1"
                                ))
                            })?
                        }
                    };
                    let encoder_backend = match encoder_backend.as_deref() {
                        None => crate::record::default_replay_encoder_backend(),
                        Some(word) => crate::record::avcodec::EncoderBackend::parse(word)
                            .ok_or_else(|| {
                                VshotError::InvalidDestination(format!(
                                    "unknown encoder backend `{word}`: pick auto, vaapi, vulkan or nvenc"
                                ))
                            })?,
                    };
                    let mic = if *no_mic {
                        None
                    } else {
                        match mic {
                            Some(name) if name.trim().is_empty() => {
                                Some(crate::record::MicChoice::Default)
                            }
                            Some(name) => Some(crate::record::MicChoice::Device(name.clone())),
                            None => crate::record::default_replay_mic(),
                        }
                    };
                    // `--app-audio` needs a window to name the application, and
                    // a portal replay never learns which window it got.
                    if *app_audio && !matches!(target, crate::record::RecordTarget::Window(_)) {
                        return Err(VshotError::InvalidDestination(
                            "`--app-audio` keeps one window's application's audio, so it needs a \
                             window: `vshot replay start window --app-audio`"
                                .into(),
                        ));
                    }
                    let portal = crate::record::default_replay_portal();
                    if *app_audio && portal {
                        return Err(VshotError::InvalidDestination(
                            "`--app-audio` needs the window's process, which a portal replay does \
                             not learn; drop --portal"
                                .into(),
                        ));
                    }
                    // `--follow` needs a window to move between, and it names
                    // the windows itself, so a starting NAME would contradict
                    // the focus the same way it does on the recording side.
                    if !follow.is_empty()
                        && !matches!(target, crate::record::RecordTarget::Window(_))
                    {
                        return Err(VshotError::InvalidDestination(
                            "`--follow` moves a window replay between windows, so it needs a \
                             window: `vshot replay start window --follow NAME`"
                                .into(),
                        ));
                    }
                    // The windows to follow, on the same terms as the recording
                    // side: an explicit list wins, `--no-follow` clears it, and
                    // only a plain `replay start window` — no NAME, no `--pick`
                    // — falls back to the config, so a remembered list cannot
                    // turn a `replay start monitor` into the error `--follow`
                    // would be there.
                    let follow = if *no_follow {
                        Vec::new()
                    } else if !follow.is_empty() {
                        follow.clone()
                    } else if matches!(
                        target,
                        crate::record::RecordTarget::Window(crate::record::WindowTarget::Active)
                    ) {
                        crate::record::default_replay_follow()
                    } else {
                        Vec::new()
                    };
                    if !follow.is_empty() {
                        if let crate::record::RecordTarget::Window(window_target) = &target {
                            if !matches!(window_target, crate::record::WindowTarget::Active) {
                                return Err(VshotError::InvalidDestination(
                                    "`--follow` names the windows to move between, so it takes no \
                                     window name or `--pick`: the focus decides which of them is \
                                     replayed"
                                        .into(),
                                ));
                            }
                        }
                    }
                    // The save directory: `--save-dir` wins, and the config's
                    // `replay.save-dir` is what a bare `replay save` falls back
                    // to; both are strftime-expanded where the file is named.
                    let save_dir = save_dir
                        .clone()
                        .or_else(crate::record::default_replay_save_dir);
                    let request = crate::record::ReplayRequest {
                        target,
                        window: window.unwrap_or_else(crate::record::default_replay_window),
                        fps: fps.unwrap_or_else(crate::record::default_replay_fps),
                        encoder,
                        encoder_backend,
                        cursor: self.cursor,
                        mic,
                        app_audio: *app_audio,
                        follow: follow.clone(),
                        portal,
                        save_dir,
                        gop_secs: gop.unwrap_or_else(crate::record::default_replay_gop),
                    };
                    Ok(Action::Replay(ReplayAction::Start {
                        request: Box::new(request),
                        background: *background,
                    }))
                }
            };
        }
        if let Command::Settings = &self.command {
            // Nothing else applies: this subcommand captures nothing and has
            // no destination to argue about.
            if self.output.is_some() || self.pin || self.clipboard {
                return Err(VshotError::InvalidDestination(
                    "--output, --clipboard and --pin do not apply to the settings subcommand"
                        .into(),
                ));
            }
            return Ok(Action::Settings);
        }
        if let Command::Ocr {
            geometry,
            interactive: _,
            input,
        } = &self.command
        {
            // The text is the whole result, so the only destination that makes
            // sense is stdout or the clipboard; --output would write an image,
            // which is `vshot region`'s job, and --pin has nothing to pin.
            if self.output.is_some() || self.pin {
                return Err(VshotError::InvalidDestination(
                    "--output and --pin do not apply to the ocr subcommand; the text goes to \
                     stdout, or to the clipboard with --clipboard"
                        .into(),
                ));
            }
            let source = match (input, geometry) {
                (Some(path), _) => OcrSource::File(path.clone()),
                (None, Some(geometry)) => OcrSource::Geometry(parse_geometry(geometry)?),
                (None, None) => OcrSource::Screen,
            };
            return Ok(Action::Ocr {
                source,
                destination: if self.clipboard {
                    OcrDestination::Clipboard
                } else {
                    OcrDestination::Stdout
                },
            });
        }
        if let Command::Pin {
            files,
            toggle,
            show,
            hide,
            close_all,
            quit,
            list,
            density,
            apply,
        } = &self.command
        {
            // Under the pin subcommand `--clipboard` selects the clipboard as
            // the image source; --output and --pin still make no sense here.
            if self.output.is_some() || self.pin {
                return Err(VshotError::InvalidDestination(
                    "--output and --pin do not apply to the pin subcommand".into(),
                ));
            }
            if let Some(session) = apply {
                return Ok(Action::PinApply(session.clone()));
            }
            return Ok(Action::Pin(crate::pin::PinInvocation::build(
                files.clone(),
                self.clipboard,
                *toggle,
                *show,
                *hide,
                *close_all,
                *quit,
                *list,
                *density,
            )?));
        }
        Ok(Action::Capture(self.parse_request()?))
    }

    pub fn parse_request(self) -> Result<Request> {
        // The flag wins, then the config file, then the built-in default.
        let compression = match self.png_compression.as_deref() {
            Some(name) => PngCompression::parse(name)?,
            None => crate::config::compression_default().unwrap_or_default(),
        };
        let destination = match (self.output, self.clipboard, self.pin) {
            (Some(path), false, false) if path.as_os_str() == "-" => Destination::Stdout,
            (Some(path), false, false) => Destination::File(path),
            (None, true, false) => Destination::Clipboard,
            (None, false, true) => Destination::Pin,
            (None, false, false) => return Err(VshotError::MissingDestination),
            _ => {
                return Err(VshotError::InvalidDestination(
                    "--output, --clipboard and --pin are mutually exclusive".into(),
                ))
            }
        };
        let target = match self.command {
            Command::Region {
                geometry: Some(geometry),
                interactive: false,
            } => CaptureTarget::RegionFixed(parse_geometry(&geometry)?),
            Command::Region {
                geometry: None,
                interactive: _,
            } => CaptureTarget::RegionInteractive,
            Command::Region {
                geometry: Some(_),
                interactive: true,
            } => return Err(VshotError::ConflictingRegionSelection),
            Command::Monitor { name } => {
                // The flag wins, then the config file, then `current`.
                let name = name
                    .or_else(|| crate::config::load().monitor)
                    .unwrap_or_else(|| "current".to_owned());
                if name.trim().is_empty() {
                    return Err(VshotError::InvalidDestination(
                        "monitor name cannot be empty".into(),
                    ));
                }
                CaptureTarget::Monitor(name)
            }
            Command::All => CaptureTarget::All,
            Command::Window {
                target: WindowTarget::Active { pixel, no_blend },
            } => CaptureTarget::ActiveWindow {
                pixel_detect: pixel,
                no_blend,
            },
            Command::Window {
                target: WindowTarget::Pick { pixel, no_blend },
            } => CaptureTarget::WindowPick {
                pixel_detect: pixel,
                no_blend,
            },
            Command::Long {
                geometry,
                notches,
                max_height,
                max_frames,
                timeout,
                ignore_top,
                inject,
            } => {
                // Each value falls back flag → config file → built-in default.
                let defaults = crate::config::load().long;
                let inject = inject
                    .or(defaults.inject)
                    .unwrap_or_else(|| "auto".to_owned());
                CaptureTarget::LongShot {
                    region: geometry.as_deref().map(parse_geometry).transpose()?,
                    options: LongShotOptions {
                        notches: notches.or(defaults.notches).unwrap_or(1),
                        max_height: max_height.or(defaults.max_height).unwrap_or(30_000),
                        max_frames: max_frames.or(defaults.max_frames).unwrap_or(6_000),
                        timeout: std::time::Duration::from_secs(
                            timeout.or(defaults.timeout).unwrap_or(120),
                        ),
                        ignore_top: ignore_top.or(defaults.ignore_top).unwrap_or(0),
                    },
                    inject: Prefer::parse(&inject)?,
                }
            }
            Command::Pin { .. } => {
                return Err(VshotError::InvalidDestination(
                    "the pin subcommand is not a capture target".into(),
                ))
            }
            Command::Settings => {
                return Err(VshotError::InvalidDestination(
                    "the settings subcommand is not a capture target".into(),
                ))
            }
            Command::Ocr { .. } => {
                return Err(VshotError::InvalidDestination(
                    "the ocr subcommand is not a capture target".into(),
                ))
            }
            Command::Record { .. } => {
                return Err(VshotError::InvalidDestination(
                    "the record subcommand is not a screenshot capture target".into(),
                ))
            }
            Command::Replay { .. } => {
                return Err(VshotError::InvalidDestination(
                    "the replay subcommand is not a screenshot capture target".into(),
                ))
            }
        };
        Ok(Request {
            target,
            destination,
            cursor: self.cursor,
            compression,
        })
    }

    #[allow(dead_code)]
    pub fn try_parse_from<I, T>(args: I) -> Result<Request>
    where
        I: IntoIterator<Item = T>,
        T: Into<std::ffi::OsString> + Clone,
    {
        let cli = <Self as Parser>::try_parse_from(args).map_err(|error| {
            let message = error.to_string();
            VshotError::InvalidDestination(message)
        })?;
        cli.parse_request()
    }

    #[cfg(test)]
    pub fn try_parse_action_from<I, T>(args: I) -> Result<Action>
    where
        I: IntoIterator<Item = T>,
        T: Into<std::ffi::OsString> + Clone,
    {
        let cli = <Self as Parser>::try_parse_from(args).map_err(|error| {
            let message = error.to_string();
            VshotError::InvalidDestination(message)
        })?;
        cli.parse_action()
    }

    #[allow(dead_code)]
    pub fn command_help() -> String {
        let mut command = Self::command();
        command.render_help().to_string()
    }

    #[allow(dead_code)]
    fn _error_kind_is_available() -> ErrorKind {
        ErrorKind::InvalidValue
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn requires_destination() {
        let error = Cli::try_parse_from(["vshot", "all"]).unwrap_err();
        assert!(error.to_string().contains("no output destination"));
    }

    #[test]
    fn parses_stdout_and_fixed_region() {
        let request = Cli::try_parse_from([
            "vshot",
            "region",
            "--geometry",
            "-10,20 40x30",
            "--output",
            "-",
        ])
        .unwrap();
        assert_eq!(request.destination, Destination::Stdout);
        assert_eq!(
            request.target,
            CaptureTarget::RegionFixed(Rect::new(-10, 20, 40, 30))
        );
    }

    #[test]
    fn defaults_monitor_to_current() {
        let request = Cli::try_parse_from(["vshot", "monitor", "--output", "-"]).unwrap();
        assert_eq!(request.target, CaptureTarget::Monitor("current".into()));
    }

    /// `record monitor` without a name means `current`, and `record window`
    /// resolves its three shapes — focused, picked, named.
    #[test]
    fn record_targets_default_the_way_the_help_says() {
        let action =
            Cli::try_parse_action_from(["vshot", "record", "monitor", "--duration", "1"]).unwrap();
        let Action::Record(RecordAction::Start(request)) = action else {
            panic!("`record monitor` is a start");
        };
        assert_eq!(
            request.target,
            crate::record::RecordTarget::Monitor("current".into())
        );
        assert_eq!(request.duration, Some(1));

        let action = Cli::try_parse_action_from(["vshot", "record", "window"]).unwrap();
        let Action::Record(RecordAction::Start(request)) = action else {
            panic!("`record window` is a start");
        };
        assert_eq!(
            request.target,
            crate::record::RecordTarget::Window(crate::record::WindowTarget::Active)
        );

        let action = Cli::try_parse_action_from(["vshot", "record", "window", "--pick"]).unwrap();
        let Action::Record(RecordAction::Start(request)) = action else {
            panic!("`record window --pick` is a start");
        };
        assert_eq!(
            request.target,
            crate::record::RecordTarget::Window(crate::record::WindowTarget::Pick)
        );

        let action = Cli::try_parse_action_from(["vshot", "record", "window", "firefox"]).unwrap();
        let Action::Record(RecordAction::Start(request)) = action else {
            panic!("`record window NAME` is a start");
        };
        assert_eq!(
            request.target,
            crate::record::RecordTarget::Window(crate::record::WindowTarget::Filter(
                "firefox".into()
            ))
        );

        // One way to name the window, not two that fight.
        assert!(Cli::try_parse_action_from(["vshot", "record", "window", "x", "--pick"]).is_err());
        assert!(Cli::try_parse_action_from(["vshot", "record", "window", ""]).is_err());

        // `record region` takes a fixed rectangle or frames one; the two
        // ways to say it are exclusive, like `vshot region`'s own.
        //
        // `--no-portal` because another test in this process points
        // XDG_CONFIG_HOME at a config that remembers `portal: true`, and a
        // remembered portal refuses a region (the portal has no rectangle
        // source); this test is about the geometry, not the portal.
        let action = Cli::try_parse_action_from([
            "vshot",
            "record",
            "region",
            "--no-portal",
            "--geometry",
            "10,20 300x200",
        ])
        .unwrap();
        let Action::Record(RecordAction::Start(request)) = action else {
            panic!("`record region --geometry` is a start");
        };
        assert_eq!(
            request.target,
            crate::record::RecordTarget::Region(crate::record::RegionTarget::Fixed(
                crate::geometry::Rect::new(10, 20, 300, 200)
            ))
        );

        let action =
            Cli::try_parse_action_from(["vshot", "record", "region", "--no-portal"]).unwrap();
        let Action::Record(RecordAction::Start(request)) = action else {
            panic!("`record region` is a start");
        };
        assert_eq!(
            request.target,
            crate::record::RecordTarget::Region(crate::record::RegionTarget::Pick)
        );

        assert!(
            Cli::try_parse_action_from([
                "vshot",
                "record",
                "region",
                "--no-portal",
                "--geometry",
                "0,0 10x10",
                "--interactive"
            ])
            .is_err(),
            "--geometry and --interactive are two ways to say it, not both"
        );

        // The portal cannot hand over a rectangle of a screen, so it is
        // refused rather than recording a whole screen and calling it one.
        assert!(
            Cli::try_parse_action_from([
                "vshot",
                "record",
                "region",
                "--portal",
                "--geometry",
                "0,0 10x10"
            ])
            .is_err(),
            "--portal has no region shape"
        );

        // The microphone: a bare `--mic` is the default source, a name is
        // that input, and `--no-mic` is a silence the config cannot override.
        let action = Cli::try_parse_action_from(["vshot", "record", "monitor", "--mic"]).unwrap();
        let Action::Record(RecordAction::Start(request)) = action else {
            panic!("`record monitor --mic` is a start");
        };
        assert_eq!(
            request.mic,
            Some(crate::record::MicChoice::Default),
            "a bare --mic asks for the default source"
        );

        let action = Cli::try_parse_action_from([
            "vshot",
            "record",
            "monitor",
            "--mic",
            "alsa_input.pci-0000_2f_00.4.analog-stereo",
        ])
        .unwrap();
        let Action::Record(RecordAction::Start(request)) = action else {
            panic!("`record monitor --mic NAME` is a start");
        };
        assert_eq!(
            request.mic,
            Some(crate::record::MicChoice::Device(
                "alsa_input.pci-0000_2f_00.4.analog-stereo".into()
            ))
        );

        let action =
            Cli::try_parse_action_from(["vshot", "record", "monitor", "--no-mic"]).unwrap();
        let Action::Record(RecordAction::Start(request)) = action else {
            panic!("`record monitor --no-mic` is a start");
        };
        assert_eq!(request.mic, None, "--no-mic is a silence");

        // The two contradict each other; clap refuses the pair.
        assert!(
            Cli::try_parse_action_from(["vshot", "record", "monitor", "--mic", "--no-mic"])
                .is_err()
        );

        // `--mic` and `--app-audio` do not contradict: they are summed into
        // one track, so a window recording may carry both at once.  `--no-portal`
        // is given because a config test in another thread sets a remembered
        // `portal: true` in the process environment now and then, and a portal
        // recording refuses `--app-audio` (it never learns the window's pid).
        let action = Cli::try_parse_action_from([
            "vshot",
            "record",
            "window",
            "--no-portal",
            "--mic",
            "--app-audio",
        ])
        .unwrap();
        let Action::Record(RecordAction::Start(request)) = action else {
            panic!("`record window --mic --app-audio` is a start");
        };
        assert_eq!(request.mic, Some(crate::record::MicChoice::Default));
        assert!(request.app_audio, "both audio sources can be kept at once");

        // `record stop` takes no options, and the microphone flags are
        // options like any other.
        assert!(Cli::try_parse_action_from(["vshot", "record", "stop", "--mic"]).is_err());
        assert!(Cli::try_parse_action_from(["vshot", "record", "stop", "--no-mic"]).is_err());
    }

    /// `record mics` and `record stop` are the two shapes that record
    /// nothing: they list or they signal, and every recording option is
    /// refused by name rather than quietly ignored.
    #[test]
    fn the_read_only_record_shapes_take_no_options() {
        assert_eq!(
            Cli::try_parse_action_from(["vshot", "record", "mics"]).unwrap(),
            Action::Record(RecordAction::Mics)
        );
        assert_eq!(
            Cli::try_parse_action_from(["vshot", "record", "stop"]).unwrap(),
            Action::Record(RecordAction::Stop)
        );

        // The flags are global, so clap lets them through; the parser is
        // what says they mean nothing next to a listing.
        assert!(Cli::try_parse_action_from(["vshot", "record", "mics", "--fps", "30"]).is_err());
        assert!(Cli::try_parse_action_from(["vshot", "record", "mics", "--mic"]).is_err());
        assert!(
            Cli::try_parse_action_from(["vshot", "record", "mics", "--output", "x.mp4"]).is_err()
        );
        assert!(Cli::try_parse_action_from(["vshot", "record", "stop", "--no-portal"]).is_err());
        assert!(
            Cli::try_parse_action_from(["vshot", "record", "stop", "--encoder", "hevc"]).is_err()
        );

        // The two portal flags contradict each other, and clap is what sees
        // it — the parser only resolves their absence.
        assert!(Cli::try_parse_action_from([
            "vshot",
            "record",
            "monitor",
            "--portal",
            "--no-portal"
        ])
        .is_err());
    }

    /// The `cli.record` section is what a flag falls back to, and a flag
    /// always wins — `--no-portal` included, which is how a remembered
    /// `true` is turned off for one recording.
    #[test]
    fn the_record_section_is_what_a_flag_falls_back_to() {
        let dir = std::env::temp_dir().join("vshot-cli-record-defaults");
        let path = dir.join("vshot").join("config.json");
        std::fs::create_dir_all(path.parent().expect("a parent directory")).unwrap();
        std::fs::write(
            &path,
            r#"{"cli":{"record":{"encoder":"hevc","fps":30,"portal":true,"mic":"alsa_input.x","follow":["game","chat"]},"replay":{"follow":["game"],"save-dir":"/tmp/vshot-clips"}}}"#,
        )
        .unwrap();
        // SAFETY: the variable is put back below.  A test in another thread
        // that parses a record command while this one runs reads whatever
        // config the variable points at, and none of them asserts a
        // remembered default, so the worst a stray read can do is parse a
        // different encoder than the file this test wrote.
        let saved = std::env::var_os("XDG_CONFIG_HOME");
        std::env::set_var("XDG_CONFIG_HOME", &dir);

        let start = |argv: &[&str]| match Cli::try_parse_action_from(argv) {
            Ok(Action::Record(RecordAction::Start(request))) => request,
            other => panic!("{argv:?} is a start: {other:?}"),
        };

        // Nothing on the command line: the file decides, field by field.
        let request = start(&["vshot", "record", "monitor"]);
        assert_eq!(request.encoder, crate::record::avcodec::VideoCodec::Hevc);
        assert_eq!(request.fps, 30);
        assert!(request.portal, "the file remembers the portal");
        assert_eq!(
            request.mic,
            Some(crate::record::MicChoice::Device("alsa_input.x".into()))
        );

        // A remembered portal reaches the refusal too: one stream is what
        // the portal hands over, so "the whole desktop" has no portal shape.
        assert!(Cli::try_parse_action_from(["vshot", "record", "all"]).is_err());
        assert!(
            Cli::try_parse_action_from(["vshot", "record", "all", "--no-portal"]).is_ok(),
            "--no-portal is how the whole desktop is recorded anyway"
        );

        // Flags win over the file, one at a time and together.
        let request = start(&[
            "vshot",
            "record",
            "monitor",
            "--no-portal",
            "--fps",
            "24",
            "--encoder",
            "av1",
        ]);
        assert!(!request.portal);
        assert_eq!(request.fps, 24);
        assert_eq!(request.encoder, crate::record::avcodec::VideoCodec::Av1);
        assert_eq!(
            request.mic,
            Some(crate::record::MicChoice::Device("alsa_input.x".into())),
            "a flag nobody gave keeps the file's value"
        );

        // A bare `--mic` is the session's default source, not the name the
        // file remembers, and `--no-mic` is a silence the file cannot fill
        // back in.
        assert_eq!(
            start(&["vshot", "record", "monitor", "--mic"]).mic,
            Some(crate::record::MicChoice::Default)
        );
        assert_eq!(start(&["vshot", "record", "monitor", "--no-mic"]).mic, None);

        // A remembered `record.follow` is read only for a bare `record
        // window`: every other target ignores it, so the file cannot turn a
        // `record monitor` into the error `--follow` would be there.
        assert_eq!(
            start(&["vshot", "record", "window"]).follow,
            vec!["game".to_owned(), "chat".to_owned()]
        );
        assert!(start(&["vshot", "record", "monitor"]).follow.is_empty());
        assert!(start(&["vshot", "record", "all", "--no-portal"])
            .follow
            .is_empty());
        // A window named on the command line is not "a bare window": the
        // remembered list stays out of it.
        assert!(start(&["vshot", "record", "window", "firefox"])
            .follow
            .is_empty());
        // `--no-follow` is an explicit empty list and wins over the file; an
        // explicit `--follow` names its own windows.
        assert!(start(&["vshot", "record", "window", "--no-follow"])
            .follow
            .is_empty());
        assert_eq!(
            start(&["vshot", "record", "window", "--follow", "chat"]).follow,
            vec!["chat".to_owned()]
        );

        // The replay side reads its own `replay.follow`, and a remembered
        // `replay.save-dir` is what a save falls back to.
        let Action::Replay(ReplayAction::Start { request, .. }) =
            Cli::try_parse_action_from(["vshot", "replay", "start", "window"]).unwrap()
        else {
            panic!("`replay start window` is a start");
        };
        assert_eq!(request.follow, vec!["game".to_owned()]);
        assert_eq!(
            request.save_dir,
            Some(std::path::PathBuf::from("/tmp/vshot-clips"))
        );
        let Action::Replay(ReplayAction::Start { request, .. }) =
            Cli::try_parse_action_from(["vshot", "replay", "start", "window", "--no-follow"])
                .unwrap()
        else {
            panic!("`replay start window --no-follow` is a start");
        };
        assert!(request.follow.is_empty());
        let Action::Replay(ReplayAction::Start { request, .. }) = Cli::try_parse_action_from([
            "vshot",
            "replay",
            "start",
            "monitor",
            "--save-dir",
            "/tmp/other",
        ])
        .unwrap() else {
            panic!("`replay start monitor --save-dir` is a start");
        };
        assert_eq!(
            request.save_dir,
            Some(std::path::PathBuf::from("/tmp/other"))
        );

        match saved {
            Some(value) => std::env::set_var("XDG_CONFIG_HOME", value),
            None => std::env::remove_var("XDG_CONFIG_HOME"),
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// `--follow` and `--no-follow` contradict each other; clap is what sees
    /// it, on both subcommands.
    #[test]
    fn follow_and_no_follow_are_mutually_exclusive() {
        assert!(Cli::try_parse_action_from([
            "vshot",
            "record",
            "window",
            "--follow",
            "game",
            "--no-follow"
        ])
        .is_err());
        assert!(Cli::try_parse_action_from([
            "vshot",
            "replay",
            "start",
            "window",
            "--follow",
            "game",
            "--no-follow"
        ])
        .is_err());
        // They are options like any other: `record stop` refuses them too.
        assert!(Cli::try_parse_action_from(["vshot", "record", "stop", "--no-follow"]).is_err());
    }

    /// The one-line replay control shapes ask a session that is already
    /// running, so a rate, a codec or a microphone given to one of them has
    /// nowhere to go.  Refused, because accepting one silently reads as if it
    /// had been applied.
    #[test]
    fn a_control_line_refuses_the_options_that_shape_a_session() {
        for shape in [vec!["status"], vec!["stop"]] {
            let mut args = vec!["vshot", "replay"];
            args.extend(shape.iter().copied());
            args.extend(["--fps", "60"]);
            let error = Cli::try_parse_action_from(args).unwrap_err();
            assert!(error.to_string().contains("--fps"), "{error}");
        }
        // `--save-dir` is the one such option a save can use — the session
        // resolves the path, so the directory travels with the request — and
        // the two shapes that cannot are the ones that refuse it.
        let Action::Replay(ReplayAction::Save { save_dir, .. }) =
            Cli::try_parse_action_from(["vshot", "replay", "save", "--save-dir", "/tmp/clips"])
                .unwrap()
        else {
            panic!("`replay save --save-dir` is a save");
        };
        assert_eq!(save_dir, Some(std::path::PathBuf::from("/tmp/clips")));
        let error =
            Cli::try_parse_action_from(["vshot", "replay", "stop", "--save-dir", "/tmp/clips"])
                .unwrap_err();
        assert!(error.to_string().contains("--save-dir"), "{error}");
        // A control line with nothing on it is still fine.
        assert_eq!(
            Cli::try_parse_action_from(["vshot", "replay", "status"]).unwrap(),
            Action::Replay(ReplayAction::Status)
        );
        assert_eq!(
            Cli::try_parse_action_from(["vshot", "replay", "stop"]).unwrap(),
            Action::Replay(ReplayAction::Stop)
        );
    }

    #[test]
    fn rejects_conflicting_destinations() {
        let error = Cli::try_parse_from(["vshot", "all", "--output", "out.png", "--clipboard"])
            .unwrap_err();
        assert!(
            error.to_string().contains("mutually exclusive")
                || error.to_string().contains("cannot be used")
        );
    }

    #[test]
    fn window_active_parses_the_pixel_flag() {
        let action =
            Cli::try_parse_action_from(["vshot", "window", "active", "--clipboard"]).unwrap();
        assert_eq!(
            action,
            Action::Capture(Request {
                target: CaptureTarget::ActiveWindow {
                    pixel_detect: false,
                    no_blend: false,
                },
                destination: Destination::Clipboard,
                cursor: false,
                compression: PngCompression::Fast,
            })
        );
        let action =
            Cli::try_parse_action_from(["vshot", "window", "active", "--pixel", "--clipboard"])
                .unwrap();
        assert!(matches!(
            action,
            Action::Capture(Request {
                target: CaptureTarget::ActiveWindow {
                    pixel_detect: true,
                    ..
                },
                ..
            })
        ));
    }

    #[test]
    fn window_active_parses_the_no_blend_flag() {
        let action =
            Cli::try_parse_action_from(["vshot", "window", "active", "--no-blend", "--clipboard"])
                .unwrap();
        assert!(matches!(
            action,
            Action::Capture(Request {
                target: CaptureTarget::ActiveWindow {
                    pixel_detect: false,
                    no_blend: true,
                },
                ..
            })
        ));
        // The two choose different boxes of pixels, so asking for both is a
        // contradiction rather than one winning.
        assert!(Cli::try_parse_action_from([
            "vshot",
            "window",
            "active",
            "--no-blend",
            "--pixel",
            "--clipboard"
        ])
        .is_err());
    }

    #[test]
    fn window_pick_parses_and_takes_a_destination() {
        let action =
            Cli::try_parse_action_from(["vshot", "window", "pick", "--clipboard"]).unwrap();
        assert!(matches!(
            action,
            Action::Capture(Request {
                target: CaptureTarget::WindowPick {
                    pixel_detect: false,
                    no_blend: false,
                },
                destination: Destination::Clipboard,
                ..
            })
        ));
        let action =
            Cli::try_parse_action_from(["vshot", "window", "pick", "--pixel", "--output", "-"])
                .unwrap();
        assert!(matches!(
            action,
            Action::Capture(Request {
                target: CaptureTarget::WindowPick {
                    pixel_detect: true,
                    ..
                },
                destination: Destination::Stdout,
                ..
            })
        ));
    }

    #[test]
    fn pin_capture_destination_parses() {
        let request =
            Cli::try_parse_from(["vshot", "region", "--geometry", "0,0 20x20", "--pin"]).unwrap();
        assert_eq!(request.destination, Destination::Pin);
    }

    #[test]
    fn pin_subcommand_maps_control_flags() {
        let action = Cli::try_parse_action_from(["vshot", "pin", "--toggle"]).unwrap();
        assert_eq!(
            action,
            Action::Pin(crate::pin::PinInvocation {
                files: Vec::new(),
                clipboard: false,
                command: Some(crate::pin::PinCommand::Toggle),
                density: None,
            })
        );
        let action = Cli::try_parse_action_from(["vshot", "pin", "a.png", "b.png"]).unwrap();
        match action {
            Action::Pin(invocation) => {
                assert_eq!(invocation.command, None);
                assert_eq!(
                    invocation.files,
                    vec![PathBuf::from("a.png"), PathBuf::from("b.png")]
                );
            }
            other => panic!("expected a pin action, got {other:?}"),
        }
    }

    #[test]
    fn pin_subcommand_accepts_clipboard_source() {
        let action = Cli::try_parse_action_from(["vshot", "pin", "--clipboard"]).unwrap();
        assert_eq!(
            action,
            Action::Pin(crate::pin::PinInvocation {
                files: Vec::new(),
                clipboard: true,
                command: None,
                density: None,
            })
        );
        let action = Cli::try_parse_action_from(["vshot", "pin", "--clipboard", "a.png"]).unwrap();
        match action {
            Action::Pin(invocation) => {
                assert!(invocation.clipboard);
                assert_eq!(invocation.files, vec![PathBuf::from("a.png")]);
            }
            other => panic!("expected a pin action, got {other:?}"),
        }
    }

    #[test]
    fn pin_subcommand_rejects_bad_combinations() {
        let error = Cli::try_parse_action_from(["vshot", "pin", "--toggle", "--hide"]).unwrap_err();
        assert!(error.to_string().contains("mutually exclusive"), "{error}");
        let error = Cli::try_parse_action_from(["vshot", "pin", "a.png", "--quit"]).unwrap_err();
        assert!(error.to_string().contains("control flag"), "{error}");
        let error =
            Cli::try_parse_action_from(["vshot", "pin", "--clipboard", "--quit"]).unwrap_err();
        assert!(error.to_string().contains("control flag"), "{error}");
        let error = Cli::try_parse_action_from(["vshot", "pin"]).unwrap_err();
        assert!(error.to_string().contains("--clipboard"), "{error}");
        let error =
            Cli::try_parse_action_from(["vshot", "pin", "a.png", "--output", "b.png"]).unwrap_err();
        assert!(error.to_string().contains("do not apply"), "{error}");
    }

    #[test]
    fn long_shot_parses_its_options() {
        let action = Cli::try_parse_action_from([
            "vshot",
            "long",
            "--geometry",
            "10,20 300x400",
            "--notches",
            "2",
            "--max-height",
            "5000",
            "--ignore-top",
            "24",
            "--inject",
            "wlr",
            "--clipboard",
        ])
        .unwrap();
        match action {
            Action::Capture(Request {
                target:
                    CaptureTarget::LongShot {
                        region,
                        options,
                        inject,
                    },
                ..
            }) => {
                assert_eq!(region, Some(Rect::new(10, 20, 300, 400)));
                assert_eq!(options.notches, 2);
                assert_eq!(options.max_height, 5000);
                assert_eq!(options.ignore_top, 24);
                assert_eq!(inject, Prefer::Wlr);
            }
            other => panic!("expected a scrolling capture, got {other:?}"),
        }
    }

    #[test]
    fn long_shot_defaults_to_picking_a_region_with_the_auto_backend() {
        let action = Cli::try_parse_action_from(["vshot", "long", "--output", "-"]).unwrap();
        match action {
            Action::Capture(Request {
                target:
                    CaptureTarget::LongShot {
                        region,
                        options,
                        inject,
                    },
                ..
            }) => {
                assert_eq!(region, None);
                assert_eq!(options, LongShotOptions::default());
                assert_eq!(inject, Prefer::Auto);
            }
            other => panic!("expected a scrolling capture, got {other:?}"),
        }
    }

    #[test]
    fn long_shot_rejects_an_unknown_wheel_backend() {
        let error =
            Cli::try_parse_action_from(["vshot", "long", "--inject", "mouse", "--clipboard"])
                .unwrap_err();
        assert!(
            error.to_string().contains("auto, wlr, portal, uinput"),
            "{error}"
        );
    }
}

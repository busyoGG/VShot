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
    after_help = r#"vshot freezes the desktop once and captures from that still frame, so nothing on screen
moves while a selection is being made.

Capture targets
  region              a rectangle dragged out in the frozen scene, or a fixed --geometry
  monitor [NAME]      one output by name, or the output under the pointer with `current`
  all                 every output, composed at its logical position
  window active       the focused window: compositor metadata, or pixels with --pixel
  window pick         the window you click, on a live desktop with the others dimmed
  long                a scrolling region: vshot scrolls it, grabs frames while it moves and
                      stitches them into one tall image
  record monitor|all|region|window
                      record to an MP4 on the GPU: a screen, the desktop, a rectangle of one
                      screen, or one window's own pixels (see below)
  record mics|stop    the audio inputs a recording could take, and the signal that ends one
  replay start|save|status|stop
                      keep the last stretch of the screen in memory and copy it to an MP4 on
                      demand (a stream copy, no re-encode)

Destination (every capture above goes to exactly one)
  -o, --output PATH   a PNG at PATH, with strftime expanded (shots/%Y%m%d-%H%M%S.png); the
                      file's URI is copied to the clipboard afterwards. `-` writes the PNG
                      to stdout and copies nothing.
  --clipboard         the PNG, copied to the clipboard
  --pin               the image pinned on screen, by the resident pin daemon

Shared modifiers
  -c, --cursor        draw the compositor cursor into the capture
  --png-compression   none | fastest | fast (default) | balanced | high, all lossless

Compositors: wlroots sessions (Hyprland, Sway, labwc, niri) are captured through
wlr-screencopy; KWin/Plasma through its own org.kde.KWin.ScreenShot2, which KWin grants only
to a client whose installed desktop file declares
`X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2` -- the package installs one for
/usr/bin/vshot, so a build run straight out of target/ cannot capture under Plasma. The
focused window's own pixels come from KWin's and niri's screenshot calls where those exist;
the rectangle routes below them read Hyprland, Sway or KWin metadata. niri reports no position
for a tiled window over IPC, so `window active` and `window pick` there go through niri's own
screenshot (and, for picking, niri's own crosshair); `--pixel` asks for the pixel path instead.

`vshot pin` captures nothing: it drives the resident pin daemon. It pins image files, or with
--clipboard whatever the clipboard holds -- a color, pinned as a card carrying the same color in
hex, RGB, HSL, HSV and CMYK; an image; or text rendered as a card that keeps its HTML, markdown
or code formatting. A pinned image is dragged to move, zoomed about its
centre with the wheel, closed with a double-click; the pin under the pointer carries the
black outline, and Space with the pointer on a pin opens the same annotator as
`vshot region`. A pin covers every output it overlaps, so it can be dragged from one
monitor onto another.

On-screen selection, picking and the pin editor run as a Qt helper, `vshot-qt-ui`;
VSHOT_QT_HELPER points at another copy of it. Other environment variables: VSHOT_LANG (UI
language), VSHOT_PIXEL_DEBUG=1 (what window detection saw), VSHOT_SESSION_DEBUG=1 (which
compositor the session was read as, and on what evidence), VSHOT_LONG_DEBUG_DIR=<dir> (every
scrolling frame and stitching decision), VSHOT_PIN_FOCUS_DEBUG=1 (every time a pin surface is
handed the keyboard or gives it back), VSHOT_PIN_SOCKET, VSHOT_PIN_DENSITY=N.

$XDG_CONFIG_HOME/vshot/config.json (or ~/.config/vshot/config.json) is optional and remembers
things between runs: the `editor` section is the annotation editor's own style, written back
when a session ends, and the `cli` section supplies defaults for flags not given here -- a
command-line flag always wins over it. An unreadable or malformed file falls back to the
built-in defaults.

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
    /// Write the result to PATH, expanding strftime formats such as
    /// `%Y%m%d`. For a capture this is PNG bytes, and the file URI is copied
    /// to the Wayland clipboard afterwards; `-` writes the PNG to stdout.
    /// For `record` it is the video file: `-` is refused there, no URI is
    /// copied, and an `.mp4` suffix is added when PATH has none.
    #[arg(
        short = 'o',
        long = "output",
        global = true,
        conflicts_with_all = ["clipboard", "pin"]
    )]
    pub output: Option<PathBuf>,
    /// Copy the result to the Wayland clipboard: PNG bytes for a capture, the
    /// recognized text for `vshot ocr`.
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
    /// clipboard: `none`, `fastest`, `fast` (the default), `balanced` or
    /// `high`. Every level is lossless; the slower ones buy a smaller file.
    /// `fast` and `fastest` use fdeflate and encode a 4K frame in tens of
    /// milliseconds, while `balanced` (the DEFLATE level most PNG writers
    /// default to) and `high` can take a second or more. Pinning an image
    /// onto the screen writes nothing to disk, so this does not apply to
    /// `--pin`.
    #[arg(long = "png-compression", global = true, value_name = "LEVEL")]
    pub png_compression: Option<String>,
}

#[derive(Debug, Subcommand)]
pub enum Command {
    /// Select a region from the frozen desktop, optionally using fixed geometry.
    #[command(
        after_help = "Without --geometry the frozen scene is handed to the Qt overlay: drag a \
rectangle, adjust it with the eight handles or the arrow keys (hold Shift for 10px steps) \
while a magnifier and a size readout follow the pointer, then confirm with Enter, a \
double-click inside the selection or the toolbar's OK. Esc or a right-click cancels the \
whole capture, except that Esc inside a text box only closes that box. The toolbar \
annotates the frame with Rect, Ellipse, Arrow, Draw, Text and Mosaic: Ctrl+Z / Ctrl+Y undo \
and redo, Delete removes the selected annotation, and Ctrl+V pastes an image from the \
clipboard while the toolbar's Image button picks one from disk. A pasted image lands centred \
on the selection, shrunk to fit when it is larger, and comes up selected so its handles \
resize it. The final PNG is re-rendered from the \
annotations, so it matches the preview.

VSHOT_QT_HELPER overrides which vshot-qt-ui is run, VSHOT_LANG its language (a value \
starting with `zh` selects Chinese, any other non-empty value English; the default follows \
the system locale)."
    )]
    Region {
        /// Fixed global geometry in `x,y widthxheight` form.
        #[arg(long, conflicts_with = "interactive", allow_hyphen_values = true)]
        geometry: Option<String>,
        /// Explicitly request pointer-driven selection. This is the default when geometry is omitted.
        #[arg(long, conflicts_with = "geometry")]
        interactive: bool,
    },
    /// Capture one monitor by name, or the monitor under the pointer with `current`.
    #[command(
        after_help = "The output is named from the frozen overlay's topology, so every capture \
needs an output that answers `zxdg_output_manager_v1`. `current` reads the pointer and therefore \
needs a seat with a pointer capability, which a nested or virtual KWin does not offer; naming the \
output works there."
    )]
    Monitor {
        /// Output name, or `current` for the output under the pointer.
        name: Option<String>,
    },
    /// Capture the complete frozen desktop scene.
    #[command(
        after_help = "Every output is placed at its logical position, so a multi-monitor desktop \
is composed as it is laid out and the gaps between outputs stay transparent."
    )]
    All,
    /// Capture the focused window, or pick one interactively.
    #[command(
        after_help = "`active` resolves the focused window from compositor metadata where vshot \
can query it (Hyprland, Sway, KWin/Plasma) and falls back to detecting it from the captured \
pixels; `pick` highlights the windows of the live desktop (everything else is dimmed) and \
captures the one you click. Picking re-captures the frame after the click and then hands it \
to the same editor as `vshot region`, so a picked window is annotated on the frame that was \
captured after the pick, not on the one the picking started from."
    )]
    Window {
        #[command(subcommand)]
        target: WindowTarget,
    },
    /// Capture a region that scrolls: scroll it automatically, grab it frame
    /// by frame, and stitch the frames into one tall image.
    #[command(
        after_help = "The region is picked interactively unless --geometry is given, then vshot \
scrolls it with synthetic wheel events and grabs frames while it moves, stitching them into \
one tall image. A hint bar is shown on screen for the duration: Enter, Space or a left click \
on it finishes and keeps the stitched image, Esc or a right-click cancels. The selection has \
to sit inside a single monitor. The wheel is sent every 120 ms and frames are grabbed as fast \
as the compositor will hand them over, so the page is still moving while it is captured and \
each frame overlaps the last. A capture ends when six wheels in a row move nothing (the page \
has reached its end), or at --max-height, --max-frames or --timeout; a target that ignores the \
wheel therefore ends the capture a moment after it starts.

VSHOT_LONG_DEBUG_DIR=<dir> writes every grabbed frame as grab-NNNN.png and appends each \
stitching decision to steps.log."
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
        /// headers and fixed toolbars.  Rows that never move are recognized on
        /// their own; this is for the ones that do not sit still.
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
        after_help = "Pins are owned by a resident daemon: the first `pin` starts it, and it \
exits by itself once nothing is pinned any more. On screen, a pin is dragged to move it, the \
wheel zooms about the image centre (0.1x-8x, the factor showing in the image's corner), a \
double-click closes that pin, and the pin under the pointer carries the black outline -- that \
is the one Space opens in the same editor as `vshot region`. Clicking a pin is what gives its \
output the keyboard; the outline follows the pointer rather than the keyboard, because the \
compositors in use never tell a layer surface that it has stopped being focused. A pin covers \
every output it overlaps, so it can be \
dragged from one monitor onto another. Right-clicking a pin opens its menu: a pinned color \
card offers its formats to copy, and every pin offers `Save as…`, which writes the image out \
as a PNG.

Visibility control goes to the running daemon over its socket, \
so --toggle/--show/--hide take effect at once and do not start a second daemon. Wayland \
clients cannot receive global keys, so there is no built-in hotkey: bind one in your \
compositor, e.g. Hyprland:
    bind = SUPER, P, exec, vshot pin --toggle
    bind = SUPER SHIFT, P, exec, vshot pin --close-all

VSHOT_PIN_SOCKET overrides the socket the daemon listens on, VSHOT_PIN_DENSITY=N the source \
density of every pinned image, exactly like --density. VSHOT_PIN_DEBUG=1 and \
VSHOT_PIN_FOCUS_DEBUG=1 make the daemon trace its density decisions and its surfaces' focus to \
stderr; a debug switch also keeps the daemon's stderr attached to the terminal that started it, \
so the trace is readable while the daemon lives on without it."
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
        /// for a screenshot taken on a 2x output. vshot works this out by
        /// itself from the capture, the image's own PNG density (a 96 DPI
        /// declaration is 1x), the screenshot tool's record, or the image
        /// size; this overrides all of that when the answer is wrong or
        /// unknown.
        #[arg(long, value_parser = clap::value_parser!(u32).range(1..=4))]
        density: Option<u32>,
        /// Internal: run one annotation editor for a pin-edit session JSON
        /// written by the daemon, then render the result back onto the pin.
        /// Not for interactive use.
        #[arg(long = "apply", hide = true, conflicts_with_all = ["toggle", "show", "hide", "close_all", "quit", "list", "density"])]
        apply: Option<PathBuf>,
    },

    /// Edit the remembered settings in a window: the annotation editor's style
    /// and the command-line defaults, both from the shared config file.
    #[command(
        after_help = "Opens a window over the same `$XDG_CONFIG_HOME/vshot/config.json` the \
annotation editor and the CLI already read. Saving writes the file, so the next `vshot region` \
opens with the chosen style and the next run falls back to the chosen defaults for flags that \
are not given. Nothing is captured and no compositor protocol is needed beyond showing a window, \
so this also works under a compositor vshot cannot otherwise capture."
    )]
    Settings,

    /// Record the screen to an MP4, on the GPU.
    #[command(
        after_help = "Frames are taken through the same capture backends the screenshots use \
(wlr-screencopy on wlroots sessions, KWin's ScreenShot2 on Plasma) and encoded on the GPU's \
media engine. The encoder runs on libavcodec (ffmpeg's libraries, the same route wf-recorder \
takes), loaded at run time: a machine without ffmpeg still takes screenshots, and `record` alone \
reports what is missing. `monitor [NAME]` records one output, `current` — what a bare \
`record monitor` means — asking the compositor which output you are on: the one under the \
pointer where it reports that, the focused output \
otherwise; a recording has nothing of its own on screen for the pointer to enter, so the seat \
itself cannot answer), `all` records every output composed at its logical position, and \
`region` records one rectangle of one output — dragged out on the frozen desktop, or fixed \
with --geometry — which on a wlroots session the compositor renders straight into a dma-buf \
like `monitor` does.\n\n\
--encoder picks the video codec: h264 (default), hevc or av1. All three run on the GPU's media \
engine through the same libavcodec route; whether the hardware offers one is checked when the \
recording opens, and the message names the encoder when it does not. HEVC is also the answer \
for a composed desktop wider than 4096 pixels, which this class of GPU cannot encode as H.264.\n\n\
A recording runs until it is stopped: `vshot record stop` sends the signal, or Ctrl+C in the \
terminal that started it. Either way the file is finished properly (a seekable MP4 with its \
sample table written) before the process exits. --duration SECONDS ends it by itself. \
--fps N sets the frame rate the loop aims for (1-240, default 60); each frame carries the wall \
time it was on screen, so playback follows the real pace rather than a nominal rate. \
`vshot record stop` needs no display and works from a keybinding:\n    \
bind = SUPER, R, exec, vshot record monitor current\n    \
bind = SUPER SHIFT, R, exec, vshot record stop\n\n\
The output path comes from the global -o/--output: VIDEO_PATH is strftime-expanded, and the \
default is vshot-%Y%m%d-%H%M%S.mp4 in the videos directory — $XDG_VIDEOS_DIR, else the one \
xdg-user-dirs names, else ~/Videos — which is created when it is missing; an `.mp4` suffix is \
added when the name has none. `-` (stdout) is refused — a video is not something a terminal \
carries.\n\n\
On a wlroots session whose compositor speaks linux-dmabuf, the frames go to the encoder \
without a copy through the CPU; elsewhere (and for `record all`) the software path is used. \
Either way the file looks the same.\n\n\
--portal records through the desktop portal (org.freedesktop.portal.ScreenCast) instead of the \
compositor's own capture protocols, which is the route that works on a desktop whose protocols \
vshot does not speak. The portal is not a silent fallback: the compositor shows its own picker, \
and whatever is chosen there is what gets recorded — `record monitor --portal` asks it to offer \
screens, `record window --portal` to offer windows, but a name or --pick does not decide the \
source itself. A screen cast produces a frame when the screen changes and none while it does \
not, so --fps is the rate asked of the compositor rather than the rate the file has, and a \
still screen becomes one long frame. `record all --portal` is refused: the portal hands over one \
stream, and it is the portal that chooses which screen that is. This needs libpipewire (and \
xdg-desktop-portal); VSHOT_PORTAL_SHM=1 asks for memory frames instead of dma-bufs, the fallback \
for a compositor whose buffers the encoder cannot import.\n\n\
--mic records the microphone into the same MP4: a bare `--mic` takes the session's default \
source, a name (or a node serial) records another input, and the soundtrack is AAC, encoded by \
ffmpeg's own encoder. The microphone is opened before the video encoder so its rate and channel \
count can be declared in the MP4's header, and the samples are drained once per video frame, so \
the two tracks share one clock. `--no-mic` refuses the microphone even when the config's \
`cli.record.mic` remembers one; neither flag means \"whatever the config says\", and a session \
with no default input answers with a sentence naming `wpctl status` instead of an opaque \
PipeWire error. `record mics` lists the inputs a session actually has, which is also what the \
settings window offers in its microphone row.\n\n\
The config file's `cli.record` section supplies the defaults the flags fall back to: `encoder`, \
`encoder-backend`, `fps`, `portal`, `mic`, `follow` and `notify`. A flag always wins over the \
file — `--no-portal` is how a remembered `portal: true` is turned off for one recording, and \
`--no-follow` is how a remembered `follow` list is — and a value the file gets wrong (an unknown \
encoder name, a rate outside 1-240) falls back to the built-in default rather than failing the \
recording. A remembered `follow` list is read only for a bare `record window` (no NAME, no \
`--pick`), so it cannot turn a `record monitor` into the error `--follow` would be there.\n\n\
A recording that never started leaves nothing behind, and a process killed outright leaves a \
file without its sample table (players report it as such rather than showing a wrong video).\n\n\
VSHOT_RECORD_PIDFILE overrides the pid file `stop` reads, VSHOT_RECORD_DEBUG=1 traces each \
frame's stage and the libavcodec version in use, and VSHOT_RECORD_NO_OVERLAY=1 forces the \
letterbox's fallback composition instead of the GPU overlay (a test hook for drivers that \
cannot blend)."
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
        /// Record the microphone into the MP4 beside the video. Without a
        /// name the session's default source is used; give a node name to
        /// record another input, and `record mics` lists the ones this
        /// session has. The config's `cli.record.mic` is what a recording
        /// with neither `--mic` nor `--no-mic` falls back to.
        #[arg(long, global = true, value_name = "DEVICE", num_args = 0..=1, default_missing_value = "")]
        mic: Option<String>,
        /// Do not record the microphone, even when the config remembers it.
        #[arg(long = "no-mic", global = true, conflicts_with = "mic")]
        no_mic: bool,
        /// Also record the recorded *window's own* audio: the sound the
        /// application that owns the window is playing. It may be combined
        /// with `--mic` — both are summed into the video's one audio track —
        /// or used on its own for the window's sound and nothing else. Only
        /// `record window` has a window to attach it to. The window's pid
        /// comes from the compositor (Hyprland and niri report it; KWin does
        /// not), and the sound from PipeWire.
        #[arg(long = "app-audio", global = true)]
        app_audio: bool,
        /// Follow the focus between windows while recording one of them: give
        /// the windows to follow (`--follow NAME`, repeated) and the recording
        /// moves to whichever of them the focus lands on, staying where it is
        /// while the focus is anywhere else. Only `record window` and
        /// `replay start window` have a window to move between, and `--follow`
        /// cannot be combined with a window NAME. A bare `record window` with
        /// no `--follow` at all follows the windows `cli.record.follow`
        /// remembers.
        #[arg(long = "follow", global = true, value_name = "NAME", action = clap::ArgAction::Append)]
        follow: Vec<String>,
        /// Do not follow the focus, even when the config's
        /// `cli.record.follow` remembers windows to follow: this window, the
        /// focused one, is the one recorded from beginning to end.
        #[arg(long = "no-follow", global = true, conflicts_with = "follow")]
        no_follow: bool,
        /// Video codec: h264 (default), hevc or av1; the config's
        /// `cli.record.encoder` when the flag is not given.
        #[arg(
            long,
            global = true,
            value_parser = crate::record::avcodec::VideoCodec::ALL.map(|codec| codec.word())
        )]
        encoder: Option<String>,
        /// Hardware encoder: auto (default; VAAPI where it opens, else Vulkan,
        /// else NVENC), vaapi (AMD/Intel), vulkan (either vendor, and the only
        /// zero-copy route on NVIDIA) or nvenc (NVIDIA). The config's
        /// `cli.record.encoder-backend` when the flag is not given. All three
        /// encode on the GPU's own media engine
        /// (`h264/hevc/av1_vaapi`, `_vulkan` or `_nvenc`); there is no CPU
        /// encoder here. VAAPI and Vulkan import the compositor's dma-buf, so
        /// no pixels cross the CPU; NVENC has no dma-buf import, so its frames
        /// are carried through the CPU — a higher CPU cost, with the encode
        /// itself still on the GPU.
        #[arg(
            long,
            global = true,
            value_parser = crate::record::avcodec::EncoderBackend::ALL.map(|backend| backend.word())
        )]
        encoder_backend: Option<String>,
        /// Record through the XDG desktop portal instead of the compositor's
        /// own protocols, which is also what the config's `cli.record.portal`
        /// asks for when the flag is not given. The portal shows the
        /// compositor's own picker.
        #[arg(long, global = true)]
        portal: bool,
        /// Refuse the portal, even when the config remembers it.
        #[arg(long = "no-portal", global = true, conflicts_with = "portal")]
        no_portal: bool,
    },

    /// Keep a rolling window of the screen in memory, and save it on demand.
    #[command(
        after_help = "A replay is a recording that holds its last seconds in memory instead of \
writing them to a file: the screen is encoded continuously, the packets go into a ring, and \
`vshot replay save` copies what the ring holds into an MP4 — a stream copy, no re-encode — so \
the trigger costs almost nothing and nothing is written until it is asked for. The window \
`--window` seconds wide is what a save can reach back through.\n\n\
`replay start` runs the session: it encodes until it is stopped, and serves saves meanwhile. \
`replay save` asks the running session to write a file, `replay status` prints how much history \
it holds, and `replay stop` ends it. The control channel is a socket under \
$XDG_RUNTIME_DIR, so `save` and `stop` need no display and work from a compositor keybinding:\n    \
bind = SUPER, R, exec, vshot replay start --background\n    \
bind = SUPER SHIFT, R, exec, vshot replay save\n    \
bind = SUPER ALT, R, exec, vshot replay stop\n\n\
The target is what `record` records: `monitor [NAME]` (a bare `replay monitor` means the output \
you are on), `all`, `region` (--geometry, or dragged out on the frozen desktop), or `window` \
(the focused one, a name, or `--pick`). The frames come from the same capture backends and go to \
the same GPU encoder through libavcodec, so a session that can record can replay, and the \
zero-copy dma-buf path is used wherever `record` uses it.\n\n\
The encoder runs with a bounded key-frame distance (`--gop` seconds, default 1), so the ring is \
a fraction of an all-intra stream and every GOP boundary is a place a save can start from. A \
save starts at the newest key frame at or before `now - seconds`, so it holds at least the \
seconds asked for and decodes from its first byte; asking for more than the window holds gives \
everything there is.\n\n\
`--fps` defaults to 30 for a replay (a recording defaults to 60): a replay is left running for \
long stretches, and 30 fps halves the encoder's work while motion still looks smooth. \
`--encoder` picks h264 (default), hevc or av1. `--mic` keeps the microphone in the ring beside \
the video, the same way `record --mic` records it.\n\n\
`replay save` writes to `--save-dir` (strftime-expanded, default the videos directory with a \
timestamped name) unless it is given a path: `vshot replay save /tmp/clip.mp4`. `--background` \
detaches the session from the terminal, so it outlives the shell that started it.\n\n\
The config file's `cli.replay` section supplies the defaults: `window`, `encoder`, \
`encoder-backend`, `fps`, `gop`, `mic`, `follow`, `portal`, `save-dir` and `notify`. A flag \
always wins over the file — `--no-follow` turns a remembered `follow` list off for one session, \
as `--no-mic` does a remembered microphone. A remembered `follow` list is read only for a bare \
`replay start window`, the same rule the recording side uses.\n\n\
VSHOT_REPLAY_SOCKET overrides the control socket, VSHOT_REPLAY_PIDFILE the pid file \
`replay stop` reads, and VSHOT_RECORD_DEBUG=1 traces each frame."
    )]
    Replay {
        #[command(subcommand)]
        action: ReplayCommandLine,
        /// Seconds of history to keep in memory; the config's `cli.replay.window`
        /// when the flag is not given, else 30.
        #[arg(long, global = true, value_parser = clap::value_parser!(u64).range(1..=3600))]
        window: Option<u64>,
        /// Frame rate the loop aims for, 1-240; the config's `cli.replay.fps`
        /// when the flag is not given, else 30.
        #[arg(long, global = true, value_parser = clap::value_parser!(u32).range(1..=240))]
        fps: Option<u32>,
        /// Key-frame distance in seconds (1-10); the config's `cli.replay.gop`
        /// when the flag is not given, else 1. A smaller value makes a save
        /// start closer to the requested edge, at the cost of a bigger ring.
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
        /// Hardware encoder: auto (default), vaapi, vulkan or nvenc; the
        /// config's `cli.replay.encoder-backend` when the flag is not given. As
        /// on the recording side all three encode on the GPU; VAAPI and Vulkan
        /// import the dma-buf, while NVENC's frames are carried through the CPU
        /// because it has no dma-buf import.
        #[arg(
            long,
            global = true,
            value_parser = crate::record::avcodec::EncoderBackend::ALL.map(|backend| backend.word())
        )]
        encoder_backend: Option<String>,
        /// Keep the microphone in the ring beside the video. Without a name
        /// the session's default source is used; `record mics` lists the ones
        /// this session has. The config's `cli.replay.mic` is the fallback.
        #[arg(long, global = true, value_name = "DEVICE", num_args = 0..=1, default_missing_value = "")]
        mic: Option<String>,
        /// Do not keep the microphone, even when the config remembers one.
        #[arg(long = "no-mic", global = true, conflicts_with = "mic")]
        no_mic: bool,
        /// Also keep the recorded window's own application's audio in the ring
        /// (`replay start window` only), as `record --app-audio` does. It may
        /// be combined with `--mic`: both are summed into the ring's one track.
        #[arg(long = "app-audio", global = true)]
        app_audio: bool,
        /// Follow the focus between windows while replaying one of them, as
        /// `record --follow` does: give the windows to follow (`--follow
        /// NAME`, repeated) and the ring moves to whichever of them the focus
        /// lands on. Only `replay start window` can follow, and a bare
        /// `replay start window` with no `--follow` follows the windows
        /// `cli.replay.follow` remembers.
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
        after_help = "Without --geometry the frozen scene is handed to the Qt overlay to frame \
the text, exactly as `vshot region` does but without the annotation editor: pick a rectangle, \
press Enter, and its text comes back. The text goes to stdout, or to the clipboard with \
--clipboard. --input reads an image file instead of the screen, which is also how the \
annotation editor's text tool gets its text.\n\n\
Recognition runs PaddleOCR's PP-OCR models (the ONNX conversions of them) on ONNX Runtime, on \
the CPU, in this process. The models are installed under /usr/share/vshot/models and are \
looked for beside the executable as well, so a source checkout works without installing \
anything.\n\n\
To use a GPU, point `ocr.engine` at an external program in \
$XDG_CONFIG_HOME/vshot/config.json: it is handed a PNG and writes the text on stdout, and \
vshot never links a GPU runtime itself. See the README's OCR section for the shape of that \
entry."
    )]
    Ocr {
        /// Fixed global geometry in `x,y widthxheight` form.
        #[arg(long, conflicts_with = "interactive", allow_hyphen_values = true)]
        geometry: Option<String>,
        /// Explicitly request pointer-driven selection. This is the default when geometry is omitted.
        #[arg(long, conflicts_with = "geometry")]
        interactive: bool,
        /// Read an image from this file instead of capturing the screen. This
        /// is what the annotation editor's text tool uses: it hands over the
        /// part of the frame that was framed, and gets the text back.
        #[arg(long, value_name = "PATH", conflicts_with_all = ["geometry", "interactive"])]
        input: Option<PathBuf>,
    },
}

#[derive(Debug, Subcommand)]
pub enum RecordTargetCommand {
    /// Record one output by name, or the one you are on with `current`.
    Monitor {
        /// Output name, or `current` for the output the compositor says you
        /// are on. A bare `record monitor` means `current`.
        #[arg(default_value = "current")]
        name: String,
    },
    /// Record the complete desktop: every output composed at its logical position.
    All,
    /// Record one rectangle of the screen — a region, not a whole output.
    #[command(
        after_help = "The rectangle is in desktop logical coordinates, in the same `x,y widthxheight` \
form `vshot region --geometry` takes, and it has to sit inside a single output: no one compositor \
call copies a region that spans two. Without --geometry the frozen scene is handed to the Qt \
overlay and the rectangle is dragged out there — the same picker `vshot region` shows, on the \
live desktop — and nothing is opened or written until a rectangle is confirmed, so a cancelled \
pick leaves nothing behind.\n\n\
On a wlroots session the compositor renders just that rectangle into a dma-buf, which goes to \
the encoder without a copy through the CPU, exactly like `record monitor`; the software path \
takes over when the session has no linux-dmabuf. `record region` follows the region as it \
changes, so a window dragged inside the rectangle moves with it — but the rectangle itself \
stays where it was drawn; moving the area being recorded means stopping and starting again.\n\n\
`--portal` records one stream the compositor's picker chooses, so with it the rectangle is not \
this command's own: the portal offers whole screens, and the recording is of the screen that \
was picked there. Use `record region` without --portal to record a rectangle."
    )]
    Region {
        /// Fixed global geometry in `x,y widthxheight` form; without it the
        /// rectangle is dragged out on the frozen desktop.
        #[arg(long, value_name = "GEOMETRY", allow_hyphen_values = true)]
        geometry: Option<String>,
        /// Explicitly request pointer-driven selection. This is the default
        /// when geometry is omitted.
        #[arg(long, conflicts_with = "geometry")]
        interactive: bool,
    },
    /// Record one window's own pixels — not the screen area it covers.
    #[command(
        after_help = "The compositor copies the window itself, so a window that is covered by \
another one records whole, and one that is dragged half off the screen still records whole. \
What is behind the window never appears: this is the window, not the area it sits in.\n\n\
The window is named by app id or title (the whole name first, else a case-insensitive \
substring of either), picked with `--pick`, or — with no argument — the focused one. The \
protocol this needs is `ext_image_copy_capture_v1` with the window as its source; a \
compositor without it — niri, whose capture support stops at outputs — casts the window \
through its own screen-cast service (`org.gnome.Mutter.ScreenCast`) instead, which needs no \
flag and shows no picker. `--follow` is not available on that route, because the service \
casts the window it was started on.\n\n\
A window that is resized while recording keeps recording: the new size is fitted into the \
recording's own canvas (scaled down to fit, centred, letterboxed), because one MP4 holds one \
frame size — the window was on screen the whole time, so the file is its whole history. A \
window that is closed ends the recording there: the file is finished properly and says why. A \
window whose output is off, disabled or disconnected never produces a frame at all, which is \
reported after a few seconds rather than waited on. With --portal the compositor's own picker \
chooses the window, so nothing here names it; what --portal decides is that the picker offers \
windows rather than screens."
    )]
    Window {
        /// App id or title of the window; the focused window when omitted.
        #[arg(value_name = "NAME")]
        name: Option<String>,
        /// Pick the window to record by clicking it.
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
        /// How many seconds to take from the ring; the whole window when
        /// omitted.
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
    Monitor {
        /// Output name, or `current` for the output the compositor says you
        /// are on. A bare `replay start monitor` means `current`.
        #[arg(default_value = "current")]
        name: String,
    },
    /// Replay the complete desktop: every output composed at its logical position.
    All,
    /// Replay one rectangle of the screen — a region, not a whole output.
    Region {
        /// Fixed global geometry in `x,y widthxheight` form; without it the
        /// rectangle is dragged out on the frozen desktop.
        #[arg(long, value_name = "GEOMETRY", allow_hyphen_values = true)]
        geometry: Option<String>,
        /// Explicitly request pointer-driven selection. This is the default
        /// when geometry is omitted.
        #[arg(long, conflicts_with = "geometry")]
        interactive: bool,
    },
    /// Replay one window's own pixels — not the screen area it covers.
    Window {
        /// App id or title of the window; the focused window when omitted.
        #[arg(value_name = "NAME")]
        name: Option<String>,
        /// Pick the window to replay by clicking it.
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
        after_help = "The window is taken from the compositor itself where it can draw one: KWin's \
ScreenShot2 and niri's `screenshot-window` both hand over the window's own pixels, so nothing \
has to be found in the scene. On niri a translucent window comes out with its alpha, so vshot \
locates that render on a fresh capture of the window's output and crops the screen there: the \
output is what the screen showed, background included. A match failure first renders the window \
again and compares: unchanged content means the render cannot be located at all (nearly invisible, \
off the output's edge, invisible workspace) and falls back to niri's own translucent picture, while \
changed content (a video, an animation) means the template went stale and the capture is retried with \
a fresh render (up to three attempts, render and grab only milliseconds apart). `--no-blend` skips \
that locating altogether and takes niri's render exactly as handed over — never misplaced, but a \
translucent window comes out transparent and niri's border (drawn on the tile) is missing. Otherwise the rectangle comes from compositor metadata \
(Hyprland, Sway), and --pixel skips that and reads the border stroke off the captured frame \
instead, falling back to background segmentation, which is also what happens when no window \
list is available at all. Borderless tiling with no gaps or shadows has no pixel signal and is \
reported as such rather than guessed. VSHOT_PIXEL_DEBUG=1 reports what each stage saw."
    )]
    Active {
        /// Skip compositor metadata and detect the focused window from the
        /// captured pixels (border bands, then background segmentation).
        /// For testing the detector and for compositors without metadata.
        #[arg(long)]
        pixel: bool,
        /// On niri, take its own window render exactly as it hands it over
        /// instead of compositing it onto the captured background. A
        /// translucent window then comes out with its alpha and nothing
        /// behind it, and niri's border is missing (it is drawn on the tile,
        /// not the window) — the spare route for when locating the render on
        /// the screen misbehaves.
        #[arg(long = "no-blend", conflicts_with = "pixel")]
        no_blend: bool,
    },
    /// Pick a window on screen: hover to highlight, click to capture it.
    #[command(
        after_help = "The live desktop is shown with the candidate windows highlighted, one at a \
time, and everything else dimmed; a left click captures the highlighted window, Esc or a \
right-click cancels. The frame is grabbed again after the click, so the captured window is \
the one visible then, not the one from the hover. Candidates come from the compositor's \
window list unless --pixel is given.

On niri this is niri's own picker instead: its IPC reports no position for a tiled window, so \
there is no rectangle to offer the overlay. niri draws a crosshair (no highlight) and the \
clicked window's own screenshot is captured — which also means no annotation editor afterwards; \
--pixel asks for the overlay and the pixel detection back. A translucent window comes out of \
niri with its alpha, so vshot locates that render on a fresh capture of the window's output and \
crops the screen there: the output is what the screen showed, background included. A render \
that cannot be located at all (nearly invisible, off the output's edge, invisible workspace) falls \
back to niri's own translucent picture — but changed content (a video, an animation) gets the capture \
retried with a fresh render, up to three attempts, before the same fallback. `--no-blend` skips \
that locating altogether and takes niri's render exactly as handed over — never misplaced, but a \
translucent window comes out transparent and niri's border (drawn on the tile) is missing."
    )]
    Pick {
        /// Skip the compositor's window list and take the candidates from the
        /// captured pixels (border bands, then background segmentation).
        /// For compositors without a window-list query and for testing the
        /// detector; borderless tiling with no gaps or shadows has no pixel
        /// signal and is reported as such.
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
            return match action {
                ReplayCommandLine::Save { path, seconds } => {
                    if self.output.is_some() || *background {
                        return Err(VshotError::InvalidDestination(
                            "`replay save` takes a PATH argument, not --output or --background"
                                .into(),
                        ));
                    }
                    Ok(Action::Replay(ReplayAction::Save {
                        path: path.clone(),
                        seconds: *seconds,
                    }))
                }
                ReplayCommandLine::Status => {
                    if self.output.is_some() || *background {
                        return Err(VshotError::InvalidDestination(
                            "`replay status` takes no options".into(),
                        ));
                    }
                    Ok(Action::Replay(ReplayAction::Status))
                }
                ReplayCommandLine::Stop => {
                    if self.output.is_some() || *background {
                        return Err(VshotError::InvalidDestination(
                            "`replay stop` takes no options".into(),
                        ));
                    }
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

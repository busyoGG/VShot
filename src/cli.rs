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
    /// Write PNG bytes to PATH, expanding strftime formats such as `%Y%m%d`;
    /// after writing, copy the file URI to the Wayland clipboard. Use `-`
    /// for stdout instead.
    #[arg(
        short = 'o',
        long = "output",
        global = true,
        conflicts_with_all = ["clipboard", "pin"]
    )]
    pub output: Option<PathBuf>,
    /// Copy PNG bytes to the Wayland clipboard.
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
and redo, Delete removes the selected annotation, and the final PNG is re-rendered from the \
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
dragged from one monitor onto another. Right-clicking a pinned color card copies one of its formats.

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

/// What `vshot` was asked to do: capture something to a destination, or drive
/// the pin daemon. Pin management never touches the Wayland capture path.
#[derive(Clone, Debug, PartialEq)]
pub enum Action {
    Capture(Request),
    Pin(crate::pin::PinInvocation),
    /// Internal: render one pin-edit session and write the result back.
    PinApply(std::path::PathBuf),
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
    /// Splits the CLI into the two things `vshot` can do: capture to a
    /// destination, or drive the pin daemon.
    pub fn parse_action(self) -> Result<Action> {
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

use std::path::PathBuf;

use clap::{error::ErrorKind, ArgGroup, CommandFactory, Parser, Subcommand};

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
    #[arg(
        long = "png-compression",
        global = true,
        value_name = "LEVEL",
        default_value = "fast"
    )]
    pub png_compression: String,
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
    Monitor {
        /// Output name, or `current` for the output under the pointer.
        #[arg(default_value = "current")]
        name: String,
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
        #[arg(long, default_value_t = 1)]
        notches: u32,
        /// Height limit of the stitched image, in pixels.
        #[arg(long = "max-height", default_value_t = 30_000)]
        max_height: u32,
        /// Frame limit for one capture.
        #[arg(long = "max-frames", default_value_t = 6_000)]
        max_frames: u32,
        /// Time limit for one capture, in seconds.
        #[arg(long, default_value_t = 120)]
        timeout: u64,
        /// Rows at the top of every frame left out of the match, for sticky
        /// headers and fixed toolbars.  Rows that never move are recognized on
        /// their own; this is for the ones that do not sit still.
        #[arg(long = "ignore-top", default_value_t = 0)]
        ignore_top: u32,
        /// Which wheel backend to use: `auto` (try them in order), `wlr` (the
        /// compositor's virtual pointer protocol), `portal` (the XDG
        /// RemoteDesktop portal) or `uinput` (`/dev/uinput`).
        #[arg(long, default_value = "auto")]
        inject: String,
    },

    /// Manage pinned images shown by the resident pin daemon. The global
    /// --clipboard flag switches the source: pin the clipboard image, or
    /// render clipboard text as a card (HTML, markdown, code, or plain),
    /// instead of image files.
    #[command(
        after_help = "Pins are owned by a resident daemon: the first `pin` starts it, and it \
exits by itself once nothing is pinned any more. On screen, a pin is dragged to move it, the \
wheel zooms about the image centre (0.1x-8x, the factor showing in the image's corner), a \
double-click closes that pin, and a click focuses it (bright outline) so that Space opens it \
in the same editor as `vshot region`. A pin covers every output it overlaps, so it can be \
dragged from one monitor onto another.

Visibility control goes to the running daemon over its socket, \
so --toggle/--show/--hide take effect at once and do not start a second daemon. Wayland \
clients cannot receive global keys, so there is no built-in hotkey: bind one in your \
compositor, e.g. Hyprland:
    bind = SUPER, P, exec, vshot pin --toggle
    bind = SUPER SHIFT, P, exec, vshot pin --close-all

VSHOT_PIN_SOCKET overrides the socket the daemon listens on, VSHOT_PIN_DENSITY=N the source \
density of every pinned image, exactly like --density."
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
        /// itself from the capture, the image's own PNG density, the
        /// screenshot tool's record, or the image size; this overrides all of
        /// that when the answer is wrong or unknown.
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
    /// Capture the currently focused window: compositor metadata when
    /// available, otherwise pixel detection on the captured frame.
    #[command(
        after_help = "The window is read from the compositor's metadata (Hyprland, Sway, \
KWin/Plasma); --pixel skips that and segments the captured frame instead, which is also what \
happens when no window list is available at all. Borderless tiling with no gaps or shadows \
has no pixel signal and is reported as such rather than guessed."
    )]
    Active {
        /// Skip compositor metadata and detect the focused window from the
        /// captured pixels (accent outline, then background segmentation).
        /// For testing the detector and for compositors without metadata.
        #[arg(long)]
        pixel: bool,
    },
    /// Pick a window on screen: hover to highlight, click to capture it.
    #[command(
        after_help = "The live desktop is shown with the candidate windows highlighted, one at a \
time, and everything else dimmed; a left click captures the highlighted window, Esc or a \
right-click cancels. The frame is grabbed again after the click, so the captured window is \
the one visible then, not the one from the hover. Candidates come from the compositor's \
window list unless --pixel is given."
    )]
    Pick {
        /// Skip the compositor's window list and take the candidates from the
        /// captured pixels (accent outlines, then background segmentation).
        /// For compositors without a window-list query and for testing the
        /// detector; borderless tiling with no gaps or shadows has no pixel
        /// signal and is reported as such.
        #[arg(long)]
        pixel: bool,
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
    },
    /// Interactive window picking: the compositor's window list when it has
    /// one, the captured pixels otherwise.
    WindowPick {
        /// Take the candidates from the pixels instead of the window list.
        pixel_detect: bool,
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
        let compression = PngCompression::parse(&self.png_compression)?;
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
                if name.trim().is_empty() {
                    return Err(VshotError::InvalidDestination(
                        "monitor name cannot be empty".into(),
                    ));
                }
                CaptureTarget::Monitor(name)
            }
            Command::All => CaptureTarget::All,
            Command::Window {
                target: WindowTarget::Active { pixel },
            } => CaptureTarget::ActiveWindow {
                pixel_detect: pixel,
            },
            Command::Window {
                target: WindowTarget::Pick { pixel },
            } => CaptureTarget::WindowPick {
                pixel_detect: pixel,
            },
            Command::Long {
                geometry,
                notches,
                max_height,
                max_frames,
                timeout,
                ignore_top,
                inject,
            } => CaptureTarget::LongShot {
                region: geometry.as_deref().map(parse_geometry).transpose()?,
                options: LongShotOptions {
                    notches,
                    max_height,
                    max_frames,
                    timeout: std::time::Duration::from_secs(timeout),
                    ignore_top,
                },
                inject: Prefer::parse(&inject)?,
            },
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
                    pixel_detect: false
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
                target: CaptureTarget::ActiveWindow { pixel_detect: true },
                ..
            })
        ));
    }

    #[test]
    fn window_pick_parses_and_takes_a_destination() {
        let action =
            Cli::try_parse_action_from(["vshot", "window", "pick", "--clipboard"]).unwrap();
        assert!(matches!(
            action,
            Action::Capture(Request {
                target: CaptureTarget::WindowPick {
                    pixel_detect: false
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
                target: CaptureTarget::WindowPick { pixel_detect: true },
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

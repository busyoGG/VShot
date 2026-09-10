use std::path::PathBuf;

use clap::{error::ErrorKind, ArgGroup, CommandFactory, Parser, Subcommand};

use crate::error::{Result, VshotError};
use crate::geometry::{parse_geometry, Rect};

#[derive(Debug, Parser)]
#[command(
    name = "vshot",
    version,
    about = "Strict-freeze Wayland screenshots for wlroots",
    group = ArgGroup::new("destination")
        .args(["output", "clipboard", "pin"])
)]
pub struct Cli {
    #[command(subcommand)]
    pub command: Command,
    /// Include the compositor cursor in each native screencopy capture.
    #[arg(short = 'c', long = "cursor", global = true)]
    pub cursor: bool,
    /// Write PNG bytes to PATH, or `-` for stdout.
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
}

#[derive(Debug, Subcommand)]
pub enum Command {
    /// Select a region from the frozen desktop, optionally using fixed geometry.
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
    All,
    /// Capture the active window using compositor-specific metadata.
    Window {
        #[command(subcommand)]
        target: WindowTarget,
    },
    /// Manage pinned images shown by the resident pin daemon. The global
    /// --clipboard flag switches the source: pin the clipboard image, or
    /// render clipboard text as a card (HTML, markdown, code, or plain),
    /// instead of image files.
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
        /// Internal: run one annotation editor for a pin-edit session JSON
        /// written by the daemon, then render the result back onto the pin.
        /// Not for interactive use.
        #[arg(long = "apply", hide = true, conflicts_with_all = ["toggle", "show", "hide", "close_all", "quit", "list"])]
        apply: Option<PathBuf>,
    },
}

#[derive(Debug, Subcommand)]
pub enum WindowTarget {
    /// Capture the currently focused window: compositor metadata when
    /// available, otherwise pixel detection on the captured frame.
    Active {
        /// Skip compositor metadata and detect the focused window from the
        /// captured pixels (accent outline, then background segmentation).
        /// For testing the detector and for compositors without metadata.
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

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum CaptureTarget {
    RegionFixed(Rect),
    RegionInteractive,
    Monitor(String),
    All,
    ActiveWindow {
        /// Detect the window from pixels instead of compositor metadata.
        pixel_detect: bool,
    },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Request {
    pub target: CaptureTarget,
    pub destination: Destination,
    pub cursor: bool,
}

/// What `vshot` was asked to do: capture something to a destination, or drive
/// the pin daemon. Pin management never touches the Wayland capture path.
#[derive(Clone, Debug, Eq, PartialEq)]
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
            )?));
        }
        Ok(Action::Capture(self.parse_request()?))
    }

    pub fn parse_request(self) -> Result<Request> {
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
}

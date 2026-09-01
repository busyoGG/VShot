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
        .args(["output", "clipboard"])
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
        conflicts_with = "clipboard"
    )]
    pub output: Option<PathBuf>,
    /// Copy PNG bytes to the Wayland clipboard.
    #[arg(long, global = true, conflicts_with = "output")]
    pub clipboard: bool,
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
}

#[derive(Debug, Subcommand)]
pub enum WindowTarget {
    Active,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Destination {
    File(PathBuf),
    Stdout,
    Clipboard,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum CaptureTarget {
    RegionFixed(Rect),
    RegionInteractive,
    Monitor(String),
    All,
    ActiveWindow,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Request {
    pub target: CaptureTarget,
    pub destination: Destination,
    pub cursor: bool,
}

impl Cli {
    pub fn parse_request(self) -> Result<Request> {
        let destination = match (self.output, self.clipboard) {
            (Some(path), false) if path.as_os_str() == "-" => Destination::Stdout,
            (Some(path), false) => Destination::File(path),
            (None, true) => Destination::Clipboard,
            (None, false) => return Err(VshotError::MissingDestination),
            (Some(_), true) => {
                return Err(VshotError::InvalidDestination(
                    "--output and --clipboard are mutually exclusive".into(),
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
                target: WindowTarget::Active,
            } => CaptureTarget::ActiveWindow,
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
}

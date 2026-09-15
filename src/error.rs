use std::path::PathBuf;

use thiserror::Error;

#[derive(Debug, Error)]
pub enum VshotError {
    #[error("invalid geometry: {0}")]
    InvalidGeometry(String),
    #[error("invalid output destination: {0}")]
    InvalidDestination(String),
    #[error(
        "no output destination supplied; use --output PATH, --output -, --clipboard, or --pin"
    )]
    MissingDestination,
    #[error("--geometry and interactive region selection are mutually exclusive")]
    ConflictingRegionSelection,
    #[error("failed to execute `{program}`: {source}")]
    CommandIo {
        program: String,
        #[source]
        source: std::io::Error,
    },
    #[error("failed to decode PNG from {origin}: {message}")]
    PngDecode { origin: String, message: String },
    #[error("failed to encode PNG: {0}")]
    PngEncode(String),
    #[error("failed to write {path}: {source}")]
    WriteFile {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("Wayland connection failed: {0}")]
    WaylandConnection(String),
    #[error("Wayland capability `{0}` is required for strict freeze mode")]
    MissingCapability(String),
    #[error("Wayland protocol error: {0}")]
    WaylandProtocol(String),
    /// No capture backend can serve the request.  The message is a complete
    /// sentence because both backends' diagnoses are composed into it.
    #[error("no usable screen capture backend: {0}")]
    NoCaptureBackend(String),
    #[error("KWin ScreenShot2 failed: {0}")]
    KwinScreenShot(String),
    #[error("KDE denied the screenshot: {0}")]
    ScreenshotDenied(String),
    #[error("screen capture timed out before the compositor returned a frame")]
    CaptureTimeout,
    #[error("frozen overlay was not ready before the timeout")]
    OverlayTimeout,
    #[error("current monitor could not be determined from pointer input before the timeout")]
    CurrentOutputTimeout,
    #[error("output topology is incomplete: {0}")]
    IncompleteTopology(String),
    #[error("output topology changed while taking the screenshot")]
    TopologyChanged,
    #[error("unsupported output mapping: {0}")]
    UnsupportedOutput(String),
    #[error("unsupported text: {0}")]
    UnsupportedText(String),
    #[error("active window is unavailable: {0}")]
    ActiveWindowUnavailable(String),
    /// Interactive window picking found nothing to offer: no compositor
    /// reported any window and the pixel fallback saw no window either.
    #[error("no window could be picked: {0}")]
    WindowPickUnavailable(String),
    #[error("interactive selection cancelled")]
    SelectionCancelled,
    #[error("interactive selection failed: {0}")]
    Selection(String),
    #[error("Wayland clipboard failed: {0}")]
    Clipboard(String),
    #[error("pin failed: {0}")]
    Pin(String),
    /// Scrolling capture: the frames could not be stitched, or what was asked
    /// of the stitcher makes no sense (a region too small, an ignore-top that
    /// covers the whole frame).
    #[error("long screenshot failed: {0}")]
    LongShotStitch(String),
    /// Scrolling capture: no way to drive the wheel on this compositor.
    #[error("long screenshot cannot scroll the page: {0}")]
    LongShotInjection(String),
    /// Scrolling capture: the selection is not usable for a long screenshot.
    #[error("long screenshot region is unusable: {0}")]
    LongShotRegion(String),
}

pub type Result<T> = std::result::Result<T, VshotError>;

pub mod active_output;
pub mod kwin;
pub mod window;
pub mod window_pixel;
pub mod wlr;

pub use window::{CompositorWindowProvider, ProcessWindowProvider};
pub use window_pixel::detect_active_window;
pub use wlr::WlrCapture;

use crate::error::{Result, VshotError};
use crate::model::Frame;

/// The compositor families vshot can capture from.
///
/// They are mutually exclusive in practice: wlroots and its relatives speak
/// `wlr-screencopy`, while KWin has no such protocol and only offers its own
/// D-Bus service.  Picking between them is a property of the session, so it
/// happens once at startup and the rest of the program stays backend-agnostic.
// A wlroots session is the common case and holds its capture state inline, so
// the variants differ wildly in size; one of these is built per run and never
// moved, which makes boxing the larger variant pure overhead.
#[allow(clippy::large_enum_variant)]
pub enum Capturer {
    Wlr(WlrCapture),
    Kwin(kwin::KwinCapture),
}

impl Capturer {
    /// Connects to whichever capture backend this session provides.
    ///
    /// wlroots is probed first because its protocol is the one vshot is built
    /// around, and only the absence of `zwlr_screencopy_manager_v1` means "not
    /// a wlroots compositor" rather than a broken session: any other failure
    /// there is a real error and is reported as it stands.  When neither
    /// backend works the message has to name both, otherwise the user cannot
    /// tell whether they are on an unsupported compositor or looking at a
    /// misconfigured one.
    pub fn connect() -> Result<Self> {
        match WlrCapture::connect() {
            Ok(capture) => Ok(Self::Wlr(capture)),
            Err(error) if is_missing_screencopy(&error) => match kwin::KwinCapture::connect() {
                Ok(capture) => Ok(Self::Kwin(capture)),
                Err(kwin_error) => Err(no_capture_backend(&kwin_error)),
            },
            Err(error) => Err(error),
        }
    }

    pub fn capture_output(&mut self, name: &str, cursor: bool) -> Result<Frame> {
        match self {
            Self::Wlr(capture) => capture.capture_output(name, cursor),
            Self::Kwin(capture) => capture.capture_output(name, cursor),
        }
    }
}

/// Does this failure mean "this compositor does not speak wlr-screencopy"?  It
/// is the one wlroots error that justifies trying somewhere else.
fn is_missing_screencopy(error: &VshotError) -> bool {
    matches!(
        error,
        VshotError::MissingCapability(capability) if capability == "zwlr_screencopy_manager_v1"
    )
}

/// Neither backend works, so the message has to carry both diagnoses: the
/// user needs to be able to tell an unsupported compositor from a broken
/// session.
fn no_capture_backend(kwin_error: &VshotError) -> VshotError {
    VshotError::NoCaptureBackend(format!(
        "the compositor provides no wlr-screencopy (`zwlr_screencopy_manager_v1`) \
         and the KWin ScreenShot2 fallback failed: {kwin_error}"
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_a_missing_screencopy_protocol_lets_another_backend_try() {
        assert!(is_missing_screencopy(&VshotError::MissingCapability(
            "zwlr_screencopy_manager_v1".into()
        )));
        // A wlroots session that is merely broken has to report its own
        // problem rather than being blamed on KWin.
        for unrelated in [
            VshotError::MissingCapability("wl_shm".into()),
            VshotError::MissingCapability("at least one wl_output".into()),
            VshotError::WaylandConnection("no display".into()),
            VshotError::CaptureTimeout,
        ] {
            assert!(
                !is_missing_screencopy(&unrelated),
                "{unrelated} must not select the KWin fallback"
            );
        }
    }

    #[test]
    fn a_dead_end_names_both_backends() {
        let error = no_capture_backend(&VshotError::KwinScreenShot("no KWin here".into()));
        let VshotError::NoCaptureBackend(message) = error else {
            panic!("the both-backends-failed case has its own error");
        };
        assert!(message.contains("zwlr_screencopy"), "{message}");
        assert!(message.contains("KWin ScreenShot2"), "{message}");
        assert!(message.contains("no KWin here"), "{message}");
    }

    /// Exercises the real selection against a KWin session, which is the
    /// session shape the fallback exists for: it has `wl_shm` and outputs but
    /// no screencopy protocol at all.  Ignored by default because it needs
    /// both a running KWin on the session bus and its Wayland socket; the
    /// setup is described on `kwin::tests::captures_a_live_kwin_screen`.
    #[test]
    #[ignore = "needs a running KWin session, see the setup on the KWin capture test"]
    fn a_kwin_session_selects_the_kwin_backend() {
        let missing = WlrCapture::connect()
            .map(|_| ())
            .map_err(|error| error.to_string());
        let message = missing.expect_err("KWin provides no wlr-screencopy");
        assert!(message.contains("zwlr_screencopy_manager_v1"), "{message}");
        match Capturer::connect() {
            Ok(Capturer::Kwin(_)) => {}
            // Without a screencopy protocol the wlroots backend cannot have
            // been the one that succeeded.
            Ok(Capturer::Wlr(_)) => panic!("the fallback must not select wlroots here"),
            Err(error) => panic!("the fallback has to reach KWin's D-Bus service: {error}"),
        }
    }
}

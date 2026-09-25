// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

pub mod active_output;
pub mod dmabuf;
pub mod hypr_cursor;
pub mod kwin;
pub mod niri;
pub mod window;
pub mod window_blend;
pub mod window_copy;
pub mod window_pid;
pub mod window_pixel;
pub mod wlr;

pub use window::{CompositorWindowProvider, ProcessWindowProvider, WindowCandidate};
pub use window_pixel::{detect_active_window, detect_window_candidates};
pub use wlr::WlrCapture;

use crate::error::{Result, VshotError};
use crate::geometry::Rect;
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

    /// Captures one output straight into a dma-buf, for the recording
    /// loop's zero-copy path.  `Ok(None)` means this backend or session
    /// cannot do it (KWin, a wlroots compositor without linux-dmabuf) and
    /// the caller should take the software path; an `Err` is a capture
    /// that should have worked and did not.
    ///
    /// KWin is deliberately `None` rather than an error: its screenshots
    /// arrive over D-Bus as PNG data, with no buffer to hand over.
    pub fn capture_output_dmabuf(
        &mut self,
        name: &str,
        cursor: bool,
    ) -> Result<Option<crate::capture::dmabuf::DmabufFrame>> {
        match self {
            Self::Wlr(capture) => match capture.capture_output_dmabuf(name, cursor) {
                Ok(frame) => Ok(Some(frame)),
                // "This session cannot" is a fallback, not a failure: the
                // caller records through the software path instead.
                Err(VshotError::MissingCapability(_)) | Err(VshotError::UnsupportedOutput(_)) => {
                    Ok(None)
                }
                Err(error) => Err(error),
            },
            Self::Kwin(_) => Ok(None),
        }
    }

    /// Captures one rectangle of an output straight into a dma-buf — the
    /// zero-copy path of `record region`.  `Ok(None)` means this backend
    /// cannot do it and the caller takes the software path, exactly like
    /// [`Capturer::capture_output_dmabuf`].
    pub fn capture_region_dmabuf(
        &mut self,
        name: &str,
        region: Rect,
        cursor: bool,
    ) -> Result<Option<crate::capture::dmabuf::DmabufFrame>> {
        match self {
            Self::Wlr(capture) => match capture.capture_region_dmabuf(name, region, cursor) {
                Ok(frame) => Ok(Some(frame)),
                Err(VshotError::MissingCapability(_)) | Err(VshotError::UnsupportedOutput(_)) => {
                    Ok(None)
                }
                Err(error) => Err(error),
            },
            Self::Kwin(_) => Ok(None),
        }
    }

    /// Asks the compositor what dma-buf it would offer for an output, by
    /// running one plain shm capture and reading the offer off it.  The
    /// recorder uses the answer to build its buffer pool before the loop
    /// starts.
    ///
    /// `region` narrows the question to one rectangle of the output: the
    /// offer a region capture carries is the region's own pixel size, which
    /// is what a pool for it must be built for.
    pub fn probe_dmabuf_offer_region(
        &mut self,
        name: &str,
        region: Option<Rect>,
    ) -> Result<Option<(u32, u32, u32, bool)>> {
        match self {
            Self::Wlr(capture) => match capture.probe_dmabuf_offer_region(name, region) {
                Ok(offer) => Ok(Some(offer)),
                Err(VshotError::MissingCapability(_)) | Err(VshotError::UnsupportedOutput(_)) => {
                    Ok(None)
                }
                Err(error) => Err(error),
            },
            Self::Kwin(_) => Ok(None),
        }
    }

    /// Builds the zero-copy buffer pool for one output shape.
    pub fn build_dmabuf_pool(&mut self, width: u32, height: u32, fourcc: u32) -> Result<()> {
        match self {
            Self::Wlr(capture) => capture.build_dmabuf_pool(width, height, fourcc),
            Self::Kwin(_) => Ok(()),
        }
    }

    /// The focused window's own pixels, when the backend can hand them over
    /// directly, together with the density they are meant to be shown at.
    ///
    /// KWin screenshots the window itself on request, decorations included; a
    /// wlroots compositor has no such call — its protocols describe outputs,
    /// not windows — so `None` means "this backend cannot answer that" and the
    /// caller has to find the window in the scene instead.
    pub fn capture_active_window(&mut self, cursor: bool) -> Option<Result<(Frame, u32)>> {
        match self {
            Self::Kwin(capture) => Some(capture.capture_active_window(cursor)),
            Self::Wlr(_) => None,
        }
    }

    /// Captures one rectangle of an output, given in output-local logical
    /// coordinates.
    ///
    /// wlroots copies just that rectangle, which is what makes a scrolled
    /// capture cheap enough to follow a page that is still moving.  KWin's
    /// ScreenShot2 does have an area method, but it is private API with no
    /// version to negotiate and its argument order could not be checked against
    /// a running KWin from here, so that backend copies the whole screen and
    /// cuts the rectangle out of it — the same pixels, only slower.  `scale` is
    /// what that cutting needs.
    pub fn capture_region(
        &mut self,
        name: &str,
        region: Rect,
        scale: u32,
        cursor: bool,
    ) -> Result<Frame> {
        match self {
            Self::Wlr(capture) => capture.capture_region(name, region, cursor),
            Self::Kwin(capture) => {
                let full = capture.capture_output(name, cursor)?;
                full.crop(scaled_region(region, scale))
            }
        }
    }
}

/// A logical rectangle inside an output, as the rectangle of that output's
/// pixels the full-screen backends have to cut out: a logical pixel covers
/// `scale` device pixels.  A scale of zero counts as one, so a nonsensical
/// output description cannot collapse the rectangle to nothing.
fn scaled_region(region: Rect, scale: u32) -> Rect {
    let scale = scale.max(1);
    let origin_scale = i32::try_from(scale).unwrap_or(i32::MAX);
    Rect::new(
        region.origin.x.saturating_mul(origin_scale),
        region.origin.y.saturating_mul(origin_scale),
        region.size.width.saturating_mul(scale),
        region.size.height.saturating_mul(scale),
    )
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
         and the KWin ScreenShot2 fallback failed: {kwin_error}; on a session like this one, \
         `vshot record --portal` is the recording route that may still work"
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
    fn the_fallback_region_is_cut_in_output_pixels() {
        // A logical rectangle relative to its output's corner, on a 2x output.
        let region = Rect::new(10, 20, 100, 50);
        assert_eq!(scaled_region(region, 2), Rect::new(20, 40, 200, 100));
        // A scale of zero is not a scale: treating it as one keeps the
        // rectangle instead of collapsing it to nothing.
        assert_eq!(scaled_region(region, 0), region);
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

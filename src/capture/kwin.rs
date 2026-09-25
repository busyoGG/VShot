// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Screen capture through KWin's private D-Bus screenshot service.
//!
//! KWin — including Plasma on Wayland — provides neither
//! `zwlr_screencopy_manager_v1` nor `ext_image_copy_capture`, so the wlroots
//! path cannot work on KDE at all.  What it does provide is
//! `org.kde.KWin.ScreenShot2` on the session bus: the caller passes in the
//! write end of a pipe, the compositor renders into it, and the reply carries
//! a variant map describing the pixels.
//!
//! That service is private API with no negotiated version, so everything here
//! is derived from what KWin 6.7.5 actually does: the results hold `width`,
//! `height`, `stride`, `format`, `scale` and `type`, `format` is
//! `QImage::Format_ARGB32_Premultiplied`, `type` is `raw`, and exactly
//! `stride * height` bytes of premultiplied BGRA arrive on the pipe.  Values
//! that are not needed are ignored rather than rejected, and the results this
//! module does need are validated instead of assumed.

use std::collections::HashMap;
use std::fs::File;
use std::io::Read;
use std::os::fd::OwnedFd;

use zbus::blocking::Connection;
use zbus::zvariant::{Fd, Value};

use crate::error::{Result, VshotError};
use crate::geometry::Size;
use crate::model::Frame;

const SERVICE: &str = "org.kde.KWin";
const PATH: &str = "/org/kde/KWin/ScreenShot2";
const INTERFACE: &str = "org.kde.KWin.ScreenShot2";

/// `QImage::Format_ARGB32_Premultiplied`, the only format ScreenShot2 has been
/// observed to send.  Anything else would have to be decoded differently, so it
/// is refused rather than guessed at.
const FORMAT_ARGB32_PREMULTIPLIED: u32 = 6;

/// The pixels' layout as the compositor announced it.  `stride` may be wider
/// than a row of pixels; the padding is dropped during conversion.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct CaptureGeometry {
    width: u32,
    height: u32,
    stride: usize,
}

/// What the payload's alpha channel means, which is what decides whether the
/// frame that comes out can still be see-through.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Alpha {
    /// The pixels cover the whole frame by definition, so the alpha is dropped:
    /// an output is opaque everywhere, including the regions nothing was drawn
    /// into.
    Flatten,
    /// The alpha is the coverage of what was drawn, so it is kept: a window
    /// capture is a picture of a window on a transparent backdrop, and the
    /// pixels the window itself does not cover — its shadow, its rounded
    /// corners — have to stay transparent.  Flattening them instead restores
    /// semi-transparent shadow over black, which reads as a black fringe around
    /// the window.
    Keep,
}

/// What a capture is aimed at.  Both of ScreenShot2's calls take options plus a
/// pipe and differ only in how the target is named, so they share one path
/// through the protocol — and one place that says what a failure was about.
enum Shot<'a> {
    /// One output, under the name KWin knows it by.
    Output { name: &'a str, cursor: bool },
    /// The focused window, decorations included.
    ActiveWindow { cursor: bool },
    /// One named window, decorations included.
    ///
    /// `handle` is the window's internal id as KWin reports it — `internalId`
    /// in the scripting API, a `QUuid` string.  It is the one description of a
    /// window that does not depend on the window having the focus or on where
    /// it currently is, which is exactly what recording a window needs: the
    /// focus moves and the geometry changes while the recording runs, and the
    /// file has to keep showing the same window.
    Window { handle: &'a str, cursor: bool },
}

impl Shot<'_> {
    /// How this capture is described inside an error message.
    fn target(&self) -> String {
        match self {
            Self::Output { name, .. } => format!("the screen `{name}`"),
            Self::ActiveWindow { .. } => "the focused window".to_string(),
            Self::Window { handle, .. } => format!("the window `{handle}`"),
        }
    }
}

pub struct KwinCapture {
    connection: Connection,
}

impl KwinCapture {
    /// Connects to the session bus and verifies that KWin's screenshot service
    /// is really there, so a session that has no such service is reported as
    /// such instead of failing later inside a capture.
    pub fn connect() -> Result<Self> {
        let connection = Connection::session().map_err(|error| {
            VshotError::KwinScreenShot(format!("cannot reach the session bus: {error}"))
        })?;
        let has_owner: bool = connection
            .call_method(
                Some("org.freedesktop.DBus"),
                "/org/freedesktop/DBus",
                Some("org.freedesktop.DBus"),
                "NameHasOwner",
                &(SERVICE),
            )
            .map_err(|error| {
                VshotError::KwinScreenShot(format!(
                    "cannot ask the session bus whether {SERVICE} is running: {error}"
                ))
            })?
            .body()
            .deserialize()
            .map_err(|error| {
                VshotError::KwinScreenShot(format!(
                    "the session bus answered NameHasOwner with something unexpected: {error}"
                ))
            })?;
        if !has_owner {
            return Err(VshotError::KwinScreenShot(format!(
                "{SERVICE} does not own a name on the session bus, so there is no KWin to ask"
            )));
        }
        // A Plasma 5 KWin can be running without ScreenShot2, which would
        // otherwise only show up as an error on the first capture.
        let introspection: String = connection
            .call_method(
                Some(SERVICE),
                PATH,
                Some("org.freedesktop.DBus.Introspectable"),
                "Introspect",
                &(),
            )
            .map_err(|error| {
                VshotError::KwinScreenShot(format!("{SERVICE} does not offer {PATH}: {error}"))
            })?
            .body()
            .deserialize()
            .map_err(|error| {
                VshotError::KwinScreenShot(format!(
                    "the introspection data of {PATH} could not be read: {error}"
                ))
            })?;
        if !declares_interface(&introspection, INTERFACE) {
            return Err(VshotError::KwinScreenShot(format!(
                "{SERVICE} does not implement {INTERFACE}, so this KWin is too old to capture through it"
            )));
        }
        Ok(Self { connection })
    }

    /// Captures one output by its `wl_output` name, which is what KWin calls a
    /// screen: `CaptureScreen` echoes the name back in its results.
    pub fn capture_output(&mut self, name: &str, cursor: bool) -> Result<Frame> {
        let (frame, _) = self.request(Shot::Output { name, cursor })?;
        Ok(frame)
    }

    /// Captures the focused window through KWin's own window screenshot.
    ///
    /// It is the only exact answer a Plasma session offers: KWin's window list
    /// is not published over any protocol vshot speaks, so the alternative is
    /// to guess the window's rectangle out of the frozen frame.  KWin hands over
    /// the window's own pixels instead — decorations included, at the window's
    /// native resolution — and states the `scale` of the screen it lives on,
    /// which is the density the frame has to keep.
    pub fn capture_active_window(&mut self, cursor: bool) -> Result<(Frame, u32)> {
        let (frame, scale) = self.request(Shot::ActiveWindow { cursor })?;
        let scale = scale.expect("a window capture always carries a scale");
        Ok((frame, scale))
    }

    /// Captures one window by its internal id, whether or not it has the
    /// focus.  `CaptureWindow` takes the window's `QUuid` string — the same
    /// value the scripting interface calls `internalId` — and renders it the
    /// way `CaptureActiveWindow` renders the focused one: decorations and
    /// shadow included, alpha kept, and the screen's `scale` in the results.
    ///
    /// This is what makes window *recording* possible on a Plasma session.
    /// `CaptureActiveWindow` answers "whatever has the focus", which changes
    /// under a recording; a handle names one window for the whole session.
    pub fn capture_window(&mut self, handle: &str, cursor: bool) -> Result<(Frame, u32)> {
        let (frame, scale) = self.request(Shot::Window { handle, cursor })?;
        let scale = scale.expect("a window capture always carries a scale");
        Ok((frame, scale))
    }

    /// Runs one capture and returns the frame KWin rendered, plus the density
    /// it stated for it when the capture is one that carries a density.
    ///
    /// KWin renders on its own thread and can answer the call before the whole
    /// frame has crossed the pipe — a 64 KiB pipe buffer against a 132 MB
    /// frame, with the reply arriving first.  Draining from a separate thread
    /// works whichever order KWin picks; reading only after the reply would
    /// depend on that order and stall on a full pipe if the compositor ever
    /// wrote inline.
    fn request(&self, shot: Shot<'_>) -> Result<(Frame, Option<u32>)> {
        let options = match &shot {
            Shot::Output { cursor, .. } => capture_options(*cursor),
            Shot::ActiveWindow { cursor } => window_options(*cursor),
            // The by-handle route is the recording path only, and a recording
            // cannot carry a shadow (see [`recording_window_options`]).
            Shot::Window { cursor, .. } => recording_window_options(*cursor),
        };
        let (read_end, write_end) = rustix::pipe::pipe().map_err(|error| {
            VshotError::KwinScreenShot(format!(
                "cannot create a pipe for KWin to render into: {error}"
            ))
        })?;
        let reader = std::thread::spawn(move || read_to_end(read_end));
        let reply = match &shot {
            Shot::Output { name, .. } => self.connection.call_method(
                Some(SERVICE),
                PATH,
                Some(INTERFACE),
                "CaptureScreen",
                &(*name, options, Fd::from(&write_end)),
            ),
            Shot::ActiveWindow { .. } => self.connection.call_method(
                Some(SERVICE),
                PATH,
                Some(INTERFACE),
                "CaptureActiveWindow",
                &(options, Fd::from(&write_end)),
            ),
            // The window is named by its `QUuid` string and nothing else in
            // the request: `CaptureWindow(s handle, a{sv} options, h pipe)`.
            Shot::Window { handle, .. } => self.connection.call_method(
                Some(SERVICE),
                PATH,
                Some(INTERFACE),
                "CaptureWindow",
                &(*handle, options, Fd::from(&write_end)),
            ),
        };
        // Only the compositor's copy of the write end may stay open, otherwise
        // the reader never sees the end of the pixel stream.
        drop(write_end);
        let reply = match reply {
            Ok(reply) => reply,
            // No pixels are coming, and KWin closes its end of the pipe when it
            // refuses, so the reader finishes on its own.  It is left detached
            // rather than joined so that reporting the refusal never waits on a
            // compositor that might not close anything.
            Err(error) => return Err(map_dbus_error(&error, &shot.target())),
        };
        let bytes = reader
            .join()
            .map_err(|_| {
                VshotError::KwinScreenShot("the thread reading the pixel pipe panicked".into())
            })?
            .map_err(|error| {
                VshotError::KwinScreenShot(format!("cannot read the pixel pipe: {error}"))
            })?;
        let body = reply.body();
        let results: HashMap<String, Value<'_>> = body.deserialize().map_err(|error| {
            VshotError::KwinScreenShot(format!("the capture results could not be read: {error}"))
        })?;
        let geometry = parse_results(&results)?;
        // A window capture is the one that has to know its density — the caller
        // either pins those pixels or writes them next to the size they are
        // meant to be shown at — so it is the one that insists on being told.
        // It is also the one whose pixels do not fill their frame, so it is the
        // one that keeps them see-through.
        let (scale, alpha) = match shot {
            Shot::ActiveWindow { .. } | Shot::Window { .. } => {
                (Some(result_scale(&results)?), Alpha::Keep)
            }
            Shot::Output { .. } => (None, Alpha::Flatten),
        };
        Ok((convert_premultiplied_bgra(&bytes, geometry, alpha)?, scale))
    }
}

/// The option map handed to ScreenShot2.  KWin ignores keys it does not know —
/// and, as far as a headless session can tell, it accepts keys it should
/// reject — so a typo here cannot be caught by a probe.  `include-cursor` is
/// documented by KDE and passed through, but whether KWin actually draws the
/// pointer has not been verified: a `--virtual` output has no pointer to draw.
///
/// `native-resolution` decides the size of the frame: without it KWin renders
/// at *logical* size, so a 4K screen at scale 2 arrives as 1920x1080 and then
/// contradicts the scale the topology reported, which the scene rejects
/// instead of upscaling silently.
fn capture_options(cursor: bool) -> HashMap<&'static str, Value<'static>> {
    let mut options = HashMap::new();
    options.insert("include-cursor", Value::from(cursor));
    options.insert("native-resolution", Value::from(true));
    options
}

/// The options for a window *screenshot*: the output ones plus the decoration
/// and KWin's default shadow.
///
/// `include-decoration` is what keeps the title bar and the window's frame in
/// the picture: the request is "this window", and a window stripped of its
/// frame is not what the user is looking at.
///
/// `include-shadow` is left at KWin's default, which draws the shadow: it is
/// part of how the window looks on screen, and the capture keeps the alpha
/// channel (see [`Alpha::Keep`]) so the pixels the shadow does not cover stay
/// transparent instead of turning into a black fringe around the window.  A
/// window that fills the screen has its shadow clipped away, so the same
/// capture can come back either padded or exactly the size of the window.
///
/// A window capture arrives at the window's native resolution either way: on
/// KWin 6.7.5 a 1920x1046 window on a scale-2 screen came back as 3840x2092
/// both with and without `native-resolution`, and the reply stated the scale.
fn window_options(cursor: bool) -> HashMap<&'static str, Value<'static>> {
    let mut options = capture_options(cursor);
    options.insert("include-decoration", Value::from(true));
    options
}

/// The options for the window *recording* path, which are [`window_options`]
/// with the shadow turned off.
///
/// A screenshot writes a PNG, which keeps the alpha channel, so the shadow's
/// transparent surround is harmless — even useful.  A recording encodes NV12,
/// which has no alpha at all: the shadow's transparent pixels become opaque
/// black, so keeping it would ring the window in a black band.  It would also
/// grow the canvas — a 941x768 window captured with its shadow came back as
/// 1072x898 — which then forces the whole frame through the fit path for
/// nothing.  Turning the shadow off makes the capture exactly the window (the
/// title bar's decoration is kept), and the recording is of the window, not of
/// its drop shadow.
fn recording_window_options(cursor: bool) -> HashMap<&'static str, Value<'static>> {
    let mut options = window_options(cursor);
    options.insert("include-shadow", Value::from(false));
    options
}

/// Drains the pipe to its end.  The compositor closes its write end when the
/// frame is complete, so the read ends at EOF.
fn read_to_end(pipe: OwnedFd) -> std::io::Result<Vec<u8>> {
    let mut file = File::from(pipe);
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes)?;
    Ok(bytes)
}

/// Does the introspection data of an object offer `interface`?  KWin answers
/// introspection through Qt's virtual objects, so this looks for the interface
/// name as an XML attribute value rather than parsing the document.
fn declares_interface(introspection: &str, interface: &str) -> bool {
    introspection.contains(&format!("name=\"{interface}\""))
}

/// Reads the geometry out of a `CaptureScreen` reply.  A missing or unexpected
/// entry is an error: guessing at the layout would turn into a garbled image or
/// a length mismatch much further from the cause.
fn parse_results(results: &HashMap<String, Value<'_>>) -> Result<CaptureGeometry> {
    if let Some(kind) = results.get("type") {
        let kind = kind.downcast_ref::<&str>().map_err(|_| {
            VshotError::KwinScreenShot("the `type` of the capture is not a string".into())
        })?;
        if kind != "raw" {
            return Err(VshotError::KwinScreenShot(format!(
                "KWin returned a `{kind}` capture, which vshot cannot read"
            )));
        }
    }
    let format = result_u32(results, "format")?;
    if format != FORMAT_ARGB32_PREMULTIPLIED {
        return Err(VshotError::KwinScreenShot(format!(
            "KWin returned QImage format {format}, but only {FORMAT_ARGB32_PREMULTIPLIED} \
             (ARGB32_Premultiplied) can be converted"
        )));
    }
    let width = result_u32(results, "width")?;
    let height = result_u32(results, "height")?;
    let stride = result_u32(results, "stride")?;
    let stride = usize::try_from(stride)
        .map_err(|_| VshotError::KwinScreenShot("the capture stride is too large".into()))?;
    if width == 0 || height == 0 {
        return Err(VshotError::KwinScreenShot(format!(
            "KWin returned a {width}x{height} capture"
        )));
    }
    Ok(CaptureGeometry {
        width,
        height,
        stride,
    })
}

fn result_u32(results: &HashMap<String, Value<'_>>, key: &str) -> Result<u32> {
    let value = results.get(key).ok_or_else(|| {
        VshotError::KwinScreenShot(format!("KWin returned no `{key}` in the capture results"))
    })?;
    value.downcast_ref::<u32>().map_err(|_| {
        VshotError::KwinScreenShot(format!("the capture's `{key}` is not an unsigned integer"))
    })
}

/// How many device pixels KWin drew per logical pixel of a window, which is the
/// density the frame has to keep: a window on a 4K screen is twice as many
/// pixels as it is logical pixels, and pinning it has to be able to work that
/// out.  Only a window capture needs to be told this — an output's scale comes
/// from the topology — and a window capture that cannot state it is refused
/// rather than assumed to be 1, because that assumption is exactly what sizes a
/// pinned image wrong.
fn result_scale(results: &HashMap<String, Value<'_>>) -> Result<u32> {
    let value = results.get("scale").ok_or_else(|| {
        VshotError::KwinScreenShot(
            "KWin returned no `scale` for the window, so the density its pixels are meant to \
             be shown at is unknown"
                .into(),
        )
    })?;
    // KWin announces a double (`2`, or `1.25` on a fractional-scale screen);
    // everything else in vshot counts whole device pixels per logical pixel.
    let scale = value
        .downcast_ref::<f64>()
        .map_err(|_| VshotError::KwinScreenShot("the capture's `scale` is not a number".into()))?;
    let rounded = scale.round();
    if !scale.is_finite() || scale < 1.0 {
        return Err(VshotError::KwinScreenShot(format!(
            "KWin announced a scale of {scale} for the capture, which no screen has"
        )));
    }
    Ok(rounded as u32)
}

/// Converts the payload ScreenShot2 writes into a pipe into the RGBA frame the
/// rest of vshot works with.
///
/// The bytes are premultiplied BGRA with a row stride that may be padded, and
/// what the alpha means is `alpha`'s to say.  [`Alpha::Flatten`] drops it: a
/// screenshot shows what the compositor composited onto an output, which is
/// opaque by definition, so a region nothing was drawn into (raw `0, 0, 0, 0`)
/// comes out black.  [`Alpha::Keep`] unpremultiplies and keeps it, so those same
/// pixels come out fully transparent and a half-covered shadow pixel keeps the
/// coverage that makes it a shadow.
fn convert_premultiplied_bgra(
    bytes: &[u8],
    geometry: CaptureGeometry,
    alpha_policy: Alpha,
) -> Result<Frame> {
    let width = usize::try_from(geometry.width)
        .map_err(|_| VshotError::KwinScreenShot("the capture width is too large".into()))?;
    let height = usize::try_from(geometry.height)
        .map_err(|_| VshotError::KwinScreenShot("the capture height is too large".into()))?;
    let row_bytes = width
        .checked_mul(4)
        .ok_or_else(|| VshotError::KwinScreenShot("the capture row is too large".into()))?;
    if geometry.stride < row_bytes {
        return Err(VshotError::KwinScreenShot(format!(
            "KWin announced a stride of {} for a {width}-pixel frame",
            geometry.stride
        )));
    }
    let expected = geometry
        .stride
        .checked_mul(height)
        .ok_or_else(|| VshotError::KwinScreenShot("the capture frame is too large".into()))?;
    if bytes.len() < expected {
        return Err(VshotError::KwinScreenShot(format!(
            "KWin announced {expected} bytes of pixels but sent {}",
            bytes.len()
        )));
    }
    let mut pixels = vec![0u8; row_bytes * height];
    for (row, frame_row) in pixels.chunks_exact_mut(row_bytes).enumerate() {
        let source_row = &bytes[row * geometry.stride..row * geometry.stride + row_bytes];
        for (destination, source) in frame_row
            .chunks_exact_mut(4)
            .zip(source_row.chunks_exact(4))
        {
            let alpha = source[3];
            let (red, green, blue) = (source[2], source[1], source[0]);
            // Nothing was drawn here (raw `0, 0, 0, 0`), which has no colour to
            // restore: it stays black, and only the coverage depends on whether
            // the frame is meant to be see-through.
            let colour = if alpha == 0 {
                [0, 0, 0]
            } else {
                [
                    unpremultiply(red, alpha),
                    unpremultiply(green, alpha),
                    unpremultiply(blue, alpha),
                ]
            };
            let coverage = match alpha_policy {
                Alpha::Flatten => 255,
                Alpha::Keep => alpha,
            };
            destination.copy_from_slice(&[colour[0], colour[1], colour[2], coverage]);
        }
    }
    Frame::new(Size::new(geometry.width, geometry.height), pixels)
}

/// Undoes `channel * alpha / 255`, rounding to the nearest colour.  A
/// premultiplied channel never exceeds its alpha, so the result cannot exceed
/// 255; the clamp only covers a compositor that sends something malformed.
fn unpremultiply(channel: u8, alpha: u8) -> u8 {
    let scaled = u32::from(channel) * 255 + u32::from(alpha) / 2;
    u8::try_from((scaled / u32::from(alpha)).min(255)).unwrap_or(255)
}

fn map_dbus_error(error: &zbus::Error, target: &str) -> VshotError {
    if let zbus::Error::MethodError(name, detail, _) = error {
        return map_method_error(name.as_str(), detail.as_deref(), target);
    }
    VshotError::KwinScreenShot(format!("the capture call failed: {error}"))
}

/// Turns a `ScreenShot2` error name into something the user can act on.  KDE's
/// authorization reply is the one a real session will hit, and the raw string
/// ("The process is not authorized to take a screenshot") reads like a prompt
/// waiting to be approved.  KWin has no such prompt, so the hint spells out the
/// desktop-file rule instead.
///
/// `target` is what the capture was aimed at, as [`Shot::target`] spells it:
/// "the screen `DP-2`" or "the focused window".
fn map_method_error(name: &str, detail: Option<&str>, target: &str) -> VshotError {
    match name {
        "org.kde.KWin.ScreenShot2.Error.NoAuthorized" => {
            VshotError::ScreenshotDenied(not_authorized_hint())
        }
        "org.kde.KWin.ScreenShot2.Error.InvalidScreen" => {
            VshotError::IncompleteTopology(format!("KWin ScreenShot2 does not know {target}"))
        }
        // The window a capture was aimed at is gone: it closed, or the handle
        // belongs to a window KWin no longer knows.  A recording ends on this
        // the way it ends on any compositor when the recorded window closes —
        // the file is finished properly rather than left without a trailer.
        "org.kde.KWin.ScreenShot2.Error.InvalidWindow" => {
            VshotError::WindowClosed(format!("{target} is no longer a window KWin knows"))
        }
        _ => {
            let detail = detail.unwrap_or("no details");
            VshotError::KwinScreenShot(format!("{name}: {detail}"))
        }
    }
}

/// Explains KWin's authorization reply.  `ScreenShotDBusInterface2::
/// checkPermissions` resolves the caller's pid to the executable behind
/// `/proc/<pid>/exe`, looks up the installed desktop file whose `Exec=` starts
/// with that path, and requires it to declare the restricted interface — there
/// is nothing for the user to click, which is why a session that only ever sees
/// this error cannot be talked into granting it.  Naming the running executable
/// keeps the desktop file to write unambiguous.
fn not_authorized_hint() -> String {
    let executable = std::env::current_exe()
        .map(|path| path.display().to_string())
        .unwrap_or_else(|_| "the vshot executable".to_string());
    format!(
        "KWin shows no permission prompt: it authorizes the ScreenShot2 interface \
         only for a client whose desktop file declares \
         `X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2`.  This binary \
         is `{executable}`.  Installing the package provides such a desktop file \
         for `/usr/bin/vshot`; a build run straight out of `target/` needs its own \
         desktop file whose `Exec=` starts with `{executable}` plus that key, and \
         then `kbuildsycoca6 --noincremental`.  The compositor-side \
         KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1 disables the check and is meant \
         for development only."
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn geometry(width: u32, height: u32, stride: usize) -> CaptureGeometry {
        CaptureGeometry {
            width,
            height,
            stride,
        }
    }

    fn results(entries: &[(&str, Value<'static>)]) -> HashMap<String, Value<'static>> {
        entries
            .iter()
            .map(|(key, value)| ((*key).to_string(), value.clone()))
            .collect()
    }

    fn full_results() -> HashMap<String, Value<'static>> {
        results(&[
            ("width", Value::from(1024u32)),
            ("height", Value::from(768u32)),
            ("stride", Value::from(4096u32)),
            ("format", Value::from(FORMAT_ARGB32_PREMULTIPLIED)),
            ("scale", Value::from(1.0f64)),
            ("type", Value::from("raw")),
        ])
    }

    #[test]
    fn parses_the_results_a_capture_returns() {
        assert_eq!(
            parse_results(&full_results()).unwrap(),
            geometry(1024, 768, 4096)
        );
    }

    #[test]
    fn an_unreadable_capture_is_rejected_rather_than_guessed_at() {
        // RGB32 (4) is not premultiplied, so the bytes would come out wrong.
        let mut wrong_format = full_results();
        wrong_format.insert("format".into(), Value::from(4u32));
        assert!(matches!(
            parse_results(&wrong_format),
            Err(VshotError::KwinScreenShot(_))
        ));

        // A future non-raw encoding would not be BGRA at all.
        let mut not_raw = full_results();
        not_raw.insert("type".into(), Value::from("dmabuf"));
        assert!(matches!(
            parse_results(&not_raw),
            Err(VshotError::KwinScreenShot(_))
        ));

        for missing in ["width", "height", "stride", "format"] {
            let mut incomplete = full_results();
            incomplete.remove(missing);
            assert!(
                matches!(
                    parse_results(&incomplete),
                    Err(VshotError::KwinScreenShot(_))
                ),
                "a capture without `{missing}` has to be refused"
            );
        }

        let mut empty = full_results();
        empty.insert("width".into(), Value::from(0u32));
        assert!(matches!(
            parse_results(&empty),
            Err(VshotError::KwinScreenShot(_))
        ));
    }

    #[test]
    fn an_unsigned_integer_that_is_not_one_is_refused() {
        let mut results = full_results();
        results.insert("stride".into(), Value::from("4096"));
        assert!(matches!(
            parse_results(&results),
            Err(VshotError::KwinScreenShot(_))
        ));
    }

    #[test]
    fn premultiplied_colours_are_unpremultiplied() {
        // A colour stored at its own alpha has to come back at full strength —
        // 128/128 of red is red — while an opaque colour passes through
        // untouched.
        let bytes = [0, 0, 128, 128, 30, 20, 10, 255, 0, 96, 0, 96];
        let frame = convert_premultiplied_bgra(&bytes, geometry(3, 1, 12), Alpha::Flatten).unwrap();
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(0, 0)),
            Some([255, 0, 0, 255])
        );
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(1, 0)),
            Some([10, 20, 30, 255])
        );
        // 96/255 of green is the same green at 96/255 of the coverage.
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(2, 0)),
            Some([0, 255, 0, 255])
        );
    }

    #[test]
    fn bgra_is_read_as_bgra() {
        // ScreenShot2 sends B, G, R, A in memory; treating that as RGB would
        // swap red and blue, which is the mistake to catch here.
        let bytes = [0, 0, 255, 255];
        let frame = convert_premultiplied_bgra(&bytes, geometry(1, 1, 4), Alpha::Flatten).unwrap();
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(0, 0)),
            Some([255, 0, 0, 255])
        );
    }

    #[test]
    fn an_output_capture_flattens_an_undrawn_pixel_to_black() {
        // An output region nothing was composited into arrives as 0, 0, 0, 0;
        // the screen is still opaque there, so the alpha becomes 255.
        let bytes = [0, 0, 0, 0, 0, 0, 0, 0];
        let frame = convert_premultiplied_bgra(&bytes, geometry(2, 1, 8), Alpha::Flatten).unwrap();
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(0, 0)),
            Some([0, 0, 0, 255])
        );
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(1, 0)),
            Some([0, 0, 0, 255])
        );
    }

    #[test]
    fn a_window_capture_keeps_its_backdrop_transparent() {
        // A window capture is a picture of a window on a transparent backdrop:
        // the pixels beyond it arrive as 0, 0, 0, 0 and the shadow arrives as
        // semi-transparent black.  Both have to survive as coverage, because
        // restoring them to opaque is exactly the black fringe around the window
        // that the flattening policy produces.
        let bytes = [
            0, 0, 0, 0, // padding: nothing was drawn here
            0, 0, 0, 128, // the window's shadow, half covered
            0, 0, 255, 255, // the window itself
        ];
        let frame = convert_premultiplied_bgra(&bytes, geometry(3, 1, 12), Alpha::Keep).unwrap();
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(0, 0)),
            Some([0, 0, 0, 0])
        );
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(1, 0)),
            Some([0, 0, 0, 128])
        );
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(2, 0)),
            Some([255, 0, 0, 255])
        );

        // The same payload read as an output is the fringe.
        let flattened =
            convert_premultiplied_bgra(&bytes, geometry(3, 1, 12), Alpha::Flatten).unwrap();
        assert_eq!(
            flattened.pixel(crate::geometry::Point::new(0, 0)),
            Some([0, 0, 0, 255])
        );
        assert_eq!(
            flattened.pixel(crate::geometry::Point::new(1, 0)),
            Some([0, 0, 0, 255])
        );
    }

    #[test]
    fn a_stride_wider_than_the_row_is_trimmed() {
        // Two padded rows: the padding bytes must not reach the frame, and the
        // second row must start at the stride, not right after the first.
        let bytes = [
            3, 2, 1, 255, 0xAA, 0xBB, 0xCC, 0xDD, // row 0 plus padding
            6, 5, 4, 255, 0xAA, 0xBB, 0xCC, 0xDD, // row 1 plus padding
        ];
        let frame = convert_premultiplied_bgra(&bytes, geometry(1, 2, 8), Alpha::Flatten).unwrap();
        assert_eq!(frame.size(), Size::new(1, 2));
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(0, 0)),
            Some([1, 2, 3, 255])
        );
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(0, 1)),
            Some([4, 5, 6, 255])
        );
        assert_eq!(frame.pixels().len(), 8);
    }

    #[test]
    fn a_payload_that_does_not_match_the_announced_geometry_is_refused() {
        // A stride narrower than the row cannot describe the pixels at all.
        assert!(matches!(
            convert_premultiplied_bgra(&[0; 8], geometry(2, 1, 4), Alpha::Flatten),
            Err(VshotError::KwinScreenShot(_))
        ));
        // A row that is never sent is a truncated frame, not a shorter image.
        assert!(matches!(
            convert_premultiplied_bgra(&[0; 4], geometry(1, 2, 4), Alpha::Flatten),
            Err(VshotError::KwinScreenShot(_))
        ));
        // Trailing bytes beyond the announced frame are harmless.
        assert!(convert_premultiplied_bgra(&[0; 9], geometry(1, 2, 4), Alpha::Flatten).is_ok());
    }

    #[test]
    fn the_capture_options_ask_for_native_pixels() {
        let options = capture_options(true);
        assert_eq!(
            options.get("include-cursor"),
            Some(&Value::from(true)),
            "the KDE option name has to survive unchanged"
        );
        assert_eq!(
            options.get("native-resolution"),
            Some(&Value::from(true)),
            "a scaled output only arrives at its real size when native resolution is asked for"
        );
    }

    #[test]
    fn a_window_capture_asks_for_the_window_itself() {
        let options = window_options(false);
        assert_eq!(
            options.get("include-decoration"),
            Some(&Value::from(true)),
            "the decoration is part of the window the user asked for"
        );
        assert_eq!(
            options.get("include-shadow"),
            None,
            "the shadow is not part of the window and would pad the image"
        );
        // The output options are still the base: `native-resolution` is what
        // keeps a HiDPI window at its real pixel count.
        assert_eq!(options.get("native-resolution"), Some(&Value::from(true)));
        assert_eq!(options.get("include-cursor"), Some(&Value::from(false)));
        assert_eq!(
            window_options(true).get("include-cursor"),
            Some(&Value::from(true))
        );
    }

    #[test]
    fn the_scale_a_window_capture_states_is_read() {
        let mut hidpi = full_results();
        hidpi.insert("scale".into(), Value::from(2.0f64));
        assert_eq!(result_scale(&hidpi).unwrap(), 2);
        // Plasma can describe a fractional scale while everything else in
        // vshot counts whole device pixels per logical pixel, so the value is
        // rounded rather than refused.
        hidpi.insert("scale".into(), Value::from(1.5f64));
        assert_eq!(result_scale(&hidpi).unwrap(), 2);
        hidpi.insert("scale".into(), Value::from(1.25f64));
        assert_eq!(result_scale(&hidpi).unwrap(), 1);

        // A capture that cannot say how big its pixels are meant to be shown is
        // refused: assuming 1 is how a pinned image ends up at the wrong size.
        let mut missing = full_results();
        missing.remove("scale");
        assert!(matches!(
            result_scale(&missing),
            Err(VshotError::KwinScreenShot(_))
        ));
        let mut not_a_number = full_results();
        not_a_number.insert("scale".into(), Value::from("2"));
        assert!(matches!(
            result_scale(&not_a_number),
            Err(VshotError::KwinScreenShot(_))
        ));
        for nonsense in [0.0f64, 0.5, -2.0, f64::NAN] {
            let mut results = full_results();
            results.insert("scale".into(), Value::from(nonsense));
            assert!(
                matches!(result_scale(&results), Err(VshotError::KwinScreenShot(_))),
                "a scale of {nonsense} is not a screen scale"
            );
        }
    }

    #[test]
    fn every_capture_says_what_it_was_aimed_at() {
        assert_eq!(
            Shot::Output {
                name: "DP-2",
                cursor: false
            }
            .target(),
            "the screen `DP-2`"
        );
        assert_eq!(
            Shot::ActiveWindow { cursor: true }.target(),
            "the focused window"
        );
    }

    #[test]
    fn the_introspection_data_is_searched_for_the_interface() {
        let xml = r#"<node><interface name="org.kde.KWin.ScreenShot2">
            <method name="CaptureScreen"/></interface>
            <interface name="org.kde.KWin.Other"/></node>"#;
        assert!(declares_interface(xml, INTERFACE));
        assert!(!declares_interface(xml, "org.kde.KWin.ScreenShot3"));
        assert!(!declares_interface("<node/>", INTERFACE));
    }

    #[test]
    fn a_denied_capture_names_the_desktop_file_requirement() {
        let error = map_method_error(
            "org.kde.KWin.ScreenShot2.Error.NoAuthorized",
            Some("The process is not authorized to take a screenshot"),
            "Virtual-0",
        );
        let VshotError::ScreenshotDenied(hint) = error else {
            panic!("NoAuthorized has to map to the permission error");
        };
        assert!(
            hint.contains("X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2"),
            "{hint}"
        );
        assert!(hint.contains("kbuildsycoca6"), "{hint}");
        assert!(
            hint.contains("KWIN_SCREENSHOT_NO_PERMISSION_CHECKS"),
            "{hint}"
        );
        // The desktop file rule is keyed on the caller's own executable, so the
        // hint has to carry that path rather than a generic instruction.
        let executable = std::env::current_exe().unwrap();
        assert!(hint.contains(&executable.display().to_string()), "{hint}");
        // KWin shows no dialog, so promising one would send the user looking for it.
        assert!(!hint.contains("prompt it shows"), "{hint}");
    }

    #[test]
    fn an_unknown_screen_is_reported_as_a_topology_problem() {
        let error = map_method_error(
            "org.kde.KWin.ScreenShot2.Error.InvalidScreen",
            None,
            "Virtual-9",
        );
        let VshotError::IncompleteTopology(message) = error else {
            panic!("InvalidScreen has to map to the unknown-screen error");
        };
        assert!(message.contains("Virtual-9"), "{message}");
    }

    #[test]
    fn other_dbus_errors_keep_their_name() {
        let error = map_method_error("org.kde.KWin.ScreenShot2.Error.Something", None, "X");
        let VshotError::KwinScreenShot(message) = error else {
            panic!("an unrecognized error has to keep its KWin context");
        };
        assert!(message.contains("Error.Something"), "{message}");
    }

    /// Really talks to KWin over D-Bus.  Ignored by default because it needs a
    /// running KWin on the session bus; the setup that makes it pass is:
    ///
    /// ```sh
    /// mkdir -p /tmp/kwin-e2e/{cfg,data,cache}
    /// KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1 XDG_CONFIG_HOME=/tmp/kwin-e2e/cfg \
    ///   XDG_DATA_HOME=/tmp/kwin-e2e/data XDG_CACHE_HOME=/tmp/kwin-e2e/cache \
    ///   kwin_wayland --virtual --socket wayland-ke2e --no-lockscreen \
    ///   --no-global-shortcuts --no-kactivities &
    /// XDG_RUNTIME_DIR=/run/user/$(id -u) cargo test -- --ignored
    /// ```
    ///
    /// `VSHOT_KWIN_E2E_OUTPUT`, `VSHOT_KWIN_E2E_WIDTH` and
    /// `VSHOT_KWIN_E2E_HEIGHT` describe the screen to capture, and
    /// `VSHOT_KWIN_E2E_COLOR=R,G,B` asserts the centre pixel when something
    /// coloured is on screen.  `KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1` is what
    /// makes the compositor answer without a prompt; without it the capture is
    /// denied by design.  KWin's own `--width`/`--height`/`--scale` set the
    /// virtual output's size, which is 1024x768 by default; a 4K frame is the
    /// interesting case because it is far larger than the pipe buffer.
    #[test]
    #[ignore = "needs a running KWin session on the same bus, see the comment above"]
    fn captures_a_live_kwin_screen() {
        let output = std::env::var("VSHOT_KWIN_E2E_OUTPUT").unwrap_or_else(|_| "Virtual-0".into());
        let expected_width: u32 = std::env::var("VSHOT_KWIN_E2E_WIDTH")
            .ok()
            .and_then(|value| value.parse().ok())
            .unwrap_or(1024);
        let expected_height: u32 = std::env::var("VSHOT_KWIN_E2E_HEIGHT")
            .ok()
            .and_then(|value| value.parse().ok())
            .unwrap_or(768);

        let mut capture = KwinCapture::connect().expect("KWin ScreenShot2 has to be reachable");
        let frame = capture
            .capture_output(&output, false)
            .expect("the capture has to succeed");
        eprintln!(
            "captured {output}: {}x{}, {} bytes",
            frame.size().width,
            frame.size().height,
            frame.pixels().len()
        );
        assert_eq!(
            frame.size(),
            Size::new(expected_width, expected_height),
            "unexpected frame size"
        );
        assert_eq!(
            frame.pixels().len(),
            expected_width as usize * expected_height as usize * 4,
            "a compact frame holds exactly width * height * 4 bytes"
        );
        // A screenshot is opaque everywhere, including the regions nothing was
        // drawn into.
        assert!(
            frame.pixels().chunks_exact(4).all(|pixel| pixel[3] == 255),
            "every captured pixel has to be opaque"
        );
        if let Ok(expected) = std::env::var("VSHOT_KWIN_E2E_COLOR") {
            let channels: Vec<u8> = expected
                .split(',')
                .map(|channel| channel.trim().parse().expect("R,G,B"))
                .collect();
            let center = crate::geometry::Point::new(
                (expected_width / 2) as i32,
                (expected_height / 2) as i32,
            );
            assert_eq!(
                frame.pixel(center),
                Some([channels[0], channels[1], channels[2], 255]),
                "the colour on screen has to survive the conversion"
            );
        }
    }

    /// Captures one window by the `QUuid` the scripting probe reports, live.
    ///
    /// The route under test is the one `record window` / `replay start window`
    /// take on Plasma: `ScreenShot2.CaptureWindow` addressed by the window's
    /// `internalId`, with the shadow off.  Set up the compositor and a window
    /// exactly as for [`captures_a_live_kwin_screen`], then start something
    /// with a class:
    ///
    /// ```text
    /// WAYLAND_DISPLAY=wayland-ke2e kitty --class kittytest -T "Hello VShot" &
    /// VSHOT_KWIN_E2E_WINDOW=kittytest cargo test -- --ignored captures_a_live_kwin_window
    /// ```
    ///
    /// `VSHOT_KWIN_E2E_WINDOW` is a substring of the window's class or title
    /// (default `kitty`), and `VSHOT_KWIN_E2E_OUTPUT` names the screen to
    /// cross-check against (default `Virtual-0`).  Three things are pinned:
    /// the window comes back **at the size the probe reports for it** — the
    /// recorder rounds that up to an even canvas itself, so the raw capture
    /// must not do it first, or the even rounding would hide a wrong size; its
    /// body is substantially opaque, which is the [`Alpha::Keep`] a window
    /// capture carries and an output one does not; and its **colours are the
    /// ones on screen** at the window's place, which is what says the
    /// un-premultiply and the byte order are right.  That last check needs a
    /// 1:1 screen at a non-negative window origin and is skipped otherwise.
    /// Measured on this machine: 936x768, body alpha 204, rounded-corner alphas
    /// 0..57, and a body pixel of (250, 243, 225) against (251, 244, 226) on
    /// screen — one step of rounding, see the assertion.
    #[test]
    #[ignore = "needs a running KWin session with a window on it, see the comment above"]
    fn captures_a_live_kwin_window() {
        use crate::capture::window::{kwin_rows, ProcessWindowRunner};

        let needle = std::env::var("VSHOT_KWIN_E2E_WINDOW").unwrap_or_else(|_| "kitty".into());
        let rows = kwin_rows(&ProcessWindowRunner).expect("the KWin probe has to answer");
        let row = rows
            .into_iter()
            .find(|row| row.app_id.contains(&needle) || row.title.contains(&needle))
            .unwrap_or_else(|| panic!("no window matching `{needle}` is on screen"));
        eprintln!(
            "capturing {} `{}` at {}x{} (handle {})",
            row.app_id, row.title, row.width, row.height, row.handle
        );
        assert!(
            !row.handle.is_empty(),
            "the probe has to report the window's internalId, or nothing can address it"
        );

        let mut capture = KwinCapture::connect().expect("KWin ScreenShot2 has to be reachable");
        let (frame, scale) = capture
            .capture_window(&row.handle, false)
            .expect("capturing the window has to succeed");
        eprintln!(
            "captured {}x{} at scale {scale}, {} bytes",
            frame.size().width,
            frame.size().height,
            frame.pixels().len()
        );
        assert_eq!(
            frame.size(),
            Size::new(row.width, row.height),
            "the capture has to come back at the size the probe reported for the window"
        );

        // A window capture keeps its alpha (`Alpha::Keep`): the window sits on a
        // transparent backdrop, so its rounded corners arrive see-through.  What
        // a recording needs is a substantially opaque body — the recorder drops
        // the alpha, so a wrong alpha on its own would not show up as a wrong
        // picture, and the colours are checked separately below.
        let centre = crate::geometry::Point::new((row.width / 2) as i32, (row.height / 2) as i32);
        let body = frame.pixel(centre).expect("the window has a middle");
        let corners = [
            crate::geometry::Point::new(0, 0),
            crate::geometry::Point::new(row.width as i32 - 1, 0),
            crate::geometry::Point::new(0, row.height as i32 - 1),
            crate::geometry::Point::new(row.width as i32 - 1, row.height as i32 - 1),
        ];
        eprintln!(
            "body alpha {}, corner alphas {:?}",
            body[3],
            corners.map(|corner| frame.pixel(corner).map(|pixel| pixel[3]))
        );
        assert!(
            body[3] >= 128,
            "the window's body has to be substantially opaque, not a ghost"
        );

        // The same pixels have to be on screen, at the window's place, when the
        // *screen* is captured instead.  That is what says the un-premultiply
        // and the byte order are right; on a 1:1 screen the window's logical
        // rectangle indexes the output frame directly.
        let output = std::env::var("VSHOT_KWIN_E2E_OUTPUT").unwrap_or_else(|_| "Virtual-0".into());
        if scale != 1 || row.x < 0 || row.y < 0 {
            eprintln!(
                "window at ({}, {}) on a scale-{scale} screen — skipping the colour cross-check",
                row.x, row.y
            );
            return;
        }
        let screen = capture
            .capture_output(&output, false)
            .expect("the screen capture has to succeed");
        let on_screen = crate::geometry::Point::new(
            row.x + (row.width / 2) as i32,
            row.y + (row.height / 2) as i32,
        );
        let window_pixel = frame.pixel(centre).expect("the window has a middle");
        let screen_pixel = screen.pixel(on_screen).expect("that point is on screen");
        let (a, b) = (window_pixel, screen_pixel);
        eprintln!(
            "window pixel {:?} against on-screen {:?} at ({}, {})",
            &a[..3],
            &b[..3],
            on_screen.x,
            on_screen.y
        );
        // One step of slack: the window capture arrives premultiplied by its own
        // alpha and is un-premultiplied back, which rounds, while the opaque
        // screen capture never left full strength.  Measured: (250, 243, 225)
        // against (251, 244, 226).
        for channel in 0..3 {
            let difference = i32::from(a[channel]) - i32::from(b[channel]);
            assert!(
                difference.abs() <= 1,
                "channel {channel} of the window capture ({}) has to match the screen ({}) \
                 at the window's place",
                a[channel],
                b[channel]
            );
        }
    }
}

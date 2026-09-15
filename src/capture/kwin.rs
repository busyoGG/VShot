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
        let options = capture_options(cursor);
        let (read_end, write_end) = rustix::pipe::pipe().map_err(|error| {
            VshotError::KwinScreenShot(format!(
                "cannot create a pipe for KWin to render into: {error}"
            ))
        })?;
        // KWin renders on its own thread and can answer the call before the
        // whole frame has crossed the pipe — a 64 KiB pipe buffer against a
        // 132 MB frame, with the reply arriving first.  Draining from a
        // separate thread works whichever order KWin picks; reading only after
        // the reply would depend on that order and stall on a full pipe if the
        // compositor ever wrote inline.
        let reader = std::thread::spawn(move || read_to_end(read_end));
        let reply = self.connection.call_method(
            Some(SERVICE),
            PATH,
            Some(INTERFACE),
            "CaptureScreen",
            &(name, options, Fd::from(&write_end)),
        );
        // Only the compositor's copy of the write end may stay open, otherwise
        // the reader never sees the end of the pixel stream.
        drop(write_end);
        let reply = match reply {
            Ok(reply) => reply,
            // No pixels are coming, and KWin closes its end of the pipe when it
            // refuses, so the reader finishes on its own.  It is left detached
            // rather than joined so that reporting the refusal never waits on a
            // compositor that might not close anything.
            Err(error) => return Err(map_dbus_error(&error, name)),
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
        convert_premultiplied_bgra(&bytes, geometry)
    }
}

/// The option map handed to ScreenShot2.  KWin ignores keys it does not know —
/// and, as far as a headless session can tell, it accepts keys it should
/// reject — so a typo here cannot be caught by a probe.  `include-cursor` is
/// documented by KDE and passed through, but whether KWin actually draws the
/// pointer has not been verified: a `--virtual` output has no pointer to draw.
fn capture_options(cursor: bool) -> HashMap<&'static str, Value<'static>> {
    let mut options = HashMap::new();
    options.insert("include-cursor", Value::from(cursor));
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

/// Converts the payload ScreenShot2 writes into a pipe into the RGBA frame the
/// rest of vshot works with.
///
/// The bytes are premultiplied BGRA with a row stride that may be padded, and
/// the alpha is dropped: a screenshot shows what the compositor composited onto
/// an output, which is opaque by definition.  Restoring the alpha over black
/// means a region nothing was drawn into (raw `0, 0, 0, 0`) comes out black
/// rather than transparent.
fn convert_premultiplied_bgra(bytes: &[u8], geometry: CaptureGeometry) -> Result<Frame> {
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
            match alpha {
                0 => destination.copy_from_slice(&[0, 0, 0, 255]),
                255 => destination.copy_from_slice(&[red, green, blue, 255]),
                alpha => destination.copy_from_slice(&[
                    unpremultiply(red, alpha),
                    unpremultiply(green, alpha),
                    unpremultiply(blue, alpha),
                    255,
                ]),
            }
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

fn map_dbus_error(error: &zbus::Error, screen: &str) -> VshotError {
    if let zbus::Error::MethodError(name, detail, _) = error {
        return map_method_error(name.as_str(), detail.as_deref(), screen);
    }
    VshotError::KwinScreenShot(format!("the capture call failed: {error}"))
}

/// Turns a `ScreenShot2` error name into something the user can act on.  KDE's
/// authorization reply is the one a real session will hit, and the raw string
/// ("The process is not authorized to take a screenshot") reads like a prompt
/// waiting to be approved.  KWin has no such prompt, so the hint spells out the
/// desktop-file rule instead.
fn map_method_error(name: &str, detail: Option<&str>, screen: &str) -> VshotError {
    match name {
        "org.kde.KWin.ScreenShot2.Error.NoAuthorized" => {
            VshotError::ScreenshotDenied(not_authorized_hint())
        }
        "org.kde.KWin.ScreenShot2.Error.InvalidScreen" => VshotError::IncompleteTopology(format!(
            "KWin ScreenShot2 does not know the screen `{screen}`"
        )),
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
        let frame = convert_premultiplied_bgra(&bytes, geometry(3, 1, 12)).unwrap();
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
        let frame = convert_premultiplied_bgra(&bytes, geometry(1, 1, 4)).unwrap();
        assert_eq!(
            frame.pixel(crate::geometry::Point::new(0, 0)),
            Some([255, 0, 0, 255])
        );
    }

    #[test]
    fn a_transparent_pixel_becomes_black_and_opaque() {
        // An output region nothing was composited into arrives as 0, 0, 0, 0;
        // the screen is still opaque there, so the alpha becomes 255.
        let bytes = [0, 0, 0, 0, 0, 0, 0, 0];
        let frame = convert_premultiplied_bgra(&bytes, geometry(2, 1, 8)).unwrap();
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
    fn a_stride_wider_than_the_row_is_trimmed() {
        // Two padded rows: the padding bytes must not reach the frame, and the
        // second row must start at the stride, not right after the first.
        let bytes = [
            3, 2, 1, 255, 0xAA, 0xBB, 0xCC, 0xDD, // row 0 plus padding
            6, 5, 4, 255, 0xAA, 0xBB, 0xCC, 0xDD, // row 1 plus padding
        ];
        let frame = convert_premultiplied_bgra(&bytes, geometry(1, 2, 8)).unwrap();
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
            convert_premultiplied_bgra(&[0; 8], geometry(2, 1, 4)),
            Err(VshotError::KwinScreenShot(_))
        ));
        // A row that is never sent is a truncated frame, not a shorter image.
        assert!(matches!(
            convert_premultiplied_bgra(&[0; 4], geometry(1, 2, 4)),
            Err(VshotError::KwinScreenShot(_))
        ));
        // Trailing bytes beyond the announced frame are harmless.
        assert!(convert_premultiplied_bgra(&[0; 9], geometry(1, 2, 4)).is_ok());
    }

    #[test]
    fn the_cursor_option_is_sent_as_a_boolean() {
        let options = capture_options(true);
        assert_eq!(
            options.get("include-cursor"),
            Some(&Value::from(true)),
            "the KDE option name has to survive unchanged"
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
}

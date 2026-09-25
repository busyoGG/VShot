// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Window capture on niri, over niri's own IPC.
//!
//! niri is the one compositor where the rectangle-based routes cannot work: its
//! IPC reports window *sizes* but no absolute position for a tiled window.
//! `WindowLayout::tile_pos_in_workspace_view` is filled only for floating
//! windows — the tiled path sets it to `None` on purpose (`src/layout/tile.rs`,
//! `src/layout/scrolling.rs` in niri's tree, which only fills
//! `pos_in_scrolling_layout`, a pair of 1-based column/tile *indices*), and no
//! request reports the scroll offset of a workspace's view.  So "find the rect
//! in the frozen scene" is a guess there, and it is a guess that cannot even be
//! attempted for a long page.
//!
//! niri offers the exact answer instead, and this module drives it:
//!
//! * `Request::FocusedWindow` / `Request::PickWindow` name a window (the
//!   focused one, or the one the user points at with niri's own crosshair
//!   picker);
//! * `Action::ScreenshotWindow { id, path }` makes niri draw that window itself
//!   and write a PNG to an absolute path.
//!
//! The PNG is what niri renders for the window — its surface tree, popups
//! included — at the output's physical scale, and it carries no density of its
//! own, so the caller states the density (see [`window_scale`]).  niri also
//! always puts that image on the clipboard as part of the same action; that is
//! niri's own behaviour and not something this side can turn off.
//!
//! Which niri is asked is decided by [`owns_this_connection`], not by the
//! desktop variables: niri does not set `XDG_CURRENT_DESKTOP` itself (only a
//! display manager does), so a manually started session would otherwise be
//! misread and the routes below would never run.  The socket is named
//! `niri.$WAYLAND_DISPLAY.$PID.sock`, and niri hands `NIRI_SOCKET` to the
//! processes it starts — the display inside the socket name matching this
//! client's `WAYLAND_DISPLAY` is what says the answering niri is ours.

use std::collections::HashMap;
use std::ffi::OsString;
use std::io::ErrorKind;
use std::path::Path;
use std::process::Output;
use std::time::{Duration, Instant};

use serde_json::Value;

use crate::error::{Result, VshotError};
use crate::geometry::Rect;
use crate::model::Frame;

use super::window::{join_label, WindowCommand, WindowCommandRunner};

/// How long niri may take to write the window's PNG.  The action itself returns
/// `Handled` at once and the encoding happens on a thread inside niri, so the
/// file is the only completion signal there is; a failure inside niri is
/// logged there and never reaches the IPC reply, which is why this has to end
/// in a timeout rather than an error of its own.
const SCREENSHOT_TIMEOUT: Duration = Duration::from_secs(5);
/// Wait between two reads of that file.
const POLL_INTERVAL: Duration = Duration::from_millis(25);

/// One window as niri's IPC describes it.  `workspace_id` is what places it on
/// an output: niri reports the workspace → output mapping separately.
///
/// `tile_size` and `offset_in_tile` are what makes the decorations findable.
/// niri draws a window's border as part of the *tile*, not the window
/// (`Tile::render` calls `self.border.render` beside `window.render_normal`),
/// so its own `screenshot-window` is the window surface alone — no border.  The
/// tile is the window plus exactly those decorations, and this is where the two
/// are related: the border on each side is `offset_in_tile` wide, and the tile
/// is `tile_size` large.  (This is also what niri's IPC documents: "tile_size —
/// size of the tile this window is in, including decorations like borders",
/// "window_size — does not include niri decorations like borders".)
///
/// `tile_pos_in_workspace_view` is the tile's own position, and niri fills it
/// **only for a floating window** (`src/layout/floating.rs`) — a tiled one
/// leaves it unset (`src/layout/tile.rs`, `src/layout/scrolling.rs`) and states
/// a pair of column/tile indices instead.  So the window the rectangle routes
/// cannot place is exactly the one niri places itself, and
/// [`NiriWindow::render_origin`] is where that is turned into a pixel position.
#[derive(Clone, Debug, PartialEq)]
pub struct NiriWindow {
    pub id: u64,
    /// `app_id — title`, when niri reports either.
    pub label: String,
    /// The app id niri reports (the Wayland `app_id`, or the X11 class), kept
    /// apart from the label because a window recording matches one against the
    /// foreign-toplevel list's own `app_id` — the joined label cannot do that.
    pub app_id: String,
    /// The window's title, as niri reports it.  May be empty.
    pub title: String,
    pub workspace_id: Option<u64>,
    /// The tile's size in logical pixels, borders included.
    pub tile_size: Option<(f64, f64)>,
    /// Where the window's own geometry sits inside its tile, in logical
    /// pixels.  Each component is the border width on that axis — how far the
    /// decoration reaches out from the window on each side.
    pub offset_in_tile: Option<(f64, f64)>,
    /// The tile's position within its workspace view, in logical pixels.
    /// Filled for floating windows only; see the type docs.
    pub tile_pos_in_workspace_view: Option<(f64, f64)>,
}

impl NiriWindow {
    /// Where this window's own render starts on its output, in device pixels,
    /// when niri stated a position for it at all.
    ///
    /// niri fills `tile_pos_in_workspace_view` only for floating windows, and
    /// for those it *is* the answer the template match has to guess at: the
    /// tile's top-left corner in workspace-view coordinates, at the output's
    /// scale.  Adding `offset_in_tile` reaches the window's own surface, which
    /// is what `screenshot-window` renders — the tile's border is drawn around
    /// it and is not part of the render.
    ///
    /// The workspace view is the output: with the overview closed, a workspace
    /// is exactly the output's size and sits at its origin (`workspaces_render_geo`
    /// in niri's `src/layout/monitor.rs` centres a `view_size`-sized rectangle,
    /// which is zero offset at zoom 1), so these coordinates are output-local
    /// as they stand.
    ///
    /// `None` for a tiled window — niri does not place it, which is the whole
    /// reason the render has to be located on a capture — or when the numbers
    /// are not a position.  Both the position and the offset are rounded to
    /// whole device pixels, the way niri itself rounds the position before
    /// reporting it (`to_physical_precise_round`).
    pub fn render_origin(&self, scale: u32) -> Option<(i32, i32)> {
        let (pos_x, pos_y) = self.tile_pos_in_workspace_view?;
        let (offset_x, offset_y) = self.offset_in_tile?;
        if !pos_x.is_finite()
            || !pos_y.is_finite()
            || !offset_x.is_finite()
            || !offset_y.is_finite()
        {
            return None;
        }
        let scale = f64::from(scale.max(1));
        let round = |value: f64| (value * scale).round();
        // A position no i32 can hold is not a position; truncating it would put
        // the crop somewhere the window is not.
        let to_i32 = |value: f64| {
            (value >= f64::from(i32::MIN) && value <= f64::from(i32::MAX)).then_some(value as i32)
        };
        Some((
            to_i32(round(pos_x + offset_x))?,
            to_i32(round(pos_y + offset_y))?,
        ))
    }

    /// The tile's rectangle on its output in device pixels, for a window niri
    /// placed itself — the floating case [`Self::render_origin`] answers.
    ///
    /// `render_size` is niri's own render size; the same adjustment
    /// [`Self::tile_from_render`] describes is applied here, so a build that
    /// draws a drop shadow around the window has it taken off before the
    /// border goes on.  Unlike the located-render path there is no search
    /// involved: the position is niri's own answer, so this is arithmetic.
    ///
    /// `None` when niri stated no usable position, or the numbers do not
    /// describe a window inside a tile with an even border.
    pub fn tile_rect(&self, render_size: (u32, u32), scale: u32) -> Option<Rect> {
        let (surface_x, surface_y) = self.render_origin(scale)?;
        let (inset_x, inset_y, border_x, border_y) = self.tile_from_render(render_size, scale)?;
        let border_x = i64::from(border_x);
        let border_y = i64::from(border_y);
        let origin_x = i64::from(surface_x) - i64::from(inset_x) - border_x;
        let origin_y = i64::from(surface_y) - i64::from(inset_y) - border_y;
        let width = i64::from(render_size.0) - 2 * i64::from(inset_x) + 2 * border_x;
        let height = i64::from(render_size.1) - 2 * i64::from(inset_y) + 2 * border_y;
        Some(Rect::new(
            i32::try_from(origin_x).ok()?,
            i32::try_from(origin_y).ok()?,
            u32::try_from(width).ok()?,
            u32::try_from(height).ok()?,
        ))
    }

    /// How to turn the located render rectangle into the window's *tile* — the
    /// window plus its border.
    ///
    /// `render_size` is the size in device pixels of the render as it was found
    /// on the screen (and `scale` the output's device-pixels-per-logical-pixel).
    /// Returns `(inset, border)`: shrink the found rectangle by `inset` to reach
    /// the window's own surface, then grow the result by `border` on every side
    /// to reach the tile.  Both are in device pixels.
    ///
    /// Two niri versions differ here and both are handled: the render may be the
    /// window surface exactly (render size = `window_size · scale`, seen on
    /// 25.11-236) or carry a drop shadow around it (the 12px-per-side case seen
    /// on an earlier lab build), so the padding is measured from the found
    /// rectangle rather than assumed to be zero.
    ///
    /// `None` when niri stated no geometry, the numbers do not describe the
    /// window plus an even border on each side, or the render is smaller than
    /// the window it is supposed to show — in every one of those cases a
    /// guess would be worse than the plain window rectangle.
    pub fn tile_from_render(
        &self,
        render_size: (u32, u32),
        scale: u32,
    ) -> Option<(u32, u32, u32, u32)> {
        let (offset_x, offset_y) = self.offset_in_tile?;
        let (tile_w, tile_h) = self.tile_size?;
        if !offset_x.is_finite()
            || !offset_y.is_finite()
            || offset_x < 0.0
            || offset_y < 0.0
            || !tile_w.is_finite()
            || !tile_h.is_finite()
        {
            return None;
        }
        // The tile is the window plus its border on each side, so the window's
        // logical size follows from the two niri-documented fields.
        let window_w = tile_w - 2.0 * offset_x;
        let window_h = tile_h - 2.0 * offset_y;
        if window_w <= 0.0 || window_h <= 0.0 {
            return None;
        }
        let scale = f64::from(scale.max(1));
        // Integer logical window geometry scales to whole device pixels.
        let window_device = (
            (window_w * scale).round() as i64,
            (window_h * scale).round() as i64,
        );
        let (render_w, render_h) = (i64::from(render_size.0), i64::from(render_size.1));
        // The render may hold the window surface exactly or carry padding
        // (a shadow) around it.  Negative padding would mean the render is
        // smaller than the window it shows, which cannot be trusted.
        let padding_w = render_w - window_device.0;
        let padding_h = render_h - window_device.1;
        if padding_w < 0 || padding_h < 0 || padding_w % 2 != 0 || padding_h % 2 != 0 {
            return None;
        }
        let inset = ((padding_w / 2) as u32, (padding_h / 2) as u32);
        let border = (
            (offset_x * scale).round() as u32,
            (offset_y * scale).round() as u32,
        );
        Some((inset.0, inset.1, border.0, border.1))
    }
}

/// The focused window, or `None` when niri has none (a layer-shell surface can
/// hold the focus).
pub fn focused_window<R: WindowCommandRunner>(runner: &R) -> Result<Option<NiriWindow>> {
    let output = run(runner, &command(&["msg", "--json", "focused-window"]))?;
    parse_window(&output.stdout, "focused-window")
}

/// Is the niri that answers over `NIRI_SOCKET` the compositor of *this* client?
///
/// niri names its IPC socket after the Wayland display it created
/// (`$XDG_RUNTIME_DIR/niri.$WAYLAND_DISPLAY.$PID.sock`) and hands that path to
/// every process it starts, as `NIRI_SOCKET`.  A socket naming our own
/// `WAYLAND_DISPLAY` therefore proves that the niri behind it is the compositor
/// serving this connection — which is the one thing the desktop variables
/// cannot say.  Those are inherited: a niri started from a TTY has no display
/// manager to put its name there, and a niri nested inside another compositor
/// leaves the *outer* name in place.  Read as the outer compositor — or as
/// nothing at all — the routes that exist for niri are never asked, and
/// `window active` falls back to the pixel detector (a window under the
/// pointer, not the focused one) while `window pick` falls back to our own
/// overlay picker instead of niri's crosshair.
///
/// A socket naming a different display is somebody else's niri, and a path that
/// is not there is a stale variable: neither is an answer, so the desktop
/// variables get to decide after all.
pub fn owns_this_connection() -> bool {
    let (Ok(socket), Ok(display)) = (
        std::env::var("NIRI_SOCKET"),
        std::env::var("WAYLAND_DISPLAY"),
    ) else {
        return false;
    };
    socket_names_display(&socket, &display) && Path::new(&socket).exists()
}

/// The naming rule behind [`owns_this_connection`], on its own so it can be
/// tested without an environment: `niri.<display>.<pid>.sock`.
///
/// `WAYLAND_DISPLAY` is a socket *name* (`wayland-1`) or the absolute path of
/// one, in which case niri named its socket after the file name it used.
/// Everything is compared component by component rather than by prefix, so the
/// socket of `wayland-11` is not mistaken for one of `wayland-1`.
fn socket_names_display(socket: &str, display: &str) -> bool {
    let Some(name) = Path::new(socket).file_name() else {
        return false;
    };
    let Some(display) = Path::new(display).file_name() else {
        return false;
    };
    let display = display.to_string_lossy();
    if display.is_empty() {
        return false;
    }
    let name = name.to_string_lossy();
    let parts: Vec<&str> = name.split('.').collect();
    parts.len() == 4
        && parts[0] == "niri"
        && parts[1] == display
        && !parts[2].is_empty()
        && parts[2].chars().all(|digit| digit.is_ascii_digit())
        && parts[3] == "sock"
}

/// Asks niri to run its own window picker and waits for the answer.  This
/// blocks until the user clicks a window or cancels (`None`); niri draws only a
/// crosshair while it runs.
pub fn pick_window<R: WindowCommandRunner>(runner: &R) -> Result<Option<NiriWindow>> {
    let output = run(runner, &command(&["msg", "--json", "pick-window"]))?;
    parse_window(&output.stdout, "pick-window")
}

/// niri's window picture, made to look the way the screen showed it.
///
/// [`capture_window`] hands over the window rendered with its alpha channel, so
/// a translucent window comes out translucent, with none of the wallpaper that
/// showed through it.  The screen *did* show that wallpaper, and a fresh
/// capture of the window's own output holds exactly that composite — so the
/// render is located on the capture (a template match over its own pixels, see
/// [`super::window_blend`]) and the capture is cropped there.  The match is
/// also the consistency check between the two separate moments — render and
/// grab — and a window whose content moved on in between is re-rendered and
/// retried before giving up.  When the render cannot be located at all — a
/// window too translucent to verify, one that hangs off its output, one on a
/// workspace that is not visible — niri's own render is used as it is, and
/// stderr says so.
pub fn capture_composited<R: WindowCommandRunner>(
    grab_output: &mut dyn FnMut(&str, bool) -> Result<Frame>,
    runner: &R,
    window: &NiriWindow,
    cursor: bool,
    output_infos: &[crate::wayland::topology::OutputInfo],
) -> Result<Frame> {
    let mut render = capture_window(runner, window, cursor)?;
    let Some((name, scale)) = window_scale(runner, window)? else {
        eprintln!(
            "vshot: niri places window {} on no output, so its own (translucent) render is \
             used as it is",
            window.id
        );
        return Ok(render);
    };
    let Some(_) = output_infos.iter().find(|info| info.name == name) else {
        eprintln!(
            "vshot: output {name} was not captured, so niri's own (translucent) render of \
             window {} is used as it is",
            window.id
        );
        return Ok(render);
    };
    // The render and the grab are two separate moments — niri's IPC answers one
    // call at a time, so nothing can make them the same instant.  What makes
    // the crop trustworthy anyway is that *locating the render is the check*:
    // the screen pixel has to equal `render · α + background · (1−α)` at well
    // over a thousand sampled points before a crop is taken, so the frame only
    // comes out when it still agrees with the render.  When it does not, the
    // reason decides what happens next: a window whose own pixels moved on
    // between the two moments (a video, an animation) made the template stale,
    // and a fresh render — milliseconds old — is worth another try; a render
    // that is current but unverifiable (nearly invisible, off the output's
    // edge, invisible workspace) is a positional dead end that no retry helps.
    //
    // A floating window never has to be searched for: niri states its position
    // itself (`tile_pos_in_workspace_view` is filled for the floating space and
    // only there), so the crop is arithmetic.  That matters most for exactly
    // the window a terminal is — mostly one flat colour, which a template match
    // cannot pin down on a desktop of a similar shade (measured: 41% at the
    // window's own position, against 100% for the pixels that carry structure).
    //
    // The position is still verified before it is used.  It comes from a
    // different moment than the grab — a window dragged or animated in between
    // would otherwise be cropped where it used to be, silently, because the
    // arithmetic would succeed on the stale numbers.  The check is the same
    // sample-and-ratio test the search uses, so a position that verifies is as
    // trustworthy as a located one, and one that does not falls through to the
    // search below rather than to a guess.
    if let Some(rect) = window.tile_rect((render.size().width, render.size().height), scale) {
        let screen = grab_output(&name, cursor)?;
        let inside = rect.intersection(Rect::new(0, 0, screen.size().width, screen.size().height));
        let confirmed = window
            .render_origin(scale)
            .and_then(|(x, y)| super::window_blend::matches_at_position(&render, &screen, x, y));
        // `Some(false)` is the one verdict that rejects the stated position —
        // the render is demonstrably somewhere else.  `None` means the render
        // says nothing either way (a nearly flat window, which is exactly the
        // one that needs the stated position), and the position stands.
        match (inside, confirmed) {
            (Some(_), Some(false)) => eprintln!(
                "vshot: niri placed window {} where its render is not, so the position is \
                 searched for instead",
                window.id
            ),
            (Some(inside), _) => {
                eprintln!(
                    "vshot: niri placed window {} at ({}, {}), so the screen's own pixels are \
                     used",
                    window.id, inside.origin.x, inside.origin.y
                );
                return screen.crop(inside);
            }
            (None, _) => {
                eprintln!(
                    "vshot: niri places window {} off {name}, so its own (translucent) render is \
                     used as it is",
                    window.id
                );
                return Ok(render);
            }
        }
    }

    const ATTEMPTS: usize = 3;
    for attempt in 0..ATTEMPTS {
        // A fresh capture of the window's output: the screen already shows the
        // window composited over whatever was behind it.  The pointer is
        // captured too when `--cursor` asked for one, matching the pointer
        // niri may have drawn into the render.
        let screen = grab_output(&name, cursor)?;
        match super::window_blend::locate_window(&render, &screen) {
            Some(rect) => {
                // The render is the window surface alone (or that surface with
                // a shadow around it, on niri builds that draw one); the screen
                // shows the window *inside its tile*, the border included and
                // drawn by niri as part of the tile.  Growing the located
                // rectangle out to the tile is what puts that border in the
                // capture — see [`NiriWindow::tile_from_render`].
                let rect = match window.tile_from_render((rect.size.width, rect.size.height), scale)
                {
                    Some((inset_x, inset_y, border_x, border_y)) => Rect::new(
                        rect.origin.x + inset_x as i32 - border_x as i32,
                        rect.origin.y + inset_y as i32 - border_y as i32,
                        rect.size.width - 2 * inset_x + 2 * border_x,
                        rect.size.height - 2 * inset_y + 2 * border_y,
                    ),
                    None => rect,
                };
                eprintln!(
                    "vshot: niri's render of window {} was located on {name}, so the screen's \
                     own pixels are used",
                    window.id
                );
                return screen.crop(rect);
            }
            None if attempt + 1 == ATTEMPTS => break,
            None => {
                let fresh = capture_window(runner, window, cursor)?;
                if super::window_blend::renders_agree(&render, &fresh) {
                    // The template is current, yet no position verifies: the
                    // window itself is the problem, not its timing.
                    eprintln!(
                        "vshot: niri's render of window {} could not be located on {name} and \
                         the window is not changing, so its own (translucent) render is used \
                         as it is",
                        window.id
                    );
                    return Ok(fresh);
                }
                // Stale template: take the fresh one into the next try, where
                // the gap between render and grab is milliseconds.
                render = fresh;
            }
        }
    }
    eprintln!(
        "vshot: niri's render of window {} kept changing under the capture and no frame \
         could be located, so its own (translucent) render is used as it is",
        window.id
    );
    Ok(render)
}

/// The output the window sits on and the scale niri lays that output out at.
/// `None` when niri cannot place the window (a workspace on no output, or an
/// output that went away between the two queries).
pub fn window_scale<R: WindowCommandRunner>(
    runner: &R,
    window: &NiriWindow,
) -> Result<Option<(String, u32)>> {
    let Some(workspace_id) = window.workspace_id else {
        return Ok(None);
    };
    let workspaces = run(runner, &command(&["msg", "--json", "workspaces"]))?;
    let Some(name) = parse_workspace_outputs(&workspaces.stdout)?.remove(&workspace_id) else {
        return Ok(None);
    };
    let outputs = run(runner, &command(&["msg", "--json", "outputs"]))?;
    Ok(parse_output_scales(&outputs.stdout)?
        .remove(&name)
        .map(|scale| (name, scale)))
}

/// niri's own screenshot of one window, as a frame.
///
/// The file is written by niri asynchronously, so this reads it until it
/// decodes: a half-written PNG must never be handed on as a result.  `cursor`
/// asks niri to draw the pointer in, which only the builds that know the
/// `show-pointer` argument can do — see [`command_for_window`].
pub fn capture_window<R: WindowCommandRunner>(
    runner: &R,
    window: &NiriWindow,
    cursor: bool,
) -> Result<Frame> {
    capture_window_within(runner, window, cursor, SCREENSHOT_TIMEOUT)
}

/// [`capture_window`] with the wait for niri's file spelled out, so a test can
/// watch a niri that never writes without sitting out the real deadline.
fn capture_window_within<R: WindowCommandRunner>(
    runner: &R,
    window: &NiriWindow,
    cursor: bool,
    timeout: Duration,
) -> Result<Frame> {
    let file = tempfile::Builder::new()
        .prefix("vshot-niri-window-")
        .suffix(".png")
        .tempfile()
        .map_err(|error| VshotError::NiriScreenshot(format!("no temporary file: {error}")))?;
    let path = file.path().to_path_buf();

    let mut command = command_for_window(window.id, &path, cursor);
    // The first attempt is run raw rather than through `run`: a niri that
    // predates `--show-pointer` refuses the whole request, and that refusal is
    // the one failure worth retrying.
    let first = runner.run(&command)?;
    let output = if cursor && !first.status.success() && rejects_show_pointer(&first) {
        // Losing the pointer beats losing the screenshot.
        eprintln!(
            "vshot: this niri has no `--show-pointer` for screenshot-window, so the pointer \
             cannot be drawn into the window"
        );
        command = command_for_window(window.id, &path, false);
        runner.run(&command)?
    } else {
        first
    };
    if !output.status.success() {
        return Err(failed_action(&output));
    }

    wait_for_png(&path, timeout)
}

fn command(args: &[&str]) -> WindowCommand {
    WindowCommand {
        program: OsString::from("niri"),
        args: args.iter().map(OsString::from).collect(),
    }
}

/// `niri msg action screenshot-window` with the window's own id.  The path has
/// to be absolute — niri rejects anything else — and `--write-to-disk` is
/// stated rather than left to its default, because the media type in the
/// clipboard it writes anyway is nothing this side controls.
fn command_for_window(id: u64, path: &Path, cursor: bool) -> WindowCommand {
    let mut command = command(&[
        "msg",
        "action",
        "screenshot-window",
        "--id",
        &id.to_string(),
        "--write-to-disk",
        "true",
        "--path",
        &path.to_string_lossy(),
    ]);
    if cursor {
        command.args.push(OsString::from("--show-pointer"));
        command.args.push(OsString::from("true"));
    }
    command
}

/// Does this failure mean "this niri does not know `--show-pointer`"?  The
/// argument arrived after 25.11, and a build without it fails the request with
/// clap's "unexpected argument" complaint instead of ignoring it.
fn rejects_show_pointer(output: &Output) -> bool {
    let stderr = String::from_utf8_lossy(&output.stderr);
    stderr.contains("unexpected argument") && stderr.contains("show-pointer")
}

fn failed_action(output: &Output) -> VshotError {
    let stderr = String::from_utf8_lossy(&output.stderr);
    let detail = stderr.trim();
    VshotError::NiriScreenshot(if detail.is_empty() {
        format!(
            "`niri msg action screenshot-window` exited with {}",
            output.status
        )
    } else {
        detail.to_owned()
    })
}

/// Reads the screenshot until it is a whole PNG.  An empty or partly written
/// file fails to decode, which is exactly the state to wait through.
fn wait_for_png(path: &Path, timeout: Duration) -> Result<Frame> {
    let deadline = Instant::now() + timeout;
    loop {
        match std::fs::read(path) {
            Ok(bytes) if !bytes.is_empty() => {
                if let Ok(frame) = Frame::from_png(&bytes) {
                    return Ok(frame);
                }
            }
            Ok(_) => {}
            Err(error) if error.kind() == ErrorKind::NotFound => {}
            Err(error) => {
                return Err(VshotError::NiriScreenshot(format!(
                    "cannot read the screenshot niri wrote to {}: {error}",
                    path.display()
                )))
            }
        }
        if Instant::now() >= deadline {
            return Err(VshotError::NiriScreenshot(format!(
                "niri did not write a window screenshot to {} within {}s",
                path.display(),
                timeout.as_secs()
            )));
        }
        std::thread::sleep(POLL_INTERVAL);
    }
}

fn run<R: WindowCommandRunner>(runner: &R, command: &WindowCommand) -> Result<Output> {
    let output = runner.run(command)?;
    if !output.status.success() {
        return Err(VshotError::NiriScreenshot(command_failure(
            command, &output,
        )));
    }
    Ok(output)
}

fn command_failure(command: &WindowCommand, output: &Output) -> String {
    let stderr = String::from_utf8_lossy(&output.stderr);
    let detail = stderr.trim();
    if detail.is_empty() {
        format!(
            "`{}` exited with {}",
            command.program.to_string_lossy(),
            output.status
        )
    } else {
        detail.to_owned()
    }
}

/// One window out of niri's JSON, or `None` for the `null` that means "no
/// window" — nothing focused, or a pick the user cancelled.  Unknown fields are
/// ignored: niri adds them between releases and leaves the rest alone.
fn parse_window(bytes: &[u8], what: &str) -> Result<Option<NiriWindow>> {
    let value: Value = serde_json::from_slice(bytes).map_err(|error| {
        VshotError::NiriScreenshot(format!("niri's {what} reply is not JSON: {error}"))
    })?;
    if value.is_null() {
        return Ok(None);
    }
    let id = value.get("id").and_then(Value::as_u64).ok_or_else(|| {
        VshotError::NiriScreenshot(format!("niri's {what} reply names no window id"))
    })?;
    let app_id = value.get("app_id").and_then(Value::as_str).unwrap_or("");
    let title = value.get("title").and_then(Value::as_str).unwrap_or("");
    let layout = value.get("layout");
    let pair = |field: &str| {
        layout
            .and_then(|layout| layout.get(field))
            .and_then(|value| value.as_array())
            .and_then(|pair| {
                let [first, second] = pair.as_slice() else {
                    return None;
                };
                Some((first.as_f64()?, second.as_f64()?))
            })
    };
    Ok(Some(NiriWindow {
        id,
        label: join_label(app_id, title),
        app_id: app_id.to_owned(),
        title: title.to_owned(),
        workspace_id: value.get("workspace_id").and_then(Value::as_u64),
        tile_size: pair("tile_size"),
        offset_in_tile: pair("window_offset_in_tile"),
        tile_pos_in_workspace_view: pair("tile_pos_in_workspace_view"),
    }))
}

/// Workspace id → the output it is on, from `niri msg --json workspaces`.
fn parse_workspace_outputs(bytes: &[u8]) -> Result<HashMap<u64, String>> {
    let values: Vec<Value> = serde_json::from_slice(bytes).map_err(|error| {
        VshotError::NiriScreenshot(format!("niri's workspace list is not JSON: {error}"))
    })?;
    let mut workspaces = HashMap::new();
    for value in values {
        let (Some(id), Some(name)) = (
            value.get("id").and_then(Value::as_u64),
            value.get("output").and_then(Value::as_str),
        ) else {
            continue;
        };
        workspaces.insert(id, name.to_owned());
    }
    Ok(workspaces)
}

/// Output name → the scale niri lays it out at, from
/// `niri msg --json outputs`.  niri states a double (`2`, or `1.25` on a
/// fractional-scale screen) while everything in vshot counts whole device
/// pixels per logical pixel, so it is rounded here the same way KWin's is
/// (`capture::kwin::result_scale`).
fn parse_output_scales(bytes: &[u8]) -> Result<HashMap<String, u32>> {
    let value: Value = serde_json::from_slice(bytes).map_err(|error| {
        VshotError::NiriScreenshot(format!("niri's output list is not JSON: {error}"))
    })?;
    let Some(outputs) = value.as_object() else {
        return Err(VshotError::NiriScreenshot(
            "niri's output list is not an object keyed by output name".into(),
        ));
    };
    let mut scales = HashMap::new();
    for (name, output) in outputs {
        let scale = output
            .get("logical")
            .and_then(|logical| logical.get("scale"))
            .and_then(Value::as_f64);
        // A disabled output carries no `logical` block at all.
        if let Some(scale) = scale.filter(|scale| *scale >= 1.0) {
            scales.insert(name.clone(), scale.round() as u32);
        }
    }
    Ok(scales)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::geometry::Size;
    use std::cell::RefCell;
    use std::collections::VecDeque;
    use std::ffi::OsStr;
    use std::ffi::OsString;
    use std::os::unix::process::ExitStatusExt;
    use std::path::PathBuf;
    use std::process::ExitStatus;

    /// A focused tiled window, shaped exactly like niri's `Window` — notice
    /// `tile_pos_in_workspace_view: null`, which is what makes the rect route
    /// impossible and the screenshot route the answer.
    const FOCUSED_TILED: &str = r#"{
        "id": 12,
        "title": "t w",
        "app_id": "Alacritty",
        "pid": 4242,
        "workspace_id": 6,
        "is_focused": true,
        "is_floating": false,
        "is_urgent": false,
        "layout": {
            "pos_in_scrolling_layout": [1, 1],
            "tile_size": [800.0, 600.0],
            "window_size": [796, 596],
            "tile_pos_in_workspace_view": null,
            "window_offset_in_tile": [2.0, 2.0]
        },
        "focus_timestamp": {"secs": 1234, "nanos": 0}
    }"#;

    const FLOATING: &str = r#"{
        "id": 13,
        "title": "floating",
        "app_id": "kitty",
        "workspace_id": 6,
        "is_focused": false,
        "is_floating": true,
        "is_urgent": false,
        "layout": {
            "pos_in_scrolling_layout": null,
            "tile_size": [400.0, 300.0],
            "window_size": [400, 300],
            "tile_pos_in_workspace_view": [100.5, 200.0],
            "window_offset_in_tile": [0.0, 0.0]
        },
        "focus_timestamp": null
    }"#;

    const WORKSPACES: &str = r#"[
        {"id": 6, "idx": 1, "name": null, "output": "DP-2", "is_urgent": false,
         "is_active": true, "active_window_id": 12},
        {"id": 7, "idx": 2, "name": null, "output": "HDMI-A-1", "is_urgent": false,
         "is_active": false, "active_window_id": null}
    ]"#;

    const OUTPUTS: &str = r#"{
        "DP-2": {
            "name": "DP-2", "make": "Dell", "model": "U2720Q", "serial": "abc",
            "physical_size": [600, 340], "modes": [], "current_mode": 0,
            "is_custom_mode": false, "vrr_supported": false, "vrr_enabled": false,
            "logical": {"x": 0, "y": 0, "width": 2560, "height": 1440, "scale": 2.0,
                        "transform": "Normal"},
            "max_bpc": 10
        },
        "HDMI-A-1": {
            "name": "HDMI-A-1", "make": "Dell", "model": "U2415", "serial": "def",
            "physical_size": [520, 320], "modes": [], "current_mode": 0,
            "is_custom_mode": false, "vrr_supported": false, "vrr_enabled": false,
            "logical": {"x": 2560, "y": 0, "width": 1920, "height": 1200, "scale": 1.0,
                        "transform": "Normal"},
            "max_bpc": 8
        }
    }"#;

    #[test]
    fn parses_a_focused_tiled_window() {
        let window = parse_window(FOCUSED_TILED.as_bytes(), "focused-window")
            .unwrap()
            .expect("a window is focused");
        assert_eq!(window.id, 12);
        assert_eq!(window.label, "Alacritty — t w");
        assert_eq!(window.workspace_id, Some(6));
    }

    #[test]
    fn the_socket_names_the_display_it_was_created_for() {
        assert!(socket_names_display(
            "/run/user/1000/niri.wayland-1.2474.sock",
            "wayland-1"
        ));
        assert!(socket_names_display("niri.wayland-2.9.sock", "wayland-2"));
        // `WAYLAND_DISPLAY` is allowed to be the absolute path of the socket,
        // in which case niri named its own after the file name it used.
        assert!(socket_names_display(
            "/run/user/1000/niri.wayland-1.7.sock",
            "/run/user/1000/wayland-1"
        ));
    }

    #[test]
    fn another_displays_socket_is_not_ours() {
        // The nesting that matters: a compositor inside niri inherits the outer
        // `NIRI_SOCKET`, whose name carries the outer display.
        assert!(!socket_names_display(
            "/run/user/1000/niri.wayland-1.2474.sock",
            "wayland-2"
        ));
        // `wayland-1` is a prefix of `wayland-11`: components are compared, not
        // prefixes, so the socket of the other display never passes.
        assert!(!socket_names_display(
            "/run/user/1000/niri.wayland-11.44.sock",
            "wayland-1"
        ));
        assert!(!socket_names_display(
            "sway-ipc.1000.2474.sock",
            "wayland-1"
        ));
        // Names that only look like niri's: no display, no pid, no socket.
        assert!(!socket_names_display(
            "/run/user/1000/niri.sock",
            "wayland-1"
        ));
        assert!(!socket_names_display("niri.wayland-1.sock", "wayland-1"));
        assert!(!socket_names_display("niri.wayland-1.x.sock", "wayland-1"));
        assert!(!socket_names_display("niri.wayland-1.7.log", "wayland-1"));
        assert!(!socket_names_display("niri.wayland-1.7.sock", ""));
        assert!(!socket_names_display("", "wayland-1"));
    }

    #[test]
    fn the_environment_says_when_the_niri_is_ours() {
        // The one test here that touches the process environment: it is the
        // only reader of these two variables, and it puts them back, so a
        // parallel runner has nothing to trip over.
        use std::env;
        let saved = (env::var_os("NIRI_SOCKET"), env::var_os("WAYLAND_DISPLAY"));
        let dir = env::temp_dir().join(format!("vshot-niri-socket-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let socket = dir.join("niri.wayland-7.123.sock");
        std::fs::write(&socket, b"").unwrap();

        env::set_var("NIRI_SOCKET", &socket);
        env::set_var("WAYLAND_DISPLAY", "wayland-7");
        assert!(owns_this_connection(), "the socket names our display");

        env::set_var("WAYLAND_DISPLAY", "wayland-8");
        assert!(
            !owns_this_connection(),
            "another display's socket is not ours to ask"
        );

        env::set_var("WAYLAND_DISPLAY", "wayland-7");
        std::fs::remove_file(&socket).unwrap();
        assert!(
            !owns_this_connection(),
            "a path that is not there is a stale variable, not an answer"
        );

        for (name, value) in [("NIRI_SOCKET", saved.0), ("WAYLAND_DISPLAY", saved.1)] {
            match value {
                Some(value) => env::set_var(name, value),
                None => env::remove_var(name),
            }
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_rounded_floating_window_is_read_the_same_way() {
        let window = parse_window(FLOATING.as_bytes(), "pick-window")
            .unwrap()
            .expect("a window was picked");
        assert_eq!(window.id, 13);
        assert_eq!(window.label, "kitty — floating");
    }

    #[test]
    fn no_window_is_an_answer_and_not_a_failure() {
        // `niri msg --json focused-window` prints `null` when a layer-shell
        // surface holds the focus, and `pick-window` prints it on Esc.
        assert_eq!(parse_window(b"null", "focused-window").unwrap(), None);
        assert_eq!(parse_window(b"null\n", "pick-window").unwrap(), None);
    }

    #[test]
    fn unknown_fields_are_ignored() {
        // niri adds fields between releases and keeps the old ones, so a reply
        // from a newer compositor must still parse.
        let json = r#"{"id": 21, "app_id": "foot", "title": "shell", "workspace_id": 6,
                       "is_window_cast_target": false, "something_new": {"nested": [1, 2]}}"#;
        let window = parse_window(json.as_bytes(), "focused-window")
            .unwrap()
            .expect("a window is focused");
        assert_eq!(window.id, 21);
        assert_eq!(window.label, "foot — shell");
    }

    #[test]
    fn a_window_without_an_id_is_a_failure() {
        let error = parse_window(b"{\"app_id\": \"foot\"}", "focused-window").unwrap_err();
        assert!(error.to_string().contains("no window id"), "{error}");
    }

    #[test]
    fn the_windows_output_comes_from_its_workspace() {
        let runner = ScriptedRunner::new()
            .replying("msg --json workspaces", WORKSPACES.as_bytes())
            .replying("msg --json outputs", OUTPUTS.as_bytes());
        let window = parse_window(FOCUSED_TILED.as_bytes(), "focused-window")
            .unwrap()
            .unwrap();
        assert_eq!(
            window_scale(&runner, &window).unwrap(),
            Some(("DP-2".to_owned(), 2))
        );

        let floating = parse_window(FLOATING.as_bytes(), "focused-window")
            .unwrap()
            .unwrap();
        assert_eq!(
            window_scale(&runner, &floating).unwrap(),
            Some(("DP-2".to_owned(), 2)),
            "the floating window is on the same workspace"
        );
    }

    #[test]
    fn a_fractional_scale_is_rounded_like_kwins() {
        let outputs = r#"{"eDP-1": {"logical": {"scale": 1.25}}}"#;
        assert_eq!(
            parse_output_scales(outputs.as_bytes())
                .unwrap()
                .get("eDP-1"),
            Some(&1)
        );
    }

    #[test]
    fn a_disabled_output_has_no_scale() {
        // niri leaves `logical` out for an output it is not driving.
        let outputs = r#"{"DP-3": {"name": "DP-3", "current_mode": null}}"#;
        assert!(parse_output_scales(outputs.as_bytes()).unwrap().is_empty());
    }

    #[test]
    fn a_window_on_no_workspace_cannot_be_placed() {
        let runner = ScriptedRunner::new();
        let window = NiriWindow {
            id: 5,
            label: String::new(),
            app_id: String::new(),
            title: String::new(),
            workspace_id: None,
            tile_size: None,
            offset_in_tile: None,
            tile_pos_in_workspace_view: None,
        };
        assert_eq!(window_scale(&runner, &window).unwrap(), None);
        assert!(runner.asked().is_empty(), "nothing worth asking about");
    }

    #[test]
    fn the_screenshot_asks_for_the_id_and_an_absolute_path() {
        let runner = ScriptedRunner::new().writes_png();
        let window = NiriWindow {
            id: 12,
            label: String::new(),
            app_id: String::new(),
            title: String::new(),
            workspace_id: Some(6),
            tile_size: None,
            offset_in_tile: None,
            tile_pos_in_workspace_view: None,
        };
        capture_window(&runner, &window, false).unwrap();

        let asked = runner.asked();
        assert_eq!(asked.len(), 1, "{asked:?}");
        assert_eq!(asked[0][0], "niri");
        let (path, head) = asked[0].split_last().expect("a path is always passed");
        assert_eq!(
            head[1..].to_vec(),
            vec![
                "msg",
                "action",
                "screenshot-window",
                "--id",
                "12",
                "--write-to-disk",
                "true",
                "--path"
            ]
        );
        assert!(
            path.starts_with('/'),
            "niri rejects a relative path: {path}"
        );
        assert!(path.ends_with(".png"), "{path}");
    }

    #[test]
    fn the_cursor_is_asked_for_only_when_it_is_wanted() {
        let runner = ScriptedRunner::new().writes_png();
        let window = NiriWindow {
            id: 12,
            label: String::new(),
            app_id: String::new(),
            title: String::new(),
            workspace_id: Some(6),
            tile_size: None,
            offset_in_tile: None,
            tile_pos_in_workspace_view: None,
        };
        capture_window(&runner, &window, true).unwrap();
        assert!(runner.asked()[0].contains(&"--show-pointer".to_owned()));
    }

    #[test]
    fn a_niri_without_show_pointer_still_gives_its_picture() {
        // 25.11 has no `show-pointer` on this action, and clap refuses the
        // whole request rather than ignoring the argument.
        let runner = ScriptedRunner::new()
            .failing(
                "--show-pointer",
                "error: unexpected argument '--show-pointer' found",
            )
            .writes_png();
        let window = NiriWindow {
            id: 12,
            label: String::new(),
            app_id: String::new(),
            title: String::new(),
            workspace_id: Some(6),
            tile_size: None,
            offset_in_tile: None,
            tile_pos_in_workspace_view: None,
        };
        let frame = capture_window(&runner, &window, true).unwrap();
        assert_eq!(frame.size(), Size::new(40, 30));

        let asked = runner.asked();
        assert_eq!(asked.len(), 2, "the pointer request is retried without it");
        assert!(!asked[1].contains(&"--show-pointer".to_owned()));
        assert!(asked[1].contains(&"--id".to_owned()));
    }

    #[test]
    fn a_half_written_screenshot_is_waited_through() {
        // niri encodes on a thread and writes the file in one go, so a reader
        // can catch it empty or cut in half; only a whole PNG may be returned.
        let runner = ScriptedRunner::new().writes_partial_png_then_the_rest();
        let window = NiriWindow {
            id: 12,
            label: String::new(),
            app_id: String::new(),
            title: String::new(),
            workspace_id: Some(6),
            tile_size: None,
            offset_in_tile: None,
            tile_pos_in_workspace_view: None,
        };
        let frame = capture_window(&runner, &window, false).unwrap();
        assert_eq!(frame.size(), Size::new(40, 30));
    }

    #[test]
    fn a_niri_that_never_writes_says_so() {
        let runner = ScriptedRunner::new(); // succeeds, writes nothing
        let window = NiriWindow {
            id: 12,
            label: String::new(),
            app_id: String::new(),
            title: String::new(),
            workspace_id: Some(6),
            tile_size: None,
            offset_in_tile: None,
            tile_pos_in_workspace_view: None,
        };
        let error =
            capture_window_within(&runner, &window, false, Duration::from_millis(50)).unwrap_err();
        assert!(error.to_string().contains("did not write"), "{error}");
    }

    #[test]
    fn a_failed_action_reports_niris_own_words() {
        let runner = ScriptedRunner::new().failing("", "error: window 99 does not exist");
        let window = NiriWindow {
            id: 99,
            label: String::new(),
            app_id: String::new(),
            title: String::new(),
            workspace_id: Some(6),
            tile_size: None,
            offset_in_tile: None,
            tile_pos_in_workspace_view: None,
        };
        let error = capture_window(&runner, &window, false).unwrap_err();
        assert!(error.to_string().contains("does not exist"), "{error}");
    }

    /// One captured output, for `capture_composited`'s output check.
    fn test_output(name: &str) -> crate::wayland::topology::OutputInfo {
        test_output_sized(name, 400, 300)
    }

    /// [`test_output`] at a size that can hold a window niri placed somewhere
    /// other than the top-left corner.
    fn test_output_sized(
        name: &str,
        width: u32,
        height: u32,
    ) -> crate::wayland::topology::OutputInfo {
        crate::wayland::topology::OutputInfo {
            global_id: 1,
            name: name.to_owned(),
            geometry: crate::geometry::Rect::new(0, 0, width, height),
            pixel_size: Size::new(width, height),
            scale: 1,
            transform: wayland_client::protocol::wl_output::Transform::Normal,
        }
    }

    /// The grab closure's screen: a plain desktop with `window` composited at
    /// (20, 30), the way the compositor would.
    fn screen_showing(window: &Frame, at: crate::geometry::Point) -> Frame {
        screen_of(Size::new(400, 300), window, at)
    }

    /// [`screen_showing`] on a screen of the caller's size, for a window placed
    /// at a position a 400×300 output cannot hold.
    fn screen_of(screen_size: Size, window: &Frame, at: crate::geometry::Point) -> Frame {
        let mut screen = Frame::solid(screen_size, [240, 240, 240, 255]).unwrap();
        for y in 0..window.size().height {
            for x in 0..window.size().width {
                let Some(color) = window.pixel(crate::geometry::Point::new(x as i32, y as i32))
                else {
                    continue;
                };
                screen.blend_pixel_at(
                    i64::from(at.x) + i64::from(x),
                    i64::from(at.y) + i64::from(y),
                    color,
                );
            }
        }
        screen
    }

    fn placed_window() -> NiriWindow {
        NiriWindow {
            id: 12,
            label: String::new(),
            app_id: String::new(),
            title: String::new(),
            workspace_id: Some(6),
            tile_size: None,
            offset_in_tile: None,
            tile_pos_in_workspace_view: None,
        }
    }

    /// A window with niri's tile geometry: a 940x1012 window inside a
    /// 948x1020 tile, i.e. a 4-logical-pixel border on every side.  Those are
    /// the numbers a live niri reports for a focused tiled window with
    /// `border { width 4 }`.
    fn bordered_window() -> NiriWindow {
        NiriWindow {
            tile_size: Some((948.0, 1020.0)),
            offset_in_tile: Some((4.0, 4.0)),
            ..placed_window()
        }
    }

    /// A floating window, with the numbers a live niri 25.11-236 reported for
    /// one (`niri msg --json focused-window`): `tile_pos_in_workspace_view` is
    /// filled for the floating space and only there, and `offset_in_tile` is
    /// the same 4-logical-pixel border — a floating window keeps its border.
    fn floating_window() -> NiriWindow {
        NiriWindow {
            tile_pos_in_workspace_view: Some((598.5, 38.5)),
            ..bordered_window()
        }
    }

    #[test]
    fn a_floating_windows_position_is_turned_into_its_render_origin() {
        // The live numbers, and the position a template match was made to find
        // by hand from them: 598.5 logical + the 4-logical border, at scale 2,
        // is device pixel 1205 — an odd coordinate, which is exactly what a
        // stride-4 sweep can never land on.  (Cross-checked against a real
        // capture: the render matched the screen there on 1194 of 1194 samples,
        // while one pixel over it matched 41%.)
        assert_eq!(floating_window().render_origin(2), Some((1205, 85)));
        // At scale 1 the same window rounds the 602.5 logical position up, the
        // way `round` takes halves away from zero.
        assert_eq!(floating_window().render_origin(1), Some((603, 43)));
    }

    #[test]
    fn a_tiled_window_states_no_position_of_its_own() {
        // niri fills `tile_pos_in_workspace_view` only for the floating space;
        // a tiled window leaves it unset and the render has to be located on a
        // capture instead.
        assert_eq!(bordered_window().render_origin(2), None);
        assert_eq!(
            bordered_window().tile_rect((1880, 2024), 2),
            None,
            "no position, no tile rectangle"
        );
    }

    #[test]
    fn a_floating_windows_tile_rectangle_wraps_its_border_around_the_position() {
        // The render is the window surface (1880x2024), and the tile adds the
        // 8-device-pixel border on every side, reaching back from the surface
        // position: origin (1205-8, 85-8) = (1197, 77), size 1880+16 x 2024+16.
        assert_eq!(
            floating_window().tile_rect((1880, 2024), 2),
            Some(Rect::new(1197, 77, 1896, 2040))
        );
    }

    #[test]
    fn a_floating_window_off_its_output_is_not_placed() {
        // A window niri reports off the output cannot be cropped from it; the
        // caller falls back to the render instead of inventing a rectangle.
        let window = NiriWindow {
            tile_pos_in_workspace_view: Some((-5000.0, 38.5)),
            ..bordered_window()
        };
        let rect = window.tile_rect((1880, 2024), 2).expect("still arithmetic");
        assert_eq!(
            rect.intersection(Rect::new(0, 0, 1920, 1080)),
            None,
            "a rectangle off the output does not intersect it"
        );
    }

    #[test]
    fn a_position_that_is_not_a_number_is_not_a_position() {
        let window = NiriWindow {
            tile_pos_in_workspace_view: Some((f64::NAN, 38.5)),
            ..bordered_window()
        };
        assert_eq!(window.render_origin(2), None);
    }

    #[test]
    fn a_floating_window_is_cropped_at_the_position_niri_stated() {
        // The whole point: on the output, a floating window is cropped where
        // niri says it is, with no search at all.  The screen is a plain
        // desktop with the window's (flat) render composited at the position
        // niri called out, which is a frame a template match cannot place — the
        // window is mostly one colour.
        let window = floating_window();
        let render = frame_of_size(1880, 2024, [200, 194, 180, 204]);
        let at = crate::geometry::Point::new(1205, 85);
        let screen = screen_of(Size::new(3840, 2160), &render, at);
        let runner = ScriptedRunner::new()
            .replying("workspaces", WORKSPACES.as_bytes())
            .replying("outputs", OUTPUTS.as_bytes())
            .writes_png_sequence(vec![render.clone(), render.clone()]);
        let got = capture_composited(
            &mut |_name, _cursor| Ok(screen.clone()),
            &runner,
            &window,
            false,
            &[test_output_sized("DP-2", 3840, 2160)],
        )
        .unwrap();
        // The tile is the render plus the border on every side.
        assert_eq!(got.size(), Size::new(1896, 2040));
        assert_eq!(
            screenshot_requests(&runner),
            1,
            "a placed window is cropped straight away, with no retry"
        );
        assert_eq!(
            got.pixel(crate::geometry::Point::new(10, 10)),
            Some([208, 203, 192, 255]),
            "the crop is the screen's own pixels where niri placed the window — the flat \
             render composited over the desktop ((200·204 + 240·51)/255 = 208)"
        );
    }

    /// A frame of one colour, all of it opaque once composited.
    fn frame_of_size(width: u32, height: u32, color: [u8; 4]) -> Frame {
        Frame::solid(Size::new(width, height), color).unwrap()
    }

    /// A window render that carries structure — two blocks of unique pixels —
    /// so a stated position can be verified against it, and a search for it
    /// has one right answer rather than many.  The blocks sit far apart on both
    /// axes, so neither the horizontal nor the vertical position can drift.
    fn textured_float_render() -> Frame {
        let (width, height) = (1880u32, 2024u32);
        let mut pixels = Vec::with_capacity(width as usize * height as usize * 4);
        for y in 0..height {
            for x in 0..width {
                let in_block = (20..40).contains(&y) && (100..140).contains(&x)
                    || (1980..2000).contains(&y) && (900..940).contains(&x);
                if in_block {
                    // Every pixel distinct, so only the true position matches.
                    let value = ((x * 7 + y * 13) % 251) as u8;
                    pixels.extend_from_slice(&[
                        value,
                        value.wrapping_mul(3),
                        value.wrapping_add(9),
                        255,
                    ]);
                } else {
                    pixels.extend_from_slice(&[200, 194, 180, 204]);
                }
            }
        }
        Frame::new(Size::new(width, height), pixels).unwrap()
    }

    #[test]
    fn a_stale_stated_position_falls_back_to_searching_for_the_render() {
        // The stated position comes from a different moment than the grab: here
        // the window has since been dragged 300 device pixels right.  Using the
        // stale position would crop the desktop where the window *was*, so the
        // verification has to reject it and the search has to take over — and
        // the search finds the window where it really is.
        let window = floating_window();
        let render = textured_float_render();
        let at = crate::geometry::Point::new(1505, 85);
        let screen = screen_of(Size::new(3840, 2160), &render, at);
        let runner = ScriptedRunner::new()
            .replying("workspaces", WORKSPACES.as_bytes())
            .replying("outputs", OUTPUTS.as_bytes())
            .writes_png_sequence(vec![render.clone(), render.clone()]);
        let got = capture_composited(
            &mut |_name, _cursor| Ok(screen.clone()),
            &runner,
            &window,
            false,
            &[test_output_sized("DP-2", 3840, 2160)],
        )
        .unwrap();
        // The crop follows the search, so it sits at the moved position rather
        // than the stated one: the tile is the render plus its border, and the
        // render's unique block is present at the offset the move implies.  (An
        // exact byte comparison would be too strict: the refinement searches
        // whole device pixels around the coarse winner and may settle one
        // pixel off, which is well inside the border.)
        assert_eq!(got.size(), Size::new(1896, 2040));
        // The block lives at (100..140, 20..40) inside the render, so within
        // the tile it starts 8 device pixels further in on each axis.  If the
        // crop had used the stale position, the window would be 300 device
        // pixels away and this block would not be here.
        let block = got
            .pixel(crate::geometry::Point::new(108 + 10, 28 + 5))
            .expect("the tile holds the block");
        assert_ne!(
            block,
            [208, 203, 192, 255],
            "the crop is the window, not the flat desktop where it used to be"
        );
        assert_eq!(block[3], 255, "the block is opaque");
    }

    #[test]
    fn a_verified_stated_position_is_used_without_a_search() {
        // The complementary half: the render *is* where niri said it is, so the
        // verification passes and the stated position is used.  One screenshot
        // is asked for — the crop itself needs none — and the result is the
        // stated rectangle, not a searched-for one.
        let window = floating_window();
        let render = textured_float_render();
        let at = crate::geometry::Point::new(1205, 85);
        let screen = screen_of(Size::new(3840, 2160), &render, at);
        let runner = ScriptedRunner::new()
            .replying("workspaces", WORKSPACES.as_bytes())
            .replying("outputs", OUTPUTS.as_bytes())
            .writes_png_sequence(vec![render.clone()]);
        let got = capture_composited(
            &mut |_name, _cursor| Ok(screen.clone()),
            &runner,
            &window,
            false,
            &[test_output_sized("DP-2", 3840, 2160)],
        )
        .unwrap();
        let expected = screen
            .crop(Rect::new(1197, 77, 1896, 2040))
            .expect("the stated tile is inside the screen");
        assert_eq!(got.pixels(), expected.pixels());
        assert_eq!(
            screenshot_requests(&runner),
            1,
            "the render is handed over once, before the verification"
        );
    }

    #[test]
    fn a_render_without_padding_grows_to_the_tile_by_its_border() {
        // The 25.11-236 render is the window surface exactly: 940x1012 logical
        // at scale 2 is 1880x2024 device pixels, and the 4-logical-pixel border
        // is 8 device pixels on each side.
        let (inset_x, inset_y, border_x, border_y) = bordered_window()
            .tile_from_render((1880, 2024), 2)
            .expect("a bordered window has a computable tile");
        assert_eq!(
            (inset_x, inset_y),
            (0, 0),
            "the render is the window itself"
        );
        assert_eq!((border_x, border_y), (8, 8), "4 logical pixels at scale 2");
    }

    #[test]
    fn a_shadowed_render_is_inset_before_the_border_grows_out() {
        // The earlier lab build drew a 12px-per-side shadow around the window,
        // so the located rectangle is 24px larger on each axis; that padding
        // has to come off before the border goes on, or the crop would be
        // 12px too large on every side.
        let (inset_x, inset_y, border_x, border_y) = bordered_window()
            .tile_from_render((1880 + 24, 2024 + 24), 2)
            .expect("a shadowed render still describes the tile");
        assert_eq!((inset_x, inset_y), (12, 12), "the shadow is not the window");
        assert_eq!((border_x, border_y), (8, 8));
    }

    #[test]
    fn a_borderless_window_states_a_zero_border() {
        // `border { off }`: tile and window are the same size, so the crop must
        // not invent a border around a window that has none.
        let window = NiriWindow {
            tile_size: Some((940.0, 1012.0)),
            offset_in_tile: Some((0.0, 0.0)),
            ..placed_window()
        };
        assert_eq!(window.tile_from_render((1880, 2024), 2), Some((0, 0, 0, 0)));
    }

    #[test]
    fn a_render_smaller_than_the_window_is_not_turned_into_a_tile() {
        // The render cannot be smaller than the window it shows; such numbers
        // are not the pair they are assumed to be, and a wrong rectangle is
        // worse than none.
        assert_eq!(bordered_window().tile_from_render((1000, 1000), 2), None);
    }

    #[test]
    fn a_window_without_tile_geometry_keeps_the_plain_rectangle() {
        // An older niri (or a fullscreen window with no tile) reports no
        // geometry: the located rectangle stands as it is.
        assert_eq!(placed_window().tile_from_render((1880, 2024), 2), None);
    }

    #[test]
    fn a_fractional_border_is_rounded_to_whole_device_pixels() {
        // niri's IPC notes borders are logical and may be fractional (a 2
        // physical-pixel border is 1.6 logical at scale 1.25).  The window here
        // is 800x600 logical, so at scale 2 the render is 1600x1200 device
        // pixels and the 1.6-logical border rounds to 3 device pixels.
        let window = NiriWindow {
            tile_size: Some((803.2, 603.2)),
            offset_in_tile: Some((1.6, 1.6)),
            ..placed_window()
        };
        assert_eq!(window.tile_from_render((1600, 1200), 2), Some((0, 0, 3, 3)));
    }

    fn screenshot_requests(runner: &ScriptedRunner) -> usize {
        runner
            .asked()
            .iter()
            .filter(|command| command.contains(&"--id".to_owned()))
            .count()
    }

    #[test]
    fn a_moving_window_is_retried_with_a_fresh_render() {
        // The window is animating: the first render (red) is stale by the time
        // the screen is grabbed — which already shows the blue state.  The
        // stability check tells a stale template from a positional dead end,
        // the fresh render is located, and the crop is the screen's own pixels
        // at the window's true position.
        let red = Frame::solid(Size::new(60, 40), [200, 30, 30, 255]).unwrap();
        let blue = Frame::solid(Size::new(60, 40), [30, 30, 200, 255]).unwrap();
        let runner = ScriptedRunner::new()
            .replying("workspaces", WORKSPACES.as_bytes())
            .replying("outputs", OUTPUTS.as_bytes())
            .writes_png_sequence(vec![red, blue.clone()]);
        let screen = screen_showing(&blue, crate::geometry::Point::new(20, 30));
        let got = capture_composited(
            &mut |_name, _cursor| Ok(screen.clone()),
            &runner,
            &placed_window(),
            false,
            &[test_output("DP-2")],
        )
        .unwrap();
        assert_eq!(got.size(), blue.size());
        assert_eq!(
            got.pixel(crate::geometry::Point::new(10, 10)),
            Some([30, 30, 200, 255]),
            "the crop is the screen's own pixels, background included"
        );
        assert_eq!(
            screenshot_requests(&runner),
            2,
            "the stale render is retried once"
        );
    }

    #[test]
    fn a_stable_render_that_cannot_be_located_is_not_retried_into_nothing() {
        // The window is not on the screen at all (invisible workspace, off the
        // output) and its content does not change between renders: the second
        // render only confirms stability, and the render is handed back as it
        // is — no third try could locate what is not there.
        let red = Frame::solid(Size::new(60, 40), [200, 30, 30, 255]).unwrap();
        let runner = ScriptedRunner::new()
            .replying("workspaces", WORKSPACES.as_bytes())
            .replying("outputs", OUTPUTS.as_bytes())
            .writes_png_sequence(vec![red.clone(), red.clone()]);
        let screen = Frame::solid(Size::new(400, 300), [240, 240, 240, 255]).unwrap();
        let got = capture_composited(
            &mut |_name, _cursor| Ok(screen.clone()),
            &runner,
            &placed_window(),
            false,
            &[test_output("DP-2")],
        )
        .unwrap();
        assert_eq!(got.pixels(), red.pixels());
        assert_eq!(
            screenshot_requests(&runner),
            2,
            "one stability render, then the positional dead end is accepted"
        );
    }

    #[test]
    fn a_window_that_keeps_changing_falls_back_to_its_freshest_render() {
        // Content changing under every attempt: the loop runs out, and the
        // newest render — the closest thing to what the user sees — is the
        // answer, not an error.
        let frames = vec![
            Frame::solid(Size::new(60, 40), [200, 30, 30, 255]).unwrap(),
            Frame::solid(Size::new(60, 40), [30, 200, 30, 255]).unwrap(),
            Frame::solid(Size::new(60, 40), [30, 30, 200, 255]).unwrap(),
        ];
        let freshest = frames[2].clone();
        let runner = ScriptedRunner::new()
            .replying("workspaces", WORKSPACES.as_bytes())
            .replying("outputs", OUTPUTS.as_bytes())
            .writes_png_sequence(frames);
        let screen = Frame::solid(Size::new(400, 300), [240, 240, 240, 255]).unwrap();
        let got = capture_composited(
            &mut |_name, _cursor| Ok(screen.clone()),
            &runner,
            &placed_window(),
            false,
            &[test_output("DP-2")],
        )
        .unwrap();
        assert_eq!(got.pixels(), freshest.pixels());
        assert_eq!(
            screenshot_requests(&runner),
            3,
            "every attempt gets one render"
        );
    }

    /// A runner that answers with fixed JSON, records every command and can
    /// write a PNG into the `--path` it is given.
    struct ScriptedRunner {
        repl: RefCell<Vec<(&'static str, Vec<u8>)>>,
        fail_with: RefCell<Vec<(&'static str, String)>>,
        asked: RefCell<Vec<Vec<String>>>,
        /// One PNG per screenshot request, handed out in order: a window whose
        /// content moves on between requests.
        pngs: RefCell<VecDeque<Vec<u8>>>,
        write_half_first: bool,
    }

    impl ScriptedRunner {
        fn new() -> Self {
            Self {
                repl: RefCell::new(Vec::new()),
                fail_with: RefCell::new(Vec::new()),
                asked: RefCell::new(Vec::new()),
                pngs: RefCell::new(VecDeque::new()),
                write_half_first: false,
            }
        }

        /// Answers the command matching `needle`, or any command when the
        /// needle is empty.
        fn replying(self, needle: &'static str, stdout: &[u8]) -> Self {
            self.repl.borrow_mut().push((needle, stdout.to_vec()));
            self
        }

        /// Fails the command matching `needle` with this stderr.
        fn failing(self, needle: &'static str, stderr: &'static str) -> Self {
            self.fail_with
                .borrow_mut()
                .push((needle, stderr.to_owned()));
            self
        }

        /// Succeeds and writes a whole PNG to the requested path.
        fn writes_png(self) -> Self {
            self.writes_png_sequence(vec![
                Frame::solid(Size::new(40, 30), [10, 20, 30, 255]).unwrap()
            ])
        }

        /// Succeeds and writes each given render in turn, one per screenshot
        /// request.
        fn writes_png_sequence(self, frames: Vec<Frame>) -> Self {
            *self.pngs.borrow_mut() = frames
                .into_iter()
                .map(|frame| frame.to_png().unwrap())
                .collect();
            self
        }

        /// Writes the first half, then the rest from another thread: the
        /// reader has to survive catching the file in between.
        fn writes_partial_png_then_the_rest(self) -> Self {
            let runner = self.writes_png();
            Self {
                write_half_first: true,
                ..runner
            }
        }

        fn asked(&self) -> Vec<Vec<String>> {
            self.asked.borrow().clone()
        }

        fn writes_to(&self, command: &WindowCommand) -> Option<PathBuf> {
            let needle = OsString::from("--path");
            let index = command.args.iter().position(|arg| *arg == needle)?;
            command.args.get(index + 1).map(PathBuf::from)
        }
    }

    impl WindowCommandRunner for ScriptedRunner {
        fn run(&self, command: &WindowCommand) -> Result<Output> {
            self.asked.borrow_mut().push(
                std::iter::once(command.program.to_string_lossy().into_owned())
                    .chain(
                        command
                            .args
                            .iter()
                            .map(|arg| arg.to_string_lossy().into_owned()),
                    )
                    .collect(),
            );
            for (needle, stderr) in self.fail_with.borrow().iter() {
                let line = command
                    .args
                    .join(OsStr::new(" "))
                    .to_string_lossy()
                    .into_owned();
                let matches = needle.is_empty() || line.contains(*needle);
                if matches {
                    return Ok(Output {
                        status: ExitStatus::from_raw(1 << 8),
                        stdout: Vec::new(),
                        stderr: stderr.clone().into_bytes(),
                    });
                }
            }
            let stdout = self
                .repl
                .borrow()
                .iter()
                .find(|(needle, _)| {
                    let line = command
                        .args
                        .join(OsStr::new(" "))
                        .to_string_lossy()
                        .into_owned();
                    needle.is_empty() || line.contains(*needle)
                })
                .map(|(_, stdout)| stdout.clone())
                .unwrap_or_default();

            // Only a screenshot request consumes a render: the metadata
            // queries carry no --path and must leave the sequence alone.
            if let Some(path) = self.writes_to(command) {
                if let Some(png) = self.pngs.borrow_mut().pop_front() {
                    if self.write_half_first {
                        let half = png.len() / 2;
                        std::fs::write(&path, &png[..half]).unwrap();
                        let path = path.clone();
                        std::thread::spawn(move || {
                            std::thread::sleep(Duration::from_millis(60));
                            std::fs::write(&path, png).unwrap();
                        });
                    } else {
                        std::fs::write(&path, &png).unwrap();
                    }
                }
            }

            Ok(Output {
                status: ExitStatus::from_raw(0),
                stdout,
                stderr: Vec::new(),
            })
        }
    }
}

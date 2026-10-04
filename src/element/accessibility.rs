// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! The accessibility tree, as a source of selectable regions.
//!
//! A window is one rectangle; the widgets inside it are many.  Those come from
//! AT-SPI, over D-Bus.  This is the client that reads them and the conversion
//! that turns them into [`RegionNode`]s, so the picker offers a button or a
//! panel the way it already offers a window.
//!
//! This is the source that is asked first, because it is the only one that
//! *knows* rather than guesses: a toolkit's tree names its widgets and reports
//! their real bounds.  It is also the one that most often cannot answer at all —
//! a program that draws its own UI has no tree to expose, and Chromium and
//! Electron only build one when launched with `--force-renderer-accessibility`.
//!
//! Two things about AT-SPI on Wayland shape everything here.
//!
//! First, **its coordinates are window-relative**, whatever the coordinate type
//! asks for.  `Component.GetExtents` answers the same numbers for `SCREEN` and
//! for `WINDOW`, because a Wayland client cannot know where the compositor put
//! it.  Global position therefore has to come from somewhere else — the
//! compositor's own window list — and every rect here is
//! `window origin + element rect`.  See `src/capture/window.rs` for that side.
//!
//! Second, **each accessible is addressed by a bus name and a path of its
//! own**, handed back by `GetChildAtIndex` as `(so)`.  There is no one
//! destination to talk to: every node carries its own.

use zbus::blocking::connection::Builder;
use zbus::blocking::{Connection, Proxy};
use zbus::zvariant::{ObjectPath, OwnedObjectPath, OwnedValue};

use crate::capture::window::WindowCandidate;
use crate::error::Result;
use crate::geometry::{Point, Rect, Size};
use crate::selection_region::{RegionKind, RegionNode};

use super::{ElementRequest, ElementSource};

/// How long one query may take before the answer is given up on.
///
/// An application that registers with AT-SPI and then never answers holds a
/// D-Bus method call open indefinitely, and the tree walk is a call per node --
/// so without this a single unresponsive window stops the picker dead instead
/// of simply having no elements.  Measured against a well-behaved tree the
/// whole walk takes a few milliseconds, so this is generous.
const METHOD_TIMEOUT: std::time::Duration = std::time::Duration::from_millis(500);

/// How many nodes a whole walk may visit.
///
/// A tree is not unbounded in practice, but a misreporting one -- a child
/// count read as a huge number, or a cycle -- would otherwise keep the walk
/// going for as long as the desktop is up.  The cap turns that into an
/// ordinary "no elements" answer.
const MAX_NODES: usize = 4096;

/// How deep a walk may go.
///
/// A web page's content is genuinely deep — measured on a Chromium tab, HTML
/// elements sit between 9 and 23 levels down, and the tree reached 26 — so a
/// small cap here is what decides whether the *inside of a page* can be picked
/// at all.  This is set past what a browser produces rather than at what looks
/// reasonable for a native app.
///
/// Depth alone is not the guard against a runaway walk; `MAX_NODES` is, since
/// it bounds the work however the tree is shaped.  Measured on that same tab,
/// the whole 753-node tree takes about a tenth of a second, so the budget is
/// what a pathological tree would run into long before this.
const MAX_DEPTH: u32 = 32;

/// An extent wider or taller than this is not a widget.  Hidden pages of a
/// notebook report their size as uninitialized memory — one measured at
/// 724708414 pixels wide — and a rect like that would be drawn across the whole
/// screen.
const MAX_EXTENT: i32 = 1 << 16;

/// One accessible, as AT-SPI addresses it: a bus name plus a path.
#[derive(Clone, Debug)]
struct Accessible {
    bus: String,
    path: String,
}

impl Accessible {
    fn root(bus: impl Into<String>) -> Self {
        Self {
            bus: bus.into(),
            path: "/org/a11y/atspi/accessible/root".to_string(),
        }
    }
}

/// A live connection to the accessibility bus.
///
/// AT-SPI does not live on the session bus.  `org.a11y.Bus` on the session bus
/// hands out the address of a second bus, and everything else happens there.
pub struct Accessibility {
    connection: Connection,
}

impl Accessibility {
    /// Opens the accessibility bus, or says why it could not.
    ///
    /// A desktop with no AT-SPI is not an error the caller should fail on —
    /// picking falls back to whole windows — so the reason is returned rather
    /// than logged here.
    pub fn connect() -> std::result::Result<Self, String> {
        let session = Connection::session().map_err(|error| format!("no session bus: {error}"))?;
        let address: String = session
            .call_method(
                Some("org.a11y.Bus"),
                "/org/a11y/bus",
                Some("org.a11y.Bus"),
                "GetAddress",
                &(),
            )
            .map_err(|error| format!("org.a11y.Bus gave no address: {error}"))?
            .body()
            .deserialize()
            .map_err(|error| format!("the a11y bus address was not a string: {error}"))?;
        let connection = Builder::address(address.as_str())
            .map_err(|error| format!("the a11y bus address was unusable: {error}"))?
            // Without this an application that registers and then never answers
            // holds a call open forever, and the tree walk is a call per node.
            .method_timeout(METHOD_TIMEOUT)
            .build()
            .map_err(|error| format!("cannot reach the a11y bus: {error}"))?;
        Ok(Self { connection })
    }

    /// The frames of every application, with the tree under each.
    ///
    /// Each frame carries the pid of the application that owns it, which is how
    /// the caller matches a frame to a compositor window: both sides know the
    /// process, and only the pair of them is a region the user can point at.
    /// A title is carried too, as the fallback for a compositor that reports no
    /// pid.
    pub fn frames(&self) -> std::result::Result<Vec<AccessibilityFrame>, String> {
        let root = Accessible::root("org.a11y.atspi.Registry");
        let count = self.child_count(&root).unwrap_or(0);
        let mut frames = Vec::new();
        for index in 0..count {
            let Some(application) = self.child_at(&root, index) else {
                continue;
            };
            // The pid belongs to the application, not to each of its frames:
            // one process can own several toplevels (a browser's windows, its
            // popovers), and every one of them is that process.
            let pid = self.pid(&application).unwrap_or(0);
            let application_count = self.child_count(&application).unwrap_or(0);
            for frame_index in 0..application_count {
                let Some(frame) = self.child_at(&application, frame_index) else {
                    continue;
                };
                let name = self.name(&frame).unwrap_or_default();
                let rect = self
                    .extents(&frame)
                    .and_then(|(x, y, width, height)| to_rect(x, y, width, height));
                frames.push(AccessibilityFrame {
                    accessible: frame,
                    title: name,
                    pid,
                    rect,
                });
            }
        }
        Ok(frames)
    }

    /// The elements inside `frame`, as regions in global logical pixels.
    ///
    /// `bounds` is the window's own global rectangle — the compositor's, not
    /// the frame's.  Every rect here is the window-relative extent shifted to
    /// global, then clipped to `bounds` so a widget that hangs outside its
    /// window does not offer a region off the side of it.
    ///
    /// Clipping to the compositor's rectangle rather than the frame's own
    /// extent matters: a frame may report itself larger than the window the
    /// compositor placed — one measured at 58 pixels above its window's top —
    /// and clipping to that would keep exactly the out-of-window rects the
    /// clip is for.
    pub fn elements(
        &self,
        frame: &AccessibilityFrame,
        bounds: Rect,
    ) -> std::result::Result<Vec<RegionNode>, String> {
        let origin = bounds.origin;
        let mut elements = Vec::new();
        let mut budget = MAX_NODES;
        let count = self.child_count(&frame.accessible).unwrap_or(0);
        for index in 0..count {
            let Some(child) = self.child_at(&frame.accessible, index) else {
                continue;
            };
            if let Some(node) = self.node(&child, origin, bounds, 0, &mut budget) {
                elements.push(node);
            }
            if budget == 0 {
                break;
            }
        }
        Ok(elements)
    }

    /// One accessible and its subtree, or `None` when it has no usable
    /// geometry.  A node with no rect cannot be pointed at, so it and
    /// everything under it is left out — which is what keeps the degenerate
    /// tenth of a real tree out of the picker.
    fn node(
        &self,
        accessible: &Accessible,
        origin: Point,
        frame: Rect,
        depth: u32,
        // Nodes left to visit across the whole walk.  A tree that misreports
        // its size would otherwise keep the walk going indefinitely; spending
        // the budget ends it as an ordinary short answer.
        budget: &mut usize,
    ) -> Option<RegionNode> {
        *budget = budget.saturating_sub(1);
        if *budget == 0 {
            return None;
        }
        let (x, y, width, height) = self.extents(accessible)?;
        let mut rect = to_rect(x, y, width, height)?;
        rect = rect.translate(origin).ok()?;
        rect = rect.clamp_to(frame)?;
        if rect.is_empty() {
            return None;
        }

        let role = self.role_name(accessible).unwrap_or_default();
        let name = self.name(accessible).unwrap_or_default();
        let label = match (role.as_str(), name.as_str()) {
            ("", "") => String::new(),
            ("", name) => name.to_string(),
            (role, "") => role.to_string(),
            (role, name) => format!("{role} {name}"),
        };

        let mut node = RegionNode::leaf(RegionKind::Element, rect, label);
        if depth + 1 < MAX_DEPTH {
            let count = self.child_count(accessible).unwrap_or(0);
            let mut children = Vec::new();
            for index in 0..count {
                let Some(child) = self.child_at(accessible, index) else {
                    continue;
                };
                if let Some(child_node) = self.node(&child, origin, frame, depth + 1, budget) {
                    children.push(child_node);
                }
                if *budget == 0 {
                    break;
                }
            }
            node = node.with_children(children);
        }
        Some(node)
    }

    fn proxy<'a>(
        &'a self,
        accessible: &'a Accessible,
        interface: &'static str,
    ) -> Option<Proxy<'a>> {
        let path = ObjectPath::try_from(accessible.path.as_str()).ok()?;
        Proxy::new(&self.connection, accessible.bus.as_str(), path, interface).ok()
    }

    /// Reads one property through an explicit `Properties.Get`.
    ///
    /// zbus's own `get_property` asks for `GetAll` first, and the AT-SPI root
    /// answers that with an empty signature, so the property never comes back.
    fn property<T>(&self, accessible: &Accessible, interface: &str, name: &str) -> Option<T>
    where
        T: TryFrom<OwnedValue>,
    {
        let proxy = self.proxy(accessible, "org.freedesktop.DBus.Properties")?;
        let reply = proxy.call_method("Get", &(interface, name)).ok()?;
        let (value,): (OwnedValue,) = reply.body().deserialize().ok()?;
        T::try_from(value).ok()
    }

    fn child_count(&self, accessible: &Accessible) -> Option<i32> {
        let count: i32 = self.property(accessible, "org.a11y.atspi.Accessible", "ChildCount")?;
        Some(count.clamp(0, 4096))
    }

    fn name(&self, accessible: &Accessible) -> Option<String> {
        self.property(accessible, "org.a11y.atspi.Accessible", "Name")
    }

    /// The process behind an application, asked of the bus rather than of the
    /// application.
    ///
    /// AT-SPI has no property for this.  Its `org.a11y.atspi.Application.Id`
    /// reads like one and is not: measured, it answers 143 for an Edge whose
    /// pid is 205344 — it is the application's own sequence number.  The pid
    /// comes from the bus daemon instead, which knows the process behind every
    /// connection: `GetConnectionUnixProcessID` on the application's unique
    /// name.  Verified against `pgrep` for two Edge processes.
    fn pid(&self, accessible: &Accessible) -> Option<i32> {
        let reply = self
            .connection
            .call_method(
                Some("org.freedesktop.DBus"),
                "/org/freedesktop/DBus",
                Some("org.freedesktop.DBus"),
                "GetConnectionUnixProcessID",
                &(accessible.bus.as_str()),
            )
            .ok()?;
        let pid: u32 = reply.body().deserialize().ok()?;
        i32::try_from(pid).ok()
    }

    fn child_at(&self, accessible: &Accessible, index: i32) -> Option<Accessible> {
        let proxy = self.proxy(accessible, "org.a11y.atspi.Accessible")?;
        let reply = proxy.call_method("GetChildAtIndex", &(index)).ok()?;
        let (bus, path): (String, OwnedObjectPath) = reply.body().deserialize().ok()?;
        Some(Accessible {
            bus,
            path: path.to_string(),
        })
    }

    fn role_name(&self, accessible: &Accessible) -> Option<String> {
        let proxy = self.proxy(accessible, "org.a11y.atspi.Accessible")?;
        let reply = proxy.call_method("GetRoleName", &()).ok()?;
        reply.body().deserialize::<String>().ok()
    }

    /// The accessible's extent, **relative to its toplevel**.  Coordinate type
    /// 1 is `WINDOW`; `SCREEN` answers the same thing on Wayland, so asking for
    /// it would only suggest a meaning the number does not have.
    fn extents(&self, accessible: &Accessible) -> Option<(i32, i32, i32, i32)> {
        let proxy = self.proxy(accessible, "org.a11y.atspi.Component")?;
        let reply = proxy.call_method("GetExtents", &(1u32)).ok()?;
        reply.body().deserialize::<(i32, i32, i32, i32)>().ok()
    }
}

/// One toplevel the accessibility bus reports.
pub struct AccessibilityFrame {
    accessible: Accessible,
    /// The frame's name, which is the window's title.
    pub title: String,
    /// The pid of the application that owns this frame, or 0 when it reported
    /// none.  This is what identifies the window: see [`elements_for_window`].
    pub pid: i32,
    /// The frame's own extent, which is its size and — only by accident — a
    /// position.  Callers use the size and take the position from the
    /// compositor.
    pub rect: Option<Rect>,
}

/// `width`/`height` as a rect, or `None` for the extents that are not: the
/// zero-sized and absent ones, and the ones whose size is uninitialized memory.
fn to_rect(x: i32, y: i32, width: i32, height: i32) -> Option<Rect> {
    if width <= 0 || height <= 0 || width > MAX_EXTENT || height > MAX_EXTENT {
        return None;
    }
    let size = Size::new(u32::try_from(width).ok()?, u32::try_from(height).ok()?);
    Some(Rect::new(x, y, size.width, size.height))
}

/// The accessibility source: the tree a toolkit exposes for its window.
///
/// The one source that knows rather than guesses, and therefore the first one
/// asked.  It declines — `None` — for a window with no tree, which is the
/// common case: a program that draws its own UI has none, and Chromium and
/// Electron only build one when launched with `--force-renderer-accessibility`.
pub struct AccessibilitySource;

impl ElementSource for AccessibilitySource {
    /// The elements of one window, or `None` when accessibility cannot answer.
    ///
    /// The window is the compositor's own description of it: the pid says
    /// *which* window, and its `geometry` is the origin AT-SPI cannot supply.
    /// Both halves are needed and neither source has both.  The frame is not
    /// used — this source reads the toolkit, not the screen.
    fn elements(&self, request: &ElementRequest<'_>) -> Option<Vec<RegionNode>> {
        let window = request.window;
        let accessibility = Accessibility::connect().ok()?;
        let frames = accessibility.frames().ok()?;
        let frame = match_frame(&frames, window)?;
        accessibility.elements(frame, window.geometry).ok()
    }
}

/// Which accessibility frame is this compositor window.
///
/// The pid is the answer, and it is asked first: both sides mean the same
/// process, so a match cannot be wrong.  A title is not — an application names
/// its toplevel twice, once for the compositor and once for AT-SPI, and is free
/// to say different things.  Chromium does exactly that: a pinned tab is
/// `… - 已固定 - Chromium` to AT-SPI while the compositor reports
/// `… - Chromium`.  Requiring the titles to be equal there failed the match,
/// the window got no elements, and picking silently kept offering the whole
/// window — the bug this function exists to not have.
///
/// The title is still used, as a fallback, because a compositor that reports no
/// pid leaves nothing else to match on (KWin without `kdotool`).  Then the
/// frame has to agree about the size as well, since two windows of one
/// application easily share a title.
fn match_frame<'a>(
    frames: &'a [AccessibilityFrame],
    window: &WindowCandidate,
) -> Option<&'a AccessibilityFrame> {
    // Every branch below insists the size agrees.  An accessibility frame's
    // extent is the toplevel's own size, which is what the compositor reports
    // for the window, so a frame of another size is another window however its
    // name reads -- and this is what keeps one application's several toplevels
    // apart when their titles are alike or empty.
    let same_size = |frame: &AccessibilityFrame| {
        frame
            .rect
            .is_some_and(|rect| rect.size == window.geometry.size)
    };

    if window.pid > 0 {
        if let Some(frame) = frames.iter().find(|frame| {
            frame.pid == window.pid && frame.title == window.title && same_size(frame)
        }) {
            return Some(frame);
        }
        // One process can own several toplevels — a browser's windows and its
        // popovers — and the compositor's own title for this one may be the
        // decorated form AT-SPI does not use.  The size settles it.
        if let Some(frame) = frames
            .iter()
            .find(|frame| frame.pid == window.pid && same_size(frame))
        {
            return Some(frame);
        }
    }

    if window.title.is_empty() {
        return None;
    }
    if let Some(frame) = frames
        .iter()
        .find(|frame| frame.title == window.title && same_size(frame))
    {
        return Some(frame);
    }
    // A title that only starts with the same stem, with the size to confirm it.
    let stem = title_stem(&window.title);
    if stem.is_empty() {
        return None;
    }
    frames.iter().find(|frame| {
        let frame_stem = title_stem(&frame.title);
        !frame_stem.is_empty() && frame_stem.starts_with(&stem) && same_size(frame)
    })
}

/// A title with its trailing ` - <something>` suffix removed, which is how an
/// application appends its own name to a document's title — and, for a browser,
/// how it appends a tab's state too: a pinned tab is `… - 已固定 - Chromium`
/// to AT-SPI while the compositor reports `… - Chromium`.
fn title_stem(title: &str) -> String {
    match title.rfind(" - ") {
        Some(at) if at > 0 && at + 3 < title.len() => title[..at].trim().to_string(),
        _ => title.trim().to_string(),
    }
}

/// Keeps [`Result`] honest: this module is best-effort and returns `Option`s.
#[allow(dead_code)]
type Unused = Result<()>;

#[cfg(test)]
mod tests {
    use super::*;

    /// The bug this matching exists for: Chromium names a pinned tab's toplevel
    /// `… - 已固定 - Chromium` to AT-SPI while the compositor reports
    /// `… - Chromium`.  Requiring equal titles dropped the window's whole
    /// element tree, and picking silently kept offering the whole window.
    #[test]
    fn a_browser_that_renames_its_toplevel_is_still_matched() {
        let window = candidate("WorkBuddy 用量看板 - Chromium", 4117827, 2550, 1397);
        let frames = vec![
            frame(
                "WorkBuddy 用量看板 - 已固定 - Chromium",
                4117827,
                2550,
                1397,
            ),
            frame("", 4117827, 1790, 88),
        ];
        let matched = match_frame(&frames, &window).expect("the pinned tab's window");
        assert_eq!(matched.title, "WorkBuddy 用量看板 - 已固定 - Chromium");
    }

    #[test]
    fn the_pid_answers_before_the_title_is_looked_at() {
        // A title that matches another window's, but the wrong pid: the pid is
        // the identity, so the frame of that pid is the window.
        let window = candidate("shared title", 100, 800, 600);
        let frames = vec![
            frame("shared title", 200, 800, 600),
            frame("different title entirely", 100, 800, 600),
        ];
        let matched = match_frame(&frames, &window).expect("matched by pid");
        assert_eq!(matched.pid, 100);
    }

    /// One process owns several toplevels -- a browser's windows and its
    /// popovers.  The size is what says which of them the compositor listed.
    #[test]
    fn one_process_with_several_toplevels_is_split_by_size() {
        let window = candidate("anything", 100, 800, 600);
        let frames = vec![
            frame("popover", 100, 200, 100),
            frame("the window", 100, 800, 600),
        ];
        let matched = match_frame(&frames, &window).expect("the size agrees");
        assert_eq!(matched.title, "the window");
    }

    /// A compositor that reports no pid (KWin without `kdotool`) leaves the
    /// title as the only thing to match on, and then the size has to confirm it.
    #[test]
    fn without_a_pid_the_title_still_matches_when_the_size_agrees() {
        let window = candidate("a document - App", 0, 800, 600);
        let frames = vec![frame("a document - App", 0, 800, 600)];
        assert!(match_frame(&frames, &window).is_some());

        // Same stem, different size: a different window of the same app, and
        // not the one the compositor listed.
        let frames = vec![frame("a document - App", 0, 400, 300)];
        assert!(match_frame(&frames, &window).is_none());
    }

    #[test]
    fn an_empty_title_with_no_pid_matches_nothing() {
        let window = candidate("", 0, 800, 600);
        let frames = vec![frame("", 0, 800, 600)];
        assert!(match_frame(&frames, &window).is_none());
    }

    fn candidate(title: &str, pid: i32, width: u32, height: u32) -> WindowCandidate {
        WindowCandidate {
            geometry: Rect::new(0, 0, width, height),
            label: String::new(),
            app_id: String::new(),
            title: title.to_string(),
            pid,
            handle: None,
        }
    }

    fn frame(title: &str, pid: i32, width: u32, height: u32) -> AccessibilityFrame {
        AccessibilityFrame {
            accessible: Accessible {
                bus: ":1.0".into(),
                path: "/org/a11y/atspi/accessible/1".into(),
            },
            title: title.to_string(),
            pid,
            rect: Some(Rect::new(0, 0, width, height)),
        }
    }

    #[test]
    fn degenerate_extents_are_not_regions() {
        // A hidden notebook page measured at 724708414 pixels wide, and
        // zero-sized nodes are the commonest kind of all.
        assert!(to_rect(0, 0, 0, 10).is_none());
        assert!(to_rect(0, 0, 10, -4).is_none());
        assert!(to_rect(0, 0, 724_708_414, 0).is_none());
        assert_eq!(to_rect(3, 4, 10, 20), Some(Rect::new(3, 4, 10, 20)));
    }

    /// Needs a live desktop: `cargo test -- --ignored a11y`.
    ///
    /// Asserts the two things that only a real window can show — that the
    /// accessibility bus is reachable and that a frame's title matches a
    /// compositor window's — and prints what it found so the geometry can be
    /// eyeballed against `hyprctl clients`.
    #[test]
    #[ignore = "needs a live desktop with accessibility enabled"]
    fn the_real_desktop_has_frames_with_titles() {
        let accessibility = match Accessibility::connect() {
            Ok(accessibility) => accessibility,
            Err(reason) => panic!(
                "cannot reach the accessibility bus: {reason}\n\
                 is it on?  busctl --user set-property org.a11y.Bus /org/a11y/bus \
                 org.a11y.Status IsEnabled b true"
            ),
        };
        let frames = accessibility.frames().expect("frames");
        assert!(
            !frames.is_empty(),
            "no frame at all: is any GTK/Qt app open?"
        );

        let named: Vec<_> = frames
            .iter()
            .filter(|frame| !frame.title.is_empty())
            .collect();
        assert!(!named.is_empty(), "every frame is unnamed");
        for frame in &named {
            let rect = frame
                .rect
                .map(|rect| format!("{}x{}", rect.size.width, rect.size.height))
                .unwrap_or_else(|| "none".into());
            println!("frame {:?} size={rect}", frame.title);
        }

        // The compositor's own list holds only the windows it is showing, and
        // a window parked on a special workspace is not one of them.  What
        // this asserts is that the two descriptions of a window name the same
        // window, so it asks `hyprctl` directly rather than going through the
        // visibility filter.
        let titles = compositor_window_titles();
        for title in &titles {
            println!("  compositor window title {:?}", title);
        }
        if titles.is_empty() {
            println!("no compositor window list; skipping the match assertion");
            return;
        }
        let matched = named.iter().any(|frame| titles.contains(&frame.title));
        assert!(
            matched,
            "no compositor window title matches an accessibility frame title"
        );
    }

    /// Needs a live desktop: `cargo test -- --ignored a11y`.
    ///
    /// The real test of the composition: element rects must come out in global
    /// coordinates, inside their window — not left at the `0,0` AT-SPI reports.
    #[test]
    #[ignore = "needs a live desktop with accessibility enabled"]
    fn element_rects_are_global_and_inside_their_window() {
        let accessibility = Accessibility::connect().expect("connect");
        let frames = accessibility.frames().expect("frames");

        // `hyprctl clients` gives every window with its global position, which
        // is exactly what AT-SPI cannot supply.  Taking the geometry straight
        // from it means the composition is checked against the compositor's own
        // answer for the window the frame belongs to.
        let windows = compositor_windows();
        let mut checked = 0;
        for (title, geometry) in &windows {
            let Some(frame) = frames.iter().find(|frame| frame.title == *title) else {
                continue;
            };
            let origin = Point::new(geometry.left(), geometry.top());
            let elements = accessibility.elements(frame, *geometry).expect("elements");
            let flat = Flatten::flatten(&elements);
            println!("window {title:?} at {origin:?}: {} elements", flat.len());
            for element in flat {
                assert!(
                    element.rect.intersection(*geometry).is_some(),
                    "element {:?} at {:?} is outside its window {:?}",
                    element.label,
                    element.rect,
                    geometry
                );
                assert!(
                    element.rect.left() >= geometry.left() && element.rect.top() >= geometry.top(),
                    "element {:?} at {:?} was not shifted into global \
                     coordinates (window starts at {:?})",
                    element.label,
                    element.rect,
                    origin
                );
                checked += 1;
            }
        }
        assert!(checked > 0, "no element was checked: no matching window");
    }

    /// Every window `hyprctl clients` reports, as `(title, geometry)`.
    ///
    /// Deliberately not [`crate::capture::window::ProcessWindowProvider`]:
    /// that list is filtered to the windows the compositor is *showing*, and a
    /// window on a special workspace is not among them — which would leave a
    /// perfectly good window unmatched in a test that only wants to know
    /// whether the two descriptions agree.
    fn compositor_windows() -> Vec<(String, Rect)> {
        let output = match std::process::Command::new("hyprctl")
            .args(["clients", "-j"])
            .output()
        {
            Ok(output) => output,
            Err(_) => return Vec::new(),
        };
        let clients = match serde_json::from_slice::<serde_json::Value>(&output.stdout)
            .ok()
            .and_then(|value| value.as_array().cloned())
        {
            Some(clients) => clients,
            None => return Vec::new(),
        };
        clients
            .iter()
            .filter_map(|client| {
                let title = client.get("title")?.as_str()?.to_string();
                let at = client.get("at")?.as_array()?;
                let size = client.get("size")?.as_array()?;
                let x = at.first()?.as_i64()?;
                let y = at.get(1)?.as_i64()?;
                let width = size.first()?.as_u64()?;
                let height = size.get(1)?.as_u64()?;
                let rect = Rect::new(
                    i32::try_from(x).ok()?,
                    i32::try_from(y).ok()?,
                    u32::try_from(width).ok()?,
                    u32::try_from(height).ok()?,
                );
                Some((title, rect))
            })
            .collect()
    }

    fn compositor_window_titles() -> Vec<String> {
        compositor_windows()
            .into_iter()
            .map(|(title, _)| title)
            .collect()
    }
}

/// Walks a node and everything under it, for callers that want the whole tree
/// flat — a test counting every element, say.
#[cfg(test)]
trait Flatten<'a> {
    fn flatten(&'a self) -> Vec<&'a RegionNode>;
}

#[cfg(test)]
impl<'a> Flatten<'a> for Vec<RegionNode> {
    fn flatten(&'a self) -> Vec<&'a RegionNode> {
        fn walk<'a>(node: &'a RegionNode, out: &mut Vec<&'a RegionNode>) {
            out.push(node);
            for child in &node.children {
                walk(child, out);
            }
        }
        let mut all = Vec::new();
        for node in self {
            walk(node, &mut all);
        }
        all
    }
}

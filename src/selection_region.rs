// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! The tree of selectable regions, and the currency every source builds.
//!
//! Interactive picking used to be one flat list of windows, each from a
//! compositor of its own.  A region is now a *tree*: a window is one level, the
//! UI elements inside it another, and a future source (a monitor, say) another
//! still.  Every level is one currency, so whatever produced a region can be
//! walked the same way.
//!
//! Two properties matter to everything downstream:
//!
//! * A node's rect is always in **global logical pixels**, the space the
//!   captured scene and the pointer use.  A source that knows only relative
//!   geometry — an element inside its window — adds the origin itself, so no
//!   caller ever has to know which source a rect came from.
//! * A node's `children` are the level below it.  An empty `children` just
//!   means the node is a leaf.
//!
//! Which node the highlight is on, and how the wheel moves between levels, is
//! the helper's business (`ui/session_protocol.hpp`'s `ElementTree`); this is
//! the shape the levels are handed over in.

use crate::geometry::Rect;

/// Which source produced a node.  A picker shows it because the step from a
/// window to its elements is a step between kinds, not only between levels.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RegionKind {
    /// A toplevel window, from the compositor's own list or the pixel fallback.
    Window,
    /// A widget inside a window, from the accessibility tree.
    Element,
    /// One output.  Not produced yet; named so the tree has the shape it will
    /// grow into.
    Monitor,
}

/// One selectable region, with the regions inside it.
///
/// The rect is global, and the children carry global rects of their own.
/// `label` is what the picker shows the user — a window's `class — title`, an
/// element's role and name.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RegionNode {
    pub kind: RegionKind,
    pub rect: Rect,
    pub label: String,
    /// The regions one level down, in the source's own order.  Empty for a
    /// leaf.
    pub children: Vec<RegionNode>,
}

impl RegionNode {
    /// A region of one kind with nothing inside it.
    pub fn leaf(kind: RegionKind, rect: Rect, label: impl Into<String>) -> Self {
        Self {
            kind,
            rect,
            label: label.into(),
            children: Vec::new(),
        }
    }

    /// The same region carrying the regions inside it.
    pub fn with_children(mut self, children: Vec<RegionNode>) -> Self {
        self.children = children;
        self
    }
}

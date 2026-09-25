// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#![allow(dead_code)]

use crate::error::{Result, VshotError};
use crate::geometry::Rect;
use crate::model::{Frame, SceneSnapshot};

// Cropping a rectangle of the desktop is deliberately not a helper here: which
// pixels it comes from — the one output holding it whole, or the composed scene
// — is the rule in `crate::main::crop_native`, and it has to be asked in one
// place.  The composed scene is stretched to the highest scale in the layout,
// so cropping it directly is what doubles a selection made on a lower-density
// screen.  What is left here is the rest of the selection rules.

pub fn crop_monitor(scene: &SceneSnapshot, name: &str) -> Result<Frame> {
    let output = scene
        .output_by_name(name)
        .ok_or_else(|| VshotError::InvalidGeometry(format!("unknown monitor `{name}`")))?;
    Ok(output.frame.clone())
}

pub fn crop_output(scene: &SceneSnapshot, global_id: u32) -> Result<Frame> {
    scene.crop_output(global_id)
}

pub fn validate_selection(scene: &SceneSnapshot, geometry: Rect) -> Result<Rect> {
    let _ = scene.crop(geometry)?;
    Ok(geometry)
}

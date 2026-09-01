#![allow(dead_code)]

use crate::error::{Result, VshotError};
use crate::geometry::Rect;
use crate::model::{Frame, SceneSnapshot};

pub fn crop_fixed(scene: &SceneSnapshot, geometry: Rect) -> Result<Frame> {
    scene.crop(geometry)
}

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

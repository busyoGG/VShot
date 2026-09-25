// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#![allow(dead_code)]

use std::fmt;
use std::str::FromStr;

use crate::error::{Result, VshotError};

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Hash)]
pub struct Point {
    pub x: i32,
    pub y: i32,
}

impl Point {
    pub const fn new(x: i32, y: i32) -> Self {
        Self { x, y }
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Hash)]
pub struct Size {
    pub width: u32,
    pub height: u32,
}

impl Size {
    pub const fn new(width: u32, height: u32) -> Self {
        Self { width, height }
    }

    pub fn area(self) -> Result<usize> {
        usize::try_from(self.width)
            .ok()
            .and_then(|width| {
                usize::try_from(self.height)
                    .ok()
                    .and_then(|height| width.checked_mul(height))
            })
            .ok_or_else(|| VshotError::InvalidGeometry("size is too large".to_string()))
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, Hash)]
pub struct Rect {
    pub origin: Point,
    pub size: Size,
}

impl Rect {
    pub const fn new(x: i32, y: i32, width: u32, height: u32) -> Self {
        Self {
            origin: Point::new(x, y),
            size: Size::new(width, height),
        }
    }

    pub const fn left(self) -> i32 {
        self.origin.x
    }

    pub const fn top(self) -> i32 {
        self.origin.y
    }

    pub fn right(self) -> Result<i32> {
        self.origin
            .x
            .checked_add(
                i32::try_from(self.size.width)
                    .map_err(|_| VshotError::InvalidGeometry("width exceeds i32".into()))?,
            )
            .ok_or_else(|| VshotError::InvalidGeometry("rectangle right edge overflows".into()))
    }

    pub fn bottom(self) -> Result<i32> {
        self.origin
            .y
            .checked_add(
                i32::try_from(self.size.height)
                    .map_err(|_| VshotError::InvalidGeometry("height exceeds i32".into()))?,
            )
            .ok_or_else(|| VshotError::InvalidGeometry("rectangle bottom edge overflows".into()))
    }

    pub fn is_empty(self) -> bool {
        self.size.width == 0 || self.size.height == 0
    }

    pub fn contains(self, point: Point) -> bool {
        self.right()
            .is_ok_and(|right| point.x >= self.left() && point.x < right)
            && self
                .bottom()
                .is_ok_and(|bottom| point.y >= self.top() && point.y < bottom)
    }

    pub fn intersection(self, other: Self) -> Option<Self> {
        let right = i64::from(self.right().ok()?.min(other.right().ok()?));
        let bottom = i64::from(self.bottom().ok()?.min(other.bottom().ok()?));
        let left = i64::from(self.left().max(other.left()));
        let top = i64::from(self.top().max(other.top()));
        if right <= left || bottom <= top {
            None
        } else {
            Some(Self::new(
                i32::try_from(left).ok()?,
                i32::try_from(top).ok()?,
                u32::try_from(right - left).ok()?,
                u32::try_from(bottom - top).ok()?,
            ))
        }
    }

    pub fn translate(self, delta: Point) -> Result<Self> {
        Ok(Self::new(
            self.origin
                .x
                .checked_add(delta.x)
                .ok_or_else(|| VshotError::InvalidGeometry("translated x overflows".into()))?,
            self.origin
                .y
                .checked_add(delta.y)
                .ok_or_else(|| VshotError::InvalidGeometry("translated y overflows".into()))?,
            self.size.width,
            self.size.height,
        ))
    }

    pub fn clamp_to(self, bounds: Self) -> Option<Self> {
        self.intersection(bounds)
    }
}

impl fmt::Display for Rect {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{},{} {}x{}",
            self.origin.x, self.origin.y, self.size.width, self.size.height
        )
    }
}

impl FromStr for Rect {
    type Err = VshotError;

    fn from_str(value: &str) -> Result<Self> {
        parse_geometry(value)
    }
}

pub fn parse_geometry(value: &str) -> Result<Rect> {
    let value = value.trim();
    let (position, dimensions) = value
        .split_once(char::is_whitespace)
        .ok_or_else(|| VshotError::InvalidGeometry("expected `x,y widthxheight`".into()))?;
    if position.is_empty() || dimensions.trim().is_empty() {
        return Err(VshotError::InvalidGeometry(
            "expected `x,y widthxheight`".into(),
        ));
    }
    let (x, y) = position
        .split_once(',')
        .ok_or_else(|| VshotError::InvalidGeometry("expected x,y position".into()))?;
    let (width, height) = dimensions
        .trim()
        .split_once('x')
        .ok_or_else(|| VshotError::InvalidGeometry("expected widthxheight size".into()))?;
    let x = x
        .parse::<i32>()
        .map_err(|_| VshotError::InvalidGeometry("x must be an integer".into()))?;
    let y = y
        .parse::<i32>()
        .map_err(|_| VshotError::InvalidGeometry("y must be an integer".into()))?;
    let width = width
        .parse::<u32>()
        .map_err(|_| VshotError::InvalidGeometry("width must be a positive integer".into()))?;
    let height = height
        .parse::<u32>()
        .map_err(|_| VshotError::InvalidGeometry("height must be a positive integer".into()))?;
    if width == 0 || height == 0 {
        return Err(VshotError::InvalidGeometry(
            "width and height must be greater than zero".into(),
        ));
    }
    let rect = Rect::new(x, y, width, height);
    rect.right()?;
    rect.bottom()?;
    Ok(rect)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_negative_origin_and_formats() {
        let rect: Rect = "-1920,10 800x600".parse().unwrap();
        assert_eq!(rect, Rect::new(-1920, 10, 800, 600));
        assert_eq!(rect.to_string(), "-1920,10 800x600");
    }

    #[test]
    fn rejects_malformed_or_zero_geometry() {
        for value in ["1,2", "1 2x3", "1,2 0x3", "1,2 3x0", "a,2 3x4"] {
            assert!(parse_geometry(value).is_err(), "{value}");
        }
    }

    #[test]
    fn computes_intersection() {
        let a = Rect::new(-10, -10, 20, 20);
        let b = Rect::new(0, 0, 20, 20);
        assert_eq!(a.intersection(b), Some(Rect::new(0, 0, 10, 10)));
    }
}

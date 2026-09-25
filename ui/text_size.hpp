// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// The text size the panel edits, and the unit it is stored in.
//
// The panel edits a **pixel size**: what the user types is what the label is,
// so 14 stays 14.  Nothing here converts between units for the user.
//
// The one thing that does need converting is the *legacy* `scale` the JSON
// protocol still carries.  `scale` is a multiple of the glyph cell the Rust
// fallback font draws with -- a 7-pixel cell, 5 columns wide, so scale 2 is
// 14 px tall -- and the fallback can only draw whole multiples of it.  The
// conversion therefore happens in exactly one place, on the way out to the
// protocol, and only there: the panel, the preview and the exported bitmap all
// work in the pixel size directly, so a label drawn at 15 px is 15 px on
// screen and in the PNG.
//
// Built only with `-DVSHOT_BUILD_CHECKS=ON` this header is also the subject of
// `ui/text_size_check.cpp`, which pins the range and the round trip to the
// legacy scale.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vshot {

/// The Rust fallback font's glyph cell: 7 rows tall, 5 columns wide plus one
/// column of spacing (`src/model/frame.rs`).  One legacy `scale` unit is this
/// many pixels of font height.
constexpr int kGlyphCellHeight = 7;

/// The pixel size range the panel offers.  The lower bound keeps a label
/// legible; the upper bound matches the legacy scale range the protocol
/// accepts, so nothing the panel produces is clamped in transit.
constexpr int kMinTextPixels = kGlyphCellHeight;      //  7 px
constexpr int kMaxTextPixels = kGlyphCellHeight * 64; // 448 px, legacy scale 64

/// The legacy glyph multiples the protocol and the Rust fallback understand.
constexpr int kMinTextScale = 1;
constexpr int kMaxTextScale = 64;

/// The pixel size a new label starts at.  This is the old default `scale = 2`
/// spelled in pixels, so existing documents and habits both keep their size.
constexpr int kDefaultTextPixels = kGlyphCellHeight * 2; // 14 px

inline int clampTextPixels(int pixels)
{
    return std::clamp(pixels, kMinTextPixels, kMaxTextPixels);
}

/// The pixel height a legacy `scale` means.
///
/// The inverse of [`textPixelsToScale`] at the whole multiples, used only to
/// migrate a config file written before the size was a pixel height: such a
/// file stores the glyph multiple (1-64), and reinterpreting a stored `2` as
/// two pixels would shrink that label from the 14 px it meant.  The two ranges
/// line up exactly -- 1-64 multiples are 7-448 pixels -- so nothing is lost.
inline int scaleToPixels(std::uint32_t scale)
{
    return clampTextPixels(static_cast<int>(scale) * kGlyphCellHeight);
}

/// The legacy `scale` a pixel size means, for the JSON protocol only.
///
/// Rounded to the *nearest* multiple and floored at 1: the Rust fallback needs
/// a whole multiple of its cell, and truncating a 15 px label to scale 2 (14
/// px) rather than rounding it up to 3 (21 px) would shrink it.  Only the
/// fallback ever sees this -- a helper that ships a bitmap renders the exact
/// pixel size instead.
inline std::uint32_t textPixelsToScale(int pixels)
{
    const int cell = std::max(kGlyphCellHeight, 1);
    const double multiple = static_cast<double>(clampTextPixels(pixels)) / static_cast<double>(cell);
    const double rounded = std::round(multiple);
    return static_cast<std::uint32_t>(
        std::clamp(rounded, static_cast<double>(kMinTextScale), static_cast<double>(kMaxTextScale)));
}

} // namespace vshot

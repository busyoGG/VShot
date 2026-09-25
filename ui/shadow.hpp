// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#pragma once

#include <QImage>
#include <QPoint>
#include <QSize>

#include <cstdlib>

namespace vshot {

/// The soft shadow behind a surface that has to draw its own decoration.
///
/// Both a pinned image and a file dialog are layer surfaces, so no compositor
/// draws them a shadow and this is the only one they get.  The values come from
/// the config file and mean the same thing for both, so the two look alike when
/// the user tunes them.
struct ShadowStyle {
    /// Master switch.  Off paints nothing, whatever the numbers below say --
    /// which is what makes it possible to keep a size around while turning the
    /// shadow off for a moment.
    bool enabled = true;
    /// How far the blur reaches past the shape, in logical pixels.
    int size = 14;
    /// How far the shape is dropped below its own rect, in logical pixels:
    /// light comes from above, so a shadow hangs below the thing casting it.
    /// Zero centres it, i.e. a halo; negative puts it above the shape instead.
    int offset = 3;
    /// Alpha of the silhouette before it is softened, 0-255.  This is what the
    /// shadow's darkness comes from; the blur only spreads it.
    int opacity = 120;

    /// How far past its own rect this shadow reaches on every side, in logical
    /// pixels.  Zero when it is off.
    ///
    /// The reach is the blur's own, plus the offset -- which moves the whole
    /// softened shape, and so carries the far side of it just as far as the
    /// near side is pulled back.  One number for all four sides: a repaint
    /// region only has to be grown, and being generous on the side the offset
    /// moved away from costs nothing but a few pixels of overpaint.
    int band() const { return enabled ? size + std::abs(offset) + 1 : 0; }
};

/// One rendered shadow: the image, and where it goes.
struct ShadowBitmap {
    /// The shadow, at the caller's device resolution.  Null when there is
    /// nothing to draw -- off, empty, or an alpha of zero.
    QImage image;
    /// Where the image's top-left lands relative to the shape's own top-left,
    /// in logical pixels.  Negative on both axes: a shadow is drawn around the
    /// shape, not inside it.
    QPoint origin;
};

/// Renders the shadow of a shape.
///
/// `shape` is the shape's size and `radius` its corner radius, both in logical
/// pixels; `ratio` is the output's device pixel ratio, which is folded in here
/// so a shadow is equally soft on a 1x and a 2x screen rather than growing with
/// the ratio.
///
/// The falloff is built at a fraction of the size rather than at full size: a
/// soft shadow has no detail to lose, and a 4K shape's shadow would otherwise
/// cost more per frame than drawing the shape did.  It is worth caching the
/// result -- a repaint that reuses the shape's own size can reuse the image.
ShadowBitmap renderShadow(const QSize &shape, qreal radius, const ShadowStyle &style, qreal ratio);

} // namespace vshot

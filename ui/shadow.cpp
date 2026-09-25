// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#include "shadow.hpp"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace vshot {
namespace {

// The falloff is built by blurring a copy at a third of the size, so a blur of
// three small pixels is nine logical ones once it is scaled back up.
constexpr int kDownscale = 3;
// Two passes: one box blur is a hard-edged average, and the second pass is what
// turns its straight ramp into a falloff the eye reads as a shadow.
constexpr int kBlurPasses = 2;

// One separable box-blur pass, of an arbitrary radius, over premultiplied data.
//
// The radius is what the shadow's own size is translated into: a wider reach is
// a softer shadow, and a blur whose radius never changed would only move the
// shadow's edges outward without softening them, which looks like nothing at
// all.  The window is clamped at the borders -- the last pixel counts for every
// sample beyond it -- so a shadow never darkens or lightens along the picture's
// own edge.
QImage boxBlurImage(const QImage &source, int radius)
{
    if (source.isNull() || radius < 1) {
        return source;
    }
    const QImage input = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int width = input.width();
    const int height = input.height();
    const int span = 2 * radius + 1;
    const auto at = [](int index, int limit) { return std::clamp(index, 0, limit - 1); };

    QImage horizontal(width, height, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < height; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(input.constScanLine(y));
        QRgb *out = reinterpret_cast<QRgb *>(horizontal.scanLine(y));
        long a = 0;
        long r = 0;
        long g = 0;
        long b = 0;
        for (int x = -radius; x <= radius; ++x) {
            const QRgb pixel = row[at(x, width)];
            a += qAlpha(pixel);
            r += qRed(pixel);
            g += qGreen(pixel);
            b += qBlue(pixel);
        }
        for (int x = 0; x < width; ++x) {
            out[x] = qRgba(static_cast<int>(r / span), static_cast<int>(g / span),
                           static_cast<int>(b / span), static_cast<int>(a / span));
            const QRgb leaving = row[at(x - radius, width)];
            const QRgb entering = row[at(x + radius + 1, width)];
            a += qAlpha(entering) - qAlpha(leaving);
            r += qRed(entering) - qRed(leaving);
            g += qGreen(entering) - qGreen(leaving);
            b += qBlue(entering) - qBlue(leaving);
        }
    }

    QImage result(width, height, QImage::Format_ARGB32_Premultiplied);
    for (int x = 0; x < width; ++x) {
        long a = 0;
        long r = 0;
        long g = 0;
        long b = 0;
        for (int y = -radius; y <= radius; ++y) {
            const QRgb pixel = reinterpret_cast<const QRgb *>(
                horizontal.constScanLine(at(y, height)))[x];
            a += qAlpha(pixel);
            r += qRed(pixel);
            g += qGreen(pixel);
            b += qBlue(pixel);
        }
        for (int y = 0; y < height; ++y) {
            reinterpret_cast<QRgb *>(result.scanLine(y))[x] =
                qRgba(static_cast<int>(r / span), static_cast<int>(g / span),
                      static_cast<int>(b / span), static_cast<int>(a / span));
            const QRgb leaving = reinterpret_cast<const QRgb *>(
                horizontal.constScanLine(at(y - radius, height)))[x];
            const QRgb entering = reinterpret_cast<const QRgb *>(
                horizontal.constScanLine(at(y + radius + 1, height)))[x];
            a += qAlpha(entering) - qAlpha(leaving);
            r += qRed(entering) - qRed(leaving);
            g += qGreen(entering) - qGreen(leaving);
            b += qBlue(entering) - qBlue(leaving);
        }
    }
    return result;
}

} // namespace

ShadowBitmap renderShadow(const QSize &shape, qreal radius, const ShadowStyle &style, qreal ratio)
{
    ShadowBitmap result;
    if (!style.enabled || shape.isEmpty() || style.size <= 0 || style.opacity <= 0) {
        return result;
    }
    const int spread = style.size;
    // The blur radius that reach means, in the downscaled copy the mask is built
    // in.  A soft shadow's visible spread is about two standard deviations either
    // way, and a box blur's is sqrt((r^2+r)/3) per pass, so the radius that puts
    // a `size`-wide falloff on the shape is about size/4 logical pixels -- which
    // is what the downscale divides down.  The floor of one small pixel is where
    // the default size of 14 lands, and one small pixel blurred twice is exactly
    // the pair of 3x3 passes this was before the size was configurable: the
    // default shadow is the one that was always there, and the size moves it
    // from there rather than replacing it.
    const int radiusSmall =
        std::max(1, qRound(static_cast<double>(spread) / (4.0 * kDownscale)));
    // The blurred box: the shape's rect grown by the blur's reach on every
    // side.  The blur is what needs that room -- its edges reach outside the
    // shape it was given, and a box cut to the shape exactly would clip them
    // off square.
    const QSize box(std::max(1, qRound((shape.width() + 2 * spread) * ratio)),
                    std::max(1, qRound((shape.height() + 2 * spread) * ratio)));
    // The shape inside that box: centred horizontally, dropped by the offset.
    const QRectF silhouette(qRound(spread * ratio), qRound((spread + style.offset) * ratio),
                            qRound(shape.width() * ratio), qRound(shape.height() * ratio));

    if (box.width() < kDownscale || box.height() < kDownscale) {
        return result;
    }
    const QSize small(std::max(1, box.width() / kDownscale),
                      std::max(1, box.height() / kDownscale));
    QImage mask(small, QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, std::clamp(style.opacity, 0, 255)));
        // Everything is divided down with the mask: the shape's own rect inside
        // the box, and the radius it was rounded by.
        const QRectF scaled(silhouette.left() / kDownscale, silhouette.top() / kDownscale,
                            silhouette.width() / kDownscale, silhouette.height() / kDownscale);
        const qreal scaledRadius =
            std::clamp(radius / kDownscale, 0.0, std::min(scaled.width(), scaled.height()) / 2.0);
        if (scaledRadius > 0.0) {
            painter.drawRoundedRect(scaled, scaledRadius, scaledRadius);
        } else {
            painter.drawRect(scaled);
        }
    }
    for (int pass = 0; pass < kBlurPasses; ++pass) {
        mask = boxBlurImage(mask, radiusSmall);
    }
    // Back up to the size it will be drawn at.  The blur ran small because a
    // soft shadow has no detail to lose and a 4K shape's would otherwise cost
    // more per frame than the shape did; the upscale is what puts the softness
    // back at the right radius.
    result.image = mask.scaled(box, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    result.origin = QPoint(-spread, -spread);
    return result;
}

} // namespace vshot

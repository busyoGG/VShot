// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QString>
#include <QVector>

namespace vshot {

// One line of the color card: the name of a format and the exact text the card
// prints for it. The same rows are what a right-click on a pinned card offers
// to copy, and both come from `colorCardRows`, so the menu and the card can
// never end up disagreeing about a value.
struct ColorRow {
    QString label;
    QString value;
};

// The rows `renderColorCard` prints, in the order it prints them: HEX, then
// HEX8 and RGBA when the color is translucent, then RGB, HSL, HSV and CMYK,
// and NAME when the color is one CSS has a word for.
QVector<ColorRow> colorCardRows(const QColor &color);

// True when `text` is nothing but one color literal, filling `out` with the
// color it names. Only the spellings a clipboard actually carries are
// accepted -- `#RGB`/`#RGBA`/`#RRGGBB`/`#RRGGBBAA` and the CSS color functions
// (rgb, rgba, hsl, hsla, hsv, hsb, cmyk) -- so a snippet of code, a document or
// a bare word is never mistaken for a color: the whole payload has to be the
// literal, with nothing around it.
bool colorFromLiteral(const QString &text, QColor *out);

// Reads the X11/Qt clipboard convention for a copied color
// (`application/x-color`): four 16-bit big-endian channels, RGBA, padded to 16
// bytes. `false` when the payload is too short to hold them.
bool colorFromX11Payload(const QByteArray &payload, QColor *out);

// Renders a copied color as a pin-ready card: the color itself as a swatch,
// beside the same color written out in every format a clipboard user might
// want again -- hex, RGB(A), HSL, HSV, CMYK, and its CSS name when it has one.
// Translucent colors get the standard checkerboard behind the swatch, plus the
// alpha-carrying hex and rgba rows. `pixelRatio` (device pixels per logical
// pixel, clamped to 1..=4) rasterizes the card at the target output's density,
// exactly like `renderTextCard`; the returned image carries a matching
// devicePixelRatio so the pin shows it at its natural logical size.
QImage renderColorCard(const QColor &color, int pixelRatio);

} // namespace vshot

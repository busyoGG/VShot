#pragma once

#include <QImage>
#include <QString>

class QMimeData;

namespace vshot {

// Renders clipboard text as a pin-ready image card. The richest available
// representation wins: embedded HTML keeps the source's own formatting
// (IDE syntax highlighting, browser selections), then GitHub-flavored
// markdown, then a monospace code card, then plain text. `pixelRatio`
// (device pixels per logical pixel, clamped to 1..=4) rasterizes the card
// at the target output's density; the returned image carries a matching
// devicePixelRatio so the pin surface shows it at its natural logical size.
QImage renderTextCard(const QMimeData *mime, const QString &text, int pixelRatio);

} // namespace vshot

#pragma once

#include <QByteArray>
#include <QString>

class QIODevice;

namespace vshot {

// The density a PNG declares about itself — device pixels per logical pixel of
// the image, 1..4 — or 0 when it declares none.
//
// A PNG states this with a `pHYs` chunk in pixels per metre, and 96 DPI is a
// density like any other: it is what vshot writes for a capture taken on a
// scale-1 output, and the pin daemon has to read it back as 1x. Otherwise that
// capture gets sized from the output it lands on instead, and a 1080p crop
// pinned on a 4K screen comes out half the size it had on screen.
//
// The chunk has to be there for the declaration to count. A decoded PNG cannot
// show the difference: Qt reports 96 DPI for an image that declares nothing at
// all, and what exactly it reports there depends on the context (3780 dots per
// metre with a QGuiApplication around, 3937 without one), while an image that
// declares nothing has to keep falling through to the rules in the daemon,
// which size it from the output it lands on. Only the bytes say which is which.
//
// Anything that is not a near-exact multiple of 96 DPI within 1..4 is not a
// device density: a print resolution (300 DPI), an aspect-ratio-only `pHYs`
// (unit 0) and a non-square one all read as 0.
int densityFromPixelDensity(int pixelsPerMeterX, int pixelsPerMeterY, int unit);

// Reads that declaration out of a PNG's chunks, starting at the device's
// current position. Only the header chunks are read: the walk stops at `IDAT`,
// where the pixels begin, so it stays cheap on a large file. 0 when the bytes
// are not a PNG, the header is malformed, or nothing is declared.
int pngDeclaredDensity(QIODevice &device);

// The same for PNG bytes already in memory (a clipboard payload) and for a PNG
// on disk (a pinned file).
int pngDeclaredDensity(const QByteArray &png);
int pngDeclaredDensityOfFile(const QString &path);

} // namespace vshot

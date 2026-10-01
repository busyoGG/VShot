// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// Offline check for screenshot translation: the placed lines a translated
// envelope becomes, the background sampler, the size read off the frame, the
// font fitter, and the session keys the standalone overlay reads.
//
// The translated envelope is the recognition envelope with `lines[].text`
// replaced and an `error` added to a line that failed, so it parses through
// `TextLayer::fromJson` unchanged and the translated unit lands at the original
// rectangle.  What is checked here is that promise, that a failed line keeps
// its source text rather than vanishing, that the fill colour read from a frame
// is the frame rather than its glyphs, that the glyphs' own height -- not the
// engine's box around them -- is the size the translation is drawn at, and that
// a long translation grows its fill instead of being clipped.
//
// Needs Qt Gui and the offscreen platform plugin (for the font database) and
// nothing else: no compositor, no recognition run and no translation run.
//
// Built only with `-DVSHOT_BUILD_CHECKS=ON`; see the README's verification
// section.

#include "capture_overlay.hpp"
#include "i18n.hpp"
#include "session_protocol.hpp"
#include "text_layer.hpp"
#include "translation_layer.hpp"

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char *what, const QString &detail = QString())
{
    if (condition) {
        std::printf("ok    %s\n", what);
        return;
    }
    ++failures;
    if (detail.isEmpty()) {
        std::printf("FAIL  %s\n", what);
    } else {
        std::printf("FAIL  %s -- %s\n", what, qPrintable(detail));
    }
}

bool sameRect(const vshot::LogicalRect &rect, std::int32_t x, std::int32_t y,
              std::uint32_t width, std::uint32_t height)
{
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}

QString rectText(const vshot::LogicalRect &rect)
{
    return QStringLiteral("(%1,%2 %3x%4)")
        .arg(rect.x)
        .arg(rect.y)
        .arg(rect.width)
        .arg(rect.height);
}

// The same font the module builds, so a width measured here is the width it
// measured.  `translatedFamily` returns a non-empty family, so the two agree.
QFont metricsFont(const QString &family, int pixels)
{
    QFont font;
    if (family.isEmpty()) {
        font = QGuiApplication::font();
    } else {
        font.setFamily(family);
    }
    font.setPixelSize(std::max(1, pixels));
    return font;
}

int advance(const QString &family, int pixels, const QString &text)
{
    return QFontMetrics(metricsFont(family, pixels)).horizontalAdvance(text);
}

// The glyphs' real ink height -- what the fill has to hold if the clip is not
// to cut it.  It is measured from the ink box, not the font's nominal metrics,
// because the two disagree for a pixel-size font.
int inkHeight(const QString &family, int pixels, const QString &text)
{
    const QFontMetricsF metrics(metricsFont(family, pixels));
    return static_cast<int>(std::ceil(metrics.boundingRect(text).height()));
}

bool near(const QColor &actual, const QColor &expected, int tolerance = 3)
{
    return std::abs(actual.red() - expected.red()) <= tolerance &&
        std::abs(actual.green() - expected.green()) <= tolerance &&
        std::abs(actual.blue() - expected.blue()) <= tolerance;
}

bool withinRange(const QColor &actual, const QColor &low, const QColor &high, int tolerance = 3)
{
    return actual.red() >= low.red() - tolerance && actual.red() <= high.red() + tolerance &&
        actual.green() >= low.green() - tolerance && actual.green() <= high.green() + tolerance &&
        actual.blue() >= low.blue() - tolerance && actual.blue() <= high.blue() + tolerance;
}

// A translated envelope: `lines[].text` is the translation, `source` keeps what
// the engine read, an `error` marks the line that failed, and the per-character
// boxes are gone -- that is the shape `vshot translate --stdin-ocr --json`
// actually prints, and the parser has to place it all the same.
const char *const kTranslatedEnvelope = R"json(
{"from":"auto","geometry":true,"provider":"external","to":"zh-Hans","version":1,"lines":[
 {"rect":{"x":10,"y":20,"width":90,"height":30},"source":"HELLO","text":"你好"},
 {"rect":{"x":10,"y":60,"width":60,"height":30},"source":"WORLD","text":"WORLD",
  "error":"the provider timed out"}]}
)json";

// The exact bytes the CLI prints for the `external` provider set to `cat` (an
// identity translation): pinned verbatim so a rename on either side of the wire
// fails here rather than quietly placing every line at the origin.
const char *const kRealBinaryEnvelope =
    R"json({"from":"auto","geometry":true,"lines":[{"rect":{"height":12,"width":30,"x":1,"y":2},"source":"hello","text":"hello"}],"provider":"external","to":"zh-Hans","version":1})json";

// A translated envelope that still carries the engine's per-character boxes:
// the other shape a provider may hand back.  The units keep their own rects and
// the union of a line is what the translation replaces.
const char *const kTranslatedWithChars = R"json(
{"version":1,"geometry":true,"provider":"stub","from":"en","to":"zh","lines":[
 {"source":"HELLO","text":"你好","rect":{"x":10,"y":20,"width":90,"height":30},
  "chars":[{"ch":"H","rect":{"x":10,"y":20,"width":30,"height":30}},
           {"ch":"E","rect":{"x":40,"y":20,"width":30,"height":30}},
           {"ch":"L","rect":{"x":70,"y":20,"width":30,"height":30}}]},
 {"source":"WORLD","text":"WORLD","error":"provider timed out",
  "rect":{"x":10,"y":60,"width":60,"height":30},
  "chars":[]}]}
)json";

const vshot::TextLayerPlacement kDoubled{100.0, 200.0, 2.0};

std::optional<vshot::TextLayer> parse(const char *document, const vshot::TextLayerPlacement &place,
                                      const char *what)
{
    QString error;
    std::optional<vshot::TextLayer> layer =
        vshot::TextLayer::fromJson(QByteArray(document), place, &error);
    if (!layer.has_value()) {
        expect(false, what, error);
    }
    return layer;
}

// The translated units sit at the original rectangles, and each line's own text
// is what the paint reads.
void checkPlacement()
{
    std::printf("--- the translation lands where the text was ----------------------\n");
    // The shape the pipeline really produces: no per-character boxes, so each
    // line is one unit over its own rect and the union is that same rect.
    std::optional<vshot::TextLayer> layer =
        parse(kTranslatedEnvelope, kDoubled, "the translated envelope parses");
    if (!layer.has_value()) {
        return;
    }
    expect(layer->hasGeometry(), "it has geometry");
    expect(layer->count() == 2, "one unit per line when the boxes are gone",
           QString::number(layer->count()));
    expect(layer->lineCount() == 2, "two lines", QString::number(layer->lineCount()));
    expect(layer->lineText(0) == QStringLiteral("你好"), "the first line holds the translation",
           layer->lineText(0));
    expect(layer->lineText(1) == QStringLiteral("WORLD"),
           "a failed line keeps the text the engine left in place", layer->lineText(1));
    expect(layer->lineText(2).isEmpty(), "a line out of range has no text");

    // Engine (10,20,90,30) at origin (100,200) and scale 2 is logical
    // (105,210,45,15).
    expect(sameRect(layer->unit(0).rect, 105, 210, 45, 15),
           "a line with no boxes is placed over its own rect", rectText(layer->unit(0).rect));
    expect(sameRect(layer->unit(1).rect, 105, 230, 30, 15),
           "and so is the line that failed", rectText(layer->unit(1).rect));

    const QVector<vshot::TranslatedLine> lines = vshot::translatedLines(*layer);
    expect(lines.size() == 2, "one placed line per recognized line",
           QString::number(lines.size()));
    if (lines.size() == 2) {
        expect(lines.at(0).text == QStringLiteral("你好"),
               "the placed line carries the translation", lines.at(0).text);
        expect(sameRect(lines.at(0).source, 105, 210, 45, 15),
               "and covers the original line's box", rectText(lines.at(0).source));
        expect(lines.at(1).text == QStringLiteral("WORLD"),
               "a failed line is placed with the source text rather than vanishing",
               lines.at(1).text);
        expect(sameRect(lines.at(1).source, 105, 230, 30, 15),
               "over its own box", rectText(lines.at(1).source));
    }

    // The exact bytes the real binary prints, pinned verbatim with no placement
    // to speak of: an identity translation of one line at (1,2,30,12).
    std::optional<vshot::TextLayer> real =
        parse(kRealBinaryEnvelope, vshot::TextLayerPlacement{}, "the CLI's own envelope parses");
    if (real.has_value()) {
        expect(real->hasGeometry(), "a real translated envelope has geometry");
        expect(real->count() == 1, "with one unit", QString::number(real->count()));
        expect(real->plainText() == QStringLiteral("hello"), "holding the translated text",
               real->plainText());
        expect(sameRect(real->unit(0).rect, 1, 2, 30, 12), "at the original line's rect",
               rectText(real->unit(0).rect));
    }

    // The other shape: a provider that keeps the per-character boxes.  The
    // units still land on their own rects and the union is the line.
    std::optional<vshot::TextLayer> withChars =
        parse(kTranslatedWithChars, kDoubled, "a translated envelope with boxes parses");
    if (withChars.has_value()) {
        expect(withChars->count() == 4, "the units still hold the engine's characters",
               QString::number(withChars->count()));
        expect(sameRect(withChars->unit(0).rect, 105, 210, 15, 15),
               "the first unit is at the original box", rectText(withChars->unit(0).rect));
        const QVector<vshot::TranslatedLine> unioned = vshot::translatedLines(*withChars);
        expect(unioned.size() == 2, "one placed line per line", QString::number(unioned.size()));
        if (unioned.size() == 2) {
            expect(sameRect(unioned.at(0).source, 105, 210, 45, 15),
                   "the placed line is the union of its unit boxes",
                   rectText(unioned.at(0).source));
            expect(unioned.at(0).text == QStringLiteral("你好"),
                   "carrying the line's translation", unioned.at(0).text);
        }
    }
}

// A frame with a flat background and a glyph box inside `line` leaves the band
// outside it pure background.
QImage flatFrame(const QColor &background, const QColor &glyphs, const QRect &line)
{
    QImage image(200, 120, QImage::Format_ARGB32);
    image.fill(background);
    if (line.isValid()) {
        QPainter painter(&image);
        painter.fillRect(line, glyphs);
    }
    return image;
}

void checkBackgroundSampler()
{
    std::printf("--- the fill reads the frame, not the glyphs ---------------------\n");
    const QRect line(40, 60, 120, 20);

    const QColor light(245, 245, 245);
    const QColor dark(20, 20, 20);
    const QColor sampledLight =
        vshot::sampleLineBackground(flatFrame(light, dark, line), line);
    expect(near(sampledLight, light), "a light background comes back light",
           QStringLiteral("got %1").arg(sampledLight.name()));
    const QColor sampledDark = vshot::sampleLineBackground(flatFrame(dark, light, line), line);
    expect(near(sampledDark, dark), "a dark background comes back dark",
           QStringLiteral("got %1").arg(sampledDark.name()));

    // A horizontal gradient: the band above and below the line samples the same
    // columns, so the fill is a colour the background actually has there.
    QImage gradient(200, 120, QImage::Format_ARGB32);
    const QColor low(10, 20, 30);
    const QColor high(200, 210, 220);
    for (int x = 0; x < gradient.width(); ++x) {
        const double t = static_cast<double>(x) / static_cast<double>(gradient.width() - 1);
        const QColor colour(static_cast<int>(std::lround(low.red() + (high.red() - low.red()) * t)),
                            static_cast<int>(std::lround(low.green() + (high.green() - low.green()) * t)),
                            static_cast<int>(std::lround(low.blue() + (high.blue() - low.blue()) * t)));
        for (int y = 0; y < gradient.height(); ++y) {
            gradient.setPixelColor(x, y, colour);
        }
    }
    const QColor sampledGradient = vshot::sampleLineBackground(gradient, line);
    expect(withinRange(sampledGradient, low, high),
           "a gradient comes back as one of its own colours",
           QStringLiteral("got %1").arg(sampledGradient.name()));

    // A null frame must not crash and must still answer something usable.
    expect(vshot::sampleLineBackground(QImage(), line).isValid(),
           "a frame that is not there still answers a colour");
}

// The size the glyphs are drawn at, the floor under it, and the fill that grows
// so the text is never clipped.
void checkFitter()
{
    std::printf("--- the text is sized to the line, then the line to the text -----\n");
    const QColor background(245, 245, 245);
    const QString text = QStringLiteral("I");
    const QString family = vshot::translatedFamily(QString(), text);
    const vshot::LogicalRect limit{0, 0, 400, 200};

    // The size comes from the ink the original glyphs covered, not from the
    // engine's box around them: a 22 px line in a 32 px box is drawn at 23 px,
    // which is the ink over the fraction of the font size glyphs cover, and the
    // box's own height is a third more than that.
    {
        const vshot::LogicalRect line{0, 0, 400, 32};
        const vshot::LineFit fit =
            vshot::fitTranslatedLine(text, line, background, family, limit, 22);
        expect(fit.fontPixels == 23, "a 22 px line is drawn at 23 px",
               QString::number(fit.fontPixels));
        expect(fit.fontPixels < static_cast<int>(line.height),
               "which is not the engine's box around it",
               QStringLiteral("drawn at %1 in a %2-tall box")
                   .arg(fit.fontPixels)
                   .arg(line.height));
        expect(fit.fill.x == 0 && fit.fill.y == 0 && fit.fill.width == 400,
               "and keeps the box's own left and width", rectText(fit.fill));
        // A font's metrics box, not just its nominal pixel size, decides the
        // height, so the fill must hold the glyphs' real ink or the clip would
        // cut them.
        expect(static_cast<int>(fit.fill.height) >= inkHeight(family, fit.fontPixels, text),
               "the fill is tall enough for the glyphs' own ink",
               QStringLiteral("fill %1, glyphs %2")
                   .arg(fit.fill.height)
                   .arg(inkHeight(family, fit.fontPixels, text)));
        expect(fit.textColor == QColor(20, 20, 20), "dark ink on a light fill");
    }

    // A line of small glyphs in a roomy box: lowercase-only text, or an engine
    // that pads its boxes generously.  The ink is the reading, and it wins.
    {
        const vshot::LogicalRect line{0, 0, 400, 32};
        const vshot::LineFit fit =
            vshot::fitTranslatedLine(text, line, background, family, limit, 12);
        expect(fit.fontPixels == 13, "a 12 px line is drawn at 13 px",
               QString::number(fit.fontPixels));
    }

    // A reading that fills the box is the one the sampler cannot be trusted
    // with -- a photograph under the text reads as ink from edge to edge -- so
    // the box's own estimate caps it rather than letting it through.  With no
    // reading at all, that estimate is all there is.
    {
        const vshot::LogicalRect line{0, 0, 400, 32};
        const vshot::LineFit capped =
            vshot::fitTranslatedLine(text, line, background, family, limit, 32);
        expect(capped.fontPixels == 23, "a reading as tall as the box is capped by the box",
               QString::number(capped.fontPixels));
        const vshot::LineFit unread =
            vshot::fitTranslatedLine(text, line, background, family, limit);
        expect(unread.fontPixels == 23, "and no reading falls back to the same number",
               QString::number(unread.fontPixels));
    }

    // A medium line: the text fits once the glyphs are shrunk, above the floor.
    {
        const QString medium = QStringLiteral("A translation of some length");
        const QString mediumFamily = vshot::translatedFamily(QString(), medium);
        const std::uint32_t width = static_cast<std::uint32_t>(advance(mediumFamily, 24, medium));
        const vshot::LogicalRect line{0, 0, width, 40};
        const vshot::LogicalRect wide{0, 0, 4000, 200};
        const vshot::LineFit fit =
            vshot::fitTranslatedLine(medium, line, background, mediumFamily, wide, 38);
        expect(fit.fontPixels < 29 && fit.fontPixels >= 14, "a longer line shrinks the glyphs",
               QString::number(fit.fontPixels));
        expect(fit.fill.width == width, "the fill is still the original box once it fits",
               rectText(fit.fill));
        expect(advance(mediumFamily, fit.fontPixels, medium) <= static_cast<int>(fit.fill.width),
               "the drawn text fits inside the fill");
    }

    // A long line and a narrow box: the glyphs stop at half the size the line's
    // own glyphs were and the fill grows to the right to hold them.
    {
        const QString long_ = QStringLiteral("This translation is far too long for the box");
        const QString longFamily = vshot::translatedFamily(QString(), long_);
        const vshot::LogicalRect line{0, 0, 10, 40};
        const vshot::LogicalRect wide{0, 0, 4000, 200};
        const vshot::LineFit fit =
            vshot::fitTranslatedLine(long_, line, background, longFamily, wide, 38);
        expect(fit.fontPixels == 14, "the glyphs stop at half the line's own size",
               QString::number(fit.fontPixels));
        expect(static_cast<int>(fit.fill.width) >= advance(longFamily, fit.fontPixels, long_),
               "the fill grows to cover the text",
               QStringLiteral("fill %1, text %2")
                   .arg(fit.fill.width)
                   .arg(advance(longFamily, fit.fontPixels, long_)));
        expect(fit.fill.width > 10, "the fill is wider than the box it replaces",
               rectText(fit.fill));
        expect(fit.fill.x == 0, "growing to the right keeps the left edge", rectText(fit.fill));
    }

    // Against the right edge the fill is pulled left rather than running off the
    // image, so the text is still whole.
    {
        const QString text_ = QStringLiteral("Some words that need room");
        const QString family_ = vshot::translatedFamily(QString(), text_);
        const vshot::LogicalRect line{250, 20, 40, 24};
        const vshot::LogicalRect limit_{0, 0, 300, 80};
        const vshot::LineFit fit =
            vshot::fitTranslatedLine(text_, line, background, family_, limit_, 22);
        expect(fit.fill.right() <= 300, "the fill stays on the image",
               QStringLiteral("right edge %1").arg(fit.fill.right()));
        expect(fit.fill.x < 250, "it is pulled left of the box it replaces", rectText(fit.fill));
        expect(advance(family_, fit.fontPixels, text_) <= static_cast<int>(fit.fill.width),
               "and the text is still whole", rectText(fit.fill));
    }
}

// The ink reading: the rows of the box the glyphs actually cover, and nothing
// when the box holds nothing but background.
void checkInkSampler()
{
    std::printf("--- the glyphs' own height is read off the frame ------------------\n");
    const QRect line(40, 60, 120, 20);
    const QColor light(245, 245, 245);
    const QColor dark(20, 20, 20);
    const auto band = [&](const QColor &background, const QColor &glyphs, const QRect &box) {
        QImage image(200, 120, QImage::Format_ARGB32);
        image.fill(background);
        QPainter painter(&image);
        painter.fillRect(box, glyphs);
        return image;
    };

    expect(vshot::sampleLineInk(band(light, dark, QRect(40, 64, 120, 12)), line, light) == 12,
           "the rows the glyphs cover are the reading",
           QString::number(vshot::sampleLineInk(band(light, dark, QRect(40, 64, 120, 12)), line,
                                                light)));
    expect(vshot::sampleLineInk(band(dark, light, QRect(40, 62, 120, 15)), line, dark) == 15,
           "light glyphs on a dark background read the same way",
           QString::number(vshot::sampleLineInk(band(dark, light, QRect(40, 62, 120, 15)), line,
                                                dark)));
    // A box over nothing but background: no reading, so the caller falls back to
    // the box's own fraction rather than drawing the text at nothing.
    expect(vshot::sampleLineInk(band(light, dark, QRect()), line, light) == 0,
           "a box over nothing but background reads nothing");
    // One stray pixel per row is what a photograph or a compression artifact
    // looks like, and it is not a line of text.
    {
        QImage noisy = band(light, dark, QRect());
        for (int y = 60; y < 80; ++y) {
            noisy.setPixelColor(90, y, dark);
        }
        expect(vshot::sampleLineInk(noisy, line, light) == 0,
               "a stray pixel in every row is not a line of glyphs",
               QString::number(vshot::sampleLineInk(noisy, line, light)));
    }
    expect(vshot::sampleLineInk(QImage(), line, light) == 0,
           "a frame that is not there reads nothing");
}

// The size a translation is drawn at is read off the frame it replaces: the
// engine's box around a line of text is its detection model's, and drawing the
// translation at the box's height draws it about a third too large.
void checkPlacementReadsTheGlyphSizeFromTheFrame()
{
    std::printf("--- the placed line is sized from the frame it replaces -------------\n");
    const int pixels = 16;
    const QString source = QStringLiteral("Settings and preferences");
    const QString translated = QStringLiteral("\u8bbe\u7f6e\u548c\u504f\u597d");
    const QFontMetricsF metrics(metricsFont(QString(), pixels));
    const QRectF ink = metrics.boundingRect(source);

    // The frame the line was captured from, and the box the detection model
    // would have reported around it: the glyphs with its own padding on every
    // side, which measured five to eight pixels at these sizes.
    QImage frame(600, 120, QImage::Format_ARGB32);
    frame.fill(QColor(250, 250, 250));
    {
        QPainter painter(&frame);
        painter.setFont(metricsFont(QString(), pixels));
        painter.setPen(QColor(20, 20, 20));
        painter.drawText(QPointF(40, 60), source);
    }
    const int inkLeft = 40 + static_cast<int>(std::floor(ink.left()));
    const int inkTop = 60 + static_cast<int>(std::floor(ink.top()));
    const QRect inkBox(inkLeft, inkTop, static_cast<int>(std::ceil(ink.width())),
                       static_cast<int>(std::ceil(ink.height())));
    const QRect box = inkBox.adjusted(-6, -6, 6, 6);

    const QByteArray document =
        QStringLiteral("{\"version\":1,\"geometry\":true,\"lines\":[{\"rect\":"
                       "{\"x\":%1,\"y\":%2,\"width\":%3,\"height\":%4},"
                       "\"source\":\"x\",\"text\":\"%5\"}]}")
            .arg(box.x())
            .arg(box.y())
            .arg(box.width())
            .arg(box.height())
            .arg(translated)
            .toUtf8();
    std::optional<vshot::TextLayer> layer =
        parse(document.constData(), vshot::TextLayerPlacement{}, "the frame's own layer parses");
    if (!layer.has_value()) {
        return;
    }
    const vshot::LogicalRect whole{0, 0, 600, 120};
    const QVector<vshot::TranslatedLine> lines =
        vshot::placedTranslations(*layer, frame, whole, 1.0, whole, QString());
    expect(lines.size() == 1, "one line is placed", QString::number(lines.size()));
    if (lines.size() != 1) {
        return;
    }
    expect(std::abs(lines.at(0).fontPixels - pixels) <= 2,
           "the translation is drawn at the size the frame's glyphs were",
           QStringLiteral("drawn at %1, the glyphs were %2, the box is %3 tall")
               .arg(lines.at(0).fontPixels)
               .arg(pixels)
               .arg(box.height()));
    expect(lines.at(0).fontPixels < box.height(),
           "which is not the engine's box around them",
           QStringLiteral("%1 against a %2-tall box").arg(lines.at(0).fontPixels).arg(box.height()));
    expect(near(lines.at(0).background, QColor(250, 250, 250)),
           "and its fill is the frame's own colour",
           lines.at(0).background.name());
}

// The reference-lines of a rendered line: everything the painter actually
// touched, which is how a smeared or clipped fill would show up.
QRect inkBounds(const QImage &image)
{
    int minX = image.width();
    int minY = image.height();
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) == 0) {
                continue;
            }
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }
    if (maxX < 0) {
        return QRect();
    }
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

void checkPaintingStaysInTheBox(const QString &family, const QString &text,
                                const vshot::LineFit &fit, const char *what)
{
    vshot::TranslatedLine line;
    line.source = fit.fill;
    line.fill = fit.fill;
    line.text = text;
    line.family = family;
    line.fontPixels = fit.fontPixels;
    // A transparent fill leaves only the glyph ink on the canvas, so the ink
    // box measures the text rather than the box around it.
    line.background = QColor(0, 0, 0, 0);
    line.textColor = fit.textColor;

    QImage canvas(4000, 200, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    vshot::paintTranslatedLine(
        painter, QRectF(fit.fill.x, fit.fill.y, fit.fill.width, fit.fill.height), 1.0, line);
    painter.end();
    const QRect painted = inkBounds(canvas);

    // The very same glyphs, drawn with no clip at all.  If the clip took
    // anything off the text, the two ink boxes would differ; equal boxes are
    // the proof that the fill is big enough for what was drawn.
    const QFont font = metricsFont(family, fit.fontPixels);
    const QFontMetricsF metrics(font);
    const QRectF ink = metrics.boundingRect(text);
    const qreal baseline =
        QRectF(fit.fill.x, fit.fill.y, fit.fill.width, fit.fill.height).center().y() -
        (ink.top() + ink.bottom()) / 2.0;
    QImage whole(4000, 200, QImage::Format_ARGB32_Premultiplied);
    whole.fill(Qt::transparent);
    {
        QPainter unclipped(&whole);
        unclipped.setRenderHint(QPainter::Antialiasing, true);
        unclipped.setFont(font);
        unclipped.setPen(fit.textColor);
        unclipped.drawText(QPointF(fit.fill.x, baseline), text);
    }
    const QRect complete = inkBounds(whole);
    expect(painted == complete, "the clip cut nothing off the glyphs",
           QStringLiteral("clipped %1x%2, whole %3x%4")
               .arg(painted.width())
               .arg(painted.height())
               .arg(complete.width())
               .arg(complete.height()));

    const QRect box(fit.fill.x, fit.fill.y, static_cast<int>(fit.fill.width),
                    static_cast<int>(fit.fill.height));
    expect(box.contains(painted), what, QString::number(painted.x()));
}

void checkPainting()
{
    std::printf("--- nothing is drawn outside the fill ----------------------------\n");
    const QColor background(245, 245, 245);
    {
        const QString text = QStringLiteral("This translation is far too long for the box");
        const QString family = vshot::translatedFamily(QString(), text);
        const vshot::LineFit fit = vshot::fitTranslatedLine(
            text, vshot::LogicalRect{0, 0, 10, 40}, background, family,
            vshot::LogicalRect{0, 0, 4000, 200});
        checkPaintingStaysInTheBox(family, text, fit,
                                   "a grown fill still holds every painted pixel");
    }
    {
        const QString text = QStringLiteral("Some words that need room");
        const QString family = vshot::translatedFamily(QString(), text);
        const vshot::LineFit fit = vshot::fitTranslatedLine(
            text, vshot::LogicalRect{250, 20, 40, 24}, background, family,
            vshot::LogicalRect{0, 0, 300, 80});
        checkPaintingStaysInTheBox(family, text, fit,
                                   "a fill pulled back from the edge holds them too");
    }
}

// A translation's fills are its lines' own boxes, grown to hold the glyphs
// drawn in them, and the fills of two closely spaced lines overlap.  Every fill
// therefore goes down before any text does, because painting line by line lets
// the lower fill repaint the upper line's glyphs.
//
// The check renders the same two lines three ways and compares how many of the
// upper line's solid ink pixels survive: alone (nothing to lose), through the
// real painter (nothing may be lost), and line by line (which is what the two
// passes replaced, and which must lose some -- otherwise this check could not
// tell the bug from the fix and would pass on either).
void checkNeighbourFillsDoNotEatText()
{
    std::printf("--- a fill never paints over the line above ---------------------\n");

    const QString upperText = QStringLiteral("更改了配置文件");
    const QString lowerText = QStringLiteral("还支持录音功能");
    const QColor ink(20, 20, 20);

    vshot::TranslatedLine upper;
    upper.text = upperText;
    upper.family = vshot::translatedFamily(QString(), upperText);
    upper.fontPixels = 26;
    upper.background = QColor(255, 255, 255);
    upper.textColor = ink;

    // Where the two boxes go is derived from the ink the font really draws,
    // rather than fixed, because the overlap between them *is* the check and
    // the family in force is the desktop's own.  How much of its box a line of
    // these characters fills, and where under the baseline the ink sits, is the
    // font's business: a rounded CJK face draws them with less ink, and lower
    // down, than the Noto face it may have replaced, and a pair of boxes tuned
    // around one of those lands beside the other one's ink and proves nothing.
    // `drawLineText` centres the font's metrics box in the fill, so that is
    // what is reproduced here: a box's height and the ink's own place inside
    // it, straight off the metrics.
    constexpr int kUpperTop = 12;
    constexpr int kGlyphPixels = 26;
    // The fill the module builds for a line is that line's own box grown to hold
    // its glyphs, and `drawLineText` centres the font's metrics box in it, so a
    // box's height and the ink's own place inside it both follow from the
    // metrics.  The lower line is given a deeper margin: its box has to begin
    // inside the upper line's ink while its glyphs stay clear of it, and how far
    // the ink sits below the box's top edge is what decides whether both can
    // hold at once.
    struct LineBox {
        int height = 0;
        qreal inkTop = 0.0; // both distances below the box's own top edge
        qreal inkBottom = 0.0;
    };
    const auto boxOf = [](const QFont &font, const QString &text, int margin) {
        const QFontMetricsF metrics(font);
        const QRectF metricsBox = metrics.boundingRect(text);
        const QRectF inkBox = metrics.tightBoundingRect(text);
        LineBox box;
        box.height = static_cast<int>(std::ceil(metricsBox.height())) + 2 * margin;
        const qreal shift = box.height / 2.0 - (metricsBox.top() + metricsBox.bottom()) / 2.0;
        box.inkTop = shift + inkBox.top();
        box.inkBottom = shift + inkBox.bottom();
        return box;
    };

    upper.fontPixels = kGlyphPixels;
    const LineBox upperInk = boxOf(metricsFont(upper.family, upper.fontPixels), upperText, 4);
    upper.fill = vshot::LogicalRect{20, kUpperTop, 420,
                                    static_cast<std::uint32_t>(upperInk.height)};

    vshot::TranslatedLine lower = upper;
    lower.text = lowerText;
    lower.family = vshot::translatedFamily(QString(), lowerText);
    const LineBox lowerInk = boxOf(metricsFont(lower.family, lower.fontPixels), lowerText, 12);
    // The lower fill begins well inside the upper line's ink, so it has ink of
    // the upper line's own to paint over, and the lower line's glyphs begin
    // three pixels below that ink -- the antialiased edge of a glyph reaches
    // about a pixel past the tight box the metrics report -- so a pixel of
    // upper ink that goes missing can only have been the lower fill's doing.
    const int lowerTop = static_cast<int>(
        std::lround(static_cast<qreal>(kUpperTop) + upperInk.inkBottom + 3.0 - lowerInk.inkTop));
    lower.fill = vshot::LogicalRect{20, lowerTop, 420,
                                    static_cast<std::uint32_t>(lowerInk.height)};
    // A colour the upper line has none of, so anything red inside it came from
    // the lower fill.
    lower.background = QColor(255, 0, 0);
    // And ink of its own colour, so the two lines' glyphs are never counted as
    // each other's.
    lower.textColor = QColor(0, 0, 90);

    const QRectF upperBox(upper.fill.x, upper.fill.y, upper.fill.width, upper.fill.height);
    const QRectF lowerBox(lower.fill.x, lower.fill.y, lower.fill.width, lower.fill.height);
    expect(upperBox.intersects(lowerBox), "the two fills overlap, or this proves nothing",
           QStringLiteral("upper %1, lower %2")
               .arg(rectText(upper.fill), rectText(lower.fill)));

    const auto target = [](const vshot::TranslatedLine &line) {
        return QRectF(line.fill.x, line.fill.y, line.fill.width, line.fill.height);
    };
    const auto canvas = [] {
        QImage image(600, 120, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        return image;
    };

    QImage alone = canvas();
    {
        QPainter painter(&alone);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        vshot::paintTranslation(painter, {upper}, target, 1.0);
    }
    QImage together = canvas();
    {
        QPainter painter(&together);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        vshot::paintTranslation(painter, {upper, lower}, target, 1.0);
    }
    QImage lineByLine = canvas();
    {
        QPainter painter(&lineByLine);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        for (const vshot::TranslatedLine &line : {upper, lower}) {
            vshot::paintTranslatedLine(painter, target(line), 1.0, line);
        }
    }

    // Only fully solid glyph pixels are compared: an antialiased edge blends
    // with whatever is behind it, and the two renders legitimately differ there
    // once a red fill sits under the upper line's box.  Only the upper line's
    // own box is counted, so the lower line's glyphs can neither pad nor
    // shrink the tally.
    const QRgb solidInk = qRgba(ink.red(), ink.green(), ink.blue(), 255);
    const QRect counted(upper.fill.x, upper.fill.y, static_cast<int>(upper.fill.width),
                        static_cast<int>(upper.fill.height));
    const auto survivingInk = [&](const QImage &image) {
        int count = 0;
        for (int y = counted.top(); y < counted.bottom() && y < image.height(); ++y) {
            for (int x = counted.left(); x < counted.right() && x < image.width(); ++x) {
                if (image.pixel(x, y) == solidInk) {
                    ++count;
                }
            }
        }
        return count;
    };
    const int intact = survivingInk(alone);
    const int throughPainter = survivingInk(together);
    const int eatenByLineByLine = survivingInk(lineByLine);

    expect(intact > 0, "the upper line drew solid ink to lose",
           QStringLiteral("%1 pixels").arg(intact));
    expect(throughPainter == intact, "no ink of the upper line is painted over",
           QStringLiteral("%1 of %2 survived").arg(throughPainter).arg(intact));
    expect(eatenByLineByLine < intact,
           "painting line by line would have lost some, so the check can tell",
           QStringLiteral("%1 of %2 survived line by line")
               .arg(eatenByLineByLine)
               .arg(intact));
}

// The `translate` session: the options and the result path, present and absent.
vshot::Session loadOneSession(const QJsonObject &root, const QString &rawPath, QString *error)
{
    vshot::Session session;
    QJsonObject object = root;
    object.insert(QStringLiteral("version"), 1);
    QJsonObject bounds;
    bounds.insert(QStringLiteral("x"), 0);
    bounds.insert(QStringLiteral("y"), 0);
    bounds.insert(QStringLiteral("width"), 4);
    bounds.insert(QStringLiteral("height"), 4);
    object.insert(QStringLiteral("bounds"), bounds);
    QJsonObject output;
    output.insert(QStringLiteral("id"), 1);
    output.insert(QStringLiteral("name"), QStringLiteral("CHECK-1"));
    // An output's geometry is flattened into the object, not nested: the
    // session reader reads x/y/width/height straight off it.
    output.insert(QStringLiteral("x"), 0);
    output.insert(QStringLiteral("y"), 0);
    output.insert(QStringLiteral("width"), 4);
    output.insert(QStringLiteral("height"), 4);
    output.insert(QStringLiteral("scale"), 1);
    output.insert(QStringLiteral("pixel_width"), 4);
    output.insert(QStringLiteral("pixel_height"), 4);
    output.insert(QStringLiteral("path"), rawPath);
    object.insert(QStringLiteral("outputs"), QJsonArray{output});
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const QString sessionPath = rawPath + QStringLiteral(".json");
    QFile file(sessionPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return session;
    }
    file.write(bytes);
    file.close();
    if (!vshot::loadSession(sessionPath, &session, error)) {
        return vshot::Session();
    }
    return session;
}

void checkSessionParsing()
{
    std::printf("--- the session keys the overlay reads ---------------------------\n");
    QTemporaryDir directory;
    if (!directory.isValid()) {
        expect(false, "a temporary directory for the session");
        return;
    }
    const QString rawPath = directory.filePath(QStringLiteral("frame.raw"));
    {
        QImage frame(4, 4, QImage::Format_RGBA8888);
        frame.fill(QColor(9, 9, 9, 255));
        QFile file(rawPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            expect(false, "a raw frame to load", file.errorString());
            return;
        }
        file.write(reinterpret_cast<const char *>(frame.constBits()), frame.sizeInBytes());
    }

    {
        QJsonObject translate;
        translate.insert(QStringLiteral("from"), QStringLiteral("en"));
        translate.insert(QStringLiteral("to"), QStringLiteral("zh"));
        translate.insert(QStringLiteral("provider"), QStringLiteral("stub"));
        QJsonObject root;
        root.insert(QStringLiteral("mode"), QStringLiteral("translate"));
        root.insert(QStringLiteral("translate"), translate);
        root.insert(QStringLiteral("result_path"), QStringLiteral("/tmp/vshot-translate-check.png"));
        QString error;
        const vshot::Session session = loadOneSession(root, rawPath, &error);
        expect(error.isEmpty(), "a translate session parses", error);
        expect(session.mode == QStringLiteral("translate"), "its mode comes through");
        expect(session.translate.has_value(), "the translate options are there");
        if (session.translate.has_value()) {
            expect(session.translate->from == QStringLiteral("en") &&
                       session.translate->to == QStringLiteral("zh") &&
                       session.translate->provider == QStringLiteral("stub"),
                   "with their values");
        }
        expect(session.resultPath == QStringLiteral("/tmp/vshot-translate-check.png"),
               "and the result path is read", session.resultPath);
    }

    {
        QJsonObject root;
        root.insert(QStringLiteral("mode"), QStringLiteral("translate"));
        QString error;
        const vshot::Session session = loadOneSession(root, rawPath, &error);
        expect(error.isEmpty(), "a translate session without the two keys parses", error);
        expect(!session.translate.has_value(), "an absent `translate` is left to the CLI's config");
        expect(session.resultPath.isEmpty(), "and an absent `result_path` was never named");
    }

    {
        QJsonObject root;
        root.insert(QStringLiteral("mode"), QStringLiteral("translate"));
        root.insert(QStringLiteral("translate"), QStringLiteral("not an object"));
        QString error;
        loadOneSession(root, rawPath, &error);
        expect(!error.isEmpty(), "a `translate` that is not an object is refused", error);
    }
    {
        QJsonObject root;
        root.insert(QStringLiteral("mode"), QStringLiteral("translate"));
        root.insert(QStringLiteral("translate"),
                    QJsonObject{{QStringLiteral("to"), 3}});
        QString error;
        loadOneSession(root, rawPath, &error);
        expect(!error.isEmpty(), "a field that is not a string is refused", error);
    }
    {
        QJsonObject root;
        root.insert(QStringLiteral("mode"), QStringLiteral("translate"));
        root.insert(QStringLiteral("result_path"), QStringLiteral("relative.png"));
        QString error;
        loadOneSession(root, rawPath, &error);
        expect(!error.isEmpty(), "a relative result path is refused", error);
    }
}

// A one-output region session whose whole frame is the selection, so the
// editor's Translate button opens with the pixels it will read under it.
vshot::Session controllerSession(const QImage &frame, const QString &mode)
{
    vshot::Session session;
    session.mode = mode;
    session.bounds = vshot::LogicalRect{0, 0, 400, 400};
    vshot::OutputSession output;
    output.id = 1;
    output.name = QStringLiteral("CHECK-1");
    output.geometry = vshot::LogicalRect{0, 0, 400, 400};
    output.surface = output.geometry;
    output.scale = 1;
    output.pixelWidth = 400;
    output.pixelHeight = 400;
    output.image = frame;
    session.outputs.push_back(output);
    session.selection = output.geometry;
    return session;
}

// The helper's own render of the session, with the selection's own top-left as
// the origin -- the same crop the CLI hands to the daemon.  An empty image
// means the render refused, which the caller reports.
QImage renderSelection(const vshot::OverlayController &controller)
{
    QImage composite;
    QImage marks;
    QString error;
    if (!controller.produceComposite(&composite, &marks, &error)) {
        return QImage();
    }
    return composite;
}

QImage lightFrame()
{
    QImage frame(400, 400, QImage::Format_ARGB32);
    frame.fill(QColor(245, 245, 245));
    // A dark band well away from the line, so a fill that read the whole frame
    // rather than the band under the line would come out wrong.
    QPainter painter(&frame);
    painter.fillRect(0, 320, 400, 80, QColor(10, 10, 10));
    return frame;
}

bool writeExecutable(const QString &path, const QByteArray &body)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(body);
    file.close();
    return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                               QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                               QFileDevice::ExeGroup | QFileDevice::ReadOther |
                               QFileDevice::ExeOther);
}

// A stand-in for `vshot`: `ocr` answers with a fixed envelope, and `translate`
// reads that envelope off its own standard input, checks it arrived, and answers
// in the shape the real step prints -- the per-character boxes dropped and only
// the line rect, its translation and the source left.  An empty or wrong pipe
// therefore comes back as a failure rather than a plausible answer, and the
// editor is driven through the same wire shape the CLI really uses.
const char *const kStubScript = R"sh(#!/bin/sh
case "$1" in
  ocr)
    printf '%s\n' '{"version":1,"geometry":true,"lines":[{"text":"HELLO","rect":{"x":10,"y":10,"width":90,"height":30},"chars":[{"ch":"H","rect":{"x":10,"y":10,"width":30,"height":30}},{"ch":"E","rect":{"x":40,"y":10,"width":30,"height":30}},{"ch":"L","rect":{"x":70,"y":10,"width":30,"height":30}}]}]}'
    ;;
  translate)
    envelope=$(cat)
    case "$envelope" in
      *'"text":"HELLO"'*) ;;
      *) exit 4 ;;
    esac
    printf '%s\n' '{"from":"en","geometry":true,"lines":[{"rect":{"height":30,"width":90,"x":10,"y":10},"source":"HELLO","text":"你好"}],"provider":"stub","to":"zh","version":1}'
    ;;
  *)
    exit 2
    ;;
esac
)sh";

void pressAt(QWidget *overlay, const QPoint &local)
{
    QMouseEvent event(QEvent::MouseButtonPress, QPointF(local),
                      QPointF(overlay->mapToGlobal(local)), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(overlay, &event);
}

void dragTo(QWidget *overlay, const QPoint &local)
{
    QMouseEvent event(QEvent::MouseMove, QPointF(local), QPointF(overlay->mapToGlobal(local)),
                      Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(overlay, &event);
}

void releaseAt(QWidget *overlay, const QPoint &local)
{
    QMouseEvent event(QEvent::MouseButtonRelease, QPointF(local),
                      QPointF(overlay->mapToGlobal(local)), Qt::LeftButton, Qt::NoButton,
                      Qt::NoModifier);
    QApplication::sendEvent(overlay, &event);
}

void sendKey(QWidget *overlay, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(overlay, &event);
}

// The editor's Translate button, driven through the stub: the two subprocesses
// run, the annotation lands, the button reports busy then done, and the commit
// path hands the renderer an image bitmap that holds the same fill.
void checkControllerWithStub()
{
    std::printf("--- the Translate button, driven by a vshot stub --------------\n");
    QScreen *screen = QApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    QTemporaryDir directory;
    if (!directory.isValid()) {
        expect(false, "a temporary directory for the stub");
        return;
    }
    const QString stub = directory.filePath(QStringLiteral("vshot"));
    expect(writeExecutable(stub, QByteArray(kStubScript)), "the vshot stub is written");
    qputenv("VSHOT_BIN", stub.toUtf8());

    vshot::OverlayController controller(controllerSession(lightFrame(), QStringLiteral("region")));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        qunsetenv("VSHOT_BIN");
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    controller.beginPresetEdit();

    auto *surface = overlay->findChild<QWidget *>(QStringLiteral("toolbarCommandSurface"));
    auto *button = surface != nullptr
        ? surface->findChild<QToolButton *>(QStringLiteral("translateButton"))
        : nullptr;
    expect(button != nullptr, "the toolbar carries a translate button");
    if (button == nullptr) {
        qunsetenv("VSHOT_BIN");
        return;
    }
    expect(button->text() == vshot::uiTr(QStringLiteral("Translate")), "it is labelled",
           button->text());
    expect(!button->toolTip().isEmpty(), "it explains itself");
    expect(!button->icon().isNull(), "it draws its own icon");
    expect(button->toolButtonStyle() == Qt::ToolButtonTextUnderIcon,
           "it is drawn like the tool buttons");

    // The labels the button flashes are driven by the controller's report, so
    // the report itself is captured here; the button's own wiring to it is what
    // the failure test below checks, where it ends on "Failed".
    QVector<vshot::TextOutcome> outcomes;
    controller.setTranslateResultCallback(
        [&outcomes](vshot::TextOutcome outcome, const QString &) { outcomes.push_back(outcome); });

    button->click();
    expect(!outcomes.isEmpty() && outcomes.constFirst() == vshot::TextOutcome::Busy,
           "the controller reports the wait first");
    expect(!outcomes.isEmpty() && outcomes.constLast() == vshot::TextOutcome::Idle,
           "then that it is done");

    expect(controller.annotations().size() == 1, "the button lands one annotation",
           QString::number(controller.annotations().size()));
    if (controller.annotations().size() == 1) {
        const vshot::Annotation &annotation = controller.annotations().constFirst();
        expect(annotation.kind == vshot::Annotation::Kind::Translation,
               "of the translation kind");
        expect(annotation.translation.size() == 1, "with one placed line",
               QString::number(annotation.translation.size()));
        if (annotation.translation.size() == 1) {
            const vshot::TranslatedLine &line = annotation.translation.constFirst();
            expect(line.text == QStringLiteral("你好"), "carrying the pipeline's own answer",
                   line.text);
            expect(sameRect(line.source, 10, 10, 90, 30), "over the original line",
                   rectText(line.source));
            expect(near(line.background, QColor(245, 245, 245)),
                   "filled with the frame's own background",
                   QStringLiteral("got %1").arg(line.background.name()));
            expect(sameRect(annotation.rect, line.fill.x, line.fill.y, line.fill.width,
                            line.fill.height),
                   "the annotation's box is the placed line's fill");
        }
    }

    // The commit path: the annotation is drawn by the helper and reaches the
    // canvas with the line's fill and the translated glyphs on it.  A
    // translation carries its lines' text and boxes, not pixels, so the render
    // is the only place those pixels exist -- if the fill or the glyphs stopped
    // being drawn, nothing else here would notice.
    const QImage composite = renderSelection(controller);
    expect(!composite.isNull(), "the session renders with the translation on it");
    if (!composite.isNull() && controller.annotations().size() == 1) {
        const vshot::LogicalRect &canvas = *controller.selection();
        const vshot::LogicalRect &placed = controller.annotations().constFirst().rect;
        int fillPixels = 0;
        int inkPixels = 0;
        const QRect box(placed.x - canvas.x, placed.y - canvas.y,
                        static_cast<int>(placed.width), static_cast<int>(placed.height));
        for (int y = box.top(); y < box.bottom(); ++y) {
            for (int x = box.left(); x < box.right(); ++x) {
                if (!composite.rect().contains(x, y)) {
                    continue;
                }
                const QColor colour = composite.pixelColor(x, y);
                if (near(colour, QColor(245, 245, 245))) {
                    ++fillPixels;
                } else if (colour.lightness() < 80) {
                    ++inkPixels;
                }
            }
        }
        expect(fillPixels > 0 && inkPixels > 0,
               "and the render holds both the fill and the translated glyphs",
               QStringLiteral("%1 fill, %2 ink").arg(fillPixels).arg(inkPixels));
    }
    qunsetenv("VSHOT_BIN");
}

void checkControllerFailure()
{
    std::printf("--- a failed translation says so -------------------------------\n");
    QScreen *screen = QApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    QTemporaryDir directory;
    if (!directory.isValid()) {
        expect(false, "a temporary directory for the stub");
        return;
    }
    const QString stub = directory.filePath(QStringLiteral("vshot"));
    expect(writeExecutable(stub, QByteArray("#!/bin/sh\nexit 3\n")), "the failing stub is written");
    qputenv("VSHOT_BIN", stub.toUtf8());

    vshot::OverlayController controller(controllerSession(lightFrame(), QStringLiteral("region")));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        qunsetenv("VSHOT_BIN");
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    controller.beginPresetEdit();
    auto *surface = overlay->findChild<QWidget *>(QStringLiteral("toolbarCommandSurface"));
    auto *button = surface != nullptr
        ? surface->findChild<QToolButton *>(QStringLiteral("translateButton"))
        : nullptr;
    if (button == nullptr) {
        qunsetenv("VSHOT_BIN");
        expect(false, "the toolbar carries a translate button");
        return;
    }
    button->click();
    expect(controller.annotations().isEmpty(), "a failed translation lands no annotation");
    expect(button->text() == vshot::uiTr(QStringLiteral("Failed")),
           "and the button says it failed", button->text());
    qunsetenv("VSHOT_BIN");
}

// The standalone translate overlay: frame a region and it translates it in
// place the moment the drag ends, Ctrl+C copies the text, Escape steps back to
// the framing, and Enter accepts and writes the composited PNG.
void checkStandaloneFlow()
{
    std::printf("--- the standalone translate overlay ---------------------------\n");
    QScreen *screen = QApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    QTemporaryDir directory;
    if (!directory.isValid()) {
        expect(false, "a temporary directory for the stub");
        return;
    }
    const QString stub = directory.filePath(QStringLiteral("vshot"));
    expect(writeExecutable(stub, QByteArray(kStubScript)), "the vshot stub is written");
    qputenv("VSHOT_BIN", stub.toUtf8());

    vshot::Session session = controllerSession(lightFrame(), QStringLiteral("translate"));
    session.selection.reset();
    session.resultPath = directory.filePath(QStringLiteral("translated.png"));
    vshot::OverlayController controller(session);
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        qunsetenv("VSHOT_BIN");
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    QString copied;
    controller.setClipboardWriter([&copied](const QString &text) {
        copied = text;
        return true;
    });

    pressAt(overlay, QPoint(10, 10));
    dragTo(overlay, QPoint(200, 200));
    releaseAt(overlay, QPoint(200, 200));
    expect(controller.selection().has_value(), "a drag frames a region");
    expect(!controller.isFinished(), "and the frame does not end the session");
    // The frame is the whole interaction, so finishing it is the request: the
    // translation runs there and then, and Enter is what accepts it instead.
    expect(controller.translatedText() == QStringLiteral("你好"),
           "finishing the frame translates it in place", controller.translatedText());

    sendKey(overlay, Qt::Key_C, Qt::ControlModifier);
    expect(copied == QStringLiteral("你好"), "Ctrl+C copies the translated text only", copied);

    // With the translation up the frame is frozen: a drag must not move a box
    // the painted text would not follow.
    const std::optional<vshot::LogicalRect> framed = controller.selection();
    pressAt(overlay, QPoint(150, 150));
    dragTo(overlay, QPoint(300, 300));
    releaseAt(overlay, QPoint(300, 300));
    const std::optional<vshot::LogicalRect> afterDrag = controller.selection();
    expect(framed.has_value() && afterDrag.has_value() &&
               sameRect(*afterDrag, framed->x, framed->y, framed->width, framed->height),
           "a drag with the translation up leaves the frame alone");

    sendKey(overlay, Qt::Key_Escape);
    expect(!controller.isCancelled() && !controller.isFinished(),
           "Escape steps back to the framing without cancelling");

    // Escape has taken the translation off, so the frame that is still up can be
    // translated again with Enter -- the drag that drew it is not the only way
    // in -- and the Enter after that accepts it.
    sendKey(overlay, Qt::Key_Return);
    sendKey(overlay, Qt::Key_Return);
    expect(controller.isFinished() && !controller.isCancelled(), "the second Enter accepts");
    expect(QFile::exists(session.resultPath), "the composited PNG is written",
           session.resultPath);
    const QJsonDocument document = controller.resultDocument();
    expect(document.object().value(QStringLiteral("translated_text")).toString() ==
               QStringLiteral("你好"),
           "the result carries the translated text");
    expect(document.object().value(QStringLiteral("image_path")).toString() == session.resultPath,
           "and the path it was written to");

    // With no translation up, the first Escape is the cancel the framing has.
    vshot::Session bare = controllerSession(lightFrame(), QStringLiteral("translate"));
    bare.selection.reset();
    vshot::OverlayController second(bare);
    QString secondError;
    vshot::CaptureOverlay *secondOverlay = second.addOverlay(0, screen, &secondError);
    if (secondOverlay != nullptr) {
        // With no frame there is nothing to crop: Enter must not quietly
        // translate the whole screen, and a click that frames nothing is not a
        // request either.
        sendKey(secondOverlay, Qt::Key_Return);
        expect(second.translatedText().isEmpty(), "Enter with no frame translates nothing");
        pressAt(secondOverlay, QPoint(40, 40));
        releaseAt(secondOverlay, QPoint(40, 40));
        expect(second.translatedText().isEmpty(),
               "a click that frames nothing translates nothing");
        sendKey(secondOverlay, Qt::Key_Escape);
        expect(second.isCancelled(), "a first Escape with nothing translated cancels");
    }
    qunsetenv("VSHOT_BIN");
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    checkPlacement();
    checkBackgroundSampler();
    checkFitter();
    checkInkSampler();
    checkPlacementReadsTheGlyphSizeFromTheFrame();
    checkPainting();
    checkNeighbourFillsDoNotEatText();
    checkSessionParsing();
    checkControllerWithStub();
    checkControllerFailure();
    checkStandaloneFlow();

    if (failures != 0) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall translation checks passed\n");
    return 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// Offline check for the text card: how much empty card surrounds the text.
//
// A pinned text card is drawn in the palette's Base colour, which on a light
// theme is white, so every pixel of padding is white. The card used to leave
// 12 logical pixels around the text, which made a short snippet read as a
// white frame with a little text in it rather than as the text itself. The
// property worth locking down is therefore geometric: the gap between the
// outer edge and the first glyph ink, per side, has to stay small -- small
// enough that the card hugs the text, and never so small that the glyphs
// touch the border or get clipped by it.
//
// The measurement is taken from the rendered pixels, not from the layout
// constants, so it also covers the font's own side bearing and line spacing
// (the two things that legitimately add a pixel or two of slack). The
// card's own 1px border is skipped: it sits on the edge and, being a
// translucent pen blended with the background, would otherwise be mistaken
// for ink.
//
// Built only with `-DVSHOT_BUILD_CHECKS=ON`. It needs Qt Gui for
// QTextDocument and an offscreen platform plugin for the palette, but no
// compositor: nothing here talks to a window system.

#include "text_card.hpp"

#include <QColor>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QPalette>
#include <QString>

#include <algorithm>
#include <cstdio>

namespace {

int failures = 0;

// The code card's background, spelled out here on purpose: a check that
// imported it from the renderer would pass for any value at all.
constexpr int kCodeR = 0x1e;
constexpr int kCodeG = 0x1e;
constexpr int kCodeB = 0x1e;

// The most empty card a card may have, in logical pixels between the outer
// edge and the first ink, per side. The padded card this replaced left 13 on
// every side. Horizontally there is almost nothing to excuse beyond the
// padding plus the font's side bearing -- except on a monospace line, whose
// last glyph sits inside a full character cell and can therefore stop a
// character's worth of pixels short of the text width; the bound gives the
// grid that much room and still sits well under the old padding. Vertically
// the font's line spacing above the cap and below the descender adds a few
// pixels, hence the looser bound. Both are far below what the old padding
// produced, which is the point of the check.
constexpr qreal kMaxHorizontalSlack = 8.0;
constexpr qreal kMaxVerticalSlack = 10.0;
// And the ink must not have reached the border: a card that hugs its text
// still has to leave the glyphs alone.
constexpr qreal kMinSlack = 2.0;

// A pixel that differs from the card's background by more than this is ink by
// our definition: glyph interiors are far beyond it, antialiased glyph edges
// land around it, and the border blend (a 20% black pen over white) is what
// the skip below keeps out of the scan instead.
constexpr int kInkDistance = 60;

struct Slack {
    qreal left = 0.0;
    qreal right = 0.0;
    qreal top = 0.0;
    qreal bottom = 0.0;
    int inkPixels = 0;
};

bool differs(const QColor &got, int r, int g, int b)
{
    return std::max({qAbs(got.red() - r), qAbs(got.green() - g), qAbs(got.blue() - b)})
        > kInkDistance;
}

// Measures a card: the distance from each outer edge to the outermost glyph
// pixel, in logical pixels, and the size of the card itself.
Slack measure(const QImage &image, int r, int g, int b, QSize *cardSize)
{
    const qreal ratio = image.devicePixelRatio();
    const int width = image.width();
    const int height = image.height();
    if (cardSize != nullptr) {
        *cardSize = QSize(qRound(width / ratio), qRound(height / ratio));
    }
    // Device pixels to skip at each edge: one logical pixel of border, plus
    // one for the antialiased edge of that line.
    const int skip = qCeil(ratio) + 1;
    Slack slack;
    int left = width;
    int right = -1;
    int top = height;
    int bottom = -1;
    for (int y = skip; y < height - skip; ++y) {
        for (int x = skip; x < width - skip; ++x) {
            if (!differs(image.pixelColor(x, y), r, g, b)) {
                continue;
            }
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
            ++slack.inkPixels;
        }
    }
    if (right < 0) {
        return slack;
    }
    slack.left = left / ratio;
    slack.right = (width - 1 - right) / ratio;
    slack.top = top / ratio;
    slack.bottom = (height - 1 - bottom) / ratio;
    return slack;
}

void report(const char *what, const Slack &slack, const QSize &card, bool enforce)
{
    const bool hug = slack.inkPixels > 0 && slack.left <= kMaxHorizontalSlack
        && slack.right <= kMaxHorizontalSlack && slack.top <= kMaxVerticalSlack
        && slack.bottom <= kMaxVerticalSlack && slack.left >= kMinSlack
        && slack.right >= kMinSlack && slack.top >= kMinSlack && slack.bottom >= kMinSlack;
    if (slack.inkPixels == 0) {
        std::printf("FAIL  %-44s no ink at all\n", what);
        ++failures;
        return;
    }
    if (!enforce) {
        std::printf("note  %-44s %dx%d, empty card %g/%g/%g/%g\n", what, card.width(), card.height(),
                    slack.left, slack.right, slack.top, slack.bottom);
        return;
    }
    if (hug) {
        std::printf("ok    %-44s %dx%d, empty card %g/%g/%g/%g\n", what, card.width(),
                    card.height(), slack.left, slack.right, slack.top, slack.bottom);
        return;
    }
    std::printf("FAIL  %-44s %dx%d, empty card %g/%g/%g/%g (want %g..%g horizontal, %g..%g "
                "vertical)\n",
                what, card.width(), card.height(), slack.left, slack.right, slack.top,
                slack.bottom, kMinSlack, kMaxHorizontalSlack, kMinSlack, kMaxVerticalSlack);
    ++failures;
}

// The card itself has to stay opaque and filled with the colour the renderer
// promised -- a light theme's Base for ordinary text, a fixed dark tone for
// code. Sampled from the left margin, which no line of text may reach.
void expectBackground(const char *what, const QImage &image, int r, int g, int b)
{
    const qreal ratio = image.devicePixelRatio();
    const QPoint device(qRound(2 * ratio), qRound(image.height() / 2.0));
    const QColor got = image.pixelColor(device);
    const bool ok =
        got.red() == r && got.green() == g && got.blue() == b && got.alpha() == 255;
    if (ok) {
        std::printf("ok    %-44s rgba(%d, %d, %d, %d)\n", what, got.red(), got.green(),
                    got.blue(), got.alpha());
        return;
    }
    std::printf("FAIL  %-44s rgba(%d, %d, %d, %d), wanted rgba(%d, %d, %d, 255)\n", what,
                got.red(), got.green(), got.blue(), got.alpha(), r, g, b);
    ++failures;
}

QMimeData *html(const QString &markup)
{
    auto *mime = new QMimeData;
    mime->setHtml(markup);
    return mime;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const QColor base = QGuiApplication::palette().color(QPalette::Base);
    std::printf("card background follows the palette: rgb(%d, %d, %d)\n", base.red(),
                base.green(), base.blue());

    const QString dumpPath = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    const QDir dump(dumpPath);
    const auto leave = [&dump, &dumpPath](const QString &name, const QImage &image) {
        if (dumpPath.isEmpty()) {
            return;
        }
        const QString path = dump.filePath(name);
        if (!image.save(path)) {
            std::printf("FAIL  could not write %s\n", qPrintable(path));
            ++failures;
        }
    };

    const QString oneLine = QStringLiteral("The card hugs the text.");
    const QString wrapped = QStringLiteral(
        "A card that keeps twelve logical pixels of its own background on every side reads as a "
        "white frame around the text; this line is long enough to be wrapped by the card's own "
        "maximum content width, so the right edge it stops at is the layout's, not the text's.");
    const QString code = QStringLiteral("fn main() {\n    let hugs = true;\n    if hugs {\n"
                                        "        println!(\"tight\");\n    }\n}");
    const QString markdown = QStringLiteral("# Heading\n\n- one\n- two\n");
    // An HTML-only clipboard has no plain text: the daemon hands the markup
    // over as the text as well, which is also how an IDE's copy arrives.
    const QString markup = QStringLiteral("<p>An <b>HTML</b> card, as an IDE hands it over.</p>");
    QMimeData *htmlMime = html(markup);

    struct Case {
        const char *name;
        const QMimeData *mime;
        const QString *text;
        bool enforce;
    };
    const Case cases[] = {
        {"plain text", nullptr, &oneLine, true},
        {"plain text, wrapped", nullptr, &wrapped, true},
        {"code card", nullptr, &code, true},
        {"markdown card", nullptr, &markdown, true},
        {"html card", htmlMime, &markup, true},
    };

    for (const Case &item : cases) {
        for (const int ratio : {1, 2, 3}) {
            const QImage image = vshot::renderTextCard(item.mime, *item.text, ratio);
            if (image.isNull()) {
                std::printf("FAIL  %s: no card at %dx\n", item.name, ratio);
                ++failures;
                continue;
            }
            const bool isCode = QString::compare(item.name, "code card") == 0;
            const int r = isCode ? kCodeR : base.red();
            const int g = isCode ? kCodeG : base.green();
            const int b = isCode ? kCodeB : base.blue();
            QSize card;
            const Slack slack = measure(image, r, g, b, &card);
            const QString label = QStringLiteral("%1 @%2x").arg(item.name).arg(ratio);
            report(qPrintable(label), slack, card, item.enforce);
            expectBackground(qPrintable(label + QStringLiteral(", background")), image, r, g, b);
            leave(QStringLiteral("text-card-%1-%2x.png")
                      .arg(item.name)
                      .arg(ratio)
                      .replace(QLatin1Char(' '), QLatin1Char('-'))
                      .replace(QLatin1Char(','), QLatin1Char('-')),
                  image);
        }
    }
    delete htmlMime;

    std::printf("--- result ---------------------------------------------------------\n");
    std::printf("%s (%d failure(s))\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}

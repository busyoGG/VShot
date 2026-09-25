// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// Offline check for the clipboard color parsing and the card it renders. It
// needs no compositor and no layer shell -- `QT_QPA_PLATFORM=offscreen` is
// enough -- so it can run anywhere `cargo test` can. Built only with
// `-DVSHOT_BUILD_CHECKS=ON`; see the README's verification section.

#include "color_card.hpp"

#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QPalette>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void expectLiteral(const QString &text, int r, int g, int b, int a = 255)
{
    QColor color;
    if (!vshot::colorFromLiteral(text, &color)) {
        std::printf("FAIL  %-34s -> not a color (wanted %d,%d,%d,%d)\n", qPrintable(text), r, g, b, a);
        ++failures;
        return;
    }
    if (color.red() != r || color.green() != g || color.blue() != b || color.alpha() != a) {
        std::printf("FAIL  %-34s -> %d,%d,%d,%d (wanted %d,%d,%d,%d)\n", qPrintable(text),
                    color.red(), color.green(), color.blue(), color.alpha(), r, g, b, a);
        ++failures;
        return;
    }
    std::printf("ok    %-34s -> %s\n", qPrintable(text), qPrintable(color.name(QColor::HexArgb)));
}

void expectRejected(const QString &text)
{
    QColor color;
    if (vshot::colorFromLiteral(text, &color)) {
        std::printf("FAIL  %-34s -> wrongly read as %s\n", qPrintable(text), qPrintable(color.name()));
        ++failures;
        return;
    }
    std::printf("ok    %-34s -> rejected\n", qPrintable(QString(text).replace('\n', ' ')));
}

// The X11 form: four 16-bit big-endian channels, padded to 16 bytes.
QByteArray x11Payload(int r, int g, int b, int a)
{
    QByteArray payload;
    for (int value : {r, g, b, a}) {
        payload.append(static_cast<char>((value >> 8) & 0xff));
        payload.append(static_cast<char>(value & 0xff));
    }
    payload.append(QByteArray(8, '\0'));
    return payload;
}

void expectPayload(const QByteArray &payload, const char *what, int r, int g, int b, int a)
{
    QColor color;
    if (!vshot::colorFromX11Payload(payload, &color) || color.red() != r || color.green() != g
        || color.blue() != b || color.alpha() != a) {
        std::printf("FAIL  x-color %-12s -> %s\n", what, qPrintable(color.name(QColor::HexArgb)));
        ++failures;
        return;
    }
    std::printf("ok    x-color %-12s -> %s\n", what, qPrintable(color.name(QColor::HexArgb)));
}

// The card has to reach the canvas edges: with the image's device pixel ratio
// applied by the painter, the last device pixel still belongs to the card.
void expectCard(const QString &directory, const QString &name, const QColor &color, int ratio)
{
    const QImage card = vshot::renderColorCard(color, ratio);
    if (card.isNull()) {
        std::printf("FAIL  render %-20s -> null\n", qPrintable(name));
        ++failures;
        return;
    }
    const int corner = qAlpha(card.pixel(card.width() - 3, card.height() - 3));
    const int middle = qAlpha(card.pixel(card.width() / 2, card.height() / 2));
    const bool scaled = qRound(card.devicePixelRatio()) == ratio;
    if (!directory.isEmpty()) {
        card.save(directory + QLatin1Char('/') + name + QStringLiteral(".png"), "PNG");
    }
    if (corner != 255 || middle != 255 || !scaled) {
        std::printf("FAIL  render %-20s -> corner a=%d, middle a=%d, dpr %g (wanted %d)\n",
                    qPrintable(name), corner, middle, card.devicePixelRatio(), ratio);
        ++failures;
        return;
    }
    std::printf("ok    render %-20s -> %dx%d device px, dpr %g\n", qPrintable(name), card.width(),
                card.height(), card.devicePixelRatio());
}

// The rows both the card and the card's right-click menu read, so these are
// the strings a menu pick can put on the clipboard. What has to hold: the
// formats are the ones the card promises, in the order it prints them, and
// every value is a literal this program itself reads back as that same color
// -- the round trip a paste into a picker or a stylesheet depends on. Only HEX
// and RGB are checked letter for letter; the others go through a parser whose
// 8-bit rounding is allowed one step (an HSL hue cannot always land back on
// the byte it came from, which the literal section above already shows).
//
// A translucent color is the one case where the rows deliberately disagree:
// only the alpha-carrying spellings (HEX8, RGBA) are expected to keep the
// alpha. HEX, RGB, HSL, HSV and CMYK are the opaque spelling of the color, on
// the card and in the menu alike, so what is copied is exactly what is shown.
void expectRows(const char *what, const QColor &color, const QStringList &labels, const QString &hex,
                const QString &rgb)
{
    const QVector<vshot::ColorRow> rows = vshot::colorCardRows(color);
    QStringList got;
    for (const vshot::ColorRow &row : rows) {
        got.append(row.label);
    }
    if (got != labels) {
        std::printf("FAIL  rows %-20s -> %s (wanted %s)\n", what, qPrintable(got.join(QLatin1Char('/'))),
                    qPrintable(labels.join(QLatin1Char('/'))));
        ++failures;
        return;
    }
    QStringList problems;
    if (rows.first().value != hex) {
        problems.append(QStringLiteral("HEX `%1` (wanted `%2`)").arg(rows.first().value, hex));
    }
    for (const vshot::ColorRow &row : rows) {
        if (row.label == QLatin1String("RGB") && row.value != rgb) {
            problems.append(QStringLiteral("RGB `%1` (wanted `%2`)").arg(row.value, rgb));
        }
        if (row.label == QLatin1String("NAME")) {
            // A word, not a literal: it has to name the very same color.
            const QColor named(row.value);
            if (!named.isValid() || named.rgba() != color.rgba()) {
                problems.append(QStringLiteral("NAME `%1` is not this color").arg(row.value));
            }
            continue;
        }
        QColor back;
        const bool parsed = vshot::colorFromLiteral(row.value, &back);
        if (!parsed) {
            problems.append(QStringLiteral("`%1` is not a literal").arg(row.value));
            continue;
        }
        const bool carriesAlpha = row.label == QLatin1String("HEX8")
            || row.label == QLatin1String("RGBA");
        const int alphaStep = carriesAlpha ? qAbs(back.alpha() - color.alpha()) : 0;
        if (qAbs(back.red() - color.red()) > 1 || qAbs(back.green() - color.green()) > 1
            || qAbs(back.blue() - color.blue()) > 1 || alphaStep > 1) {
            problems.append(QStringLiteral("`%1` reads back as %2")
                                .arg(row.value, back.name(QColor::HexArgb)));
        }
    }
    if (!problems.isEmpty()) {
        std::printf("FAIL  rows %-20s -> %s\n", what, qPrintable(problems.join(QLatin1String("; "))));
        ++failures;
        return;
    }
    std::printf("ok    rows %-20s -> %d rows, every value reads back as the color\n", what,
                static_cast<int>(rows.size()));
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const QString directory = app.arguments().value(1);

    std::printf("--- color literals -------------------------------------------------\n");
    expectLiteral("#f00", 255, 0, 0);
    expectLiteral("#ff0", 255, 255, 0);
    expectLiteral("#F00F", 255, 0, 0);
    expectLiteral("#1234", 0x11, 0x22, 0x33, 0x44);
    expectLiteral("#ff0000", 255, 0, 0);
    expectLiteral("#ff000080", 255, 0, 0, 0x80);
    expectLiteral("  #3B82F6  ", 0x3b, 0x82, 0xf6);
    expectLiteral("rgb(255, 0, 0)", 255, 0, 0);
    expectLiteral("rgb(255,0,0)", 255, 0, 0);
    expectLiteral("rgb(100%, 0%, 0%)", 255, 0, 0);
    expectLiteral("rgba(255, 0, 0, 0.5)", 255, 0, 0, 128);
    expectLiteral("rgba(255, 0, 0, 128)", 255, 0, 0, 128);
    expectLiteral("rgba(255, 0, 0, 50%)", 255, 0, 0, 128);
    expectLiteral("rgb(255 0 0 / 50%)", 255, 0, 0, 128);
    expectLiteral("hsl(0, 100%, 50%)", 255, 0, 0);
    // Hue wraps at a full turn: 400 degrees is 40, an orange.
    expectLiteral("hsl(400, 100%, 50%)", 255, 0xaa, 0x00);
    // The CSS hue-to-rgb formula, checked against a hand-computed value.
    expectLiteral("hsl(217, 91%, 60%)", 0x3c, 0x83, 0xf6);
    expectLiteral("hsla(0, 100%, 50%, 0.5)", 255, 0, 0, 128);
    expectLiteral("hsv(0, 100%, 100%)", 255, 0, 0);
    expectLiteral("hsb(120, 100%, 50%)", 0x00, 0x80, 0x00);
    expectLiteral("cmyk(0%, 100%, 100%, 0%)", 255, 0, 0);
    expectLiteral("cmyk(76, 47, 0, 4)", 0x3b, 0x82, 0xf5);
    expectLiteral("cmyka(0%, 100%, 100%, 0%, 0.5)", 255, 0, 0, 128);

    std::printf("--- not colors -----------------------------------------------------\n");
    expectRejected("");
    expectRejected("   ");
    expectRejected("red");      // a word, not a literal
    expectRejected("ff0000");   // no `#`: could be a hash, an id, anything
    expectRejected("0x400000"); // an address in a code snippet
    expectRejected("#gg0000");
    expectRejected("#ff0000ff00");
    expectRejected("color: #ff0000;");
    expectRejected("#ff0000\n#00ff00");
    expectRejected("rgb(255, 0)");
    expectRejected("rgb(255, 0, 0, 0, 0)");
    expectRejected("cmyk(0, 0, 0)");
    expectRejected("cmyk(0, 0, 0, 0, 0)");
    expectRejected("rgb(var(--r), 0, 0)");
    expectRejected("hsl(217deg, 91%, 60%)");
    expectRejected("The border is #ff0000 in the spec");
    expectRejected("rgba(255, 0, 0, 0.5) and that is all\n");

    std::printf("--- application/x-color --------------------------------------------\n");
    expectPayload(x11Payload(0xffff, 0, 0, 0xffff), "opaque red", 255, 0, 0, 255);
    expectPayload(x11Payload(0xffff, 0, 0, 0x8000), "half red", 255, 0, 0, 128);
    expectPayload(x11Payload(0x3b82, 0x8287, 0xf6f6, 0xffff), "3B82F6", 0x3b, 0x82, 0xf6, 255);
    expectPayload(QByteArray(8, '\0'), "black", 0, 0, 0, 0);
    QColor discarded;
    if (vshot::colorFromX11Payload(QByteArray(7, '\0'), &discarded)
        || vshot::colorFromX11Payload(QByteArray(7, '\0'), nullptr)) {
        std::printf("FAIL  a seven-byte payload was accepted\n");
        ++failures;
    } else {
        std::printf("ok    short payload rejected\n");
    }

    std::printf("--- rows the card prints (and the menu offers) ---------------------\n");
    expectRows("opaque red", QColor(255, 0, 0),
               {QStringLiteral("HEX"), QStringLiteral("RGB"), QStringLiteral("HSL"),
                QStringLiteral("HSV"), QStringLiteral("CMYK"), QStringLiteral("NAME")},
               QStringLiteral("#FF0000"), QStringLiteral("rgb(255, 0, 0)"));
    expectRows("opaque 3B82F6", QColor(0x3b, 0x82, 0xf6),
               {QStringLiteral("HEX"), QStringLiteral("RGB"), QStringLiteral("HSL"),
                QStringLiteral("HSV"), QStringLiteral("CMYK")},
               QStringLiteral("#3B82F6"), QStringLiteral("rgb(59, 130, 246)"));
    expectRows("half-transparent 3B82F6", QColor(0x3b, 0x82, 0xf6, 128),
               {QStringLiteral("HEX"), QStringLiteral("HEX8"), QStringLiteral("RGB"),
                QStringLiteral("RGBA"), QStringLiteral("HSL"), QStringLiteral("HSV"),
                QStringLiteral("CMYK")},
               QStringLiteral("#3B82F6"), QStringLiteral("rgb(59, 130, 246)"));

    std::printf("--- cards ----------------------------------------------------------\n");
    expectCard(directory, QStringLiteral("red"), QColor(255, 0, 0), 1);
    expectCard(directory, QStringLiteral("red-2x"), QColor(255, 0, 0), 2);
    expectCard(directory, QStringLiteral("blue"), QColor(0x3b, 0x82, 0xf6), 1);
    expectCard(directory, QStringLiteral("alpha"), QColor(0x3b, 0x82, 0xf6, 128), 1);
    expectCard(directory, QStringLiteral("black"), QColor(0, 0, 0), 1);
    QPalette dark;
    dark.setColor(QPalette::Base, QColor(0x1e, 0x1e, 0x1e));
    dark.setColor(QPalette::Text, QColor(0xe6, 0xe6, 0xe6));
    QGuiApplication::setPalette(dark);
    expectCard(directory, QStringLiteral("dark"), QColor(0x1e, 0x90, 0xff), 1);
    expectCard(directory, QStringLiteral("dark-alpha"), QColor(0x1e, 0x90, 0xff, 96), 1);
    if (vshot::renderColorCard(QColor(), 1).isNull()) {
        std::printf("ok    an invalid color renders nothing\n");
    } else {
        std::printf("FAIL  an invalid color produced a card\n");
        ++failures;
    }

    std::printf("--- result ---------------------------------------------------------\n");
    std::printf("%s (%d failure(s))\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}

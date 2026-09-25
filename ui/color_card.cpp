// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#include "color_card.hpp"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRegularExpression>
#include <QStringList>
#include <QVector>
#include <QtMath>

#include <algorithm>

namespace vshot {
namespace {

// Layout in logical pixels; the card is rasterized at `pixelRatio` device
// pixels per logical pixel so the values stay crisp on HiDPI outputs.
constexpr qreal kPadding = 12.0;
constexpr qreal kBorder = 1.0;
constexpr qreal kChipWidth = 84.0;
constexpr qreal kChipRadius = 10.0;
constexpr qreal kChipGap = 16.0;  // swatch to the label column
constexpr qreal kLabelGap = 14.0; // label column to the value column
constexpr qreal kRowGap = 6.0;    // between two value rows
constexpr qreal kMinContentHeight = 84.0;
constexpr qreal kCheckerSquare = 7.0;
// The longest literal worth treating as a color: `cmyk(100%, 100%, 100%,
// 100%)` is 24 characters and `rgba(255, 255, 255, 0.999)` 28, so anything
// longer has an explanation that is not "a copied color".
constexpr int kMaxLiteralLength = 48;

// One channel as CSS writes it: a bare number, or a percentage when the token
// carries a `%`. `ok` reports whether the token was a number at all, which is
// what keeps prose from parsing as a color.
double number(const QString &token, bool *ok)
{
    QString text = token.trimmed();
    if (text.endsWith(QLatin1Char('%'))) {
        text.chop(1);
    }
    return text.toDouble(ok);
}

bool isPercent(const QString &token)
{
    return token.trimmed().endsWith(QLatin1Char('%'));
}

// A 0..255 channel: percentages are of full scale, bare numbers are bytes.
int channelByte(const QString &token)
{
    bool ok = false;
    const double value = number(token, &ok);
    if (!ok) {
        return -1;
    }
    const double scaled = isPercent(token) ? value * 255.0 / 100.0 : value;
    return qBound(0, qRound(scaled), 255);
}

// Alpha is the one channel CSS writes three ways: a fraction (0..1), a
// percentage, or -- from tools that hand over the raw byte -- 0..255. Returns
// -1 when the token is not a number.
int alphaByte(const QString &token)
{
    bool ok = false;
    const double value = number(token, &ok);
    if (!ok) {
        return -1;
    }
    double fraction = 0.0;
    if (isPercent(token)) {
        fraction = value / 100.0;
    } else if (value > 1.0) {
        fraction = value / 255.0;
    } else {
        fraction = value;
    }
    return qBound(0, qRound(fraction * 255.0), 255);
}

// A 0..1 fraction of a saturation/lightness/value/ink channel, which CSS
// writes as a percentage and some tools as the bare 0..100 number.
double fraction(const QString &token)
{
    bool ok = false;
    const double value = number(token, &ok);
    if (!ok) {
        return 0.0;
    }
    return qBound(0.0, value / 100.0, 1.0);
}

// Hues are degrees; anything outside one turn wraps.
double hueFraction(const QString &token)
{
    bool ok = false;
    double degrees = number(token, &ok);
    if (!ok) {
        return 0.0;
    }
    degrees = std::fmod(degrees, 360.0);
    if (degrees < 0.0) {
        degrees += 360.0;
    }
    return degrees / 360.0;
}

// `#RGB`, `#RGBA`, `#RRGGBB` or `#RRGGBBAA`. The 4- and 8-digit forms are read
// the CSS way, alpha last: that is what web color pickers and design tools
// copy, and it is the spelling this module's own card emits. (Qt's own
// `QColor::name(HexArgb)` writes them alpha first, which is the ambiguity the
// card avoids by labelling the row `HEX8`.)
bool parseHexLiteral(const QString &text, QColor *out)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A#([0-9a-fA-F]{3}|[0-9a-fA-F]{4}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})\\z"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) {
        return false;
    }
    const QString digits = match.captured(1);
    const int channels = (digits.size() == 4 || digits.size() == 8) ? 4 : 3;
    const int perChannel = digits.size() / channels;
    int values[4] = {0, 0, 0, 255};
    for (int index = 0; index < channels; ++index) {
        bool ok = false;
        const int value = digits.mid(index * perChannel, perChannel).toInt(&ok, 16);
        if (!ok) {
            return false;
        }
        // A single hex digit is a nibble repeated: `#f` is 0xff, not 0x0f.
        values[index] = perChannel == 1 ? value * 17 : value;
    }
    *out = QColor(values[0], values[1], values[2], values[3]);
    return out->isValid();
}

// `rgb(...)`, `rgba(...)`, `hsl(...)`, `hsla(...)`, `hsv(...)`/`hsb(...)` and
// `cmyk(...)`/`cmyka(...)`, with the CSS4 `/` allowed in front of the alpha.
bool parseFunctionLiteral(const QString &text, QColor *out)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A([a-zA-Z]{3,5})\\(([^()]*)\\)\\z"));
    static const QRegularExpression separators(QStringLiteral("[,\\s/]+"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) {
        return false;
    }
    const QString name = match.captured(1).toLower();
    const bool rgb = name == QLatin1String("rgb") || name == QLatin1String("rgba");
    const bool hsl = name == QLatin1String("hsl") || name == QLatin1String("hsla");
    const bool hsv = name == QLatin1String("hsv") || name == QLatin1String("hsva")
        || name == QLatin1String("hsb") || name == QLatin1String("hsba");
    const bool cmyka = name == QLatin1String("cmyka");
    const bool cmyk = cmyka || name == QLatin1String("cmyk");
    if (!rgb && !hsl && !hsv && !cmyk) {
        return false;
    }
    const QStringList tokens =
        match.captured(2).split(separators, Qt::SkipEmptyParts);
    // Every argument has to be a number: `rgb(var(--x), 0, 0)` is markup, not
    // a color someone copied. The alpha slot is the fourth argument of the
    // rgb/hsl/hsv families, while in CMYK the fourth is the black channel --
    // `cmyk` takes four arguments and no alpha at all, `cmyka` five.
    const int needed = cmyk ? (cmyka ? 5 : 4) : 3;
    if (tokens.size() < needed || tokens.size() > (cmyk ? needed : 4)) {
        return false;
    }
    for (const QString &token : tokens) {
        bool ok = false;
        number(token, &ok);
        if (!ok) {
            return false;
        }
    }
    int alpha = 255;
    if (cmyka) {
        alpha = alphaByte(tokens.at(4));
    } else if (!cmyk && tokens.size() == 4) {
        alpha = alphaByte(tokens.at(3));
    }
    if (alpha < 0) {
        return false;
    }

    if (rgb) {
        const int red = channelByte(tokens.at(0));
        const int green = channelByte(tokens.at(1));
        const int blue = channelByte(tokens.at(2));
        if (red < 0 || green < 0 || blue < 0) {
            return false;
        }
        *out = QColor(red, green, blue, alpha);
        return out->isValid();
    }
    if (hsl || hsv) {
        const float hue = static_cast<float>(hueFraction(tokens.at(0)));
        const float saturation = static_cast<float>(fraction(tokens.at(1)));
        const float third = static_cast<float>(fraction(tokens.at(2)));
        const float opacity = static_cast<float>(alpha / 255.0);
        *out = hsv ? QColor::fromHsvF(hue, saturation, third, opacity)
                   : QColor::fromHslF(hue, saturation, third, opacity);
        return out->isValid();
    }
    *out = QColor::fromCmykF(static_cast<float>(fraction(tokens.at(0))),
                             static_cast<float>(fraction(tokens.at(1))),
                             static_cast<float>(fraction(tokens.at(2))),
                             static_cast<float>(fraction(tokens.at(3))),
                             static_cast<float>(alpha / 255.0));
    return out->isValid();
}

// The CSS/Qt name of a color, empty when it has none. Qt keeps the names in a
// list rather than a reverse map, and the list is short enough (and a pin rare
// enough) that a scan is the whole lookup.
QString colorName(const QColor &color)
{
    const QStringList names = QColor::colorNames();
    for (const QString &name : names) {
        const QColor candidate(name);
        if (candidate.isValid() && candidate.rgb() == color.rgb()
            && candidate.alpha() == color.alpha()) {
            return name;
        }
    }
    return QString();
}

int percentOf(qreal channel)
{
    return qRound(channel * 100.0);
}

// Qt hands hues over as a fraction of a turn (`hslHueF`, `hsvHueF`), while CSS
// and every picker write degrees; -1 marks an achromatic color, which reads as
// hue 0.
int hueDegrees(qreal hue)
{
    return hue < 0.0 ? 0 : qRound(hue * 360.0);
}

QString hex8(const QColor &color)
{
    const QString rgb = color.name(QColor::HexRgb).mid(1).toUpper();
    const QString alpha =
        QStringLiteral("%1").arg(color.alpha(), 2, 16, QLatin1Char('0')).toUpper();
    return QStringLiteral("#") + rgb + alpha;
}

} // namespace

// Same rows, same order, same text as the card below prints -- the card and the
// right-click menu that offers to copy a value read this one function, so a
// menu entry can never quote a value the card does not show.
QVector<ColorRow> colorCardRows(const QColor &color)
{
    const bool translucent = color.alpha() < 255;
    const QString red = QString::number(color.red());
    const QString green = QString::number(color.green());
    const QString blue = QString::number(color.blue());

    QVector<ColorRow> rows;
    rows.append({QStringLiteral("HEX"), color.name(QColor::HexRgb).toUpper()});
    if (translucent) {
        rows.append({QStringLiteral("HEX8"), hex8(color)});
    }
    rows.append({QStringLiteral("RGB"),
                 QStringLiteral("rgb(%1, %2, %3)").arg(red, green, blue)});
    if (translucent) {
        rows.append({QStringLiteral("RGBA"),
                     QStringLiteral("rgba(%1, %2, %3, %4)")
                         .arg(red, green, blue,
                              QString::number(color.alpha() / 255.0, 'f', 2))});
    }
    rows.append({QStringLiteral("HSL"),
                 QStringLiteral("hsl(%1, %2%, %3%)")
                     .arg(hueDegrees(color.hslHueF()))
                     .arg(percentOf(color.hslSaturationF()))
                     .arg(percentOf(color.lightnessF()))});
    rows.append({QStringLiteral("HSV"),
                 QStringLiteral("hsv(%1, %2%, %3%)")
                     .arg(hueDegrees(color.hsvHueF()))
                     .arg(percentOf(color.hsvSaturationF()))
                     .arg(percentOf(color.valueF()))});
    rows.append({QStringLiteral("CMYK"),
                 QStringLiteral("cmyk(%1%, %2%, %3%, %4%)")
                     .arg(percentOf(color.cyanF()))
                     .arg(percentOf(color.magentaF()))
                     .arg(percentOf(color.yellowF()))
                     .arg(percentOf(color.blackF()))});
    if (const QString name = colorName(color); !name.isEmpty()) {
        rows.append({QStringLiteral("NAME"), name});
    }
    return rows;
}

bool colorFromLiteral(const QString &text, QColor *out)
{
    if (out == nullptr) {
        return false;
    }
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kMaxLiteralLength) {
        return false;
    }
    if (trimmed.contains(QLatin1Char('\n')) || trimmed.contains(QLatin1Char('\r'))) {
        return false;
    }
    // A clipboard that carries a color as text carries nothing else, so a
    // payload that merely mentions one -- a design spec, a CSS rule, a line of
    // code -- stays text and becomes a text card.
    if (trimmed.startsWith(QLatin1Char('#'))) {
        return parseHexLiteral(trimmed, out);
    }
    return parseFunctionLiteral(trimmed, out);
}

bool colorFromX11Payload(const QByteArray &payload, QColor *out)
{
    if (out == nullptr || payload.size() < 8) {
        return false;
    }
    const auto *bytes = reinterpret_cast<const uchar *>(payload.constData());
    int values[4] = {0, 0, 0, 255};
    for (int index = 0; index < 4; ++index) {
        const int raw = (static_cast<int>(bytes[2 * index]) << 8)
            | static_cast<int>(bytes[2 * index + 1]);
        values[index] = qRound(raw * 255.0 / 65535.0);
    }
    *out = QColor(values[0], values[1], values[2], values[3]);
    return out->isValid();
}

QImage renderColorCard(const QColor &color, int pixelRatio)
{
    if (!color.isValid()) {
        return QImage();
    }
    const int ratio = std::clamp(pixelRatio, 1, 4);
    const QVector<ColorRow> rows = colorCardRows(color);

    const QFont labelFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    const QFont valueFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const QFontMetricsF labelMetrics(labelFont);
    const QFontMetricsF valueMetrics(valueFont);

    qreal labelWidth = 0.0;
    qreal valueWidth = 0.0;
    for (const ColorRow &row : rows) {
        labelWidth = std::max(labelWidth, labelMetrics.horizontalAdvance(row.label));
        valueWidth = std::max(valueWidth, valueMetrics.horizontalAdvance(row.value));
    }
    const qreal rowHeight = valueMetrics.height() + kRowGap;
    const qreal rowsHeight = rows.size() * rowHeight - kRowGap;
    const qreal contentHeight = std::max(rowsHeight, kMinContentHeight);
    const qreal contentWidth = kChipWidth + kChipGap + labelWidth + kLabelGap + valueWidth;
    const qreal cardWidth = contentWidth + 2.0 * (kPadding + kBorder);
    const qreal cardHeight = contentHeight + 2.0 * (kPadding + kBorder);

    QImage image(qCeil(cardWidth) * ratio, qCeil(cardHeight) * ratio,
                 QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return QImage();
    }
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // Like the text card, the card follows the application palette, so light
    // and dark themes both stay readable; only the swatch itself is the
    // clipboard's own color, printed as it is.
    const QPalette palette = QGuiApplication::palette();
    const QColor textColor = palette.color(QPalette::Text);
    QColor labelColor = textColor;
    labelColor.setAlpha(160);

    const QRectF cardRect(0.5, 0.5, qCeil(cardWidth) - 1.0, qCeil(cardHeight) - 1.0);
    painter.setPen(QPen(QColor(0, 0, 0, 50), kBorder));
    painter.setBrush(palette.color(QPalette::Base));
    painter.drawRect(cardRect);

    // The swatch: the color itself, the full height of the card's content so
    // it is the first thing the eye lands on.
    const QRectF chip(kPadding + kBorder, cardRect.top() + kPadding + kBorder, kChipWidth,
                      contentHeight);
    QPainterPath chipPath;
    chipPath.addRoundedRect(chip, kChipRadius, kChipRadius);
    painter.save();
    painter.setClipPath(chipPath);
    if (color.alpha() < 255) {
        // Translucency has to be visible, and the checkerboard is what every
        // image editor shows behind a partly transparent pixel.
        painter.fillRect(chip, QColor(0xff, 0xff, 0xff));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0xcc, 0xcc, 0xcc));
        const int columns = qCeil(chip.width() / kCheckerSquare);
        const int lines = qCeil(chip.height() / kCheckerSquare);
        for (int line = 0; line < lines; ++line) {
            for (int column = 0; column < columns; ++column) {
                if ((line + column) % 2 == 0) {
                    continue;
                }
                painter.drawRect(QRectF(chip.left() + column * kCheckerSquare,
                                        chip.top() + line * kCheckerSquare, kCheckerSquare,
                                        kCheckerSquare));
            }
        }
        painter.setBrush(Qt::NoBrush);
    }
    painter.fillRect(chip, color);
    painter.restore();
    QColor chipBorder = textColor;
    chipBorder.setAlpha(90);
    painter.setPen(QPen(chipBorder, kBorder));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(chip.adjusted(0.5, 0.5, -0.5, -0.5), kChipRadius - 0.5,
                            kChipRadius - 0.5);

    // The formats, one row each, right of the swatch.
    const qreal labelX = chip.right() + kChipGap;
    const qreal valueX = labelX + labelWidth + kLabelGap;
    const qreal rowsTop = chip.top() + std::max(0.0, (contentHeight - rowsHeight) / 2.0);
    for (int index = 0; index < rows.size(); ++index) {
        const QRectF box(0.0, rowsTop + index * rowHeight, 0.0, valueMetrics.height());
        painter.setFont(labelFont);
        painter.setPen(labelColor);
        painter.drawText(QRectF(labelX, box.top(), labelWidth, box.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, rows.at(index).label);
        painter.setFont(valueFont);
        painter.setPen(textColor);
        painter.drawText(QRectF(valueX, box.top(), valueWidth + 1.0, box.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, rows.at(index).value);
    }
    painter.end();
    return image;
}

} // namespace vshot

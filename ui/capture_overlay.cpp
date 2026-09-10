#include "capture_overlay.hpp"
#include "i18n.hpp"

#include <LayerShellQt/Window>

#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QConicalGradient>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QLineF>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QStringList>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace vshot {
namespace {

constexpr int kHandleRadius = 6;
constexpr int kMinimumSelection = 5;
constexpr std::uint32_t kTextScale = 2;
constexpr int kMaxUndoSteps = 100;
constexpr int kLoupeRadius = 7;
constexpr int kLoupeZoom = 8;
constexpr int kLoupeDiameter = (2 * kLoupeRadius + 1) * kLoupeZoom;
constexpr int kLoupeMargin = 10;

std::int64_t right(const LogicalRect &rect)
{
    return rect.right();
}

std::int64_t bottom(const LogicalRect &rect)
{
    return rect.bottom();
}

bool intersection(const LogicalRect &first, const LogicalRect &second, LogicalRect *result)
{
    const std::int64_t left = std::max<std::int64_t>(first.x, second.x);
    const std::int64_t top = std::max<std::int64_t>(first.y, second.y);
    const std::int64_t rightEdge = std::min(right(first), right(second));
    const std::int64_t bottomEdge = std::min(bottom(first), bottom(second));
    if (rightEdge <= left || bottomEdge <= top || result == nullptr) {
        return false;
    }
    result->x = static_cast<std::int32_t>(left);
    result->y = static_cast<std::int32_t>(top);
    result->width = static_cast<std::uint32_t>(rightEdge - left);
    result->height = static_cast<std::uint32_t>(bottomEdge - top);
    return true;
}

LogicalRect rectFromEdges(std::int64_t left, std::int64_t top, std::int64_t rightEdge,
                          std::int64_t bottomEdge)
{
    LogicalRect result;
    result.x = static_cast<std::int32_t>(left);
    result.y = static_cast<std::int32_t>(top);
    result.width = static_cast<std::uint32_t>(rightEdge - left);
    result.height = static_cast<std::uint32_t>(bottomEdge - top);
    return result;
}

QRectF localRect(const OutputSession &output, const LogicalRect &rect, const QSize &size)
{
    const double sx = output.geometry.width == 0
        ? 1.0
        : static_cast<double>(size.width()) / static_cast<double>(output.geometry.width);
    const double sy = output.geometry.height == 0
        ? 1.0
        : static_cast<double>(size.height()) / static_cast<double>(output.geometry.height);
    return QRectF((static_cast<double>(rect.x) - output.geometry.x) * sx,
                  (static_cast<double>(rect.y) - output.geometry.y) * sy,
                  static_cast<double>(rect.width) * sx,
                  static_cast<double>(rect.height) * sy);
}

QRect sourceRect(const OutputSession &output, const LogicalRect &rect)
{
    const std::int64_t x = (static_cast<std::int64_t>(rect.x) - output.geometry.x) * output.scale;
    const std::int64_t y = (static_cast<std::int64_t>(rect.y) - output.geometry.y) * output.scale;
    const std::int64_t width = static_cast<std::int64_t>(rect.width) * output.scale;
    const std::int64_t height = static_cast<std::int64_t>(rect.height) * output.scale;
    return QRect(static_cast<int>(x), static_cast<int>(y), static_cast<int>(width),
                 static_cast<int>(height));
}

QPointF localPoint(const OutputSession &output, const Point &point, const QSize &size)
{
    const double sx = output.geometry.width == 0
        ? 1.0
        : static_cast<double>(size.width()) / static_cast<double>(output.geometry.width);
    const double sy = output.geometry.height == 0
        ? 1.0
        : static_cast<double>(size.height()) / static_cast<double>(output.geometry.height);
    return QPointF((static_cast<double>(point.x) - output.geometry.x) * sx,
                   (static_cast<double>(point.y) - output.geometry.y) * sy);
}

QString toolName(Tool tool)
{
    switch (tool) {
    case Tool::Rectangle:
        return QStringLiteral("rectangle");
    case Tool::Ellipse:
        return QStringLiteral("ellipse");
    case Tool::Arrow:
        return QStringLiteral("arrow");
    case Tool::Pen:
        return QStringLiteral("pen");
    case Tool::Mosaic:
        return QStringLiteral("mosaic");
    case Tool::Text:
        return QStringLiteral("text");
    case Tool::Select:
        return QStringLiteral("select");
    }
    return QStringLiteral("pen");
}

QFont textFont(const QString &family, int pixelSize)
{
    QFont font = family.isEmpty() ? QApplication::font() : QFont(family);
    font.setPixelSize(std::max(1, pixelSize));
    return font;
}

QFont annotationFont(const Annotation &annotation)
{
    return textFont(annotation.font, std::max(1, static_cast<int>(7 * annotation.scale)));
}

QSize textMetrics(const Annotation &annotation)
{
    const QFont font = annotationFont(annotation);
    const QFontMetrics metrics(font);
    const QStringList lines = annotation.text.split(QLatin1Char('\n'));
    int width = 1;
    for (const QString &line : lines) {
        width = std::max(width, metrics.horizontalAdvance(line));
    }
    return QSize(width, std::max(1, static_cast<int>(lines.size()) * metrics.lineSpacing()));
}

QCursor cursorForHandle(int handle)
{
    switch (handle) {
    case 1:
    case 5:
        return Qt::SizeFDiagCursor;
    case 2:
    case 6:
        return Qt::SizeVerCursor;
    case 3:
    case 7:
        return Qt::SizeBDiagCursor;
    case 4:
    case 8:
        return Qt::SizeHorCursor;
    case 9:
        return Qt::SizeAllCursor;
    default:
        return Qt::CrossCursor;
    }
}
QIcon toolbarIcon(Tool tool, const QColor &color = QColor(230, 225, 229),
                  qreal devicePixelRatio = 1.0)
{
    const qreal ratio = std::max(1.0, devicePixelRatio);
    QPixmap pixmap(qRound(24 * ratio), qRound(24 * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    switch (tool) {
    case Tool::Select:
        painter.setBrush(color);
        painter.drawPolygon(QPolygonF{QPointF(5, 3), QPointF(18, 14), QPointF(12, 15),
                                      QPointF(9, 21), QPointF(6, 19), QPointF(9, 14),
                                      QPointF(5, 3)});
        break;
    case Tool::Rectangle:
        painter.drawRoundedRect(QRectF(4, 5, 16, 14), 2, 2);
        break;
    case Tool::Ellipse:
        painter.drawEllipse(QRectF(4, 5, 16, 14));
        break;
    case Tool::Arrow:
        painter.drawLine(QPointF(4, 19), QPointF(18, 5));
        painter.drawLine(QPointF(11, 5), QPointF(18, 5));
        painter.drawLine(QPointF(18, 5), QPointF(18, 12));
        break;
    case Tool::Pen:
        painter.drawLine(QPointF(4, 17), QPointF(8, 12));
        painter.drawLine(QPointF(8, 12), QPointF(12, 15));
        painter.drawLine(QPointF(12, 15), QPointF(20, 7));
        break;
    case Tool::Text:
        painter.setFont(QFont(QStringLiteral("Sans"), 15, QFont::Bold));
        painter.drawText(QRectF(3, 2, 18, 20), Qt::AlignCenter, QStringLiteral("T"));
        break;
    case Tool::Mosaic:
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                if ((row + column) % 2 == 0) {
                    painter.drawRoundedRect(QRectF(4 + column * 6, 4 + row * 6, 5, 5),
                                            1, 1);
                }
            }
        }
        break;
    }
    return QIcon(pixmap);
}

QIcon historyIcon(bool redo, const QColor &color = QColor(67, 72, 84),
                  qreal devicePixelRatio = 1.0)
{
    const qreal ratio = std::max(1.0, devicePixelRatio);
    QPixmap pixmap(qRound(24 * ratio), qRound(24 * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QRectF arc(4.0, 4.0, 16.0, 16.0);
    painter.drawArc(arc, redo ? -45 * 16 : 45 * 16, 285 * 16);
    painter.setBrush(color);
    const QPolygonF arrow = redo
        ? QPolygonF{QPointF(18, 4), QPointF(20, 9), QPointF(15, 8)}
        : QPolygonF{QPointF(6, 4), QPointF(4, 9), QPointF(9, 8)};
    painter.drawPolygon(arrow);
    return QIcon(pixmap);
}
QFont pillFont()
{
    QFont font;
    font.setPixelSize(13);
    font.setBold(true);
    return font;
}

// Draws a dark rounded label (dimensions, pixel coordinates) anchored at `anchor`
// inside `bounds`; flips above the anchor when there is no room below.
void drawInfoPill(QPainter *painter, const QPointF &anchor, const QString &text,
                  const QRectF &bounds)
{
    const QFontMetrics metrics(pillFont());
    const int textWidth = metrics.horizontalAdvance(text);
    const int pillWidth = textWidth + 16;
    const int pillHeight = metrics.height() + 8;
    qreal x = anchor.x() - pillWidth / 2.0;
    x = std::clamp(x, bounds.left() + 2.0, std::max(bounds.left() + 2.0, bounds.right() - pillWidth - 2.0));
    qreal y = anchor.y() + 10.0;
    if (y + pillHeight > bounds.bottom()) {
        y = anchor.y() - pillHeight - 10.0;
    }
    y = std::clamp(y, bounds.top() + 2.0, std::max(bounds.top() + 2.0, bounds.bottom() - pillHeight - 2.0));
    const QRectF pill(x, y, pillWidth, pillHeight);
    painter->setFont(pillFont());
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(20, 20, 20, 225));
    painter->drawRoundedRect(pill, 4, 4);
    painter->setPen(QPen(QColor(120, 120, 120), 1.0));
    painter->drawRoundedRect(pill.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    painter->setPen(Qt::white);
    painter->drawText(pill, Qt::AlignCenter, text);
}

// Mosaic strength levels: block size in device pixels for a given output
// scale (P1 fine, P2 standard, P3 coarse) — mirrors edit::mosaic_block_size.
int mosaicBlockForStrength(std::uint32_t strength, int scale)
{
    const int base = std::max(1, 12 * scale);
    switch (strength) {
    case 1:
        return std::max(4, base / 2);
    case 3:
        return base * 2;
    default:
        return base;
    }
}

// Mosaic strength for the freehand brush: smear radius factor — mirrors
// edit::mosaic_brush_radius.
int brushRadiusForStrength(std::uint32_t strength, int radius)
{
    switch (strength) {
    case 1:
        return std::max(1, radius / 2);
    case 3:
        return radius * 2;
    default:
        return radius;
    }
}

// Builds the pen for an annotation, translating the wire line style into a
// dash pattern that approximates the Rust renderer (dashes of 3w with 2w gaps,
// width-wide dots with 2w gaps, in device pixels).
QPen penForAnnotation(const Annotation &annotation)
{
    const bool solid = annotation.dash == QStringLiteral("solid");
    QPen pen(annotation.color, static_cast<double>(annotation.width), Qt::SolidLine,
             solid ? Qt::RoundCap : Qt::FlatCap, Qt::RoundJoin);
    if (annotation.dash == QStringLiteral("dashed")) {
        pen.setDashPattern({3.0, 2.0});
    } else if (annotation.dash == QStringLiteral("dotted")) {
        pen.setDashPattern({0.001, 2.0});
        pen.setCapStyle(Qt::RoundCap);
    }
    return pen;
}

// Dashed rectangles are drawn along the stroke band centerline in the final
// renderer, so the preview walks the same inset path instead of drawRect.
QPolygonF insetRectPolygon(const QRectF &rect, double width)
{
    const double inset = std::max(0.0, (width - 1.0) / 2.0);
    const double outer = std::max(0.0, width / 2.0);
    const double left = rect.left() + inset;
    const double top = rect.top() + inset;
    const double right = std::max(left + 0.5, rect.right() - outer);
    const double bottom = std::max(top + 0.5, rect.bottom() - outer);
    QPolygonF polygon;
    polygon << QPointF(left, top) << QPointF(right, top) << QPointF(right, bottom)
            << QPointF(left, bottom) << QPointF(left, top);
    return polygon;
}

// Computes the average color of one device-pixel block, mirroring the Rust
// block averaging (4x4 subsampling, round-half-up per channel).
QColor averageBlockColor(const uchar *bits, qsizetype bytesPerLine,
                         int x, int y, int width, int height)
{
    std::int64_t sums[4] = {0, 0, 0, 0};
    std::int64_t count = 0;
    const int stepX = std::max(1, width / 4);
    const int stepY = std::max(1, height / 4);
    for (int yy = 0; yy < height; yy += stepY) {
        for (int xx = 0; xx < width; xx += stepX) {
            const uchar *pixel = bits + static_cast<qsizetype>(y + yy) * bytesPerLine +
                static_cast<qsizetype>(x + xx) * 4;
            for (int channel = 0; channel < 4; ++channel) {
                sums[channel] += pixel[channel];
            }
            ++count;
        }
    }
    if (count == 0) {
        return QColor(0, 0, 0);
    }
    return QColor(static_cast<int>(sums[0] / count), static_cast<int>(sums[1] / count),
                  static_cast<int>(sums[2] / count), static_cast<int>(sums[3] / count));
}

void fillLogicalBlock(QPainter *painter, const OutputSession &output, const LogicalRect &bounds,
                      const QSize &size, const QRect &source, int scale, int x, int y, int width,
                      int height, const QColor &color)
{
    LogicalRect blockLogical;
    blockLogical.x = bounds.x + static_cast<std::int32_t>((x - source.x()) / scale);
    blockLogical.y = bounds.y + static_cast<std::int32_t>((y - source.y()) / scale);
    blockLogical.width = static_cast<std::uint32_t>(width / scale);
    blockLogical.height = static_cast<std::uint32_t>(height / scale);
    painter->fillRect(localRect(output, blockLogical, size), color);
}

// Renders a real pixelation mosaic over the annotation bounds, matching the
// Rust renderer: blocks of 12 * scale device pixels averaged independently and
// aligned to the bounds origin.  `mask` picks a rectangular or elliptical area;
// ellipse boundary blocks are averaged and filled per pixel so the edge stays
// as smooth as the final render.
void drawMosaicAnnotation(QPainter *painter, const OutputSession &output,
                          const LogicalRect &bounds, const QString &mask,
                          std::uint32_t strength, const QSize &size)
{
    const QImage &image = output.image;
    if (image.isNull() || bounds.isEmpty()) {
        return;
    }
    const QRect source = sourceRect(output, bounds);
    const QRect clipped = source.intersected(QRect(0, 0, image.width(), image.height()));
    if (clipped.isEmpty()) {
        return;
    }
    const int scale = static_cast<int>(output.scale > 0 ? output.scale : 1);
    const int block = mosaicBlockForStrength(strength, scale);
    const uchar *bits = image.constBits();
    const qsizetype bytesPerLine = image.bytesPerLine();
    const bool ellipse = mask == QStringLiteral("ellipse");
    // Mirrors the Rust integer midline ellipse (center = left + size/2).
    const std::int64_t left = source.x();
    const std::int64_t top = source.y();
    const std::int64_t rectWidth = source.width();
    const std::int64_t rectHeight = source.height();
    const std::int64_t centerX = left + rectWidth / 2;
    const std::int64_t centerY = top + rectHeight / 2;
    const std::int64_t a = std::max<std::int64_t>(2, rectWidth) / 2;
    const std::int64_t b = std::max<std::int64_t>(2, rectHeight) / 2;
    const std::int64_t aSquared = a * a;
    const std::int64_t bSquared = b * b;
    const std::int64_t threshold = aSquared * bSquared;
    auto insideEllipse = [&](std::int64_t px, std::int64_t py) {
        const std::int64_t dx = px - centerX;
        const std::int64_t dy = py - centerY;
        return dx * dx * bSquared + dy * dy * aSquared <= threshold;
    };
    for (int y = clipped.top(); y <= clipped.bottom(); y += block) {
        for (int x = clipped.left(); x <= clipped.right(); x += block) {
            const int width = std::min(block, clipped.right() - x + 1);
            const int height = std::min(block, clipped.bottom() - y + 1);
            if (!ellipse) {
                fillLogicalBlock(painter, output, bounds, size, source, scale, x, y, width,
                                 height,
                                 averageBlockColor(bits, bytesPerLine, x, y, width,
                                                   height));
                continue;
            }
            // The ellipse interior is convex: a block whose corners are all
            // inside is fully inside and skips the per-pixel pass.
            const bool fullyInside = insideEllipse(x, y) && insideEllipse(x + width - 1, y) &&
                insideEllipse(x, y + height - 1) && insideEllipse(x + width - 1, y + height - 1);
            if (fullyInside) {
                fillLogicalBlock(painter, output, bounds, size, source, scale, x, y, width,
                                 height,
                                 averageBlockColor(bits, bytesPerLine, x, y, width,
                                                   height));
                continue;
            }
            std::int64_t sums[4] = {0, 0, 0, 0};
            std::int64_t count = 0;
            for (int yy = 0; yy < height; ++yy) {
                for (int xx = 0; xx < width; ++xx) {
                    if (!insideEllipse(x + xx, y + yy)) {
                        continue;
                    }
                    const uchar *pixel = bits + static_cast<qsizetype>(y + yy) * bytesPerLine +
                        static_cast<qsizetype>(x + xx) * 4;
                    for (int channel = 0; channel < 4; ++channel) {
                        sums[channel] += pixel[channel];
                    }
                    ++count;
                }
            }
            if (count == 0) {
                continue;
            }
            const QColor average(static_cast<int>(sums[0] / count),
                                 static_cast<int>(sums[1] / count),
                                 static_cast<int>(sums[2] / count),
                                 static_cast<int>(sums[3] / count));
            for (int yy = 0; yy < height; ++yy) {
                for (int xx = 0; xx < width; ++xx) {
                    if (!insideEllipse(x + xx, y + yy)) {
                        continue;
                    }
                    fillLogicalBlock(painter, output, bounds, size, source, scale, x + xx,
                                     y + yy, scale, scale, average);
                }
            }
        }
    }
}

// Smears mosaic discs along the path, mirroring Frame::mosaic_brush: device
// space, radius = width/2, stamps every radius/2 pixels, each stamp averaged
// from the pristine source image.
void drawMosaicBrush(QPainter *painter, const OutputSession &output, const QVector<Point> &points,
                     std::uint32_t widthLogical, std::uint32_t strength, const QSize &size)
{
    const QImage &image = output.image;
    if (image.isNull() || points.isEmpty()) {
        return;
    }
    const std::uint32_t scaleValue = output.scale > 0 ? output.scale : 1;
    const double scale = static_cast<double>(scaleValue);
    const int baseRadius = std::clamp(static_cast<int>(widthLogical * scale / 2.0), 1, 512);
    const int radius = std::clamp(brushRadiusForStrength(strength, baseRadius), 1, 512);
    const double step = std::max(1, radius / 2);
    const uchar *bits = image.constBits();
    const qsizetype bytesPerLine = image.bytesPerLine();
    const double radiusSquared = static_cast<double>(radius) * radius;

    QVector<QPointF> centers;
    centers.reserve(points.size());
    centers.append(QPointF((points.constFirst().x - output.geometry.x) * scale,
                           (points.constFirst().y - output.geometry.y) * scale));
    for (int index = 0; index + 1 < points.size(); ++index) {
        const QPointF first((points.at(index).x - output.geometry.x) * scale,
                            (points.at(index).y - output.geometry.y) * scale);
        const QPointF second((points.at(index + 1).x - output.geometry.x) * scale,
                             (points.at(index + 1).y - output.geometry.y) * scale);
        const double length = std::hypot(second.x() - first.x(), second.y() - first.y());
        const int count = std::max(1, static_cast<int>(std::ceil(length / step)));
        for (int k = 1; k <= count; ++k) {
            const double t = static_cast<double>(k) / count;
            centers.append(first + (second - first) * t);
        }
    }

    const bool antialiased = painter->testRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::Antialiasing, false);
    for (const QPointF &center : centers) {
        std::int64_t sums[4] = {0, 0, 0, 0};
        std::int64_t count = 0;
        const int centerX = static_cast<int>(std::lround(center.x()));
        const int centerY = static_cast<int>(std::lround(center.y()));
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (static_cast<double>(dx * dx + dy * dy) > radiusSquared) {
                    continue;
                }
                const int px = centerX + dx;
                const int py = centerY + dy;
                if (px < 0 || py < 0 || px >= image.width() || py >= image.height()) {
                    continue;
                }
                const uchar *pixel = bits + static_cast<qsizetype>(py) * bytesPerLine +
                    static_cast<qsizetype>(px) * 4;
                for (int channel = 0; channel < 4; ++channel) {
                    sums[channel] += pixel[channel];
                }
                ++count;
            }
        }
        if (count == 0) {
            continue;
        }
        const QColor average(static_cast<int>(sums[0] / count), static_cast<int>(sums[1] / count),
                             static_cast<int>(sums[2] / count),
                             static_cast<int>(sums[3] / count));
        const Point global{static_cast<std::int32_t>(std::lround(output.geometry.x +
                                                                 centerX / scale)),
                           static_cast<std::int32_t>(std::lround(output.geometry.y +
                                                                 centerY / scale))};
        const QPointF local = localPoint(output, global, size);
        painter->setPen(Qt::NoPen);
        painter->setBrush(average);
        painter->drawEllipse(local, radius / scale, radius / scale);
    }
    painter->setRenderHint(QPainter::Antialiasing, antialiased);
}

// Superellipse (squircle) outline path, matching the ProcessManager cards
// (corner radius with exponent 5 instead of a plain circular corner).
QPainterPath superellipsePathCorners(const QRectF &bounds, qreal topLeft, qreal topRight,
                                     qreal bottomRight, qreal bottomLeft, qreal exponent)
{
    QPainterPath path;
    const qreal maxRadius = std::min(bounds.width(), bounds.height()) / 2.0;
    topLeft = std::clamp(topLeft, 0.0, maxRadius);
    topRight = std::clamp(topRight, 0.0, maxRadius);
    bottomRight = std::clamp(bottomRight, 0.0, maxRadius);
    bottomLeft = std::clamp(bottomLeft, 0.0, maxRadius);
    const int steps = 12;
    bool first = true;
    // Each corner quadrant runs from an edge midside point to the next; the
    // straight edges close the gaps between consecutive quadrants.
    const auto corner = [&](qreal radius, qreal centerX, qreal centerY, qreal startDegrees,
                            qreal endDegrees) {
        if (radius <= 0.0) {
            if (first) {
                path.moveTo(centerX, centerY);
                first = false;
            } else {
                path.lineTo(centerX, centerY);
            }
            return;
        }
        for (int i = 0; i <= steps; ++i) {
            const qreal angle = qDegreesToRadians(startDegrees +
                                                  (endDegrees - startDegrees) * i / steps);
            const qreal cosine = std::cos(angle);
            const qreal sine = std::sin(angle);
            const qreal x = centerX + std::copysign(std::pow(std::abs(cosine), 2.0 / exponent),
                                                    cosine) * radius;
            const qreal y = centerY + std::copysign(std::pow(std::abs(sine), 2.0 / exponent),
                                                    sine) * radius;
            if (first) {
                path.moveTo(x, y);
                first = false;
            } else {
                path.lineTo(x, y);
            }
        }
    };
    corner(topLeft, bounds.left() + topLeft, bounds.top() + topLeft, 180.0, 270.0);
    corner(topRight, bounds.right() - topRight, bounds.top() + topRight, 270.0, 360.0);
    corner(bottomRight, bounds.right() - bottomRight, bounds.bottom() - bottomRight, 0.0, 90.0);
    corner(bottomLeft, bounds.left() + bottomLeft, bounds.bottom() - bottomLeft, 90.0, 180.0);
    path.closeSubpath();
    return path;
}

QPainterPath superellipsePath(const QRectF &bounds, qreal radius, qreal exponent)
{
    radius = std::clamp(radius, 0.0, std::min(bounds.width(), bounds.height()) / 2.0);
    return superellipsePathCorners(bounds, radius, radius, radius, radius, exponent);
}

// One separable 3x3 box-blur pass; averaging premultiplied channels stays valid.
QImage boxBlurImage(const QImage &source)
{
    if (source.isNull()) {
        return {};
    }
    const QImage input = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int width = input.width();
    const int height = input.height();
    QImage horizontal(width, height, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < height; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(input.constScanLine(y));
        QRgb *out = reinterpret_cast<QRgb *>(horizontal.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const QRgb left = row[std::max(0, x - 1)];
            const QRgb center = row[x];
            const QRgb right = row[std::min(width - 1, x + 1)];
            out[x] = qRgba((qRed(left) + qRed(center) + qRed(right)) / 3,
                           (qGreen(left) + qGreen(center) + qGreen(right)) / 3,
                           (qBlue(left) + qBlue(center) + qBlue(right)) / 3,
                           (qAlpha(left) + qAlpha(center) + qAlpha(right)) / 3);
        }
    }
    QImage result(width, height, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < height; ++y) {
        const QRgb *up =
            reinterpret_cast<const QRgb *>(horizontal.constScanLine(std::max(0, y - 1)));
        const QRgb *middle =
            reinterpret_cast<const QRgb *>(horizontal.constScanLine(y));
        const QRgb *down = reinterpret_cast<const QRgb *>(
            horizontal.constScanLine(std::min(height - 1, y + 1)));
        QRgb *out = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < width; ++x) {
            out[x] = qRgba((qRed(up[x]) + qRed(middle[x]) + qRed(down[x])) / 3,
                           (qGreen(up[x]) + qGreen(middle[x]) + qGreen(down[x])) / 3,
                           (qBlue(up[x]) + qBlue(middle[x]) + qBlue(down[x])) / 3,
                           (qAlpha(up[x]) + qAlpha(middle[x]) + qAlpha(down[x])) / 3);
        }
    }
    return result;
}

// Frosted backdrop: strong downscale + box blur of the frozen frame region
// behind the panel, upscaled with a smooth transform at draw time.
QImage frostedBackdrop(const QImage &frame, const QRect &deviceRect)
{
    if (frame.isNull() || deviceRect.isEmpty()) {
        return {};
    }
    const QImage region = frame.copy(deviceRect);
    const QSize smallSize(std::max(1, region.width() / 14), std::max(1, region.height() / 14));
    const QImage small =
        region.scaled(smallSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return boxBlurImage(small);
}

// Squircle color chip for the swatch row: QSS backgrounds only produce
// circular corners, so the chip and its selection ring are painted.
QIcon swatchIcon(const QColor &color, bool selected, qreal devicePixelRatio)
{
    const qreal ratio = std::max(1.0, devicePixelRatio);
    QPixmap pixmap(qRound(20 * ratio), qRound(20 * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPath(superellipsePath(QRectF(0.5, 0.5, 19.0, 19.0), 7.0, 5.0));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(selected ? QPen(QColor(233, 236, 255), 2.0)
                            : QPen(QColor(86, 93, 104), 1.0));
    painter.drawPath(superellipsePath(QRectF(1.5, 1.5, 17.0, 17.0), 6.0, 5.0));
    return QIcon(pixmap);
}

// Rainbow squircle for the custom color swatch.
QIcon colorPickerIcon(bool selected, qreal devicePixelRatio)
{
    const qreal ratio = std::max(1.0, devicePixelRatio);
    QPixmap pixmap(qRound(20 * ratio), qRound(20 * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QConicalGradient gradient(10.0, 10.0, 90.0);
    for (int index = 0; index <= 6; ++index) {
        gradient.setColorAt(index / 6.0,
                            QColor::fromHsvF(index == 6 ? 0.0 : index / 6.0, 0.75, 1.0));
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawPath(superellipsePath(QRectF(0.5, 0.5, 19.0, 19.0), 7.0, 5.0));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(selected ? QPen(QColor(233, 236, 255), 2.0)
                            : QPen(QColor(86, 93, 104), 1.0));
    painter.drawPath(superellipsePath(QRectF(1.5, 1.5, 17.0, 17.0), 6.0, 5.0));
    return QIcon(pixmap);
}

// Frame for a segmented control (Solid/Dash/Dot, Open V/Filled, Rect/Ellip/Brush).
// The outline, separators, active and hover cards are painted here because
// QSS borders with partial per-button radii render broken (disconnected
// border segments around the rounded ends).
class SegmentFrame final : public QWidget {
public:
    explicit SegmentFrame(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_StyledBackground, false);
    }

    void trackButton(QPushButton *button)
    {
        button->installEventFilter(this);
        buttons_.push_back(button);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
            hovered_ = event->type() == QEvent::Enter ? static_cast<QWidget *>(watched) : nullptr;
            update();
        }
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        constexpr qreal radius = 8.0;
        constexpr qreal cardRadius = 9.0;
        constexpr qreal exponent = 5.0;
        const auto ends = [](const QWidget *button) {
            const QString position = button->property("segmentPosition").toString();
            return std::pair<bool, bool>{position == QStringLiteral("first"),
                                         position == QStringLiteral("last")};
        };
        const auto cardPath = [&ends](QWidget *button, qreal expand) {
            const auto [leftEnd, rightEnd] = ends(button);
            return superellipsePathCorners(
                QRectF(button->geometry()).adjusted(-expand, -expand, expand, expand),
                leftEnd ? cardRadius : 0.0, rightEnd ? cardRadius : 0.0,
                rightEnd ? cardRadius : 0.0, leftEnd ? cardRadius : 0.0, exponent);
        };
        // Inactive hover card, kept inside the frame.
        painter.setPen(Qt::NoPen);
        for (QWidget *button : buttons_) {
            if (!button->isVisibleTo(this) || button->property("active").toBool() ||
                hovered_ != button) {
                continue;
            }
            painter.setBrush(QColor(44, 50, 59));
            painter.drawPath(cardPath(button, 0.0));
        }
        QRect outline;
        for (QWidget *button : buttons_) {
            if (button->isVisibleTo(this)) {
                outline = outline.isNull() ? button->geometry()
                                           : outline.united(button->geometry());
            }
        }
        if (!outline.isNull()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(86, 93, 104), 1.0));
            painter.drawPath(superellipsePathCorners(
                QRectF(outline).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius, radius,
                radius, exponent));
            for (int index = 0; index + 1 < buttons_.size(); ++index) {
                QWidget *left = buttons_.at(index);
                QWidget *right = buttons_.at(index + 1);
                if (!left->isVisibleTo(this) || !right->isVisibleTo(this)) {
                    continue;
                }
                const int x = right->geometry().left();
                painter.drawLine(x, 5, x, height() - 6);
            }
        }
        // The active card paints last, expanded by 1px so it covers the frame
        // and the adjacent separators: a selected segment reads as a card on
        // top of the control instead of a dark-rimmed cell.
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(221, 225, 255));
        for (QWidget *button : buttons_) {
            if (button->isVisibleTo(this) && button->property("active").toBool()) {
                painter.drawPath(cardPath(button, 1.0));
            }
        }
    }

private:
    QVector<QPushButton *> buttons_;
    QWidget *hovered_ = nullptr;
};

// Saturation/value square for a fixed hue.
class SatValPane final : public QWidget {
public:
    using Picked = std::function<void(qreal, qreal)>;

    SatValPane(Picked picked, QWidget *parent)
        : QWidget(parent)
        , picked_(std::move(picked))
    {
        setFixedSize(184, 116);
        setCursor(Qt::CrossCursor);
    }

    void setHue(qreal hue) { hue_ = hue; update(); }
    void setSatVal(qreal sat, qreal val) { sat_ = sat; val_ = val; update(); }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setClipPath(superellipsePathCorners(QRectF(rect()), 6.0, 6.0, 6.0, 6.0, 5.0));
        QLinearGradient across(0, 0, width(), 0);
        across.setColorAt(0.0, QColor(255, 255, 255));
        across.setColorAt(1.0, QColor::fromHsvF(hue_, 1.0, 1.0));
        painter.fillRect(rect(), across);
        QLinearGradient down(0, 0, 0, height());
        down.setColorAt(0.0, QColor(0, 0, 0, 0));
        down.setColorAt(1.0, QColor(0, 0, 0));
        painter.fillRect(rect(), down);
        painter.setClipping(false);
        const QPointF marker(std::clamp(sat_ * (width() - 1), 6.0, width() - 7.0),
                             std::clamp((1.0 - val_) * (height() - 1), 6.0, height() - 7.0));
        painter.setPen(QPen(QColor(0, 0, 0, 160), 4.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(marker, 5.0, 5.0);
        painter.setPen(QPen(Qt::white, 2.0));
        painter.drawEllipse(marker, 5.0, 5.0);
    }

    void mousePressEvent(QMouseEvent *event) override { pick(event->position()); }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons() & Qt::LeftButton) {
            pick(event->position());
        }
    }

private:
    void pick(const QPointF &position)
    {
        sat_ = std::clamp(position.x() / std::max(1, width() - 1), 0.0, 1.0);
        val_ = std::clamp(1.0 - position.y() / std::max(1, height() - 1), 0.0, 1.0);
        update();
        if (picked_) {
            picked_(sat_, val_);
        }
    }

    Picked picked_;
    qreal hue_ = 0.0;
    qreal sat_ = 1.0;
    qreal val_ = 1.0;
};

// Horizontal hue strip.
class HuePane final : public QWidget {
public:
    using Picked = std::function<void(qreal)>;

    HuePane(Picked picked, QWidget *parent)
        : QWidget(parent)
        , picked_(std::move(picked))
    {
        setFixedSize(184, 14);
        setCursor(Qt::CrossCursor);
    }

    void setHue(qreal hue) { hue_ = hue; update(); }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setClipPath(superellipsePathCorners(QRectF(rect()), 7.0, 7.0, 7.0, 7.0, 5.0));
        QLinearGradient across(0, 0, width(), 0);
        for (int index = 0; index <= 6; ++index) {
            across.setColorAt(index / 6.0, QColor::fromHsvF(index == 6 ? 0.0 : index / 6.0, 1.0, 1.0));
        }
        painter.fillRect(rect(), across);
        painter.setClipping(false);
        const QPointF marker(std::clamp(hue_ * (width() - 1), 6.0, width() - 7.0),
                             height() / 2.0);
        painter.setPen(QPen(QColor(0, 0, 0, 160), 4.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(marker, 5.0, 5.0);
        painter.setPen(QPen(Qt::white, 2.0));
        painter.drawEllipse(marker, 5.0, 5.0);
    }

    void mousePressEvent(QMouseEvent *event) override { pick(event->position()); }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons() & Qt::LeftButton) {
            pick(event->position());
        }
    }

private:
    void pick(const QPointF &position)
    {
        hue_ = std::clamp(position.x() / std::max(1, width() - 1), 0.0, 1.0);
        update();
        if (picked_) {
            picked_(hue_);
        }
    }

    Picked picked_;
    qreal hue_ = 0.0;
};

// In-panel HSV color picker. A native QColorDialog is a regular top-level
// window and would open underneath the layer-shell overlay, so the picker is
// a child widget of the toolbar styled like the panel itself.
class ColorPickerPopup final : public QWidget {
public:
    ColorPickerPopup(std::function<void(const QColor &)> apply, QWidget *parent)
        : QWidget(parent)
        , apply_(std::move(apply))
    {
        setObjectName(QStringLiteral("vshotColorPicker"));
        setAttribute(Qt::WA_StyledBackground, false);
        setCursor(Qt::ArrowCursor);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);
        satVal_ = new SatValPane([this](qreal sat, qreal val) { sat_ = sat; val_ = val; syncPreview(); }, this);
        layout->addWidget(satVal_);
        huePane_ = new HuePane([this](qreal value) { hue_ = value; satVal_->setHue(value); syncPreview(); }, this);
        layout->addWidget(huePane_);
        auto *row = new QHBoxLayout;
        row->setSpacing(4);
        preview_ = new QLabel(this);
        preview_->setObjectName(QStringLiteral("colorPreview"));
        preview_->setFixedSize(20, 20);
        row->addWidget(preview_);
        hexEdit_ = new QLineEdit(this);
        hexEdit_->setObjectName(QStringLiteral("colorHexEdit"));
        hexEdit_->setFixedWidth(56);
        hexEdit_->setMaxLength(7);
        hexEdit_->setToolTip(uiTr("Hex color (#rrggbb)"));
        connect(hexEdit_, &QLineEdit::editingFinished, this, [this] { applyHex(); });
        row->addWidget(hexEdit_);
        row->addStretch(1);
        auto *cancel = new QPushButton(uiTr("Cancel"), this);
        cancel->setObjectName(QStringLiteral("cancelButton"));
        cancel->setCursor(Qt::PointingHandCursor);
        cancel->setFocusPolicy(Qt::NoFocus);
        connect(cancel, &QPushButton::clicked, this, [this] { hide(); });
        row->addWidget(cancel);
        auto *ok = new QPushButton(uiTr("OK"), this);
        ok->setObjectName(QStringLiteral("confirmButton"));
        ok->setCursor(Qt::PointingHandCursor);
        ok->setFocusPolicy(Qt::NoFocus);
        connect(ok, &QPushButton::clicked, this, [this] {
            if (apply_) {
                apply_(pickedColor());
            }
            hide();
        });
        row->addWidget(ok);
        layout->addLayout(row);
        setStyleSheet(QStringLiteral(
            "QLineEdit { color: #e6e1e5; background: #2a303a; border: 1px solid #565d68; "
            "border-radius: 7px; padding: 0 5px; min-height: 22px; "
            "selection-color: #00145c; selection-background-color: #dde1ff; } "
            "QLineEdit:focus { border-color: #c7d7f5; } "
            "QPushButton { color: #e6e1e5; background: #333a45; "
            "border: 1px solid transparent; border-radius: 7px; padding: 0 8px; "
            "min-height: 22px; font-size: 12px; } "
            "QPushButton:hover { background: #3c4450; } "
            "QPushButton#confirmButton { background: #dde1ff; color: #00145c; "
            "font-weight: 600; } "
            "QPushButton#confirmButton:hover { background: #e9ecff; } "
            "QPushButton#cancelButton { color: #ffdad6; border-color: #5f3b38; "
            "background: transparent; } "
            "QPushButton#cancelButton:hover { background: #3a2725; }"));
    }

    void setOpener(QWidget *opener) { opener_ = opener; }

    void openAt(const QColor &color, const QPoint &topLeft)
    {
        setHsvFrom(color);
        move(topLeft);
        show();
        raise();
    }

    QColor pickedColor() const
    {
        qreal hue = std::fmod(hue_, 1.0);
        if (hue < 0.0) {
            hue += 1.0;
        }
        return QColor::fromHsvF(hue, std::clamp(sat_, 0.0, 1.0), std::clamp(val_, 0.0, 1.0));
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPainterPath shape = superellipsePath(QRectF(rect()), 18.0, 5.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(30, 34, 41, 236));
        painter.drawPath(shape);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(64, 71, 82), 1.0));
        painter.drawPath(superellipsePath(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                          17.5, 5.0));
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            hide();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        if (QCoreApplication *application = QCoreApplication::instance()) {
            application->installEventFilter(this);
        }
    }

    void hideEvent(QHideEvent *event) override
    {
        QWidget::hideEvent(event);
        if (QCoreApplication *application = QCoreApplication::instance()) {
            application->removeEventFilter(this);
        }
    }

    // Click-outside dismissal: any press that lands outside the popup (and on
    // anything but the swatch that toggles it) closes the picker.
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonPress && watched->isWidgetType() &&
            isVisible()) {
            auto *widget = static_cast<QWidget *>(watched);
            if (!isAncestorOf(widget) && widget != opener_) {
                hide();
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    void setHsvFrom(const QColor &color)
    {
        float hue = -1.0f;
        float sat = 1.0f;
        float val = 1.0f;
        color.getHsvF(&hue, &sat, &val);
        if (hue >= 0.0f) {
            hue_ = hue;
        }
        sat_ = std::clamp(sat, 0.0f, 1.0f);
        val_ = std::clamp(val, 0.0f, 1.0f);
        satVal_->setHue(hue_);
        satVal_->setSatVal(sat_, val_);
        huePane_->setHue(hue_);
        syncPreview();
    }

    void applyHex()
    {
        QString text = hexEdit_->text().trimmed();
        if (!text.startsWith(QLatin1Char('#'))) {
            text.prepend(QLatin1Char('#'));
        }
        const QColor parsed(text);
        if (parsed.isValid()) {
            setHsvFrom(parsed);
        } else {
            syncPreview();
        }
    }

    void syncPreview()
    {
        const QColor color = pickedColor();
        preview_->setPixmap(swatchIcon(color, false, devicePixelRatioF())
                                .pixmap(QSize(20, 20), devicePixelRatioF()));
        const QSignalBlocker blocker(hexEdit_);
        hexEdit_->setText(color.name().toUpper());
    }

    std::function<void(const QColor &)> apply_;
    SatValPane *satVal_ = nullptr;
    HuePane *huePane_ = nullptr;
    QLabel *preview_ = nullptr;
    QLineEdit *hexEdit_ = nullptr;
    QWidget *opener_ = nullptr;
    qreal hue_ = 0.0;
    qreal sat_ = 1.0;
    qreal val_ = 1.0;
};

// Font previews are applied only while visible rows are painted. Applying a
// different QFont to every QListWidgetItem makes Qt measure every installed
// family when the list is shown, which can block the overlay for seconds on
// systems with large font collections.
class FontPreviewDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(1, 26);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem previewOption(option);
        const QString family = index.data(Qt::DisplayRole).toString();
        QFont face(family);
        face.setPointSize(11);
        previewOption.font = face;
        previewOption.fontMetrics = QFontMetrics(face);
        QStyledItemDelegate::paint(painter, previewOption, index);
    }
};

// In-panel font picker. Native font dropdowns are separate top-level windows
// and would open underneath the layer-shell overlay, so the list is a child
// widget of the toolbar styled like the panel itself.
class FontPickerPopup final : public QWidget {
public:
    FontPickerPopup(std::function<void(const QString &)> apply, QWidget *parent)
        : QWidget(parent)
        , apply_(std::move(apply))
    {
        setObjectName(QStringLiteral("vshotFontPicker"));
        setAttribute(Qt::WA_StyledBackground, false);
        setCursor(Qt::ArrowCursor);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        list_ = new QListWidget(this);
        list_->setObjectName(QStringLiteral("fontList"));
        list_->setCursor(Qt::ArrowCursor);
        list_->setFocusPolicy(Qt::NoFocus);
        list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list_->setUniformItemSizes(true);
        list_->setItemDelegate(new FontPreviewDelegate(list_));
        for (const QString &family : QFontDatabase::families()) {
            new QListWidgetItem(family, list_);
        }
        connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
            if (apply_ && item != nullptr) {
                apply_(item->text());
            }
            hide();
        });
        layout->addWidget(list_);
        setStyleSheet(QStringLiteral(
            "QListWidget { background: transparent; border: 0; color: #e6e1e5; "
            "font-size: 12px; outline: 0; } "
            "QListWidget::item { padding: 3px 8px; border-radius: 6px; } "
            "QListWidget::item:hover { background: #2c323b; } "
            "QListWidget::item:selected { background: #dde1ff; color: #00145c; } "
            "QScrollBar:vertical { background: transparent; width: 6px; margin: 2px; } "
            "QScrollBar::handle:vertical { background: #565d68; border-radius: 3px; "
            "min-height: 24px; } "
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; } "
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"));
    }

    void setOpener(QWidget *opener) { opener_ = opener; }

    void openAt(const QString &currentFamily, const QPoint &topLeft)
    {
        const QString family = currentFamily.isEmpty() ? QApplication::font().family() : currentFamily;
        const QList<QListWidgetItem *> matches = list_->findItems(family, Qt::MatchExactly);
        if (matches.isEmpty()) {
            list_->clearSelection();
            list_->setCurrentItem(nullptr);
        } else {
            list_->setCurrentItem(matches.constFirst());
            list_->scrollToItem(matches.constFirst(), QAbstractItemView::PositionAtCenter);
        }
        move(topLeft);
        show();
        raise();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(30, 34, 41, 236));
        painter.drawPath(superellipsePath(QRectF(rect()), 18.0, 5.0));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(64, 71, 82), 1.0));
        painter.drawPath(superellipsePath(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                          17.5, 5.0));
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            hide();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        if (QCoreApplication *application = QCoreApplication::instance()) {
            application->installEventFilter(this);
        }
    }

    void hideEvent(QHideEvent *event) override
    {
        QWidget::hideEvent(event);
        if (QCoreApplication *application = QCoreApplication::instance()) {
            application->removeEventFilter(this);
        }
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonPress && watched->isWidgetType() &&
            isVisible()) {
            auto *widget = static_cast<QWidget *>(watched);
            if (!isAncestorOf(widget) && widget != opener_) {
                hide();
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    QListWidget *list_ = nullptr;
    std::function<void(const QString &)> apply_;
    QWidget *opener_ = nullptr;
};

// Paints squircle highlight cards behind the command-row tool buttons; QSS
// backgrounds are limited to circular corners.
class ToolCardFrame final : public QWidget {
public:
    explicit ToolCardFrame(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_StyledBackground, false);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Enter:
            hovered_ = static_cast<QWidget *>(watched);
            update();
            break;
        case QEvent::Leave:
            if (hovered_ == watched) {
                hovered_ = nullptr;
            }
            update();
            break;
        case QEvent::MouseButtonPress:
            pressed_ = static_cast<QWidget *>(watched);
            update();
            break;
        case QEvent::MouseButtonRelease:
            pressed_ = nullptr;
            update();
            break;
        default:
            break;
        }
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto buttons = findChildren<QToolButton *>();
        QToolButton *active = nullptr;
        for (QToolButton *button : buttons) {
            if (button->isVisibleTo(this) && button->property("active").toBool()) {
                active = button;
                break;
            }
        }
        painter.setPen(Qt::NoPen);
        for (QToolButton *button : buttons) {
            if (!button->isVisibleTo(this) || button == active ||
                (hovered_ != button && pressed_ != button)) {
                continue;
            }
            painter.setBrush(pressed_ == button ? QColor(53, 60, 70) : QColor(44, 50, 59));
            painter.drawPath(superellipsePath(QRectF(button->geometry()), 14.0, 5.0));
        }
        if (active != nullptr) {
            painter.setBrush(QColor(221, 225, 255));
            painter.drawPath(superellipsePath(QRectF(active->geometry()), 14.0, 5.0));
        }
    }

private:
    QWidget *hovered_ = nullptr;
    QWidget *pressed_ = nullptr;
};

} // namespace

struct OverlayController::Gesture {
    enum class Type {
        None,
        Selecting,
        Moving,
        Resizing,
        MovingAnnotation,
        ResizingAnnotation,
        Drawing,
    };

    Type type = Type::None;
    Point anchor;
    Point current;
    LogicalRect origin;
    int handle = 0;
    QVector<Point> points;
};

class OverlayController::FloatingToolbar final : public QWidget {
public:
    explicit FloatingToolbar(OverlayController *controller, QWidget *parent)
        : QWidget(parent)
        , controller_(controller)
    {
        setObjectName(QStringLiteral("vshotToolbar"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_StyledBackground, false);
        setAutoFillBackground(false);
        // Blank panel areas are the drag grip; interactive children override.
        setCursor(Qt::SizeAllCursor);
        setStyleSheet(QStringLiteral(
            "QWidget#vshotToolbar { color: #e6e1e5; } "
            "QLabel { color: #c3c9d1; font-size: 11px; } "
            "QPushButton { color: #e6e1e5; background: transparent; "
            "border: 1px solid transparent; border-radius: 8px; padding: 0 10px; "
            "min-height: 28px; font-size: 12px; } "
            "QPushButton:hover { background: #2c323b; } "
            "QPushButton:pressed { background: #353c46; } "
            "QPushButton:focus { border-color: #c7d7f5; } "
            "QPushButton:disabled { color: #6f7680; background: transparent; "
            "border-color: transparent; } "
            "QToolButton { color: #dfe4ec; background: transparent; border: 0; "
            "border-radius: 10px; padding: 2px; font-size: 10px; } "
            "QToolButton[active=\"true\"] { color: #00145c; } "
            "QToolButton:disabled { background: transparent; color: #6f7680; } "
            "QPushButton#undoButton, QPushButton#redoButton { "
            "border-radius: 10px; padding: 0; } "
            "QPushButton#confirmButton { background: #dde1ff; color: #00145c; "
            "font-weight: 600; } "
            "QPushButton#confirmButton:hover { background: #e9ecff; } "
            "QPushButton#confirmButton:pressed { background: #c3cafb; } "
            "QPushButton#cancelButton { color: #ffdad6; border-color: #5f3b38; } "
            "QPushButton#cancelButton:hover { background: #3a2725; "
            "border-color: #8c5650; } "
            "QFrame#toolbarDivider { background: #3a414b; max-width: 1px; "
            "border: 0; } "
            "QFrame#styleDivider { background: #343b45; max-height: 1px; "
            "border: 0; } "
            "QPushButton[segment=\"true\"] { border: 0; background: transparent; "
            "border-radius: 0; padding: 0 9px; min-height: 22px; color: #d9dde3; } "
            "QPushButton[segment=\"true\"]:hover { color: #ffffff; } "
            "QPushButton[segment=\"true\"]:disabled { color: #6f7680; } "
            "QPushButton[segment=\"true\"][active=\"true\"] { color: #00145c; "
            "font-weight: 600; } "
            "QPushButton[segment=\"true\"][active=\"true\"]:hover { color: #00145c; } "
            "QSlider { min-height: 20px; background: transparent; } "
            "QSlider::groove:horizontal { height: 4px; background: #4a515c; "
            "border-radius: 2px; } "
            "QSlider::sub-page:horizontal { background: #dde1ff; "
            "border-radius: 2px; } "
            "QSlider::add-page:horizontal { background: #4a515c; "
            "border-radius: 2px; } "
            "QSlider::handle:horizontal { width: 14px; margin: -5px 0; "
            "background: #dde1ff; border: 0; border-radius: 7px; } "
            "QSlider::handle:horizontal:hover { background: #e9ecff; } "
            "QSpinBox { min-height: 22px; color: #e6e1e5; background: #2a303a; "
            "border: 1px solid #565d68; border-radius: 8px; padding: 0 5px; "
            "selection-color: #00145c; selection-background-color: #dde1ff; } "
            "QSpinBox:hover { border-color: #7b8290; } "
            "QSpinBox:focus { border-color: #c7d7f5; } "
            "QSpinBox:disabled { color: #6f7680; background: #242933; } "
            "QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { "
            "width: 14px; border: 0; background: transparent; } "
            "QAbstractSpinBox::up-button:hover, QAbstractSpinBox::down-button:hover { "
            "background: #3a414b; border-radius: 4px; }"));

        auto *rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(6, 5, 6, 6);
        rootLayout->setSpacing(4);

        auto *toolSurface = new ToolCardFrame(this);
        toolSurface->setObjectName(QStringLiteral("toolbarCommandSurface"));
        toolSurface->setCursor(Qt::ArrowCursor);
        auto *toolLayout = new QHBoxLayout(toolSurface);
        toolLayout->setContentsMargins(2, 2, 2, 2);
        toolLayout->setSpacing(2);
        addTool(toolLayout, uiTr("Select"), Tool::Select);
        addTool(toolLayout, uiTr("Rect"), Tool::Rectangle);
        addTool(toolLayout, uiTr("Ellipse"), Tool::Ellipse);
        addTool(toolLayout, uiTr("Arrow"), Tool::Arrow);
        addTool(toolLayout, uiTr("Draw"), Tool::Pen);
        addTool(toolLayout, uiTr("Text"), Tool::Text);
        addTool(toolLayout, uiTr("Mosaic"), Tool::Mosaic);
        for (QToolButton *button : toolSurface->findChildren<QToolButton *>()) {
            button->installEventFilter(toolSurface);
        }
        toolLayout->addSpacing(5);
        auto *historyDivider = new QFrame(toolSurface);
        historyDivider->setObjectName(QStringLiteral("toolbarDivider"));
        historyDivider->setFrameShape(QFrame::VLine);
        historyDivider->setFrameShadow(QFrame::Plain);
        historyDivider->setFixedHeight(20);
        historyDivider->setCursor(Qt::ArrowCursor);
        toolLayout->addWidget(historyDivider);
        toolLayout->addSpacing(3);
        undo_ = addActionButton(toolLayout, uiTr("Undo"));
        undo_->setObjectName(QStringLiteral("undoButton"));
        undo_->setText(QString());
        undo_->setIcon(historyIcon(false, QColor(QStringLiteral("#dfe4ec"))));
        undo_->setIconSize(QSize(18, 18));
        undo_->setFixedSize(32, 28);
        undo_->setToolTip(uiTr("Undo last change (Ctrl+Z)"));
        connect(undo_, &QPushButton::clicked, [controller = controller_] { controller->undo(); });
        redo_ = addActionButton(toolLayout, uiTr("Redo"));
        redo_->setObjectName(QStringLiteral("redoButton"));
        redo_->setText(QString());
        redo_->setIcon(historyIcon(true, QColor(QStringLiteral("#dfe4ec"))));
        redo_->setIconSize(QSize(18, 18));
        redo_->setFixedSize(32, 28);
        redo_->setToolTip(uiTr("Redo last change (Ctrl+Y)"));
        connect(redo_, &QPushButton::clicked, [controller = controller_] { controller->redo(); });
        toolLayout->addSpacing(5);
        auto *actionDivider = new QFrame(toolSurface);
        actionDivider->setObjectName(QStringLiteral("toolbarDivider"));
        actionDivider->setFrameShape(QFrame::VLine);
        actionDivider->setFrameShadow(QFrame::Plain);
        actionDivider->setFixedHeight(20);
        actionDivider->setCursor(Qt::ArrowCursor);
        toolLayout->addWidget(actionDivider);
        toolLayout->addSpacing(3);
        auto *ok = addActionButton(toolLayout, uiTr("OK"));
        ok->setObjectName(QStringLiteral("confirmButton"));
        ok->setToolTip(uiTr("Confirm capture (Enter)"));
        connect(ok, &QPushButton::clicked, [controller = controller_] { controller->confirm(); });
        auto *cancel = addActionButton(toolLayout, uiTr("Cancel"));
        cancel->setObjectName(QStringLiteral("cancelButton"));
        cancel->setToolTip(uiTr("Discard capture (Esc)"));
        connect(cancel, &QPushButton::clicked, [controller = controller_] { controller->cancel(); });
        rootLayout->addWidget(toolSurface);

        styleDivider_ = new QFrame(this);
        styleDivider_->setObjectName(QStringLiteral("styleDivider"));
        styleDivider_->setCursor(Qt::ArrowCursor);
        rootLayout->addWidget(styleDivider_);

        styleRow_ = new QWidget(this);
        styleRow_->setObjectName(QStringLiteral("toolbarStyleRow"));
        styleRow_->setCursor(Qt::ArrowCursor);
        styleRow_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        auto *styleLayout = new QVBoxLayout(styleRow_);
        styleLayout->setContentsMargins(2, 2, 2, 2);
        styleLayout->setSpacing(4);
        styleLayout->setAlignment(Qt::AlignLeft);
        optionsRow_ = new QWidget(styleRow_);
        optionsRow_->setObjectName(QStringLiteral("toolbarOptionsRow"));
        optionsRow_->setCursor(Qt::ArrowCursor);
        auto *optionLayout = new QHBoxLayout(optionsRow_);
        optionLayout->setContentsMargins(0, 0, 0, 0);
        optionLayout->setSpacing(4);
        optionLayout->setAlignment(Qt::AlignLeft);
        numericRow_ = new QWidget(styleRow_);
        numericRow_->setObjectName(QStringLiteral("toolbarNumericRow"));
        numericRow_->setCursor(Qt::ArrowCursor);
        auto *numericLayout = new QHBoxLayout(numericRow_);
        numericLayout->setContentsMargins(0, 0, 0, 0);
        numericLayout->setSpacing(4);
        numericLayout->setAlignment(Qt::AlignLeft);

        // Color swatches.
        colorGroup_ = addGroup(optionLayout);
        colorGroup_->setObjectName(QStringLiteral("colorGroup"));
        static const QColor palette[] = {
            QColor(255, 64, 64),   QColor(255, 165, 0), QColor(255, 225, 53),
            QColor(46, 204, 64),   QColor(61, 111, 214), QColor(255, 255, 255),
            QColor(17, 17, 17),
        };
        for (const QColor &color : palette) {
            auto *swatch = new QPushButton(colorGroup_);
            swatch->setObjectName(QStringLiteral("colorSwatch"));
            swatch->setFixedSize(20, 20);
            swatch->setIconSize(QSize(20, 20));
            // The chip is painted by swatchIcon(); keep every QSS background
            // off the button.
            swatch->setStyleSheet(QStringLiteral(
                "QPushButton#colorSwatch { background: transparent; border: 0; "
                "padding: 0; min-height: 0px; } "
                "QPushButton#colorSwatch:hover { background: transparent; } "
                "QPushButton#colorSwatch:pressed { background: transparent; }"));
            swatch->setCursor(Qt::PointingHandCursor);
            swatch->setFocusPolicy(Qt::NoFocus);
            swatch->setAccessibleName(uiTr("Annotation color %1").arg(color.name()));
            swatchColors_.push_back(color);
            swatchButtons_.push_back(swatch);
            colorGroup_->layout()->addWidget(swatch);
            const QColor swatchColor = color;
            connect(swatch, &QPushButton::clicked, [controller = controller_, swatchColor] {
                controller->setCurrentColor(swatchColor);
            });
        }

        // Custom color entry point: opens the HSV picker popup.
        auto *picker = new QPushButton(colorGroup_);
        picker->setObjectName(QStringLiteral("colorPickerButton"));
        picker->setFixedSize(20, 20);
        picker->setIconSize(QSize(20, 20));
        picker->setIcon(colorPickerIcon(false, devicePixelRatioF()));
        picker->setStyleSheet(QStringLiteral(
            "QPushButton#colorPickerButton { background: transparent; border: 0; "
            "padding: 0; min-height: 0px; } "
            "QPushButton#colorPickerButton:hover { background: transparent; } "
            "QPushButton#colorPickerButton:pressed { background: transparent; }"));
        picker->setCursor(Qt::PointingHandCursor);
        picker->setFocusPolicy(Qt::NoFocus);
        picker->setToolTip(uiTr("Custom color"));
        picker->setAccessibleName(uiTr("Custom color"));
        connect(picker, &QPushButton::clicked, this, [this] { togglePickerPopup(); });
        colorGroup_->layout()->addWidget(picker);
        pickerButton_ = picker;

        // Text font family picker. It is only visible while the text tool or a
        // text annotation is active, but the selected family is retained for
        // the next text label.
        fontGroup_ = addGroup(optionLayout);
        fontGroup_->setObjectName(QStringLiteral("fontGroup"));
        fontButton_ = new QPushButton(fontGroup_);
        fontButton_->setObjectName(QStringLiteral("fontButton"));
        fontButton_->setFixedWidth(150);
        fontButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        fontButton_->setCursor(Qt::PointingHandCursor);
        fontButton_->setFocusPolicy(Qt::NoFocus);
        fontButton_->setToolTip(uiTr("Text font family"));
        fontButton_->setAccessibleName(uiTr("Text font family"));
        fontGroup_->layout()->addWidget(fontButton_);
        connect(fontButton_, &QPushButton::clicked, this, [this] { toggleFontPopup(); });

        // Line styles.
        dashGroup_ = addGroup(optionLayout, true);
        dashGroup_->setObjectName(QStringLiteral("dashGroup"));
        addStyleButtons(dashGroup_, {uiTr("Solid"), uiTr("Dash"),
                                     uiTr("Dot")},
                        uiTr("Line style"),
                        {QStringLiteral("solid"), QStringLiteral("dashed"),
                         QStringLiteral("dotted")},
                        &dashButtons_, &dashValues_, [controller = controller_](QString value) {
                            controller->setDash(value);
                        });

        // Arrow head styles.
        arrowStyleGroup_ = addGroup(optionLayout, true);
        arrowStyleGroup_->setObjectName(QStringLiteral("arrowStyleGroup"));
        addStyleButtons(arrowStyleGroup_, {uiTr("Open V"), uiTr("Filled")},
                        uiTr("Arrow head style"),
                        {QStringLiteral("open"), QStringLiteral("filled")},
                        &arrowStyleButtons_, &arrowStyleValues_,
                        [controller = controller_](QString value) {
                            controller->setArrowStyle(value);
                        });

        // Stroke widths (logical pixels).
        widthGroup_ = addGroup(numericLayout);
        widthGroup_->setObjectName(QStringLiteral("widthGroup"));
        widthGroup_->setProperty("toolbarGroupKind", "numeric");
        widthLabel_ = new QLabel(uiTr("Width"), widthGroup_);
        widthLabel_->setFixedWidth(
            QFontMetrics(widthLabel_->font()).horizontalAdvance(uiTr("Width %1").arg(64)));
        widthSlider_ = new QSlider(Qt::Horizontal, widthGroup_);
        widthSlider_->setObjectName(QStringLiteral("widthSlider"));
        widthSlider_->setRange(1, 64);
        widthSlider_->setSingleStep(1);
        widthSlider_->setPageStep(4);
        widthSlider_->setFixedWidth(110);
        widthSlider_->setValue(static_cast<int>(controller_->currentWidth_));
        widthSlider_->setToolTip(uiTr("Stroke width (1-64 logical pixels)"));
        widthGroup_->layout()->addWidget(widthLabel_);
        widthGroup_->layout()->addWidget(widthSlider_);

        // Arrow head size.
        arrowGroup_ = addGroup(numericLayout);
        arrowGroup_->setObjectName(QStringLiteral("arrowSizeGroup"));
        arrowGroup_->setProperty("toolbarGroupKind", "numeric");
        arrowLabel_ = new QLabel(uiTr("Arrow"), arrowGroup_);
        arrowLabel_->setFixedWidth(
            QFontMetrics(arrowLabel_->font()).horizontalAdvance(uiTr("Arrow %1").arg(8)));
        arrowSlider_ = new QSlider(Qt::Horizontal, arrowGroup_);
        arrowSlider_->setObjectName(QStringLiteral("arrowSizeSlider"));
        arrowSlider_->setRange(1, 8);
        arrowSlider_->setSingleStep(1);
        arrowSlider_->setFixedWidth(80);
        arrowSlider_->setValue(static_cast<int>(controller_->arrowSize_));
        arrowSlider_->setToolTip(uiTr("Arrow head size (1-8)"));
        arrowGroup_->layout()->addWidget(arrowLabel_);
        arrowGroup_->layout()->addWidget(arrowSlider_);

        // Text size as a bounded numeric input.
        textGroup_ = addGroup(numericLayout);
        textGroup_->setObjectName(QStringLiteral("textGroup"));
        textGroup_->setProperty("toolbarGroupKind", "numeric");
        textLabel_ = new QLabel(uiTr("Text"), textGroup_);
        textSpin_ = new QSpinBox(textGroup_);
        textSpin_->setObjectName(QStringLiteral("textSizeSpinBox"));
        textSpin_->setRange(1, 64);
        textSpin_->setSingleStep(1);
        textSpin_->setKeyboardTracking(false);
        textSpin_->setFixedSize(64, 22);
        textSpin_->setValue(static_cast<int>(controller_->textSize_));
        textSpin_->setToolTip(uiTr("Text size (1-64)"));
        textGroup_->layout()->addWidget(textLabel_);
        textGroup_->layout()->addWidget(textSpin_);

        // Mosaic area shapes.
        mosaicGroup_ = addGroup(optionLayout, true);
        mosaicGroup_->setObjectName(QStringLiteral("mosaicGroup"));
        addStyleButtons(mosaicGroup_,
                        {uiTr("Rect"), uiTr("Ellip"),
                         uiTr("Brush")},
                        uiTr("Mosaic shape"),
                        {QStringLiteral("rect"), QStringLiteral("ellipse"),
                         QStringLiteral("brush")},
                        &mosaicButtons_, &mosaicValues_,
                        [controller = controller_](QString value) {
                            controller->setMosaicShape(value);
                        });

        // Mosaic strength.
        strengthGroup_ = addGroup(numericLayout);
        strengthGroup_->setObjectName(QStringLiteral("strengthGroup"));
        strengthGroup_->setProperty("toolbarGroupKind", "numeric");
        strengthLabel_ = new QLabel(uiTr("Mosaic"), strengthGroup_);
        strengthLabel_->setFixedWidth(
            QFontMetrics(strengthLabel_->font()).horizontalAdvance(uiTr("Mosaic %1").arg(3)));
        strengthSlider_ = new QSlider(Qt::Horizontal, strengthGroup_);
        strengthSlider_->setObjectName(QStringLiteral("mosaicStrengthSlider"));
        strengthSlider_->setRange(1, 3);
        strengthSlider_->setSingleStep(1);
        strengthSlider_->setFixedWidth(64);
        strengthSlider_->setValue(static_cast<int>(controller_->mosaicStrength_));
        strengthSlider_->setToolTip(uiTr("Mosaic strength (1-3)"));
        strengthGroup_->layout()->addWidget(strengthLabel_);
        strengthGroup_->layout()->addWidget(strengthSlider_);

        styleLayout->addWidget(optionsRow_);
        styleLayout->addWidget(numericRow_);
        rootLayout->addWidget(styleRow_);

        connect(widthSlider_, &QSlider::sliderPressed, this,
                [controller = controller_] { controller->beginStyleAdjustment(); });
        connect(widthSlider_, &QSlider::sliderReleased, this,
                [controller = controller_] { controller->endStyleAdjustment(); });
        connect(widthSlider_, &QSlider::valueChanged, this,
                [controller = controller_](int value) {
                    controller->setWidth(static_cast<std::uint32_t>(value));
                });
        connect(arrowSlider_, &QSlider::sliderPressed, this,
                [controller = controller_] { controller->beginStyleAdjustment(); });
        connect(arrowSlider_, &QSlider::sliderReleased, this,
                [controller = controller_] { controller->endStyleAdjustment(); });
        connect(arrowSlider_, &QSlider::valueChanged, this,
                [controller = controller_](int value) {
                    controller->setArrowSize(static_cast<std::uint32_t>(value));
                });
        connect(strengthSlider_, &QSlider::sliderPressed, this,
                [controller = controller_] { controller->beginStyleAdjustment(); });
        connect(strengthSlider_, &QSlider::sliderReleased, this,
                [controller = controller_] { controller->endStyleAdjustment(); });
        connect(strengthSlider_, &QSlider::valueChanged, this,
                [controller = controller_](int value) {
                    controller->setMosaicStrength(static_cast<std::uint32_t>(value));
                });
        connect(textSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
                [controller = controller_](int value) {
                    controller->beginStyleAdjustment();
                    controller->setTextSize(static_cast<std::uint32_t>(value));
                });
        connect(textSpin_, &QSpinBox::editingFinished, this,
                [controller = controller_] { controller->endStyleAdjustment(); });
        // The popup lives on the overlay, not on the toolbar: Qt clips child
        // widgets to their parent's rect, and the popup must extend past the
        // panel's bounds. It follows the toolbar across overlays below.
        pickerPopup_ = new ColorPickerPopup([controller = controller_](const QColor &color) {
            controller->setCurrentColor(color);
        }, parent);
        pickerPopup_->setOpener(pickerButton_);
        fontPopup_ = new FontPickerPopup([controller = controller_](const QString &family) {
            controller->setCurrentFont(family);
        }, parent);
        fontPopup_->setOpener(fontButton_);
        syncState();
    }

    void syncState()
    {
        const qreal ratio = devicePixelRatioF();
        for (int index = 0; index < toolButtons_.size(); ++index) {
            setToolButtonActive(toolButtons_.at(index), tools_.at(index),
                                tools_.at(index) == controller_->tool_, ratio);
        }
        const Annotation *selected = nullptr;
        if (controller_->selectedAnnotation_ >= 0 &&
            controller_->selectedAnnotation_ < controller_->annotations_.size()) {
            selected = &controller_->annotations_[controller_->selectedAnnotation_];
        }
        undo_->setEnabled(!controller_->undoStack_.isEmpty());
        redo_->setEnabled(!controller_->redoStack_.isEmpty());
        undo_->setIcon(historyIcon(false, QColor(QStringLiteral("#dfe4ec")), ratio));
        redo_->setIcon(historyIcon(true, QColor(QStringLiteral("#dfe4ec")), ratio));

        // The style row edits the selected annotation when one is active,
        // otherwise it edits the pending drawing style.
        const QString target = controller_->styleTargetTool();
        const bool shape = target == QStringLiteral("rectangle") ||
            target == QStringLiteral("ellipse");
        const bool line = shape || target == QStringLiteral("arrow") ||
            target == QStringLiteral("pen");
        const bool text = target == QStringLiteral("text");
        const bool mosaic = target == QStringLiteral("mosaic");
        const bool mosaicBrush = mosaic &&
            (selected != nullptr ? selected->kind == Annotation::Kind::Stroke
                                 : controller_->mosaicShape_ == QStringLiteral("brush"));
        const bool selectedMosaicShape = selected != nullptr &&
            selected->kind == Annotation::Kind::Shape && mosaic;
        const bool showColor = line || text;
        const bool showDash = line;
        const bool showArrowHead = target == QStringLiteral("arrow");
        const bool showWidth = line || mosaicBrush;
        const bool showTextSize = text;
        const bool showFont = text;
        const bool showArrowSize = target == QStringLiteral("arrow");
        const bool showMosaic = mosaic;
        const bool showStrength = mosaic;
        colorGroup_->setVisible(showColor);
        fontGroup_->setVisible(showFont);
        dashGroup_->setVisible(showDash);
        arrowStyleGroup_->setVisible(showArrowHead);
        widthGroup_->setVisible(showWidth);
        textGroup_->setVisible(showTextSize);
        arrowGroup_->setVisible(showArrowSize);
        mosaicGroup_->setVisible(showMosaic);
        strengthGroup_->setVisible(showStrength);
        for (int index = 0; index < mosaicButtons_.size(); ++index) {
            const bool brush = mosaicValues_.at(index) == QStringLiteral("brush");
            mosaicButtons_.at(index)->setEnabled(
                selected == nullptr || !mosaic || (selectedMosaicShape ? !brush : brush));
        }

        // The two property rows share one canonical group order. When the
        // visible groups fit within the row width cap they merge into a
        // single line to keep the panel short; the widest combination (the
        // arrow tool) stays wrapped across two rows.
        struct StyleGroup {
            QWidget *widget;
            bool shown;
            bool optionsRow;
        };
        const StyleGroup orderedGroups[] = {
            {colorGroup_, showColor, true},          {fontGroup_, showFont, true},
            {dashGroup_, showDash, true},
            {arrowStyleGroup_, showArrowHead, true}, {mosaicGroup_, showMosaic, true},
            {widthGroup_, showWidth, false},         {arrowGroup_, showArrowSize, false},
            {textGroup_, showTextSize, false},       {strengthGroup_, showStrength, false},
        };
        QVector<QWidget *> visibleGroups;
        int visibleWidth = 0;
        for (const StyleGroup &entry : orderedGroups) {
            if (!entry.shown) {
                continue;
            }
            visibleGroups.append(entry.widget);
            visibleWidth += entry.widget->sizeHint().width();
        }
        auto *optionLayout = static_cast<QHBoxLayout *>(optionsRow_->layout());
        auto *numericLayout = static_cast<QHBoxLayout *>(numericRow_->layout());
        constexpr int kStyleRowMaxWidth = 640;
        const int groupCount = visibleGroups.size();
        const int mergedWidth = visibleWidth +
            optionLayout->spacing() * std::max(0, groupCount - 1) +
            optionLayout->contentsMargins().left() + optionLayout->contentsMargins().right() +
            4; // styleRow_ side margins
        const bool singleRow = groupCount > 0 && mergedWidth <= kStyleRowMaxWidth;
        const bool anyOptionsGroup = showColor || showFont || showDash || showArrowHead || showMosaic;
        const bool anyNumericGroup = showWidth || showArrowSize || showTextSize || showStrength;
        for (const StyleGroup &entry : orderedGroups) {
            QWidget *homeRow = (singleRow || entry.optionsRow) ? optionsRow_ : numericRow_;
            if (entry.widget->parentWidget() == homeRow) {
                continue;
            }
            if (QWidget *previous = entry.widget->parentWidget()) {
                if (QLayout *layout = previous->layout()) {
                    layout->removeWidget(entry.widget);
                }
            }
            (homeRow == optionsRow_ ? optionLayout : numericLayout)->addWidget(entry.widget);
        }
        optionsRow_->setVisible(singleRow ? groupCount > 0 : anyOptionsGroup);
        numericRow_->setVisible(!singleRow && anyNumericGroup);
        styleRow_->setVisible(optionsRow_->isVisibleTo(styleRow_) ||
                              numericRow_->isVisibleTo(styleRow_));
        styleDivider_->setVisible(styleRow_->isVisible());
        if (pickerPopup_ != nullptr && pickerPopup_->isVisible() &&
            !colorGroup_->isVisibleTo(this)) {
            pickerPopup_->hide();
        }
        if (fontPopup_ != nullptr && fontPopup_->isVisible() &&
            !fontGroup_->isVisibleTo(this)) {
            fontPopup_->hide();
        }
        // The visibility toggles above can leave nested size-hint caches
        // stale when a row's hint is unchanged (e.g. swapping two equally
        // tall groups): adjustSize() would then keep a stale panel height.
        optionsRow_->updateGeometry();
        numericRow_->updateGeometry();
        styleRow_->updateGeometry();

        const QColor color = selected != nullptr ? selected->color : controller_->currentColor_;
        const QString dash = selected != nullptr ? selected->dash : controller_->currentDash_;
        const std::uint32_t width = selected != nullptr ? selected->width : controller_->currentWidth_;
        const std::uint32_t size = selected != nullptr ? selected->size : controller_->arrowSize_;
        const QString arrowStyle = selected != nullptr ? selected->arrowStyle
                                                        : controller_->currentArrowStyle_;
        const QString font = selected != nullptr ? selected->font : controller_->currentFont_;
        const std::uint32_t textSize = selected != nullptr ? selected->scale : controller_->textSize_;
        const QString mask = selected != nullptr
            ? (selected->kind == Annotation::Kind::Stroke ? QStringLiteral("brush") : selected->mask)
            : controller_->mosaicShape_;
        const std::uint32_t strength =
            selected != nullptr ? selected->strength : controller_->mosaicStrength_;
        lastColor_ = color;
        const bool customColor = !swatchColors_.contains(color);
        pickerButton_->setIcon(colorPickerIcon(customColor, ratio));
        for (int index = 0; index < swatchButtons_.size(); ++index) {
            swatchButtons_.at(index)->setIcon(
                swatchIcon(swatchColors_.at(index), swatchColors_.at(index) == color, ratio));
        }
        syncToggleGroup(dashButtons_, dashValues_, dash);
        fontButton_->setText(font.isEmpty() ? QApplication::font().family() : font);
        syncToggleGroup(arrowStyleButtons_, arrowStyleValues_, arrowStyle);
        syncToggleGroup(mosaicButtons_, mosaicValues_, mask);
        {
            const QSignalBlocker widthBlocker(widthSlider_);
            const QSignalBlocker arrowBlocker(arrowSlider_);
            const QSignalBlocker textBlocker(textSpin_);
            const QSignalBlocker strengthBlocker(strengthSlider_);
            widthSlider_->setValue(static_cast<int>(std::clamp(width, 1u, 64u)));
            arrowSlider_->setValue(static_cast<int>(std::clamp(size, 1u, 8u)));
            textSpin_->setValue(static_cast<int>(std::clamp(textSize, 1u, 64u)));
            strengthSlider_->setValue(static_cast<int>(std::clamp(strength, 1u, 3u)));
        }
        widthLabel_->setText(uiTr("Width %1").arg(widthSlider_->value()));
        arrowLabel_->setText(uiTr("Arrow %1").arg(arrowSlider_->value()));
        strengthLabel_->setText(uiTr("Mosaic %1").arg(strengthSlider_->value()));
        layout()->activate();
        adjustSize();
    }

    // Flips the style sub-panel to the far side of the command bar: above it
    // when the panel sits above the selection, below when it sits underneath.
    void setStyleRowAbove(bool above)
    {
        styleRowAbove_ = above;
        auto *box = qobject_cast<QBoxLayout *>(layout());
        if (box != nullptr) {
            box->setDirection(above ? QBoxLayout::BottomToTop : QBoxLayout::TopToBottom);
        }
    }

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ParentChange && pickerPopup_ != nullptr) {
            // The toolbar is re-parented when the selection moves to another
            // output; the popup must tag along.
            pickerPopup_->setParent(parentWidget());
            if (fontPopup_ != nullptr) {
                fontPopup_->setParent(parentWidget());
            }
        }
        return QWidget::event(event);
    }

    void hideEvent(QHideEvent *event) override
    {
        if (pickerPopup_ != nullptr) {
            pickerPopup_->hide();
        }
        if (fontPopup_ != nullptr) {
            fontPopup_->hide();
        }
        QWidget::hideEvent(event);
    }

    // Paints the frosted-glass superellipse surface: the frozen frame behind
    // the panel is sampled, blurred, tinted and clipped to the squircle.
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPainterPath shape = superellipsePath(QRectF(rect()), 26.0, 5.0);
        if (parentWidget() != backdropOwner_ || geometry() != backdropGeometry_) {
            backdropOwner_ = parentWidget();
            backdropGeometry_ = geometry();
            backdrop_ = frostedBackdrop(toolbarFrame(), backdropDeviceRect());
        }
        if (!backdrop_.isNull()) {
            painter.save();
            painter.setClipPath(shape);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(rect(), backdrop_);
            painter.restore();
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(30, 34, 41, 204));
        painter.drawPath(shape);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(64, 71, 82), 1.0));
        painter.drawPath(superellipsePath(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                          25.5, 5.0));
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        // Empty panel areas act as a grip: dragging detaches the panel from
        // its automatic selection-following position.
        if (event->button() == Qt::LeftButton &&
            childAt(event->position().toPoint()) == nullptr) {
            dragging_ = true;
            dragOffset_ = event->globalPosition().toPoint() - mapToGlobal(QPoint(0, 0));
            controller_->notifyPanelDragged();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (dragging_ && (event->buttons() & Qt::LeftButton) && parentWidget() != nullptr) {
            const QPoint target = event->globalPosition().toPoint() - dragOffset_;
            const QPoint local = parentWidget()->mapFromGlobal(target);
            const int x = std::clamp(local.x(), 0,
                                     std::max(0, parentWidget()->width() - width()));
            const int y = std::clamp(local.y(), 0,
                                     std::max(0, parentWidget()->height() - height()));
            move(x, y);
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (dragging_) {
            dragging_ = false;
            // Re-parenting mid-drag would drop the implicit mouse grab, so the
            // panel only hops to another output when the drag settles.
            controller_->settlePanelAtGlobal(event->globalPosition().toPoint() - dragOffset_);
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QImage toolbarFrame() const
    {
        const QWidget *owner = parentWidget();
        if (owner == nullptr) {
            return {};
        }
        for (int index = 0; index < controller_->overlays_.size(); ++index) {
            if (controller_->overlays_.at(index) == owner) {
                if (index >= controller_->session_.outputs.size()) {
                    return {};
                }
                return controller_->session_.outputs.at(index).image;
            }
        }
        return {};
    }

    QRect backdropDeviceRect() const
    {
        const QWidget *owner = parentWidget();
        const QImage frame = toolbarFrame();
        if (owner == nullptr || frame.isNull() || owner->width() <= 0 ||
            owner->height() <= 0) {
            return {};
        }
        const double sx = static_cast<double>(frame.width()) / owner->width();
        const double sy = static_cast<double>(frame.height()) / owner->height();
        const QRect device(static_cast<int>(std::round(x() * sx)),
                           static_cast<int>(std::round(y() * sy)),
                           static_cast<int>(std::round(width() * sx)),
                           static_cast<int>(std::round(height() * sy)));
        return device.intersected(frame.rect());
    }

    static void setButtonActive(QAbstractButton *button, bool active)
    {
        if (button->property("active").toBool() == active) {
            return;
        }
        button->setProperty("active", active);
        button->style()->unpolish(button);
        button->style()->polish(button);
        button->update();
        // SegmentFrame paints the active fill for its child buttons.
        if (QWidget *parent = button->parentWidget()) {
            parent->update();
        }
    }

    static void setToolButtonActive(QAbstractButton *button, Tool tool, bool active, qreal ratio)
    {
        setButtonActive(button, active);
        if (auto *toolButton = qobject_cast<QToolButton *>(button)) {
            toolButton->setIcon(toolbarIcon(
                tool, active ? QColor(QStringLiteral("#12243d"))
                             : QColor(QStringLiteral("#dfe3ea")),
                ratio));
        }
    }

    template<typename Apply>
    void addStyleButtons(QWidget *group, const QStringList &labels, const QString &tooltip,
                         const QStringList &values, QVector<QPushButton *> *buttons,
                         QVector<QString> *storedValues, Apply apply)
    {
        const int segmentCount = labels.size();
        for (int index = 0; index < segmentCount; ++index) {
            auto *button = new QPushButton(labels.at(index), group);
            button->setProperty("segment", true);
            button->setProperty("segmentPosition", index == 0
                                                         ? QStringLiteral("first")
                                                         : index == segmentCount - 1
                                                             ? QStringLiteral("last")
                                                             : QStringLiteral("middle"));
            button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::NoFocus);
            button->setToolTip(tooltip);
            button->setAccessibleName(QStringLiteral("%1: %2").arg(tooltip, labels.at(index)));
            group->layout()->addWidget(button);
            if (group->property("segmentFrame").toBool()) {
                // Only SegmentFrame groups carry this property.
                static_cast<SegmentFrame *>(group)->trackButton(button);
            }
            buttons->push_back(button);
            storedValues->push_back(values.at(index));
            const QString value = values.at(index);
            connect(button, &QPushButton::clicked, [apply, value] { apply(value); });
        }
    }

    template<typename Values>
    void syncToggleGroup(const QVector<QPushButton *> &buttons, const Values &values,
                         const QString &current)
    {
        for (int index = 0; index < buttons.size(); ++index) {
            setButtonActive(buttons.at(index), values.at(index) == current);
        }
    }

    // Opens or closes the custom color picker next to the swatch row, on the
    // same side the style sub-panel expanded (away from the selection).
    void togglePickerPopup()
    {
        if (pickerPopup_->isVisible()) {
            pickerPopup_->hide();
            return;
        }
        pickerPopup_->adjustSize();
        QWidget *overlay = parentWidget();
        if (overlay == nullptr) {
            return;
        }
        // Overlay coordinates: the popup is a child of the overlay so it can
        // extend beyond the toolbar's clipped rect.
        QPoint target = pos() + pickerButton_->mapTo(this, QPoint(0, 0));
        target.rx() -= (pickerPopup_->width() - pickerButton_->width()) / 2;
        constexpr int gap = 4;
        const int above = y() - pickerPopup_->height() - gap;
        const int below = y() + height() + gap;
        int placedY = styleRowAbove_ ? above : below;
        const auto fits = [this, overlay](int value) {
            return value >= 0 && value + pickerPopup_->height() <= overlay->height();
        };
        // Prefer the side the style sub-panel expanded to; fall back to the
        // opposite side when the popup would leave the output.
        if (!fits(placedY)) {
            placedY = fits(above) ? above : below;
        }
        target.ry() = placedY;
        target.rx() = std::clamp(target.x(), 0,
                                 std::max(0, overlay->width() - pickerPopup_->width()));
        target.ry() = std::clamp(target.y(), 0,
                                 std::max(0, overlay->height() - pickerPopup_->height()));
        pickerPopup_->openAt(lastColor_, target);
    }

    void toggleFontPopup()
    {
        if (fontPopup_->isVisible()) {
            fontPopup_->hide();
            return;
        }
        fontPopup_->adjustSize();
        QWidget *overlay = parentWidget();
        if (overlay == nullptr) {
            return;
        }
        QPoint target = pos() + fontButton_->mapTo(this, QPoint(0, 0));
        target.rx() -= (fontPopup_->width() - fontButton_->width()) / 2;
        constexpr int gap = 4;
        const int above = y() - fontPopup_->height() - gap;
        const int below = y() + height() + gap;
        int placedY = styleRowAbove_ ? above : below;
        const auto fits = [this, overlay](int value) {
            return value >= 0 && value + fontPopup_->height() <= overlay->height();
        };
        if (!fits(placedY)) {
            placedY = fits(above) ? above : below;
        }
        target.ry() = std::clamp(placedY, 0,
                                 std::max(0, overlay->height() - fontPopup_->height()));
        target.rx() = std::clamp(target.x(), 0,
                                 std::max(0, overlay->width() - fontPopup_->width()));
        fontPopup_->openAt(fontButton_->text(), target);
    }


    QWidget *addGroup(QHBoxLayout *row, bool framed = false)
    {
        QWidget *group = framed ? new SegmentFrame(this) : new QWidget(this);
        group->setProperty("toolbarGroup", true);
        group->setCursor(Qt::ArrowCursor);
        if (framed) {
            group->setProperty("segmentFrame", true);
        }
        group->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        auto *layout = new QHBoxLayout(group);
        layout->setContentsMargins(2, 2, 2, 2);
        layout->setSpacing(0);
        row->addWidget(group);
        return group;
    }

    QPushButton *addActionButton(QHBoxLayout *layout, const QString &label)
    {
        auto *button = new QPushButton(label, this);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setAccessibleName(label);
        layout->addWidget(button);
        return button;
    }

    void addTool(QHBoxLayout *layout, const QString &label, Tool tool)
    {
        auto *button = new QToolButton(this);
        button->setProperty("toolButton", true);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setText(label);
        button->setIcon(toolbarIcon(tool, QColor(230, 225, 229), devicePixelRatioF()));
        button->setIconSize(QSize(20, 20));
        button->setFixedSize(48, 46);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(toolTipForTool(tool));
        button->setAccessibleName(uiTr("Tool: %1").arg(label));
        layout->addWidget(button);
        tools_.push_back(tool);
        toolButtons_.push_back(button);
        connect(button, &QToolButton::clicked, [controller = controller_, tool] {
            controller->chooseTool(tool);
        });
    }

    static QString toolTipForTool(Tool tool)
    {
        switch (tool) {
        case Tool::Select:
            return uiTr(
                "Adjust selection; click an annotation to select, drag to move, "
                "handles to resize, double-click text to re-edit");
        case Tool::Rectangle:
            return uiTr("Draw a rectangular annotation");
        case Tool::Ellipse:
            return uiTr("Draw an elliptical annotation");
        case Tool::Arrow:
            return uiTr("Draw an arrow with an adjustable head");
        case Tool::Pen:
            return uiTr("Draw a freehand line");
        case Tool::Text:
            return uiTr("Click to place a text label, click text to re-edit");
        case Tool::Mosaic:
            return uiTr("Pixelate an area: rectangle, ellipse or freehand brush");
        }
        return QString();
    }

    OverlayController *controller_;
    QWidget *styleRow_ = nullptr;
    QFrame *styleDivider_ = nullptr;
    QVector<QAbstractButton *> toolButtons_;
    QVector<Tool> tools_;
    QVector<QColor> swatchColors_;
    QVector<QPushButton *> swatchButtons_;
    QWidget *colorGroup_ = nullptr;
    QPushButton *pickerButton_ = nullptr;
    ColorPickerPopup *pickerPopup_ = nullptr;
    QWidget *fontGroup_ = nullptr;
    QPushButton *fontButton_ = nullptr;
    FontPickerPopup *fontPopup_ = nullptr;
    QColor lastColor_;
    bool styleRowAbove_ = false;
    QWidget *dashGroup_ = nullptr;
    QWidget *widthGroup_ = nullptr;
    QWidget *arrowGroup_ = nullptr;
    QWidget *arrowStyleGroup_ = nullptr;
    QWidget *textGroup_ = nullptr;
    QWidget *mosaicGroup_ = nullptr;
    QWidget *strengthGroup_ = nullptr;
    QWidget *optionsRow_ = nullptr;
    QWidget *numericRow_ = nullptr;
    QVector<QPushButton *> dashButtons_;
    QVector<QString> dashValues_;
    QVector<QPushButton *> mosaicButtons_;
    QVector<QString> mosaicValues_;
    QVector<QPushButton *> arrowStyleButtons_;
    QVector<QString> arrowStyleValues_;
    QSlider *widthSlider_ = nullptr;
    QLabel *widthLabel_ = nullptr;
    QSlider *arrowSlider_ = nullptr;
    QLabel *arrowLabel_ = nullptr;
    QSpinBox *textSpin_ = nullptr;
    QLabel *textLabel_ = nullptr;
    QSlider *strengthSlider_ = nullptr;
    QLabel *strengthLabel_ = nullptr;
    QPushButton *undo_ = nullptr;
    QPushButton *redo_ = nullptr;
    bool dragging_ = false;
    QPoint dragOffset_;
    QImage backdrop_;
    QRect backdropGeometry_;
    const QWidget *backdropOwner_ = nullptr;
};

class OverlayController::InlineTextEdit final : public QLineEdit {
public:
    using Finished = std::function<void(bool)>;

    explicit InlineTextEdit(QWidget *parent, Finished finished)
        : QLineEdit(parent)
        , finished_(std::move(finished))
    {
        setAttribute(Qt::WA_DeleteOnClose, false);
        setFrame(true);
        setPlaceholderText(uiTr("Text"));
        // No input validator: the label is rasterized to a bitmap by Qt (with
        // fontconfig fallback) and composited by Rust, so any Unicode text —
        // including CJK typed through an input method — is supported. An ASCII
        // validator here would silently drop IME commits.
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (finished_) {
                finished_(true);
            }
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            if (finished_) {
                finished_(false);
            }
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

private:
    Finished finished_;
};

OverlayController::OverlayController(Session session)
    : session_(std::move(session))
    , gesture_(new Gesture)
{
}

OverlayController::~OverlayController()
{
    removeTextEditor();
    delete toolbar_;
    delete gesture_;
}

int OverlayController::outputCount() const
{
    return session_.outputs.size();
}

const Session &OverlayController::session() const
{
    return session_;
}

CaptureOverlay *OverlayController::addOverlay(int outputIndex, QScreen *screen, QString *error)
{
    if (outputIndex < 0 || outputIndex >= session_.outputs.size() || screen == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid screen/output while creating overlay");
        }
        return nullptr;
    }
    auto *overlay = new CaptureOverlay(outputIndex, this, screen);
    overlays_.push_back(overlay);
    if (toolbar_ == nullptr) {
        toolbarOutput_ = outputIndex;
    }
    return overlay;
}

Point OverlayController::globalPoint(CaptureOverlay *overlay, const QPointF &local) const
{
    const OutputSession &output = overlay->output();
    const double sx = overlay->width() > 0
        ? static_cast<double>(output.geometry.width) / static_cast<double>(overlay->width())
        : 1.0;
    const double sy = overlay->height() > 0
        ? static_cast<double>(output.geometry.height) / static_cast<double>(overlay->height())
        : 1.0;
    const auto x = static_cast<std::int64_t>(std::floor(output.geometry.x + local.x() * sx));
    const auto y = static_cast<std::int64_t>(std::floor(output.geometry.y + local.y() * sy));
    return clampPoint(Point{static_cast<std::int32_t>(std::clamp<std::int64_t>(
                                  x, std::numeric_limits<std::int32_t>::min(),
                                  std::numeric_limits<std::int32_t>::max())),
                              static_cast<std::int32_t>(std::clamp<std::int64_t>(
                                  y, std::numeric_limits<std::int32_t>::min(),
                                  std::numeric_limits<std::int32_t>::max()))});
}

Point OverlayController::clampPoint(Point point) const
{
    const std::int64_t x = std::clamp<std::int64_t>(point.x, session_.bounds.x,
                                                     session_.bounds.right() - 1);
    const std::int64_t y = std::clamp<std::int64_t>(point.y, session_.bounds.y,
                                                     session_.bounds.bottom() - 1);
    return Point{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
}

LogicalRect OverlayController::selectionBetween(Point first, Point second) const
{
    const std::int64_t left = std::min(first.x, second.x);
    const std::int64_t top = std::min(first.y, second.y);
    const std::int64_t rightEdge = std::max(first.x, second.x) + 1;
    const std::int64_t bottomEdge = std::max(first.y, second.y) + 1;
    LogicalRect candidate = rectFromEdges(left, top, rightEdge, bottomEdge);
    LogicalRect result;
    if (intersection(candidate, session_.bounds, &result)) {
        return result;
    }
    return LogicalRect{};
}

LogicalRect OverlayController::moveSelection(LogicalRect origin, Point anchor, Point current) const
{
    const std::int64_t dx = static_cast<std::int64_t>(current.x) - anchor.x;
    const std::int64_t dy = static_cast<std::int64_t>(current.y) - anchor.y;
    const std::int64_t minX = session_.bounds.x;
    const std::int64_t minY = session_.bounds.y;
    const std::int64_t maxX = session_.bounds.right() - origin.width;
    const std::int64_t maxY = session_.bounds.bottom() - origin.height;
    const std::int64_t x = std::clamp<std::int64_t>(origin.x + dx, minX, maxX);
    const std::int64_t y = std::clamp<std::int64_t>(origin.y + dy, minY, maxY);
    return rectFromEdges(x, y, x + origin.width, y + origin.height);
}

LogicalRect OverlayController::resizeSelection(LogicalRect origin, int handle, Point current) const
{
    const std::int64_t boundsLeft = session_.bounds.x;
    const std::int64_t boundsTop = session_.bounds.y;
    const std::int64_t boundsRight = session_.bounds.right();
    const std::int64_t boundsBottom = session_.bounds.bottom();
    std::int64_t left = origin.x;
    std::int64_t top = origin.y;
    std::int64_t rightEdge = origin.right();
    std::int64_t bottomEdge = origin.bottom();
    const std::int64_t x = current.x;
    const std::int64_t y = current.y;
    switch (handle) {
    case 1:
        left = std::clamp<std::int64_t>(x, boundsLeft, rightEdge - 1);
        top = std::clamp<std::int64_t>(y, boundsTop, bottomEdge - 1);
        break;
    case 2:
        top = std::clamp<std::int64_t>(y, boundsTop, bottomEdge - 1);
        break;
    case 3:
        rightEdge = std::clamp<std::int64_t>(x + 1, left + 1, boundsRight);
        top = std::clamp<std::int64_t>(y, boundsTop, bottomEdge - 1);
        break;
    case 4:
        rightEdge = std::clamp<std::int64_t>(x + 1, left + 1, boundsRight);
        break;
    case 5:
        rightEdge = std::clamp<std::int64_t>(x + 1, left + 1, boundsRight);
        bottomEdge = std::clamp<std::int64_t>(y + 1, top + 1, boundsBottom);
        break;
    case 6:
        bottomEdge = std::clamp<std::int64_t>(y + 1, top + 1, boundsBottom);
        break;
    case 7:
        left = std::clamp<std::int64_t>(x, boundsLeft, rightEdge - 1);
        bottomEdge = std::clamp<std::int64_t>(y + 1, top + 1, boundsBottom);
        break;
    case 8:
        left = std::clamp<std::int64_t>(x, boundsLeft, rightEdge - 1);
        break;
    default:
        break;
    }
    return rectFromEdges(left, top, rightEdge, bottomEdge);
}

int OverlayController::hitHandle(Point point) const
{
    if (!selection_.has_value()) {
        return 0;
    }
    const LogicalRect &selection = *selection_;
    const std::int64_t left = selection.x;
    const std::int64_t top = selection.y;
    const std::int64_t rightEdge = selection.right() - 1;
    const std::int64_t bottomEdge = selection.bottom() - 1;
    const auto close = [](std::int64_t first, std::int64_t second) {
        return std::abs(first - second) <= kHandleRadius;
    };
    const bool nearLeft = close(point.x, left);
    const bool nearRight = close(point.x, rightEdge);
    const bool nearTop = close(point.y, top);
    const bool nearBottom = close(point.y, bottomEdge);
    if (nearLeft && nearTop) {
        return 1;
    }
    if (nearTop && close(point.x, (left + rightEdge) / 2)) {
        return 2;
    }
    if (nearRight && nearTop) {
        return 3;
    }
    if (nearRight && close(point.y, (top + bottomEdge) / 2)) {
        return 4;
    }
    if (nearRight && nearBottom) {
        return 5;
    }
    if (nearBottom && close(point.x, (left + rightEdge) / 2)) {
        return 6;
    }
    if (nearLeft && nearBottom) {
        return 7;
    }
    if (nearLeft && close(point.y, (top + bottomEdge) / 2)) {
        return 8;
    }
    if (selection.x <= point.x && point.x < selection.right() && selection.y <= point.y &&
        point.y < selection.bottom()) {
        return 9;
    }
    return 0;
}

void OverlayController::startSelection(Point point)
{
    gesture_->type = Gesture::Type::Selecting;
    gesture_->anchor = clampPoint(point);
    gesture_->current = gesture_->anchor;
    selection_.reset();
    selectedAnnotation_ = -1;
    editing_ = false;
    hideToolbar();
}

void OverlayController::updateSelection(Point point)
{
    gesture_->current = clampPoint(point);
    selection_ = selectionBetween(gesture_->anchor, gesture_->current);
}

void OverlayController::finishSelection(Point point)
{
    updateSelection(point);
    gesture_->type = Gesture::Type::None;
    if (selection_.has_value()) {
        editing_ = true;
        showToolbar();
    }
}

void OverlayController::beginDrawing(Point point)
{
    gesture_->type = Gesture::Type::Drawing;
    gesture_->anchor = clampPoint(point);
    gesture_->current = gesture_->anchor;
    gesture_->points.clear();
    gesture_->points.push_back(gesture_->anchor);
}

void OverlayController::updateDrawing(Point point)
{
    const Point bounded = clampPoint(point);
    gesture_->current = bounded;
    if (tool_ == Tool::Arrow) {
        // Straight arrow: only the anchor and the current point matter, so the
        // gesture never records the wandering intermediate positions.
        gesture_->points = {gesture_->anchor, bounded};
        return;
    }
    if (gesture_->points.isEmpty() || gesture_->points.constLast().x != bounded.x ||
        gesture_->points.constLast().y != bounded.y) {
        gesture_->points.push_back(bounded);
    }
}

void OverlayController::finishDrawing(Point point)
{
    updateDrawing(point);
    const QVector<Point> points = gesture_->points;
    const Tool drawingTool = tool_;
    gesture_->type = Gesture::Type::None;
    gesture_->points.clear();
    if (points.isEmpty()) {
        return;
    }
    if (drawingTool == Tool::Arrow &&
        (points.size() < 2 || points.constFirst() == points.constLast())) {
        return;
    }
    Annotation annotation;
    annotation.tool = toolName(drawingTool);
    annotation.scale = kTextScale;
    annotation.color = currentColor_;
    annotation.width = currentWidth_;
    annotation.dash = currentDash_;
    annotation.size = arrowSize_;
    annotation.arrowStyle = currentArrowStyle_;
    annotation.strength = mosaicStrength_;
    if (drawingTool == Tool::Rectangle || drawingTool == Tool::Ellipse) {
        annotation.kind = Annotation::Kind::Shape;
        annotation.rect = selectionBetween(points.constFirst(), points.constLast());
        // A click without a drag yields a degenerate 1x1 rect: drop it.
        if (annotation.rect.isEmpty() || annotation.rect.width < 2 ||
            annotation.rect.height < 2) {
            return;
        }
    } else if (drawingTool == Tool::Mosaic && mosaicShape_ != QStringLiteral("brush")) {
        // Rectangle/ellipse mosaic is a shape annotation so it can be selected
        // and resized afterwards.
        annotation.kind = Annotation::Kind::Shape;
        annotation.rect = selectionBetween(points.constFirst(), points.constLast());
        annotation.mask = mosaicShape_;
        if (annotation.rect.isEmpty() || annotation.rect.width < 2 ||
            annotation.rect.height < 2) {
            return;
        }
    } else {
        annotation.kind = Annotation::Kind::Stroke;
        annotation.points = points;
    }
    QVector<Annotation> next = annotations_;
    next.push_back(annotation);
    const int newIndex = next.size() - 1;
    mutateAnnotations(std::move(next));
    // Newly drawn annotations stay selected so the panel restyles them and
    // the Select tool can immediately move/resize them.
    selectAnnotation(newIndex);
}

void OverlayController::undo()
{
    if (finished_ || cancelled_ || undoStack_.isEmpty()) {
        return;
    }
    if (textEdit_ != nullptr) {
        finishText(true);
    }
    redoStack_.push_back(annotations_);
    annotations_ = undoStack_.takeLast();
    selectedAnnotation_ = -1;
    updateAll();
}

void OverlayController::redo()
{
    if (finished_ || cancelled_ || redoStack_.isEmpty()) {
        return;
    }
    if (textEdit_ != nullptr) {
        finishText(true);
    }
    undoStack_.push_back(annotations_);
    annotations_ = redoStack_.takeLast();
    selectedAnnotation_ = -1;
    updateAll();
}

void OverlayController::mutateAnnotations(QVector<Annotation> next)
{
    undoStack_.push_back(annotations_);
    if (undoStack_.size() > kMaxUndoSteps) {
        undoStack_.removeFirst();
    }
    redoStack_.clear();
    annotations_ = std::move(next);
    if (selectedAnnotation_ >= annotations_.size()) {
        selectedAnnotation_ = -1;
    }
    updateAll();
}

bool OverlayController::annotationBounds(const Annotation &annotation, LogicalRect *bounds) const
{
    if (annotation.kind == Annotation::Kind::Shape) {
        *bounds = annotation.rect;
        return !bounds->isEmpty();
    }
    const bool hasText = !annotation.text.isEmpty();
    if (annotation.kind == Annotation::Kind::Text) {
        const QSize metrics = textMetrics(annotation);
        bounds->x = annotation.origin.x;
        bounds->y = annotation.origin.y;
        bounds->width = static_cast<std::uint32_t>(metrics.width());
        bounds->height = static_cast<std::uint32_t>(metrics.height());
        return hasText;
    }
    if (annotation.points.isEmpty()) {
        return false;
    }
    std::int32_t minX = annotation.points.constFirst().x;
    std::int32_t maxX = minX;
    std::int32_t minY = annotation.points.constFirst().y;
    std::int32_t maxY = minY;
    for (const Point &point : annotation.points) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }
    bounds->x = minX;
    bounds->y = minY;
    bounds->width = static_cast<std::uint32_t>(static_cast<std::int64_t>(maxX) - minX + 1);
    bounds->height = static_cast<std::uint32_t>(static_cast<std::int64_t>(maxY) - minY + 1);
    return true;
}

bool OverlayController::canDrawAt(Point point) const
{
    return point.x >= session_.bounds.x && point.x < session_.bounds.right() &&
           point.y >= session_.bounds.y && point.y < session_.bounds.bottom();
}

void OverlayController::beginText(CaptureOverlay *overlay, Point point)
{
    if (textEdit_ != nullptr) {
        finishText(true);
    }
    point = clampPoint(point);
    int index = -1;
    for (int i = annotations_.size() - 1; i >= 0; --i) {
        LogicalRect bounds;
        if (annotations_.at(i).kind == Annotation::Kind::Text &&
            annotationBounds(annotations_.at(i), &bounds) && bounds.x - 3 <= point.x &&
            point.x < bounds.right() + 3 && bounds.y - 3 <= point.y &&
            point.y < bounds.bottom() + 3) {
            index = i;
            break;
        }
    }
    startTextEditor(overlay, index, index >= 0 ? annotations_.at(index).origin : point);
}

void OverlayController::startTextEditor(CaptureOverlay *overlay, int index, Point origin)
{
    QString initial;
    if (index >= 0) {
        // Snapshot the pre-edit state now so accepting the replacement keeps a
        // single undo step that restores the original text.
        undoStack_.push_back(annotations_);
        if (undoStack_.size() > kMaxUndoSteps) {
            undoStack_.removeFirst();
        }
        redoStack_.clear();
        textEditSnapshot_ = true;
        cancelledText_ = annotations_.takeAt(index);
        editingTextIndex_ = index;
        initial = cancelledText_->text;
        origin = cancelledText_->origin;
        selectedAnnotation_ = -1;
    } else {
        cancelledText_.reset();
        editingTextIndex_ = -1;
        // A fresh label must not inherit the previous selection: otherwise
        // style edits made while this editor is open (font/color/size) would
        // restyle the last committed annotation instead.
        selectedAnnotation_ = -1;
    }
    // While re-editing, the editor mirrors the annotation's own style so the
    // user must not re-pick it after moving a label around.
    const std::uint32_t editScale = cancelledText_.has_value() ? cancelledText_->scale : textSize_;
    const QColor editColor = cancelledText_.has_value() ? cancelledText_->color : currentColor_;
    textEditFont_ = cancelledText_.has_value() ? cancelledText_->font : currentFont_;
    textOutput_ = overlay->outputIndex();
    textOrigin_ = origin;
    auto *owner = overlay;
    textEdit_ = new InlineTextEdit(owner, [this](bool accept) {
        if (accept) {
            finishText(true);
        } else {
            finishText(false);
        }
    });
    textEdit_->setText(initial);
    QFont editorFont = textFont(textEditFont_, std::max(1, static_cast<int>(7 * editScale)));
    textEdit_->setFont(editorFont);
    textEdit_->setStyleSheet(
        QStringLiteral("QLineEdit { color: %1; background: rgba(0, 0, 0, 140); "
                       "border: 1px solid #888; padding: 0 3px; }")
            .arg(editColor.name()));
    const QPointF local = owner->localFromGlobal(origin);
    const int width = std::min(360, std::max(160, owner->width() - 16));
    // Hug the mirrored glyph height instead of QLineEdit's roomy default
    // frame: the box only needs the text plus a small breathing margin.
    const int height = std::max(20, QFontMetrics(editorFont).height() + 6);
    const int x = std::clamp(static_cast<int>(std::round(local.x())), 4, std::max(4, owner->width() - width - 4));
    const int y = std::clamp(static_cast<int>(std::round(local.y())), 4, std::max(4, owner->height() - height - 4));
    textEdit_->setGeometry(x, y, width, height);
    textEdit_->show();
    textEdit_->raise();
    textEdit_->setFocus(Qt::OtherFocusReason);
    if (!initial.isEmpty()) {
        textEdit_->selectAll();
    }
    updateAll();
}

void OverlayController::finishText(bool accept)
{
    if (textEdit_ == nullptr) {
        return;
    }
    const QString value = textEdit_->text();
    textEdit_->hide();
    textEdit_->deleteLater();
    textEdit_ = nullptr;
    if (accept && !value.isEmpty()) {
        Annotation annotation;
        annotation.kind = Annotation::Kind::Text;
        annotation.tool = QStringLiteral("text");
        annotation.origin = cancelledText_.has_value() ? cancelledText_->origin : textOrigin_;
        annotation.text = value;
        if (cancelledText_.has_value()) {
            // Re-edits keep the label's own style unless the panel restyles
            // it while editing; textEditFont_ tracks a live font change.
            annotation.scale = cancelledText_->scale;
            annotation.color = cancelledText_->color;
            annotation.font = textEditFont_;
        } else {
            annotation.scale = textSize_;
            annotation.color = currentColor_;
            annotation.font = currentFont_;
        }
        QVector<Annotation> next = annotations_;
        if (editingTextIndex_ >= 0) {
            // Re-edit: the pre-edit snapshot was already pushed by beginText,
            // so a single undo restores the original text.
            next.insert(std::min(editingTextIndex_, static_cast<int>(next.size())), annotation);
            textEditSnapshot_ = false;
            redoStack_.clear();
            annotations_ = std::move(next);
            selectedAnnotation_ = std::min(editingTextIndex_, static_cast<int>(annotations_.size()) - 1);
            updateAll();
        } else {
            next.push_back(annotation);
            const int newIndex = next.size() - 1;
            mutateAnnotations(std::move(next));
            selectAnnotation(newIndex);
        }
    } else if (!accept && cancelledText_.has_value()) {
        if (textEditSnapshot_) {
            // Rejected edit: drop the beginText snapshot so undo does not
            // replay a no-op, then restore the previous text exactly.
            undoStack_.removeLast();
            textEditSnapshot_ = false;
        }
        annotations_.insert(std::min(editingTextIndex_, static_cast<int>(annotations_.size())),
                            *cancelledText_);
        updateAll();
    }
    cancelledText_.reset();
    editingTextIndex_ = -1;
    textOutput_ = -1;
}

void OverlayController::showToolbar()
{
    if (!selection_.has_value() || overlays_.isEmpty()) {
        return;
    }
    // The selection may have moved to another output (drag, resize, arrow
    // keys): re-evaluate the owning overlay on every show.
    toolbarOutput_ = outputIndexForSelection();
    if (toolbar_ == nullptr) {
        toolbar_ = new FloatingToolbar(this, overlays_.at(toolbarOutput_));
    } else if (toolbar_->parentWidget() != overlays_.at(toolbarOutput_)) {
        toolbar_->setParent(overlays_.at(toolbarOutput_));
    }
    toolbar_->adjustSize();
    toolbar_->show();
    toolbar_->raise();
    toolbar_->syncState();
    updateToolbarGeometry();
}

// The output that currently hosts the selection (by its center point).
int OverlayController::outputIndexForSelection() const
{
    if (overlays_.isEmpty()) {
        return 0;
    }
    if (selection_.has_value()) {
        const std::int64_t centerX =
            selection_->x + static_cast<std::int64_t>(selection_->width) / 2;
        const std::int64_t centerY =
            selection_->y + static_cast<std::int64_t>(selection_->height) / 2;
        for (CaptureOverlay *overlay : overlays_) {
            const OutputSession &output = overlay->output();
            if (centerX >= output.geometry.x && centerX < output.geometry.right() &&
                centerY >= output.geometry.y && centerY < output.geometry.bottom()) {
                return overlay->outputIndex();
            }
        }
    }
    return toolbarOutput_ >= 0 && toolbarOutput_ < overlays_.size() ? toolbarOutput_ : 0;
}

// Drops a dragged panel on whichever output it was released over.
void OverlayController::settlePanelAtGlobal(QPoint topLeft)
{
    if (toolbar_ == nullptr || overlays_.isEmpty() || !toolbar_->isVisible()) {
        return;
    }
    const QPoint center = topLeft + QPoint(toolbar_->width() / 2, toolbar_->height() / 2);
    CaptureOverlay *target = nullptr;
    for (CaptureOverlay *overlay : overlays_) {
        const QRect bounds(overlay->mapToGlobal(QPoint(0, 0)), overlay->size());
        if (bounds.contains(center)) {
            target = overlay;
            break;
        }
    }
    if (target == nullptr) {
        return;
    }
    if (target != toolbar_->parentWidget()) {
        toolbarOutput_ = target->outputIndex();
        toolbarAnchorValid_ = false;
        toolbar_->setParent(target);
        toolbar_->show();
        toolbar_->raise();
    }
    const QPoint local = target->mapFromGlobal(topLeft);
    const int x = std::clamp(local.x(), 0, std::max(0, target->width() - toolbar_->width()));
    const int y = std::clamp(local.y(), 0, std::max(0, target->height() - toolbar_->height()));
    toolbar_->move(x, y);
}

void OverlayController::hideToolbar()
{
    toolbarAnchorValid_ = false;
    if (toolbar_ != nullptr) {
        toolbar_->hide();
    }
}

void OverlayController::updateToolbarGeometry()
{
    if (toolbar_ == nullptr || !toolbar_->isVisible() || !selection_.has_value() ||
        toolbarOutput_ < 0 || toolbarOutput_ >= overlays_.size()) {
        return;
    }
    if (panelPinned_) {
        // The user moved the panel to a custom spot; keep it there.
        return;
    }
    // Follow the selection across outputs while it is being dragged or resized.
    const int outputIndex = outputIndexForSelection();
    if (outputIndex != toolbarOutput_) {
        toolbarOutput_ = outputIndex;
        toolbarAnchorValid_ = false;
        if (toolbar_->parentWidget() != overlays_.at(toolbarOutput_)) {
            toolbar_->setParent(overlays_.at(toolbarOutput_));
        }
    }
    CaptureOverlay *owner = overlays_.at(toolbarOutput_);
    // Keep the panel attached to the selection: centered above it, flipping
    // below when there is no room, always clamped to the owning output.
    const OutputSession &output = owner->output();
    const QRectF localSelection = localRect(output, *selection_, owner->size());
    const QRect anchorSelection = localSelection.toRect();
    if (toolbarAnchorValid_ && toolbarAnchorSelection_ == anchorSelection) {
        // The selection has not moved: hold the edge next to the selection so
        // a style-row toggle grows the panel away from it without moving the
        // command bar.
        int y = toolbarAnchor_.y();
        if (!toolbarAnchorBelow_) {
            y += toolbarAnchorHeight_ - toolbar_->height();
        }
        const int x = std::clamp(toolbarAnchor_.x(), 0,
                                 std::max(0, owner->width() - toolbar_->width()));
        y = std::clamp(y, 0, std::max(0, owner->height() - toolbar_->height()));
        toolbar_->move(x, y);
        return;
    }
    const int width = toolbar_->width();
    const int height = toolbar_->height();
    const int x = std::clamp(static_cast<int>(std::round(localSelection.center().x() - width / 2.0)),
                             4, std::max(4, owner->width() - width - 4));
    int y = static_cast<int>(std::round(localSelection.top())) - height - 8;
    bool below = false;
    if (y < 4) {
        y = static_cast<int>(std::round(localSelection.bottom())) + 8;
        below = true;
    }
    y = std::clamp(y, 4, std::max(4, owner->height() - height - 4));
    // The style sub-panel pops away from the selection: on top of the command
    // bar when the panel floats above the selection, underneath when the
    // panel had to flip below it.
    toolbar_->setStyleRowAbove(!below);
    toolbarAnchor_ = QPoint(x, y);
    toolbarAnchorSelection_ = anchorSelection;
    toolbarAnchorHeight_ = height;
    toolbarAnchorBelow_ = below;
    toolbarAnchorValid_ = true;
    toolbar_->setGeometry(x, y, width, height);
}

int OverlayController::sceneScale() const
{
    int scale = 1;
    for (const OutputSession &output : session_.outputs) {
        scale = std::max(scale, static_cast<int>(output.scale));
    }
    return scale;
}

void OverlayController::updateAll()
{
    for (CaptureOverlay *overlay : overlays_) {
        overlay->update();
    }
    if (toolbar_ != nullptr && toolbar_->isVisible()) {
        toolbar_->syncState();
        updateToolbarGeometry();
    }
}

void OverlayController::press(CaptureOverlay *overlay, const QPointF &local,
                              Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    Q_UNUSED(modifiers);
    if (finished_ || cancelled_) {
        return;
    }
    if (button == Qt::RightButton) {
        cancel();
        return;
    }
    if (button != Qt::LeftButton) {
        return;
    }
    if (textEdit_ != nullptr) {
        finishText(true);
    }
    const Point point = globalPoint(overlay, local);
    if (tool_ == Tool::Text) {
        beginText(overlay, point);
        return;
    }
    if (tool_ == Tool::Select) {
        // Annotation handles and annotations take precedence over the outer
        // capture selection, so every existing mark remains adjustable.
        const int annotationHandle =
            selectedAnnotation_ >= 0 ? annotationHandleAt(point) : 0;
        const int annotationIndex = annotationHitAt(point);
        if (annotationHandle != 0) {
            beginAnnotationDrag(point, true);
        } else if (annotationIndex >= 0) {
            selectAnnotation(annotationIndex);
            beginAnnotationDrag(point, false);
        } else if (pinEdit_) {
            // The canvas is fixed in pin-edit mode: clicking empty space only
            // clears the current annotation selection.
            selectAnnotation(-1);
        } else {
            const int handle = hitHandle(point);
            if (selection_.has_value() && handle != 0 && handle != 9) {
                gesture_->anchor = point;
                gesture_->current = point;
                gesture_->origin = *selection_;
                gesture_->handle = handle;
                gesture_->type = Gesture::Type::Resizing;
            } else if (selection_.has_value() && handle == 9) {
                gesture_->anchor = point;
                gesture_->current = point;
                gesture_->origin = *selection_;
                gesture_->handle = handle;
                gesture_->type = Gesture::Type::Moving;
            } else {
                startSelection(point);
            }
        }
    } else {
        if (!canDrawAt(point)) {
            return;
        }
        beginDrawing(point);
    }
    updateAll();
}

void OverlayController::move(CaptureOverlay *overlay, const QPointF &local, Qt::MouseButtons buttons,
                             Qt::KeyboardModifiers modifiers)
{
    Q_UNUSED(modifiers);
    if (finished_ || cancelled_) {
        return;
    }
    const Point point = globalPoint(overlay, local);
    pointer_ = point;
    pointerOutput_ = overlay->outputIndex();
    if (buttons != Qt::NoButton) {
        // The shape must match the gesture in progress: move for drags, the
        // handle's resize arrow, cross for drawing.
        switch (gesture_->type) {
        case Gesture::Type::Moving:
        case Gesture::Type::MovingAnnotation:
            overlay->setCursor(Qt::SizeAllCursor);
            break;
        case Gesture::Type::Resizing:
        case Gesture::Type::ResizingAnnotation:
            overlay->setCursor(cursorForHandle(gesture_->handle));
            break;
        default:
            overlay->setCursor(Qt::CrossCursor);
            break;
        }
    } else if (tool_ == Tool::Select && selection_.has_value() && editing_) {
        // Handles map to resize arrows; anywhere else inside the selection
        // (hitHandle returns 9) means the selection itself can be dragged.
        overlay->setCursor(cursorForHandle(hitHandle(point)));
    }
    if (gesture_->type == Gesture::Type::Selecting) {
        updateSelection(point);
    } else if (gesture_->type == Gesture::Type::Moving) {
        selection_ = moveSelection(gesture_->origin, gesture_->anchor, clampPoint(point));
    } else if (gesture_->type == Gesture::Type::Resizing) {
        selection_ = resizeSelection(gesture_->origin, gesture_->handle, clampPoint(point));
    } else if (gesture_->type == Gesture::Type::MovingAnnotation ||
               gesture_->type == Gesture::Type::ResizingAnnotation) {
        updateAnnotationDrag(point);
    } else if (gesture_->type == Gesture::Type::Drawing) {
        updateDrawing(point);
    } else {
        return;
    }
    updateAll();
}

void OverlayController::release(CaptureOverlay *overlay, const QPointF &local,
                                Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    Q_UNUSED(modifiers);
    if (finished_ || cancelled_ || button != Qt::LeftButton) {
        return;
    }
    const Point point = globalPoint(overlay, local);
    switch (gesture_->type) {
    case Gesture::Type::Selecting:
        toolbarOutput_ = overlay->outputIndex();
        finishSelection(point);
        break;
    case Gesture::Type::Moving:
        selection_ = moveSelection(gesture_->origin, gesture_->anchor, clampPoint(point));
        gesture_->type = Gesture::Type::None;
        showToolbar();
        break;
    case Gesture::Type::Resizing:
        selection_ = resizeSelection(gesture_->origin, gesture_->handle, clampPoint(point));
        gesture_->type = Gesture::Type::None;
        showToolbar();
        break;
    case Gesture::Type::MovingAnnotation:
    case Gesture::Type::ResizingAnnotation:
        finishAnnotationDrag(overlay, point);
        break;
    case Gesture::Type::Drawing:
        finishDrawing(point);
        break;
    case Gesture::Type::None:
        break;
    }
    updateAll();
}

void OverlayController::doubleClick(CaptureOverlay *overlay, const QPointF &local,
                                     Qt::MouseButton button)
{
    if (button != Qt::LeftButton) {
        return;
    }
    const Point point = globalPoint(overlay, local);
    const int index = annotationHitAt(point);
    if (index >= 0 && annotations_.at(index).kind == Annotation::Kind::Text) {
        startTextEditor(overlay, index, annotations_.at(index).origin);
        return;
    }
    if (selection_.has_value() && hitHandle(point) != 0) {
        confirm();
    }
}

void OverlayController::key(CaptureOverlay *overlay, int key, Qt::KeyboardModifiers modifiers)
{
    Q_UNUSED(overlay);
    if (key == Qt::Key_Escape) {
        if (textEdit_ != nullptr) {
            finishText(false);
        } else {
            cancel();
        }
        return;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        if (textEdit_ != nullptr) {
            finishText(true);
        } else {
            confirm();
        }
        return;
    }
    if (textEdit_ != nullptr) {
        return;
    }
    if (modifiers & Qt::ControlModifier) {
        if (key == Qt::Key_Z && !(modifiers & Qt::ShiftModifier)) {
            undo();
        } else if (key == Qt::Key_Z || key == Qt::Key_Y) {
            redo();
        }
        return;
    }
    if ((key == Qt::Key_Delete || key == Qt::Key_Backspace) &&
        gesture_->type == Gesture::Type::None && selectedAnnotation_ >= 0) {
        deleteSelectedAnnotation();
        return;
    }
    if (!selection_.has_value() || !editing_ || gesture_->type != Gesture::Type::None) {
        return;
    }
    const int step = (modifiers & Qt::ShiftModifier) ? 10 : 1;
    int dx = 0;
    int dy = 0;
    switch (key) {
    case Qt::Key_Left:
        dx = -step;
        break;
    case Qt::Key_Right:
        dx = step;
        break;
    case Qt::Key_Up:
        dy = -step;
        break;
    case Qt::Key_Down:
        dy = step;
        break;
    default:
        return;
    }
    const Point anchor{selection_->x, selection_->y};
    const Point current{selection_->x + dx, selection_->y + dy};
    selection_ = moveSelection(*selection_, anchor, clampPoint(current));
    updateAll();
}

void OverlayController::chooseTool(Tool tool)
{
    if (finished_ || cancelled_) {
        return;
    }
    if (textEdit_ != nullptr) {
        finishText(true);
    }
    tool_ = tool;
    // Keep the annotation selection when moving to Select so a freshly drawn
    // annotation can be adjusted right away; drawing tools start fresh.
    if (tool != Tool::Select) {
        selectedAnnotation_ = -1;
    }
    for (CaptureOverlay *overlay : overlays_) {
        overlay->setCursor(Qt::CrossCursor);
    }
    updateAll();
}

void OverlayController::setCurrentColor(const QColor &color)
{
    if (finished_ || cancelled_ || !color.isValid()) {
        return;
    }
    currentColor_ = color;
    applyStyleToSelected([&color](Annotation &annotation) { annotation.color = color; });
    updateAll();
}

void OverlayController::setCurrentFont(const QString &family)
{
    if (finished_ || cancelled_) {
        return;
    }
    currentFont_ = family.trimmed();
    // Restyle an open editor live so the chosen face shows while typing and
    // is what finishText commits.
    if (textEdit_ != nullptr) {
        textEditFont_ = currentFont_;
        const std::uint32_t editScale =
            cancelledText_.has_value() ? cancelledText_->scale : textSize_;
        QFont editorFont = textFont(textEditFont_, std::max(1, static_cast<int>(7 * editScale)));
        textEdit_->setFont(editorFont);
    }
    applyStyleToSelected([this](Annotation &annotation) {
        if (annotation.kind == Annotation::Kind::Text) {
            annotation.font = currentFont_;
        }
    });
    updateAll();
}
void OverlayController::setWidth(std::uint32_t width)
{
    if (finished_ || cancelled_) {
        return;
    }
    currentWidth_ = std::clamp(width, 1u, 64u);
    applyStyleToSelected([this](Annotation &annotation) {
        if (annotation.kind != Annotation::Kind::Text) {
            annotation.width = currentWidth_;
        }
    });
    updateAll();
}

void OverlayController::setDash(const QString &dash)
{
    if (finished_ || cancelled_) {
        return;
    }
    currentDash_ = dash == QStringLiteral("dashed") || dash == QStringLiteral("dotted")
        ? dash
        : QStringLiteral("solid");
    applyStyleToSelected([this](Annotation &annotation) {
        if (annotation.kind != Annotation::Kind::Text) {
            annotation.dash = currentDash_;
        }
    });
    updateAll();
}

void OverlayController::setArrowSize(std::uint32_t size)
{
    if (finished_ || cancelled_) {
        return;
    }
    arrowSize_ = std::clamp(size, 1u, 8u);
    applyStyleToSelected([this](Annotation &annotation) {
        if (annotation.tool == QStringLiteral("arrow")) {
            annotation.size = arrowSize_;
        }
    });
    updateAll();
}

void OverlayController::setArrowStyle(const QString &style)
{
    if (finished_ || cancelled_) {
        return;
    }
    currentArrowStyle_ = style == QStringLiteral("filled") ? QStringLiteral("filled")
                                                             : QStringLiteral("open");
    applyStyleToSelected([this](Annotation &annotation) {
        if (annotation.tool == QStringLiteral("arrow")) {
            annotation.arrowStyle = currentArrowStyle_;
        }
    });
    updateAll();
}

void OverlayController::setTextSize(std::uint32_t size)
{
    if (finished_ || cancelled_) {
        return;
    }
    textSize_ = std::clamp(size, 1u, 64u);
    applyStyleToSelected([this](Annotation &annotation) {
        if (annotation.kind == Annotation::Kind::Text) {
            annotation.scale = textSize_;
        }
    });
    updateAll();
}

void OverlayController::setMosaicShape(const QString &shape)
{
    if (finished_ || cancelled_) {
        return;
    }
    const QString requested = shape == QStringLiteral("ellipse") || shape == QStringLiteral("brush")
        ? shape
        : QStringLiteral("rect");
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size()) {
        const Annotation &selected = annotations_.at(selectedAnnotation_);
        if (selected.tool == QStringLiteral("mosaic")) {
            // Area mosaics and brush mosaics have different wire payloads; do
            // not create a Shape carrying the unsupported mask="brush" value.
            mosaicShape_ = selected.kind == Annotation::Kind::Stroke
                ? QStringLiteral("brush")
                : (requested == QStringLiteral("brush") ? selected.mask : requested);
        } else {
            mosaicShape_ = requested;
        }
    } else {
        mosaicShape_ = requested;
    }
    applyStyleToSelected([this](Annotation &annotation) {
        if (annotation.tool != QStringLiteral("mosaic")) {
            return;
        }
        if (annotation.kind == Annotation::Kind::Shape && mosaicShape_ != QStringLiteral("brush")) {
            annotation.mask = mosaicShape_;
        }
    });
    updateAll();
}

void OverlayController::setMosaicStrength(std::uint32_t strength)
{
    if (finished_ || cancelled_) {
        return;
    }
    mosaicStrength_ = std::clamp(strength, 1u, 3u);
    applyStyleToSelected([this](Annotation &annotation) {
        if (annotation.tool == QStringLiteral("mosaic")) {
            annotation.strength = mosaicStrength_;
        }
    });
    updateAll();
}

void OverlayController::notifyPanelDragged()
{
    panelPinned_ = true;
}

void OverlayController::beginStyleAdjustment()
{
    if (styleAdjustmentActive_ || selectedAnnotation_ < 0 ||
        selectedAnnotation_ >= annotations_.size()) {
        return;
    }
    styleAdjustmentActive_ = true;
    styleAdjustmentChanged_ = false;
    styleAdjustmentSnapshot_ = annotations_;
}

void OverlayController::endStyleAdjustment()
{
    if (!styleAdjustmentActive_) {
        return;
    }
    styleAdjustmentActive_ = false;
    if (!styleAdjustmentChanged_) {
        styleAdjustmentSnapshot_.clear();
        return;
    }
    // The live annotation was updated without history during the drag; commit
    // the original snapshot as one undo step when the slider is released.
    undoStack_.push_back(std::move(styleAdjustmentSnapshot_));
    if (undoStack_.size() > kMaxUndoSteps) {
        undoStack_.removeFirst();
    }
    redoStack_.clear();
    styleAdjustmentSnapshot_.clear();
    updateAll();
}

QString OverlayController::styleTargetTool() const
{
    if (selectedAnnotation_ >= 0 && selectedAnnotation_ < annotations_.size()) {
        const Annotation &annotation = annotations_.at(selectedAnnotation_);
        if (annotation.kind == Annotation::Kind::Text) {
            return QStringLiteral("text");
        }
        return annotation.tool;
    }
    return toolName(tool_);
}

void OverlayController::applyStyleToSelected(
    const std::function<void(Annotation &)> &mutate)
{
    if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size()) {
        return;
    }
    QVector<Annotation> next = annotations_;
    mutate(next[selectedAnnotation_]);
    if (annotationEquals(next.at(selectedAnnotation_),
                         annotations_.at(selectedAnnotation_))) {
        return;
    }
    if (styleAdjustmentActive_) {
        annotations_ = std::move(next);
        styleAdjustmentChanged_ = true;
        updateAll();
        return;
    }
    mutateAnnotations(std::move(next));
}

int OverlayController::annotationHitAt(Point point) const
{
    const auto distanceToSegment = [](const Point &value, const Point &first,
                                      const Point &second) {
        const double vx = static_cast<double>(second.x - first.x);
        const double vy = static_cast<double>(second.y - first.y);
        const double wx = static_cast<double>(value.x - first.x);
        const double wy = static_cast<double>(value.y - first.y);
        const double lengthSquared = vx * vx + vy * vy;
        const double projection = lengthSquared > 0.0
            ? std::clamp((wx * vx + wy * vy) / lengthSquared, 0.0, 1.0)
            : 0.0;
        const double dx = static_cast<double>(value.x - first.x) - projection * vx;
        const double dy = static_cast<double>(value.y - first.y) - projection * vy;
        return std::hypot(dx, dy);
    };
    for (int index = annotations_.size() - 1; index >= 0; --index) {
        const Annotation &annotation = annotations_.at(index);
        LogicalRect bounds;
        if (!annotationBounds(annotation, &bounds)) {
            continue;
        }
        const double tolerance = 4.0 +
            (annotation.kind == Annotation::Kind::Text ? 0.0 : annotation.width / 2.0);
        if (annotation.kind == Annotation::Kind::Shape) {
            if (annotation.tool != QStringLiteral("ellipse")) {
                if (point.x >= bounds.x - tolerance && point.x < bounds.right() + tolerance &&
                    point.y >= bounds.y - tolerance && point.y < bounds.bottom() + tolerance) {
                    return index;
                }
            } else {
                const double cx = bounds.x + bounds.width / 2.0;
                const double cy = bounds.y + bounds.height / 2.0;
                const double rx = std::max(1.0, bounds.width / 2.0 + tolerance);
                const double ry = std::max(1.0, bounds.height / 2.0 + tolerance);
                const double dx = (point.x - cx) / rx;
                const double dy = (point.y - cy) / ry;
                if (dx * dx + dy * dy <= 1.0) {
                    return index;
                }
            }
            continue;
        }
        if (annotation.kind == Annotation::Kind::Text) {
            if (point.x >= bounds.x - tolerance && point.x < bounds.right() + tolerance &&
                point.y >= bounds.y - tolerance && point.y < bounds.bottom() + tolerance) {
                return index;
            }
            continue;
        }
        if (annotation.points.isEmpty()) {
            continue;
        }
        const double radius = annotation.tool == QStringLiteral("mosaic")
            ? std::max(1.0, annotation.width / 2.0)
            : std::max(1.0, annotation.width / 2.0);
        for (int segment = 1; segment < annotation.points.size(); ++segment) {
            if (distanceToSegment(point, annotation.points.at(segment - 1),
                                  annotation.points.at(segment)) <= radius + 4.0) {
                return index;
            }
        }
        if (annotation.points.size() == 1 &&
            distanceToSegment(point, annotation.points.constFirst(),
                              annotation.points.constFirst()) <= radius + 4.0) {
            return index;
        }
        if (annotation.tool == QStringLiteral("arrow") && annotation.points.size() >= 2) {
            const Point &start = annotation.points.at(annotation.points.size() - 2);
            const Point &end = annotation.points.constLast();
            const double dx = static_cast<double>(end.x - start.x);
            const double dy = static_cast<double>(end.y - start.y);
            const double length = std::hypot(dx, dy);
            if (length > 0.0) {
                const double head = std::min(
                    std::max(6.0, static_cast<double>(annotation.width) * 4.0) * annotation.size,
                    length);
                const double wing = std::max(head * 0.55, static_cast<double>(annotation.width));
                const Point base{
                    static_cast<std::int32_t>(std::lround(end.x - dx / length * head)),
                    static_cast<std::int32_t>(std::lround(end.y - dy / length * head)),
                };
                const Point left{
                    static_cast<std::int32_t>(std::lround(base.x - dy / length * wing)),
                    static_cast<std::int32_t>(std::lround(base.y + dx / length * wing)),
                };
                const Point right{
                    static_cast<std::int32_t>(std::lround(base.x + dy / length * wing)),
                    static_cast<std::int32_t>(std::lround(base.y - dx / length * wing)),
                };
                if (distanceToSegment(point, end, left) <= radius + 4.0 ||
                    distanceToSegment(point, end, right) <= radius + 4.0) {
                    return index;
                }
            }
        }
    }
    return -1;
}

int OverlayController::annotationHandleAt(Point point) const
{
    if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size()) {
        return 0;
    }
    const Annotation &annotation = annotations_.at(selectedAnnotation_);
    if (annotation.kind == Annotation::Kind::Text) {
        return 0; // text annotations move but never resize
    }
    LogicalRect bounds;
    if (!annotationBounds(annotation, &bounds)) {
        return 0;
    }
    const std::int64_t left = bounds.x;
    const std::int64_t top = bounds.y;
    const std::int64_t rightEdge = bounds.right() - 1;
    const std::int64_t bottomEdge = bounds.bottom() - 1;
    const auto close = [](std::int64_t first, std::int64_t second) {
        return std::abs(first - second) <= kHandleRadius;
    };
    const bool nearLeft = close(point.x, left);
    const bool nearRight = close(point.x, rightEdge);
    const bool nearTop = close(point.y, top);
    const bool nearBottom = close(point.y, bottomEdge);
    if (nearLeft && nearTop) {
        return 1;
    }
    if (nearTop && close(point.x, (left + rightEdge) / 2)) {
        return 2;
    }
    if (nearRight && nearTop) {
        return 3;
    }
    if (nearRight && close(point.y, (top + bottomEdge) / 2)) {
        return 4;
    }
    if (nearRight && nearBottom) {
        return 5;
    }
    if (nearBottom && close(point.x, (left + rightEdge) / 2)) {
        return 6;
    }
    if (nearLeft && nearBottom) {
        return 7;
    }
    if (nearLeft && close(point.y, (top + bottomEdge) / 2)) {
        return 8;
    }
    return 0;
}

void OverlayController::selectAnnotation(int index)
{
    selectedAnnotation_ = index >= 0 && index < annotations_.size() ? index : -1;
    updateAll();
}

void OverlayController::deleteSelectedAnnotation()
{
    if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size()) {
        return;
    }
    QVector<Annotation> next = annotations_;
    next.remove(selectedAnnotation_);
    selectedAnnotation_ = -1;
    mutateAnnotations(std::move(next));
}

void OverlayController::beginAnnotationDrag(Point point, bool resize)
{
    if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size()) {
        return;
    }
    gesture_->type = resize ? Gesture::Type::ResizingAnnotation
                            : Gesture::Type::MovingAnnotation;
    gesture_->anchor = point;
    gesture_->current = point;
    gesture_->handle = resize ? annotationHandleAt(point) : 0;
    dragAnnotation_ = annotations_.at(selectedAnnotation_);
    dragSnapshot_ = annotations_;
    dragMoved_ = false;
}

void OverlayController::updateAnnotationDrag(Point point)
{
    if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size()) {
        gesture_->type = Gesture::Type::None;
        return;
    }
    const Point current = clampPoint(point);
    gesture_->current = current;
    if (!dragMoved_) {
        const std::int64_t dx = static_cast<std::int64_t>(current.x) - gesture_->anchor.x;
        const std::int64_t dy = static_cast<std::int64_t>(current.y) - gesture_->anchor.y;
        if (dx * dx + dy * dy <= 16) {
            return; // stay below the drag threshold: a plain click selects the mark
        }
        dragMoved_ = true;
    }
    if (gesture_->type == Gesture::Type::MovingAnnotation) {
        const int dx = current.x - gesture_->anchor.x;
        const int dy = current.y - gesture_->anchor.y;
        annotations_[selectedAnnotation_] = translatedAnnotation(dragAnnotation_, dx, dy);
    } else {
        LogicalRect originalBounds;
        if (!annotationBounds(dragAnnotation_, &originalBounds)) {
            return;
        }
        const LogicalRect newBounds = resizeSelection(originalBounds, gesture_->handle, current);
        annotations_[selectedAnnotation_] = scaledAnnotation(dragAnnotation_, newBounds);
    }
}

void OverlayController::finishAnnotationDrag(CaptureOverlay *overlay, Point point)
{
    Q_UNUSED(overlay);
    updateAnnotationDrag(point);
    const bool moved = dragMoved_;
    gesture_->type = Gesture::Type::None;
    if (!moved) {
        // A plain click selects every annotation.  Text editing is explicitly a
        // double-click action so a selected label can still receive style edits.
        return;
    }
    undoStack_.push_back(dragSnapshot_);
    if (undoStack_.size() > kMaxUndoSteps) {
        undoStack_.removeFirst();
    }
    redoStack_.clear();
    updateAll();
}

Annotation OverlayController::translatedAnnotation(const Annotation &original, int dx,
                                                    int dy) const
{
    int clampedDx = dx;
    int clampedDy = dy;
    LogicalRect bounds;
    if (annotationBounds(original, &bounds)) {
        const std::int64_t minDx = session_.bounds.x - bounds.x;
        const std::int64_t maxDx = session_.bounds.right() - bounds.right();
        const std::int64_t minDy = session_.bounds.y - bounds.y;
        const std::int64_t maxDy = session_.bounds.bottom() - bounds.bottom();
        clampedDx = static_cast<int>(std::clamp<std::int64_t>(dx, minDx, std::max(minDx, maxDx)));
        clampedDy = static_cast<int>(std::clamp<std::int64_t>(dy, minDy, std::max(minDy, maxDy)));
    }
    Annotation result = original;
    switch (original.kind) {
    case Annotation::Kind::Shape:
        result.rect.x = static_cast<std::int32_t>(result.rect.x + clampedDx);
        result.rect.y = static_cast<std::int32_t>(result.rect.y + clampedDy);
        break;
    case Annotation::Kind::Stroke:
        for (Point &point : result.points) {
            point.x = static_cast<std::int32_t>(point.x + clampedDx);
            point.y = static_cast<std::int32_t>(point.y + clampedDy);
        }
        break;
    case Annotation::Kind::Text:
        result.origin.x = static_cast<std::int32_t>(result.origin.x + clampedDx);
        result.origin.y = static_cast<std::int32_t>(result.origin.y + clampedDy);
        break;
    }
    return result;
}

Annotation OverlayController::scaledAnnotation(const Annotation &original,
                                               const LogicalRect &newBounds) const
{
    Annotation result = original;
    if (original.kind == Annotation::Kind::Shape) {
        result.rect = newBounds;
        return result;
    }
    LogicalRect oldBounds;
    if (!annotationBounds(original, &oldBounds) || oldBounds.width == 0 ||
        oldBounds.height == 0) {
        return result;
    }
    const double spanX = oldBounds.width > 1 ?
        static_cast<double>(newBounds.width - 1) / static_cast<double>(oldBounds.width - 1) : 0.0;
    const double spanY = oldBounds.height > 1 ?
        static_cast<double>(newBounds.height - 1) / static_cast<double>(oldBounds.height - 1) : 0.0;
    for (Point &point : result.points) {
        point.x = static_cast<std::int32_t>(std::lround(
            newBounds.x + (static_cast<double>(point.x) - oldBounds.x) * spanX));
        point.y = static_cast<std::int32_t>(std::lround(
            newBounds.y + (static_cast<double>(point.y) - oldBounds.y) * spanY));
    }
    return result;
}

bool OverlayController::hasValidSelection() const
{
    return selection_.has_value() && selection_->width >= kMinimumSelection &&
           selection_->height >= kMinimumSelection;
}

void OverlayController::beginPinEdit()
{
    if (!pinEdit_ || finished_ || cancelled_) {
        return;
    }
    // The canvas is the whole pin image: preselect everything and jump
    // straight into the annotation editing state.
    selection_ = LogicalRect{session_.bounds.x, session_.bounds.y, session_.bounds.width,
                             session_.bounds.height};
    editing_ = true;
    toolbarOutput_ = 0;
    showToolbar();
}

void OverlayController::confirm()
{
    if (finished_ || cancelled_) {
        return;
    }
    if (textEdit_ != nullptr) {
        finishText(true);
    }
    if (!hasValidSelection()) {
        return;
    }
    terminal(false);
}

void OverlayController::cancel()
{
    if (finished_ || cancelled_) {
        return;
    }
    if (textEdit_ != nullptr) {
        finishText(false);
    }
    terminal(true);
}

void OverlayController::terminal(bool cancelled)
{
    if (finished_ || cancelled_) {
        return;
    }
    finished_ = true;
    cancelled_ = cancelled;
    hideToolbar();
    removeTextEditor();
    if (terminalCallback_) {
        terminalCallback_();
    }
}

void OverlayController::removeTextEditor()
{
    if (textEdit_ != nullptr) {
        textEdit_->hide();
        textEdit_->deleteLater();
        textEdit_ = nullptr;
    }
}

bool OverlayController::isFinished() const
{
    return finished_;
}

bool OverlayController::isCancelled() const
{
    return cancelled_;
}

const std::optional<LogicalRect> &OverlayController::selection() const
{
    return selection_;
}

const QVector<Annotation> &OverlayController::annotations() const
{
    return annotations_;
}

QJsonDocument OverlayController::resultDocument(const QString &bitmapDirectory,
                                                QString *error) const
{
    QJsonObject root;
    if (cancelled_) {
        root.insert(QStringLiteral("status"), QStringLiteral("cancelled"));
        return QJsonDocument(root);
    }
    root.insert(QStringLiteral("status"), QStringLiteral("ok"));
    QJsonObject selection;
    if (selection_.has_value()) {
        selection.insert(QStringLiteral("x"), static_cast<qint64>(selection_->x));
        selection.insert(QStringLiteral("y"), static_cast<qint64>(selection_->y));
        selection.insert(QStringLiteral("width"), static_cast<qint64>(selection_->width));
        selection.insert(QStringLiteral("height"), static_cast<qint64>(selection_->height));
    }
    root.insert(QStringLiteral("selection"), selection);

    QJsonArray outputAnnotations;
    for (const Annotation &annotation : annotations_) {
        QJsonObject value;
        if (annotation.kind == Annotation::Kind::Shape) {
            value.insert(QStringLiteral("kind"), QStringLiteral("shape"));
            value.insert(QStringLiteral("tool"), annotation.tool);
            value.insert(QStringLiteral("color"), annotation.color.name(QColor::HexRgb));
            value.insert(QStringLiteral("width"), static_cast<qint64>(annotation.width));
            value.insert(QStringLiteral("dash"), annotation.dash);
            if (annotation.tool == QStringLiteral("mosaic")) {
                value.insert(QStringLiteral("mask"), annotation.mask);
                value.insert(QStringLiteral("strength"),
                             static_cast<qint64>(annotation.strength));
            }
            QJsonObject rect;
            rect.insert(QStringLiteral("x"), static_cast<qint64>(annotation.rect.x));
            rect.insert(QStringLiteral("y"), static_cast<qint64>(annotation.rect.y));
            rect.insert(QStringLiteral("width"), static_cast<qint64>(annotation.rect.width));
            rect.insert(QStringLiteral("height"), static_cast<qint64>(annotation.rect.height));
            value.insert(QStringLiteral("rect"), rect);
        } else if (annotation.kind == Annotation::Kind::Stroke) {
            value.insert(QStringLiteral("kind"), QStringLiteral("stroke"));
            value.insert(QStringLiteral("tool"), annotation.tool);
            value.insert(QStringLiteral("color"), annotation.color.name(QColor::HexRgb));
            value.insert(QStringLiteral("width"), static_cast<qint64>(annotation.width));
            value.insert(QStringLiteral("dash"), annotation.dash);
            if (annotation.tool == QStringLiteral("arrow")) {
                value.insert(QStringLiteral("size"), static_cast<qint64>(annotation.size));
                value.insert(QStringLiteral("arrow_style"), annotation.arrowStyle);
            }
            if (annotation.tool == QStringLiteral("mosaic")) {
                value.insert(QStringLiteral("strength"),
                             static_cast<qint64>(annotation.strength));
            }
            QJsonArray points;
            for (const Point &point : annotation.points) {
                QJsonObject item;
                item.insert(QStringLiteral("x"), static_cast<qint64>(point.x));
                item.insert(QStringLiteral("y"), static_cast<qint64>(point.y));
                points.push_back(item);
            }
            value.insert(QStringLiteral("points"), points);
        } else {
            value.insert(QStringLiteral("kind"), QStringLiteral("text"));
            QJsonObject origin;
            origin.insert(QStringLiteral("x"), static_cast<qint64>(annotation.origin.x));
            origin.insert(QStringLiteral("y"), static_cast<qint64>(annotation.origin.y));
            value.insert(QStringLiteral("origin"), origin);
            value.insert(QStringLiteral("text"), annotation.text);
            value.insert(QStringLiteral("scale"), static_cast<qint64>(annotation.scale));
            value.insert(QStringLiteral("color"), annotation.color.name(QColor::HexRgb));
            if (!annotation.font.isEmpty()) {
                value.insert(QStringLiteral("font"), annotation.font);
            }
            // Rasterize the label with the exact preview font so the final PNG
            // matches what the user saw. The bitmap is rendered in scene device
            // pixels (the highest output scale), matching how Rust composites
            // the label onto the cropped frame.
            if (!bitmapDirectory.isEmpty()) {
                const int scale = sceneScale();
                const QSize logical = textMetrics(annotation);
                const int width = logical.width() * scale;
                const int height = logical.height() * scale;
                if (width > 0 && height > 0 &&
                    static_cast<qint64>(width) * static_cast<qint64>(height) <=
                        16LL * 1024 * 1024) {
                    QImage bitmap(width, height, QImage::Format_RGBA8888);
                    if (!bitmap.isNull()) {
                        bitmap.fill(Qt::transparent);
                        QPainter bitmapPainter(&bitmap);
                        bitmapPainter.setRenderHint(QPainter::Antialiasing, true);
                        bitmapPainter.setRenderHint(QPainter::TextAntialiasing, true);
                        QFont font = textFont(annotation.font,
                                              std::max(1, 7 * static_cast<int>(annotation.scale) * scale));
                        bitmapPainter.setFont(font);
                        bitmapPainter.setPen(annotation.color);
                        const QStringList lines = annotation.text.split(QLatin1Char('\n'));
                        const QFontMetrics metrics(font);
                        qreal y = 0.0;
                        for (const QString &line : lines) {
                            bitmapPainter.drawText(QRectF(0, y, width, metrics.height()),
                                                   Qt::AlignLeft | Qt::AlignTop, line);
                            y += metrics.lineSpacing();
                        }
                        bitmapPainter.end();
                        const QString path = QStringLiteral("%1/text-%2.rgba")
                                                 .arg(bitmapDirectory)
                                                 .arg(textBitmapIndex_++);
                        QFile file(path);
                        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                            file.write(reinterpret_cast<const char *>(bitmap.constBits()),
                                       static_cast<qint64>(bitmap.sizeInBytes())) ==
                                static_cast<qint64>(bitmap.sizeInBytes())) {
                            value.insert(QStringLiteral("bitmap_width"),
                                         static_cast<qint64>(width));
                            value.insert(QStringLiteral("bitmap_height"),
                                         static_cast<qint64>(height));
                            value.insert(QStringLiteral("bitmap"), path);
                        } else if (error != nullptr) {
                            *error = QStringLiteral("cannot write text bitmap `%1`: %2")
                                         .arg(path, file.errorString());
                            return QJsonDocument();
                        }
                    } else if (error != nullptr) {
                        *error = QStringLiteral("cannot allocate %1x%2 text bitmap")
                                     .arg(width)
                                     .arg(height);
                        return QJsonDocument();
                    }
                }
            }
        }
        outputAnnotations.push_back(value);
    }
    root.insert(QStringLiteral("annotations"), outputAnnotations);
    return QJsonDocument(root);
}

void OverlayController::setTerminalCallback(std::function<void()> callback)
{
    terminalCallback_ = std::move(callback);
}

void OverlayController::paint(CaptureOverlay *overlay, QPainter *painter)
{
    const OutputSession &output = overlay->output();
    const QRectF target(0, 0, overlay->width(), overlay->height());
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter->drawImage(target, output.image);
    // The dim-out only makes sense around a selectable region: pin editing
    // shows the image unshaded.
    if (!pinEdit_) {
        painter->fillRect(target, QColor(0, 0, 0, 80));
    }

    if (selection_.has_value()) {
        LogicalRect visible;
        if (intersection(*selection_, output.geometry, &visible)) {
            painter->drawImage(localRect(output, visible, overlay->size()), output.image,
                               sourceRect(output, visible));
        }
    }

    painter->setRenderHint(QPainter::Antialiasing, true);
    const QRectF outputBounds = target;
    painter->setClipRect(outputBounds);
    painter->setBrush(Qt::NoBrush);
    auto drawAnnotation = [this, &output, overlay, painter](const Annotation &annotation) {
        const double scale = output.scale > 0 ? static_cast<double>(output.scale) : 1.0;
        const QPen annotationPen = penForAnnotation(annotation);
        if (annotation.kind == Annotation::Kind::Shape) {
            if (annotation.tool == QStringLiteral("mosaic")) {
                drawMosaicAnnotation(painter, output, annotation.rect, annotation.mask,
                                     annotation.strength, overlay->size());
                return;
            }
            painter->setPen(annotationPen);
            painter->setBrush(Qt::NoBrush);
            const QRectF rect = localRect(output, annotation.rect, overlay->size());
            if (annotation.tool == QStringLiteral("ellipse")) {
                painter->drawEllipse(rect);
            } else if (annotation.dash != QStringLiteral("solid")) {
                // Match the final renderer's band-centerline dash walk.
                painter->drawPolyline(insetRectPolygon(rect, annotation.width));
            } else {
                painter->drawRect(rect);
            }
            return;
        }
        if (annotation.kind == Annotation::Kind::Text) {
            LogicalRect bounds;
            if (!annotationBounds(annotation, &bounds)) {
                return;
            }
            QFont font = annotationFont(annotation);
            painter->setFont(font);
            painter->setPen(annotation.color);
            // Top-left anchored inside the measured bounds so the preview
            // matches the Rust glyph origin and the re-edit hit test.
            painter->drawText(localRect(output, bounds, overlay->size()),
                              Qt::AlignLeft | Qt::AlignTop, annotation.text);
            return;
        }
        if (annotation.points.isEmpty()) {
            return;
        }
        if (annotation.tool == QStringLiteral("mosaic")) {
            // Freehand mosaic brush: smear discs along the path.
            drawMosaicBrush(painter, output, annotation.points, annotation.width,
                            annotation.strength, overlay->size());
            return;
        }
        QPolygonF polygon;
        for (const Point &point : annotation.points) {
            polygon.push_back(localPoint(output, point, overlay->size()));
        }
        painter->setPen(annotationPen);
        painter->drawPolyline(polygon);
        if (annotation.tool == QStringLiteral("arrow") && polygon.size() >= 2) {
            // The head is always solid and scales with the annotation's size.
            painter->setPen(QPen(annotation.color, static_cast<double>(annotation.width),
                                 Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPointF end = polygon.constLast();
            const QPointF start = polygon.at(polygon.size() - 2);
            const QLineF line(start, end);
            if (line.length() > 1.0) {
                const double widthDevice = static_cast<double>(annotation.width) * scale;
                const double headDevice = std::min(std::max(6.0, widthDevice * 4.0) *
                                                       static_cast<double>(annotation.size),
                                                   line.length() * scale);
                const double wingDevice = std::max(headDevice * 0.55, widthDevice);
                const double wing = wingDevice / scale;
                const double unitX = (end.x() - start.x()) / line.length();
                const double unitY = (end.y() - start.y()) / line.length();
                const double baseX = end.x() - unitX * headDevice / scale;
                const double baseY = end.y() - unitY * headDevice / scale;
                const QPointF first(baseX - unitY * wing, baseY + unitX * wing);
                const QPointF second(baseX + unitY * wing, baseY - unitX * wing);
                if (annotation.arrowStyle == QStringLiteral("filled")) {
                    QPolygonF head;
                    head << end << first << second;
                    painter->setBrush(annotation.color);
                    painter->drawPolygon(head);
                }
                painter->drawLine(end, first);
                painter->drawLine(end, second);
            }
        }
    };

    for (const Annotation &annotation : annotations_) {
        drawAnnotation(annotation);
    }
    if (gesture_->type == Gesture::Type::Drawing && !gesture_->points.isEmpty()) {
        Annotation preview;
        preview.tool = toolName(tool_);
        preview.color = currentColor_;
        preview.width = currentWidth_;
        preview.dash = currentDash_;
        preview.size = arrowSize_;
        preview.arrowStyle = currentArrowStyle_;
        preview.mask = mosaicShape_;
        preview.strength = mosaicStrength_;
        if (tool_ == Tool::Rectangle || tool_ == Tool::Ellipse ||
            (tool_ == Tool::Mosaic && mosaicShape_ != QStringLiteral("brush"))) {
            preview.kind = Annotation::Kind::Shape;
            preview.rect = selectionBetween(gesture_->points.constFirst(),
                                            gesture_->points.constLast());
            if (tool_ == Tool::Mosaic) {
                preview.tool = QStringLiteral("mosaic");
            }
        } else {
            preview.kind = Annotation::Kind::Stroke;
            preview.points = gesture_->points;
            if (tool_ == Tool::Mosaic) {
                preview.tool = QStringLiteral("mosaic");
            }
        }
        drawAnnotation(preview);
    }

    // Highlight the selected annotation with handles while the Select tool is
    // manipulating it.
    if (tool_ == Tool::Select && selectedAnnotation_ >= 0 &&
        selectedAnnotation_ < annotations_.size() &&
        (gesture_->type == Gesture::Type::None ||
         gesture_->type == Gesture::Type::MovingAnnotation ||
         gesture_->type == Gesture::Type::ResizingAnnotation)) {
        const Annotation &annotation = annotations_.at(selectedAnnotation_);
        LogicalRect bounds;
        if (annotationBounds(annotation, &bounds)) {
            const QRectF local = localRect(output, bounds, overlay->size());
            painter->setPen(QPen(Qt::white, 1.0, Qt::DashLine));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(local);
            if (annotation.kind != Annotation::Kind::Text) {
                painter->setBrush(Qt::white);
                painter->setPen(QPen(Qt::black, 1.0));
                const QPointF midX((local.left() + local.right()) / 2.0, 0);
                const QPointF midY(0, (local.top() + local.bottom()) / 2.0);
                const QPointF corners[] = {
                    local.topLeft(), {midX.x(), local.top()}, local.topRight(),
                    {local.right(), midY.y()}, local.bottomRight(),
                    {midX.x(), local.bottom()}, local.bottomLeft(),
                    {local.left(), midY.y()},
                };
                for (const QPointF &corner : corners) {
                    painter->drawRect(QRectF(corner.x() - 4, corner.y() - 4, 8, 8));
                }
            }
        }
    }

    if (selection_.has_value()) {
        LogicalRect visible;
        if (intersection(*selection_, output.geometry, &visible)) {
            painter->setPen(QPen(Qt::white, 2.0, Qt::SolidLine));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(localRect(output, visible, overlay->size()));
            if (editing_) {
                painter->setBrush(Qt::white);
                painter->setPen(QPen(Qt::black, 1.0));
                const LogicalRect &selection = *selection_;
                const Point handles[] = {
                    {selection.x, selection.y},
                    {static_cast<std::int32_t>((static_cast<std::int64_t>(selection.x) + selection.right() - 1) / 2), selection.y},
                    {static_cast<std::int32_t>(selection.right() - 1), selection.y},
                    {static_cast<std::int32_t>(selection.right() - 1), static_cast<std::int32_t>((static_cast<std::int64_t>(selection.y) + selection.bottom() - 1) / 2)},
                    {static_cast<std::int32_t>(selection.right() - 1), static_cast<std::int32_t>(selection.bottom() - 1)},
                    {static_cast<std::int32_t>((static_cast<std::int64_t>(selection.x) + selection.right() - 1) / 2), static_cast<std::int32_t>(selection.bottom() - 1)},
                    {selection.x, static_cast<std::int32_t>(selection.bottom() - 1)},
                    {selection.x, static_cast<std::int32_t>((static_cast<std::int64_t>(selection.y) + selection.bottom() - 1) / 2)},
                };
                for (const Point &point : handles) {
                    if (point.x >= output.geometry.x && point.x < output.geometry.right() &&
                        point.y >= output.geometry.y && point.y < output.geometry.bottom()) {
                        const QPointF localPointValue = localPoint(output, point, overlay->size());
                        painter->drawRect(QRectF(localPointValue.x() - 4, localPointValue.y() - 4, 8, 8));
                    }
                }
            }
        }
    }

    if (selection_.has_value() &&
        (gesture_->type == Gesture::Type::Selecting || editing_)) {
        const QString dimensions =
            QStringLiteral("%1 × %2").arg(selection_->width).arg(selection_->height);
        drawInfoPill(painter, localPoint(output, Point{selection_->x, selection_->y},
                                         overlay->size()),
                     dimensions, target);
    }

    const bool loupeActive = gesture_->type == Gesture::Type::Selecting ||
        gesture_->type == Gesture::Type::Moving || gesture_->type == Gesture::Type::Resizing ||
        gesture_->type == Gesture::Type::MovingAnnotation ||
        gesture_->type == Gesture::Type::ResizingAnnotation;
    if (loupeActive && pointerOutput_ == overlay->outputIndex()) {
        drawLoupe(overlay, painter);
    }
    painter->restore();
}

void OverlayController::drawLoupe(CaptureOverlay *overlay, QPainter *painter)
{
    const OutputSession &output = overlay->output();
    const QPointF local = localPoint(output, pointer_, overlay->size());
    const std::uint32_t scale = output.scale > 0 ? output.scale : 1;
    const int sourceWidth = static_cast<int>(output.image.width());
    const int sourceHeight = static_cast<int>(output.image.height());
    if (sourceWidth <= 0 || sourceHeight <= 0) {
        return;
    }
    const int centerX = std::clamp(
        static_cast<int>(std::floor((pointer_.x - output.geometry.x) * static_cast<double>(scale))),
        0, sourceWidth - 1);
    const int centerY = std::clamp(
        static_cast<int>(std::floor((pointer_.y - output.geometry.y) * static_cast<double>(scale))),
        0, sourceHeight - 1);
    const int sampleLeft = std::clamp(centerX - kLoupeRadius, 0, sourceWidth - 1);
    const int sampleTop = std::clamp(centerY - kLoupeRadius, 0, sourceHeight - 1);
    const int sampleWidth = std::min(2 * kLoupeRadius + 1, sourceWidth - sampleLeft);
    const int sampleHeight = std::min(2 * kLoupeRadius + 1, sourceHeight - sampleTop);
    const QRect source(sampleLeft, sampleTop, sampleWidth, sampleHeight);
    const qreal radius = kLoupeDiameter / 2.0;

    QPointF center = local + QPointF(radius * 1.1, radius * 1.1);
    if (center.x() + radius > overlay->width() - kLoupeMargin ||
        center.y() + radius > overlay->height() - kLoupeMargin) {
        if (center.x() + radius > overlay->width() - kLoupeMargin) {
            center.setX(local.x() - radius * 1.1);
        }
        if (center.y() + radius > overlay->height() - kLoupeMargin) {
            center.setY(local.y() - radius * 1.1);
        }
    }

    painter->save();
    QPainterPath clipPath;
    clipPath.addEllipse(center, radius, radius);
    painter->setClipPath(clipPath);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
    const QRectF target(center.x() - radius + (sampleLeft - (centerX - kLoupeRadius)) * kLoupeZoom,
                        center.y() - radius + (sampleTop - (centerY - kLoupeRadius)) * kLoupeZoom,
                        sampleWidth * kLoupeZoom, sampleHeight * kLoupeZoom);
    painter->drawImage(target, output.image, source);
    painter->restore();

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(QColor(0, 0, 0, 190), 4.0));
    painter->drawEllipse(center, radius, radius);
    painter->setPen(QPen(Qt::white, 1.5));
    painter->drawEllipse(center, radius, radius);
    painter->setPen(QPen(QColor(255, 255, 255, 200), 1.0));
    painter->drawLine(center - QPointF(radius / 2.5, 0), center + QPointF(radius / 2.5, 0));
    painter->drawLine(center, center - QPointF(0, radius / 2.5));
    painter->drawLine(center, center + QPointF(0, radius / 2.5));
    const QString coordinates = QStringLiteral("%1, %2").arg(centerX).arg(centerY);
    painter->restore();
    drawInfoPill(painter, center + QPointF(0, radius + 2.0), coordinates,
                 QRectF(0, 0, overlay->width(), overlay->height()));
}

CaptureOverlay::CaptureOverlay(int outputIndex, OverlayController *controller, QScreen *screen)
    : QWidget(nullptr)
    , outputIndex_(outputIndex)
    , controller_(controller)
    , screen_(screen)
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_TranslucentBackground);
    setAutoFillBackground(false);
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setAcceptDrops(false);
    resize(static_cast<int>(output().geometry.width), static_cast<int>(output().geometry.height));
}

CaptureOverlay::~CaptureOverlay() = default;

int CaptureOverlay::outputIndex() const
{
    return outputIndex_;
}

const OutputSession &CaptureOverlay::output() const
{
    return controller_->session().outputs.at(outputIndex_);
}

QPointF CaptureOverlay::localFromGlobal(Point point) const
{
    return localPoint(output(), point, size());
}

bool CaptureOverlay::showLayerSurface()
{
    if (layerWindow_ == nullptr) {
        winId();
        layerWindow_ = windowHandle();
        if (layerWindow_ == nullptr) {
            return false;
        }
        auto *layer = LayerShellQt::Window::get(layerWindow_);
        if (layer == nullptr) {
            return false;
        }
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        LayerShellQt::Window::Anchors anchors(LayerShellQt::Window::AnchorTop);
        anchors |= LayerShellQt::Window::AnchorBottom;
        anchors |= LayerShellQt::Window::AnchorLeft;
        anchors |= LayerShellQt::Window::AnchorRight;
        layer->setAnchors(anchors);
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
        layer->setActivateOnShow(true);
        layer->setScope(QStringLiteral("vshot-qt-ui"));
        layer->setDesiredSize(QSize(0, 0));
        layer->setScreen(screen_);
    }
    show();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
    return true;
}

bool CaptureOverlay::showLayerSurfaceAt(int globalX, int globalY, int width, int height)
{
    winId();
    layerWindow_ = windowHandle();
    if (layerWindow_ == nullptr) {
        return false;
    }
    auto *layer = LayerShellQt::Window::get(layerWindow_);
    if (layer == nullptr) {
        return false;
    }
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    // Anchor top-left only and carve the box out with margins, so the surface
    // lands exactly on the given global rect regardless of output origin.
    LayerShellQt::Window::Anchors anchors(LayerShellQt::Window::AnchorTop);
    anchors |= LayerShellQt::Window::AnchorLeft;
    layer->setAnchors(anchors);
    layer->setExclusiveZone(-1);
    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
    layer->setActivateOnShow(true);
    layer->setScope(QStringLiteral("vshot-pin-edit"));
    layer->setDesiredSize(QSize(width, height));
    layer->setScreen(screen_);
    const QRect output = screen_ != nullptr ? screen_->geometry() : QRect();
    layer->setMargins(QMargins(std::max(0, globalX - output.left()),
                               std::max(0, globalY - output.top()), 0, 0));
    resize(width, height);
    show();
    setFocus(Qt::OtherFocusReason);
    return true;
}

void CaptureOverlay::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    controller_->paint(this, &painter);
}

void CaptureOverlay::mousePressEvent(QMouseEvent *event)
{
    controller_->press(this, event->position(), event->button(), event->modifiers());
    event->accept();
}

void CaptureOverlay::mouseMoveEvent(QMouseEvent *event)
{
    controller_->move(this, event->position(), event->buttons(), event->modifiers());
    event->accept();
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    controller_->release(this, event->position(), event->button(), event->modifiers());
    event->accept();
}

void CaptureOverlay::mouseDoubleClickEvent(QMouseEvent *event)
{
    controller_->doubleClick(this, event->position(), event->button());
    event->accept();
}

void CaptureOverlay::keyPressEvent(QKeyEvent *event)
{
    controller_->key(this, event->key(), event->modifiers());
    event->accept();
}

void CaptureOverlay::closeEvent(QCloseEvent *event)
{
    // The compositor (or a stray close request) must not hang the Rust side:
    // treat an externally closed overlay as a cancelled session.
    controller_->cancel();
    event->accept();
}

void CaptureOverlay::leaveEvent(QEvent *event)
{
    setCursor(Qt::CrossCursor);
    QWidget::leaveEvent(event);
}

} // namespace vshot

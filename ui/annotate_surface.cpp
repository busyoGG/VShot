// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#include "annotate_surface.hpp"

#include "i18n.hpp"

#include <LayerShellQt/Window>

#include <QApplication>
#include <QEnterEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPair>
#include <QScreen>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QSlider>
#include <QTransform>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace vshot {

namespace {

// The palette, in logical pixels.  The panel is one horizontal row wherever an
// output gives it room, and wraps onto more rows only where it does not; a row
// is kToolbarHeight tall, the button height plus the same 6 logical pixels
// above and below it the capture overlay's palette used.
constexpr int kToolbarHeight = 40;
constexpr int kToolbarPad = 8;
constexpr int kGripWidth = 16;
constexpr int kButtonHeight = 28;
constexpr int kToolbarPadY = (kToolbarHeight - kButtonHeight) / 2;
constexpr int kLabelPadX = 9;
constexpr int kDotBox = 26;
constexpr qreal kPanelRadius = 12.0;
constexpr qreal kButtonRadius = 8.0;
constexpr qreal kSuperellipseExponent = 5.0;

// The dynamic property that marks a divider to the flow layout: a separator
// that may not be left at the end of a row, where a wrap would strand it.
constexpr auto kSeparatorProperty = "vshotFlowSeparator";

// The label font of the panel's text buttons, at the size the capture
// overlay's QSS used for the same row.
QFont buttonFont()
{
    QFont font = QApplication::font();
    font.setPixelSize(12);
    return font;
}

// The annotation text font.  A text label is sized from the stroke width so the
// one width control covers both: 6x the pen width keeps a label legible beside
// the line it annotates.  Pixel size rather than point size, so a label is the
// same number of pixels on a 2x output as it is on a 1x one.
QFont annotationFont(int width)
{
    QFont font = QApplication::font();
    font.setPixelSize(std::max(1, 6 * width));
    return font;
}

// Squircle (superellipse) outline, corner radius with exponent 5, matching the
// capture overlay's palette.  The two files share no code on purpose, so this
// is a local copy rather than a shared header.
QPainterPath superellipsePath(const QRectF &bounds, qreal radius, qreal exponent)
{
    if (bounds.isEmpty()) {
        return QPainterPath();
    }
    const qreal most = std::min(bounds.width(), bounds.height()) / 2.0;
    radius = std::clamp(radius, 0.0, most);
    QPainterPath path;
    if (radius <= 0.0) {
        path.addRect(bounds);
        return path;
    }
    const int steps = 12;
    bool first = true;
    const auto corner = [&](qreal centerX, qreal centerY, qreal startDegrees, qreal endDegrees) {
        for (int i = 0; i <= steps; ++i) {
            const qreal angle =
                qDegreesToRadians(startDegrees + (endDegrees - startDegrees) * i / steps);
            const qreal cosine = std::cos(angle);
            const qreal sine = std::sin(angle);
            const qreal x = centerX
                + std::copysign(std::pow(std::abs(cosine), 2.0 / exponent), cosine) * radius;
            const qreal y = centerY
                + std::copysign(std::pow(std::abs(sine), 2.0 / exponent), sine) * radius;
            if (first) {
                path.moveTo(x, y);
                first = false;
            } else {
                path.lineTo(x, y);
            }
        }
    };
    corner(bounds.left() + radius, bounds.top() + radius, 180.0, 270.0);
    corner(bounds.right() - radius, bounds.top() + radius, 270.0, 360.0);
    corner(bounds.right() - radius, bounds.bottom() - radius, 0.0, 90.0);
    corner(bounds.left() + radius, bounds.bottom() - radius, 90.0, 180.0);
    path.closeSubpath();
    return path;
}

QRectF normalizedRect(const QPointF &a, const QPointF &b)
{
    return QRectF(QPointF(std::min(a.x(), b.x()), std::min(a.y(), b.y())),
                  QPointF(std::max(a.x(), b.x()), std::max(a.y(), b.y())));
}

double pointSegmentDistance(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const double length2 = ab.x() * ab.x() + ab.y() * ab.y();
    if (length2 <= 0.0) {
        return std::hypot(p.x() - a.x(), p.y() - a.y());
    }
    double t = ((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / length2;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF projection(a.x() + t * ab.x(), a.y() + t * ab.y());
    return std::hypot(p.x() - projection.x(), p.y() - projection.y());
}

// Two pi, spelled out rather than read from a platform's `M_PI`: the wave's
// shape must not depend on which libm the build landed on, and the constant is
// the one place that could silently differ.
constexpr double kTau = 6.283185307179586476925286766559;

// The peak deviation of a wave stroke from its centre line, in logical pixels:
// `max(width * 2, 4)`, so a wave is as tall as the pen is thick and never
// flatter than four pixels.
double waveAmplitude(int width)
{
    return std::max(width * 2, 4);
}

// One full period of a wave stroke along its line, in logical pixels, floored
// at `max(width * 6, 18)` the same way the amplitude is.
double waveWavelength(int width)
{
    return std::max(width * 6, 18);
}

// Samples the sine wave along the segment `start`..`end` as a polyline, one
// point per `step` of arc length.  `amplitude` is the peak deviation from the
// centre line and `wavelength` one full period along the line, both in the same
// units as the segment; `annotate_surface` works in logical pixels, so `step`
// is one of those.
//
// A zero-length segment returns the single `start` point, which the caller
// turns into a dot.
//
// The phase finishes on a whole number of cycles: `cycles = max(1, round(L /
// wavelength))` and the wavelength actually used is `L / cycles`.  That is the
// step that puts both ends back on the line the user dragged -- without it the
// far end is left wherever the phase happened to be, smeared off to one side,
// and the wave no longer reads as one drawn from A to B.
QVector<QPointF> wavePolyline(const QPointF &start, const QPointF &end, double amplitude,
                              double wavelength, double step)
{
    const QPointF delta = end - start;
    const double length = std::hypot(delta.x(), delta.y());
    if (length <= 0.0) {
        return {start};
    }
    // One sample per `step`, plus both endpoints.
    const int n = std::max(2, static_cast<int>(std::ceil(length / std::max(step, 1e-9))) + 1);
    const QPointF dir(delta.x() / length, delta.y() / length);
    // The 90-degree rotation of `dir`: the direction the wave deviates in.
    const QPointF normal(-dir.y(), dir.x());
    const double cycles = std::max(1.0, std::round(length / std::max(wavelength, 1.0)));
    const double radiansPerPixel = kTau / (length / cycles);
    QVector<QPointF> points;
    points.reserve(n);
    const double last = static_cast<double>(n - 1);
    for (int i = 0; i < n; ++i) {
        const double u = (static_cast<double>(i) / last) * length;
        const double offset = amplitude * std::sin(radiansPerPixel * u);
        points.append(start + dir * u + normal * offset);
    }
    return points;
}

// One point on the cubic whose control points are `p0`..`p3`, at parameter `t`.
// The Bernstein form is the one `QPainterPath::cubicTo` evaluates, so a sample
// taken here lies on the very curve the painter draws -- which is what lets the
// bounding box and the eraser's hit test be built from samples.
QPointF cubicPoint(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3,
                   double t)
{
    const double u = 1.0 - t;
    const double a = u * u * u;
    const double b = 3.0 * u * u * t;
    const double c = 3.0 * u * t * t;
    const double d = t * t * t;
    return QPointF(a * p0.x() + b * p1.x() + c * p2.x() + d * p3.x(),
                   a * p0.y() + b * p1.y() + c * p2.y() + d * p3.y());
}

// The incoming control point of an anchor: the mirror of its outgoing handle
// through the anchor itself.  The wire format the capture editor sends Rust
// carries one handle per anchor and the curve is symmetric, so the incoming side
// is derived rather than stored.
QPointF mirrorHandle(const QPointF &anchor, const QPointF &handleOut)
{
    return QPointF(2.0 * anchor.x() - handleOut.x(), 2.0 * anchor.y() - handleOut.y());
}

// How many anchors a pen path has.  `points` is interleaved
// [anchor0, handleOut0, anchor1, handleOut1, ...], so it is always even and the
// anchors are half of it.
int bezierAnchors(const QVector<QPointF> &points)
{
    return static_cast<int>(points.size() / 2);
}

// The pen path as a QPainterPath.  The preview and the committed ink both draw
// through it, so the two cannot disagree about the curve.  A closed path joins
// the last anchor back to the first, through both their handles.
QPainterPath bezierPath(const QVector<QPointF> &points, bool closed)
{
    QPainterPath path;
    const int anchors = bezierAnchors(points);
    if (anchors <= 0) {
        return path;
    }
    path.moveTo(points.at(0));
    const int segments = closed ? anchors : anchors - 1;
    for (int index = 0; index < segments; ++index) {
        const int next = (index + 1) % anchors;
        path.cubicTo(points.at(2 * index + 1),
                     mirrorHandle(points.at(2 * next), points.at(2 * next + 1)),
                     points.at(2 * next));
    }
    if (closed) {
        path.closeSubpath();
    }
    return path;
}

// The same path sampled into a polyline, about one sample per two logical
// pixels of control polygon so the sample follows the curve's own turning.  The
// samples lie on the curve, which is what the tight bounding box and the
// eraser's hit test need: a box over the control points alone would be far
// larger than the ink, and one over the anchors alone would clip the very bulge
// the handles pull out.
QVector<QPointF> bezierPolyline(const QVector<QPointF> &points, bool closed)
{
    QVector<QPointF> samples;
    const int anchors = bezierAnchors(points);
    if (anchors <= 0) {
        return samples;
    }
    samples.append(points.at(0));
    const int segments = closed ? anchors : anchors - 1;
    for (int index = 0; index < segments; ++index) {
        const int next = (index + 1) % anchors;
        const QPointF &start = points.at(2 * index);
        const QPointF &handle = points.at(2 * index + 1);
        const QPointF &end = points.at(2 * next);
        const QPointF incoming = mirrorHandle(end, points.at(2 * next + 1));
        const double span = QLineF(start, handle).length() + QLineF(handle, incoming).length() +
            QLineF(incoming, end).length();
        const int steps = std::clamp(static_cast<int>(std::ceil(span / 2.0)), 8, 96);
        for (int step = 1; step <= steps; ++step) {
            samples.append(cubicPoint(start, handle, incoming, end,
                                      static_cast<double>(step) / steps));
        }
    }
    return samples;
}

// The tight logical box of a sampled polyline, or a null rect when there is
// nothing to bound.  Folded by hand rather than with `QRectF::united`: uniting
// onto a null rect returns the other operand, which turns a box that was never
// seeded into whatever the last point happened to be -- a bounding box that
// collapses to a point, and stale ink left on screen.
QRectF polylineBounds(const QVector<QPointF> &samples)
{
    if (samples.isEmpty()) {
        return QRectF();
    }
    double left = samples.constFirst().x();
    double right = left;
    double top = samples.constFirst().y();
    double bottom = top;
    for (const QPointF &point : samples) {
        left = std::min(left, point.x());
        right = std::max(right, point.x());
        top = std::min(top, point.y());
        bottom = std::max(bottom, point.y());
    }
    return QRectF(QPointF(left, top), QPointF(right, bottom));
}

// Distance from a point to a rectangle's outline, zero inside it.  A rectangle
// stroke is only its outline: the eraser must not be able to take it by
// brushing the empty middle.
double rectOutlineDistance(const QPointF &p, const QRectF &rect)
{
    if (rect.contains(p)) {
        return 0.0;
    }
    const QPointF topLeft(rect.left(), rect.top());
    const QPointF topRight(rect.right(), rect.top());
    const QPointF bottomRight(rect.right(), rect.bottom());
    const QPointF bottomLeft(rect.left(), rect.bottom());
    return std::min({pointSegmentDistance(p, topLeft, topRight),
                     pointSegmentDistance(p, topRight, bottomRight),
                     pointSegmentDistance(p, bottomRight, bottomLeft),
                     pointSegmentDistance(p, bottomLeft, topLeft)});
}

bool pointInTriangle(const QPointF &p, const QPointF &a, const QPointF &b, const QPointF &c)
{
    const auto side = [](const QPointF &p1, const QPointF &p2, const QPointF &p3) {
        return (p1.x() - p3.x()) * (p2.y() - p3.y()) - (p2.x() - p3.x()) * (p1.y() - p3.y());
    };
    const double d1 = side(p, a, b);
    const double d2 = side(p, b, c);
    const double d3 = side(p, c, a);
    const bool negative = d1 < 0.0 || d2 < 0.0 || d3 < 0.0;
    const bool positive = d1 > 0.0 || d2 > 0.0 || d3 > 0.0;
    return !(negative && positive);
}

// The three corners of an arrow's head: tip, then the two wings.  Sized from
// the stroke width exactly as the capture overlay's final renderer does, so
// the preview, the canvas and the eraser all agree on where the ink is.
QVector<QPointF> arrowHeadPoints(const QPointF &tip, const QPointF &tail, double width)
{
    const QLineF line(tail, tip);
    if (line.length() <= 1.0) {
        return {};
    }
    const double head = std::min(std::max(6.0, width * 4.0), line.length());
    const double wing = std::max(head * 0.55, width);
    const double unitX = (tip.x() - tail.x()) / line.length();
    const double unitY = (tip.y() - tail.y()) / line.length();
    const QPointF base(tip.x() - unitX * head, tip.y() - unitY * head);
    return {tip, QPointF(base.x() - unitY * wing, base.y() + unitX * wing),
            QPointF(base.x() + unitY * wing, base.y() - unitX * wing)};
}

// The rect a text stroke's glyphs occupy, top-left anchored at its first point.
QRectF textBounds(const QVector<QPointF> &points, int width, const QString &text)
{
    if (points.isEmpty()) {
        return QRectF();
    }
    const QFontMetricsF metrics(annotationFont(width));
    return QRectF(points.constFirst(),
                  QSizeF(metrics.horizontalAdvance(text), metrics.height()));
}

// A numbered badge is sized from the stroke width rather than from a control of
// its own, so the one width slider covers it too: six pen widths across, floored
// at 18 logical pixels so the thinnest pen still draws something legible and
// capped at 96 so the thickest one does not paint a billboard.
constexpr int kNumberMinDiameter = 18;
constexpr int kNumberMaxDiameter = 96;

int numberDiameter(int width)
{
    return std::clamp(width * 6, kNumberMinDiameter, kNumberMaxDiameter);
}

// The glyphs are a touch over half the badge, bold, so that a two-digit count
// still sits inside the disc.
int numberFontPixels(int diameter)
{
    return std::max(1, static_cast<int>(std::lround(diameter * 0.55)));
}

QFont numberFont(int diameter)
{
    QFont font = QApplication::font();
    font.setPixelSize(numberFontPixels(diameter));
    font.setBold(true);
    return font;
}

// The square the badge occupies, centred on the point the click landed on.  The
// click is the badge's centre rather than its corner so that a badge lands
// exactly where the pointer was, the way a click-to-place tool should.
QRectF numberBadgeRect(const QPointF &center, int width)
{
    const double diameter = numberDiameter(width);
    return QRectF(center.x() - diameter / 2.0, center.y() - diameter / 2.0, diameter, diameter);
}

// The halo the bare-number style outlines its glyphs with: a light glyph gets a
// dark rim and a dark glyph a light one, so the count reads over a desktop of
// any colour.  The ink itself is never changed -- the halo sits under it.
QColor numberHalo(const QColor &ink)
{
    return ink.lightness() > 140 ? QColor(0, 0, 0, 210) : QColor(255, 255, 255, 210);
}

// The glyph colour that reads on top of a badge filled with `ink`.  White is the
// ①②③ look and what a red, green, blue or black badge gets, but the palette
// also carries a yellow and a white: white-on-white would erase the count, so a
// light badge takes near-black glyphs instead.
QColor numberOnInk(const QColor &ink)
{
    return ink.lightness() > 160 ? QColor(20, 20, 20) : QColor(255, 255, 255);
}

constexpr qreal kNumberHaloWidth = 2.0;

// The badge style's name, spelled the same way by the standalone toolbar's
// tooltip and the capture editor's style row.
QString numberStyleName(AnnotateSurface::NumberStyle style)
{
    switch (style) {
    case AnnotateSurface::NumberStyle::FilledCircle:
        return uiTr("Filled circle");
    case AnnotateSurface::NumberStyle::Ring:
        return uiTr("Ring");
    case AnnotateSurface::NumberStyle::Square:
        return uiTr("Square");
    case AnnotateSurface::NumberStyle::Plain:
        return uiTr("Plain");
    }
    return QString();
}

// The style a second click on the armed number button lands on.  One button
// cycling through the four keeps the palette on one row.
AnnotateSurface::NumberStyle nextNumberStyle(AnnotateSurface::NumberStyle style)
{
    switch (style) {
    case AnnotateSurface::NumberStyle::FilledCircle:
        return AnnotateSurface::NumberStyle::Ring;
    case AnnotateSurface::NumberStyle::Ring:
        return AnnotateSurface::NumberStyle::Square;
    case AnnotateSurface::NumberStyle::Square:
        return AnnotateSurface::NumberStyle::Plain;
    case AnnotateSurface::NumberStyle::Plain:
        break;
    }
    return AnnotateSurface::NumberStyle::FilledCircle;
}

// The one place a numbered badge is turned into ink.  Every path a number takes
// to the screen -- the live surface's preview, its per-stroke raster and the
// capture editor's bitmap -- draws through here, so the four styles cannot drift
// apart between them.
void paintNumberBadge(QPainter &painter, const QRectF &box, const QString &text,
                      AnnotateSurface::NumberStyle style, const QColor &color, int width)
{
    const int diameter = std::max(1, static_cast<int>(std::lround(box.width())));
    painter.setFont(numberFont(diameter));
    switch (style) {
    case AnnotateSurface::NumberStyle::FilledCircle:
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(box);
        // The glyphs are the badge's own knockout: the disc carries the colour,
        // and the count reads against it.
        painter.setBrush(Qt::NoBrush);
        painter.setPen(numberOnInk(color));
        painter.drawText(box, Qt::AlignCenter, text);
        break;
    case AnnotateSurface::NumberStyle::Ring: {
        // A hollow ring whose line is the current stroke width, inset by half of
        // it so the ink stays inside the badge's own box.
        const qreal pen = std::clamp(static_cast<qreal>(std::max(1, width)), 1.0,
                                     std::max(1.0, box.width() / 4.0));
        const qreal inset = pen / 2.0;
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(color, pen, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawEllipse(box.adjusted(inset, inset, -inset, -inset));
        painter.setPen(color);
        painter.drawText(box, Qt::AlignCenter, text);
        break;
    }
    case AnnotateSurface::NumberStyle::Square: {
        const qreal radius = box.width() * 0.22;
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(box, radius, radius);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(numberOnInk(color));
        painter.drawText(box, Qt::AlignCenter, text);
        break;
    }
    case AnnotateSurface::NumberStyle::Plain: {
        // No background: the glyphs themselves, stroked once with a thin
        // contrasting halo before the fill lands on top of it.  A path rather
        // than drawText because only a path can be stroked.
        const QFontMetricsF metrics(painter.font());
        const QRectF glyph = metrics.boundingRect(text);
        const QPointF origin(box.center().x() - glyph.width() / 2.0 - glyph.left(),
                             box.center().y() + metrics.capHeight() / 2.0);
        QPainterPath path;
        path.addText(origin, painter.font(), text);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(numberHalo(color), kNumberHaloWidth, Qt::SolidLine, Qt::RoundCap,
                            Qt::RoundJoin));
        painter.drawPath(path);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPath(path);
        break;
    }
    }
}

// The logical rect a stroke can have painted into, already grown by its width
// and, for an arrow, by its head.  Both the repaint region and the eraser's
// reach are asked from here.  `closed` is only ever true for a committed pen
// path: an in-progress preview has not been closed yet.
QRectF strokeBounds(AnnotateSurface::Tool tool, const QVector<QPointF> &points, int width,
                    const QString &text, bool closed = false)
{
    if (tool == AnnotateSurface::Tool::Text) {
        return textBounds(points, width, text);
    }
    if (tool == AnnotateSurface::Tool::Number) {
        if (points.isEmpty()) {
            return QRectF();
        }
        // The badge's box, grown by half the pen width: the ring style draws its
        // line centred on the ellipse, so its ink reaches that far past the box,
        // and the repaint region has to cover it.
        const qreal grow = width / 2.0 + 1.0;
        return numberBadgeRect(points.constFirst(), width).adjusted(-grow, -grow, grow, grow);
    }
    if (points.isEmpty()) {
        return QRectF();
    }
    // A pen path is a cubic per segment, and a cubic leaves the box of its own
    // anchors -- the handles pull it out.  The box therefore comes from the
    // sampled curve, not from the points the model holds; a box over the
    // control points instead would be far larger than the ink, and one over the
    // anchors alone would clip the bulge and leave a stale arc on screen.
    if (tool == AnnotateSurface::Tool::Bezier) {
        const qreal grow = width / 2.0 + 1.0;
        if (bezierAnchors(points) < 2) {
            // One anchor and no segment between anything: the click is a dot,
            // and it gets the box the pen gives one.
            return QRectF(points.constFirst(), QSizeF(0, 0))
                .adjusted(-grow, -grow, grow, grow);
        }
        return polylineBounds(bezierPolyline(points, closed))
            .adjusted(-grow, -grow, grow, grow);
    }
    // A wave is not the straight line between its two points: its crests reach
    // `amplitude` off that line, so its box has to come from the sampled
    // polyline.  A box over the endpoints alone would leave every crest outside
    // the repaint region, and the stale crest would stay on screen.  The line
    // tool is the plain two-point case the loop below already gives: one
    // `normalizedRect` of its endpoints.
    if (tool == AnnotateSurface::Tool::Wave && points.size() >= 2) {
        const QVector<QPointF> wave =
            wavePolyline(points.constFirst(), points.constLast(), waveAmplitude(width),
                         waveWavelength(width), 1.0);
        // A zero-length wave samples to a single point; the generic path below
        // then gives the dot it draws the same small box a pen dot gets.
        if (wave.size() >= 2) {
            QRectF waveBox = normalizedRect(wave.constFirst(), wave.at(1));
            for (qsizetype i = 2; i < wave.size(); ++i) {
                waveBox = waveBox.united(normalizedRect(wave.at(i - 1), wave.at(i)));
            }
            const qreal waveGrow = width / 2.0 + 1.0;
            return waveBox.adjusted(-waveGrow, -waveGrow, waveGrow, waveGrow);
        }
    }
    QRectF box(points.constFirst(), QSizeF(0, 0));
    for (qsizetype i = 1; i < points.size(); ++i) {
        box = box.united(normalizedRect(points.at(i - 1), points.at(i)));
    }
    qreal grow = width / 2.0 + 1.0;
    if (tool == AnnotateSurface::Tool::Arrow && points.size() >= 2) {
        const QVector<QPointF> head =
            arrowHeadPoints(points.constLast(), points.at(points.size() - 2), width);
        for (const QPointF &point : head) {
            box = box.united(QRectF(point, QSizeF(0, 0)));
        }
        grow = std::max(grow, static_cast<qreal>(width));
    }
    return box.adjusted(-grow, -grow, grow, grow);
}

// The one place a stroke is turned into ink.  The pen, the rectangle, the
// arrow, the line, the wave, the bezier pen, the text label and the numbered
// badge are drawn here, whether into the backing image or straight onto the
// surface as a preview.
void paintStrokeInk(QPainter &painter, AnnotateSurface::Tool tool, const QVector<QPointF> &points,
                    const QColor &color, int width, const QString &text,
                    AnnotateSurface::NumberStyle numberStyle, int number, bool closed)
{
    painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    switch (tool) {
    case AnnotateSurface::Tool::Pen:
        if (points.size() >= 2) {
            painter.drawPolyline(points.constData(), points.size());
        } else if (points.size() == 1) {
            // A click that never moved is still a dot of ink.
            painter.drawPoint(points.constFirst());
        }
        break;
    case AnnotateSurface::Tool::Rect:
        if (points.size() >= 2) {
            painter.drawRect(normalizedRect(points.constFirst(), points.constLast()));
        }
        break;
    case AnnotateSurface::Tool::Arrow:
        if (points.size() >= 2) {
            painter.drawPolyline(points.constData(), points.size());
            const QVector<QPointF> head =
                arrowHeadPoints(points.constLast(), points.at(points.size() - 2), width);
            if (head.size() == 3) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                painter.drawPolygon(head.constData(), 3);
            }
        }
        break;
    case AnnotateSurface::Tool::Line:
        if (points.size() >= 2) {
            // Round caps, so the two ends are half-discs rather than the pen's
            // square corners.
            painter.drawLine(points.constFirst(), points.constLast());
        } else if (points.size() == 1) {
            painter.drawPoint(points.constFirst());
        }
        break;
    case AnnotateSurface::Tool::Wave:
        if (points.size() >= 2) {
            const QVector<QPointF> wave =
                wavePolyline(points.constFirst(), points.constLast(), waveAmplitude(width),
                             waveWavelength(width), 1.0);
            if (wave.size() >= 2) {
                painter.drawPolyline(wave.constData(), wave.size());
            } else if (wave.size() == 1) {
                painter.drawPoint(wave.constFirst());
            }
        }
        break;
    case AnnotateSurface::Tool::Bezier:
        if (points.isEmpty()) {
            break;
        }
        if (bezierAnchors(points) < 2) {
            // A click that was never dragged past its own anchor is a dot, the
            // same ink the pen gives one.
            painter.drawPoint(points.constFirst());
            break;
        }
        {
            const QPainterPath path = bezierPath(points, closed);
            if (closed) {
                // Fill first and stroke second, so the outline is not tinted
                // by the translucent fill it sits on.  The fill is the stroke's
                // own colour at half its alpha, floored: that is what "a
                // translucent fill under a solid outline" means for a colour
                // the user picked an opacity for.
                QColor fill = color;
                fill.setAlpha(color.alpha() / 2);
                painter.fillPath(path, fill);
                painter.strokePath(path, QPen(color, width, Qt::SolidLine, Qt::RoundCap,
                                              Qt::RoundJoin));
                break;
            }
            painter.drawPath(path);
        }
        break;
    case AnnotateSurface::Tool::Text:
        if (!points.isEmpty()) {
            painter.setFont(annotationFont(width));
            painter.setPen(color);
            painter.drawText(textBounds(points, width, text), Qt::AlignLeft | Qt::AlignTop, text);
        }
        break;
    case AnnotateSurface::Tool::Number:
        if (!points.isEmpty()) {
            paintNumberBadge(painter, numberBadgeRect(points.constFirst(), width),
                             QString::number(number), numberStyle, color, width);
        }
        break;
    case AnnotateSurface::Tool::Eraser:
        // Not a drawing tool: it removes whole strokes.
        break;
    }
}

QRect grownDirtyRect(const QRectF &box)
{
    if (box.isNull()) {
        return QRect();
    }
    return box.toAlignedRect().adjusted(-2, -2, 2, 2);
}

// One button of the panel.  QSS backgrounds are limited to circular corners and
// the capture overlay's ToolCardFrame paints by event-filtering other widgets,
// so a button that paints its own hover, press and active state is the smaller
// machine here.  Every button is a plain QWidget wired through std::function:
// no signal is emitted from this file.
class ToolbarButton final : public QWidget {
public:
    enum class Kind {
        Label,
        Swatch,
        Width,
        Cross,
        // The number tool's button: a badge drawn in the style that is armed,
        // so the look on the button is the look the next click will place.
        Number,
    };

    ToolbarButton(QWidget *parent, Kind kind, const QString &label, const QColor &swatch,
                  int diameter)
        : QWidget(parent)
        , kind_(kind)
        , label_(label)
        , swatch_(swatch)
        , diameter_(diameter)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
    }

    void setOnClick(std::function<void()> onClick) { onClick_ = std::move(onClick); }
    void setActive(bool active)
    {
        if (active_ != active) {
            active_ = active;
            update();
        }
    }
    // The badge the number button shows.  Only that one button has a style of
    // its own; for every other kind the setter is a no-op.
    void setNumberStyle(AnnotateSurface::NumberStyle style)
    {
        if (numberStyle_ == style) {
            return;
        }
        numberStyle_ = style;
        update();
    }
    void setEnabled(bool enabled)
    {
        if (enabled_ != enabled) {
            enabled_ = enabled;
            update();
        }
    }

    QSize sizeHint() const override
    {
        if (kind_ == Kind::Label) {
            const QFontMetrics metrics(buttonFont());
            return QSize(metrics.horizontalAdvance(label_) + 2 * kLabelPadX, kButtonHeight);
        }
        return QSize(kDotBox, kButtonHeight);
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        hovered_ = true;
        update();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        hovered_ = false;
        pressed_ = false;
        update();
        QWidget::leaveEvent(event);
    }

    // Every left press is consumed, disabled buttons included: an ignored press
    // would propagate to the panel and then to the surface, and clicking "Undo"
    // with nothing to undo would start drawing a stroke instead.
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        if (enabled_) {
            pressed_ = true;
        }
        update();
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        const bool fire = pressed_ && enabled_ && rect().contains(event->position().toPoint());
        pressed_ = false;
        update();
        if (fire && onClick_) {
            onClick_();
        }
        event->accept();
    }

    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF box = QRectF(rect()).adjusted(1.0, 2.0, -1.0, -2.0);
        const bool contentActive =
            active_ && (kind_ == Kind::Label || kind_ == Kind::Cross || kind_ == Kind::Number);
        if (contentActive) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(221, 225, 255));
            painter.drawPath(superellipsePath(box, kButtonRadius, kSuperellipseExponent));
        } else if (enabled_ && (hovered_ || pressed_)) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(pressed_ ? QColor(53, 60, 70) : QColor(44, 50, 59));
            painter.drawPath(superellipsePath(box, kButtonRadius, kSuperellipseExponent));
        }

        const QColor content = !enabled_ ? QColor(111, 118, 128)
                                         : (contentActive ? QColor(0, 20, 92)
                                                          : QColor(230, 225, 229));
        const QPointF center = QRectF(rect()).center();
        switch (kind_) {
        case Kind::Label:
            painter.setFont(buttonFont());
            painter.setPen(content);
            painter.drawText(rect(), Qt::AlignCenter, label_);
            break;
        case Kind::Swatch: {
            const qreal radius = 9.0;
            if (active_) {
                // A ring rather than a light fill: the fill would hide the very
                // colour the button exists to show.
                painter.setPen(QPen(QColor(221, 225, 255), 2.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(center, radius + 3.0, radius + 3.0);
            }
            painter.setPen(QPen(QColor(255, 255, 255, 60), 1.0));
            painter.setBrush(enabled_ ? swatch_ : swatch_.darker(160));
            painter.drawEllipse(center, radius, radius);
            break;
        }
        case Kind::Width: {
            const qreal radius = diameter_ / 2.0;
            if (active_) {
                painter.setPen(QPen(QColor(221, 225, 255), 2.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(center, radius + 3.5, radius + 3.5);
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(content);
            painter.drawEllipse(center, radius, radius);
            break;
        }
        case Kind::Number: {
            // A miniature of the armed badge.  The button borrows the panel's
            // own content colour rather than the pen's: a black pen would leave
            // a black badge on a black panel, and the swatch already shows the
            // colour.  The tooltip names the style.
            const qreal side = std::min(box.width(), box.height()) - 6.0;
            if (side > 4.0) {
                const QRectF badge(center.x() - side / 2.0, center.y() - side / 2.0, side,
                                   side);
                paintNumberBadge(painter, badge, QStringLiteral("1"), numberStyle_, content,
                                 2);
            }
            break;
        }
        case Kind::Cross: {
            // A drawn glyph keeps the quit button independent of whichever font
            // the session happens to have.
            painter.setPen(QPen(content, 2.0, Qt::SolidLine, Qt::RoundCap));
            const qreal arm = 5.0;
            painter.drawLine(QPointF(center.x() - arm, center.y() - arm),
                             QPointF(center.x() + arm, center.y() + arm));
            painter.drawLine(QPointF(center.x() - arm, center.y() + arm),
                             QPointF(center.x() + arm, center.y() - arm));
            break;
        }
        }
    }

private:
    Kind kind_;
    QString label_;
    QColor swatch_;
    int diameter_ = 0;
    AnnotateSurface::NumberStyle numberStyle_ = AnnotateSurface::NumberStyle::FilledCircle;
    bool hovered_ = false;
    bool pressed_ = false;
    bool active_ = false;
    bool enabled_ = true;
    std::function<void()> onClick_;
};

// A layout that wraps its items onto as many rows as the width it is handed
// needs.  The palette is one row on an output with room for it -- its order,
// its spacing and its separators are the user's muscle memory and must not
// move -- but that same row is wider than a small output, and a panel that runs
// off the edge of the screen it annotates is unusable.  This is the Qt
// "FlowLayout" machine: a QLayout whose height depends on its width, so the
// panel hands it the width the output allows and takes back however many rows
// that turns into.
//
// One wrinkle the Qt example does not carry: several of the palette's children
// are fixed-size (the drag grip, the divider, the opacity slider and its value
// label), and a widget's sizeHint does not shrink to the maximum its own
// setFixedSize imposes.  Every item is therefore measured through `clamped()`,
// the hint after its minimum and maximum are honoured, which is exactly the
// size a QBoxLayout would have allocated it.
class FlowLayout final : public QLayout {
public:
    explicit FlowLayout(QWidget *parent)
        : QLayout(parent)
    {
    }

    ~FlowLayout() override
    {
        while (QLayoutItem *item = takeAt(0)) {
            delete item;
        }
    }

    void addItem(QLayoutItem *item) override { items_.append(item); }

    // A bare gap, as QBoxLayout::addSpacing leaves one: a fixed-width item that
    // still takes the ordinary spacing on either side of it.
    void addSpacing(int size)
    {
        addItem(new QSpacerItem(size, 0, QSizePolicy::Fixed, QSizePolicy::Minimum));
    }

    int count() const override { return static_cast<int>(items_.size()); }
    QLayoutItem *itemAt(int index) const override
    {
        return index >= 0 && index < items_.size() ? items_.at(index) : nullptr;
    }
    QLayoutItem *takeAt(int index) override
    {
        return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
    }

    // Nothing in the palette stretches: every child is a fixed-size button,
    // dot, divider or slider, exactly the size its own hint asks for.
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override
    {
        return doLayout(QRect(0, 0, width, 0), true);
    }

    // The natural size: everything on one row.  This is what "does the palette
    // fit widthwise" is asked about, so it is the one-row width rather than the
    // smallest size the flow could ever be squeezed into.
    QSize sizeHint() const override
    {
        const QMargins margins = contentsMargins();
        int width = margins.left() + margins.right();
        int height = 0;
        for (int i = 0; i < items_.size(); ++i) {
            const QSize item = clamped(items_.at(i));
            width += item.width();
            if (i > 0) {
                width += spacing();
            }
            height = std::max(height, item.height());
        }
        return QSize(width, height + margins.top() + margins.bottom());
    }

    QSize minimumSize() const override
    {
        const QMargins margins = contentsMargins();
        QSize size;
        for (const QLayoutItem *item : items_) {
            size = size.expandedTo(clamped(item));
        }
        return size
            + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    }

    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        doLayout(rect, false);
    }

private:
    // The size the layout allocates an item: its hint, never past the maximum a
    // fixed size imposes nor below its minimum.
    static QSize clamped(const QLayoutItem *item)
    {
        return item->sizeHint().boundedTo(item->maximumSize()).expandedTo(item->minimumSize());
    }

    // Whether an item is one of the palette's separators.  A separator is glued
    // to the item it introduces so a wrap never leaves it stranded at the end
    // of a row: it starts the next row together with the group it divides off.
    static bool isSeparator(const QLayoutItem *item)
    {
        const QWidget *widget = item->widget();
        return widget != nullptr && widget->property(kSeparatorProperty).toBool();
    }

    // Packs the items into rows inside `rect` and returns the height they take.
    // With `testOnly` the placement is measured but nothing moves, which is
    // what `heightForWidth` needs.
    int doLayout(const QRect &rect, bool testOnly) const
    {
        const QMargins margins = contentsMargins();
        const QRect row = rect.adjusted(margins.left(), margins.top(),
                                        -margins.right(), -margins.bottom());
        const int gap = spacing();
        int x = row.x();
        int y = row.y();
        int rowHeight = 0;
        int rowStart = 0;
        for (int i = 0; i < items_.size(); ++i) {
            const QSize size = clamped(items_.at(i));
            int need = size.width();
            if (isSeparator(items_.at(i)) && i + 1 < items_.size()) {
                need += gap + clamped(items_.at(i + 1)).width();
            }
            if (x + need > row.right() + 1 && rowHeight > 0) {
                if (!testOnly) {
                    placeRow(rowStart, i, row.x(), y, rowHeight);
                }
                x = row.x();
                y += rowHeight + gap;
                rowHeight = 0;
                rowStart = i;
            }
            x += size.width() + gap;
            rowHeight = std::max(rowHeight, size.height());
        }
        if (!testOnly && rowStart < items_.size()) {
            placeRow(rowStart, items_.size(), row.x(), y, rowHeight);
        }
        return y + rowHeight - rect.y() + margins.bottom();
    }

    // Lays one row's items out left to right, each centred against the row's
    // tallest item the way a QBoxLayout centres its children.
    void placeRow(int begin, int end, int left, int top, int height) const
    {
        int x = left;
        for (int i = begin; i < end; ++i) {
            const QSize size = clamped(items_.at(i));
            items_.at(i)->setGeometry(QRect(QPoint(x, top + (height - size.height()) / 2), size));
            x += size.width() + spacing();
        }
    }

    QList<QLayoutItem *> items_;
};

} // namespace

// The floating palette.  A child widget of the surface, so its buttons keep
// their clicks apart from the canvas without the canvas losing a pixel; the
// blank space around them drags the panel, exactly like the capture overlay's
// FloatingToolbar.
class AnnotateSurface::Toolbar final : public QWidget {
public:
    explicit Toolbar(AnnotateSurface *surface)
        : QWidget(surface)
        , surface_(surface)
    {
        setObjectName(QStringLiteral("vshotAnnotateToolbar"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAutoFillBackground(false);
        // Blank panel areas are the grip; interactive children override.
        setCursor(Qt::SizeAllCursor);

        auto *layout = new FlowLayout(this);
        layout->setContentsMargins(kToolbarPad, kToolbarPadY, kToolbarPad, kToolbarPadY);
        layout->setSpacing(4);
        layout->addWidget(new Grip(this));
        layout->addSpacing(4);

        addTool(layout, uiTr("Draw"), AnnotateSurface::Tool::Pen);
        addTool(layout, uiTr("Erase"), AnnotateSurface::Tool::Eraser);
        addTool(layout, uiTr("Rect"), AnnotateSurface::Tool::Rect);
        addTool(layout, uiTr("Arrow"), AnnotateSurface::Tool::Arrow);
        addTool(layout, uiTr("Line"), AnnotateSurface::Tool::Line);
        addTool(layout, uiTr("Wave"), AnnotateSurface::Tool::Wave);
        addTool(layout, uiTr("Bezier"), AnnotateSurface::Tool::Bezier);
        addTool(layout, uiTr("Text"), AnnotateSurface::Tool::Text);
        addNumberTool(layout);

        addDivider(layout);
        // The palette's order is the requirement's: the default red first, then
        // the two most legible colours on a dark panel, then black and white.
        static const QColor palette[] = {
            QColor(0xe5, 0x39, 0x35), QColor(0xfd, 0xd8, 0x35), QColor(0x43, 0xa0, 0x47),
            QColor(0x1e, 0x88, 0xe5), QColor(0x00, 0x00, 0x00), QColor(0xff, 0xff, 0xff),
        };
        for (const QColor &color : palette) {
            addSwatch(layout, color);
        }

        addAlpha(layout);

        addDivider(layout);
        for (const int width : kWidths) {
            addWidth(layout, width);
        }

        addDivider(layout);
        auto *undo = addText(layout, uiTr("Undo"), [this] { surface_->undo(); });
        auto *redo = addText(layout, uiTr("Redo"), [this] { surface_->redo(); });
        undo_ = undo;
        redo_ = redo;
        addText(layout, uiTr("Clear"), [this] { surface_->clear(); });

        addDivider(layout);
        auto *quit = new ToolbarButton(this, ToolbarButton::Kind::Cross, QString(), QColor(), 0);
        quit->setToolTip(uiTr("Quit annotation"));
        quit->setOnClick([this] {
            if (surface_->quit_) {
                surface_->quit_();
            }
        });
        layout->addWidget(quit);

        // The panel's size follows the output's width, so it is settled in
        // updateSize() rather than fixed here; this sizes it before the first
        // reposition() finds it a place.
        updateSize();
        syncState();
    }

    void syncState()
    {
        for (const auto &entry : toolButtons_) {
            entry.first->setActive(surface_->tool() == entry.second);
        }
        // RGB only: a swatch carries no alpha, so a translucent version of a
        // palette colour is that colour and its ring must stay lit.
        for (const auto &entry : swatchButtons_) {
            entry.first->setActive(surface_->color().rgb() == entry.second.rgb());
        }
        for (const auto &entry : widthButtons_) {
            entry.first->setActive(surface_->penWidth() == entry.second);
        }
        if (numberButton_ != nullptr) {
            // The button carries two things at once: whether the number tool is
            // armed, and which badge it would place.
            numberButton_->setActive(surface_->tool() == AnnotateSurface::Tool::Number);
            numberButton_->setNumberStyle(surface_->numberStyle());
            numberButton_->setToolTip(
                uiTr("Number: %1 (click again to change the style)")
                    .arg(numberStyleName(surface_->numberStyle())));
        }
        if (alphaSlider_ != nullptr) {
            // The colour can also be set from outside the toolbar; the slider
            // follows it without echoing back as an edit.
            const QSignalBlocker blocker(alphaSlider_);
            alphaSlider_->setValue(surface_->color().alpha());
        }
        if (alphaValue_ != nullptr) {
            alphaValue_->setText(QStringLiteral("%1%").arg(
                qRound(surface_->color().alpha() * 100.0 / 255.0)));
        }
        if (undo_ != nullptr) {
            undo_->setEnabled(surface_->canUndo());
        }
        if (redo_ != nullptr) {
            redo_->setEnabled(surface_->canRedo());
        }
    }

    // Puts the panel where it belongs: the last dragged position when there is
    // one, otherwise centred under the output's top edge.  Called on every
    // resize, so a dragged panel is pulled back inside a shrunken output.
    void reposition()
    {
        // A resize changes both how wide the panel may be and how many rows it
        // needs, so the size is settled before the place is.
        updateSize();
        if (surface_->toolbarOrigin_.isNull()) {
            const QPoint centered((surface_->width() - width()) / 2, kToolbarMargin);
            move(clamped(centered));
        } else {
            move(clamped(surface_->toolbarOrigin_));
        }
    }

    // Sizes the panel to the width the output can give it and lets the flow
    // layout say how many rows that takes.  The row of items is allowed the
    // output's whole width: an output wide enough to hold it keeps the palette
    // on one row -- every normal output, and the look the palette has always
    // had -- and only an output narrower than the row itself makes it wrap.  A
    // wrapped panel then backs off the output's edges by the toolbar margin, so
    // two rows read as a panel rather than as a band across the screen.
    void updateSize()
    {
        const int output = std::max(1, surface_->width());
        const int natural = layout()->sizeHint().width();
        const int width = natural <= output
            ? natural
            : std::max(1, output - 2 * AnnotateSurface::kToolbarMargin);
        setFixedSize(width, layout()->heightForWidth(width));
        // A layout activates through a posted event, which an off-screen
        // surface (the checks render one, they never show it) never gets; place
        // the rows now so the panel's size and its rows always agree.
        layout()->setGeometry(contentsRect());
    }

    void beginDrag(const QPoint &globalPos)
    {
        dragging_ = true;
        dragOffset_ = globalPos - mapToGlobal(QPoint(0, 0));
    }

    void dragTo(QMouseEvent *event)
    {
        if (!dragging_ || !(event->buttons() & Qt::LeftButton)) {
            return;
        }
        const QPoint target = event->globalPosition().toPoint() - dragOffset_;
        const QPoint placed = clamped(surface_->mapFromGlobal(target));
        surface_->toolbarOrigin_ = placed;
        move(placed);
    }

    void endDrag() { dragging_ = false; }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        if (childAt(event->position().toPoint()) == nullptr) {
            beginDrag(event->globalPosition().toPoint());
        }
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (dragging_ && (event->buttons() & Qt::LeftButton)) {
            dragTo(event);
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        endDrag();
        event->accept();
    }

    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // A plain rim, no frosted glass and no drop shadow: frosted glass needs
        // a frozen frame (there is none over a live desktop), and a child widget
        // cannot paint outside its own rect, so a shadow would be clipped.
        const QPainterPath shape =
            superellipsePath(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), kPanelRadius,
                             kSuperellipseExponent);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(30, 34, 41, 204));
        painter.drawPath(shape);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(64, 71, 82), 1.0));
        painter.drawPath(shape);
    }

private:
    // The dotted drag handle.  It is its own widget only so it can carry the
    // "Drag to move" tooltip and start the drag itself; it paints nothing but
    // its dots, letting the panel's rim show through.
    class Grip final : public QWidget {
    public:
        explicit Grip(Toolbar *toolbar)
            : QWidget(toolbar)
            , toolbar_(toolbar)
        {
            setToolTip(uiTr("Drag to move"));
            setCursor(Qt::SizeAllCursor);
            setFixedWidth(kGripWidth);
            setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        }

        // A box layout stretches the grip to the row it sits in, but the flow
        // layout measures a widget by its hint: without one, the grip would
        // measure as a fixed width with no height and vanish from its row.
        QSize sizeHint() const override { return QSize(kGripWidth, kButtonHeight); }

    protected:
        void mousePressEvent(QMouseEvent *event) override
        {
            if (event->button() != Qt::LeftButton) {
                QWidget::mousePressEvent(event);
                return;
            }
            toolbar_->beginDrag(event->globalPosition().toPoint());
            event->accept();
        }

        void mouseMoveEvent(QMouseEvent *event) override { toolbar_->dragTo(event); }

        void mouseReleaseEvent(QMouseEvent *event) override
        {
            if (event->button() == Qt::LeftButton) {
                toolbar_->endDrag();
                event->accept();
                return;
            }
            QWidget::mouseReleaseEvent(event);
        }

        void paintEvent(QPaintEvent *event) override
        {
            Q_UNUSED(event);
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(150, 157, 168));
            const QPointF center = QRectF(rect()).center();
            for (int row = -1; row <= 1; ++row) {
                for (int column = -1; column <= 1; ++column) {
                    painter.drawEllipse(
                        QPointF(center.x() + column * 5.0, center.y() + row * 5.0), 1.4, 1.4);
                }
            }
        }

    private:
        Toolbar *toolbar_;
    };

    QPoint clamped(const QPoint &point) const
    {
        const int maxX = std::max(0, surface_->width() - width());
        const int maxY = std::max(0, surface_->height() - height());
        return QPoint(std::clamp(point.x(), 0, maxX), std::clamp(point.y(), 0, maxY));
    }

    void addTool(FlowLayout *layout, const QString &label, AnnotateSurface::Tool tool)
    {
        auto *button = new ToolbarButton(this, ToolbarButton::Kind::Label, label, QColor(), 0);
        button->setAccessibleName(label);
        button->setOnClick([this, tool] { surface_->setTool(tool); });
        layout->addWidget(button);
        toolButtons_.append(qMakePair(button, tool));
    }

    // The number tool's single button.  Its first click arms the tool; a click
    // on the already-armed button cycles the badge style instead, because the
    // palette is long already and four buttons for one tool would push the row
    // onto a second line.  The thumbnail on the button is the style that is
    // armed, and the tooltip names it.
    void addNumberTool(FlowLayout *layout)
    {
        auto *button =
            new ToolbarButton(this, ToolbarButton::Kind::Number, QString(), QColor(), 0);
        button->setAccessibleName(uiTr("Number"));
        button->setOnClick([this] {
            if (surface_->tool() == AnnotateSurface::Tool::Number) {
                surface_->setNumberStyle(nextNumberStyle(surface_->numberStyle()));
            } else {
                surface_->setTool(AnnotateSurface::Tool::Number);
            }
        });
        layout->addWidget(button);
        numberButton_ = button;
    }

    void addSwatch(FlowLayout *layout, const QColor &color)
    {
        auto *button =
            new ToolbarButton(this, ToolbarButton::Kind::Swatch, QString(), color, 0);
        // A swatch has no alpha of its own: it changes the RGB and keeps the
        // opacity the user already chose.
        button->setOnClick([this, color] {
            QColor chosen = color;
            chosen.setAlpha(surface_->color().alpha());
            surface_->setColor(chosen);
        });
        layout->addWidget(button);
        swatchButtons_.append(qMakePair(button, color));
    }

    // The opacity control, drawn beside the colours because opacity is a
    // property of the colour being drawn with.  The range is the storage range
    // (0-255); the value on screen is a percentage, which is what a user thinks
    // in.  There is no "Alpha" caption: the panel is a compact row of controls,
    // so the tooltip and the accessible name carry the word instead.
    void addAlpha(FlowLayout *layout)
    {
        alphaSlider_ = new QSlider(Qt::Horizontal, this);
        alphaSlider_->setRange(0, 255);
        alphaSlider_->setValue(surface_->color().alpha());
        alphaSlider_->setFixedWidth(36);
        alphaSlider_->setFocusPolicy(Qt::NoFocus);
        alphaSlider_->setCursor(Qt::PointingHandCursor);
        alphaSlider_->setToolTip(uiTr("Annotation opacity"));
        alphaSlider_->setAccessibleName(uiTr("Annotation opacity"));
        // The panel is self-painted and carries no global stylesheet, so the
        // slider is styled here in the panel's own colours.
        alphaSlider_->setStyleSheet(QStringLiteral(
            "QSlider { min-height: 20px; background: transparent; } "
            "QSlider::groove:horizontal { height: 4px; background: #4a515c; "
            "border-radius: 2px; } "
            "QSlider::sub-page:horizontal { background: #dde1ff; border-radius: 2px; } "
            "QSlider::add-page:horizontal { background: #4a515c; border-radius: 2px; } "
            "QSlider::handle:horizontal { width: 12px; margin: -5px 0; "
            "background: #dde1ff; border: 0; border-radius: 6px; } "
            "QSlider::handle:horizontal:hover { background: #e9ecff; }"));
        connect(alphaSlider_, &QSlider::valueChanged, this, [this](int value) {
            QColor color = surface_->color();
            color.setAlpha(value);
            surface_->setColor(color);
        });
        layout->addWidget(alphaSlider_);
        alphaValue_ = new QLabel(this);
        alphaValue_->setObjectName(QStringLiteral("alphaValue"));
        alphaValue_->setStyleSheet(QStringLiteral("color: #e6e1e5; font-size: 10px;"));
        alphaValue_->setFixedWidth(26);
        alphaValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(alphaValue_);
    }

    void addWidth(FlowLayout *layout, int width)
    {
        // The dot's diameter is the width made visible, so the three dots read
        // as thin, medium and thick before they are clicked.
        auto *button = new ToolbarButton(this, ToolbarButton::Kind::Width, QString(), QColor(),
                                         width + 5);
        button->setOnClick([this, width] { surface_->setPenWidth(width); });
        layout->addWidget(button);
        widthButtons_.append(qMakePair(button, width));
    }

    ToolbarButton *addText(FlowLayout *layout, const QString &label,
                           std::function<void()> onClick)
    {
        auto *button = new ToolbarButton(this, ToolbarButton::Kind::Label, label, QColor(), 0);
        button->setAccessibleName(label);
        button->setOnClick(std::move(onClick));
        layout->addWidget(button);
        return button;
    }

    void addDivider(FlowLayout *layout)
    {
        auto *line = new QFrame(this);
        line->setFixedSize(1, 20);
        line->setStyleSheet(QStringLiteral("background: #3a414b; border: none;"));
        // Transparent to the pointer: a click on the separator is a click on
        // blank panel, and drags the panel like any other gap.
        line->setAttribute(Qt::WA_TransparentForMouseEvents);
        // Marks it for the flow layout: on one row the separator is just a
        // divider, but where the row wraps it travels with the group it opens
        // instead of being left dangling at the end of a row.
        line->setProperty(kSeparatorProperty, true);
        layout->addWidget(line);
    }

    AnnotateSurface *surface_ = nullptr;
    QVector<QPair<ToolbarButton *, AnnotateSurface::Tool>> toolButtons_;
    QVector<QPair<ToolbarButton *, QColor>> swatchButtons_;
    QVector<QPair<ToolbarButton *, int>> widthButtons_;
    ToolbarButton *numberButton_ = nullptr;
    ToolbarButton *undo_ = nullptr;
    ToolbarButton *redo_ = nullptr;
    QSlider *alphaSlider_ = nullptr;
    QLabel *alphaValue_ = nullptr;
    bool dragging_ = false;
    QPoint dragOffset_;
};

// The inline text editor.  Enter commits, Escape cancels, losing focus commits.
// No input validator: an ASCII validator here would silently drop input-method
// commits, and a Chinese label has to survive entry.
class AnnotateTextEdit final : public QLineEdit {
public:
    using Finished = std::function<void(bool)>;

    AnnotateTextEdit(QWidget *parent, Finished finished)
        : QLineEdit(parent)
        , finished_(std::move(finished))
    {
        setAttribute(Qt::WA_DeleteOnClose, false);
        setFrame(true);
        setPlaceholderText(uiTr("Text"));
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

    void focusOutEvent(QFocusEvent *event) override
    {
        QLineEdit::focusOutEvent(event);
        if (finished_) {
            finished_(true);
        }
    }

private:
    Finished finished_;
};

AnnotateSurface::AnnotateSurface(QScreen *screen)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint)
    , screen_(screen)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setCursor(Qt::CrossCursor);
    if (screen_ != nullptr) {
        // One surface per output, covering all of it.  The compositor confirms
        // this through the layer configure; resizing here only avoids a
        // wrong-size first frame.
        resize(screen_->geometry().size());
    }
    toolbar_ = new Toolbar(this);
    toolbar_->reposition();
    toolbar_->show();
}

AnnotateSurface::~AnnotateSurface() = default;

bool AnnotateSurface::showLayerSurface()
{
    winId();
    QWindow *window = windowHandle();
    if (window == nullptr) {
        return false;
    }
    auto *layer = LayerShellQt::Window::get(window);
    if (layer == nullptr) {
        return false;
    }
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    // All four edges anchored with zero margins: the canvas is the whole output,
    // so nothing about the annotation ever involves a compositor round trip.
    LayerShellQt::Window::Anchors anchors(LayerShellQt::Window::AnchorTop);
    anchors |= LayerShellQt::Window::AnchorBottom;
    anchors |= LayerShellQt::Window::AnchorLeft;
    anchors |= LayerShellQt::Window::AnchorRight;
    layer->setAnchors(anchors);
    layer->setExclusiveZone(-1);
    // No keyboard to begin with.  This surface covers every window on the
    // output, and a surface that held the keyboard would stop the user typing
    // in whatever is underneath; drawing a stroke needs no keys.  Only the
    // text editor raises it, through setKeyboardWanted().
    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
    layer_ = layer;
    keyboardWanted_ = false;
    layer->setScope(QStringLiteral("vshot-annotate"));
    layer->setDesiredSize(QSize(0, 0)); // follow the anchored edges
    layer->setScreen(screen_);
    surfaceReady_ = true;
    show();
    return true;
}

void AnnotateSurface::setTool(Tool tool)
{
    if (tool_ == tool) {
        return;
    }
    if (drawing_ && pending_.tool == Tool::Bezier && tool != Tool::Bezier) {
        // An unfinished pen path goes with the tool: it is one gesture rather
        // than a drawing that outlives a tool change, and leaving it in
        // `pending_` would have the next press extend a path nobody is looking
        // at any more.
        const QRect dirty = previewRect();
        drawing_ = false;
        pending_ = Stroke();
        touch(dirty);
    }
    tool_ = tool;
    if (toolbar_ != nullptr) {
        toolbar_->syncState();
    }
}

void AnnotateSurface::setColor(const QColor &color)
{
    color_ = color;
    if (toolbar_ != nullptr) {
        toolbar_->syncState();
    }
}

void AnnotateSurface::setPenWidth(int width)
{
    // Clamped to the range the capture editor's own width control uses, so a
    // value arriving from outside the toolbar cannot shrink a stroke to
    // nothing or blow it up past a usable line.
    width_ = std::clamp(width, 1, 64);
    if (toolbar_ != nullptr) {
        toolbar_->syncState();
    }
}

void AnnotateSurface::setNumberStyle(NumberStyle style)
{
    if (numberStyle_ == style) {
        return;
    }
    numberStyle_ = style;
    if (toolbar_ != nullptr) {
        toolbar_->syncState();
    }
}

int AnnotateSurface::strokeCount() const
{
    return static_cast<int>(strokes_.size());
}

int AnnotateSurface::strokeNumber(int index) const
{
    if (index < 0 || index >= strokes_.size()) {
        return 0;
    }
    const Stroke &stroke = strokes_.at(index);
    return stroke.tool == Tool::Number ? stroke.number : 0;
}

int AnnotateSurface::strokeAnchorCount(int index) const
{
    if (index < 0 || index >= strokes_.size()) {
        return 0;
    }
    const Stroke &stroke = strokes_.at(index);
    return stroke.tool == Tool::Bezier ? bezierAnchors(stroke.points) : 0;
}

bool AnnotateSurface::isEmpty() const
{
    return strokes_.isEmpty();
}

bool AnnotateSurface::canUndo() const
{
    return !undoStack_.isEmpty();
}

bool AnnotateSurface::canRedo() const
{
    return !redoStack_.isEmpty();
}

QRect AnnotateSurface::toolbarRect() const
{
    if (toolbarHidden_ || toolbar_ == nullptr) {
        return QRect();
    }
    return toolbar_->geometry();
}

void AnnotateSurface::setToolbarHidden(bool hidden)
{
    if (toolbarHidden_ == hidden) {
        return;
    }
    toolbarHidden_ = hidden;
    if (toolbar_ == nullptr) {
        return;
    }
    const QRect rect = toolbar_->geometry();
    toolbar_->setVisible(!hidden);
    // Only the panel's rect: hiding it must not repaint the canvas, and showing
    // it must have the canvas underneath it repainted before the button glow.
    if (rect.isValid()) {
        update(rect);
    }
}

double AnnotateSurface::deviceRatio() const
{
    return screen_ != nullptr ? screen_->devicePixelRatio() : 1.0;
}

void AnnotateSurface::paintStroke(QPainter &painter, const Stroke &stroke) const
{
    paintStrokeInk(painter, stroke.tool, stroke.points, stroke.color, stroke.width, stroke.text,
                   stroke.numberStyle, stroke.number, stroke.closed);
}

const AnnotateSurface::StrokeRaster *AnnotateSurface::rasterFor(Stroke &stroke)
{
    if (stroke.tool == Tool::Eraser) {
        return nullptr;
    }
    const QRectF bounds =
        strokeBounds(stroke.tool, stroke.points, stroke.width, stroke.text, stroke.closed);
    if (bounds.isNull()) {
        return nullptr;
    }
    const double ratio = deviceRatio();
    if (stroke.raster != nullptr && stroke.raster->ratio == ratio) {
        return stroke.raster.get();
    }
    const QRect device = deviceDirtyRect(stroke);
    if (device.isEmpty()) {
        return nullptr;
    }
    auto raster = std::make_shared<StrokeRaster>();
    raster->logicalBounds = bounds;
    raster->origin = device.topLeft();
    raster->ratio = ratio;
    raster->image = QImage(device.size(), QImage::Format_ARGB32_Premultiplied);
    raster->image.fill(Qt::transparent);
    {
        QPainter painter(&raster->image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // Scaled by the device ratio and shifted by whole device pixels, so the
        // ink lands on exactly the pixels it landed on when every stroke was
        // drawn into one canvas: same scale, same antialiasing phase.
        painter.setTransform(QTransform(ratio, 0.0, 0.0, ratio,
                                        -static_cast<qreal>(device.x()),
                                        -static_cast<qreal>(device.y())));
        paintStroke(painter, stroke);
    }
    ++rasterBuilds_;
    stroke.raster = raster;
    return stroke.raster.get();
}

QRect AnnotateSurface::deviceDirtyRect(const Stroke &stroke) const
{
    const QRectF box =
        strokeBounds(stroke.tool, stroke.points, stroke.width, stroke.text, stroke.closed);
    if (box.isNull()) {
        return QRect();
    }
    const double ratio = deviceRatio();
    const int left = static_cast<int>(std::floor(box.left() * ratio)) - 1;
    const int top = static_cast<int>(std::floor(box.top() * ratio)) - 1;
    const int right = static_cast<int>(std::ceil(box.right() * ratio)) + 1;
    const int bottom = static_cast<int>(std::ceil(box.bottom() * ratio)) + 1;
    return QRect(QPoint(left, top), QPoint(right, bottom));
}

void AnnotateSurface::touch(const QRect &logical)
{
    if (!logical.isValid()) {
        return;
    }
    // Accumulated as well as sent: the checks compare one interactive step's
    // repaint region against the pixels that step changed.
    invalidated_ = invalidated_.united(logical);
    update(logical);
}

bool AnnotateSurface::strokeHits(const Stroke &stroke, const QPointF &local) const
{
    if (stroke.tool == Tool::Text) {
        return rectOutlineDistance(local, textBounds(stroke.points, stroke.width, stroke.text))
            <= kEraserRadius;
    }
    if (stroke.tool == Tool::Number) {
        if (stroke.points.isEmpty()) {
            return false;
        }
        // A badge is a solid mark: the eraser takes it from anywhere in its box,
        // not only from the outline, which is what a click on the disc does.
        const QRectF box = numberBadgeRect(stroke.points.constFirst(), stroke.width);
        return box.adjusted(-kEraserRadius, -kEraserRadius, kEraserRadius, kEraserRadius)
            .contains(local);
    }
    if (stroke.points.isEmpty()) {
        return false;
    }
    if (stroke.tool == Tool::Rect && stroke.points.size() >= 2) {
        return rectOutlineDistance(
                   local, normalizedRect(stroke.points.constFirst(), stroke.points.constLast()))
            <= kEraserRadius;
    }
    if (stroke.tool == Tool::Wave && stroke.points.size() >= 2) {
        // The ink of a wave is the sampled polyline, not the straight line
        // between its two points: an eraser landing on a crest has to take the
        // stroke, so the distance is measured to the wave the painter draws.
        const QVector<QPointF> wave =
            wavePolyline(stroke.points.constFirst(), stroke.points.constLast(),
                         waveAmplitude(stroke.width), waveWavelength(stroke.width), 1.0);
        double nearest = std::numeric_limits<double>::infinity();
        for (qsizetype i = 1; i < wave.size(); ++i) {
            nearest = std::min(nearest,
                               pointSegmentDistance(local, wave.at(i - 1), wave.at(i)));
        }
        if (wave.size() == 1) {
            nearest = QLineF(local, wave.constFirst()).length();
        }
        return nearest <= kEraserRadius;
    }
    if (stroke.tool == Tool::Bezier) {
        // A closed path is a solid mark: its fill reaches the inside, so an
        // eraser landing there has to take it, exactly as it does for a badge.
        if (stroke.closed && bezierAnchors(stroke.points) >= 2 &&
            bezierPath(stroke.points, true).contains(local)) {
            return true;
        }
        // Otherwise the ink is the sampled curve, not the anchors: an eraser
        // landing on a bulge the handles pulled out has to take the stroke.
        const QVector<QPointF> curve = bezierPolyline(stroke.points, stroke.closed);
        double nearest = std::numeric_limits<double>::infinity();
        if (curve.size() == 1) {
            nearest = QLineF(local, curve.constFirst()).length();
        }
        for (qsizetype i = 1; i < curve.size(); ++i) {
            nearest =
                std::min(nearest, pointSegmentDistance(local, curve.at(i - 1), curve.at(i)));
        }
        return nearest <= kEraserRadius;
    }
    double nearest = std::numeric_limits<double>::infinity();
    if (stroke.points.size() == 1) {
        nearest = QLineF(local, stroke.points.constFirst()).length();
    } else {
        for (qsizetype i = 1; i < stroke.points.size(); ++i) {
            nearest = std::min(
                nearest,
                pointSegmentDistance(local, stroke.points.at(i - 1), stroke.points.at(i)));
        }
        if (stroke.tool == Tool::Arrow) {
            // The filled head is ink too: take a stroke the eraser lands on the
            // head of, not just one it lands on the shaft of.
            const QVector<QPointF> head = arrowHeadPoints(
                stroke.points.constLast(), stroke.points.at(stroke.points.size() - 2),
                stroke.width);
            if (head.size() == 3) {
                if (pointInTriangle(local, head.at(0), head.at(1), head.at(2))) {
                    return true;
                }
                nearest = std::min(
                    {nearest, pointSegmentDistance(local, head.at(0), head.at(1)),
                     pointSegmentDistance(local, head.at(1), head.at(2)),
                     pointSegmentDistance(local, head.at(2), head.at(0))});
            }
        }
    }
    return nearest <= kEraserRadius;
}

bool AnnotateSurface::eraseStrokeAt(const QPointF &local)
{
    // Frontmost first: the ink on top is the ink the eraser would visibly take.
    for (qsizetype i = strokes_.size(); i-- > 0;) {
        const Stroke &stroke = strokes_.at(i);
        if (!strokeHits(stroke, local)) {
            continue;
        }
        const QRectF box =
            strokeBounds(stroke.tool, stroke.points, stroke.width, stroke.text, stroke.closed);
        const QRect dirty = grownDirtyRect(box);
        pushHistory(dirty);
        // Dropping the stroke out of the list is the whole change: every other
        // stroke owns its own cached ink, so nothing else has to be rebuilt.
        strokes_.removeAt(i);
        touch(dirty);
        if (toolbar_ != nullptr) {
            toolbar_->syncState();
        }
        return true;
    }
    return false;
}

void AnnotateSurface::pushHistory(const QRect &dirty)
{
    undoStack_.append(HistoryEntry{strokes_, dirty});
    // Any fresh change makes the redo branch unreachable.
    redoStack_.clear();
}

void AnnotateSurface::undo()
{
    if (undoStack_.isEmpty()) {
        return;
    }
    const HistoryEntry entry = undoStack_.takeLast();
    // The state being replaced keeps the entry's rect: the region that differs
    // is the same in both directions.
    redoStack_.append(HistoryEntry{strokes_, entry.dirty});
    strokes_ = entry.strokes;
    touch(entry.dirty);
    if (toolbar_ != nullptr) {
        toolbar_->syncState();
    }
}

void AnnotateSurface::redo()
{
    if (redoStack_.isEmpty()) {
        return;
    }
    const HistoryEntry entry = redoStack_.takeLast();
    // Symmetric to undo: the entry's rect is the region that has to be
    // repainted, whichever way the step goes.
    undoStack_.append(HistoryEntry{strokes_, entry.dirty});
    strokes_ = entry.strokes;
    touch(entry.dirty);
    if (toolbar_ != nullptr) {
        toolbar_->syncState();
    }
}

void AnnotateSurface::clear()
{
    if (strokes_.isEmpty()) {
        return;
    }
    QRect dirty;
    for (const Stroke &stroke : strokes_) {
        dirty = dirty.united(grownDirtyRect(
            strokeBounds(stroke.tool, stroke.points, stroke.width, stroke.text, stroke.closed)));
    }
    pushHistory(dirty);
    strokes_.clear();
    touch(dirty);
    if (toolbar_ != nullptr) {
        toolbar_->syncState();
    }
}

void AnnotateSurface::setKeyboardWanted(bool wanted)
{
    if (layer_ == nullptr || keyboardWanted_ == wanted) {
        return;
    }
    keyboardWanted_ = wanted;
    // Exclusive while a label is being typed, none otherwise: the change only
    // reaches the compositor with the next commit, which requestUpdate
    // schedules so the keyboard arrives (or leaves) now rather than at some
    // later repaint.
    layer_->setKeyboardInteractivity(wanted
                                         ? LayerShellQt::Window::KeyboardInteractivityExclusive
                                         : LayerShellQt::Window::KeyboardInteractivityNone);
    if (QWindow *window = windowHandle()) {
        window->requestUpdate();
    }
}

void AnnotateSurface::beginText(const QPointF &local)
{
    const QFont font = annotationFont(width_);
    const QFontMetrics metrics(font);
    const int boxWidth = std::min(360, std::max(160, width() - 16));
    const int boxHeight = std::max(20, metrics.height() + 6);
    const int x = std::clamp(static_cast<int>(std::lround(local.x())), 4,
                             std::max(4, width() - boxWidth - 4));
    const int y = std::clamp(static_cast<int>(std::lround(local.y())), 4,
                             std::max(4, height() - boxHeight - 4));
    textOrigin_ = local;
    if (textEdit_ != nullptr) {
        // Only one label is being typed at a time: a second click moves the box
        // instead of opening another.
        textEdit_->setFont(font);
        textEdit_->setGeometry(x, y, boxWidth, boxHeight);
        textEdit_->setFocus(Qt::OtherFocusReason);
        return;
    }
    textEdit_ = new AnnotateTextEdit(this, [this](bool accept) { finishText(accept); });
    textEdit_->setFont(font);
    textEdit_->setStyleSheet(QStringLiteral("QLineEdit { color: %1; background: rgba(0, 0, 0, 140); "
                                            "border: 1px solid #888888; padding: 0 3px; }")
                                 .arg(color_.name()));
    textEdit_->setGeometry(x, y, boxWidth, boxHeight);
    textEdit_->show();
    textEdit_->raise();
    // Ask for the keyboard before focusing, or the compositor never gives this
    // surface the keys the editor needs.
    setKeyboardWanted(true);
    textEdit_->setFocus(Qt::OtherFocusReason);
}

void AnnotateSurface::finishText(bool accept)
{
    if (textEdit_ == nullptr) {
        return;
    }
    const QString value = textEdit_->text();
    QLineEdit *editor = textEdit_;
    // Cleared before the editor is hidden: hiding fires focus-out, whose
    // commit would otherwise re-enter this function.
    textEdit_ = nullptr;
    editor->hide();
    editor->deleteLater();
    setKeyboardWanted(false);
    if (!accept || value.isEmpty()) {
        return;
    }
    Stroke stroke;
    stroke.tool = Tool::Text;
    stroke.color = color_;
    stroke.width = width_;
    stroke.points = {textOrigin_};
    stroke.text = value;
    const QRect dirty = grownDirtyRect(strokeBounds(stroke.tool, stroke.points, stroke.width,
                                                    stroke.text));
    pushHistory(dirty);
    // The label's ink is built on the next paint by `rasterFor`, like any other
    // stroke's.
    strokes_.append(stroke);
    touch(dirty);
    if (toolbar_ != nullptr) {
        toolbar_->syncState();
    }
}

void AnnotateSurface::placeNumber(const QPointF &local)
{
    Stroke stroke;
    stroke.tool = Tool::Number;
    stroke.color = color_;
    stroke.width = width_;
    stroke.points = {local};
    stroke.numberStyle = numberStyle_;
    // One past the highest badge already on the canvas, read fresh every time.
    // Undoing a badge therefore hands its number back to the next click, and
    // there is no counter that could drift out of step with the list.
    int highest = 0;
    for (const Stroke &existing : strokes_) {
        highest = std::max(highest, existing.number);
    }
    stroke.number = highest + 1;
    const QRect dirty =
        grownDirtyRect(strokeBounds(stroke.tool, stroke.points, stroke.width, stroke.text));
    pushHistory(dirty);
    // The ink is built on the next paint by `rasterFor`, like any other stroke's.
    strokes_.append(stroke);
    touch(dirty);
    if (toolbar_ != nullptr) {
        toolbar_->syncState();
    }
}

QRect AnnotateSurface::previewRect() const
{
    QRectF box = strokeBounds(pending_.tool, pending_.points, pending_.width, pending_.text,
                              pending_.closed);
    if (pending_.tool == Tool::Bezier && pending_.points.size() >= 2) {
        // The rubber band is drawn from the path's last anchor to the pointer,
        // so the region a move has to repaint is the path plus that segment.  A
        // pointer sitting exactly on the anchor leaves a zero-length band, which
        // is not ink and is skipped -- including it would only inflate the rect.
        const QLineF band(pending_.points.at(pending_.points.size() - 2), bezierCursor_);
        if (band.length() > 0.0) {
            const qreal grow = pending_.width / 2.0 + 1.0;
            box = box.united(QRectF(band.p1(), band.p2())
                                 .normalized()
                                 .adjusted(-grow, -grow, grow, grow));
        }
    }
    return grownDirtyRect(box);
}

void AnnotateSurface::commitBezier(const QRect &stale)
{
    Stroke committed = pending_;
    pending_ = Stroke();
    if (committed.points.isEmpty()) {
        return;
    }
    QRect dirty = grownDirtyRect(strokeBounds(committed.tool, committed.points, committed.width,
                                              committed.text, committed.closed));
    if (stale.isValid()) {
        // The preview may have covered a rubber band the committed path does
        // not: the band has to be painted over, not left behind it.
        dirty = dirty.united(stale);
    }
    pushHistory(dirty);
    // The ink is built on the next paint by `rasterFor`, like any other
    // stroke's.
    strokes_.append(committed);
    touch(dirty);
    if (toolbar_ != nullptr) {
        toolbar_->syncState();
    }
}

void AnnotateSurface::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    // Clear the repainted region to transparent first: the strokes are blitted
    // with SourceOver, and apart from its ink this surface is see-through.
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(event->rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    // Each stroke is a device-pixel image blitted at 1:1, so smoothing would
    // only cost time and soften the ink.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    // A repaint narrowed to what a gesture touched must not pay for the strokes
    // outside it.  The clip is in this widget's logical coordinates, the same
    // ones a stroke's bounds are in.
    const QRectF exposed = painter.hasClipping() ? painter.clipBoundingRect() : QRectF(rect());
    for (Stroke &stroke : strokes_) {
        const StrokeRaster *raster = rasterFor(stroke);
        if (raster == nullptr || !raster->logicalBounds.intersects(exposed)) {
            continue;
        }
        const double ratio = raster->ratio;
        const QRectF target(static_cast<qreal>(raster->origin.x()) / ratio,
                            static_cast<qreal>(raster->origin.y()) / ratio,
                            static_cast<qreal>(raster->image.width()) / ratio,
                            static_cast<qreal>(raster->image.height()) / ratio);
        painter.drawImage(target, raster->image, QRectF(raster->image.rect()));
    }
    // The stroke being drawn lives only in `pending_`: the preview is the one
    // thing this surface paints from the model rather than from cached ink, and
    // every tool goes through it now.
    if (drawing_) {
        painter.setRenderHint(QPainter::Antialiasing, true);
        paintStroke(painter, pending_);
        // The pen's path is built from a run of clicks, so between two of them
        // the preview also shows where the next segment would reach: a rubber
        // band from the last anchor to the pointer.
        if (pending_.tool == Tool::Bezier && pending_.points.size() >= 2) {
            const QLineF band(pending_.points.at(pending_.points.size() - 2), bezierCursor_);
            if (band.length() > 0.0) {
                painter.setPen(QPen(pending_.color, pending_.width, Qt::SolidLine, Qt::RoundCap,
                                    Qt::RoundJoin));
                painter.setBrush(Qt::NoBrush);
                painter.drawLine(band);
            }
        }
    }
}

void AnnotateSurface::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPointF local = event->position();
    // A click on the canvas commits any open label first.  The editor is a
    // child widget, so a click inside it never reaches here at all.
    if (textEdit_ != nullptr) {
        finishText(true);
    }
    switch (tool_) {
    case Tool::Pen:
        pending_ = Stroke();
        pending_.tool = Tool::Pen;
        pending_.color = color_;
        pending_.width = width_;
        pending_.points = {local};
        drawing_ = true;
        // The preview paints the first dot straight away; nothing is committed
        // until the button comes up.
        touch(grownDirtyRect(
            strokeBounds(pending_.tool, pending_.points, pending_.width, pending_.text)));
        break;
    case Tool::Rect:
    case Tool::Arrow:
    case Tool::Line:
    case Tool::Wave:
        // The two-point tools: the drag is recorded as its anchor and its
        // current point, never the wandering positions in between.
        pending_ = Stroke();
        pending_.tool = tool_;
        pending_.color = color_;
        pending_.width = width_;
        pending_.points = {local, local};
        drawing_ = true;
        break;
    case Tool::Bezier: {
        // The pen is the one tool whose gesture is not a single drag: every
        // press adds an anchor, and the drag that follows bends the segment
        // arriving at it.
        const QRect stale = drawing_ ? previewRect() : QRect();
        const double reach = std::max(8.0, width_ * 2.0);
        if (drawing_ && pending_.tool == Tool::Bezier && bezierAnchors(pending_.points) >= 2 &&
            QLineF(pending_.points.constFirst(), local).length() <= reach) {
            // A press back onto the first anchor closes the path.  It is
            // committed here rather than on a release: the click is the whole
            // closing gesture, and waiting for the button to come up would leave
            // the filled shape hanging on a press the user already made.
            pending_.closed = true;
            drawing_ = false;
            commitBezier(stale);
            break;
        }
        if (!drawing_) {
            pending_ = Stroke();
            pending_.tool = Tool::Bezier;
            pending_.color = color_;
            pending_.width = width_;
            drawing_ = true;
        }
        // The anchor, and its outgoing handle starting on top of it: the drag
        // that follows pulls the handle out, and the incoming side is its
        // mirror, so the handle is symmetric by construction.
        pending_.points.append(local);
        pending_.points.append(local);
        bezierCursor_ = local;
        QRect dirty = previewRect();
        if (stale.isValid()) {
            dirty = dirty.united(stale);
        }
        touch(dirty);
        break;
    }
    case Tool::Text:
        beginText(local);
        break;
    case Tool::Number:
        // A numbered badge is placed on the press and never dragged: the whole
        // gesture is the click, so nothing waits for a release.
        placeNumber(local);
        break;
    case Tool::Eraser:
        erasing_ = true;
        eraseFrom_ = local;
        eraseStrokeAt(local);
        break;
    }
}

void AnnotateSurface::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF local = event->position();
    if (erasing_ && (event->buttons() & Qt::LeftButton)) {
        // The eraser's own path is walked, not just its current point: a fast
        // drag sends few events, and a stroke between two of them must still be
        // taken.  Half the eraser's radius keeps the sampled discs overlapping.
        const QPointF from = eraseFrom_;
        const double distance = QLineF(from, local).length();
        const int steps =
            std::max(1, static_cast<int>(std::ceil(distance / (kEraserRadius * 0.5))));
        for (int i = 1; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps;
            eraseStrokeAt(QPointF(from.x() + (local.x() - from.x()) * t,
                                  from.y() + (local.y() - from.y()) * t));
        }
        eraseFrom_ = local;
        return;
    }
    if (!drawing_ || !(event->buttons() & Qt::LeftButton)) {
        // The pen's rubber band follows the pointer with the button up: the
        // path is built from a run of clicks, so the preview has to track the
        // pointer between them or the user cannot see where the next anchor
        // would land.
        if (drawing_ && pending_.tool == Tool::Bezier) {
            const QRect before = previewRect();
            bezierCursor_ = local;
            const QRect after = previewRect();
            if (after != before) {
                touch(before);
                touch(after);
            }
            return;
        }
        QWidget::mouseMoveEvent(event);
        return;
    }
    if (tool_ == Tool::Pen) {
        const QPointF from = pending_.points.constLast();
        pending_.points.append(local);
        // The preview paints the whole polyline from the model, so only the part
        // this step can have changed needs repainting.
        touch(grownDirtyRect(
            normalizedRect(from, local)
                .adjusted(-pending_.width, -pending_.width, pending_.width, pending_.width)));
    } else if (tool_ == Tool::Bezier) {
        // The drag pulls the outgoing handle of the anchor just placed.  The
        // incoming side is its mirror, so the handle is symmetric by
        // construction and only one of the two is ever stored.  The pointer is
        // the rubber band's target too: while the handle is being pulled out the
        // band *is* the handle line, which is what shows the user the bend they
        // are making.
        const QRect before = previewRect();
        if (pending_.points.size() >= 2) {
            pending_.points.last() = local;
        }
        bezierCursor_ = local;
        const QRect after = previewRect();
        if (after != before) {
            // Repaint what the path left before drawing the new preview.
            touch(before);
            touch(after);
        }
    } else if (tool_ == Tool::Rect || tool_ == Tool::Arrow || tool_ == Tool::Line ||
               tool_ == Tool::Wave) {
        const QRect before = grownDirtyRect(
            strokeBounds(pending_.tool, pending_.points, pending_.width, pending_.text));
        if (pending_.points.size() >= 2) {
            pending_.points.last() = local;
        }
        const QRect after = grownDirtyRect(
            strokeBounds(pending_.tool, pending_.points, pending_.width, pending_.text));
        if (after != before) {
            // Repaint what the shape left before drawing the new preview.
            touch(before);
            touch(after);
        }
    }
}

void AnnotateSurface::mouseReleaseEvent(QMouseEvent *event)
{
    if (erasing_ && event->button() == Qt::LeftButton) {
        erasing_ = false;
        eraseFrom_ = QPointF();
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (!drawing_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (pending_.tool == Tool::Bezier) {
        // A release only ends the handle drag that followed the last press.  The
        // path itself is not finished until it is closed or double-clicked, so
        // nothing is committed and `drawing_` stays set: the preview is still
        // what the user is looking at.
        QWidget::mouseReleaseEvent(event);
        return;
    }
    drawing_ = false;
    QRect stalePreview;
    if (tool_ == Tool::Rect || tool_ == Tool::Arrow || tool_ == Tool::Line ||
        tool_ == Tool::Wave) {
        // The preview on screen was drawn where the pointer was at the last
        // motion event.  Releasing a little away from it can shrink the shape,
        // leaving the old outline outside the committed rect -- so the rect the
        // preview occupied is remembered and repainted as well.
        stalePreview = grownDirtyRect(
            strokeBounds(pending_.tool, pending_.points, pending_.width, pending_.text));
        if (pending_.points.size() >= 2) {
            pending_.points.last() = event->position();
        }
    }
    if (!pending_.points.isEmpty()) {
        const Stroke committed = pending_;
        pending_ = Stroke();
        QRect dirty = grownDirtyRect(strokeBounds(committed.tool, committed.points,
                                                  committed.width, committed.text,
                                                  committed.closed));
        if (stalePreview.isValid()) {
            dirty = dirty.united(stalePreview);
        }
        pushHistory(dirty);
        strokes_.append(committed);
        // The stroke appears in one go at release, so its whole rect has to be
        // repainted: the preview is gone and the committed ink is drawn instead.
        touch(dirty);
        if (toolbar_ != nullptr) {
            toolbar_->syncState();
        }
        return;
    }
    pending_ = Stroke();
}

void AnnotateSurface::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    if (drawing_ && pending_.tool == Tool::Bezier) {
        // A double click ends the path where it stands, open.  Qt delivers the
        // second click of the pair as this event rather than as a press, so the
        // anchor it would have placed is the one the first click already did --
        // the path is not left with a duplicate point on its end.
        const QRect stale = previewRect();
        drawing_ = false;
        commitBezier(stale);
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void AnnotateSurface::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // Nothing to rebuild: a stroke's cached ink is sized in device pixels and
    // does not depend on the surface's size, so a resize only moves the toolbar.
    if (toolbar_ != nullptr) {
        toolbar_->reposition();
    }
}

void AnnotateSurface::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        if (textEdit_ != nullptr) {
            finishText(false);
            event->accept();
            return;
        }
        if (drawing_) {
            // Drop the in-progress stroke.  It lives only in `pending_`, which
            // the preview paints, so repainting its rect is the whole undo.  For
            // the pen that rect is the path plus the rubber band, which is what
            // `previewRect` gives.
            const QRect dirty = previewRect();
            drawing_ = false;
            pending_ = Stroke();
            touch(dirty);
            event->accept();
            return;
        }
    }
    const bool control = event->modifiers() & Qt::ControlModifier;
    if (control && event->key() == Qt::Key_Z) {
        if (event->modifiers() & Qt::ShiftModifier) {
            redo();
        } else {
            undo();
        }
        event->accept();
        return;
    }
    if (control && event->key() == Qt::Key_Y) {
        redo();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace vshot

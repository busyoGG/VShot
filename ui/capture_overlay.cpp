// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#include "capture_overlay.hpp"

#include "pixel_fd.hpp"
#include "config.hpp"
#include "shortcuts.hpp"
#include "i18n.hpp"
#include "text_size.hpp"

#include <LayerShellQt/Window>

#include <QAbstractButton>
#include <QApplication>
#include <QBuffer>
#include <QCloseEvent>
#include <QConicalGradient>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHelpEvent>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QLineEdit>
#include <QLineF>
#include <QListWidget>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QStringList>
#include <QUrl>
#include <QWindow>
#include <QSocketNotifier>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <unistd.h>

namespace vshot {
namespace {

constexpr int kHandleRadius = 6;
// How many pixels one cursor step covers while the step modifier is held. One
// pixel is the point of walking the cursor at all -- a corner the mouse cannot
// land on exactly -- and ten is the coarse half of the same trip.
constexpr int kCoarseCursorStep = 10;
// Whether `key` is one of the four arrow keys.  A cursor step is either a nudge
// of the selected mark or a walk of the cursor, and which one it is hangs on
// the key that was pressed rather than on which action it reached: an arrow
// nudges a mark when there is one, a letter always walks.
bool pressedArrow(int key)
{
    return key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up
        || key == Qt::Key_Down;
}
// How far outside a mark's bounds the pointer still counts as being on its
// border rather than on the canvas beyond it.
constexpr int kBorderGrab = 3;
constexpr int kMinimumSelection = 5;
// Widest window label the picker's size pill shows before eliding it.
constexpr int kPickerLabelWidth = 360;
// How often the picker may ask the CLI for a fresh candidate list while the
// pointer travels.  Fast enough that a workspace switch is reflected by the
// time the pointer reaches the window it is heading for, slow enough that a
// drag across the screen does not turn into a stream of compositor queries.
constexpr int kCandidateRefreshIntervalMs = 150;
// And how often it asks when the pointer is not moving at all: a window can
// move, or another monitor's workspace can be switched, without this surface
// seeing an event, and the highlight must not keep describing what used to be
// there.  Only the picker's lifetime pays for this.
constexpr int kCandidateRefreshPollMs = 300;
// How often a walk of the keyboard cursor may ask the CLI to move the real
// pointer.  Every request is a pipe write, a process round trip and a
// compositor call, and a held key repeats far faster than that; the editor's
// own cursor still moves on every repeat, so this is only how far the arrow on
// the screen lags behind the loupe.  Short enough that a few taps look
// immediate.
constexpr int kPointerWarpIntervalMs = 40;
// How long after a keyboard walk a pointer motion is still read as the echo of
// that walk rather than as the user taking the pointer back.  VShot moves the
// pointer by asking the compositor, and the compositor reports the result as an
// ordinary motion event -- which arrives after the step that asked for it, so a
// handler that treated it as "the user moved the mouse" would end the magnifier
// flash the step had just raised, every step, and the loupe would blink.  The
// distance test below is what separates the two in practice; this is the window
// it is applied in, and it has to be wider than the warp throttle so a step
// held back by it is still recognised when it finally goes out.
constexpr qint64 kPointerWarpEchoMs = 250;
// How many pin moves may be written before the daemon answers the first.  More
// than one keeps it from idling between replies; the position is absolute and
// only the newest matters, so the queue never needs to be long.
constexpr int kPinMovesInFlight = 3;
// How long the editor keeps drawing after it has asked to be let go.  The CLI
// holds its answer until the daemon says the picture the editor is drawing is
// on the screen, and the daemon has its own deadline for a surface that never
// draws one; this is the backstop for the CLI itself being gone, which has
// nothing to answer with.  Comfortably past the daemon's own wait, so it never
// cuts a real handoff short.
constexpr int kHandoffWaitMs = 600;
constexpr int kMaxUndoSteps = 100;
constexpr int kLoupeRadius = 7;
constexpr int kLoupeZoom = 8;
constexpr int kLoupeDiameter = (2 * kLoupeRadius + 1) * kLoupeZoom;
constexpr int kLoupeMargin = 10;

// The text-selection colour and the outline that shows what was recognized: a
// translucent blue fill over the characters in the range and a lighter blue
// line around each recognized line.  Both are legible over an undimmed
// screenshot without hiding the text underneath.
const QColor kTextSelectionFill(64, 132, 240, 110);
const QColor kTextOutline(150, 195, 255, 140);

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

// Surface a given output's overlay canvas covers, in global logical pixels.
// Region capture and plain pin editing cover exactly the output; the pin
// editor widens the surface to its whole screen, so every local<->global
// conversion keys off this rect rather than the output geometry.
const LogicalRect &surfaceOf(const OutputSession &output)
{
    return output.surface.width > 0 && output.surface.height > 0 ? output.surface
                                                                 : output.geometry;
}

QRectF localRect(const OutputSession &output, const LogicalRect &rect, const QSize &size)
{
    const LogicalRect &surface = surfaceOf(output);
    const double sx = surface.width == 0
        ? 1.0
        : static_cast<double>(size.width()) / static_cast<double>(surface.width);
    const double sy = surface.height == 0
        ? 1.0
        : static_cast<double>(size.height()) / static_cast<double>(surface.height);
    return QRectF((static_cast<double>(rect.x) - surface.x) * sx,
                  (static_cast<double>(rect.y) - surface.y) * sy,
                  static_cast<double>(rect.width) * sx,
                  static_cast<double>(rect.height) * sy);
}

// Device pixels per logical pixel of an output, as the number it is. A real
// output's density is a whole number, but the pin editor's virtual output
// carries the zoom its image is shown at, which is not -- a 160-pixel pin
// across 176 logical pixels is 0.909 -- so it is never narrowed to an int
// before it is used. Narrowing it to 1 is what drew a zoomed pin's marks at
// the wrong size and in the wrong place.
double outputScale(const OutputSession &output)
{
    return output.scale > 0.0 ? output.scale : 1.0;
}

QRect sourceRect(const OutputSession &output, const LogicalRect &rect)
{
    const double scale = outputScale(output);
    const double x = (static_cast<double>(rect.x) - output.geometry.x) * scale;
    const double y = (static_cast<double>(rect.y) - output.geometry.y) * scale;
    return QRect(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)),
                 static_cast<int>(std::lround(static_cast<double>(rect.width) * scale)),
                 static_cast<int>(std::lround(static_cast<double>(rect.height) * scale)));
}

// The exact inverse of `sourceRect`: a rect in one output's captured device
// pixels becomes the global logical rect the overlay draws in.  The engine's
// coordinates are counted from a crop of that frame, so this is also the one
// place a placement's origin comes from.  The division stays in `double` and
// each edge is rounded only at the end, the same way the text layer places a
// box, so a rect that was mapped out and back comes home.
LogicalRect logicalFromSource(const OutputSession &output, const QRect &rect)
{
    const double scale = outputScale(output);
    const double x = static_cast<double>(output.geometry.x) + static_cast<double>(rect.x()) / scale;
    const double y = static_cast<double>(output.geometry.y) + static_cast<double>(rect.y()) / scale;
    const double width = static_cast<double>(rect.width()) / scale;
    const double height = static_cast<double>(rect.height()) / scale;
    LogicalRect result;
    result.x = static_cast<std::int32_t>(std::lround(x));
    result.y = static_cast<std::int32_t>(std::lround(y));
    result.width = static_cast<std::uint32_t>(std::max(0L, std::lround(width)));
    result.height = static_cast<std::uint32_t>(std::max(0L, std::lround(height)));
    return result;
}

// The top-left pixel of anything actually drawn in `image`.
QPoint firstInk(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) != 0) {
                return QPoint(x, y);
            }
        }
    }
    return QPoint();
}

// How far inside a line edit its own glyphs start, measured against where the
// overlay draws the very same glyphs for the committed label.  The editor is a
// QLineEdit and whatever its style bakes into the content rect shifts the text
// a pixel or two; the committed label is drawn at the mark's origin with
// AlignLeft|AlignTop.  The difference is a constant for a given font and style,
// so a one-glyph probe in both places measures it exactly -- subtracting the
// two puts the editor's text where the label will land instead of where the
// style happens to put it.
QPoint lineEditGlyphInset(QLineEdit *editor)
{
    const QFont font = editor->font();
    const QSize size = editor->size();
    const qreal ratio = editor->devicePixelRatioF() > 0 ? editor->devicePixelRatioF() : 1.0;

    QImage reference(size, QImage::Format_ARGB32_Premultiplied);
    reference.fill(Qt::transparent);
    {
        QPainter painter(&reference);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(QRect(QPoint(0, 0), size), Qt::AlignLeft | Qt::AlignTop,
                         QStringLiteral("H"));
    }

    QImage probe(size, QImage::Format_ARGB32_Premultiplied);
    probe.fill(Qt::transparent);
    editor->render(&probe);

    const QPoint want = firstInk(reference);
    const QPoint got = firstInk(probe);
    return QPoint(static_cast<int>(std::lround((got.x() - want.x()) / ratio)),
                  static_cast<int>(std::lround((got.y() - want.y()) / ratio)));
}

QPointF localPoint(const OutputSession &output, const Point &point, const QSize &size)
{
    const LogicalRect &surface = surfaceOf(output);
    const double sx = surface.width == 0
        ? 1.0
        : static_cast<double>(size.width()) / static_cast<double>(surface.width);
    const double sy = surface.height == 0
        ? 1.0
        : static_cast<double>(size.height()) / static_cast<double>(surface.height);
    return QPointF((static_cast<double>(point.x) - surface.x) * sx,
                   (static_cast<double>(point.y) - surface.y) * sy);
}

// The fixed device-pixel crop the magnifier shows, centred on the cursor's own
// pixel.  Near a frame edge the window would run off the image; the pixels that
// do not exist are borrowed from the nearest edge instead of being dropped, so
// the cursor's pixel stays at the centre of the loupe and the circle is filled
// edge to edge.  Dropping them is what left the loupe showing the frame under
// it near the screen's borders.
QImage loupeCrop(const QImage &image, int centerX, int centerY)
{
    constexpr int span = 2 * kLoupeRadius + 1;
    QImage crop(span, span, QImage::Format_ARGB32);
    if (crop.isNull()) {
        return crop;
    }
    for (int row = 0; row < span; ++row) {
        const int sourceY = std::clamp(centerY - kLoupeRadius + row, 0, image.height() - 1);
        for (int column = 0; column < span; ++column) {
            const int sourceX = std::clamp(centerX - kLoupeRadius + column, 0, image.width() - 1);
            crop.setPixel(column, row, image.pixel(sourceX, sourceY));
        }
    }
    return crop;
}

// Two pi, spelled out rather than read from a platform's `M_PI`: the wave's
// shape must not depend on which libm the build landed on, and the constant is
// the one place that could silently differ.
constexpr double kTau = 6.283185307179586476925286766559;

// Samples the sine wave along the segment `start`..`end` as a polyline.  This
// is the only wave there is -- the preview and the committed mark both come
// from here -- so a wave looks the same live as it does once it is down.
//
// `start` and `end` are overlay-local logical pixels.  `widthLogical` is the
// annotation's width in logical pixels; the amplitude and the wavelength are
// its `max(width * 2, 4)` and `max(width * 6, 18)`, in logical pixels, so the
// shape does not depend on the output's scale.  `scale` is that output's device
// scale and sets only the sampling distance: one sample per *device* pixel
// means a step of `1 / scale` logical pixels, so a wave's polyline is as fine
// as the screen it is drawn on and no finer.
//
// The phase finishes on a whole number of cycles -- `cycles = max(1, round(L /
// wavelength))`, with the wavelength actually used being `L / cycles` -- so both
// ends come back onto the centre line.  Without that step the far end is left
// wherever the phase happened to be, off to one side, and the wave stops
// reading as one drawn from A to B.
//
// A zero-length segment returns the single `start` point, which the callers
// turn into a dot.
QVector<QPointF> wavePolyline(const QPointF &start, const QPointF &end, double amplitude,
                              double wavelength, double scale)
{
    const QPointF delta = end - start;
    const double length = std::hypot(delta.x(), delta.y());
    if (length <= 0.0) {
        return {start};
    }
    // One sample per device pixel, plus both endpoints.
    const double step = 1.0 / std::max(1.0, scale);
    const int n = std::max(2, static_cast<int>(std::ceil(length / step)) + 1);
    const QPointF dir(delta.x() / length, delta.y() / length);
    // The 90-degree rotation of `dir`: the direction the wave deviates in.
    const QPointF normal(-dir.y(), dir.x());
    const double cycles = std::max(1.0, std::round(length / std::max(1.0, wavelength)));
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

// A wave's crest offset and period for one mark, in logical pixels: the
// annotation's own numbers when it carries them, and the width-derived defaults
// when it does not -- a wave the user never tuned keeps exactly the look it had
// before those numbers could be set.  The preview, the committed raster and the
// padding all ask here, so a wave cannot be drawn as one shape and saved as
// another.
double effectiveWaveAmplitude(const Annotation &annotation)
{
    return annotation.amplitude > 0
        ? static_cast<double>(annotation.amplitude)
        : std::max(static_cast<double>(annotation.width) * 2.0, 4.0);
}

double effectiveWaveWavelength(const Annotation &annotation)
{
    return annotation.wavelength > 0
        ? static_cast<double>(annotation.wavelength)
        : std::max(static_cast<double>(annotation.width) * 6.0, 18.0);
}

// The pen tool's geometry.  The model stores one handle per anchor, interleaved
// [anchor0, handleOut0, anchor1, handleOut1, ...] -- exactly the shape the wire
// format carries -- and derives the incoming side by mirroring, so a handle is
// symmetric by construction and the preview, the cached raster and the JSON
// cannot disagree about the curve.

// One point on the cubic whose control points are `p0`..`p3`, at parameter `t`.
// The Bernstein form is the one `QPainterPath::cubicTo` evaluates, so a sample
// taken here lies on the very curve the painter draws.
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
// through the anchor itself.
QPointF mirrorHandle(const QPointF &anchor, const QPointF &handleOut)
{
    return QPointF(2.0 * anchor.x() - handleOut.x(), 2.0 * anchor.y() - handleOut.y());
}

// How many anchors a pen path has: the interleaved list is always even, and the
// anchors are half of it.
int bezierAnchors(const QVector<Point> &points)
{
    return static_cast<int>(points.size() / 2);
}

// The same list in the floating-point coordinates the painter works in.
QVector<QPointF> pointFs(const QVector<Point> &points)
{
    QVector<QPointF> result;
    result.reserve(points.size());
    for (const Point &point : points) {
        result.append(QPointF(point.x, point.y));
    }
    return result;
}

// The pen path as a QPainterPath, from points already in the painter's own
// coordinates.  The live preview, the cached raster and the hit test all build
// it here, so the three cannot disagree about the curve.  A closed path joins
// the last anchor back to the first through both their handles.
QPainterPath bezierPathAt(const QVector<QPointF> &at, bool closed)
{
    QPainterPath path;
    const int anchors = static_cast<int>(at.size() / 2);
    if (anchors <= 0) {
        return path;
    }
    path.moveTo(at.at(0));
    const int segments = closed ? anchors : anchors - 1;
    for (int index = 0; index < segments; ++index) {
        const int next = (index + 1) % anchors;
        path.cubicTo(at.at(2 * index + 1), mirrorHandle(at.at(2 * next), at.at(2 * next + 1)),
                     at.at(2 * next));
    }
    if (closed) {
        path.closeSubpath();
    }
    return path;
}

// The same path from the session-space points the model stores.
QPainterPath bezierPath(const QVector<Point> &points, bool closed)
{
    return bezierPathAt(pointFs(points), closed);
}

// The pen path's ink, in whatever coordinate space the caller has converted its
// points into.  The preview and the committed mark both draw through here, so a
// path cannot end up filled in one place and only stroked in another.
//
// `fill` is "stroke", "fill" or "both", and none of them fills a path the user
// left open: an open path is stroked in "fill" and in "both" alike, so the two
// modes differ only in whether a closed shape keeps its outline.  Filling an
// open path would close it behind the user's back -- the shape would be whole
// the moment it appeared, as if the pen had drawn something nobody closed.
void paintBezierInk(QPainter *painter, const QVector<QPointF> &at, bool closed, const QString &fill,
                    const QColor &color, int width)
{
    if (at.isEmpty()) {
        return;
    }
    if (at.size() < 4) {
        // A click that was never dragged past its own anchor is a dot, the same
        // ink the freehand pen gives one.  It is always stroked: a fill on its
        // own would leave the click invisible.
        painter->setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        painter->drawPoint(at.constFirst());
        return;
    }
    const bool wantFill = closed && fill != QStringLiteral("stroke");
    const bool wantStroke = !wantFill || fill == QStringLiteral("both");
    const QPainterPath path = bezierPathAt(at, closed);
    if (wantFill) {
        // Fill first and stroke second, so the outline is not tinted by the
        // translucent fill it sits on.
        // The fill is the stroke's own colour at half its alpha, floored: that
        // is what "a translucent fill under a solid outline" means for a colour
        // the user picked an opacity for.
        QColor fillInk = color;
        fillInk.setAlpha(color.alpha() / 2);
        painter->fillPath(path, fillInk);
    }
    if (wantStroke) {
        painter->setBrush(Qt::NoBrush);
        painter->strokePath(path, QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    }
}

// The same path sampled into a polyline, about one sample per two logical
// pixels of control polygon so the sample follows the curve's own turning.  The
// samples lie on the curve, which is what the tight bounding box and the hit
// test need: a box over the control points alone would be far larger than the
// ink, and one over the anchors alone would clip the very bulge the handles pull
// out.
QVector<QPointF> bezierPolyline(const QVector<Point> &points, bool closed)
{
    QVector<QPointF> samples;
    const int anchors = bezierAnchors(points);
    if (anchors <= 0) {
        return samples;
    }
    const QVector<QPointF> at = pointFs(points);
    samples.append(at.at(0));
    const int segments = closed ? anchors : anchors - 1;
    for (int index = 0; index < segments; ++index) {
        const int next = (index + 1) % anchors;
        const QPointF &start = at.at(2 * index);
        const QPointF &handle = at.at(2 * index + 1);
        const QPointF &end = at.at(2 * next);
        const QPointF incoming = mirrorHandle(end, at.at(2 * next + 1));
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

// The tight box of a sampled polyline in session coordinates, rounded outward,
// or `false` when there is nothing to bound.  Folded by hand rather than with
// `QRectF::united`: uniting onto a null rect returns the other operand, which
// turns a box that was never seeded into whatever the last point happened to be
// -- a bounding box that collapses to a point, and stale ink left on screen.
bool polylineLogicalBounds(const QVector<QPointF> &samples, LogicalRect *bounds)
{
    if (samples.isEmpty()) {
        return false;
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
    const std::int32_t x = static_cast<std::int32_t>(std::floor(left));
    const std::int32_t y = static_cast<std::int32_t>(std::floor(top));
    bounds->x = x;
    bounds->y = y;
    bounds->width = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(std::ceil(right)) - x + 1);
    bounds->height = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(std::ceil(bottom)) - y + 1);
    return true;
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
    case Tool::Line:
        return QStringLiteral("line");
    case Tool::Wave:
        return QStringLiteral("wave");
    case Tool::Bezier:
        return QStringLiteral("bezier");
    case Tool::Pen:
        return QStringLiteral("pen");
    case Tool::Mosaic:
        return QStringLiteral("mosaic");
    case Tool::Text:
        return QStringLiteral("text");
    case Tool::Number:
        return QStringLiteral("number");
    case Tool::Picker:
        return QStringLiteral("picker");
    }
    return QStringLiteral("pen");
}

/// The inverse of [`toolName`], for a name that came out of the config file.
/// An unrecognized name is a typo in a file the user can edit, and "select" is
/// the name the old Select tool was remembered under -- still the default in
/// every config file written before that tool went away.  Neither names a tool
/// there is anything to arm, so both come back empty and the session stays
/// unarmed, which is the state that tool used to be.
std::optional<Tool> toolForName(const QString &name)
{
    if (name == QStringLiteral("rectangle")) {
        return Tool::Rectangle;
    }
    if (name == QStringLiteral("ellipse")) {
        return Tool::Ellipse;
    }
    if (name == QStringLiteral("arrow")) {
        return Tool::Arrow;
    }
    if (name == QStringLiteral("line")) {
        return Tool::Line;
    }
    if (name == QStringLiteral("wave")) {
        return Tool::Wave;
    }
    if (name == QStringLiteral("bezier")) {
        return Tool::Bezier;
    }
    if (name == QStringLiteral("pen")) {
        return Tool::Pen;
    }
    if (name == QStringLiteral("mosaic")) {
        return Tool::Mosaic;
    }
    if (name == QStringLiteral("text")) {
        return Tool::Text;
    }
    if (name == QStringLiteral("number")) {
        return Tool::Number;
    }
    return std::nullopt;
}

QFont textFont(const QString &family, int pixelSize)
{
    QFont font = family.isEmpty() ? QApplication::font() : QFont(family);
    font.setPixelSize(std::max(1, pixelSize));
    return font;
}

QFont annotationFont(const Annotation &annotation)
{
    // The stored size is already the pixel height, so it goes straight in.
    return textFont(annotation.font, std::max(1, static_cast<int>(annotation.textPixels)));
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

// The halo the bare-number style outlines its glyphs with, and the glyph colour
// that reads on top of a filled badge.  Both are chosen from the ink so the
// count stays legible whatever colour the pen is -- the palette carries a yellow
// and a white, and white-on-white would erase the count.
QColor numberHalo(const QColor &ink)
{
    return ink.lightness() > 140 ? QColor(0, 0, 0, 210) : QColor(255, 255, 255, 210);
}

QColor numberOnInk(const QColor &ink)
{
    return ink.lightness() > 160 ? QColor(20, 20, 20) : QColor(255, 255, 255);
}

constexpr qreal kNumberHaloWidth = 2.0;

// The one place a numbered badge is turned into ink.  The overlay's live
// preview, its cached per-mark raster and the layer handed to the CLI all draw
// through here, so the four styles cannot drift apart between them.
void paintNumberBadge(QPainter &painter, const QRectF &box, const QString &text, NumberStyle style,
                      const QColor &color)
{
    const int diameter = std::max(1, static_cast<int>(std::lround(box.width())));
    painter.setFont(numberFont(diameter));
    switch (style) {
    case NumberStyle::FilledCircle:
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(box);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(numberOnInk(color));
        painter.drawText(box, Qt::AlignCenter, text);
        break;
    case NumberStyle::Ring: {
        // The ring's line comes from the badge's own diameter now that the width
        // slider no longer describes a badge: an eighth of it, floored at two
        // pixels so a small badge still shows a ring, and never past a quarter.
        const qreal pen = std::clamp(box.width() / 8.0, 2.0, std::max(1.0, box.width() / 4.0));
        const qreal inset = pen / 2.0;
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(color, pen, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawEllipse(box.adjusted(inset, inset, -inset, -inset));
        painter.setPen(color);
        painter.drawText(box, Qt::AlignCenter, text);
        break;
    }
    case NumberStyle::Square: {
        const qreal radius = box.width() * 0.22;
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(box, radius, radius);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(numberOnInk(color));
        painter.drawText(box, Qt::AlignCenter, text);
        break;
    }
    case NumberStyle::Plain: {
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

// The badge style's name, for the style row's segment tooltips and the number
// tool's own tooltip.
QString numberStyleName(NumberStyle style)
{
    switch (style) {
    case NumberStyle::FilledCircle:
        return uiTr("Filled circle");
    case NumberStyle::Ring:
        return uiTr("Ring");
    case NumberStyle::Square:
        return uiTr("Square");
    case NumberStyle::Plain:
        return uiTr("Plain");
    }
    return QString();
}

// The tag the four number-style segments carry, and the name the style travels
// under in the marks document: a badge that comes back from a re-edit has to be
// the same shape it went out as.
QString numberStyleValue(NumberStyle style)
{
    switch (style) {
    case NumberStyle::FilledCircle:
        return QStringLiteral("filled_circle");
    case NumberStyle::Ring:
        return QStringLiteral("ring");
    case NumberStyle::Square:
        return QStringLiteral("square");
    case NumberStyle::Plain:
        break;
    }
    return QStringLiteral("plain");
}

// Whether an annotation is the number tool's badge rather than a typed label.
// Both are text annotations and differ only in the one name, which is what
// keeps every reader that does not care about badges from having to know.
bool isNumberAnnotation(const Annotation &annotation)
{
    return annotation.kind == Annotation::Kind::Text &&
        annotation.tool == QStringLiteral("number");
}

// The point a badge is centred on, recovered from its own box.
Point numberCenter(const Annotation &annotation)
{
    return Point{annotation.rect.x + static_cast<std::int32_t>(annotation.rect.width / 2),
                 annotation.rect.y + static_cast<std::int32_t>(annotation.rect.height / 2)};
}

} // namespace

// A numbered badge's diameter is a value of its own, set by the number tool's
// size control: floored at 18 logical pixels so the count stays legible, and
// capped at 96 so it does not paint a billboard.  It used to be six pen widths
// across, which tied a badge's size to the width slider and left no way to size
// one without restyling every stroke.  The standalone annotate surface spells
// the same two numbers out for itself: the two files share no code on purpose,
// and this is the price of that.
int numberDiameter(std::uint32_t size)
{
    return std::clamp(static_cast<int>(size), kNumberMinDiameter, kNumberMaxDiameter);
}

// The four looks, read from the tag the wire carries.
NumberStyle numberStyleForName(const QString &value)
{
    if (value == QStringLiteral("ring")) {
        return NumberStyle::Ring;
    }
    if (value == QStringLiteral("square")) {
        return NumberStyle::Square;
    }
    if (value == QStringLiteral("plain")) {
        return NumberStyle::Plain;
    }
    return NumberStyle::FilledCircle;
}

// Lays a badge's box out around `center` for the diameter it currently carries.
//
// The box is not decoration: the hit test, the drag clamp, the raster cache and
// the bitmap the renderer is handed are all sized from it, so it is re-derived
// wherever the diameter changes -- at placement, and when the size control
// restyles the badge under the selection.
void layoutNumberBox(Annotation &annotation, Point center)
{
    const int diameter = numberDiameter(annotation.numberSize);
    const Point origin{center.x - diameter / 2, center.y - diameter / 2};
    annotation.origin = origin;
    annotation.rect = LogicalRect{origin.x, origin.y, static_cast<std::uint32_t>(diameter),
                                  static_cast<std::uint32_t>(diameter)};
    // The glyph multiple the wire carries is only read by a reader that has to
    // draw the badge itself, so the count it means is the badge's own diameter:
    // such a reader then draws glyphs about as tall as the badge, rather than a
    // label at some unrelated size.
    annotation.textPixels = static_cast<std::uint32_t>(diameter);
}

namespace {

// The logical rect an annotation occupies, whatever its kind.  Shared by the
// hit test, the drag clamps and the render cache so all three agree on what a
// mark covers; `false` means there is nothing to draw or hit.
bool annotationLogicalBounds(const Annotation &annotation, LogicalRect *bounds)
{
    switch (annotation.kind) {
    case Annotation::Kind::Shape:
        *bounds = annotation.rect;
        return !bounds->isEmpty();
    case Annotation::Kind::Image:
        // The rect is where the pixels were placed, which is not the image's
        // own size: the paste fits it to the canvas and the handles resize it.
        *bounds = annotation.rect;
        return !annotation.pixels.isNull() && !bounds->isEmpty();
    case Annotation::Kind::Text: {
        // A numbered badge's box is the badge itself, recorded on the
        // annotation when it was placed.  The hit test, the drag clamp, the
        // raster cache and the paint all read it from here, so filling `rect`
        // at placement is what makes a badge selectable, draggable and
        // deletable rather than only visible.
        if (isNumberAnnotation(annotation)) {
            *bounds = annotation.rect;
            return !bounds->isEmpty();
        }
        if (annotation.text.isEmpty()) {
            return false;
        }
        const QSize metrics = textMetrics(annotation);
        *bounds = LogicalRect{annotation.origin.x, annotation.origin.y,
                              static_cast<std::uint32_t>(metrics.width()),
                              static_cast<std::uint32_t>(metrics.height())};
        return true;
    }
    case Annotation::Kind::Translation:
        // The line boxes the translation replaces are its whole extent; the
        // union of the fills is what was stored as `rect`.
        *bounds = annotation.rect;
        return !bounds->isEmpty();
    case Annotation::Kind::Stroke:
        break;
    }
    if (annotation.points.isEmpty()) {
        return false;
    }
    if (annotation.tool == QStringLiteral("bezier")) {
        // A pen path is a cubic per segment and a cubic leaves the box of its
        // own anchors, so the box has to come from the sampled curve.  Taking
        // the control points instead would report a rect -- and so a raster --
        // far larger than the ink, and taking the anchors alone would clip the
        // bulge and leave a stale arc on screen.
        return polylineLogicalBounds(bezierPolyline(annotation.points, annotation.closed),
                                     bounds);
    }
    // The box of a stroke's own points.  For a wave those are its two ends and
    // the box is the segment between them; the crests that reach off it are
    // accounted for by the raster's padding (`StrokeRaster::padding`, read here
    // through `annotationReach`) rather than by this box -- the same split the
    // arrow's head already uses.
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
    case Tool::Line:
        // The arrow's shaft without its head: a plain straight segment.
        painter.drawLine(QPointF(4, 19), QPointF(20, 5));
        break;
    case Tool::Wave: {
        QPainterPath wave;
        wave.moveTo(3.0, 12.0);
        for (int step = 1; step <= 18; ++step) {
            wave.lineTo(3.0 + step, 12.0 + std::sin(step * kTau / 9.0) * 5.0);
        }
        painter.drawPath(wave);
        break;
    }
    case Tool::Bezier: {
        // A curve with both its end anchors: the one icon in the row that says
        // "this one bends between the points you click".
        QPainterPath curve;
        curve.moveTo(4.0, 19.0);
        curve.cubicTo(4.0, 8.0, 20.0, 16.0, 20.0, 5.0);
        painter.drawPath(curve);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRect(QRectF(2.0, 17.0, 4.0, 4.0));
        painter.drawRect(QRectF(18.0, 3.0, 4.0, 4.0));
        painter.setBrush(Qt::NoBrush);
        break;
    }
    case Tool::Pen:
        painter.drawLine(QPointF(4, 17), QPointF(8, 12));
        painter.drawLine(QPointF(8, 12), QPointF(12, 15));
        painter.drawLine(QPointF(12, 15), QPointF(20, 7));
        break;
    case Tool::Text:
        painter.setFont(QFont(QStringLiteral("Sans"), 15, QFont::Bold));
        painter.drawText(QRectF(3, 2, 18, 20), Qt::AlignCenter, QStringLiteral("T"));
        break;
    case Tool::Number:
        // A miniature of the badge itself, drawn with the same painter as the
        // mark so the button and the click agree on the look.
        paintNumberBadge(painter, QRectF(4.0, 4.0, 16.0, 16.0), QStringLiteral("1"),
                         NumberStyle::FilledCircle, color);
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
    case Tool::Picker:
        // An eyedropper: the tip that takes the pixel, the barrel behind it and
        // the bulb that stands for the colour it brings back.  The bulb is
        // filled, like the pin's head, so the shape still reads at button size.
        painter.setBrush(color);
        painter.drawPolygon(QPolygonF{QPointF(3.5, 20.5), QPointF(6.5, 19.0),
                                      QPointF(5.0, 17.5)});
        painter.drawEllipse(QPointF(17.0, 7.0), 4.0, 4.0);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(QPointF(6.0, 18.0), QPointF(14.0, 10.0));
        break;
    }
    return QIcon(pixmap);
}

// The ink of the undo/redo arrows.  The glyph is a pixmap, so the stylesheet's
// disabled colour never reaches it: the button has to pick its own ink, and
// this is what makes "there is a step to go back to" visible at a glance
// rather than by hovering.
const QColor kHistoryInk(QStringLiteral("#e6e1e5"));
const QColor kHistoryInkDimmed(QStringLiteral("#4b525d"));

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

// The paste-image action's icon: a framed picture with a horizon and a sun,
// the shape everyone reads as "an image file".
QIcon pasteIcon(const QColor &color = QColor(230, 225, 229), qreal devicePixelRatio = 1.0)
{
    const qreal ratio = std::max(1.0, devicePixelRatio);
    QPixmap pixmap(qRound(24 * ratio), qRound(24 * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(3.5, 5.5, 17, 13), 2.5, 2.5);
    // The horizon and the peak, drawn as one polyline inside the frame.
    painter.drawPolyline(QPolygonF{QPointF(4, 16), QPointF(9, 11), QPointF(13, 15), QPointF(16, 12),
                                   QPointF(20, 16)});
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QPointF(8.5, 9.0), 1.4, 1.4);
    return QIcon(pixmap);
}

// The text-recognition action's icon: corner brackets around two text lines,
// which is what "read the text in this box" looks like.
QIcon recognizeTextIcon(const QColor &color = QColor(230, 225, 229),
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
    constexpr qreal inset = 3.5;
    constexpr qreal arm = 4.0;
    const qreal right = 24.0 - inset;
    const qreal bottom = 24.0 - inset;
    painter.drawPolyline(QPolygonF{QPointF(inset + arm, inset), QPointF(inset, inset),
                                   QPointF(inset, inset + arm)});
    painter.drawPolyline(QPolygonF{QPointF(right - arm, inset), QPointF(right, inset),
                                   QPointF(right, inset + arm)});
    painter.drawPolyline(QPolygonF{QPointF(inset, bottom - arm), QPointF(inset, bottom),
                                   QPointF(inset + arm, bottom)});
    painter.drawPolyline(QPolygonF{QPointF(right, bottom - arm), QPointF(right, bottom),
                                   QPointF(right - arm, bottom)});
    painter.drawLine(QPointF(8.5, 10.5), QPointF(15.5, 10.5));
    painter.drawLine(QPointF(8.5, 14.0), QPointF(13.0, 14.0));
    return QIcon(pixmap);
}

// The translation action's icon: a Latin "A" and an arrow into whatever script
// it becomes.  Drawn from strokes rather than glyphs so the picture is the same
// on a font-less machine, and kept apart from the recognition brackets.
QIcon translateIcon(const QColor &color = QColor(230, 225, 229),
                    qreal devicePixelRatio = 1.0)
{
    const qreal ratio = std::max(1.0, devicePixelRatio);
    QPixmap pixmap(qRound(24 * ratio), qRound(24 * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    // The "A": two legs and a crossbar.
    painter.drawLine(QPointF(2.5, 19.0), QPointF(6.5, 6.0));
    painter.drawLine(QPointF(6.5, 6.0), QPointF(10.5, 19.0));
    painter.drawLine(QPointF(4.2, 14.0), QPointF(8.8, 14.0));
    // The arrow into the translated text.
    painter.drawLine(QPointF(13.0, 12.5), QPointF(21.0, 12.5));
    painter.drawPolyline(QPolygonF{QPointF(17.5, 8.5), QPointF(21.5, 12.5),
                                   QPointF(17.5, 16.5)});
    return QIcon(pixmap);
}

// The scrolling-capture action's icon: a downward arrow over stacked rows,
// which is what "scroll this and stitch what comes back" looks like.
QIcon scrollIcon(const QColor &color = QColor(230, 225, 229), qreal devicePixelRatio = 1.0)
{
    const qreal ratio = std::max(1.0, devicePixelRatio);
    QPixmap pixmap(qRound(24 * ratio), qRound(24 * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    // The arrow points down at the rows the scroll will stack underneath.
    painter.drawLine(QPointF(12.0, 3.5), QPointF(12.0, 13.5));
    painter.drawPolyline(QPolygonF{QPointF(8.0, 9.5), QPointF(12.0, 13.5), QPointF(16.0, 9.5)});
    // The stacked rows below: the tall image the stitch builds.
    painter.drawLine(QPointF(6.0, 17.0), QPointF(18.0, 17.0));
    painter.drawLine(QPointF(6.0, 20.5), QPointF(18.0, 20.5));
    return QIcon(pixmap);
}

// The pin action's icon: a thumbtack seen from the side -- a solid head, the
// collar under it and the needle -- the shape everyone reads as "pin this".
// It is drawn at the tool icons' weight and size, because it is the same kind
// of button.
QIcon pinIcon(const QColor &color = QColor(230, 225, 229), qreal devicePixelRatio = 1.0)
{
    const qreal ratio = std::max(1.0, devicePixelRatio);
    QPixmap pixmap(qRound(24 * ratio), qRound(24 * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // The needle and the collar, in the same stroke as every other icon.
    painter.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawLine(QPointF(6.5, 11.0), QPointF(17.5, 11.0));
    painter.drawLine(QPointF(12.0, 11.0), QPointF(12.0, 20.5));
    // The head, filled so it reads as a tack rather than as another circle.
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(12.0, 7.0), 3.6, 3.6);
    return QIcon(pixmap);
}

// How far the size pill keeps from the selection it hangs off, and how much
// room its text needs.
constexpr int kInfoPillGap = 8;

QSizeF pillSize(const QString &text)
{
    const QFontMetrics metrics(pillFont());
    return QSizeF(metrics.horizontalAdvance(text) + 16, metrics.height() + 8);
}

// Draws a dark rounded label (dimensions, pixel coordinates) in `pill`, pulled
// back inside `bounds` when it would hang over an edge.
void drawPillBox(QPainter *painter, const QRectF &pill, const QString &text, const QRectF &bounds)
{
    const qreal x = std::clamp(pill.left(), bounds.left() + 2.0,
                               std::max(bounds.left() + 2.0, bounds.right() - pill.width() - 2.0));
    const qreal y = std::clamp(pill.top(), bounds.top() + 2.0,
                               std::max(bounds.top() + 2.0, bounds.bottom() - pill.height() - 2.0));
    const QRectF box(x, y, pill.width(), pill.height());
    painter->setFont(pillFont());
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(20, 20, 20, 225));
    painter->drawRoundedRect(box, 4, 4);
    painter->setPen(QPen(QColor(120, 120, 120), 1.0));
    painter->drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    painter->setPen(Qt::white);
    painter->drawText(box, Qt::AlignCenter, text);
}

// Draws a dark rounded label anchored at `anchor` inside `bounds`; flips above
// the anchor when there is no room below.  The loupe's coordinate readout.
void drawInfoPill(QPainter *painter, const QPointF &anchor, const QString &text,
                  const QRectF &bounds)
{
    const QSizeF size = pillSize(text);
    qreal y = anchor.y() + 10.0;
    if (y + size.height() > bounds.bottom()) {
        y = anchor.y() - size.height() - 10.0;
    }
    drawPillBox(painter, QRectF(anchor.x() - size.width() / 2.0, y, size.width(), size.height()),
                text, bounds);
}

// Draws the size pill of a capture selection, which hangs *outside* the region
// so that the label never covers the pixels the capture is about to keep.  The
// first place it tries is beside the selection, level with its top edge: that
// is the one side the floating toolbar never reaches -- it settles above the
// selection or below it -- and it is a fixed spot while the far corner is
// dragged.  Then above the top-left corner, then below the selection, and a
// selection that fills the output every way leaves the pill inside, under the
// corner, where there is nowhere else for it to be.
//
// `corner` is the selection's top-left in overlay-local coordinates, `region`
// the selection itself, `bounds` the whole surface, and `toolbar` the floating
// toolbar in the same coordinates -- a child widget paints over this paint(),
// so a pill under the toolbar would simply not be seen.
void drawSelectionPill(QPainter *painter, const QPointF &corner, const QRectF &region,
                       const QString &text, const QRectF &bounds, const QRectF &toolbar)
{
    const QSizeF size = pillSize(text);
    // Sideways the pill is centred on the selection's corner, pulled back
    // inside the surface when the corner is against an edge.
    const qreal x = std::clamp(corner.x() - size.width() / 2.0, bounds.left() + 2.0,
                               std::max(bounds.left() + 2.0, bounds.right() - size.width() - 2.0));
    const bool blocked = !toolbar.isEmpty();
    const auto free = [&](const QRectF &box) {
        return box.left() >= bounds.left() + 2.0 && box.top() >= bounds.top() + 2.0 &&
               box.right() <= bounds.right() - 2.0 && box.bottom() <= bounds.bottom() - 2.0 &&
               !(blocked && box.intersects(toolbar));
    };
    // Beside the region.  The top is pulled inside the surface when the
    // selection starts at the very edge of the output.
    QRectF pill(corner.x() - kInfoPillGap - size.width(),
                std::max(corner.y(), bounds.top() + 2.0), size.width(), size.height());
    if (!free(pill)) {
        pill = QRectF(x, corner.y() - kInfoPillGap - size.height(), size.width(), size.height());
        if (!free(pill)) {
            pill = QRectF(x, region.bottom() + kInfoPillGap, size.width(), size.height());
            if (!free(pill)) {
                pill = QRectF(x, corner.y() + kInfoPillGap, size.width(), size.height());
            }
        }
    }
    drawPillBox(painter, pill, text, bounds);
}

// Mosaic strength levels: block size in device pixels for a given output
// scale (P1 fine, P2 standard, P3 coarse) — mirrors edit::mosaic_block_size.
// The scale is the ratio it is, not a whole number: a zoomed pin's is 0.909,
// and rounding that to 1 would pixelate at a different block than the renderer.
int mosaicBlockForStrength(std::uint32_t strength, double scale)
{
    const int base = std::max(1, static_cast<int>(std::lround(12.0 * scale)));
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

// Builds the pen for an annotation, translating the wire line style into a dash
// pattern in device pixels: dashes of 3w with 2w gaps, dots of w with 2w gaps.
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

// The pen a wave is drawn with: always solid, whatever the style says.  A wave
// is a sampled sine and has no dash to carry, so offering one would put a
// control on the toolbar that changes nothing.
QPen wavePen(const Annotation &annotation)
{
    return QPen(annotation.color, static_cast<double>(annotation.width), Qt::SolidLine,
                Qt::RoundCap, Qt::RoundJoin);
}

// Computes the average color of one device-pixel block: a 4x4 subsample of the
// block, rounded half up per channel.
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

// Blits one solid block straight into the destination image when the painter is
// drawing onto an ARGB32-premultiplied QImage under a pure scale-and-translate
// transform with no clip.  `fillLogicalBlock` runs once per mosaic block, and a
// 1080p area mosaic at the standard 12-device-pixel block is about 14k calls,
// each of which the QPainter path pays a call and a clip test for.  The direct
// write reproduces `QPainter::fillRect(QRectF, color)` exactly: the logical rect
// is mapped through the painter transform and aligned outward to whole device
// pixels (the raster engine's fill for a solid, unantialiased colour), and a
// fully opaque colour written over the destination is the same as a source-over
// fill of it.  Anything else -- a translucent block, a widget or pixmap target,
// a rotation, an active clip -- returns false and lets the caller fall back.
bool fillRectDirect(QPainter *painter, const QRectF &logical, const QColor &color)
{
    if (color.alpha() != 255 || painter->hasClipping()) {
        return false;
    }
    QPaintDevice *device = painter->device();
    if (device == nullptr || device->devType() != QInternal::Image) {
        return false;
    }
    QImage *image = static_cast<QImage *>(device);
    if (image->format() != QImage::Format_ARGB32_Premultiplied) {
        return false;
    }
    const QTransform transform = painter->combinedTransform();
    if (transform.type() > QTransform::TxScale) {
        return false;
    }
    // The raster engine antialiases a rect whose edges land between device
    // pixels, so only an exactly pixel-aligned block may be written directly;
    // anything fractional (a scaled painter, a non-integer device ratio) falls
    // back to QPainter.
    const QRectF mapped = transform.mapRect(logical);
    const QRect deviceRect = mapped.toAlignedRect();
    if (mapped != QRectF(deviceRect)) {
        return false;
    }
    const QRect clipped = deviceRect.intersected(QRect(0, 0, image->width(), image->height()));
    if (clipped.isEmpty()) {
        return true;
    }
    const QRgb value = qRgba(color.red(), color.green(), color.blue(), 255);
    for (int row = clipped.top(); row <= clipped.bottom(); ++row) {
        QRgb *line = reinterpret_cast<QRgb *>(image->scanLine(row)) + clipped.left();
        for (int col = 0; col < clipped.width(); ++col) {
            line[col] = value;
        }
    }
    return true;
}

void fillLogicalBlock(QPainter *painter, const OutputSession &output, const LogicalRect &bounds,
                      const QSize &size, const QRect &source, double scale, int x, int y, int width,
                      int height, const QColor &color)
{
    // The block is in device pixels and the rect is logical, so the division is
    // the ratio it is: a zoomed pin's scale is 0.909, and a block mapped back
    // through a rounded 1 would land on the wrong part of the picture.
    LogicalRect blockLogical;
    blockLogical.x = bounds.x + static_cast<std::int32_t>(std::lround((x - source.x()) / scale));
    blockLogical.y = bounds.y + static_cast<std::int32_t>(std::lround((y - source.y()) / scale));
    blockLogical.width = static_cast<std::uint32_t>(std::lround(width / scale));
    blockLogical.height = static_cast<std::uint32_t>(std::lround(height / scale));
    const QRectF logical = localRect(output, blockLogical, size);
    if (fillRectDirect(painter, logical, color)) {
        return;
    }
    painter->fillRect(logical, color);
}

// Renders a real pixelation mosaic over the annotation bounds: blocks of
// 12 * scale device pixels averaged independently and aligned to the bounds
// origin.  `mask` picks a rectangular or elliptical area;
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
    const double scale = outputScale(output);
    const int block = mosaicBlockForStrength(strength, scale);
    const uchar *bits = image.constBits();
    const qsizetype bytesPerLine = image.bytesPerLine();
    const bool ellipse = mask == QStringLiteral("ellipse");
    // An integer midline ellipse: the centre is `left + size/2`, which is what
    // keeps an odd-sized rect's ellipse on the pixel the user drew around.
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
                                     y + yy, 1, 1, average);
                }
            }
        }
    }
}

// Appends the device-space stamp centers of one segment, plus the segment's own
// start when it is the first one, to `centers`.  Each segment's stamps depend
// only on its two endpoints, which is what lets a growing freehand stroke stamp
// a segment once and never touch it again.
void appendMosaicCenters(QVector<QPointF> &centers, const OutputSession &output,
                         const Point &first, const Point &second, bool includeFirst, double scale,
                         double step)
{
    const QPointF a((first.x - output.geometry.x) * scale, (first.y - output.geometry.y) * scale);
    const QPointF b((second.x - output.geometry.x) * scale,
                    (second.y - output.geometry.y) * scale);
    if (includeFirst) {
        centers.append(a);
    }
    const double length = std::hypot(b.x() - a.x(), b.y() - a.y());
    const int count = std::max(1, static_cast<int>(std::ceil(length / step)));
    for (int k = 1; k <= count; ++k) {
        const double t = static_cast<double>(k) / count;
        centers.append(a + (b - a) * t);
    }
}

// Averages the source image under each stamp center and paints the disc.  The
// whole-path brush and the incremental one share this so both smear alike.
void stampMosaicDiscs(QPainter *painter, const OutputSession &output,
                      const QVector<QPointF> &centers, int radius, double scale, const QSize &size)
{
    const QImage &image = output.image;
    if (image.isNull() || centers.isEmpty()) {
        return;
    }
    const uchar *bits = image.constBits();
    const qsizetype bytesPerLine = image.bytesPerLine();
    const double radiusSquared = static_cast<double>(radius) * radius;
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

// Stamps one segment of the brush, the unit the incremental freehand raster
// bakes one at a time.
void stampMosaicSegment(QPainter *painter, const OutputSession &output, const Point &first,
                        const Point &second, bool includeFirst, int radius, double scale,
                        double step, const QSize &size)
{
    QVector<QPointF> centers;
    appendMosaicCenters(centers, output, first, second, includeFirst, scale, step);
    stampMosaicDiscs(painter, output, centers, radius, scale, size);
}

// Smears mosaic discs along the path, mirroring Frame::mosaic_brush: device
// space, radius = width/2, stamps every radius/2 pixels, each stamp averaged
// from the pristine source image.
void drawMosaicBrush(QPainter *painter, const OutputSession &output, const QVector<Point> &points,
                     std::uint32_t widthLogical, std::uint32_t strength, const QSize &size)
{
    if (output.image.isNull() || points.isEmpty()) {
        return;
    }
    const double scale = outputScale(output);
    const int baseRadius = std::clamp(static_cast<int>(widthLogical * scale / 2.0), 1, 512);
    const int radius = std::clamp(brushRadiusForStrength(strength, baseRadius), 1, 512);
    // The same spacing the live preview uses (`paintLiveStroke`), so a previewed
    // mosaic brush stamps the same discs as the mark it commits.
    const double step = std::max(1.0, radius / 2.0);
    QVector<QPointF> centers;
    centers.reserve(points.size() + 8);
    if (points.size() == 1) {
        centers.append(QPointF((points.constFirst().x - output.geometry.x) * scale,
                               (points.constFirst().y - output.geometry.y) * scale));
    } else {
        for (int index = 0; index + 1 < points.size(); ++index) {
            appendMosaicCenters(centers, output, points.at(index), points.at(index + 1),
                                index == 0, scale, step);
        }
    }
    stampMosaicDiscs(painter, output, centers, radius, scale, size);
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
    const QPainterPath chip = superellipsePath(QRectF(0.5, 0.5, 19.0, 19.0), 7.0, 5.0);
    // A translucent chip shows a checkerboard through it, the way the settings
    // window's color button does, so "this swatch carries alpha" is visible at a
    // glance rather than reading as a slightly darker opaque colour.
    if (color.alpha() < 255) {
        painter.save();
        painter.setClipPath(chip);
        painter.fillRect(QRectF(0.0, 0.0, 20.0, 20.0), QColor(0x6f, 0x76, 0x80));
        painter.fillRect(QRectF(0.0, 0.0, 10.0, 10.0), QColor(0x9a, 0xa3, 0xae));
        painter.fillRect(QRectF(10.0, 10.0, 10.0, 10.0), QColor(0x9a, 0xa3, 0xae));
        painter.restore();
    }
    painter.setBrush(color);
    painter.drawPath(chip);
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

// The editor's own tooltip, a plain child widget of the overlay.
//
// Qt's `QToolTip` is a popup window, and this process runs with the layer-shell
// platform integration — the same one that makes the overlay a layer surface.  A
// tooltip's popup is stamped onto that integration, the compositor never gets a
// usable popup, and Qt ends up painting the tip into the overlay's own
// full-output surface: a screen-sized block of panel colour with the text tucked
// into a corner.  A child widget has none of that; it is positioned next to what
// it describes and drawn with the same card treatment as the picker popups.
class HoverTip final : public QLabel {
public:
    explicit HoverTip(QWidget *parent)
        : QLabel(parent)
    {
        setObjectName(QStringLiteral("vshotTooltip"));
        // The tip is under the pointer by construction, so it must never take a
        // mouse event: a Leave it caused would dismiss it again immediately.
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_StyledBackground, false);
        setFocusPolicy(Qt::NoFocus);
        setTextFormat(Qt::PlainText);
        setWordWrap(false);
        setContentsMargins(9, 5, 9, 5);
        setStyleSheet(QStringLiteral("QLabel { color: #e6e1e5; font-size: 11px; }"));
        hide();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(30, 34, 41, 246));
        painter.drawPath(superellipsePath(QRectF(rect()), 9.0, 4.0));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(64, 71, 82), 1.0));
        painter.drawPath(superellipsePath(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8.5, 4.0));
        QLabel::paintEvent(event);
    }
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
        // Opacity, as a 0-255 slider under the hue strip.  Its value rides on
        // the picker's result, so a colour chosen here can be translucent.
        auto *alphaRow = new QHBoxLayout;
        alphaRow->setSpacing(6);
        auto *alphaLabel = new QLabel(uiTr("Alpha"), this);
        alphaLabel->setObjectName(QStringLiteral("alphaLabel"));
        alphaRow->addWidget(alphaLabel);
        alphaSlider_ = new QSlider(Qt::Horizontal, this);
        alphaSlider_->setObjectName(QStringLiteral("alphaSlider"));
        alphaSlider_->setRange(0, 255);
        alphaSlider_->setValue(255);
        alphaSlider_->setFocusPolicy(Qt::NoFocus);
        alphaSlider_->setCursor(Qt::PointingHandCursor);
        alphaSlider_->setToolTip(uiTr("Opacity (0-255)"));
        connect(alphaSlider_, &QSlider::valueChanged, this, [this](int) { syncPreview(); });
        alphaRow->addWidget(alphaSlider_, 1);
        alphaValue_ = new QLabel(this);
        alphaValue_->setObjectName(QStringLiteral("alphaValue"));
        alphaValue_->setFixedWidth(34);
        alphaValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        alphaRow->addWidget(alphaValue_);
        layout->addLayout(alphaRow);
        auto *row = new QHBoxLayout;
        row->setSpacing(4);
        preview_ = new QLabel(this);
        preview_->setObjectName(QStringLiteral("colorPreview"));
        preview_->setFixedSize(20, 20);
        row->addWidget(preview_);
        hexEdit_ = new QLineEdit(this);
        hexEdit_->setObjectName(QStringLiteral("colorHexEdit"));
        hexEdit_->setFixedWidth(72);
        hexEdit_->setMaxLength(9);
        hexEdit_->setToolTip(uiTr("Hex color (#rrggbb or #rrggbbaa)"));
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
            "QLabel#alphaLabel { color: #c3c9d1; font-size: 11px; } "
            "QLabel#alphaValue { color: #e6e1e5; font-size: 11px; } "
            "QSlider { min-height: 20px; background: transparent; } "
            "QSlider::groove:horizontal { height: 4px; background: #4a515c; "
            "border-radius: 2px; } "
            "QSlider::sub-page:horizontal { background: #dde1ff; border-radius: 2px; } "
            "QSlider::add-page:horizontal { background: #4a515c; border-radius: 2px; } "
            "QSlider::handle:horizontal { width: 14px; margin: -5px 0; "
            "background: #dde1ff; border: 0; border-radius: 7px; } "
            "QSlider::handle:horizontal:hover { background: #e9ecff; } "
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

    // Told when the picker opens and closes so the pin editor can widen its
    // input region over it: the popup reaches past the toolbar's own rect, and
    // a region that stopped at the toolbar would paint the picker but send its
    // clicks to the desktop behind.
    void setVisibilityCallback(std::function<void()> callback)
    {
        visibilityChanged_ = std::move(callback);
    }

    void openAt(const QColor &color, const QPoint &topLeft)
    {
        setHsvFrom(color);
        move(topLeft);
        show();
        raise();
        if (visibilityChanged_) {
            visibilityChanged_();
        }
    }

    QColor pickedColor() const
    {
        qreal hue = std::fmod(hue_, 1.0);
        if (hue < 0.0) {
            hue += 1.0;
        }
        const qreal alpha = alphaSlider_ != nullptr ? alphaSlider_->value() / 255.0 : 1.0;
        return QColor::fromHsvF(hue, std::clamp(sat_, 0.0, 1.0), std::clamp(val_, 0.0, 1.0),
                                alpha);
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
        if (visibilityChanged_) {
            visibilityChanged_();
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
    std::function<void()> visibilityChanged_;
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
        if (alphaSlider_ != nullptr) {
            // Blocked so loading a colour does not echo back as a user edit;
            // the preview is refreshed below either way.
            const QSignalBlocker blocker(alphaSlider_);
            alphaSlider_->setValue(color.alpha());
        }
        satVal_->setHue(hue_);
        satVal_->setSatVal(sat_, val_);
        huePane_->setHue(hue_);
        syncPreview();
    }

    void applyHex()
    {
        // Parsed through the config spelling rather than `QColor(QString)`:
        // Qt reads an eight-digit literal as `#aarrggbb`, so `#ff880080` --
        // which this field and the config file both call semi-transparent
        // orange -- would come out the wrong way round.
        const QColor parsed = parseColorText(hexEdit_->text());
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
        if (alphaValue_ != nullptr) {
            alphaValue_->setText(
                QStringLiteral("%1%").arg(qRound(color.alpha() * 100.0 / 255.0)));
        }
        const QSignalBlocker blocker(hexEdit_);
        // The same spelling the protocol carries and the field accepts:
        // `#rrggbb` while opaque, `#rrggbbaa` once alpha is lifted off full.
        hexEdit_->setText(colorText(color).toUpper());
    }

    std::function<void(const QColor &)> apply_;
    SatValPane *satVal_ = nullptr;
    HuePane *huePane_ = nullptr;
    QLabel *preview_ = nullptr;
    QLineEdit *hexEdit_ = nullptr;
    QSlider *alphaSlider_ = nullptr;
    QLabel *alphaValue_ = nullptr;
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

    // Told when the picker opens and closes so the pin editor can widen its
    // input region over it; see the colour picker's own.
    void setVisibilityCallback(std::function<void()> callback)
    {
        visibilityChanged_ = std::move(callback);
    }

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
        if (visibilityChanged_) {
            visibilityChanged_();
        }
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
        if (visibilityChanged_) {
            visibilityChanged_();
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
    std::function<void()> visibilityChanged_;
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

// An image read out of the clipboard, plus where it came from when the
// clipboard named a file rather than carrying pixels.
struct ClipboardImage {
    bool installed = true; // `wl-paste` could be run at all
    bool offered = false;  // something is copied
    QImage image;
    QString source;
};

// How long a clipboard helper is given to start and to answer.  Both `wl-paste`
// and `wl-copy` are small programs that either answer at once or not at all.
constexpr int kClipboardProcessTimeoutMs = 5000;

// One `wl-paste` run. `false` means the program could not be started at all,
// which is a different failure from an empty clipboard; `ok` says whether the
// request itself succeeded.
bool runWlPaste(const QStringList &arguments, QByteArray *bytes, bool *ok)
{
    constexpr int kTimeoutMs = 5000;
    QProcess process;
    process.setProgram(QStringLiteral("wl-paste"));
    process.setArguments(arguments);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start();
    if (!process.waitForStarted(kTimeoutMs)) {
        return false;
    }
    const bool finished = process.waitForFinished(kTimeoutMs);
    if (!finished) {
        // A clipboard owner that never answers must not hold the editor's
        // event loop any longer than this.
        process.kill();
        process.waitForFinished(kTimeoutMs);
    }
    *bytes = process.readAllStandardOutput();
    *ok = finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    return true;
}

// Puts `text` on the clipboard through `wl-copy`, the writing counterpart of
// the `wl-paste` above -- and, like it, used instead of Qt's own clipboard,
// which is unreliable under this compositor setup.  The daemon has its own
// copy of this; the two processes share no code.
//
// `wl-copy` forks and the child stays alive as the selection owner, so only
// the short-lived process started here is waited for: the copy outlives the
// call.
bool runWlCopy(const QString &text)
{
    constexpr int kTimeoutMs = 5000;
    QProcess process;
    process.setProgram(QStringLiteral("wl-copy"));
    // `--` ends the options: a value is content, never a switch.
    process.setArguments({QStringLiteral("--")});
    process.start();
    if (!process.waitForStarted(kTimeoutMs)) {
        return false;
    }
    process.write(text.toUtf8());
    process.closeWriteChannel();
    const bool finished = process.waitForFinished(kTimeoutMs);
    if (!finished) {
        process.kill();
        process.waitForFinished(kTimeoutMs);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

// The same, for pixels.  `wl-copy --type image/png` reads the encoding from the
// bytes themselves, so nothing has to be declared beyond the type, and the
// result is an image a paste target can take rather than a file it has to be
// told about.
bool runWlCopyImage(const QImage &image)
{
    constexpr int kTimeoutMs = 5000;
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
        return false;
    }
    buffer.close();
    QProcess process;
    process.setProgram(QStringLiteral("wl-copy"));
    process.setArguments({QStringLiteral("--type"), QStringLiteral("image/png")});
    process.start();
    if (!process.waitForStarted(kTimeoutMs)) {
        return false;
    }
    process.write(bytes);
    process.closeWriteChannel();
    const bool finished = process.waitForFinished(kTimeoutMs);
    if (!finished) {
        process.kill();
        process.waitForFinished(kTimeoutMs);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

// The image encodings worth asking for, best first; any other `image/*` the
// clipboard offers is taken after these.
constexpr const char *kClipboardImageTypes[] = {
    "image/png", "image/jpeg", "image/webp", "image/bmp", "image/tiff",
};
// Reads an image out of the clipboard the way the pin daemon does: through
// `wl-paste`, not Qt's own clipboard. Qt implements only the wlroots
// `zwlr_data_control_v1`, which a compositor offering the standardized
// `ext_data_control_manager_v1` instead (KWin) leaves empty.
//
// Resolution order: image data first, then a copied file -- as a URI list, then
// as a plain path. Copying a file in a file manager is the ordinary way to say
// "this picture", and it puts a path on the clipboard rather than pixels.
ClipboardImage readClipboardImage()
{
    ClipboardImage result;
    QByteArray listed;
    bool ok = false;
    if (!runWlPaste({QStringLiteral("--list-types")}, &listed, &ok)) {
        result.installed = false;
        return result;
    }
    if (!ok) {
        return result; // nothing is copied
    }
    result.offered = true;
    QStringList types;
    for (const QByteArray &line : listed.split('\n')) {
        const QString type = QString::fromUtf8(line).trimmed();
        if (!type.isEmpty() && !types.contains(type)) {
            types.append(type);
        }
    }
    const auto fetch = [&types](const QString &type) -> QByteArray {
        if (!types.contains(type)) {
            return QByteArray();
        }
        QByteArray bytes;
        bool fetched = false;
        if (!runWlPaste({QStringLiteral("--type"), type, QStringLiteral("--no-newline")}, &bytes,
                        &fetched)
            || !fetched) {
            return QByteArray();
        }
        return bytes;
    };

    QString imageType;
    for (const char *candidate : kClipboardImageTypes) {
        const QString type = QLatin1String(candidate);
        if (types.contains(type)) {
            imageType = type;
            break;
        }
    }
    if (imageType.isEmpty()) {
        for (const QString &type : types) {
            if (type.startsWith(QLatin1String("image/"))) {
                imageType = type;
                break;
            }
        }
    }
    if (!imageType.isEmpty()) {
        const QByteArray bytes = fetch(imageType);
        if (!bytes.isEmpty()) {
            result.image = QImage::fromData(bytes);
            if (!result.image.isNull()) {
                return result;
            }
        }
    }

    // A copied file: a file manager offers `text/uri-list`, and a terminal
    // that copies a path offers plain text.
    QStringList candidates;
    for (const QUrl &url : QUrl::fromStringList(
             QString::fromUtf8(fetch(QStringLiteral("text/uri-list")))
                 .split(QLatin1Char('\n'), Qt::SkipEmptyParts))) {
        if (url.isLocalFile()) {
            candidates.append(url.toLocalFile());
        }
    }
    for (const char *type : {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING"}) {
        const QString text = QString::fromUtf8(fetch(QLatin1String(type))).trimmed();
        if (!text.isEmpty() && !text.contains(QLatin1Char('\n'))) {
            candidates.append(text);
            break;
        }
    }
    for (const QString &path : candidates) {
        QImageReader reader(path);
        if (!reader.canRead()) {
            continue;
        }
        const QImage image = reader.read();
        if (!image.isNull()) {
            result.image = image;
            result.source = path;
            return result;
        }
    }
    return result;
}

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
        // The pen path.  It is the one gesture that outlives a release: a path
        // spans as many presses as it has anchors, so `release` leaves this
        // state standing until the path is closed or double-clicked.
        Bezier,
    };

    Type type = Type::None;
    Point anchor;
    Point current;
    LogicalRect origin;
    int handle = 0;
    // Alt was down when the handle was grabbed.  It is read once, at the press:
    // a resize that changed its mind halfway through would jump, and the key is
    // held for the whole drag in practice anyway.
    bool preserveAspect = false;
    QVector<Point> points;
    // The in-progress freehand stroke, rasterized incrementally.  Re-stroking
    // the whole path on every paint is O(points) each time -- quadratic over a
    // long scribble -- so each paint adds only the points appended since the
    // last one and blits the accumulated image instead.
    QImage liveRaster;
    QPoint liveOrigin;
    int liveOutput = -1;
    int liveBaked = 0;     // points already in liveRaster
    double liveLength = 0.0; // local path length up to the last baked point
    QByteArray liveKey;    // style/output/size the raster was built for
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
            "QPushButton#undoButton:enabled, QPushButton#redoButton:enabled { "
            "background: #2e353f; } "
            "QPushButton#undoButton:enabled:hover, QPushButton#redoButton:enabled:hover { "
            "background: #3a424e; } "
            "QPushButton#undoButton:disabled, QPushButton#redoButton:disabled { "
            "background: transparent; } "
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
            "background: #3a414b; border-radius: 4px; } ")
            // The two ends of the capture are the only buttons on the card whose
            // width is fixed rather than asked for, so their padding is the one
            // thing that can leave a label wider than its own box: "Cancel" at
            // the generic ten pixels either side wants more than the block is
            // given, and its last letters are cut off.  The block's width is
            // measured from these two hints below, so the padding here and the
            // size the corner is drawn at cannot drift apart.
            + QStringLiteral("QPushButton#confirmButton, QPushButton#cancelButton "
                             "{ padding: 0 %1px; }")
                  .arg(kEndsPadding));

        auto *rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(6, 5, 6, 6);
        rootLayout->setSpacing(4);

        auto *toolSurface = new ToolCardFrame(this);
        toolSurface->setObjectName(QStringLiteral("toolbarCommandSurface"));
        toolSurface->setCursor(Qt::ArrowCursor);
        commandSurface_ = toolSurface;
        // The command surface is two rows of buttons with the four that end or
        // step back the capture pinned to its right-hand edge, one row each:
        // undo over redo, OK over Cancel.
        //
        // It was one row until the buttons filled it: twelve drawing tools, the
        // one-shot actions, the history pair and the two ends of the capture came
        // to about a thousand logical pixels, which is most of a 1080p output and
        // wider than the panels it has to sit beside -- a bar that long can only
        // be clamped against the screen edge, and there it reads as a band across
        // the capture rather than a panel on it.  So the buttons took two rows.
        //
        // Which button goes in which row is not "the tools here, the actions
        // there": the tools alone are about three times the width of the actions,
        // so a split along that line leaves the first row setting the card's
        // width while two thirds of the second sits empty -- and every tool added
        // since makes the first row longer and the second no shorter.  The rows
        // are filled in order and split where they come out closest to the same
        // length instead, which is both what keeps the card as narrow as two rows
        // can make it and what keeps the two rows from drifting apart as buttons
        // are added.  See `placeCommandRows`.
        //
        // The ends had to be somewhere, and a row of the column was the wrong
        // place for them twice over: it made that row the height of whatever it
        // held while leaving a stretch of empty card beside the buttons, and the
        // buttons themselves were the four the eye goes to last, at the far end
        // of the longest row.  Pinned to the right they are always in the same
        // place -- the corner of the card, which is where a confirm and a cancel
        // are looked for -- and the two rows to their left are one column whose
        // width no longer depends on how long the rows happen to be.
        auto *cardLayout = new QHBoxLayout(toolSurface);
        cardLayout->setContentsMargins(2, 2, 2, 2);
        cardLayout->setSpacing(kRowSpacing);
        auto *commandColumn = new QVBoxLayout();
        commandColumn->setSpacing(kRowSpacing);
        auto *firstRow = new QHBoxLayout();
        firstRow->setSpacing(kRowSpacing);
        auto *secondRow = new QHBoxLayout();
        secondRow->setSpacing(kRowSpacing);
        commandColumn->addLayout(firstRow);
        commandColumn->addLayout(secondRow);
        cardLayout->addLayout(commandColumn);
        addTool(Tool::Rectangle);
        addTool(Tool::Ellipse);
        addTool(Tool::Arrow);
        addTool(Tool::Line);
        addTool(Tool::Wave);
        addTool(Tool::Bezier);
        addTool(Tool::Pen);
        addTool(Tool::Text);
        addTool(Tool::Number);
        addTool(Tool::Mosaic);
        // The eyedropper closes the list of tools: it draws nothing itself, it
        // reads the pixel under the click and hands the colour to the tool it was
        // armed from, which is where the pick leaves the session.
        addTool(Tool::Picker);
        // The eleven are in, so their one size can be measured from their own
        // labels; the actions that follow are built at it, which is why this
        // stands between the last tool and the first action.
        sizeToolButtons();
        // The actions are the same kind of thing to click as the tools they
        // follow -- same box, same label under the same icon -- and they are not
        // modes: nothing stays selected, so they are kept out of `toolButtons_`,
        // which is what the active-state pass walks.  They are placed in the same
        // order they are built in, after the tools, by `placeCommandRows` once
        // the last of them is in.
        //
        // Paste takes an image off disk through the file dialog; Ctrl+V takes
        // whatever is on the clipboard.  Both land in the same paste.
        auto *paste = addToolAction(uiTr("Image"), pasteIcon(QColor(230, 225, 229),
                                                             devicePixelRatioF()),
                                    uiTr("Paste an image onto the capture (Ctrl+V for the "
                                         "clipboard)"),
                                    QStringLiteral("pasteButton"));
        connect(paste, &QToolButton::clicked, [controller = controller_] {
            QString error;
            if (!controller->pasteFromFile(&error)) {
                std::fprintf(stderr, "vshot-qt-ui: %s\n", error.toUtf8().constData());
                std::fflush(stderr);
            }
        });
        // Text selection: the recognized characters of the selection are drawn
        // where they were and the pointer selects a range of them, then the
        // range is copied.  The mode lives in the controller -- the toolbar has
        // no selection to work on -- so the button only asks for it, and the
        // controller reports the result back through the callback below.
        auto *text = addToolAction(uiTr("Text+"),
                                   recognizeTextIcon(QColor(230, 225, 229), devicePixelRatioF()),
                                   uiTr("Select the text in the selection and copy what you "
                                        "select"),
                                   QStringLiteral("ocrButton"),
                                   {uiTr("OCR…"), uiTr("Copied"), uiTr("Failed")});
        textButton_ = text;
        connect(text, &QToolButton::clicked, [controller = controller_] {
            controller->beginTextSelection(nullptr);
        });
        // The result is reported where the user is looking: the button itself,
        // which is the thing they just clicked.  The copy can be triggered by a
        // key, which the controller sees and this toolbar does not, so the
        // controller reports through this callback rather than the handler
        // above.  The width was settled for every label it can show when it was
        // built, so the word is not elided now.  `Busy` and `Idle` are the two
        // reports that do not expire: the first is replaced by the outcome that
        // follows it, the second is the button's own label coming back.
        controller_->setTextResultCallback([this](TextOutcome outcome, const QString &) {
            if (textButton_ == nullptr) {
                return;
            }
            switch (outcome) {
            case TextOutcome::Busy:
                // Short on purpose: the button is sized once, for every label
                // it can ever show, and a long word here would widen it past
                // every other button in the row for good.
                textButton_->setText(uiTr("OCR…"));
                return;
            case TextOutcome::Idle:
                textButton_->setText(uiTr("Text+"));
                return;
            case TextOutcome::Copied:
                textButton_->setText(uiTr("Copied"));
                break;
            case TextOutcome::Failed:
                textButton_->setText(uiTr("Failed"));
                break;
            }
            QTimer::singleShot(1200, textButton_, [this] {
                if (controller_->isFinished() || controller_->isCancelled()) {
                    return;
                }
                textButton_->setText(uiTr("Text+"));
            });
        });
        // Screenshot translation: the same selection the text button reads is
        // recognized and then translated, and the translation is drawn over the
        // text it replaces.  It is a one-shot action like `Text+`, so it is not
        // a mode and stays out of `toolButtons_`; the controller reports its
        // two-subprocess wait and its outcome through the callback below.
        auto *translate = addToolAction(
            uiTr("Translate"),
            translateIcon(QColor(230, 225, 229), devicePixelRatioF()),
            uiTr("Translate the text in the selection and draw it in place"),
            QStringLiteral("translateButton"),
            {uiTr("Translating…"), uiTr("Failed")});
        translateButton_ = translate;
        connect(translate, &QToolButton::clicked, [controller = controller_] {
            controller->translateSelection(nullptr);
        });
        controller_->setTranslateResultCallback([this](TextOutcome outcome, const QString &) {
            if (translateButton_ == nullptr) {
                return;
            }
            switch (outcome) {
            case TextOutcome::Busy:
                translateButton_->setText(uiTr("Translating…"));
                return;
            case TextOutcome::Idle:
                translateButton_->setText(uiTr("Translate"));
                return;
            case TextOutcome::Copied:
                break;
            case TextOutcome::Failed:
                translateButton_->setText(uiTr("Failed"));
                break;
            }
            QTimer::singleShot(1200, translateButton_, [this] {
                if (controller_->isFinished() || controller_->isCancelled()) {
                    return;
                }
                translateButton_->setText(uiTr("Translate"));
            });
        });
        // Scrolling capture: the region the user drew is scrolled with
        // synthetic wheels and stitched into one tall image.  It is not a mode
        // -- nothing stays selected -- and it is not the ordinary confirmation
        // either: it ends the session, and the CLI reads the answer as "scroll
        // this, do not keep this frame".  Only the region editor is offered
        // it: window editing and the pin editor reuse this toolbar on a
        // picture that has nothing to scroll, and a button that can never be
        // pressed is worse than no button at all.  Whether it can be pressed
        // right now -- the selection has to fit in one output -- is
        // `syncState`'s to say.
        if (controller_->longAllowed_) {
            longButton_ = addToolAction(
                uiTr("Scroll"),
                scrollIcon(QColor(230, 225, 229), devicePixelRatioF()),
                uiTr("Scroll the selection and stitch it into one tall image"),
                QStringLiteral("longButton"));
            connect(longButton_, &QToolButton::clicked,
                    [controller = controller_] { controller->requestLongCapture(); });
        }
        // Pinning finishes the session the way OK does, so it sits with the
        // capture's own actions rather than in the corner: it is a tool-shaped
        // button like the ones it is laid out among, and the corner is the four
        // text-shaped ends.  The pin editor hides it, being a pin already.
        pinButton_ = addToolAction(uiTr("Pin"),
                                   pinIcon(QColor(230, 225, 229), devicePixelRatioF()),
                                   uiTr("Pin the result on the screen"),
                                   QStringLiteral("pinButton"));
        connect(pinButton_, &QToolButton::clicked,
                [controller = controller_] { controller->pin(); });
        // The last button is in, so the two rows can be filled: this is the one
        // place the split between them is decided, and it needs every button's
        // own size to decide it.
        placeCommandRows(firstRow, secondRow);
        // The slack of the card -- what the style row below is wider than the
        // two rows are -- is taken up here, between the column and the ends, so
        // the buttons stay where the pointer left them and the ends stay in the
        // corner however wide the panel turns out to be.
        cardLayout->addStretch(1);
        // One divider for the whole height of the block rather than one per row:
        // it separates the two rows from the four ends, and there is one gap to
        // read rather than two.  Its height is the two rows and the gap between
        // them, which is the column's own spacing.
        auto *endsDivider = new QFrame(toolSurface);
        endsDivider->setObjectName(QStringLiteral("toolbarDivider"));
        endsDivider->setFrameShape(QFrame::VLine);
        endsDivider->setFrameShadow(QFrame::Plain);
        endsDivider->setFixedHeight(toolButtonHeight_ * 2 + kRowSpacing);
        endsDivider->setCursor(Qt::ArrowCursor);
        cardLayout->addWidget(endsDivider);
        // The four ends, the history pair over the two ends of the capture:
        // undo and redo on the first row, OK and Cancel under them.  Two rows of
        // two, so the block is exactly as tall as the two rows beside it, and
        // Cancel, which ends the capture either way, lands in the corner it is
        // looked for in.
        //
        // The history pair is built like the tools on the left -- same box, same
        // label under the same icon, same hover and press painting -- because it
        // is the same kind of thing to click.  The two ends of the capture are
        // not: one of them is the primary button and reads as one, and neither
        // wants a label, so they stay the text buttons they are.
        auto *endsGrid = new QGridLayout();
        endsGrid->setContentsMargins(0, 0, 0, 0);
        // Two rows a hair apart like the rows beside them, but the two buttons of
        // a row further apart than that: at the style's own 2 px the two ends of
        // the capture read as one box with a line down it.
        endsGrid->setVerticalSpacing(2);
        endsGrid->setHorizontalSpacing(kEndsSpacing);
        undo_ = makeToolButton(uiTr("Undo"), historyIcon(false, QColor(QStringLiteral("#dfe4ec"))),
                               controller_->shortcutHint(ShortcutAction::Undo,
                                                         uiTr("Undo last change")),
                               QStringLiteral("undoButton"));
        connect(undo_, &QToolButton::clicked, [controller = controller_] { controller->undo(); });
        redo_ = makeToolButton(uiTr("Redo"), historyIcon(true, QColor(QStringLiteral("#dfe4ec"))),
                               controller_->shortcutHint(ShortcutAction::Redo,
                                                         uiTr("Redo last change")),
                               QStringLiteral("redoButton"));
        connect(redo_, &QToolButton::clicked, [controller = controller_] { controller->redo(); });
        auto *ok = addActionButton(uiTr("OK"));
        ok->setObjectName(QStringLiteral("confirmButton"));
        ok->setToolTip(controller_->shortcutHint(ShortcutAction::Confirm, uiTr("Confirm capture")));
        connect(ok, &QPushButton::clicked, [controller = controller_] { controller->confirm(); });
        auto *cancel = addActionButton(uiTr("Cancel"));
        cancel->setObjectName(QStringLiteral("cancelButton"));
        cancel->setToolTip(controller_->shortcutHint(ShortcutAction::Cancel,
                                                     uiTr("Discard capture")));
        connect(cancel, &QPushButton::clicked, [controller = controller_] { controller->cancel(); });
        // The width of the whole block: the wider of the two ends' own hints,
        // which the style already sizes to its label plus the padding the rule
        // above gives these two.  Summing the label and the padding here by
        // hand was the same arithmetic a second time, and it is the copy that
        // went stale -- the labels were measured in a font that had not been
        // polished yet and the padding was the panel's generic ten rather than
        // the seven the box is drawn at, so "Cancel" came out wider than the
        // button it was drawn in and the ends were clipped.  The hint cannot
        // drift from either: it is the box.  The history pair above takes the
        // same width, so the columns of the grid are the buttons themselves
        // rather than the buttons and a strip of slack.
        //
        // The two ends are not the height of the rows beside them either: a text
        // button stretched to 41 px is a slab with a small word in it, which is
        // what the corner looked like before.  The history pair is the tool box,
        // so the cells are as tall as the rows and each button sits centred in
        // its own.
        ok->ensurePolished();
        cancel->ensurePolished();
        const int endsWidth = std::max(ok->sizeHint().width(), cancel->sizeHint().width());
        const int endsHeight = std::max(ok->sizeHint().height(), cancel->sizeHint().height());
        for (QPushButton *button : {ok, cancel}) {
            button->setFixedSize(endsWidth, endsHeight);
        }
        undo_->setFixedWidth(endsWidth);
        redo_->setFixedWidth(endsWidth);
        endsGrid->setRowMinimumHeight(0, toolButtonHeight_);
        endsGrid->setRowMinimumHeight(1, toolButtonHeight_);
        endsGrid->addWidget(undo_, 0, 0, Qt::AlignCenter);
        endsGrid->addWidget(redo_, 0, 1, Qt::AlignCenter);
        endsGrid->addWidget(ok, 1, 0, Qt::AlignCenter);
        endsGrid->addWidget(cancel, 1, 1, Qt::AlignCenter);
        // And a gap in front of the block as wide as the one the panel's own
        // margin leaves behind it: the four buttons pressed up against the
        // divider read as though the line were cutting into them, while the
        // corner has room to spare.
        cardLayout->addSpacing(kEndsGap);
        cardLayout->addLayout(endsGrid);
        // Every button on the card gets the frame's hover and press painting; it
        // finds them by type, so this has to run after the last one was added --
        // the history pair among the ends included.
        for (QToolButton *button : toolSurface->findChildren<QToolButton *>()) {
            button->installEventFilter(toolSurface);
        }
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
            // A swatch carries no alpha of its own: clicking one changes the
            // RGB and leaves the user's opacity where they set it, so picking a
            // translucent pen and then a colour does not silently make it opaque.
            connect(swatch, &QPushButton::clicked, [controller = controller_, swatchColor] {
                QColor chosen = swatchColor;
                chosen.setAlpha(
                    controller->toolStyle(controller->styleTargetTool()).color.alpha());
                controller->setCurrentColor(chosen);
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
        widthSlider_->setValue(
            static_cast<int>(controller_->toolStyle(controller_->styleTargetTool()).width));
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
        textSpin_->setRange(kMinTextPixels, kMaxTextPixels);
        textSpin_->setSingleStep(1);
        textSpin_->setKeyboardTracking(false);
        textSpin_->setFixedSize(64, 22);
        textSpin_->setValue(clampTextPixels(static_cast<int>(controller_->textSize_)));
        textSpin_->setToolTip(uiTr("Text size in pixels (7-448)"));
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

        // Number badge styles: the four looks the number tool can place.  One
        // segment per look, shown only while the number tool or a placed badge
        // is the style target.
        numberGroup_ = addGroup(optionLayout, true);
        numberGroup_->setObjectName(QStringLiteral("numberGroup"));
        // "Disc", not "Solid" or "Fill": the line-style row already owns
        // "Solid" and the pen's fill mode owns "Fill", so either word would
        // collide in the translation table and one row would show the other's
        // text.
        addStyleButtons(numberGroup_,
                        {uiTr("Disc"), uiTr("Ring"), uiTr("Square"), uiTr("Plain")},
                        uiTr("Number style"),
                        {numberStyleValue(NumberStyle::FilledCircle),
                         numberStyleValue(NumberStyle::Ring),
                         numberStyleValue(NumberStyle::Square),
                         numberStyleValue(NumberStyle::Plain)},
                        &numberButtons_, &numberValues_,
                        [controller = controller_](QString value) {
                            controller->setNumberStyle(numberStyleForName(value));
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

        // The colour's opacity, on the bar rather than behind the picker button:
        // it is a number, so it belongs with the others.
        alphaGroup_ = addGroup(numericLayout);
        alphaGroup_->setObjectName(QStringLiteral("alphaGroup"));
        alphaGroup_->setProperty("toolbarGroupKind", "numeric");
        alphaLabel_ = new QLabel(uiTr("Alpha"), alphaGroup_);
        alphaLabel_->setFixedWidth(
            QFontMetrics(alphaLabel_->font()).horizontalAdvance(uiTr("Alpha %1").arg(255)));
        alphaSlider_ = new QSlider(Qt::Horizontal, alphaGroup_);
        alphaSlider_->setObjectName(QStringLiteral("colorAlphaSlider"));
        alphaSlider_->setRange(0, 255);
        alphaSlider_->setSingleStep(1);
        alphaSlider_->setPageStep(16);
        alphaSlider_->setFixedWidth(72);
        alphaSlider_->setValue(
            controller_->toolStyle(controller_->styleTargetTool()).color.alpha());
        alphaSlider_->setToolTip(uiTr("Color opacity (0-255)"));
        alphaGroup_->layout()->addWidget(alphaLabel_);
        alphaGroup_->layout()->addWidget(alphaSlider_);

        // The wave's own shape: how far a crest leaves the line its two points
        // describe, and how long one full period is, both in logical pixels.
        amplitudeGroup_ = addGroup(numericLayout);
        amplitudeGroup_->setObjectName(QStringLiteral("amplitudeGroup"));
        amplitudeGroup_->setProperty("toolbarGroupKind", "numeric");
        amplitudeLabel_ = new QLabel(uiTr("Amplitude"), amplitudeGroup_);
        amplitudeLabel_->setFixedWidth(QFontMetrics(amplitudeLabel_->font())
                                           .horizontalAdvance(uiTr("Amplitude %1").arg(64)));
        amplitudeSlider_ = new QSlider(Qt::Horizontal, amplitudeGroup_);
        amplitudeSlider_->setObjectName(QStringLiteral("waveAmplitudeSlider"));
        amplitudeSlider_->setRange(1, 64);
        amplitudeSlider_->setSingleStep(1);
        amplitudeSlider_->setFixedWidth(80);
        amplitudeSlider_->setValue(
            static_cast<int>(controller_->toolStyle(QStringLiteral("wave")).amplitude));
        amplitudeSlider_->setToolTip(uiTr("Wave height (1-64 logical pixels)"));
        amplitudeGroup_->layout()->addWidget(amplitudeLabel_);
        amplitudeGroup_->layout()->addWidget(amplitudeSlider_);

        wavelengthGroup_ = addGroup(numericLayout);
        wavelengthGroup_->setObjectName(QStringLiteral("wavelengthGroup"));
        wavelengthGroup_->setProperty("toolbarGroupKind", "numeric");
        wavelengthLabel_ = new QLabel(uiTr("Wavelength"), wavelengthGroup_);
        wavelengthLabel_->setFixedWidth(QFontMetrics(wavelengthLabel_->font())
                                            .horizontalAdvance(uiTr("Wavelength %1").arg(256)));
        wavelengthSlider_ = new QSlider(Qt::Horizontal, wavelengthGroup_);
        wavelengthSlider_->setObjectName(QStringLiteral("waveWavelengthSlider"));
        wavelengthSlider_->setRange(6, 256);
        wavelengthSlider_->setSingleStep(1);
        wavelengthSlider_->setFixedWidth(80);
        wavelengthSlider_->setValue(
            static_cast<int>(controller_->toolStyle(QStringLiteral("wave")).wavelength));
        wavelengthSlider_->setToolTip(uiTr("Wave period (6-256 logical pixels)"));
        wavelengthGroup_->layout()->addWidget(wavelengthLabel_);
        wavelengthGroup_->layout()->addWidget(wavelengthSlider_);

        // How the pen path is painted.  "Outline", not "Stroke": the settings
        // window already owns that word for its line-style card, and a shared
        // key would give one of the two the other's translation.
        fillGroup_ = addGroup(optionLayout, true);
        fillGroup_->setObjectName(QStringLiteral("fillGroup"));
        addStyleButtons(fillGroup_, {uiTr("Outline"), uiTr("Fill"), uiTr("Both")},
                        uiTr("Pen fill mode"),
                        {QStringLiteral("stroke"), QStringLiteral("fill"),
                         QStringLiteral("both")},
                        &fillButtons_, &fillValues_,
                        [controller = controller_](QString value) {
                            controller->setFill(value);
                        });

        styleLayout->addWidget(optionsRow_);
        styleLayout->addWidget(numericRow_);
        rootLayout->addWidget(styleRow_);
        // One line for the tools whose gesture is not obvious from a single
        // drag.  It is hidden for every other tool, so the panel stays as short
        // as it was.
        usageHint_ = new QLabel(this);
        usageHint_->setObjectName(QStringLiteral("usageHint"));
        usageHint_->setVisible(false);
        rootLayout->addWidget(usageHint_);

        connect(widthSlider_, &QSlider::sliderPressed, this,
                [controller = controller_] { controller->beginStyleAdjustment(); });
        connect(widthSlider_, &QSlider::sliderReleased, this,
                [controller = controller_] { controller->endStyleAdjustment(); });
        // The one slider serves two numbers: the stroke width of the tools that
        // draw with it, and -- while the number tool or a placed badge is the
        // style target -- the badge's diameter.  Which one it means follows from
        // the target, the same answer `syncState` takes the label and the range
        // from, so the two can never disagree about what the value is.
        connect(widthSlider_, &QSlider::valueChanged, this,
                [controller = controller_](int value) {
                    if (controller->styleTargetTool() == QStringLiteral("number")) {
                        controller->setNumberSize(static_cast<std::uint32_t>(value));
                    } else {
                        controller->setWidth(static_cast<std::uint32_t>(value));
                    }
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
        connect(alphaSlider_, &QSlider::sliderPressed, this,
                [controller = controller_] { controller->beginStyleAdjustment(); });
        connect(alphaSlider_, &QSlider::sliderReleased, this,
                [controller = controller_] { controller->endStyleAdjustment(); });
        // The slider carries opacity alone; the colour it belongs to is whatever
        // the style target already has, so dragging it never moves the hue.
        connect(alphaSlider_, &QSlider::valueChanged, this,
                [controller = controller_](int value) {
                    QColor color = controller->toolStyle(controller->styleTargetTool()).color;
                    color.setAlpha(value);
                    controller->setCurrentColor(color);
                });
        connect(amplitudeSlider_, &QSlider::sliderPressed, this,
                [controller = controller_] { controller->beginStyleAdjustment(); });
        connect(amplitudeSlider_, &QSlider::sliderReleased, this,
                [controller = controller_] { controller->endStyleAdjustment(); });
        connect(amplitudeSlider_, &QSlider::valueChanged, this,
                [controller = controller_](int value) {
                    controller->setWaveAmplitude(static_cast<std::uint32_t>(value));
                });
        connect(wavelengthSlider_, &QSlider::sliderPressed, this,
                [controller = controller_] { controller->beginStyleAdjustment(); });
        connect(wavelengthSlider_, &QSlider::sliderReleased, this,
                [controller = controller_] { controller->endStyleAdjustment(); });
        connect(wavelengthSlider_, &QSlider::valueChanged, this,
                [controller = controller_](int value) {
                    controller->setWaveWavelength(static_cast<std::uint32_t>(value));
                });
        connect(textSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
                [controller = controller_](int value) {
                    controller->beginStyleAdjustment();
                    controller->setTextSize(static_cast<std::uint32_t>(clampTextPixels(value)));
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
        // An open picker is part of the chrome the pin editor has to keep
        // clickable, and it reaches past the toolbar's own rect.
        const auto popupVisibility = [controller = controller_] { controller->scheduleInputMask(); };
        pickerPopup_->setVisibilityCallback(popupVisibility);
        fontPopup_->setVisibilityCallback(popupVisibility);
        // The tooltips are drawn by the panel itself, not by Qt (see
        // `HoverTip`), so every event in this process is filtered to catch the
        // hover and turn it into one.
        qApp->installEventFilter(this);
        syncState();
    }

    // The windows this panel owns that are not part of its own rect: the two
    // pickers, which are parented to the overlay so they can reach past the
    // panel's clipped box.  The pin editor's input region has to include them
    // or an open picker would be painted but not clickable.
    QVector<QWidget *> popups() const
    {
        QVector<QWidget *> result;
        for (QWidget *popup : {static_cast<QWidget *>(pickerPopup_),
                               static_cast<QWidget *>(fontPopup_)}) {
            if (popup != nullptr) {
                result.push_back(popup);
            }
        }
        return result;
    }

    void syncState()
    {
        const qreal ratio = devicePixelRatioF();
        // The text mode is exclusive: a tool change would drop the recognized
        // layer, so the tools are not offered while it is on.  The Text+ button
        // itself stays enabled and shows the mode is up.
        const bool textMode = controller_->textMode_;
        for (int index = 0; index < toolButtons_.size(); ++index) {
            // An unarmed session lights no tool: the row says nothing is
            // selected, which is exactly the state a fresh capture starts in.
            const bool active = controller_->tool_.has_value() &&
                tools_.at(index) == *controller_->tool_;
            setToolButtonActive(toolButtons_.at(index), tools_.at(index), active, ratio);
            toolButtons_.at(index)->setEnabled(!textMode);
        }        if (textButton_ != nullptr) {
            setButtonActive(textButton_, textMode);
        }
        if (longButton_ != nullptr) {
            // The action needs a selection that fits in one output: a scroll
            // container never spans two monitors, and the CLI would refuse the
            // region anyway.  Saying so on the button beats a failure after
            // the fact.
            longButton_->setEnabled(!textMode && controller_->canRequestLongCapture());
        }
        if (translateButton_ != nullptr) {
            // Translation reads the same selection the text mode works on, so
            // it is out of reach while that mode is up.
            translateButton_->setEnabled(!textMode);
        }
        const Annotation *selected = nullptr;
        if (controller_->selectedAnnotation_ >= 0 &&
            controller_->selectedAnnotation_ < controller_->annotations_.size()) {
            selected = &controller_->annotations_[controller_->selectedAnnotation_];
        }
        undo_->setEnabled(!controller_->undoStack_.isEmpty());
        redo_->setEnabled(!controller_->redoStack_.isEmpty());
        // An available step is drawn in the toolbar's bright ink; one that is
        // not there is dimmed to a fraction of it.  Without this the two states
        // looked the same -- the icon is a pixmap, so the stylesheet's disabled
        // colour never reached it -- and the button said nothing about whether
        // pressing it would do anything.
        const QColor undoInk = undo_->isEnabled() ? kHistoryInk : kHistoryInkDimmed;
        const QColor redoInk = redo_->isEnabled() ? kHistoryInk : kHistoryInkDimmed;
        undo_->setIcon(historyIcon(false, undoInk, ratio));
        redo_->setIcon(historyIcon(true, redoInk, ratio));

        // The style row edits the selected annotation when one is active,
        // otherwise it edits the pending drawing style.
        const QString target = controller_->styleTargetTool();
        const bool shape = target == QStringLiteral("rectangle") ||
            target == QStringLiteral("ellipse");
        // Every tool that paints a stroked shape: the rectangle and ellipse
        // outlines, the arrow, the pen, the two segment tools and the bezier
        // pen.  They share the colour and width controls; the dash is only
        // meaningful to the tools that carry a dash pattern -- the wave is
        // sampled as a solid sine and a bezier path is stroked whole, so
        // neither is offered a dash control.
        const bool stroke = shape || target == QStringLiteral("arrow") ||
            target == QStringLiteral("pen") || target == QStringLiteral("line") ||
            target == QStringLiteral("wave") || target == QStringLiteral("bezier");
        const bool text = target == QStringLiteral("text");
        const bool mosaic = target == QStringLiteral("mosaic");
        // A numbered badge is a text annotation by wire but nothing like one on
        // the panel: the number tool shares the colour and width controls, and
        // takes the badge-style segments instead of the font and size boxes.
        const bool number = target == QStringLiteral("number");
        const bool mosaicBrush = mosaic &&
            (selected != nullptr ? selected->kind == Annotation::Kind::Stroke
                                 : controller_->mosaicShape_ == QStringLiteral("brush"));
        const bool selectedMosaicShape = selected != nullptr &&
            selected->kind == Annotation::Kind::Shape && mosaic;
        const bool showColor = stroke || text || number;
        const bool showDash = stroke && target != QStringLiteral("wave") &&
            target != QStringLiteral("bezier");
        const bool showArrowHead = target == QStringLiteral("arrow");
        const bool showWidth = stroke || mosaicBrush || number;
        const bool showTextSize = text;
        const bool showFont = text;
        const bool showArrowSize = target == QStringLiteral("arrow");
        const bool showMosaic = mosaic;
        const bool showStrength = mosaic;
        const bool showNumberStyle = number;
        // The wave draws with the colour like every other stroke, but its shape
        // is the wave's own business.
        const bool showWaveShape = target == QStringLiteral("wave");
        // How a pen path is painted, offered only for the pen.
        const bool showFill = target == QStringLiteral("bezier");
        colorGroup_->setVisible(showColor);
        fontGroup_->setVisible(showFont);
        dashGroup_->setVisible(showDash);
        arrowStyleGroup_->setVisible(showArrowHead);
        widthGroup_->setVisible(showWidth);
        textGroup_->setVisible(showTextSize);
        arrowGroup_->setVisible(showArrowSize);
        mosaicGroup_->setVisible(showMosaic);
        strengthGroup_->setVisible(showStrength);
        numberGroup_->setVisible(showNumberStyle);
        alphaGroup_->setVisible(showColor);
        fillGroup_->setVisible(showFill);
        amplitudeGroup_->setVisible(showWaveShape);
        wavelengthGroup_->setVisible(showWaveShape);
        // The width slider carries the badge's diameter while the number tool or
        // a placed badge is the target, and the range follows: a badge is
        // measured in tens of pixels, a stroke outline in single ones.
        {
            const QSignalBlocker rangeBlocker(widthSlider_);
            widthSlider_->setRange(number ? kNumberMinDiameter : 1,
                                   number ? kNumberMaxDiameter : 64);
        }
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
            {numberGroup_, showNumberStyle, true},   {fillGroup_, showFill, true},
            {alphaGroup_, showColor, false},         {widthGroup_, showWidth, false},
            {arrowGroup_, showArrowSize, false},     {textGroup_, showTextSize, false},
            {strengthGroup_, showStrength, false},   {amplitudeGroup_, showWaveShape, false},
            {wavelengthGroup_, showWaveShape, false},
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
        const bool anyOptionsGroup = showColor || showFont || showDash || showArrowHead ||
            showMosaic || showNumberStyle || showFill;
        const bool anyNumericGroup = showWidth || showArrowSize || showTextSize ||
            showStrength || showColor || showWaveShape;
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

        // The pin editor is already showing a pin: pinning again from here
        // would have nothing to mean, so the button is only in the capture
        // editor.
        if (pinButton_ != nullptr) {
            pinButton_->setVisible(!controller_->isPinEdit());
        }
        // The style the row shows while nothing is selected: the tool the row is
        // pointed at owns its colour and its numbers, so switching tools shows
        // that tool's values rather than one shared set.
        const ToolStyle &pendingStyle = controller_->toolStyle(controller_->styleTargetTool());
        const QColor color = selected != nullptr ? selected->color : pendingStyle.color;
        const QString dash = selected != nullptr ? selected->dash : controller_->currentDash_;
        const std::uint32_t width = selected != nullptr ? selected->width : pendingStyle.width;
        const std::uint32_t size = selected != nullptr ? selected->size : controller_->arrowSize_;
        const QString arrowStyle = selected != nullptr ? selected->arrowStyle
                                                        : controller_->currentArrowStyle_;
        const QString font = selected != nullptr ? selected->font : controller_->currentFont_;
        const std::uint32_t textSize = selected != nullptr ? selected->textPixels : controller_->textSize_;
        const QString mask = selected != nullptr
            ? (selected->kind == Annotation::Kind::Stroke ? QStringLiteral("brush") : selected->mask)
            : controller_->mosaicShape_;
        const std::uint32_t strength =
            selected != nullptr ? selected->strength : controller_->mosaicStrength_;
        const NumberStyle numberStyle =
            selected != nullptr && isNumberAnnotation(*selected) ? selected->numberStyle
                                                                 : controller_->numberStyle_;
        lastColor_ = color;
        // A swatch is "selected" when its RGB matches, whatever the current
        // alpha: the swatches carry no alpha, so a translucent version of a
        // palette colour is still that colour and must keep its ring.
        const bool customColor =
            std::none_of(swatchColors_.cbegin(), swatchColors_.cend(),
                         [&color](const QColor &swatch) { return swatch.rgb() == color.rgb(); });
        pickerButton_->setIcon(colorPickerIcon(customColor, ratio));
        for (int index = 0; index < swatchButtons_.size(); ++index) {
            swatchButtons_.at(index)->setIcon(swatchIcon(
                swatchColors_.at(index), swatchColors_.at(index).rgb() == color.rgb(), ratio));
        }
        syncToggleGroup(dashButtons_, dashValues_, dash);
        fontButton_->setText(font.isEmpty() ? QApplication::font().family() : font);
        syncToggleGroup(arrowStyleButtons_, arrowStyleValues_, arrowStyle);
        syncToggleGroup(mosaicButtons_, mosaicValues_, mask);
        syncToggleGroup(numberButtons_, numberValues_, numberStyleValue(numberStyle));
        // The number tool's button names the style that is armed, the way the
        // standalone toolbar's does; the badge it would place is what the style
        // row shows.
        for (int index = 0; index < tools_.size(); ++index) {
            if (tools_.at(index) == Tool::Number) {
                toolButtons_.at(index)->setToolTip(
                    uiTr("Number: %1 (click to place a number)")
                        .arg(numberStyleName(numberStyle)));
            }
        }
        const std::uint32_t numberSize = selected != nullptr && isNumberAnnotation(*selected)
            ? selected->numberSize
            : controller_->toolStyle(QStringLiteral("number")).numberSize;
        const bool waveSelected = selected != nullptr && selected->tool == QStringLiteral("wave");
        // A wave that was never tuned carries zeroes; the slider shows the shape
        // that is actually on screen, which is the derived one.
        const std::uint32_t amplitude = waveSelected
            ? static_cast<std::uint32_t>(std::lround(effectiveWaveAmplitude(*selected)))
            : pendingStyle.amplitude;
        const std::uint32_t wavelength = waveSelected
            ? static_cast<std::uint32_t>(std::lround(effectiveWaveWavelength(*selected)))
            : pendingStyle.wavelength;
        const QString fill = selected != nullptr && selected->tool == QStringLiteral("bezier")
            ? selected->fill
            : controller_->currentFill_;
        {
            const QSignalBlocker widthBlocker(widthSlider_);
            const QSignalBlocker arrowBlocker(arrowSlider_);
            const QSignalBlocker textBlocker(textSpin_);
            const QSignalBlocker strengthBlocker(strengthSlider_);
            const QSignalBlocker alphaBlocker(alphaSlider_);
            const QSignalBlocker amplitudeBlocker(amplitudeSlider_);
            const QSignalBlocker wavelengthBlocker(wavelengthSlider_);
            widthSlider_->setValue(number ? static_cast<int>(numberSize)
                                          : static_cast<int>(std::clamp(width, 1u, 64u)));
            arrowSlider_->setValue(static_cast<int>(std::clamp(size, 1u, 8u)));
            textSpin_->setValue(clampTextPixels(static_cast<int>(textSize)));
            strengthSlider_->setValue(static_cast<int>(std::clamp(strength, 1u, 3u)));
            alphaSlider_->setValue(color.alpha());
            amplitudeSlider_->setValue(static_cast<int>(std::clamp(amplitude, 1u, 64u)));
            wavelengthSlider_->setValue(static_cast<int>(std::clamp(wavelength, 6u, 256u)));
        }
        syncToggleGroup(fillButtons_, fillValues_, fill);
        // The width slider carries the badge's diameter while a badge is the
        // target, and its label has to say so.
        widthLabel_->setText(number ? uiTr("Size %1").arg(widthSlider_->value())
                                    : uiTr("Width %1").arg(widthSlider_->value()));
        arrowLabel_->setText(uiTr("Arrow %1").arg(arrowSlider_->value()));
        strengthLabel_->setText(uiTr("Mosaic %1").arg(strengthSlider_->value()));
        alphaLabel_->setText(uiTr("Alpha %1").arg(alphaSlider_->value()));
        amplitudeLabel_->setText(uiTr("Amplitude %1").arg(amplitudeSlider_->value()));
        wavelengthLabel_->setText(uiTr("Wavelength %1").arg(wavelengthSlider_->value()));
        // The pen and the wave each need a sentence: neither gesture reads off
        // its button, and a user said so.  The eyedropper needs one for the same
        // reason -- where the colour it takes will land is not visible on the
        // button -- and it is the one hint keyed off the armed tool rather than
        // off the style row's target, which the picker borrows from the tool it
        // is about to hand the colour to.
        const QString usage =
            controller_->tool_ == Tool::Picker
            ? uiTr("Click a pixel to take its color for the %1")
                  .arg(toolLabel(controller_->pickerTarget()))
            : (target == QStringLiteral("bezier")
                   ? uiTr("Click to drop an anchor, drag from it to bend the curve, click the "
                          "first anchor to close, double click to finish")
                   : (target == QStringLiteral("wave")
                          ? uiTr("Drag between two points, then shape the wave with Amplitude "
                                 "and Wavelength")
                          : QString()));
        usageHint_->setText(usage);
        usageHint_->setVisible(!usage.isEmpty());
        // While a label is being typed the size box must not take the keyboard:
        // clicking it would blur the editor the user is typing in.  A spin box
        // defaults to WheelFocus, so it is the one control on this bar whose
        // policy is flipped rather than set once.  With no editor open it keeps
        // the normal policy, so the value stays typeable.
        textSpin_->setFocusPolicy(controller_->textEdit_ != nullptr ? Qt::NoFocus
                                                                    : Qt::WheelFocus);
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
            // Settle the row order now: the controller reads the command bar's
            // size right after the flip, and may only realize the geometry on
            // the next event-loop pass otherwise.
            box->activate();
        }
    }

    bool styleRowAbove() const { return styleRowAbove_; }

    // Height of the command bar itself (the tool row, without the style row).
    // The controller pins this row to the selection, so it needs the size on
    // its own rather than the whole panel's.
    int commandBarHeight() const
    {
        return commandSurface_ != nullptr ? commandSurface_->height() : 0;
    }

    // Distance from the panel's top edge to its first row.  Constant, but read
    // off the live layout so a theme change to the padding stays correct.
    int panelTopPadding() const { return layout()->contentsMargins().top(); }

    // Height the style row adds to the panel while it is shown, and 0 while it
    // is hidden.  The controller uses it to decide whether the row still fits
    // beyond the command bar or has to double back over the selection.
    int styleRowExtra() const
    {
        const QMargins margins = layout()->contentsMargins();
        return std::max(0, height() - commandBarHeight() - margins.top() - margins.bottom());
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
            if (tip_ != nullptr) {
                tip_->setParent(parentWidget());
            }
        }
        return QWidget::event(event);
    }

    // Turns a hover into the panel's own tooltip, and swallows the event so Qt
    // never raises a `QToolTip` popup, which is what the layer-shell platform
    // integration cannot host (see `HoverTip`).
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::ToolTip: {
            auto *widget = qobject_cast<QWidget *>(watched);
            if (widget != nullptr && owns(widget) && !widget->toolTip().isEmpty()) {
                auto *help = static_cast<QHelpEvent *>(event);
                showTip(widget, help->globalPos());
                return true;
            }
            break;
        }
        // Anything that means the pointer is no longer resting on the widget
        // that raised the tip takes it away again.
        case QEvent::Leave:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::Wheel:
        case QEvent::KeyPress:
        case QEvent::Hide:
        case QEvent::WindowDeactivate:
            hideTip();
            break;
        default:
            break;
        }
        return QWidget::eventFilter(watched, event);
    }

    void hideEvent(QHideEvent *event) override
    {
        if (pickerPopup_ != nullptr) {
            pickerPopup_->hide();
        }
        if (fontPopup_ != nullptr) {
            fontPopup_->hide();
        }
        hideTip();
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
            // The panel is the pin editor's chrome, so its input region travels
            // with it as it is dragged.
            controller_->scheduleInputMask();
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
    // Whether `widget` is one the panel is responsible for: the panel itself or
    // one of its descendants, or a popup it put on the overlay.  Every other
    // widget's tooltips are left to Qt.
    bool owns(const QWidget *widget) const
    {
        if (widget == this || isAncestorOf(widget)) {
            return true;
        }
        for (const QWidget *popup : {static_cast<const QWidget *>(pickerPopup_),
                                     static_cast<const QWidget *>(fontPopup_)}) {
            if (popup != nullptr &&
                (widget == popup || popup->isAncestorOf(widget))) {
                return true;
            }
        }
        return false;
    }

    // Puts the tip next to `widget`, below it when there is room and above it
    // otherwise, and never past the overlay's own edges.
    void showTip(QWidget *widget, const QPoint &globalPos)
    {
        Q_UNUSED(globalPos);
        QWidget *host = parentWidget();
        if (host == nullptr) {
            return;
        }
        if (tip_ == nullptr) {
            tip_ = new HoverTip(host);
        } else if (tip_->parentWidget() != host) {
            tip_->setParent(host);
        }
        tip_->setText(widget->toolTip());
        tip_->adjustSize();
        const QPoint anchor = widget->mapTo(host, QPoint(widget->width() / 2, 0));
        int x = anchor.x() - tip_->width() / 2;
        int y = anchor.y() + widget->height() + 6;
        if (y + tip_->height() > host->height() - 4) {
            y = anchor.y() - tip_->height() - 6;
        }
        x = std::clamp(x, 4, std::max(4, host->width() - tip_->width() - 4));
        y = std::clamp(y, 4, std::max(4, host->height() - tip_->height() - 4));
        tip_->move(x, y);
        tip_->show();
        tip_->raise();
    }

    void hideTip()
    {
        if (tip_ != nullptr) {
            tip_->hide();
        }
    }

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

    // The panel's rect in image pixels, or an empty rect when the panel is not
    // fully covered by the pinned/frozen image. Pin editing puts the toolbar on
    // the bare canvas beside the image, where there is nothing to sample: the
    // frosted glass would otherwise smear a stretched crop across the panel.
    QRect backdropDeviceRect() const
    {
        const QWidget *owner = parentWidget();
        const QImage frame = toolbarFrame();
        if (owner == nullptr || frame.isNull() || owner->width() <= 0 ||
            owner->height() <= 0) {
            return {};
        }
        const OutputSession *output = nullptr;
        for (int index = 0; index < controller_->overlays_.size(); ++index) {
            if (controller_->overlays_.at(index) == owner &&
                index < controller_->session_.outputs.size()) {
                output = &controller_->session_.outputs.at(index);
                break;
            }
        }
        if (output == nullptr) {
            return {};
        }
        const LogicalRect &surface = surfaceOf(*output);
        const double sx = static_cast<double>(output->scale);
        const double logicalToLocal =
            static_cast<double>(owner->width()) / static_cast<double>(surface.width);
        const double logicalToLocalY =
            static_cast<double>(owner->height()) / static_cast<double>(surface.height);
        if (logicalToLocal <= 0 || logicalToLocalY <= 0) {
            return {};
        }
        // Panel local rect -> global logical -> image pixels.
        const double left = surface.x + x() / logicalToLocal;
        const double top = surface.y + y() / logicalToLocalY;
        const QRect panel(static_cast<int>(std::round(left)),
                          static_cast<int>(std::round(top)),
                          static_cast<int>(std::round(width() / logicalToLocal)),
                          static_cast<int>(std::round(height() / logicalToLocalY)));
        const LogicalRect &geometry = output->geometry;
        const QRect globalImage(geometry.x, geometry.y, static_cast<int>(geometry.width),
                                static_cast<int>(geometry.height));
        if (!globalImage.contains(panel)) {
            return {};
        }
        const QRect device(
            static_cast<int>(std::round((panel.x() - geometry.x) * sx)),
            static_cast<int>(std::round((panel.y() - geometry.y) * sx)),
            static_cast<int>(std::round(panel.width() * sx)),
            static_cast<int>(std::round(panel.height() * sx)));
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

    // A button that does something to the capture rather than draw on it: the
    // two ends of it and the history pair.  Built here, placed by the caller --
    // the four of them are one grid, and the layout they go into is that grid's
    // business rather than this one's.
    QPushButton *addActionButton(const QString &label)
    {
        auto *button = new QPushButton(label, this);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setAccessibleName(label);
        return button;
    }

    // What a tool's button says.  One list rather than one per call site: the
    // eyedropper's tooltip names the tool it will hand its colour to, and it
    // has to say the same word the button does.
    static QString toolLabel(Tool tool)
    {
        switch (tool) {
        case Tool::Rectangle:
            return uiTr("Rect");
        case Tool::Ellipse:
            return uiTr("Ellipse");
        case Tool::Arrow:
            return uiTr("Arrow");
        case Tool::Line:
            return uiTr("Line");
        case Tool::Wave:
            return uiTr("Wave");
        case Tool::Bezier:
            return uiTr("Bezier");
        case Tool::Pen:
            return uiTr("Draw");
        case Tool::Text:
            return uiTr("Text");
        case Tool::Number:
            return uiTr("Number");
        case Tool::Mosaic:
            return uiTr("Mosaic");
        case Tool::Picker:
            return uiTr("Pick");
        }
        return QString();
    }

    void addTool(Tool tool)
    {
        const QString label = toolLabel(tool);
        auto *button = new QToolButton(this);
        button->setProperty("toolButton", true);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setText(label);
        button->setIcon(toolbarIcon(tool, QColor(230, 225, 229), devicePixelRatioF()));
        button->setIconSize(QSize(20, 20));
        // No size of its own: the eleven of them are sized together once the
        // last one is in, by `sizeToolButtons`.  A button fixed here would
        // report that fixed width back from `sizeHint`, which is the one number
        // that pass has to read.
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(toolTipForTool(tool));
        button->setAccessibleName(uiTr("Tool: %1").arg(label));
        // The tool it selects, so a check can tell the eleven drawing tools from
        // the actions laid out among them -- the two kinds of button are drawn
        // the same way and the rows no longer separate them.  Empty on every
        // other button on the card.
        button->setProperty("tool", toolName(tool));
        // Not placed here: `placeCommandRows` fills the two rows from this list
        // once every button is in, which is what lets the split between them be
        // taken from the buttons' own widths.
        commandButtons_.push_back(button);
        tools_.push_back(tool);
        toolButtons_.push_back(button);
        connect(button, &QToolButton::clicked, [controller = controller_, tool] {
            controller->toggleTool(tool);
        });
    }

    static QString toolTipForTool(Tool tool)
    {
        switch (tool) {
        case Tool::Rectangle:
            return uiTr("Draw a rectangular annotation");
        case Tool::Ellipse:
            return uiTr("Draw an elliptical annotation");
        case Tool::Arrow:
            return uiTr("Draw an arrow with an adjustable head");
        case Tool::Line:
            return uiTr("Draw a straight line");
        case Tool::Wave:
            return uiTr("Draw a wavy line between two points; the Amplitude and "
                        "Wavelength sliders shape it");
        case Tool::Bezier:
            return uiTr("Draw a curved path: click to drop an anchor, drag from it to "
                        "bend the curve, click the first anchor to close the path, "
                        "double-click to finish it open; Stroke/Fill/Both decides how "
                        "it is painted");
        case Tool::Pen:
            return uiTr("Draw a freehand line");
        case Tool::Text:
            return uiTr("Click to place a text label, click text to re-edit");
        case Tool::Number:
            return uiTr("Click to place a number; each click counts up from one");
        case Tool::Mosaic:
            return uiTr("Pixelate an area: rectangle, ellipse or freehand brush");
        case Tool::Picker:
            return uiTr("Pick a color from the image; the pick hands it to the tool "
                        "you were using and leaves you on it");
        }
        return QString();
    }

    // The width a button needs to draw every one of `labels` in full.
    //
    // A tool button elides its text to the box it was given, so one sized for
    // the label it opens with cuts a longer one off.  That is what the text
    // button did: it is built for "Text+" and then flashes "Copied" in the same
    // box, and on a font that draws the word any wider than this one it comes
    // out cut off.  Asking the style for each label's own size hint is asking it
    // the same question it elides against, so the answer holds for whatever
    // font and style are in force.  The tool buttons' own width is the floor, so
    // a narrower label cannot shrink the row out of shape.
    //
    // This has to run before the width is fixed: a button at a fixed width
    // reports that width back from `sizeHint`, so measuring afterwards would
    // only ever confirm the width it already had.
    int widthForLabels(QToolButton *button, const QStringList &labels) const
    {
        button->ensurePolished();
        const QString shown = button->text();
        int widest = toolButtonWidth_;
        for (const QString &label : labels) {
            button->setText(label);
            widest = std::max(widest, button->sizeHint().width());
        }
        button->setText(shown);
        return widest;
    }

    // One size for all eleven drawing tools, taken from what each label and icon
    // actually asks the style for rather than from a number picked by hand.
    //
    // The hand-picked number was 48x46, chosen when the labels were the widest
    // thing in the row; measured against the labels of both languages it stood
    // about a tenth more than any of them needed, and the eleven buttons put
    // that slack on the panel's width and height together.  Reading the size
    // back from the style is also what keeps a wider font -- the desktop's own,
    // which the toolbar is drawn in -- from eliding a label.
    //
    // Uniform rather than per-button: a row of buttons each as wide as its own
    // word is a row with no column to read across, and it is the same size the
    // row of one-shot actions below is built at.
    void sizeToolButtons()
    {
        int width = 0;
        int height = 0;
        for (QAbstractButton *button : toolButtons_) {
            // The font is the toolbar's own style sheet's, and that is only in
            // force once the button has been polished; measuring before that
            // measures the application's font, which is wider.
            button->ensurePolished();
            width = std::max(width, button->sizeHint().width());
            height = std::max(height, button->sizeHint().height());
        }
        toolButtonWidth_ = std::max(kMinToolButtonWidth, width);
        toolButtonHeight_ = std::max(kMinToolButtonHeight, height);
        for (QAbstractButton *button : toolButtons_) {
            button->setFixedSize(toolButtonWidth_, toolButtonHeight_);
        }
    }

    // The floor under the measured size: a tool button narrower or shorter than
    // this is a corner rather than a button, however short its label is.
    static constexpr int kMinToolButtonWidth = 34;
    static constexpr int kMinToolButtonHeight = 34;

    // The gap between two buttons of a row, and between the two rows themselves:
    // the card's own spacing.  Written down once because `rowWidth` measures a
    // row with it before the layout lays that row out with it, and the two have
    // to be the same number.
    static constexpr int kRowSpacing = 2;

    // How much room the two ends of the capture keep inside their own box: the
    // label and this much either side of it, rather than the style's own ten.
    // The constructor appends it as a stylesheet rule, and the block's width is
    // the buttons' own hints, so this is the one place the corner's padding is
    // written down.
    static constexpr int kEndsPadding = 7;

    // And how far apart the two buttons of a row are: the ends of the capture
    // have to read as two buttons rather than one box with a line down it.
    static constexpr int kEndsSpacing = 6;

    // The room between the divider and the four ends pinned to the right of it:
    // what the panel's own margin leaves on their other side.  Measured against
    // the rendered panel, this is what makes the two gaps the same width -- the
    // layout's spacing already sits between the divider and this.
    static constexpr int kEndsGap = 6;

    // The size the tools came out at, which the one-shot actions that follow are
    // built at so the two rows are one panel rather than two.  Set by
    // `sizeToolButtons`, before the first action is added.
    int toolButtonWidth_ = kMinToolButtonWidth;
    int toolButtonHeight_ = kMinToolButtonHeight;

    // A button that does one thing instead of entering a mode, drawn exactly like
    // the tool buttons it is laid out among: same size, same text-under-icon
    // shape, same hover and press painting from ToolCardFrame.  Kept out of
    // `toolButtons_` because nothing stays selected: a paste and a text read
    // happen and are over.  It is placed with them, in the order it is built in,
    // by `placeCommandRows`.
    //
    // `alsoShows` names every label the button will show after `label`, so the
    // box is wide enough for all of them from the start.  A button that changes
    // its text is the one case where the size cannot be a constant.
    QToolButton *addToolAction(const QString &label, const QIcon &icon,
                               const QString &tooltip, const QString &objectName,
                               const QStringList &alsoShows = QStringList())
    {
        QToolButton *button = makeToolButton(label, icon, tooltip, objectName, alsoShows);
        commandButtons_.push_back(button);
        return button;
    }

    // What the buttons from `from` up to `to` come to in one row: their own fixed
    // widths and the gaps between them.  It is the same spacing the layout is
    // built at rather than a second copy of the number, so the width the split is
    // chosen by is the width the row is laid out at.
    int rowWidth(int from, int to) const
    {
        int width = 0;
        for (int i = from; i < to; ++i) {
            width += commandButtons_.at(i)->sizeHint().width();
        }
        if (to > from) {
            width += kRowSpacing * (to - from - 1);
        }
        return width;
    }

    // Fills the two rows of the command bar from the one list of buttons, in the
    // order they were built: the drawing tools first, then the actions.
    //
    // The split is taken where the wider of the two rows comes out narrowest,
    // which is the same as taking it where the two are closest to the same
    // length -- and that is the width the card ends up being, so it is the
    // narrowest card two rows can make of these buttons.  Splitting by kind
    // instead, tools on the first row and actions on the second, leaves a row
    // three times the length of its neighbour: the first row sets the card's
    // width while most of the second sits empty, and every tool added since makes
    // that worse.  Taken this way the two stay level however many buttons are
    // added.  A tie goes to the first row, so the split is stable as buttons are
    // appended.
    //
    // Both rows are packed to the left rather than spread across the panel: a row
    // with room to spare would otherwise space its own buttons evenly apart,
    // which slides them out from under the pointer as the style row above or
    // below changes the panel's width.  The button positions are what the user
    // aims at, so they stay put.
    void placeCommandRows(QHBoxLayout *first, QHBoxLayout *second)
    {
        const int count = commandButtons_.size();
        int split = count;
        int best = -1;
        for (int candidate = 1; candidate < count; ++candidate) {
            const int wider = std::max(rowWidth(0, candidate), rowWidth(candidate, count));
            if (best < 0 || wider < best) {
                best = wider;
                split = candidate;
            }
        }
        for (int i = 0; i < count; ++i) {
            (i < split ? first : second)->addWidget(commandButtons_.at(i));
        }
        first->addStretch(1);
        second->addStretch(1);
    }

    // The same button, built but not placed: the caller puts it in whatever
    // layout it belongs to.  The four ends pinned to the right of the card are
    // one such grid, and the history pair among them is built exactly like the
    // tools on the left -- same box, same label under the same 20 px icon, same
    // hover and press painting -- because it is the same kind of thing to click.
    QToolButton *makeToolButton(const QString &label, const QIcon &icon, const QString &tooltip,
                                const QString &objectName,
                                const QStringList &alsoShows = QStringList())
    {
        auto *button = new QToolButton(this);
        button->setProperty("toolButton", true);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setText(label);
        button->setIcon(icon);
        button->setIconSize(QSize(20, 20));
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(tooltip);
        button->setAccessibleName(label);
        button->setObjectName(objectName);
        QStringList all;
        all << label << alsoShows;
        button->setFixedSize(widthForLabels(button, all), toolButtonHeight_);
        return button;
    }

    OverlayController *controller_;
    QWidget *commandSurface_ = nullptr;
    QWidget *styleRow_ = nullptr;
    QFrame *styleDivider_ = nullptr;
    QVector<QAbstractButton *> toolButtons_;
    // Every button of the two command rows in the order they were built -- the
    // tools, then the actions -- which is the order `placeCommandRows` fills the
    // rows in and the list it measures the split by.
    QVector<QAbstractButton *> commandButtons_;
    QVector<Tool> tools_;
    // The Text+ button, whose label reports what a text selection did.  The
    // result can come from a key the toolbar never sees, so it is stored rather
    // than reached through the click handler's capture.
    QToolButton *textButton_ = nullptr;
    // The Translate button, whose label reports the two-subprocess wait and its
    // outcome the same way the text button's does.
    QToolButton *translateButton_ = nullptr;
    // The scrolling-capture button, enabled only when the session offers the
    // action and the selection fits in one output; its state is set by
    // `syncState` along with every other button's.
    QToolButton *longButton_ = nullptr;
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
    QWidget *numberGroup_ = nullptr;
    QWidget *alphaGroup_ = nullptr;
    QWidget *amplitudeGroup_ = nullptr;
    QWidget *wavelengthGroup_ = nullptr;
    QWidget *optionsRow_ = nullptr;
    QWidget *numericRow_ = nullptr;
    QVector<QPushButton *> dashButtons_;
    QVector<QString> dashValues_;
    QVector<QPushButton *> mosaicButtons_;
    QVector<QString> mosaicValues_;
    QVector<QPushButton *> numberButtons_;
    QVector<QString> numberValues_;
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
    // The colour's opacity, on the bar rather than only inside the picker: the
    // user asked for it where the colours are, not one popup deeper.
    QSlider *alphaSlider_ = nullptr;
    QLabel *alphaLabel_ = nullptr;
    // The wave's own shape, shown only while the wave is the style target.
    QSlider *amplitudeSlider_ = nullptr;
    QLabel *amplitudeLabel_ = nullptr;
    QSlider *wavelengthSlider_ = nullptr;
    QLabel *wavelengthLabel_ = nullptr;
    // How the pen path is painted, shown only while the pen is the target.
    QWidget *fillGroup_ = nullptr;
    QVector<QPushButton *> fillButtons_;
    QVector<QString> fillValues_;
    // One line telling the user how the pointer-heavy tools work, shown only
    // while one of them is armed.
    QLabel *usageHint_ = nullptr;
    QToolButton *undo_ = nullptr;
    QToolButton *redo_ = nullptr;
    QToolButton *pinButton_ = nullptr;
    HoverTip *tip_ = nullptr;
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

OverlayController::OverlayController(Session session, QObject *parent)
    : QObject(parent)
    , session_(std::move(session))
    , gesture_(new Gesture)
{
    // Whether the toolbar offers the scrolling-capture action.  Read from
    // `session_` rather than the parameter: the parameter has already been
    // moved from by the time this body runs.
    longAllowed_ = session_.longAllowed;
    // A trace of the drag's round trip, for measuring where the latency is.
    pinDebug_ = qEnvironmentVariableIsSet("VSHOT_PIN_DEBUG");
    // Window picking is driven by the session's candidate list instead of a
    // free-hand drag: the pointer highlights a candidate and a click takes it.
    if (session_.mode == QStringLiteral("window-pick")) {
        pickMode_ = true;
        candidates_ = session_.candidates;
    }
    // Scrolling capture wants a rectangle, not an editor: the pixels it will
    // annotate only exist once the page has been scrolled and stitched.
    selectOnly_ = session_.mode == QStringLiteral("region-only");
    // Screenshot translation is its own small flow: frame a region and the
    // translation runs the moment the frame is finished, then Enter accepts it.
    // No toolbar is ever shown, so the flag that draws one is never set for it.
    translateMode_ = session_.mode == QStringLiteral("translate");
    resultPath_ = session_.resultPath;
    // The style the user last left the editor in, and the keys they bound.
    const EditorPreferences preferences = loadEditorPreferences();
    shortcuts_ = loadShortcutPreferences();
    currentFont_ = preferences.font;
    textSize_ = preferences.textSize;
    currentDash_ = preferences.dash;
    arrowSize_ = preferences.arrowSize;
    currentArrowStyle_ = preferences.arrowStyle;
    mosaicShape_ = preferences.mosaicShape;
    looseSelect_ = preferences.selectMode == QStringLiteral("loose");
    mosaicStrength_ = preferences.mosaicStrength;
    // Every tool starts from the same remembered style and keeps its own copy
    // from there: a width moved on the rectangle must not follow the user to the
    // pen.  The badge diameter and the wave's shape have no entry of their own in
    // the config file -- there is one remembered style, not one per tool -- so
    // they start from the editor's defaults, with the wave derived from the
    // remembered width so a wide default still draws a proportionate wave.
    const Tool everyTool[] = {
        Tool::Rectangle, Tool::Ellipse, Tool::Arrow, Tool::Line, Tool::Wave,
        Tool::Bezier,    Tool::Pen,     Tool::Text,  Tool::Number, Tool::Mosaic,
        Tool::Picker,
    };
    for (const Tool entry : everyTool) {
        ToolStyle style;
        style.color = preferences.color;
        style.width = preferences.width;
        style.amplitude = std::max(2u * preferences.width, 4u);
        style.wavelength = std::max(6u * preferences.width, 18u);
        toolStyles_.insert(toolName(entry), style);
    }
    // The remembered tool is restored where a tool is already meaningful, and
    // *not* at the open of a session whose first gesture is the frame itself.
    //
    // A session that arrives with its selection made -- the window picker's
    // follow-up (`beginPresetEdit`) and the pin editor, whose whole image is
    // preselected in `beginPinEdit` -- has nothing left to frame, so it opens
    // on the user's tool.  A fresh region session does not: its first step is
    // dragging the rectangle, and arming a tool there would make that first
    // click start a mark instead, which reads as "region capture is broken".
    // Nor does it arm one when the frame is done.  The frame is a frame, and
    // the tool the config remembers is a default for a *new* mark, not a
    // decision the session gets to make for the user: opening armed meant the
    // first click inside a fresh region inked instead of letting the user
    // adjust the frame they had just drawn.  Region stays unarmed until a tool
    // is picked, and an unarmed drag re-frames.  Scrolling capture
    // (`selectOnly_`) and picking (`pickMode_`) stay unarmed either way: they
    // have no annotation step for a tool to belong to.
    const bool startsInEdit =
        session_.selection.has_value() || session_.mode == QStringLiteral("pin-edit");
    if (!selectOnly_ && !pickMode_ && !translateMode_ && startsInEdit) {
        tool_ = toolForName(preferences.tool);
    }
}

OverlayController::~OverlayController()
{
    removeTextEditor();
    delete toolbar_;
    toolbar_ = nullptr;
    delete gesture_;
    // Explicit rather than parented: the controller is not a QObject.
    delete pinSocket_;
    delete candidateReader_;
    delete candidateTimer_;
    delete inputMaskTimer_;
    delete pointerWarpTimer_;
    // The overlays are the caller's to delete (see `main`), so they are not
    // owned here -- but they must stop painting through a controller that is
    // gone.  Detach them, which hides them too.
    for (CaptureOverlay *overlay : overlays_) {
        overlay->detachController();
    }
    overlays_.clear();
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
    return clampPoint(unclampedGlobalPoint(overlay, local));
}

// Same conversion as globalPoint() but without clamping into the session
// bounds: the pin editor needs to tell a click on the pinned image (inside the
// bounds) from one on the surrounding canvas (outside them).
Point OverlayController::unclampedGlobalPoint(CaptureOverlay *overlay,
                                              const QPointF &local) const
{
    const OutputSession &output = overlay->output();
    const LogicalRect &surface = surfaceOf(output);
    const double sx = overlay->width() > 0
        ? static_cast<double>(surface.width) / static_cast<double>(overlay->width())
        : 1.0;
    const double sy = overlay->height() > 0
        ? static_cast<double>(surface.height) / static_cast<double>(overlay->height())
        : 1.0;
    const auto x = static_cast<std::int64_t>(std::floor(surface.x + local.x() * sx));
    const auto y = static_cast<std::int64_t>(std::floor(surface.y + local.y() * sy));
    return Point{static_cast<std::int32_t>(std::clamp<std::int64_t>(
                      x, std::numeric_limits<std::int32_t>::min(),
                      std::numeric_limits<std::int32_t>::max())),
                 static_cast<std::int32_t>(std::clamp<std::int64_t>(
                     y, std::numeric_limits<std::int32_t>::min(),
                     std::numeric_limits<std::int32_t>::max()))};
}

Point OverlayController::clampPoint(Point point) const
{
    const LogicalRect &limits = annotationLimits();
    const std::int64_t x = std::clamp<std::int64_t>(point.x, limits.x, limits.right() - 1);
    const std::int64_t y = std::clamp<std::int64_t>(point.y, limits.y, limits.bottom() - 1);
    return Point{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
}

// Area the tool may paint on. Region capture paints over the whole session;
// the pin editor confines drawing to the pinned image, which the user can drag
// around the surrounding canvas.
const LogicalRect &OverlayController::annotationLimits() const
{
    if (pinEdit_ && marksOrigin_.has_value()) {
        // Use the confirmed position: marks are constrained to where the FP16
        // image actually is, not where the cursor is asking it to go next.
        return *marksOrigin_;
    }
    if (pinEdit_ && selection_.has_value()) {
        return *selection_;
    }
    return session_.bounds;
}

// Area the selection itself may occupy. Region capture keeps it inside the
// frozen scene; in the pin editor the image may roam over the whole output,
// which is exactly the surface the overlay covers.
LogicalRect OverlayController::selectionLimits() const
{
    if (pinEdit_ && !session_.outputs.isEmpty()) {
        return surfaceOf(session_.outputs.constFirst());
    }
    return session_.bounds;
}

// Shifts every annotation by the given global delta (the image under them
// moved). Deliberately unclamped: the image and its marks travel as one rigid
// body, so a mark sitting on the image's edge must move with it rather than
// being pinned back to the edge it started on.
void OverlayController::translateAnnotations(std::int32_t dx, std::int32_t dy)
{
    if (dx == 0 && dy == 0) {
        return;
    }
    for (Annotation &annotation : annotations_) {
        switch (annotation.kind) {
        case Annotation::Kind::Shape:
        case Annotation::Kind::Image:
            annotation.rect.x = static_cast<std::int32_t>(annotation.rect.x + dx);
            annotation.rect.y = static_cast<std::int32_t>(annotation.rect.y + dy);
            break;
        case Annotation::Kind::Stroke:
            for (Point &point : annotation.points) {
                point.x = static_cast<std::int32_t>(point.x + dx);
                point.y = static_cast<std::int32_t>(point.y + dy);
            }
            break;
        case Annotation::Kind::Text:
            annotation.origin.x = static_cast<std::int32_t>(annotation.origin.x + dx);
            annotation.origin.y = static_cast<std::int32_t>(annotation.origin.y + dy);
            if (isNumberAnnotation(annotation)) {
                // A badge's box travels with it: the hit test and the raster
                // bounds are read from it, so leaving it behind would strand the
                // badge where the image used to be.
                annotation.rect.x = static_cast<std::int32_t>(annotation.rect.x + dx);
                annotation.rect.y = static_cast<std::int32_t>(annotation.rect.y + dy);
            }
            break;
        case Annotation::Kind::Translation:
            // Every box of a placed translation travels with the frame it was
            // read from, or it would sit over the wrong words.
            annotation.rect.x = static_cast<std::int32_t>(annotation.rect.x + dx);
            annotation.rect.y = static_cast<std::int32_t>(annotation.rect.y + dy);
            for (TranslatedLine &line : annotation.translation) {
                line.source.x = static_cast<std::int32_t>(line.source.x + dx);
                line.source.y = static_cast<std::int32_t>(line.source.y + dy);
                line.fill.x = static_cast<std::int32_t>(line.fill.x + dx);
                line.fill.y = static_cast<std::int32_t>(line.fill.y + dy);
            }
            break;
        }
    }
}

LogicalRect OverlayController::selectionBetween(Point first, Point second) const
{
    const std::int64_t left = std::min(first.x, second.x);
    const std::int64_t top = std::min(first.y, second.y);
    const std::int64_t rightEdge = std::max(first.x, second.x) + 1;
    const std::int64_t bottomEdge = std::max(first.y, second.y) + 1;
    LogicalRect candidate = rectFromEdges(left, top, rightEdge, bottomEdge);
    LogicalRect result;
    if (intersection(candidate, annotationLimits(), &result)) {
        return result;
    }
    return LogicalRect{};
}

LogicalRect OverlayController::moveSelection(LogicalRect origin, Point anchor, Point current) const
{
    const std::int64_t dx = static_cast<std::int64_t>(current.x) - anchor.x;
    const std::int64_t dy = static_cast<std::int64_t>(current.y) - anchor.y;
    const LogicalRect &limits = selectionLimits();
    const std::int64_t minX = limits.x;
    const std::int64_t minY = limits.y;
    const std::int64_t maxX = std::max<std::int64_t>(limits.right() - origin.width, minX);
    const std::int64_t maxY = std::max<std::int64_t>(limits.bottom() - origin.height, minY);
    const std::int64_t x = std::clamp<std::int64_t>(origin.x + dx, minX, maxX);
    const std::int64_t y = std::clamp<std::int64_t>(origin.y + dy, minY, maxY);
    return rectFromEdges(x, y, x + origin.width, y + origin.height);
}

LogicalRect OverlayController::resizeSelection(LogicalRect origin, int handle, Point current,
                                              bool preserveAspect) const
{
    // Region capture resizes the selection inside the frozen scene; in the pin
    // editor the same helper resizes a mark inside the image (which may have
    // been dragged), so the limit follows the drawing area, not the scene.
    const LogicalRect &limits = annotationLimits();
    const std::int64_t boundsLeft = limits.x;
    const std::int64_t boundsTop = limits.y;
    const std::int64_t boundsRight = limits.right();
    const std::int64_t boundsBottom = limits.bottom();
    std::int64_t left = origin.x;
    std::int64_t top = origin.y;
    std::int64_t rightEdge = origin.right();
    std::int64_t bottomEdge = origin.bottom();
    std::int64_t x = current.x;
    std::int64_t y = current.y;
    if (preserveAspect && handle != 0 && handle != 9) {
        // Alt: the corner the pointer is not on stays put and the other follows
        // the pointer, but pulled onto the box's own diagonal so the two edges
        // keep the ratio they started with.  The corner that is dragged is the
        // one the handle names, so it is the one that has to be derived.
        const bool movesLeft = handle == 1 || handle == 7 || handle == 8;
        const bool movesRight = handle == 3 || handle == 4 || handle == 5;
        const bool movesTop = handle == 1 || handle == 2 || handle == 3;
        const bool movesBottom = handle == 5 || handle == 6 || handle == 7;
        const double ratio = static_cast<double>(origin.width) /
            static_cast<double>(std::max<std::int64_t>(1, origin.height));
        // Which way the pointer went, measured from the corner that is anchored.
        const std::int64_t anchorX = movesLeft ? origin.right() : origin.x;
        const std::int64_t anchorY = movesTop ? origin.bottom() : origin.y;
        double width = static_cast<double>(std::abs(x - anchorX));
        double height = static_cast<double>(std::abs(y - anchorY));
        // The wider travel wins, so the box tracks whichever axis the pointer is
        // actually pushing on instead of collapsing when one of them stalls.
        if (width < height * ratio) {
            width = height * ratio;
        } else {
            height = width / ratio;
        }
        const std::int64_t limitX = movesLeft ? anchorX - boundsLeft
                                              : boundsRight - anchorX;
        const std::int64_t limitY = movesTop ? anchorY - boundsTop
                                             : boundsBottom - anchorY;
        width = std::min(width, static_cast<double>(std::max<std::int64_t>(0, limitX)));
        height = std::min(height, static_cast<double>(std::max<std::int64_t>(0, limitY)));
        // Both edges are taken from the same clamped pair, so the ratio survives
        // the limits rather than being applied before them.
        const std::int64_t edgeX = movesLeft ? anchorX - static_cast<std::int64_t>(width)
                                             : anchorX + static_cast<std::int64_t>(width);
        const std::int64_t edgeY = movesTop ? anchorY - static_cast<std::int64_t>(height)
                                            : anchorY + static_cast<std::int64_t>(height);
        if (movesLeft || movesRight) {
            x = edgeX;
        }
        if (movesTop || movesBottom) {
            y = edgeY;
        }
    }
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

/// The candidate window on top at `point`, or -1.  The session's list arrives
/// in stacking order, bottom to top, so the *last* window containing the point
/// is the one on top — which is what the user means by pointing at that spot.
/// The rule is not "the smallest one": a floating window sitting on a tiled one
/// is usually the smaller of the two but not always, and picking the smaller
/// one there would hand back the window underneath.
int OverlayController::candidateIndexAt(Point point) const
{
    int best = -1;
    for (int index = 0; index < candidates_.size(); ++index) {
        const LogicalRect &rect = candidates_.at(index).rect;
        if (point.x < rect.x || point.y < rect.y || point.x >= rect.right() ||
            point.y >= rect.bottom()) {
            continue;
        }
        best = index;
    }
    return best;
}

/// What the size pill reads while a candidate is hovered: the window's label
/// when it has one, always followed by the size the capture would have.
QString OverlayController::candidatePillText() const
{
    const QString dimensions =
        QStringLiteral("%1 × %2").arg(selection_->width).arg(selection_->height);
    if (hoveredCandidate_ < 0 || hoveredCandidate_ >= candidates_.size()) {
        return dimensions;
    }
    const QString label = candidates_.at(hoveredCandidate_).label.trimmed();
    if (label.isEmpty()) {
        return dimensions;
    }
    // The pill is single-line and drawn at the window's corner, so a long
    // title is elided rather than pushed across the screen.
    const QFontMetrics metrics(pillFont());
    const QString elided = metrics.elidedText(label, Qt::ElideRight, kPickerLabelWidth);
    return QStringLiteral("%1  %2").arg(elided, dimensions);
}

/// The size pill's text for the selection as it stands: the dimensions, or the
/// hovered window's label and the dimensions while the picker previews one.
/// The paint and the damage rectangle both measure the pill from this, so the
/// two can never disagree about how wide it is.
QString OverlayController::selectionPillText() const
{
    if (pickMode_ && !editing_ && hoveredCandidate_ >= 0) {
        return candidatePillText();
    }
    if (!selection_.has_value()) {
        return QString();
    }
    return QStringLiteral("%1 × %2").arg(selection_->width).arg(selection_->height);
}

/// Moves the hover highlight to the candidate under the pointer.  Returns true
/// when the highlight actually changed, so the caller can skip the repaint.
bool OverlayController::applyCandidateHover(Point point, CaptureOverlay *overlay)
{
    const int index = candidateIndexAt(point);
    overlay->setCursor(index >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (index == hoveredCandidate_ && selection_.has_value() == (index >= 0)) {
        return false;
    }
    hoveredCandidate_ = index;
    if (index >= 0) {
        selection_ = candidates_.at(index).rect;
    } else {
        selection_.reset();
    }
    return true;
}

void OverlayController::enableCandidateRefresh()
{
    if (!pickMode_ || candidateRefreshEnabled_) {
        return;
    }
    candidateRefreshEnabled_ = true;
    // The CLI answers on the same pipe the session path came in on.  It is a
    // pipe, so this never blocks on a terminal: drain what is there, and let
    // the notifier wake the controller when more arrives.
    const int flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
    candidateReader_ = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(candidateReader_, &QSocketNotifier::activated, candidateReader_, [this] {
        readCandidateReplies();
    });
    // The pointer asks as it travels; this asks when it does not, so a desktop
    // that changed without an event here still catches up.
    candidateClock_.restart();
    candidateTimer_ = new QTimer();
    candidateTimer_->setInterval(kCandidateRefreshPollMs);
    QObject::connect(candidateTimer_, &QTimer::timeout, candidateTimer_, [this] {
        requestCandidateRefresh();
    });
    candidateTimer_->start();
}

void OverlayController::enablePointerWarp()
{
    pointerWarpEnabled_ = true;
}

/// One request to the CLI, on the pipe the session path arrived on: a single
/// JSON object and a newline, which is the shape the CLI reads.  Nothing is
/// written unless the session said there is a CLI to read it -- a helper run by
/// hand, or by a check, has a pipe nobody is on the other end of, and a request
/// written into it would sit there until the pipe filled.
bool OverlayController::writeCliRequest(const QByteArray &request)
{
    if (std::fwrite(request.constData(), 1, static_cast<std::size_t>(request.size()), stdout) !=
        static_cast<std::size_t>(request.size())) {
        return false;
    }
    return std::fflush(stdout) == 0;
}

/// Asks the CLI to put the real pointer at `point`, in global logical pixels.
///
/// The keyboard walks a cursor of the editor's own, which is enough for
/// everything the editor draws -- the loupe reads it, a press uses it -- but it
/// is not the pointer the compositor paints on the screen.  The user aiming at
/// a pixel with a key needs to *see* where it went, and only the compositor can
/// move what it draws, so the walk asks.  The CLI owns the injection backends
/// (the compositor's virtual-pointer protocol, the portal, `/dev/uinput`) and
/// the desktop geometry the request is expressed in, and the helper does not.
///
/// Throttled, and *coalescing* rather than dropping: a held key repeats far
/// faster than a round trip through another process and a compositor, and
/// throwing the later steps away would leave the pointer short of where the
/// walk ended -- tap right five times quickly and the arrow moves one pixel.
/// So a step inside the interval is remembered and sent when the interval is
/// up, and only the newest one, because the position is absolute.
void OverlayController::requestPointerWarp(Point point)
{
    if (!pointerWarpEnabled_ || finished_ || cancelled_) {
        return;
    }
    if (pointerWarpClock_.isValid() && pointerWarpClock_.elapsed() < kPointerWarpIntervalMs) {
        pointerWarpPending_ = point;
        if (pointerWarpTimer_ == nullptr) {
            pointerWarpTimer_ = new QTimer();
            pointerWarpTimer_->setSingleShot(true);
            QObject::connect(pointerWarpTimer_, &QTimer::timeout, pointerWarpTimer_, [this] {
                if (!pointerWarpPending_.has_value()) {
                    return;
                }
                const Point pending = *pointerWarpPending_;
                pointerWarpPending_.reset();
                requestPointerWarp(pending);
            });
        }
        pointerWarpTimer_->start(kPointerWarpIntervalMs);
        return;
    }
    pointerWarpClock_.restart();
    // What the compositor is about to report back, so the motion it makes of
    // this request is not mistaken for the user moving the mouse; see
    // `isPointerWarpEcho`.
    pointerWarpTarget_ = point;
    const QByteArray request =
        QByteArrayLiteral("{\"request\":\"pointer\",\"x\":")
        + QByteArray::number(point.x) + QByteArrayLiteral(",\"y\":")
        + QByteArray::number(point.y) + QByteArrayLiteral("}\n");
    writeCliRequest(request);
}

/// Whether a motion event at `point` is the compositor reporting the pointer
/// position VShot itself asked for, rather than the user moving the mouse.
///
/// The keyboard walks a cursor of its own and asks the CLI to put the real
/// pointer there; the compositor then reports the moved pointer as an ordinary
/// motion, which reaches this surface after the step that asked for it.  Read
/// as a mouse move it would end the magnifier flash the step had just raised --
/// so the loupe would blink on every step of a walk, and whether it survived
/// the last one would depend on whether the echo happened to arrive before or
/// after the step.
///
/// The test is the distance.  A warp puts the pointer exactly where the walk
/// put its cursor, so the echo lands on `pointerWarpTarget_` to the pixel; a
/// hand on the mouse covers a pixel or more, and a hand that covers none has
/// not moved anything.  The window bounds how long that stays true, since a
/// user who moves the pointer back onto the same pixel later means it.
bool OverlayController::isPointerWarpEcho(Point point) const
{
    if (!pointerWarpTarget_.has_value() || !pointerWarpClock_.isValid() ||
        pointerWarpClock_.elapsed() > kPointerWarpEchoMs) {
        return false;
    }
    return pointerWarpTarget_->x == point.x && pointerWarpTarget_->y == point.y;
}

void OverlayController::requestCandidateRefresh()
{
    if (!candidateRefreshEnabled_ || candidateRefreshPending_ || finished_ || cancelled_) {
        return;
    }
    if (candidateClock_.elapsed() < kCandidateRefreshIntervalMs) {
        return;
    }
    candidateClock_.restart();
    const QByteArray request = QByteArrayLiteral("{\"request\":\"candidates\"}\n");
    if (!writeCliRequest(request)) {
        return;
    }
    candidateRefreshPending_ = true;
}

/// Reads whatever the CLI has answered so far: one JSON object per line, a
/// `candidates` array being a fresh list.  Anything without one (an empty
/// object, or a CLI that has no window list to offer) leaves the current list
/// alone, so a picker that cannot be refreshed still highlights something.
void OverlayController::readCandidateReplies()
{
    char buffer[4096];
    while (true) {
        const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (got > 0) {
            candidateReplies_.append(buffer, static_cast<int>(got));
            continue;
        }
        if (got == 0) {
            // The CLI closed the pipe (it is on its way out): stop listening.
            candidateReader_->setEnabled(false);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        candidateReader_->setEnabled(false);
        return;
    }

    int newline = candidateReplies_.indexOf('\n');
    while (newline >= 0) {
        const QByteArray line = candidateReplies_.left(newline);
        candidateReplies_.remove(0, newline + 1);
        newline = candidateReplies_.indexOf('\n');
        candidateRefreshPending_ = false;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            continue;
        }
        const QJsonValue value = document.object().value(QStringLiteral("candidates"));
        if (!value.isArray()) {
            continue;
        }
        const QJsonArray array = value.toArray();
        QVector<WindowCandidate> candidates;
        candidates.reserve(array.size());
        bool valid = true;
        for (int index = 0; index < array.size(); ++index) {
            QString reason;
            WindowCandidate candidate;
            if (!array.at(index).isObject() ||
                !parseWindowCandidate(array.at(index).toObject(),
                                      QStringLiteral("candidate %1").arg(index), &candidate,
                                      &reason)) {
                valid = false;
                break;
            }
            candidates.push_back(std::move(candidate));
        }
        if (valid) {
            applyCandidates(std::move(candidates));
        }
    }
}

/// Replaces the candidate list with a fresh one and points the hover at
/// whatever the (unmoved) pointer is over now.  The picker polls, so most
/// answers are the list it already has: comparing first keeps the veil from
/// being repainted three times a second for nothing.
void OverlayController::applyCandidates(QVector<WindowCandidate> candidates)
{
    const bool sameList = candidates.size() == candidates_.size() &&
        std::equal(candidates.cbegin(), candidates.cend(), candidates_.cbegin(),
                   [](const WindowCandidate &first, const WindowCandidate &second) {
                       return first.rect.x == second.rect.x && first.rect.y == second.rect.y &&
                           first.rect.width == second.rect.width &&
                           first.rect.height == second.rect.height &&
                           first.label == second.label;
                   });
    if (sameList) {
        return;
    }
    candidates_ = std::move(candidates);
    // The pointer has not moved, but what sits under it may have: re-point the
    // hover at the new list instead of keeping a highlight that no longer
    // describes anything on screen.
    hoveredCandidate_ = candidateIndexAt(pointer_);
    if (hoveredCandidate_ >= 0) {
        selection_ = candidates_.at(hoveredCandidate_).rect;
    } else {
        selection_.reset();
    }
    for (CaptureOverlay *overlay : overlays_) {
        overlay->setCursor(hoveredCandidate_ >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    updateAll();
}

void OverlayController::beginSelectionGesture(Point point, bool preserveAspect)
{
    const int handle = hitHandle(point);
    if (selection_.has_value() && handle != 0 && handle != 9) {
        gesture_->anchor = point;
        gesture_->current = point;
        gesture_->origin = *selection_;
        gesture_->handle = handle;
        gesture_->preserveAspect = preserveAspect;
        gesture_->type = Gesture::Type::Resizing;
    } else {
        // A press that landed on no handle draws a new frame.  The body of the
        // selection is deliberately not a target here: dragging it is Ctrl's,
        // which is what keeps an ordinary press meaning "frame this instead".
        startSelection(point);
    }
}

void OverlayController::beginSelectionMove(Point point)
{
    if (!selection_.has_value()) {
        startSelection(point);
        return;
    }
    gesture_->anchor = point;
    gesture_->current = point;
    gesture_->origin = *selection_;
    gesture_->handle = 9;
    gesture_->type = Gesture::Type::Moving;
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
    if (!selection_.has_value()) {
        return;
    }
    if (selectOnly_) {
        // A drag that lands on something usable ends the session right there;
        // a stray click leaves the surface alone so the user can try again.
        if (hasValidSelection()) {
            terminal(false);
        }
        return;
    }
    if (translateMode_) {
        // The rectangle is only the frame the translation will be drawn in, and
        // finishing it *is* the request: the drag ends, the translation runs,
        // and Enter is what accepts it from there on.  (Enter still runs the
        // translation too, so the frame an Escape took the translation off can
        // be translated again without drawing it anew.)  A stray click that made
        // nothing usable leaves no frame to translate.
        if (!hasValidSelection()) {
            selection_.reset();
            updateAll();
            return;
        }
        QString error;
        if (!runTranslateStage(&error)) {
            std::fprintf(stderr, "vshot-qt-ui: %s\n", error.toUtf8().constData());
            std::fflush(stderr);
        }
        return;
    }
    editing_ = true;
    showToolbar();
}

void OverlayController::beginDrawing(Point point)
{
    gesture_->type = Gesture::Type::Drawing;
    gesture_->anchor = clampPoint(point);
    gesture_->current = gesture_->anchor;
    gesture_->points.clear();
    gesture_->points.push_back(gesture_->anchor);
    // The live raster starts over with each stroke.
    gesture_->liveRaster = QImage();
    gesture_->liveOrigin = QPoint();
    gesture_->liveBaked = 1;
    gesture_->liveLength = 0.0;
    gesture_->liveKey.clear();
    liveStrokeBakes_ = 0;
}

void OverlayController::updateDrawing(Point point)
{
    const Point bounded = clampPoint(point);
    gesture_->current = bounded;
    if (tool_ == Tool::Arrow || tool_ == Tool::Line || tool_ == Tool::Wave) {
        // The two-point tools: only the anchor and the current point matter, so
        // the gesture never records the wandering intermediate positions.  The
        // arrow and the line are the segment between them; the wave is the sine
        // sample of that same segment, drawn from it on every paint.
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
    // A stroke can only be committed by a tool that is armed: the press that
    // started it checked, and nothing disarms mid-gesture, so the empty case is
    // unreachable rather than a state to draw from.
    const Tool drawingTool = *tool_;
    gesture_->type = Gesture::Type::None;
    gesture_->points.clear();
    gesture_->liveRaster = QImage();
    gesture_->liveKey.clear();
    gesture_->liveBaked = 0;
    gesture_->liveLength = 0.0;
    if (points.isEmpty()) {
        return;
    }
    if (drawingTool == Tool::Arrow &&
        (points.size() < 2 || points.constFirst() == points.constLast())) {
        return;
    }
    Annotation annotation;
    annotation.tool = toolName(drawingTool);
    const ToolStyle &style = toolStyle(annotation.tool);
    annotation.textPixels = textSize_;
    annotation.color = style.color;
    annotation.width = style.width;
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

void OverlayController::beginBezier(Point point)
{
    gesture_->type = Gesture::Type::Bezier;
    gesture_->anchor = clampPoint(point);
    gesture_->current = gesture_->anchor;
    gesture_->points.clear();
    // None of the freehand stroke's incremental raster is used here: a pen path
    // is redrawn whole from its anchors on every paint, which is what it always
    // was, so there is nothing to accumulate.
    gesture_->liveRaster = QImage();
    gesture_->liveOrigin = QPoint();
    gesture_->liveBaked = 0;
    gesture_->liveLength = 0.0;
    gesture_->liveKey.clear();
}

void OverlayController::updateBezier(Point point, bool dragging)
{
    const Point bounded = clampPoint(point);
    gesture_->current = bounded;
    if (dragging && !gesture_->points.isEmpty()) {
        // The drag pulls the outgoing handle of the anchor just placed out; the
        // incoming side is its mirror, so only one of the two is ever stored --
        // the same symmetric handle the wire format describes.
        gesture_->points.last() = bounded;
    }
}

void OverlayController::finishBezier(bool closed)
{
    const QVector<Point> points = gesture_->points;
    gesture_->type = Gesture::Type::None;
    gesture_->points.clear();
    if (points.isEmpty()) {
        updateAll();
        return;
    }
    Annotation annotation;
    annotation.kind = Annotation::Kind::Stroke;
    annotation.tool = toolName(Tool::Bezier);
    // A single anchor has no segment to close, so a path that was somehow closed
    // before it had two of them stays open rather than being filled as a point.
    annotation.closed = closed && bezierAnchors(points) >= 2;
    const ToolStyle &style = toolStyle(annotation.tool);
    annotation.color = style.color;
    annotation.width = style.width;
    annotation.fill = currentFill_;
    annotation.dash = currentDash_;
    annotation.points = points;
    QVector<Annotation> next = annotations_;
    next.push_back(annotation);
    const int newIndex = next.size() - 1;
    mutateAnnotations(std::move(next));
    // Newly drawn annotations stay selected so the panel restyles them and the
    // Select tool can immediately move them.
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
    // An edit is not the keyboard's walk any more: the run of nudges ends here,
    // and the selection is no longer "all of them" unless the caller says so.
    nudgeBase_.reset();
    allSelected_ = false;
    updateAll();
}

bool OverlayController::annotationBounds(const Annotation &annotation, LogicalRect *bounds) const
{
    return annotationLogicalBounds(annotation, bounds);
}

bool OverlayController::canDrawAt(Point point) const
{
    const LogicalRect &limits = annotationLimits();
    return point.x >= limits.x && point.x < limits.right() &&
           point.y >= limits.y && point.y < limits.bottom();
}

// The band the pin's own border occupies, as the image grown by half the
// stroke's width. `PinSurface` centres the stroke on the image's edge, so that
// is exactly how far outside the image it reaches; with no border the band is
// the image itself and nothing more.
LogicalRect OverlayController::pinBorderBand() const
{
    const LogicalRect &image = annotationLimits();
    const std::uint32_t half = session_.pinBorderWidth / 2;
    if (half == 0) {
        return image;
    }
    return LogicalRect{image.x - static_cast<std::int32_t>(half),
                       image.y - static_cast<std::int32_t>(half), image.width + 2 * half,
                       image.height + 2 * half};
}

bool OverlayController::insidePinImage(Point point) const
{
    if (!pinEdit_) {
        return canDrawAt(point);
    }
    const LogicalRect &image = annotationLimits();
    return point.x >= image.x && point.x < image.right() &&
           point.y >= image.y && point.y < image.bottom();
}

bool OverlayController::onPinBorder(Point point) const
{
    if (!pinEdit_ || insidePinImage(point)) {
        return false;
    }
    const LogicalRect band = pinBorderBand();
    return point.x >= band.x && point.x < band.right() &&
           point.y >= band.y && point.y < band.bottom();
}

void OverlayController::placeNumber(Point point)
{
    Annotation annotation;
    annotation.kind = Annotation::Kind::Text;
    annotation.tool = QStringLiteral("number");
    // One past the highest badge already on the canvas, read fresh every time.
    // Undoing a badge therefore hands its number back to the next click, and
    // there is no counter that could drift out of step with the list.
    int highest = 0;
    for (const Annotation &existing : annotations_) {
        highest = std::max(highest, existing.number);
    }
    annotation.number = highest + 1;
    annotation.numberStyle = numberStyle_;
    const ToolStyle &style = toolStyle(annotation.tool);
    annotation.color = style.color;
    annotation.numberSize = style.numberSize;
    // The badge hangs from the point the click landed on, and its box is
    // recorded on the annotation: the hit test, the drag clamp, the raster cache
    // and the paint all read it from there, which is what makes a badge
    // selectable, movable and deletable rather than merely visible.  `origin` is
    // the same corner, because that is where the renderer blits the bitmap.
    layoutNumberBox(annotation, clampPoint(point));
    QVector<Annotation> next = annotations_;
    next.push_back(annotation);
    const int newIndex = next.size() - 1;
    mutateAnnotations(std::move(next));
    selectAnnotation(newIndex);
}

// The eyedropper's whole gesture: read the pixel under the pointer out of the
// frozen frame and write it into the style of the tool the picker was armed
// from.  Only the colour is taken -- the tool keeps the opacity the user set,
// the way a palette swatch leaves it -- and the pick hands the session back to
// that tool, so the next click draws with what was just read.
bool OverlayController::pickColorAt(CaptureOverlay *overlay, Point point)
{
    if (!canDrawAt(point)) {
        return false;
    }
    const OutputSession &output = overlay->output();
    const std::uint32_t scale = output.scale > 0 ? output.scale : 1;
    const int width = static_cast<int>(output.image.width());
    const int height = static_cast<int>(output.image.height());
    if (width <= 0 || height <= 0) {
        return false;
    }
    // The mapping the loupe reads its pixels through, so the colour the readout
    // shows under the crosshair is the one the click takes.
    const int x = std::clamp(
        static_cast<int>(std::floor((point.x - output.geometry.x) * static_cast<double>(scale))),
        0, width - 1);
    const int y = std::clamp(
        static_cast<int>(std::floor((point.y - output.geometry.y) * static_cast<double>(scale))),
        0, height - 1);
    QColor color = output.image.pixelColor(x, y);
    if (!color.isValid()) {
        return false;
    }
    color.setAlpha(toolStyle(styleTargetTool()).color.alpha());
    pickedColor_ = color;
    setCurrentColor(color);
    return true;
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
        // Only a real label is re-editable.  A numbered badge is a text
        // annotation by wire but carries no typed string, so letting the scan
        // find one would open an empty editor over it and replace the badge with
        // a label on commit.
        if (annotations_.at(i).kind == Annotation::Kind::Text &&
            !isNumberAnnotation(annotations_.at(i)) &&
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
    textEditPixels_ = cancelledText_.has_value() ? cancelledText_->textPixels : textSize_;
    const QColor editColor = cancelledText_.has_value()
        ? cancelledText_->color
        : toolStyle(QStringLiteral("text")).color;
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
    QFont editorFont = textFont(textEditFont_, std::max(1, static_cast<int>(textEditPixels_)));
    textEdit_->setFont(editorFont);
    // The editor carries no frame and no padding, so its glyphs start exactly at
    // its own top-left.  A framed, padded box would draw the text a few pixels
    // in from the corner and the label would jump the moment the editor was
    // accepted; this way the corner is the label's origin and nothing moves.
    textEdit_->setFrame(false);
    textEdit_->setTextMargins(0, 0, 0, 0);
    const QString editorSheet =
        QStringLiteral("QLineEdit { color: %1; border: none; padding: 0; background: %2; }")
            .arg(editColor.name(), QStringLiteral("rgba(0, 0, 0, 140)"));
    const QString probeSheet =
        QStringLiteral("QLineEdit { color: %1; border: none; padding: 0; background: transparent; }")
            .arg(editColor.name());
    textEdit_->setStyleSheet(editorSheet);
    const QPointF local = owner->localFromGlobal(origin);
    const QFontMetrics editorMetrics(editorFont);
    // As tall as its own glyphs, so the vertical centring QLineEdit applies is
    // a no-op and the text's ascent sits on the widget's top edge; as wide as
    // the room left of the origin, so the corner stays where the label is drawn
    // rather than being clamped away from it.
    const int x = static_cast<int>(std::round(local.x()));
    const int y = static_cast<int>(std::round(local.y()));
    const int height = std::max(1, editorMetrics.height());
    const int width = std::clamp(owner->width() - x - 4, 40, 360);
    textEdit_->setGeometry(x, y, width, height);
    textEdit_->show();
    textEdit_->raise();
    textEdit_->setFocus(Qt::OtherFocusReason);
    // Measure where the style actually puts the glyphs (a frame, a margin) and
    // pull the box back by exactly that, so the label lands where it was typed.
    // The probe is painted on a transparent background so only its ink is read.
    textEdit_->setStyleSheet(probeSheet);
    textEdit_->setText(QStringLiteral("H"));
    const QPoint inset = lineEditGlyphInset(textEdit_);
    textEdit_->setStyleSheet(editorSheet);
    textEdit_->setText(initial);
    textEdit_->setGeometry(x - inset.x(), y - inset.y(), width, height);
    if (!initial.isEmpty()) {
        textEdit_->selectAll();
    }
    // The editor is chrome like the toolbar: the surface has to take clicks on
    // it, or the user could see the box they are typing into but not click in
    // it.
    scheduleInputMask();
    updateAll();
}

void OverlayController::finishText(bool accept)
{
    if (textEdit_ == nullptr) {
        return;
    }
    const QString value = textEdit_->text();
    // Capture the height the editor is actually drawing at before the state is
    // cleared: a size change made while the box was open lives here and in
    // nowhere else (during a re-edit no annotation is selected to restyle).
    const std::uint32_t editedPixels = textEditPixels_;
    textEdit_->hide();
    textEdit_->deleteLater();
    textEdit_ = nullptr;
    textEditPixels_ = 0;
    scheduleInputMask();
    if (accept && !value.isEmpty()) {
        Annotation annotation;
        annotation.kind = Annotation::Kind::Text;
        annotation.tool = QStringLiteral("text");
        annotation.origin = cancelledText_.has_value() ? cancelledText_->origin : textOrigin_;
        annotation.text = value;
        if (cancelledText_.has_value()) {
            // Re-edits keep the label's own style unless the panel restyles
            // it while editing; textEditFont_ tracks a live font change and
            // textEditPixels_ a live size change.
            annotation.textPixels = editedPixels > 0 ? editedPixels : cancelledText_->textPixels;
            annotation.color = cancelledText_->color;
            annotation.font = textEditFont_;
        } else {
            annotation.textPixels = textSize_;
            annotation.color = toolStyle(QStringLiteral("text")).color;
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
    if (translateMode_) {
        // The translate overlay is a region frame with no editor: the marks a
        // toolbar places have nowhere to go and the frame is not saved as one.
        return;
    }
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
    scheduleInputMask();
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
    scheduleInputMask();
}

void OverlayController::hideToolbar()
{
    toolbarAnchorValid_ = false;
    if (toolbar_ != nullptr) {
        toolbar_->hide();
    }
    scheduleInputMask();
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
    // Keep the command bar attached to the selection: it settles centred above
    // the selection, or below when the panel does not fit above, always clamped
    // to the owning output.  The style row takes whichever side of the command
    // bar still has room; when neither side does -- a capture that nearly fills
    // the display -- it doubles back over the selection instead, so showing or
    // hiding it never nudges the buttons.
    const OutputSession &output = owner->output();
    // Anchor to the same rect the marks and the image are drawn against: in the
    // pin editor that is the daemon-confirmed image rect, not the optimistic
    // cursor target, so the bar tracks the picture rather than running ahead of
    // it mid-drag.
    const LogicalRect &anchor =
        pinEdit_ && marksOrigin_.has_value() ? *marksOrigin_ : *selection_;
    const QRectF localSelection = localRect(output, anchor, owner->size());
    const QRect anchorSelection = localSelection.toRect();
    const int width = toolbar_->width();
    const int height = toolbar_->height();
    const int barHeight = toolbar_->commandBarHeight();
    const int topPadding = toolbar_->panelTopPadding();
    const int styleExtra = toolbar_->styleRowExtra();
    constexpr int kEdgeMargin = 4;
    constexpr int kSelectionGap = 8;
    const int selectionTop = static_cast<int>(std::round(localSelection.top()));
    const int selectionBottom = static_cast<int>(std::round(localSelection.bottom()));
    // While the selection stays put, keep the side it was placed on so a style
    // toggle cannot throw the command bar to the other side of the selection.
    const bool selectionSettled =
        toolbarAnchorValid_ && toolbarAnchorSelection_ == anchorSelection;
    const bool below = selectionSettled
        ? toolbarAnchorBelow_
        : selectionTop - kSelectionGap - height < kEdgeMargin;
    // The style row prefers the far side of the command bar; when it does not
    // fit there, it expands the other way, over the selection.
    bool styleAbove = !below;
    if (styleAbove) {
        styleAbove = selectionTop - kSelectionGap - barHeight - topPadding - styleExtra >=
            kEdgeMargin;
    } else {
        const bool fitsBelow =
            selectionBottom + kSelectionGap - topPadding + height <= owner->height() - kEdgeMargin;
        styleAbove = !fitsBelow;
    }
    toolbar_->setStyleRowAbove(styleAbove);
    // Anchor the command bar's edge nearest the selection, not the panel edge:
    // the style row may sit on either side of it, so only this edge is fixed.
    const int barTop =
        below ? selectionBottom + kSelectionGap : selectionTop - kSelectionGap - barHeight;
    int y = barTop - (styleAbove ? topPadding + styleExtra : topPadding);
    int x = 0;
    if (selectionSettled) {
        x = std::clamp(toolbarAnchor_.x(), 0, std::max(0, owner->width() - width));
    } else {
        x = std::clamp(static_cast<int>(std::round(localSelection.center().x() - width / 2.0)),
                       kEdgeMargin, std::max(kEdgeMargin, owner->width() - width - kEdgeMargin));
    }
    y = std::clamp(y, kEdgeMargin,
                   std::max(kEdgeMargin, owner->height() - height - kEdgeMargin));
    toolbarAnchor_ = QPoint(x, y);
    toolbarAnchorSelection_ = anchorSelection;
    toolbarAnchorBelow_ = below;
    toolbarAnchorValid_ = true;
    toolbar_->setGeometry(x, y, width, height);
    // The panel moved, so the strip of the surface that takes input moved with
    // it.
    scheduleInputMask();
}

int OverlayController::sceneScale() const
{
    // The density the helper rasterized its bitmaps at: the highest one any
    // output declares.  A pin-edit session's single output carries the pin's
    // zoom rather than a density, and its text is composited from the helper's
    // own bitmap, so the whole-number part is what the bitmap was drawn at --
    // a zoom below 1 is one bitmap pixel per logical pixel, which is 1.
    int scale = 1;
    for (const OutputSession &output : session_.outputs) {
        scale = std::max(scale, static_cast<int>(std::lround(outputScale(output))));
    }
    return scale;
}

void OverlayController::repaintEverything()
{
    for (CaptureOverlay *overlay : overlays_) {
        overlay->update();
    }
    if (toolbar_ != nullptr && toolbar_->isVisible()) {
        toolbar_->syncState();
        updateToolbarGeometry();
    }
}

void OverlayController::updateAll()
{
    // A full repaint erases whatever the narrow ones left, so the rect they were
    // tracking stops being the record of what is on the surface.
    hasLastTouch_ = false;
    // The hover frame is not drawn by a full repaint, and the mark it named may
    // not even be there any more -- a delete, an undo, a tool change all land
    // here.  Forgetting it means the next motion re-derives it against whatever
    // the mark list is now, rather than framing a stale index.
    markHovered_ = -1;
    repaintEverything();
}

void OverlayController::scheduleInputMask()
{
    if (!pinEdit_) {
        return;
    }
    if (inputMaskTimer_ == nullptr) {
        // Parentless and deleted explicitly, like the controller's other timers:
        // the controller is not a QObject, so there is no parent to hang it on.
        inputMaskTimer_ = new QTimer();
        inputMaskTimer_->setSingleShot(true);
        inputMaskTimer_->setInterval(0);
        QObject::connect(inputMaskTimer_, &QTimer::timeout, inputMaskTimer_, [this] {
            applyPinEditInputMask();
        });
    }
    // Applied now as well as on the timer.  The first call is the one that has
    // to be in force before the event loop runs -- the surface is born taking
    // the whole output -- and every later one is cheap, because a mask that has
    // not changed is not re-sent.
    applyPinEditInputMask();
    inputMaskTimer_->start();
}

// The editor's surface covers the whole output so the toolbar has somewhere to
// sit beside the image, but the editor only owns the chrome it draws: the
// pinned image, the band its border occupies, the toolbar and anything open
// over them.  Everything else on the screen belongs to the desktop underneath,
// and an input region that said otherwise is what made a click far from the pin
// land on the editor instead of on the window behind it.
void OverlayController::applyPinEditInputMask()
{
    if (!pinEdit_ || overlays_.isEmpty()) {
        return;
    }
    // The band is where the daemon last confirmed the image to be, not where
    // the session put it: a pin that has been dragged since would otherwise
    // leave its input region behind at the old spot.
    const LogicalRect band = pinBorderBand();
    QRegion mask;
    for (CaptureOverlay *overlay : overlays_) {
        // `pinBorderBand` already falls back to the image when the session named
        // no border, so this is the whole of what the pin occupies.
        const LogicalRect &visible = band;
        if (visible.width == 0 || visible.height == 0) {
            continue;
        }
        mask += localRect(overlay->output(), visible, overlay->size()).toAlignedRect();
        // The toolbar and its popups are children of the overlay, so their own
        // geometry is already in overlay coordinates.
        const auto addWidget = [&mask, overlay](const QWidget *widget) {
            if (widget == nullptr || !widget->isVisible() || widget->parentWidget() != overlay) {
                return;
            }
            mask += QRegion(widget->geometry());
        };
        addWidget(toolbar_);
        addWidget(textEdit_);
        if (toolbar_ != nullptr) {
            for (const QWidget *popup : toolbar_->popups()) {
                addWidget(popup);
            }
        }
        // No mask was asked for: leave the surface whole rather than cutting
        // the editor down to nothing and making it unreachable.
        if (mask.isEmpty()) {
            continue;
        }
        overlay->setInputMask(mask);
    }
}

// How far outside a mark's own rect its pixels can reach.  The answer lives in
// the mark's rasterizer -- the pen width, the arrow head, the mosaic brush
// radius -- which is why it is not simply a field of the annotation.
int annotationReach(const Annotation &annotation);

// Pixels of chrome a selection drag can paint outside the selection itself: the
// two-pixel outline and the round handles centred on its corners.
constexpr int kSelectionChrome = 6;

namespace {

// The session-space union of two rects, written with the explicit 64-bit
// arithmetic `LogicalRect`'s mixed-signed fields otherwise make awkward.  An
// empty rect is the identity, which is what a gesture that has not touched
// anything yet hands over.
LogicalRect uniteLogical(const LogicalRect &first, const LogicalRect &second)
{
    if (first.width == 0 || first.height == 0) {
        return second;
    }
    if (second.width == 0 || second.height == 0) {
        return first;
    }
    const std::int64_t left = std::min<std::int64_t>(first.x, second.x);
    const std::int64_t top = std::min<std::int64_t>(first.y, second.y);
    const std::int64_t far = std::max(right(first), right(second));
    const std::int64_t low = std::max(bottom(first), bottom(second));
    return LogicalRect{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                       static_cast<std::uint32_t>(far - left),
                       static_cast<std::uint32_t>(low - top)};
}

// The same rect grown by `margin` logical pixels on every side.
LogicalRect growBy(LogicalRect rect, int margin)
{
    rect.x -= static_cast<std::int32_t>(margin);
    rect.y -= static_cast<std::int32_t>(margin);
    rect.width += static_cast<std::uint32_t>(2 * margin);
    rect.height += static_cast<std::uint32_t>(2 * margin);
    return rect;
}

// How far outside its path a live preview paints.  The freehand pen reaches out
// by half its width; the mosaic brush stamps a block whose radius comes from the
// strength and can be far wider than the cursor.  `paintLiveStroke` builds its
// own padding out of the same two numbers, so the two move together.
double liveStrokeMargin(bool brush, int widthLogical, double scale, std::uint32_t strength)
{
    if (!brush) {
        return widthLogical / 2.0 + 2.0;
    }
    // The ratio is a real number, not a whole one: a zoomed pin shows 160
    // pixels across 176 logical ones, so a scale rounded to a whole number
    // would be zero here and the division below would be by zero.
    const double ratio = scale > 0.0 ? scale : 1.0;
    const double deviceRadius = std::clamp(
        brushRadiusForStrength(
            strength, std::clamp(static_cast<int>(widthLogical * ratio / 2), 1, 512)),
        1, 512);
    return deviceRadius / ratio + 2.0;
}

/// How far a wave's pixels reach from the line its two points describe.
///
/// A crest leaves that line by the amplitude and the stroke adds half its width
/// on top, plus a pixel for the antialiased edge.  The live preview's damage
/// rectangle and a committed wave's padding are both computed from this one
/// number: they used to be spelled out separately, and the drag path was
/// missing the amplitude term altogether, which left the previous frame's
/// crests on screen after the wave moved.
double waveReach(double amplitude, double width)
{
    return amplitude + width / 2.0 + 4.0;
}

// Floor for the room the size pill needs, in the direction that matters least:
// the pill is measured from its own text (see `selectionTouch`), and this only
// keeps a short one from shrinking the step's rect below what the selection's
// chrome -- the border and the eight handles -- already reaches.
constexpr int kInfoPillSlack = 64;

} // namespace

// The magnifier the editor follows the pointer with while a gesture is dragging
// something, plus the coordinate and colour pills that hang off it.  The loupe
// sits a little past the pointer and flips to the other side near an edge, so
// the box is the pointer plus the whole reach on every side: half a diameter in
// x, and enough in y for both pills stacked below the circle.
LogicalRect OverlayController::pointerTouch() const
{
    return pointerTouchAt(pointer_);
}

// The same box around an arbitrary point.  The magnifier follows the pointer
// while the right button is held, so a step has to be able to name the rect the
// *previous* position's loupe covered -- which is not `pointer_` by the time
// the step asks, because the pointer has already moved.
LogicalRect OverlayController::pointerTouchAt(Point point) const
{
    constexpr int kReachX = kLoupeDiameter + kLoupeMargin;
    constexpr int kReachY = kLoupeDiameter + 2 * kInfoPillSlack + kLoupeMargin;
    return LogicalRect{point.x - kReachX, point.y - kReachY, 2 * kReachX, 2 * kReachY};
}

LogicalRect OverlayController::selectionTouch() const
{
    if (!selection_.has_value()) {
        return LogicalRect{};
    }
    // The size pill hangs off the selection and reaches half its own width to
    // either side of the corner -- its whole width when it has to go beside the
    // region -- so the step's rect is measured from the very text the paint
    // will use rather than from a fixed slack a long size could outgrow.
    const QSizeF pill = pillSize(selectionPillText());
    const int reach = std::max(kInfoPillSlack,
                               static_cast<int>(std::ceil(pill.width())) + kInfoPillGap);
    return uniteLogical(growBy(*selection_, reach), pointerTouch());
}

LogicalRect OverlayController::annotationTouch() const
{
    if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size()) {
        return LogicalRect{};
    }
    const Annotation &annotation = annotations_.at(selectedAnnotation_);
    LogicalRect bounds;
    if (!annotationBounds(annotation, &bounds)) {
        return LogicalRect{};
    }
    // The mark's own rasterizer knows how far its pixels reach; a mark that has
    // not been painted yet has none, so the padding comes from a temporary one.
    return uniteLogical(growBy(bounds, annotationReach(annotation) + kSelectionChrome),
                        pointerTouch());
}

LogicalRect OverlayController::drawingTouch(int pointsBefore) const
{
    if (gesture_->points.isEmpty()) {
        return LogicalRect{};
    }
    // A growing stroke only stamps the segments the last pointer move added; the
    // steps before are already baked into the raster and copied, so their pixels
    // stay on screen.  The straight tools redraw their whole shape from the
    // anchor every step, so all of it counts as touched.
    const int count = static_cast<int>(gesture_->points.size());
    int first = 0;
    if (drawsGrowingStroke()) {
        first = std::clamp(pointsBefore - 1, 0, count - 1);
    }
    LogicalRect touched{gesture_->points.at(first).x, gesture_->points.at(first).y, 1u, 1u};
    for (int index = first + 1; index < count; ++index) {
        const Point &point = gesture_->points.at(index);
        touched = uniteLogical(touched, LogicalRect{point.x, point.y, 1u, 1u});
    }
    // The preview is stamped on the output the stroke started on, and that
    // output's scale is what turns the device-space brush radius back into
    // logical pixels.  It is the ratio itself, not a rounded one: the pin
    // editor's output is zoomed, and rounding 0.909 to a whole number is zero.
    double scale = 1.0;
    for (CaptureOverlay *overlay : overlays_) {
        if (overlay->outputIndex() == gesture_->liveOutput) {
            scale = outputScale(overlay->output());
            break;
        }
    }
    const bool brush = tool_ == Tool::Mosaic;
    const QString liveTool = tool_.has_value() ? toolName(*tool_) : QStringLiteral("pen");
    const ToolStyle &style = toolStyle(liveTool);
    // A wave is the one two-point tool whose pixels leave the box its endpoints
    // describe, so it is measured by its own reach rather than by the half-width
    // margin every other straight tool fits inside.
    const double reach = tool_ == Tool::Wave
        ? waveReach(style.amplitude, style.width)
        : liveStrokeMargin(brush, std::max(1, static_cast<int>(style.width)), scale,
                           mosaicStrength_);
    // The magnifier follows every drawing drag, and it travels with the pointer:
    // it is drawn a whole diameter away from the cursor, so a rect that only
    // covered the ink would leave the circle it moved away from on screen.
    return growBy(touched, static_cast<int>(std::ceil(reach)) + kSelectionChrome);
}

LogicalRect OverlayController::bezierTouch() const
{
    // A path always holds whole [anchor, handle] pairs, so anything shorter than
    // two has nothing to bound and nothing to join.
    if (gesture_->points.size() < 2) {
        return LogicalRect{};
    }
    // The preview is the path plus the rubber band, so its rect is the path's
    // own box united with the band.  Both are measured through the same two
    // helpers the preview paints through, so a step can never invalidate less
    // than it changed.
    LogicalRect bounds;
    if (!annotationLogicalBounds(previewAnnotation(), &bounds)) {
        return LogicalRect{};
    }
    // The rubber band is a straight segment from the path's last anchor to the
    // pointer, so its own rect is the box between the two -- the segment never
    // leaves it.
    const Point &anchor = gesture_->points.at(gesture_->points.size() - 2);
    const Point &cursor = gesture_->current;
    const LogicalRect band{std::min(anchor.x, cursor.x), std::min(anchor.y, cursor.y),
                           static_cast<std::uint32_t>(std::abs(cursor.x - anchor.x) + 1),
                           static_cast<std::uint32_t>(std::abs(cursor.y - anchor.y) + 1)};
    // The anchor and handle guides reach past the curve itself: a handle is
    // pulled out from its anchor, often well outside the box the sampled path
    // occupies, and the chrome around it would otherwise be left on screen.
    for (const Point &point : gesture_->points) {
        bounds = uniteLogical(bounds, LogicalRect{point.x, point.y, 1u, 1u});
    }
    return growBy(uniteLogical(bounds, band),
                  annotationReach(previewAnnotation()) + kSelectionChrome);
}

// The mark the pen path in progress would commit: the path's anchors and
// handles as they stand.  The preview, its bounds and the rubber band's own
// segment all read the same shape from it.
Annotation OverlayController::previewAnnotation() const
{
    Annotation preview;
    preview.kind = Annotation::Kind::Stroke;
    preview.tool = QStringLiteral("bezier");
    const ToolStyle &style = toolStyle(preview.tool);
    preview.color = style.color;
    preview.width = style.width;
    preview.fill = currentFill_;
    preview.dash = currentDash_;
    preview.points = gesture_->points;
    return preview;
}

bool OverlayController::drawsGrowingStroke() const
{
    // Only an opaque pen stroke can be baked a segment at a time: a translucent
    // one has to be composited as a whole, and the mosaic brush stamps blocks
    // the committed mark rebuilds from the source.
    // The live raster is only ever built for a stroke being drawn, so an
    // unarmed session -- which cannot draw -- never asks this.
    if (!tool_.has_value()) {
        return false;
    }
    return (tool_ == Tool::Pen && toolStyle(QStringLiteral("pen")).color.alpha() == 255) ||
           (tool_ == Tool::Mosaic && mosaicShape_ == QStringLiteral("brush"));
}

void OverlayController::updateTouch(const LogicalRect &touched)
{
    lastTouchLocal_ = QRect();
    if (touched.width == 0 || touched.height == 0) {
        updateAll();
        return;
    }
    // The step before this one was a full repaint -- the press that started the
    // gesture, a tool change -- so what is on the surface is not known from a
    // rect.  Cover everything once, and remember this rect so that the steps
    // which follow can be narrow: what a step has to erase is what the step
    // before it painted, and that is exactly this rect.
    if (!hasLastTouch_) {
        lastTouch_ = touched;
        hasLastTouch_ = true;
        repaintEverything();
        return;
    }
    const LogicalRect region = uniteLogical(touched, lastTouch_);
    lastTouch_ = touched;
    hasLastTouch_ = true;
    for (CaptureOverlay *overlay : overlays_) {
        LogicalRect visible;
        // The overlay drawable is the whole surface, not the output geometry: in
        // the pin editor the image (and the marks pinned to it) can be dragged
        // away from the rect the session recorded, and the magnifier and chrome
        // are clipped to the image's *current* place.  Clipping the damage to
        // the stale geometry would leave their pixels on the canvas.
        if (!intersection(region, surfaceOf(overlay->output()), &visible)) {
            continue;
        }
        // One pixel of slack: a mark's rounded or antialiased edge can spill
        // past the rect its geometry reports.
        const QRect local =
            localRect(overlay->output(), visible, overlay->size()).toAlignedRect().adjusted(
                -1, -1, 1, 1);
        lastTouchLocal_ = local;
        overlay->update(local);
    }
    if (toolbar_ != nullptr && toolbar_->isVisible()) {
        toolbar_->syncState();
        updateToolbarGeometry();
    }
}

QRect OverlayController::lastInteractiveUpdate() const
{
    return lastTouchLocal_;
}

QString OverlayController::shortcutText(ShortcutAction action) const
{
    // Nothing for a binding the user cleared: a tooltip that named a key the
    // editor will not act on would be a lie the settings page itself told.
    return shortcuts_.hasKeys(action) ? shortcuts_.textFor(action) : QString();
}

QString OverlayController::shortcutHint(ShortcutAction action, const QString &label) const
{
    const QString keys = shortcutText(action);
    return keys.isEmpty() ? label : QStringLiteral("%1 (%2)").arg(label, keys);
}

// Repaints the overlays' part of `region`, nothing else.  Unlike `updateTouch`
// this leaves the "last touch" bookkeeping alone: it exists for damage that is
// not a gesture step (the pin editor's daemon confirmation), which must not
// clobber the rect a gesture's next step unions against.
void OverlayController::invalidateLogicalRegion(const LogicalRect &region)
{
    if (region.width == 0 || region.height == 0) {
        return;
    }
    for (CaptureOverlay *overlay : overlays_) {
        LogicalRect visible;
        if (!intersection(region, surfaceOf(overlay->output()), &visible)) {
            continue;
        }
        const QRect local =
            localRect(overlay->output(), visible, overlay->size()).toAlignedRect().adjusted(
                -1, -1, 1, 1);
        overlay->update(local);
    }
}

void OverlayController::press(CaptureOverlay *overlay, const QPointF &local,
                              Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    if (finished_ || cancelled_) {
        return;
    }
    // A loose drag still held here belongs to a press whose release never
    // arrived (a grab lost mid-drag); this press replaces it either way.
    looseDrag_.reset();
    if (button == Qt::RightButton) {
        // The right button is the magnifier's, not a cancel: it is the button
        // the user holds while aiming at a pixel.  Escape is the cancel, and
        // always was the one the overlay's own text offered.
        magnifierHeld_ = true;
        endMagnifierFlash();
        pointer_ = globalPoint(overlay, local);
        pointerOutput_ = overlay->outputIndex();
        lastMagnifierPointer_ = pointer_;
        updateAll();
        return;
    }
    if (button == Qt::MiddleButton) {
        // The middle button is the drag's: it moves a framed region by its body
        // or slides the pin's image under the marks.  It is a button rather than
        // a held modifier because the gesture is a *drag* -- the state has to be
        // on for as long as the pointer travels -- and a modifier made the user
        // hold a key down through the whole move while the left button did the
        // work.  A middle drag on a mark's rim still picks the mark up, exactly
        // as a left one does: the rim is a deliberate target on its own, and
        // taking it away under this button would make the one affordance the
        // pointer promises there the one that does not work.
        const Point grab = globalPoint(overlay, local);
        if (selection_.has_value() && editing_) {
            const int annotationHandle =
                selectedAnnotation_ >= 0 ? annotationHandleAt(grab) : 0;
            const int hit = annotationHitAt(grab);
            if (annotationHandle != 0) {
                beginAnnotationDrag(grab, true,
                                    shortcuts_.held(ShortcutAction::PreserveAspect,
                                                    static_cast<int>(modifiers)));
                updateAll();
                return;
            }
            if (hit >= 0 && annotationBorderOf(hit, grab) != 0) {
                selectAnnotation(hit);
                beginAnnotationDrag(grab, false);
                updateAll();
                return;
            }
            // The mark the pick-up modifier has already put under the pointer
            // is the one this press takes, wherever it lands -- the same
            // promise the left button makes, and the reason the mark follows a
            // drag that started nowhere near it.  Asking the selection's body
            // first would swallow that: the body covers nearly the whole
            // picture, so almost every loose drag starts inside it.
            if (looseSelect_ && selectedAnnotation_ >= 0 && !pinEdit_) {
                looseDrag_ = grab;
                updateAll();
                return;
            }
            // The eight resize handles are the region selection's: the CLI
            // refuses any size change to a pin, so a pin has only its body to
            // drag.  Handle 9 is that body, and it is the one that matters
            // here.
            const int handle = hitHandle(grab);
            if (!pinEdit_ && handle != 0 && handle != 9) {
                // A handle is the one thing that stretches, and a middle press
                // on it means the same as a left one.
                selectAnnotation(-1);
                beginSelectionGesture(grab, shortcuts_.held(ShortcutAction::PreserveAspect,
                                                            static_cast<int>(modifiers)));
                updateAll();
                return;
            }
            if (handle == 9) {
                selectAnnotation(-1);
                beginSelectionMove(grab);
                updateAll();
                return;
            }
        }
        return;
    }
    if (button != Qt::LeftButton) {
        return;
    }
    // The standalone translate overlay: the frame is the whole interaction.
    // While framing, a press starts the rectangle over -- the same drag a
    // region-only session has -- and once a translation is up the frame is
    // frozen, so a click cannot drag a box the painted translation would not
    // follow; Escape is what takes it back to framing.
    if (translateMode_) {
        if (!translated_) {
            startSelection(globalPoint(overlay, local));
            updateAll();
        }
        return;
    }
    if (textMode_) {
        // The mode owns the left button: a press on a character starts the
        // range there, a press on the empty canvas clears it.  No tool path
        // runs while the mode is on, and the pointer the mode selects with is
        // the press itself, so the range starts where the button went down.
        pointer_ = globalPoint(overlay, local);
        pointerOutput_ = overlay->outputIndex();
        const int index =
            textLayer_.has_value() ? textLayer_->indexAt(pointer_.x, pointer_.y) : -1;
        if (index >= 0) {
            selectTextRange(index, index);
            textDragging_ = true;
        } else {
            textAnchor_ = -1;
            textFocus_ = -1;
            textDragging_ = false;
        }
        updateAll();
        return;
    }
    if (textEdit_ != nullptr) {
        finishText(true);
    }
    const Point point = globalPoint(overlay, local);
    if (pickMode_ && !editing_) {
        // The click takes the window under the pointer and ends the session:
        // picking only decides what to capture, and the pixels come from the
        // frame Rust captures once this overlay is off the screen.
        const int index = candidateIndexAt(point);
        if (index >= 0) {
            hoveredCandidate_ = index;
            selection_ = candidates_.at(index).rect;
            pointer_ = point;
            // Take the highlight off the screen first: the compositor destroys
            // these surfaces asynchronously, and a lingering copy of the
            // picker would end up in the very capture that follows.
            for (CaptureOverlay *item : overlays_) {
                item->hide();
            }
            terminal(false);
        } else {
            updateAll();
        }
        return;
    }
    // The membership tests below are asked of the *unclamped* point:
    // `globalPoint` folds a point outside the image onto its nearest edge, so
    // asking it would answer "on the image" for every point on the surrounding
    // canvas -- which is what made a click out there drag the pin.
    const Point raw = unclampedGlobalPoint(overlay, local);
    if (pinEdit_ && !insidePinImage(raw)) {
        // Outside the image itself.  The pin's own border is still the pin --
        // the daemon draws it centred on the image's edge, and the user aiming
        // at the rim means to move the pin, not to paint on the canvas -- so a
        // press there starts the same move a press on the image does.  It takes
        // no ink, because the mark would be drawn outside the picture and the
        // renderer clips it away.
        if (onPinBorder(raw) && selection_.has_value()) {
            // The rim is the pin too, and a press on it is the same "this one"
            // as a press on the picture: ask for the pin back on top.
            requestPinRaise();
            selectAnnotation(-1);
            // Anchored on the raw point, not the clamped one: a press on
            // the border lands outside the image, and clamping it to the
            // edge would swallow the first pixels of the drag.
            gesture_->anchor = raw;
            gesture_->current = raw;
            gesture_->origin = *selection_;
            gesture_->handle = 9;
            gesture_->type = Gesture::Type::Moving;
            updateAll();
            return;
        }
        // The bare canvas: it drops the current annotation selection and lets
        // the press go.  A mark is not drawn out here -- there is no image for
        // it to sit on.
        selectAnnotation(-1);
        updateAll();
        return;
    }
    if (pinEdit_) {
        // A press on the image is the user saying which pin they mean, and while
        // an edit is open the pin under the pointer is the one being annotated.
        // The click cannot reach the pin's own surface to raise it the ordinary
        // way -- the editor's layer surface covers the output and holds the
        // keyboard -- so the editor asks the daemon instead.  Asked here, before
        // anything decides what the press *does*, because that is what a click
        // on a pin means in every other state: it comes to the front.
        requestPinRaise();
    }
    // A press on a mark picks it up whatever tool is armed, and so does a press
    // on the selected mark's own handles.  That is what makes the marks
    // adjustable without a tool of their own: the drawing tools keep drawing,
    // and the one thing a press does before it draws is ask whether it landed
    // on something already there.
    //
    // A mark's *body* is the exception.  Picking a mark up there and drawing on
    // it are the same gesture, and the tool the user armed is the one that has
    // to win: a pen that could not start a stroke on top of an existing mark
    // would be a pen that stops working wherever the picture is busiest.  So
    // the body answers to the pick-up modifier instead -- held while the press
    // is made, and only for the press -- which leaves the armed tool alone:
    // entering the state is not a tool change, so letting the modifier go puts
    // the user back exactly where they were.
    //
    // The handles are not gated on it.  They are small, deliberate targets that
    // can only mean one thing, and the user who aims at a mark's rim means to
    // stretch it whether or not a modifier is down.  Neither is the rim itself:
    // see below.
    const bool picksMarkUp = pickingMarks(static_cast<int>(modifiers));
    {
        const int annotationHandle = selectedAnnotation_ >= 0 ? annotationHandleAt(point) : 0;
        const int hit = annotationHitAt(point);
        // The rim of a mark the pointer is *not* on yet still counts as a hit
        // even with a tool armed, so that a mark can be picked up and moved
        // without the tool being put down first.  The body is the case that
        // cannot: there the armed tool and the pick-up are the same gesture, and
        // the tool is the one the user chose.  A rim has no such reading -- it
        // is a deliberate target on an outline, not a place to start a stroke.
        const int border = annotationBorderOf(hit, point);
        const int annotationIndex =
            (!tool_.has_value() || picksMarkUp || border != 0) ? hit : -1;
        if (annotationHandle != 0) {
            // A handle, which is the one thing that stretches: the whole of a
            // mark's edge moves it, and the eight small targets resize it.
            beginAnnotationDrag(point, true, shortcuts_.held(ShortcutAction::PreserveAspect,
                                                            static_cast<int>(modifiers)));
            updateAll();
            return;
        }
        if (annotationIndex >= 0) {
            selectAnnotation(annotationIndex);
            beginAnnotationDrag(point, false);
            updateAll();
            return;
        }
    }
    // The capture selection's own border and handles.  They are not gated on
    // the middle button: they are small, deliberate targets that can only mean
    // one thing, and the user who aims at the rim of the selection means to
    // resize it.  The *body* of the selection is the middle button's, and is
    // handled below.
    if (!pinEdit_ && selection_.has_value() && editing_) {
        const int handle = hitHandle(point);
        if (handle != 0 && handle != 9) {
            selectAnnotation(-1);
            beginSelectionGesture(point, shortcuts_.held(ShortcutAction::PreserveAspect,
                                                         static_cast<int>(modifiers)));
            updateAll();
            return;
        }
    }
    if (!tool_.has_value()) {
        // Nothing armed: the selection is adjusted, not drawn on.  The middle
        // button drags its body; a plain drag starts a new frame over it, which
        // is the one thing a bare drag has always meant on a capture.
        //
        // Loose mode is asked first, and is the one press that does not need
        // the middle button: a mark the user has just selected follows a drag
        // from anywhere, and "anywhere" includes the inside of the selection --
        // asking the selection's body first would swallow every loose drag that
        // started there, which is nearly all of them.
        if (looseSelect_ && selectedAnnotation_ >= 0 && !pinEdit_) {
            // Loose mode: the selected mark follows a drag from anywhere, so
            // this press is held back rather than acted on -- until it moves it
            // is still a click, and a click that lands on nothing lets the mark
            // go.
            looseDrag_ = point;
            updateAll();
            return;
        }
        if (pinEdit_) {
            // Empty image surface: the middle button is what starts the move,
            // the same as it is for a region selection's body.  Without it the
            // press lets go of the current selection and does nothing else --
            // the pin editor's image is not reframed by a drag (the CLI refuses
            // a size change), so a bare press there has nothing to mean, and the
            // pointer does not promise one either.
            selectAnnotation(-1);
        } else {
            // A bare drag on the canvas starts a new frame over the old one.
            // That is what an unarmed drag has always meant on a capture, and
            // it is the whole point of a region session opening with no tool
            // armed: the user who has just framed a region and wants a
            // different one draws it, rather than having to find the toolbar
            // first.  `startSelection` clears the old frame and puts the
            // overlay back into the framing state, so the drag that follows is
            // the same drag that made the first one.
            selectAnnotation(-1);
            startSelection(point);
        }
        updateAll();
        return;
    }
    const Tool armed = *tool_;
    if (armed == Tool::Text) {
        beginText(overlay, point);
        return;
    }
    if (armed == Tool::Number) {
        // One click, one badge.  Nothing waits for a release: the tool has no
        // drag to preview, so the press is the whole gesture.
        placeNumber(point);
        return;
    }
    if (armed == Tool::Picker) {
        // One click takes one pixel, for the same reason: there is no drag to
        // preview.  The style row already points at the tool the picker was
        // armed from, and the pick hands the session back to it -- a colour is
        // only worth anything on the tool that is about to draw with it.
        if (pickColorAt(overlay, point)) {
            chooseTool(pickerReturnTool_);
        }
        return;
    }
    if (armed == Tool::Bezier) {
        if (!canDrawAt(point)) {
            return;
        }
        // A press back onto the first anchor closes the path.  It commits here
        // rather than on a release: the click is the whole closing gesture, and
        // waiting for the button to come up would leave the filled shape
        // hanging on a press the user already made.
        const double reach =
            std::max(8.0, static_cast<double>(toolStyle(QStringLiteral("bezier")).width) * 2.0);
        if (gesture_->type == Gesture::Type::Bezier &&
            bezierAnchors(gesture_->points) >= 2 &&
            std::hypot(static_cast<double>(gesture_->points.constFirst().x - point.x),
                       static_cast<double>(gesture_->points.constFirst().y - point.y)) <= reach) {
            finishBezier(true);
            return;
        }
        if (gesture_->type != Gesture::Type::Bezier) {
            beginBezier(point);
        }
        // The anchor, and its outgoing handle starting on top of it: the drag
        // that follows pulls the handle out, and the incoming side is its
        // mirror, so the handle is symmetric by construction.
        const Point bounded = clampPoint(point);
        gesture_->points.push_back(bounded);
        gesture_->points.push_back(bounded);
        gesture_->current = bounded;
        updateAll();
        return;
    }
    {
        if (!canDrawAt(point)) {
            return;
        }
        beginDrawing(point);
        // The live raster is local to the overlay the stroke starts on; a
        // stroke that wanders onto another output is clipped away there anyway.
        gesture_->liveOutput = overlay->outputIndex();
    }
    updateAll();
}

void OverlayController::move(CaptureOverlay *overlay, const QPointF &local, Qt::MouseButtons buttons,
                             Qt::KeyboardModifiers modifiers)
{
    if (finished_ || cancelled_) {
        return;
    }
    const Point point = globalPoint(overlay, local);
    pointer_ = point;
    pointerOutput_ = overlay->outputIndex();
    lastModifiers_ = static_cast<int>(modifiers);
    // A real pointer motion takes the cursor back from the keyboard, and the
    // magnifier a key press put up has nothing left to point at -- but the
    // motion VShot's own warp makes is not the user moving anything, and
    // treating it as one would end the flash on the step that raised it.  See
    // `isPointerWarpEcho`; the echo is left to the walk that asked for it.
    if (!isPointerWarpEcho(point)) {
        keyboardCursor_.reset();
        endMagnifierFlash();
    }
    if (magnifierHeld_) {
        // The magnifier the right button holds is a colour picker, and a picker
        // is aimed *by* moving: it follows the pointer for as long as the
        // button is down, so what the pill says is always the pixel under the
        // cursor rather than the one the button went down on.  It is also the
        // one step that has to repaint without a gesture: no drag is under way,
        // so nothing else here would ask for the frame to be redrawn.
        //
        // No test on which buttons are down: the motion event that carries a
        // held button names it in `buttons`, so the right button is still
        // reported on every step of the very drag it is holding, and asking for
        // `NoButton` here -- as this once did -- made the whole step a no-op.
        // The loupe stayed where the button went down and the pill read the
        // pixel that was under the cursor when it did.
        const LogicalRect previous = pointerTouchAt(lastMagnifierPointer_);
        lastMagnifierPointer_ = point;
        updateTouch(uniteLogical(previous, pointerTouchAt(point)));
        // ...but the loupe is a *second* reading of the same motion, not a
        // replacement for it.  The right button is how the user aims at a pixel
        // while a gesture is already under way -- a rectangle being drawn, a
        // pin being dragged -- and returning here swallowed every step of that
        // gesture: the preview froze at the point the button went down and only
        // moved again once it was released.  Only a step with no gesture to
        // serve ends here.
        if (gesture_->type == Gesture::Type::None) {
            return;
        }
    }
    if (textMode_) {
        // The mode has the pointer to itself: no candidate hover, no gesture.
        // A drag widens the range to the character nearest the pointer, which
        // is what keeps a drag that left the text meaning something; an idle
        // pointer is the caret the mode selects with.
        if (textDragging_ && textLayer_.has_value()) {
            const int index = textLayer_->nearestIndex(point.x, point.y);
            if (index >= 0) {
                selectTextRange(textAnchor_, index);
                updateAll();
            }
        } else {
            overlay->setCursor(Qt::IBeamCursor);
        }
        return;
    }
    if (looseDrag_.has_value()) {
        // A loose drag is held back until it actually moves, so a press that
        // never travels is still a click.  Past the threshold the mark picks up
        // the drag from where the button went down, which is what lets the
        // pointer be nowhere near the mark it is moving.
        if (buttons == Qt::NoButton) {
            // The button went up without a release event reaching us (a widget
            // change, a grab lost): treat it as the click it was.
            looseDrag_.reset();
        } else {
            const std::int64_t dx = static_cast<std::int64_t>(point.x) - looseDrag_->x;
            const std::int64_t dy = static_cast<std::int64_t>(point.y) - looseDrag_->y;
            if (dx * dx + dy * dy <= 16) {
                overlay->setCursor(Qt::SizeAllCursor);
                return;
            }
            const Point anchor = *looseDrag_;
            looseDrag_.reset();
            beginAnnotationDrag(anchor, false);
            updateAnnotationDrag(point);
            updateTouch(annotationTouch());
            return;
        }
    }
    if (tool_ == Tool::Picker && gesture_->type == Gesture::Type::None) {
        // The eyedropper reads one pixel at a time, so its loupe follows the
        // idle pointer rather than a drag: the readout is the instrument.  The
        // step repaints the loupe and its pill where they were and where they
        // are now -- `pointerTouch` bounds both around the pointer -- so the
        // magnified frame is never left behind on the surface.
        overlay->setCursor(Qt::CrossCursor);
        updateTouch(pointerTouch());
        return;
    }
    if (pickMode_ && !editing_ && buttons == Qt::NoButton &&
        gesture_->type == Gesture::Type::None) {
        // Nothing is committed yet: the pointer only previews which window a
        // click would take.  Travelling is also the moment to re-check what
        // the compositor has — a workspace switch or a moved window since the
        // list was taken would otherwise leave the highlight pointing at
        // nothing (or at the wrong window).
        requestCandidateRefresh();
        if (applyCandidateHover(point, overlay)) {
            updateAll();
        }
        return;
    }
    const Point raw = unclampedGlobalPoint(overlay, local);
    // The pin's border counts as the pin for the pointer's own sake: aiming at
    // the rim is aiming at the image, so the cursor says "drag me" there too.
    const bool insideImage = insidePinImage(raw) || onPinBorder(raw);
    if (pinEdit_ && !insideImage && buttons == Qt::NoButton &&
        gesture_->type == Gesture::Type::None) {
        // Bare canvas around the pin image: the toolbar lives there, so the
        // canvas keeps a neutral pointer and no crosshair.
        overlay->setCursor(Qt::ArrowCursor);
        return;
    }
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
    } else if (pinEdit_) {
        // Inside the image the pointer announces the drag that moves it;
        // anywhere else with a drawing tool it is the crosshair.  A mark's own
        // rim answers first, exactly as it does on the canvas: a press there
        // picks the mark up rather than starting a stroke, so the pointer has to
        // say so -- otherwise the one target that stays live with a tool armed
        // is the one the cursor lies about.
        const int border = insideImage ? annotationBorderAt(point) : 0;
        if (border != 0) {
            overlay->setCursor(Qt::SizeAllCursor);
        } else if (insideImage && pickingMarks(static_cast<int>(modifiers))
                   && annotationHitAt(point) >= 0) {
            overlay->setCursor(Qt::SizeAllCursor);
        } else {
            // The bare image says "drag me" only while the middle button is
            // down, exactly as the region selection's body does: that button is
            // the only way the drag can start, so without it the pointer is
            // promising a drag the press will not make.
            const bool drags = (buttons & Qt::MiddleButton) != 0;
            overlay->setCursor(insideImage && drags ? Qt::SizeAllCursor : Qt::ArrowCursor);
        }
        // The hover frame and the pick-up selection follow the pointer in here
        // too.  The pin editor opens on marks already on the canvas -- that is
        // the whole point of a re-edit -- so leaving this out made the one
        // session with marks to pick up the one session where picking them up
        // did nothing.
        if (insideImage) {
            refreshMarkHover();
        }
    } else {
        // The selection's chrome is live whatever is armed, so the pointer says
        // what each part of it does.  Its handles map to their resize arrows;
        // its body says "drag me" only while the middle button is down, because
        // that is the only way it can be dragged -- without it the press draws
        // a new frame instead, and a move cursor there would be a promise the
        // editor does not keep.
        const int handle = selection_.has_value() && editing_ ? hitHandle(point) : 0;
        if (handle != 0) {
            const bool drags = (buttons & Qt::MiddleButton) != 0;
            overlay->setCursor(handle == 9 ? (drags ? Qt::SizeAllCursor : Qt::CrossCursor)
                                           : cursorForHandle(handle));
        } else {
            // Not the selection: a mark's own rim answers next.  It is live on
            // its own -- a press there picks the mark up and drags it, with
            // nothing held and nothing armed -- so the pointer says "drag me"
            // rather than promising the stroke the press will not make.  The
            // handles are the mark's own and are drawn only once it is selected,
            // so before that the rim is all there is to aim at.
            const int border = annotationBorderAt(point);
            if (border != 0) {
                overlay->setCursor(Qt::SizeAllCursor);
            } else if (pickingMarks(static_cast<int>(modifiers))
                       && annotationHitAt(point) >= 0) {
                overlay->setCursor(Qt::SizeAllCursor);
            } else {
                overlay->setCursor(Qt::CrossCursor);
            }
        }
        // The mark the pick-up modifier has put under the pointer wears a frame
        // of its own, which is the feedback that the press would take it and not
        // draw.  Repainted only where it changed: the whole point of it is that
        // it appears and disappears as the pointer crosses a mark, and a full
        // repaint per motion would make hovering cost what drawing costs.
        refreshMarkHover();
    }
    if (gesture_->type == Gesture::Type::Selecting) {
        updateSelection(point);
        updateTouch(selectionTouch());
    } else if (gesture_->type == Gesture::Type::Moving) {
        // The raw point again: a pin drag may have started on the border, and
        // clamping the motion would hold the pin still until the pointer had
        // travelled all the way onto the image.
        applySelectionMove(gesture_->origin, gesture_->anchor, pinEdit_ ? raw : point);
        updateTouch(selectionTouch());
    } else if (gesture_->type == Gesture::Type::Resizing) {
        selection_ = resizeSelection(gesture_->origin, gesture_->handle, clampPoint(point),
                                     gesture_->preserveAspect);
        updateTouch(selectionTouch());
    } else if (gesture_->type == Gesture::Type::MovingAnnotation ||
               gesture_->type == Gesture::Type::ResizingAnnotation) {
        updateAnnotationDrag(point);
        updateTouch(annotationTouch());
    } else if (gesture_->type == Gesture::Type::Drawing) {
        const int pointsBefore = gesture_->points.size();
        updateDrawing(point);
        updateTouch(drawingTouch(pointsBefore));
    } else if (gesture_->type == Gesture::Type::Bezier) {
        // With the button down the pointer is pulling the last anchor's handle
        // out; with it up the pointer only says where the rubber band reaches.
        // Both are the same step to the path, and both repaint the same rect.
        updateBezier(point, buttons != Qt::NoButton);
        updateTouch(bezierTouch());
    } else {
        return;
    }
}

void OverlayController::release(CaptureOverlay *overlay, const QPointF &local,
                                Qt::MouseButton button, Qt::KeyboardModifiers modifiers)
{
    Q_UNUSED(modifiers);
    if (finished_ || cancelled_) {
        return;
    }
    if (button == Qt::RightButton) {
        // Letting the right button go puts the magnifier away; the gesture it
        // was held over is untouched, which is the whole point of the button
        // being the magnifier's rather than a cancel.
        if (magnifierHeld_) {
            magnifierHeld_ = false;
            updateAll();
        }
        return;
    }
    if (button == Qt::MiddleButton) {
        // The middle button owns the two drags that move rather than draw, and
        // both of them are started and finished by the left button's own code
        // paths -- the gesture is the same gesture either way, only the button
        // that began it differs.  So the release is the ordinary one, and the
        // left button's guard below is simply not applied to it.  A loose drag
        // is the exception: it is a press held back until it travels, so it is
        // still a click while the button is down and has to reach the code
        // below that lets the mark go.
        if (!looseDrag_.has_value() && gesture_->type != Gesture::Type::Moving &&
            gesture_->type != Gesture::Type::MovingAnnotation) {
            return;
        }
    } else if (button != Qt::LeftButton) {
        return;
    }
    if (textMode_) {
        textDragging_ = false;
        return;
    }
    if (looseDrag_.has_value()) {
        // The press never travelled, so it was a click on nothing: the mark is
        // let go, and with nothing selected the next drag reaches the capture
        // selection again.  The selection itself is left as it was -- a click
        // is not how one is drawn.
        looseDrag_.reset();
        selectAnnotation(-1);
        updateAll();
        return;
    }
    const Point point = globalPoint(overlay, local);
    // The keyboard walks the cursor while a stroke is in progress, and the
    // button is still down for the whole of it -- so the release arrives at
    // wherever the mouse was when the press was made, which is the anchor.  The
    // far end is the cursor's, not the release's: taking the release's position
    // would throw away every step the keys just made.
    const Point end = (gesture_->type == Gesture::Type::Drawing ||
                       gesture_->type == Gesture::Type::Bezier)
        ? gesture_->current
        : point;
    switch (gesture_->type) {
    case Gesture::Type::Selecting:
        toolbarOutput_ = overlay->outputIndex();
        finishSelection(point);
        break;
    case Gesture::Type::Moving:
        // The raw point, to match `move()`: a pin drag that started on the
        // border would otherwise land a few pixels short of the pointer.
        applySelectionMove(gesture_->origin, gesture_->anchor,
                           pinEdit_ ? unclampedGlobalPoint(overlay, local) : point);
        gesture_->type = Gesture::Type::None;
        showToolbar();
        break;
    case Gesture::Type::Resizing:
        selection_ = resizeSelection(gesture_->origin, gesture_->handle, clampPoint(point),
                                     gesture_->preserveAspect);
        gesture_->type = Gesture::Type::None;
        showToolbar();
        break;
    case Gesture::Type::MovingAnnotation:
    case Gesture::Type::ResizingAnnotation:
        finishAnnotationDrag(overlay, point);
        break;
    case Gesture::Type::Drawing:
        finishDrawing(end);
        break;
    case Gesture::Type::Bezier:
        // A release only ends the handle drag that followed the last press.  The
        // path itself is not finished until it is closed or double-clicked, so
        // the gesture stays in progress and the preview stays on screen.
        updateBezier(end, false);
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
    if (textMode_) {
        // A double click takes the word under the pointer, the way it does in
        // a text field, and the index is -1 off the text, which takes nothing.
        //
        // A third click is the same event: Qt has no triple-click, it reports
        // the second *and* the third press of a rapid run as a double click.
        // So a double click that lands within the platform's double-click
        // interval of the last one is the third press, and widens the word to
        // its whole line -- otherwise a triple click would read as a second
        // double click and reselect the same word.
        const int index = textLayer_.has_value() ? textLayer_->indexAt(point.x, point.y) : -1;
        const bool third = index >= 0 && textClickClock_.isValid() &&
            textClickClock_.elapsed() <= QApplication::doubleClickInterval();
        textClickClock_.restart();
        if (third) {
            textSelectLine(index);
        } else {
            textSelectWord(index);
        }
        return;
    }
    if (tool_ == Tool::Bezier && gesture_->type == Gesture::Type::Bezier) {
        // A double click ends the path where it stands, open.  Qt delivers the
        // second click of the pair as this event rather than as a press, so the
        // anchor it would have placed is the one the first click already did --
        // the path is not left with a duplicate point on its end.
        finishBezier(false);
        return;
    }
    const int index = annotationHitAt(point);
    if (index >= 0 && annotations_.at(index).kind == Annotation::Kind::Text &&
        !isNumberAnnotation(annotations_.at(index))) {
        // Only a typed label re-edits.  Opening the editor over a badge would
        // take the badge out of the list and commit a label in its place.
        startTextEditor(overlay, index, annotations_.at(index).origin);
        return;
    }
    if (selection_.has_value() && !pinEdit_ && hitHandle(point) != 0) {
        confirm();
    }
}

void OverlayController::key(CaptureOverlay *overlay, int key, Qt::KeyboardModifiers modifiers)
{
    Q_UNUSED(overlay);
    // Shift itself arrives here as a key with no keycode of its own, and it is
    // the pick-up modifier: holding it is what puts the hover frame on the mark
    // under the pointer, so the frame has to follow the modifier and not only
    // the pointer.  The stored bits are updated before anything acts on them so
    // that the whole of this press sees one state.
    const int bits = static_cast<int>(modifiers);
    if (bits != lastModifiers_) {
        lastModifiers_ = bits;
        refreshMarkHover();
    }
    const QKeySequence pressed(
        static_cast<int>(static_cast<int>(modifiers) | static_cast<int>(key)));
    // Everything below looks a key up in the binding table rather than
    // comparing it against a hard-coded `Qt::Key_`, so a binding the user has
    // changed in the settings is the one honoured here.  The pressed key is
    // built once and asked about; `ShortcutPreferences::matches` is the only
    // place that knows how a stored sequence and a pressed one are compared.
    const auto is = [this, &pressed](ShortcutAction action) {
        return shortcuts_.matches(action, pressed);
    };
    if (is(ShortcutAction::Cancel)) {
        if (translateMode_) {
            // The translation goes first, back to the framing it replaced; a
            // second Escape is the cancel the framing has.
            if (translated_) {
                translated_ = false;
                translatedLines_.clear();
                translatedText_.clear();
                updateAll();
            } else {
                cancel();
            }
        } else if (textEdit_ != nullptr) {
            finishText(false);
        } else if (textMode_) {
            // The mode goes first: Escape leaves the text selection without
            // ending the capture, so a second Escape is what cancels it.
            leaveTextMode();
        } else if (gesture_->type == Gesture::Type::Bezier) {
            // The pen path in progress goes first: Escape drops it without
            // ending the session, the way it drops any other in-progress
            // gesture.  A second Escape then cancels the capture.
            gesture_->type = Gesture::Type::None;
            gesture_->points.clear();
            updateAll();
        } else {
            cancel();
        }
        return;
    }
    if (is(ShortcutAction::Confirm)) {
        if (translateMode_) {
            // Enter keeps its two stages: it translates a frame nothing has
            // translated yet -- the drag that drew it normally has, so this is
            // the re-run after an Escape -- and accepts the translation that is
            // up.  The same key walks both because the overlay has no toolbar
            // to put a button in.
            const bool accepting = translated_;
            QString error;
            const bool ok = accepting ? acceptTranslation(&error) : runTranslateStage(&error);
            if (!ok) {
                std::fprintf(stderr, "vshot-qt-ui: %s\n", error.toUtf8().constData());
                std::fflush(stderr);
            } else if (accepting) {
                terminal(false);
            }
        } else if (textEdit_ != nullptr) {
            finishText(true);
        } else if (textMode_) {
            // Enter copies what the range holds and leaves the mode.
            copyTextSelection();
        } else {
            confirm();
        }
        return;
    }
    if (textEdit_ != nullptr) {
        return;
    }
    if (translateMode_) {
        // Only the copy and the step back reach the translate overlay; every
        // other key is swallowed so nothing moves the frame under it.
        if (is(ShortcutAction::CopyText) && translated_) {
            writeClipboard(translatedText_);
        }
        return;
    }
    if (textMode_) {
        // The mode owns the keys: copying the range and selecting it all are
        // the two it answers, and every other key is swallowed so the arrow
        // keys cannot move the capture's selection out from under the text.
        if (is(ShortcutAction::CopyText)) {
            copyTextSelection();
        } else if (is(ShortcutAction::SelectAll)) {
            textSelectAll();
        }
        return;
    }
    if (is(ShortcutAction::Undo)) {
        undo();
        return;
    }
    if (is(ShortcutAction::Redo)) {
        redo();
        return;
    }
    if (is(ShortcutAction::Paste)) {
        // Paste is the one action here that can fail for a reason the user
        // needs told: an empty clipboard, or `wl-paste` missing. There is
        // no status line on a frozen overlay, so the message goes to stderr
        // where the CLI's own diagnostics already land.
        QString error;
        if (!pasteFromClipboard(&error)) {
            std::fprintf(stderr, "vshot-qt-ui: %s\n", error.toUtf8().constData());
            std::fflush(stderr);
        }
        return;
    }
    if (is(ShortcutAction::Copy)) {
        // Copy the capture with its marks, through the same composite the
        // Copy button makes.
        copyToClipboard();
        return;
    }
    if (is(ShortcutAction::SelectAll)) {
        // Every mark, so a style or a delete reaches all of them at once.
        selectAllAnnotations();
        return;
    }
    if (is(ShortcutAction::SelectNone)) {
        selectAnnotation(-1);
        return;
    }
    if (is(ShortcutAction::NextMark) || is(ShortcutAction::PreviousMark)) {
        // Tab and Shift+Tab walk the marks; Ctrl+Tab does the same, so a run
        // of them can be walked in either direction without Shift.
        const int step = is(ShortcutAction::PreviousMark) ? -1 : 1;
        cycleAnnotationFocus(step);
        return;
    }
    if (colorPickerVisible()) {
        // The picker owns its two colour keys while it is up: the user is
        // looking at a pixel through it, and those are the two things to do
        // with one.  They are bound only here so the letters stay free for the
        // keyboard cursor below when no picker is up.  Neither is a letter the
        // cursor walk wants -- `C` and `V` are not among the WASD keys -- and
        // that matters most here: the picker is up while the right button is
        // held, which is the one moment the pointer is still being moved, so
        // the cursor keys have to keep working underneath it.
        if (is(ShortcutAction::CopyColor)) {
            copyColorUnderCursor();
            return;
        }
        if (is(ShortcutAction::AdoptColor)) {
            adoptColorUnderCursor();
            return;
        }
    }
    if (is(ShortcutAction::ShowMagnifier)) {
        // The magnifier on demand: the same two seconds a cursor step gives
        // the user, asked for directly.  A cursor step is what usually raises
        // it, and a step ends by repainting -- so `flashMagnifier` on its own
        // here set the flag and left the frame alone, and the key looked dead
        // until something else happened to redraw.
        flashMagnifier();
        updateAll();
        return;
    }
    if (is(ShortcutAction::Delete) && gesture_->type == Gesture::Type::None
        && selectedAnnotation_ >= 0) {
        deleteSelectedAnnotation();
        return;
    }
    if (gesture_->type == Gesture::Type::None || gesture_->type == Gesture::Type::Selecting ||
        gesture_->type == Gesture::Type::Drawing || gesture_->type == Gesture::Type::Bezier) {
        // Walking the cursor with the keyboard.  The point of it is to pick a
        // spot the mouse cannot reach exactly -- the corner of a region, the
        // end of an arrow -- and a step of one pixel is what makes that worth
        // doing; the step modifier takes ten at a time for the coarse part of
        // the trip.
        //
        // The arrow keys nudge the mark the keyboard has selected, when there
        // is one, and walk the cursor when there is not -- which is the state
        // framing is in.  WASD always walks the cursor, so the two are both
        // reachable without letting go of a mark.  Which of the two a key is
        // comes from the binding: an arrow is a nudge, a letter is a walk.
        // The step modifier is read as part of the step's *size*, so it is not
        // part of the key the step is bound to: Shift+Left is "left, ten
        // pixels", not a key of its own.  Every comparison below therefore
        // matches with those bits taken out of both sides.
        //
        // A stroke in progress is walked too.  Its press chose the start and
        // its release will choose the end, so the button is held for the whole
        // of it -- and that is precisely when the mouse cannot place the end
        // exactly, which is what the walk is for.  The step moves the *live*
        // gesture rather than the committed marks: the anchor the press set
        // stays where it is and the far end follows, which is the same edit the
        // pointer would have made, made a pixel at a time.
        const int coarse = shortcuts_.held(ShortcutAction::CoarseStep, static_cast<int>(modifiers))
            ? static_cast<int>(Qt::ShiftModifier)
            : 0;
        const int step = coarse != 0 ? kCoarseCursorStep : 1;
        const auto steps = [this, &pressed, coarse](ShortcutAction action) {
            return shortcuts_.matches(action, pressed, coarse);
        };
        int dx = 0;
        int dy = 0;
        bool nudge = false;
        if (steps(ShortcutAction::CursorLeft)) {
            dx = -step;
            nudge = pressedArrow(key);
        } else if (steps(ShortcutAction::CursorRight)) {
            dx = step;
            nudge = pressedArrow(key);
        } else if (steps(ShortcutAction::CursorUp)) {
            dy = -step;
            nudge = pressedArrow(key);
        } else if (steps(ShortcutAction::CursorDown)) {
            dy = step;
            nudge = pressedArrow(key);
        }
        if (dx != 0 || dy != 0) {
            if (gesture_->type == Gesture::Type::Drawing ||
                gesture_->type == Gesture::Type::Bezier) {
                walkLiveGesture(dx, dy);
            } else if (nudge && selectedAnnotation_ >= 0) {
                nudgeSelectedAnnotation(dx, dy);
            } else {
                moveCursorBy(dx, dy);
            }
            return;
        }
    }
    if (!selection_.has_value() || !editing_ || gesture_->type != Gesture::Type::None) {
        return;
    }
    if (pinEdit_) {
        // The pin editor's selection is the image itself: it moves with the
        // image (drag or arrow keys) and never resizes.
        return;
    }
}

void OverlayController::walkLiveGesture(int dx, int dy)
{
    // The far end is the gesture's own, not the pointer record's: the preview on
    // screen is drawn from `gesture_->current`, so continuing from there is what
    // makes the ink follow the keys even if the pointer never reported the press
    // (a session driven without a mouse, or one whose press arrived on a surface
    // whose motion never came).
    const Point from = gesture_->current;
    const Point moved = clampPoint(Point{from.x + dx, from.y + dy});
    // The point the press set stays exactly where it was: a stroke is drawn
    // from its anchor, and walking the cursor is choosing where the far end
    // lands, not redrawing what is already there.  For the pen and the brush
    // that is the whole of it -- they grow toward the cursor and the steps in
    // between are the stroke -- so this is the pointer moving without a mouse.
    if (gesture_->type == Gesture::Type::Bezier) {
        // A pen path is anchors, not a trail: the last anchor has been placed
        // and what the pointer does now is pull its outgoing handle out.  A
        // path with nothing placed yet has no anchor to pull, and the step
        // would have nowhere to land.
        if (gesture_->points.size() < 2) {
            return;
        }
        updateBezier(moved, true);
        updateTouch(bezierTouch());
    } else {
        // A growing stroke stamps the segments the last step added, so the walk
        // tells `updateDrawing` the same way a pointer motion would and lets it
        // append the one point this step contributes.  The two-point tools
        // replace their pair outright, which is what makes the far end follow.
        const int pointsBefore = gesture_->points.size();
        updateDrawing(moved);
        updateTouch(drawingTouch(pointsBefore));
    }
    keyboardCursor_ = moved;
    pointer_ = moved;
    // The pointer is wherever the press left it -- the keyboard has not moved
    // the mouse -- so the magnifier is the only thing on screen that says which
    // pixel the far end is on, and the warp is what puts the arrow there too.
    flashMagnifier();
    requestPointerWarp(moved);
}

// The cursor the keyboard moves, in global logical pixels.  It starts wherever
// the pointer last was, so a key press continues from what the user was
// looking at rather than jumping to a corner of the screen.
Point OverlayController::cursorPoint() const
{
    if (keyboardCursor_.has_value()) {
        return *keyboardCursor_;
    }
    if (pointerOutput_ >= 0) {
        return pointer_;
    }
    const LogicalRect &surface = session_.bounds;
    return Point{static_cast<std::int32_t>(surface.x + static_cast<std::int64_t>(surface.width) / 2),
                 static_cast<std::int32_t>(surface.y + static_cast<std::int64_t>(surface.height) / 2)};
}

void OverlayController::moveCursorBy(int dx, int dy)
{
    const Point moved = clampPoint(Point{cursorPoint().x + dx, cursorPoint().y + dy});
    keyboardCursor_ = moved;
    pointer_ = moved;
    // The magnifier is the only way to see where the cursor went: it has not
    // moved a mouse, so the pointer the compositor draws is wherever it was
    // left, and the user is aiming at a pixel.  It comes up for a moment and
    // goes again, so the frame is not permanently covered by it.
    flashMagnifier();
    // And the pointer itself, which is the other half of "where the cursor
    // went": the loupe says which pixel, and the arrow on the screen says where
    // on the desktop.  Only the CLI can move it, so the request goes over the
    // pipe the session came in on.  A session with nobody listening is left
    // alone -- the editor's own cursor has already moved either way.
    requestPointerWarp(moved);
    updateAll();
}

bool OverlayController::pickingMarks(int modifiers) const
{
    // Read through the binding table so a build that moves the modifier moves
    // this with it.  `held` is the only reader that can see it: `matches` wants
    // a key *and* a modifier, and this is held on its own while a press is
    // made, with no key of its own to match.
    return shortcuts_.held(ShortcutAction::SelectMark, modifiers);
}

void OverlayController::cycleAnnotationFocus(int step)
{
    if (annotations_.isEmpty()) {
        return;
    }
    const int count = annotations_.size();
    int index = selectedAnnotation_;
    if (index < 0 || index >= count) {
        // Nothing selected yet: Tab starts at the back of the list and Shift+
        // Tab at the front, so the first press lands on the mark nearest the
        // end the user is coming from.
        index = step > 0 ? -1 : 0;
    }
    index = ((index + step) % count + count) % count;
    selectAnnotation(index);
    // The cursor follows the mark, so the magnifier and the colour readout
    // point at the thing the keyboard just picked.
    LogicalRect bounds;
    if (annotationBounds(annotations_.at(index), &bounds)) {
        keyboardCursor_ = Point{static_cast<std::int32_t>(bounds.x + static_cast<std::int64_t>(bounds.width) / 2),
                                static_cast<std::int32_t>(bounds.y + static_cast<std::int64_t>(bounds.height) / 2)};
        pointer_ = *keyboardCursor_;
        if (pointerOutput_ < 0) {
            pointerOutput_ = outputContaining(bounds);
        }
    }
    updateAll();
}

void OverlayController::selectAllAnnotations()
{
    if (annotations_.isEmpty()) {
        return;
    }
    selectAnnotation(annotations_.size() - 1);
    allSelected_ = true;
    updateAll();
}

void OverlayController::nudgeSelectedAnnotation(int dx, int dy)
{
    if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size()) {
        return;
    }
    const Annotation original = annotations_.at(selectedAnnotation_);
    LogicalRect bounds;
    if (!annotationBounds(original, &bounds)) {
        return;
    }
    // The same translation a drag makes, through the same clamp, so a mark
    // walked to the edge of the image stops there rather than sliding off it.
    const Annotation moved = translatedAnnotation(original, dx, dy);
    LogicalRect after;
    if (annotationBounds(moved, &after) && after.x == bounds.x && after.y == bounds.y) {
        return; // already against the edge the nudge was pushing toward
    }
    // A run of nudges is one edit: the first one of the run is what undo comes
    // back to, and the key's own auto-repeat does not bury the user's last real
    // step under a hundred entries.  Any other edit or selection ends the run.
    if (!nudgeBase_.has_value()) {
        nudgeBase_ = annotations_;
        undoStack_.push_back(*nudgeBase_);
        if (undoStack_.size() > kMaxUndoSteps) {
            undoStack_.removeFirst();
        }
        redoStack_.clear();
    }
    annotations_[selectedAnnotation_] = moved;
    updateAll();
}

// Moves the selection — the pinned image, in pin-edit mode — to follow the
// pointer.
//
// Marks are stored in global coordinates and must remain aligned to the FP16
// helper surface, which shows the image at the *daemon-confirmed* position.
// Translating marks here (the optimistic position) races the daemon: by the
// time the reply arrives, the cursor has often moved on, and applyPinRect then
// corrects against an already-moved selection_, shifting marks the wrong way
// and making them appear to fragment.  The marks are only moved in
// applyPinRect, when the daemon says where the image actually is.
void OverlayController::applySelectionMove(LogicalRect origin, Point anchor, Point current)
{
    const LogicalRect moved = moveSelection(origin, anchor, current);
    if (pinEdit_ && selection_.has_value()) {
        // Optimistically track where the cursor would like the image to be,
        // for the daemon request and for computing the next incremental delta.
        // Do NOT translate annotations: they stay at marksOrigin_ (the last
        // confirmed daemon position) until applyPinRect updates them.
        requestPinMove(Point{moved.x, moved.y});
    }
    selection_ = moved;
}

// The editor has drawn everything it is going to draw, and asks to be let go.
//
// It does not stop here.  A session that rendered a capture is showing that
// capture, and the caller has not been told about it yet -- for a pin, the
// pixels the daemon is about to show come from a result this process has not
// even written.  The two pictures have to overlap or the user sees the marks
// blink out between them, so the editor asks the CLI to put its capture where
// it belongs first, and keeps drawing until it is answered.
//
// The ask carries the session's whole answer -- the result JSON and, over the
// pixel channel, the rendered capture -- written before the request so the CLI
// has both by the time it reads the request.  What the CLI does with them is
// its own business: for a pin it hands them to the daemon with an ask that
// holds the daemon's answer until the pin's own frame is on the screen, and for
// everything else it writes them out and answers at once.
//
// Answered by a line on stdin, by the pipe closing, or by the backstop: the
// session must not be able to outlive the request.  A helper run by hand -- or
// by a check -- has no CLI on the other end of this pipe, so there is nothing
// to hand over to and nothing to wait for; it quits, as it always did.
void OverlayController::beginHandoff()
{
    // The same test the pointer walk uses to tell a CLI from a terminal.
    if (::isatty(STDOUT_FILENO)) {
        QCoreApplication::quit();
        return;
    }
    QString resultError;
    const QJsonDocument result = resultDocument(&resultError);
    if (result.isNull() && !resultError.isEmpty()) {
        // Nothing to hand over, and the CLI's own read of the result would
        // report the same thing; saying it here leaves the reason on stderr
        // instead of in a half-written answer.
        std::fprintf(stderr, "vshot-qt-ui: %s\n", resultError.toUtf8().constData());
        QCoreApplication::quit();
        return;
    }
    const QByteArray encoded = result.toJson(QJsonDocument::Compact);
    std::fwrite(encoded.constData(), 1, static_cast<std::size_t>(encoded.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    resultSent_ = true;
    const int flags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
    // Any line at all is the answer: the CLI has nothing to tell the editor
    // beyond "it is done", and a refusal still means it is done with the
    // editor.  A pipe that closes means the same thing -- the CLI is gone, so
    // nothing is coming.
    handoffReader_ = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    QObject::connect(handoffReader_, &QSocketNotifier::activated, handoffReader_, [this] {
        char buffer[256];
        const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (got <= 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            QCoreApplication::quit();
            return;
        }
        if (got > 0) {
            QCoreApplication::quit();
        }
    });
    handoffClock_.start();
    handoffTimer_ = new QTimer(this);
    handoffTimer_->setSingleShot(true);
    QObject::connect(handoffTimer_, &QTimer::timeout, this, [this] { QCoreApplication::quit(); });
    // The backstop for a CLI that never answers at all.  The CLI waits on the
    // daemon, which has its own deadline, so this only fires when something in
    // between has gone; without it the editor would keep drawing for ever.
    handoffTimer_->start(kHandoffWaitMs);
    writeCliRequest(QByteArrayLiteral("{\"request\":\"release\"}\n"));
}

// Whether the session rendered a capture of its own, which is what has to be
// on the screen before this process may stop; see `beginHandoff`.
bool OverlayController::rendersCapture() const
{
    return pixelChannelAvailable() && selection_.has_value();
}

void OverlayController::setPinTarget(std::uint64_t pinId, const QString &socketPath)
{
    pinId_ = pinId;
    pinSocketPath_ = socketPath;
    // Connected now rather than at the first drag.  The daemon says which pin is
    // the live one on every connection it accepts, and the editor draws the
    // image's frame from that answer -- waiting for the user to move something
    // before asking would leave the frame off until then, and leave the answer
    // to a report racing the connection that carries it.
    if (pinId_ != 0 && !pinSocketPath_.isEmpty()) {
        openPinSocket();
    }
}

// The daemon's answer to "which pin is the live one".  A pin is live while this
// editor's own surface holds the keyboard and the pointer is over it; the
// moment the user clicks away, another pin takes both and the daemon says so.
//
// The editor's frame is the only thing this changes.  The marks, the input
// region and the drag are all about the edit, which is still open, and taking
// them away because the pointer went elsewhere would end an edit the user never
// ended.
void OverlayController::notePinActive(std::uint64_t pinId)
{
    const bool active = pinId != 0 && pinId == pinId_;
    if (active == pinActive_) {
        return;
    }
    pinActive_ = active;
    // Exactly the frame's own band, so losing it costs one stroke's worth of
    // pixels rather than the whole output.
    if (marksOrigin_.has_value()) {
        const LogicalRect band = *marksOrigin_;
        const std::int32_t reach = 2;
        invalidateLogicalRegion(LogicalRect{band.x - reach, band.y - reach,
                                            band.width + 2 * reach,
                                            band.height + 2 * reach});
    } else {
        repaintEverything();
    }
}

// Sends the pin's new top-left (global logical pixels) to the daemon that owns
// it and applies whatever comes back.  The daemon keeps the connection open and
// the protocol is newline-delimited, so a drag reuses one socket rather than
// opening and tearing down one per motion event.
void OverlayController::requestPinMove(Point globalTopLeft)
{
    if (pinSocketPath_.isEmpty() || pinId_ == 0) {
        return;
    }
    pendingPinOrigin_ = globalTopLeft;
    flushPinMove();
}

// Asks the daemon to put the pin being edited back on top of the stack.
//
// The click that means this cannot reach the pin's own surface: the editor's
// layer surface holds the keyboard and covers the output, so the pointer lands
// on the editor, and the pin surface underneath -- where a click normally raises
// a pin -- never sees it.  A press on the image is the user saying which pin
// they mean, and while an edit is open that is the only pin it can be, so the
// editor asks on their behalf.
void OverlayController::requestPinRaise()
{
    if (pinSocketPath_.isEmpty() || pinId_ == 0) {
        return;
    }
    if (pinSocket_ == nullptr) {
        // The pending raise goes out as soon as it connects; `pinRaisePending_`
        // is what carries it across the connect.
        pinRaisePending_ = true;
        openPinSocket();
        return;
    }
    if (pinSocket_->state() != QLocalSocket::ConnectedState) {
        pinRaisePending_ = true;
        return;
    }
    pinRaisePending_ = false;
    QJsonObject request;
    request.insert(QStringLiteral("cmd"), QStringLiteral("raise"));
    request.insert(QStringLiteral("id"), static_cast<qint64>(pinId_));
    QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact);
    line.append('\n');
    pinSocket_->write(line);
    pinSocket_->flush();
}

void OverlayController::flushPinMove()
{
    // A raise asked for before the connection was up goes first: it is what the
    // press that also started this move meant, and it is a restack the daemon
    // has to apply before the stack it paints for the move is composed.
    // `requestPinRaise` is what decides whether the connection is ready for it.
    if (pinRaisePending_) {
        requestPinRaise();
    }
    if (!pendingPinOrigin_.has_value() || pinSocketPath_.isEmpty() || pinId_ == 0) {
        return;
    }
    if (pinSocket_ == nullptr) {
        // The pending position goes out as soon as it connects.
        openPinSocket();
        return;
    }
    if (pinSocket_->state() != QLocalSocket::ConnectedState) {
        return;
    }
    if (pinMovesInFlight_ >= kPinMovesInFlight) {
        // The reply to an earlier position frees a slot and flushes the newest
        // one; keeping the queue short is what bounds the lead the marks can
        // take over the image the daemon has actually drawn.
        return;
    }
    const Point origin = *pendingPinOrigin_;
    pendingPinOrigin_.reset();
    QJsonObject request;
    request.insert(QStringLiteral("cmd"), QStringLiteral("move"));
    request.insert(QStringLiteral("id"), static_cast<qint64>(pinId_));
    request.insert(QStringLiteral("x"), static_cast<qint64>(origin.x));
    request.insert(QStringLiteral("y"), static_cast<qint64>(origin.y));
    QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact);
    line.append('\n');
    pinSocket_->write(line);
    pinSocket_->flush();
    ++pinMovesInFlight_;
    if (pinDebug_) {
        pinMoveClock_.start();
    }
}

// Opens the drag's one connection.  A refused or dropped connection is not
// fatal: the editor keeps working, it just cannot move the live pin, and the
// next move opens a fresh connection.
void OverlayController::openPinSocket()
{
    auto *socket = new QLocalSocket;
    pinSocket_ = socket;
    QObject::connect(socket, &QLocalSocket::connected, socket, [this] {
        pinMovesInFlight_ = 0;
        // Asking for the live-pin answer is what puts this connection in the
        // daemon's list of clients it writes to.  It is asked for rather than
        // pushed: an ordinary drag's client has no use for the lines, and a
        // daemon that wrote them to everyone would be answering the CLI's own
        // socket with something that is not the reply it is waiting for.
        QJsonObject watch;
        watch.insert(QStringLiteral("cmd"), QStringLiteral("watch-active"));
        QByteArray line = QJsonDocument(watch).toJson(QJsonDocument::Compact);
        line.append('\n');
        pinSocket_->write(line);
        pinSocket_->flush();
        flushPinMove();
    });
    QObject::connect(socket, &QLocalSocket::readyRead, socket, [this] { readPinReplies(); });
    const auto lost = [this] { dropPinSocket(); };
    QObject::connect(socket, &QLocalSocket::errorOccurred, socket,
                     [lost](QLocalSocket::LocalSocketError) { lost(); });
    QObject::connect(socket, &QLocalSocket::disconnected, socket, lost);
    socket->connectToServer(pinSocketPath_);
}

void OverlayController::dropPinSocket()
{
    if (pinSocket_ != nullptr) {
        // Cleared before deleteLater() so a repeated signal finds nothing to do.
        pinSocket_->deleteLater();
        pinSocket_ = nullptr;
    }
    pinReplyBuffer_.clear();
    pinMovesInFlight_ = 0;
}

// Drains every answer the daemon has sent.  One move is one reply, so each line
// retires one in-flight position and frees a slot for the newest one.
void OverlayController::readPinReplies()
{
    if (pinSocket_ == nullptr) {
        return;
    }
    pinReplyBuffer_ += pinSocket_->readAll();
    qsizetype newline = -1;
    while ((newline = pinReplyBuffer_.indexOf('\n')) >= 0) {
        const QByteArray line = pinReplyBuffer_.left(newline);
        pinReplyBuffer_.remove(0, newline + 1);
        // Only a move's own answer retires a move.  The daemon also says which
        // pin is the live one, on this same connection and at any time, and
        // counting that as a reply would free a slot the daemon has not
        // answered yet and let the queue grow past the bound that keeps the
        // marks from running ahead of the image.
        const QJsonObject reply = QJsonDocument::fromJson(line).object();
        const bool isMoveReply = reply.contains(QStringLiteral("ok"));
        if (isMoveReply && pinMovesInFlight_ > 0) {
            --pinMovesInFlight_;
        }
        if (pinDebug_ && isMoveReply) {
            std::fprintf(stderr, "vshot-qt-ui: pin move round trip %lld ms\n",
                         static_cast<long long>(pinMoveClock_.elapsed()));
            pinMoveClock_.restart();
        }
        if (!line.isEmpty()) {
            applyPinReply(line);
        }
    }
    flushPinMove();
}

void OverlayController::applyPinReply(QByteArray line)
{
    const QJsonDocument document = QJsonDocument::fromJson(line);
    const QJsonObject reply = document.object();
    // Not every line on this socket is a move's answer: the daemon also says
    // which pin is the live one, on the same connection and at any time, so the
    // frame can follow the user's attention rather than a drag.  A line with no
    // `ok` is not a move's reply at all and must not retire one of the moves in
    // flight -- reading it as a refusal would free a slot the daemon has not
    // answered yet and let the queue grow past its bound.
    if (!reply.contains(QStringLiteral("ok"))) {
        const QJsonValue active = reply.value(QStringLiteral("active"));
        if (active.isDouble()) {
            notePinActive(active.toVariant().toULongLong());
        }
        return;
    }    if (!reply.value(QStringLiteral("ok")).toBool()) {
        // The daemon refused (pin gone, bad request): keep editing locally.
        return;
    }
    LogicalRect landed;
    landed.x = static_cast<std::int32_t>(reply.value(QStringLiteral("x")).toInteger());
    landed.y = static_cast<std::int32_t>(reply.value(QStringLiteral("y")).toInteger());
    landed.width = static_cast<std::uint32_t>(reply.value(QStringLiteral("width")).toInteger());
    landed.height = static_cast<std::uint32_t>(reply.value(QStringLiteral("height")).toInteger());
    if (landed.width > 0 && landed.height > 0) {
        applyPinRect(landed);
    }
}

// The daemon confirmed where it placed the pin.  Marks are anchored to
// marksOrigin_ (the previous confirmed position); translate them by the delta
// from there to the new confirmed position, then update both marksOrigin_ and
// selection_ so the next confirmation is computed correctly.
void OverlayController::applyPinRect(const LogicalRect &rect)
{
    if (!marksOrigin_.has_value()) {
        return;
    }
    const LogicalRect previous = *marksOrigin_;
    const std::int32_t dx = rect.x - previous.x;
    const std::int32_t dy = rect.y - previous.y;
    marksOrigin_ = LogicalRect{rect.x, rect.y, previous.width, previous.height};
    // Keep the session's own record of where the image is in step with the
    // confirmation.  Region capture pins that rect to the output, but here it
    // describes the image, and it is what the mosaic samples its blocks through
    // -- left behind, a mosaic drawn on a dragged pin would average the wrong
    // part of the picture.
    for (OutputSession &output : session_.outputs) {
        output.geometry.x = rect.x;
        output.geometry.y = rect.y;
    }
    // Keep the optimistic selection in step with the latest confirmed position
    // so that the next drag's incremental delta is computed from here.
    selection_ = *marksOrigin_;
    if (dx == 0 && dy == 0) {
        return;
    }
    translateAnnotations(dx, dy);
    // Only the image's old and new rects can hold pixels that changed: marks are
    // clipped to the image, so the union of the two covers every one of them.
    // A full repaint here was a whole output's worth of work on every motion
    // event's confirmation.
    invalidateLogicalRegion(uniteLogical(previous, *marksOrigin_));
    // The input region follows the image, or the editor would take clicks on
    // the band it just left and pass through clicks on the band it just took.
    scheduleInputMask();
    if (toolbar_ != nullptr && toolbar_->isVisible()) {
        updateToolbarGeometry();
    }
}

void OverlayController::chooseTool(std::optional<Tool> tool)
{
    // Any tool change invalidates the recognized layer: the marks are about to
    // be drawn over the text, and the pointer is no longer selecting it.
    leaveTextMode();
    if (finished_ || cancelled_) {
        return;
    }
    if (textEdit_ != nullptr) {
        finishText(true);
    }
    if (gesture_->type == Gesture::Type::Bezier && tool != Tool::Bezier) {
        // An unfinished pen path goes with the tool: it is one gesture rather
        // than a drawing that outlives a tool change, and leaving it in the
        // gesture would have the next press extend a path nobody is looking at
        // any more.
        gesture_->type = Gesture::Type::None;
        gesture_->points.clear();
    }
    if (tool == Tool::Picker && tool_ != Tool::Picker) {
        // The colour a pick takes has to land somewhere visible, and the picker
        // has no style of its own to hold it: it goes to the tool that was
        // armed, which is the one the user is about to draw with.  With nothing
        // armed there is no such tool -- a session starts that way, and an
        // unarmed session is what the old Select tool became -- so the pick goes
        // to the pen instead, whose colour is the session's own ink.
        pickerReturnTool_ = tool_.has_value() ? *tool_ : Tool::Pen;
    }
    tool_ = tool;
    // The mark selection belongs to the state the marks are adjusted in, and
    // that state is the one with nothing armed: a mark stays picked up while
    // the user changes the colour or the width, and arming a drawing tool lets
    // it go -- the white outline and its handles would otherwise sit under the
    // ink about to be laid down, and be read as part of it.
    if (tool.has_value()) {
        selectedAnnotation_ = -1;
    }
    for (CaptureOverlay *overlay : overlays_) {
        overlay->setCursor(Qt::CrossCursor);
    }
    updateAll();
}

void OverlayController::toggleTool(Tool tool)
{
    // The button of the tool already armed disarms it, which is how the user
    // gets back to the state a fresh capture starts in: nothing drawing, the
    // selection and the marks the only things a press can act on.
    chooseTool(tool_.has_value() && *tool_ == tool ? std::nullopt
                                                   : std::optional<Tool>(tool));
}

void OverlayController::setCurrentColor(const QColor &color)
{
    if (finished_ || cancelled_ || !color.isValid()) {
        return;
    }
    toolStyle(styleTargetTool()).color = color;
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
        // The height comes from the size box's current value, which is also
        // what setTextSize leaves in textEditPixels_ -- the two restyle paths
        // have to agree or a font change would revert a size change.
        QFont editorFont =
            textFont(textEditFont_, std::max(1, static_cast<int>(textEditPixels_)));
        textEdit_->setFont(editorFont);
        textEdit_->setFixedHeight(std::max(20, QFontMetrics(editorFont).height() + 6));
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
    const QString target = styleTargetTool();
    const std::uint32_t clamped = std::clamp(width, 1u, 64u);
    toolStyle(target).width = clamped;
    // A numbered badge is a text annotation by wire, but its diameter is a value
    // of its own and this control never reaches it: the badges already on the
    // canvas keep the size they were placed with.  A label's size is its own box
    // for the same reason.
    if (target == QStringLiteral("number")) {
        updateAll();
        return;
    }
    applyStyleToSelected([&clamped](Annotation &annotation) {
        if (annotation.kind != Annotation::Kind::Text) {
            annotation.width = clamped;
        }
    });
    updateAll();
}

void OverlayController::setNumberSize(std::uint32_t size)
{
    if (finished_ || cancelled_) {
        return;
    }
    const std::uint32_t clamped =
        std::clamp(size, static_cast<std::uint32_t>(kNumberMinDiameter),
                   static_cast<std::uint32_t>(kNumberMaxDiameter));
    toolStyle(QStringLiteral("number")).numberSize = clamped;
    applyStyleToSelected([&clamped](Annotation &annotation) {
        if (isNumberAnnotation(annotation)) {
            const Point center = numberCenter(annotation);
            annotation.numberSize = clamped;
            // The box travels with the diameter, or a placed badge would keep
            // the size it was placed at while the slider moved.
            layoutNumberBox(annotation, center);
        }
    });
    updateAll();
}

void OverlayController::setWaveAmplitude(std::uint32_t amplitude)
{
    if (finished_ || cancelled_) {
        return;
    }
    const std::uint32_t clamped = std::clamp(amplitude, 1u, 64u);
    toolStyle(QStringLiteral("wave")).amplitude = clamped;
    applyStyleToSelected([&clamped](Annotation &annotation) {
        if (annotation.tool == QStringLiteral("wave")) {
            annotation.amplitude = clamped;
        }
    });
    updateAll();
}

void OverlayController::setWaveWavelength(std::uint32_t wavelength)
{
    if (finished_ || cancelled_) {
        return;
    }
    const std::uint32_t clamped = std::clamp(wavelength, 6u, 256u);
    toolStyle(QStringLiteral("wave")).wavelength = clamped;
    applyStyleToSelected([&clamped](Annotation &annotation) {
        if (annotation.tool == QStringLiteral("wave")) {
            annotation.wavelength = clamped;
        }
    });
    updateAll();
}

void OverlayController::setFill(const QString &fill)
{
    if (finished_ || cancelled_) {
        return;
    }
    currentFill_ = fill == QStringLiteral("stroke") || fill == QStringLiteral("fill")
        ? fill
        : QStringLiteral("both");
    applyStyleToSelected([this](Annotation &annotation) {
        if (annotation.tool == QStringLiteral("bezier")) {
            annotation.fill = currentFill_;
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
    textSize_ = static_cast<std::uint32_t>(clampTextPixels(static_cast<int>(size)));
    // Restyle an open editor live, the way a font change does: the height is
    // what the size box is for, and seeing it change while typing is the point.
    if (textEdit_ != nullptr) {
        textEditPixels_ = textSize_;
        QFont editorFont = textFont(textEditFont_, std::max(1, static_cast<int>(textEditPixels_)));
        textEdit_->setFont(editorFont);
        // Grow the box with the glyphs so a bigger size is not clipped.
        const int height = std::max(20, QFontMetrics(editorFont).height() + 6);
        textEdit_->setFixedHeight(height);
    }
    applyStyleToSelected([this](Annotation &annotation) {
        // A badge's diameter has a size control of its own; letting the label
        // size box write over it would decouple the bitmap's density from the
        // scale the protocol derives.
        if (annotation.kind == Annotation::Kind::Text && !isNumberAnnotation(annotation)) {
            annotation.textPixels = textSize_;
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

void OverlayController::setNumberStyle(NumberStyle style)
{
    if (finished_ || cancelled_) {
        return;
    }
    numberStyle_ = style;
    applyStyleToSelected([style](Annotation &annotation) {
        if (isNumberAnnotation(annotation)) {
            // The box is the same square for all four looks, so only the style
            // travels: no re-layout, no change to where the badge sits.
            annotation.numberStyle = style;
        }
    });
    updateAll();
}

void OverlayController::notifyPanelDragged()
{
    panelPinned_ = true;
    scheduleInputMask();
}

bool OverlayController::pasteImage(const QImage &image, const QString &source)
{
    Q_UNUSED(source);
    if (finished_ || cancelled_ || image.isNull() || !editing_ || !selection_.has_value()) {
        return false;
    }
    const LogicalRect &canvas = *selection_;
    if (canvas.width == 0 || canvas.height == 0) {
        return false;
    }
    // The image is placed at its own pixel size, shrunk to fit the canvas when
    // it is larger -- an oversized paste would land with its edges already
    // outside the crop, which reads as a bug rather than as a placement. Small
    // images stay their own size: blowing them up to fill the canvas would
    // blur them and is not what "paste this here" means.
    double fit = 1.0;
    if (image.width() > 0 && image.height() > 0) {
        fit = std::min(1.0, std::min(static_cast<double>(canvas.width) / image.width(),
                                      static_cast<double>(canvas.height) / image.height()));
    }
    const int width = std::max(1, static_cast<int>(std::lround(image.width() * fit)));
    const int height = std::max(1, static_cast<int>(std::lround(image.height() * fit)));

    Annotation annotation;
    annotation.kind = Annotation::Kind::Image;
    annotation.tool = QStringLiteral("image");
    annotation.pixels = image;
    annotation.rect = LogicalRect{
        static_cast<std::int32_t>(canvas.x + (static_cast<std::int64_t>(canvas.width) - width) / 2),
        static_cast<std::int32_t>(canvas.y +
                                  (static_cast<std::int64_t>(canvas.height) - height) / 2),
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
    };
    // The paste replaces whatever was selected: leaving the old selection on
    // would make the handles resize the previous mark while the new image sits
    // there looking like the thing that is selected.
    QVector<Annotation> next = annotations_;
    next.push_back(annotation);
    mutateAnnotations(next);
    // Selected, so the handles are up and the image can be moved or resized
    // without a trip through the toolbar, and nothing armed so the next drag
    // adjusts it instead of inking over it.
    chooseTool(std::nullopt);
    selectAnnotation(next.size() - 1);
    return true;
}

bool OverlayController::canPaste() const
{
    return !finished_ && !cancelled_ && editing_ && selection_.has_value();
}

// The `vshot` binary this helper drives: a sibling of this process, because
// both live in the same directory once installed.  `VSHOT_BIN` names it
// outright, which is also the hook a check uses to point at a stub script.
QString vshotProgram()
{
    const QString override = QString::fromLocal8Bit(qgetenv("VSHOT_BIN"));
    if (!override.isEmpty()) {
        return override;
    }
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return QString();
    }
    buffer[length] = '\0';
    const QString helper = QString::fromLocal8Bit(buffer);
    // `vshot` is the same binary with the helper's directory walked back one
    // level: installed layouts put both in /usr/bin, and a source checkout has
    // `build-qt/vshot-qt-ui` beside `target/release/vshot`.
    QString program = QFileInfo(helper).absolutePath() + QStringLiteral("/vshot");
    if (!QFileInfo::exists(program)) {
        const QString beside =
            QFileInfo(helper).absolutePath() + QStringLiteral("/../target/release/vshot");
        if (QFileInfo::exists(beside)) {
            program = QDir::cleanPath(beside);
        } else {
            program = QStringLiteral("vshot");
        }
    }
    return program;
}

// How long a recognition run is given: a model load takes a moment on the first
// run, the recognition itself is a fraction of a second, and a hang still has
// to end.
constexpr int kTextProcessTimeoutMs = 60 * 1000;
// The translation reaches a provider over the network, so it gets the same
// generous deadline rather than the clipboard's short one.
constexpr int kTranslateProcessTimeoutMs = 60 * 1000;

// What one completed `vshot` run left behind.  A run that never started and one
// killed at the deadline are told apart by the two flags, which is what lets
// each caller name the failure it saw.
struct ProcessRun {
    bool started = false;
    bool finished = false;
    int exitCode = -1;
    QByteArray output;
    QByteArray errorOutput;
};

// One `vshot` run.  A null `input` gives the child no standard input at all --
// what the recognition step wants -- while a non-null one, even empty, is piped
// in, which is how the translated envelope reaches the translation step.
ProcessRun runProcess(const QString &program, const QStringList &arguments, const QByteArray &input,
                      int timeoutMs)
{
    ProcessRun result;
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    if (input.isNull()) {
        process.setStandardInputFile(QProcess::nullDevice());
    }
    process.start();
    if (!process.waitForStarted(kClipboardProcessTimeoutMs)) {
        return result;
    }
    result.started = true;
    if (!input.isNull()) {
        process.write(input);
        process.closeWriteChannel();
    }
    result.finished = process.waitForFinished(timeoutMs);
    if (!result.finished) {
        process.kill();
        process.waitForFinished(kClipboardProcessTimeoutMs);
    }
    result.output = process.readAllStandardOutput();
    result.errorOutput = process.readAllStandardError();
    result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    return result;
}

bool OverlayController::beginTextSelection(QString *error)
{
    // Every way this can fail is reported both to the caller and to the button
    // the user pressed: the copy can be triggered by a key the toolbar never
    // sees, so the label cannot be driven from the click handler alone.
    const auto fail = [this, error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        if (textResultCallback_) {
            textResultCallback_(TextOutcome::Failed, message);
        }
        return false;
    };
    if (!canPaste()) {
        return fail(uiTr("Reading text needs a selection to read from."));
    }
    const LogicalRect &canvas = *selection_;
    if (canvas.isEmpty()) {
        return fail(uiTr("The selection is empty."));
    }
    // The pixels come from the output the selection sits on, at that output's
    // own scale -- the same source the mosaic preview reads, so what is
    // recognized is what the user sees under the rectangle.
    const int index = outputContaining(canvas);
    if (index < 0 || index >= session_.outputs.size()) {
        return fail(uiTr("The selection is on no output."));
    }
    const OutputSession &output = session_.outputs.at(index);
    if (output.image.isNull()) {
        return fail(uiTr("The captured frame is not available."));
    }
    // The rect is clipped to the output: a selection dragged past the edge of
    // its screen has no pixels beyond it to read.
    const QRect source = sourceRect(output, canvas)
                             .intersected(QRect(0, 0, output.image.width(), output.image.height()));
    if (source.isEmpty()) {
        return fail(uiTr("The selection has no pixels on this output."));
    }
    const QImage pixels = output.image.copy(source);
    if (pixels.isNull()) {
        return fail(uiTr("The selection has no pixels on this output."));
    }

    QTemporaryDir directory;
    if (!directory.isValid()) {
        return fail(uiTr("Cannot create a temporary directory for the text."));
    }
    const QString path = directory.filePath(QStringLiteral("selection.png"));
    if (!pixels.save(path, "PNG")) {
        return fail(uiTr("Cannot write the selection to read its text."));
    }

    // The engine lives in `vshot`, which is a sibling of this helper; the
    // translation path finds it the same way, and `VSHOT_BIN` overrides both.
    const QString program = vshotProgram();
    if (program.isEmpty()) {
        return fail(uiTr("Cannot locate vshot to read the text."));
    }

    // The engine takes a moment -- a model load on the first run -- and the
    // wait below runs on the GUI thread, so the button says what it is waiting
    // for, and the label is given its paint before the wait begins.  Input is
    // held back for that paint: a click landing mid-recognition would reach a
    // mode that is not up yet.
    if (textResultCallback_) {
        textResultCallback_(TextOutcome::Busy, QString());
    }
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    // `--json` carries the position of every character, which is what the text
    // mode draws and selects with; without it the engine prints only the text.
    const ProcessRun run = runProcess(
        program,
        {QStringLiteral("ocr"), QStringLiteral("--input"), path, QStringLiteral("--json")},
        QByteArray(), kTextProcessTimeoutMs);
    if (!run.started) {
        return fail(uiTr("Cannot start vshot to read the text."));
    }
    // A model load takes a moment on the first run and the recognition itself
    // is a fraction of a second, so the wait is generous compared to the
    // clipboard's; a hang still has to end, hence the deadline.
    if (!run.finished) {
        return fail(uiTr("Reading the text took too long."));
    }
    if (run.exitCode != 0) {
        const QString stderr = QString::fromUtf8(run.errorOutput).trimmed();
        return fail(stderr.isEmpty() ? uiTr("Reading the text failed.") : stderr);
    }
    const QByteArray document = run.output;
    // The recognition came back: the mode starts, or -- for an engine that
    // reported no positions -- the whole text is copied here and now.
    if (!enterTextSelection(document, error)) {
        return fail(error != nullptr ? *error : QString());
    }
    if (textResultCallback_) {
        // The mode being up means nothing has been copied yet -- the button
        // waits for the gesture that picks a range.  The no-positions fallback
        // is the other way to get here, and that one did copy.
        textResultCallback_(textMode() ? TextOutcome::Idle : TextOutcome::Copied, QString());
    }
    return true;
}

bool OverlayController::enterTextSelection(const QByteArray &document, QString *error)
{
    if (!canPaste()) {
        if (error != nullptr) {
            *error = uiTr("Reading text needs a selection to read from.");
        }
        return false;
    }
    const LogicalRect &canvas = *selection_;
    if (canvas.isEmpty()) {
        if (error != nullptr) {
            *error = uiTr("The selection is empty.");
        }
        return false;
    }
    const int index = outputContaining(canvas);
    if (index < 0 || index >= session_.outputs.size()) {
        if (error != nullptr) {
            *error = uiTr("The selection is on no output.");
        }
        return false;
    }
    const OutputSession &output = session_.outputs.at(index);
    if (output.image.isNull()) {
        if (error != nullptr) {
            *error = uiTr("The captured frame is not available.");
        }
        return false;
    }
    // The same crop `beginTextSelection` recognized: the engine's coordinates
    // are counted from it, so its top-left in the overlay's own logical pixels
    // is where the layer is placed.
    const QRect source = sourceRect(output, canvas)
                             .intersected(QRect(0, 0, output.image.width(), output.image.height()));
    if (source.isEmpty()) {
        if (error != nullptr) {
            *error = uiTr("The selection has no pixels on this output.");
        }
        return false;
    }

    const LogicalRect sourceLogical = logicalFromSource(output, source);
    TextLayerPlacement placement;
    placement.scale = static_cast<double>(output.scale);
    placement.originX = sourceLogical.x;
    placement.originY = sourceLogical.y;

    QString parseError;
    std::optional<TextLayer> layer = TextLayer::fromJson(document, placement, &parseError);
    if (!layer.has_value()) {
        if (error != nullptr) {
            *error = uiTr("Reading the text failed.");
        }
        // The parser's own message says what was actually wrong with the
        // document, which is a diagnostic rather than something to show.
        std::fprintf(stderr, "vshot-qt-ui: %s\n", parseError.toUtf8().constData());
        std::fflush(stderr);
        return false;
    }
    if (layer->hasGeometry()) {
        textLayer_ = std::move(layer);
        textMode_ = true;
        textDragging_ = false;
        // The whole layer starts selected.  The user asked for the text and
        // usually wants all of it, so the copy is one key or one more click
        // away; a drag narrows the range from there.  A layer whose lines
        // carry no characters at all is left with nothing selected, which is
        // the one case where the mode still starts empty.
        selectTextRange(0, textLayer_->count() - 1);
        // A label being typed would take the keys the mode now needs.
        if (textEdit_ != nullptr) {
            finishText(false);
        }
        updateTextModeCursor();
        updateAll();
        return true;
    }
    // An external engine reports text and no positions, so there is nothing to
    // select: the whole text is handed over the way this used to be the only
    // thing it did.
    if (layer->plainText().isEmpty()) {
        if (error != nullptr) {
            *error = uiTr("No text was found in the selection.");
        }
        return false;
    }
    std::fprintf(stderr, "vshot-qt-ui: the engine reported no character positions; "
                         "copying the whole text instead\n");
    std::fflush(stderr);
    if (!writeClipboard(layer->plainText())) {
        if (error != nullptr) {
            *error = uiTr("Cannot copy the text to the clipboard.");
        }
        return false;
    }
    return true;
}

void OverlayController::leaveTextMode()
{
    if (!textMode_) {
        return;
    }
    textMode_ = false;
    textLayer_.reset();
    textAnchor_ = -1;
    textFocus_ = -1;
    textDragging_ = false;
    // A triple-click run cannot outlive the mode: the next entry starts a
    // fresh one.
    textClickClock_.invalidate();
    updateTextModeCursor();
    updateAll();
}

QString OverlayController::selectedText() const
{
    if (!textMode_ || !textLayer_.has_value() || textAnchor_ < 0 || textFocus_ < 0) {
        return QString();
    }
    return textLayer_->rangeText(textAnchor_, textFocus_);
}

void OverlayController::setTextResultCallback(
    std::function<void(TextOutcome, const QString &)> callback)
{
    textResultCallback_ = std::move(callback);
}

void OverlayController::setClipboardWriter(std::function<bool(const QString &)> writer)
{
    clipboardWriter_ = std::move(writer);
}

bool OverlayController::writeClipboard(const QString &text)
{
    if (clipboardWriter_) {
        return clipboardWriter_(text);
    }
    return runWlCopy(text);
}

void OverlayController::setTranslateResultCallback(
    std::function<void(TextOutcome, const QString &)> callback)
{
    translateResultCallback_ = std::move(callback);
}

bool OverlayController::translateSelection(QString *error)
{
    // Every way this can fail is reported to the button the user pressed; the
    // translation itself can also be started from the overlay's own drag or
    // Enter, and the button is the only place the user is looking either way.
    const auto fail = [this, error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        if (translateResultCallback_) {
            translateResultCallback_(TextOutcome::Failed, message);
        }
        return false;
    };
    if (finished_ || cancelled_) {
        return fail(uiTr("Translation needs a selection to read from."));
    }
    QVector<TranslatedLine> lines;
    QString text;
    if (!computeTranslation(&lines, &text, error)) {
        return fail(error != nullptr ? *error : QString());
    }
    Annotation annotation;
    annotation.kind = Annotation::Kind::Translation;
    annotation.tool = QStringLiteral("translate");
    annotation.font = lines.constFirst().family;
    annotation.translation = lines;
    LogicalRect box = lines.constFirst().fill;
    for (const TranslatedLine &line : lines) {
        const std::int64_t left = std::min<std::int64_t>(box.x, line.fill.x);
        const std::int64_t top = std::min<std::int64_t>(box.y, line.fill.y);
        const std::int64_t right = std::max<std::int64_t>(box.right(), line.fill.right());
        const std::int64_t bottom = std::max<std::int64_t>(box.bottom(), line.fill.bottom());
        box = LogicalRect{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                          static_cast<std::uint32_t>(right - left),
                          static_cast<std::uint32_t>(bottom - top)};
    }
    annotation.rect = box;
    // One annotation, so the whole translation undoes in one step and goes out
    // through the renderer as a single mark.
    QVector<Annotation> next = annotations_;
    next.push_back(annotation);
    mutateAnnotations(std::move(next));
    if (translateResultCallback_) {
        translateResultCallback_(TextOutcome::Idle, QString());
    }
    return true;
}

bool OverlayController::computeTranslation(QVector<TranslatedLine> *lines, QString *text,
                                           QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    // The frame the translation reads: the selection, or -- for the editor's
    // button, which a session could in principle reach with none -- the whole
    // captured image of the first output.
    LogicalRect canvas;
    int index = -1;
    if (selection_.has_value() && !selection_->isEmpty()) {
        canvas = *selection_;
        index = outputContaining(canvas);
    } else if (!session_.outputs.isEmpty()) {
        index = 0;
        canvas = session_.outputs.at(0).geometry;
    }
    if (index < 0 || index >= session_.outputs.size()) {
        return fail(uiTr("Translation needs a selection to read from."));
    }
    const OutputSession &output = session_.outputs.at(index);
    if (output.image.isNull()) {
        return fail(uiTr("The captured frame is not available."));
    }
    const QRect source = sourceRect(output, canvas)
                             .intersected(QRect(0, 0, output.image.width(), output.image.height()));
    if (source.isEmpty()) {
        return fail(uiTr("The selection has no pixels on this output."));
    }
    const QImage pixels = output.image.copy(source);
    if (pixels.isNull()) {
        return fail(uiTr("The selection has no pixels on this output."));
    }

    // The two subprocesses take a while -- a model load, then a network round
    // trip -- and both run on the GUI thread, so the button says so first and
    // is given its paint before the wait starts.
    if (translateResultCallback_) {
        translateResultCallback_(TextOutcome::Busy, QString());
    }
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

    QByteArray document;
    if (!runTranslationPipeline(pixels, &document, error)) {
        return false;
    }

    // The engine's coordinates are counted from the crop, so its top-left in
    // the overlay's own logical pixels is where the layer is placed -- the same
    // placement the text-selection mode uses.
    const LogicalRect sourceLogical = logicalFromSource(output, source);
    TextLayerPlacement placement;
    placement.scale = static_cast<double>(output.scale);
    placement.originX = sourceLogical.x;
    placement.originY = sourceLogical.y;
    QString parseError;
    std::optional<TextLayer> layer = TextLayer::fromJson(document, placement, &parseError);
    if (!layer.has_value()) {
        // The parser's own message says what was wrong with the document, which
        // is a diagnostic rather than something to show the user.
        std::fprintf(stderr, "vshot-qt-ui: %s\n", parseError.toUtf8().constData());
        std::fflush(stderr);
        return fail(uiTr("Translating the text failed."));
    }
    if (!layer->hasGeometry() || layer->lineCount() == 0) {
        return fail(uiTr("The translation has no positions to draw."));
    }
    const QString plain = layer->plainText();
    // The editor's own font is the starting point, the same one the text tool
    // draws a label with; `placedTranslations` then swaps in a family that can
    // actually draw each line's script rather than boxing every CJK glyph.
    //
    // The fills grow within `canvas`, the crop that becomes the image: a box
    // against the selection's right edge is pulled back rather than spilling
    // onto pixels the saved picture will not contain, so the preview the user
    // sees and the PNG that is written hold the same text in the same place.
    QVector<TranslatedLine> placed =
        placedTranslations(*layer, output.image, output.geometry,
                           static_cast<double>(output.scale), canvas, currentFont_);
    if (placed.isEmpty()) {
        return fail(uiTr("No text was found in the selection."));
    }
    *lines = std::move(placed);
    *text = plain;
    return true;
}

bool OverlayController::runTranslationPipeline(const QImage &pixels, QByteArray *document,
                                               QString *error) const
{
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    const QString program = vshotProgram();
    if (program.isEmpty()) {
        return fail(uiTr("Cannot locate vshot to translate the text."));
    }
    QTemporaryDir directory;
    if (!directory.isValid()) {
        return fail(uiTr("Cannot create a temporary directory for the text."));
    }
    const QString path = directory.filePath(QStringLiteral("selection.png"));
    if (!pixels.save(path, "PNG")) {
        return fail(uiTr("Cannot write the selection to read its text."));
    }
    const ProcessRun recognition = runProcess(
        program,
        {QStringLiteral("ocr"), QStringLiteral("--input"), path, QStringLiteral("--json")},
        QByteArray(), kTextProcessTimeoutMs);
    if (!recognition.started) {
        return fail(uiTr("Cannot start vshot to read the text."));
    }
    if (!recognition.finished) {
        return fail(uiTr("Reading the text took too long."));
    }
    if (recognition.exitCode != 0) {
        const QString stderr = QString::fromUtf8(recognition.errorOutput).trimmed();
        return fail(stderr.isEmpty() ? uiTr("Reading the text failed.") : stderr);
    }

    // The recognized envelope is piped straight into the translation step, and
    // the session's own choices are added only when it made them: an absent one
    // is the CLI's config default, which is what asking for nothing means.
    QStringList arguments{QStringLiteral("translate"), QStringLiteral("--stdin-ocr"),
                          QStringLiteral("--json")};
    if (session_.translate.has_value()) {
        const TranslateOptions &options = *session_.translate;
        if (!options.to.isEmpty()) {
            arguments << QStringLiteral("--to") << options.to;
        }
        if (!options.from.isEmpty()) {
            arguments << QStringLiteral("--from") << options.from;
        }
        if (!options.provider.isEmpty()) {
            arguments << QStringLiteral("--provider") << options.provider;
        }
    }
    const ProcessRun translated =
        runProcess(program, arguments, recognition.output, kTranslateProcessTimeoutMs);
    if (!translated.started) {
        return fail(uiTr("Cannot start vshot to translate the text."));
    }
    if (!translated.finished) {
        return fail(uiTr("Translating the text took too long."));
    }
    if (translated.exitCode != 0) {
        const QString stderr = QString::fromUtf8(translated.errorOutput).trimmed();
        return fail(stderr.isEmpty() ? uiTr("Translating the text failed.") : stderr);
    }
    *document = translated.output;
    return true;
}

bool OverlayController::runTranslateStage(QString *error)
{
    // The standalone overlay translates the rectangle it framed -- the moment
    // the drag that drew it ends, or on Enter after an Escape took an earlier
    // translation off -- so it needs one: there is nothing to crop without it.
    // (The editor's button is the caller that falls back to the whole image.)
    if (!selection_.has_value() || selection_->isEmpty()) {
        if (error != nullptr) {
            *error = uiTr("Translation needs a selection to read from.");
        }
        return false;
    }
    QVector<TranslatedLine> lines;
    QString text;
    if (!computeTranslation(&lines, &text, error)) {
        return false;
    }
    translatedLines_ = std::move(lines);
    translatedText_ = std::move(text);
    translated_ = true;
    updateAll();
    return true;
}

bool OverlayController::acceptTranslation(QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (!translated_ || translatedLines_.isEmpty() || !selection_.has_value()) {
        return fail(uiTr("There is no translation to save."));
    }
    const int index = outputContaining(*selection_);
    if (index < 0 || index >= session_.outputs.size()) {
        return fail(uiTr("The selection is on no output."));
    }
    const OutputSession &output = session_.outputs.at(index);
    if (resultPath_.isEmpty()) {
        return fail(uiTr("The session names no file to write the translation to."));
    }
    if (output.image.isNull()) {
        return fail(uiTr("The captured frame is not available."));
    }
    const QRect source = sourceRect(output, *selection_)
                             .intersected(QRect(0, 0, output.image.width(), output.image.height()));
    if (source.isEmpty()) {
        return fail(uiTr("The selection has no pixels on this output."));
    }
    // The frozen scene the user framed, with the translation composited over it
    // through the same painter the preview uses, so the file matches the screen.
    QImage composite =
        output.image.copy(source).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const double scale = outputScale(output);
    {
        QPainter painter(&composite);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        paintTranslation(
            painter, translatedLines_,
            [&](const TranslatedLine &line) {
                const qreal x =
                    (static_cast<qreal>(line.fill.x) - output.geometry.x) * scale - source.x();
                const qreal y =
                    (static_cast<qreal>(line.fill.y) - output.geometry.y) * scale - source.y();
                return QRectF(x, y, line.fill.width * scale, line.fill.height * scale);
            },
            scale);
    }
    if (!composite.save(resultPath_, "PNG")) {
        return fail(uiTr("Cannot write the translated image."));
    }
    resultImagePath_ = resultPath_;
    return true;
}

void OverlayController::selectTextRange(int anchor, int focus)
{
    if (!textLayer_.has_value() || textLayer_->count() == 0) {
        textAnchor_ = -1;
        textFocus_ = -1;
        return;
    }
    const int last = textLayer_->count() - 1;
    textAnchor_ = std::clamp(anchor, 0, last);
    textFocus_ = std::clamp(focus, 0, last);
}

void OverlayController::textSelectAll()
{
    if (!textLayer_.has_value() || textLayer_->count() == 0) {
        return;
    }
    selectTextRange(0, textLayer_->count() - 1);
    updateAll();
}

void OverlayController::textSelectWord(int index)
{
    if (!textLayer_.has_value() || index < 0) {
        return;
    }
    int first = -1;
    int last = -1;
    textLayer_->wordRange(index, &first, &last);
    if (first < 0) {
        return;
    }
    selectTextRange(first, last);
    updateAll();
}

void OverlayController::textSelectLine(int index)
{
    if (!textLayer_.has_value() || index < 0) {
        return;
    }
    int first = -1;
    int last = -1;
    textLayer_->lineRange(index, &first, &last);
    if (first < 0) {
        return;
    }
    selectTextRange(first, last);
    updateAll();
}

void OverlayController::copyTextSelection()
{
    if (!textMode_ || !textLayer_.has_value()) {
        return;
    }
    const QString text = selectedText();
    if (text.isEmpty()) {
        // Nothing is selected, so there is nothing to copy and the mode stays
        // up for the user to try again.
        if (textResultCallback_) {
            textResultCallback_(TextOutcome::Failed, uiTr("No text is selected."));
        }
        return;
    }
    if (!writeClipboard(text)) {
        // The selection is not lost: the copy can be tried again.
        if (textResultCallback_) {
            textResultCallback_(TextOutcome::Failed, uiTr("Cannot copy the text to the clipboard."));
        }
        return;
    }
    if (textResultCallback_) {
        textResultCallback_(TextOutcome::Copied, QString());
    }
    leaveTextMode();
}

void OverlayController::updateTextModeCursor()
{
    const Qt::CursorShape shape = textMode_ ? Qt::IBeamCursor : Qt::CrossCursor;
    for (CaptureOverlay *overlay : overlays_) {
        overlay->setCursor(shape);
    }
}

bool OverlayController::pasteFromFile(QString *error)
{
    if (!canPaste()) {
        if (error != nullptr) {
            *error = uiTr("Paste needs a selection to paste onto.");
        }
        return false;
    }
    // The helper is this program.  It runs the dialog in a process of its own
    // because that process has to be free of this one's event loop, not because
    // the dialog is a different kind of window: it is a layer surface like this
    // overlay, mapped after it, which is what puts it above the frozen frame
    // instead of behind it.
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        if (error != nullptr) {
            *error = uiTr("Cannot locate the vshot helper to open the file dialog.");
        }
        return false;
    }
    buffer[length] = '\0';
    const QString helper = QString::fromLocal8Bit(buffer);
    // The dialog opens on the output the user is annotating, so it lands in
    // front of them rather than on whichever screen the compositor favours.
    const int outputIndex = outputIndexForSelection();
    const QString outputName = outputIndex >= 0 && outputIndex < session_.outputs.size()
        ? session_.outputs.at(outputIndex).name
        : QString();
    // The controller is not a QObject, so the watcher is parented to the
    // application: it has to outlive this call, and the overlay's own widgets
    // can be torn down while the dialog is still up.
    auto *dialog = new QProcess(qApp);
    dialog->setProgram(helper);
    dialog->setArguments({QStringLiteral("--open-dialog"), QString(), outputName});
    dialog->setStandardInputFile(QProcess::nullDevice());
    QObject::connect(dialog, &QProcess::finished, dialog,
                     [this, dialog](int code, QProcess::ExitStatus) {
                         const QByteArray out = dialog->readAllStandardOutput();
                         dialog->deleteLater();
                         QJsonParseError parseError;
                         const QJsonDocument document =
                             QJsonDocument::fromJson(out.trimmed(), &parseError);
                         if (code != 0 || parseError.error != QJsonParseError::NoError
                             || !document.isObject()
                             || !document.object().value(QStringLiteral("ok")).toBool()) {
                             return; // cancelled
                         }
                         const QString path =
                             document.object().value(QStringLiteral("path")).toString();
                         if (path.isEmpty()) {
                             return;
                         }
                         QImageReader reader(path);
                         const QImage image = reader.read();
                         if (image.isNull()) {
                             std::fprintf(stderr, "vshot-qt-ui: cannot read image `%s`\n",
                                          qPrintable(path));
                             std::fflush(stderr);
                             return;
                         }
                         // The session may have been confirmed or cancelled
                         // while the dialog was up; pasteImage checks that
                         // itself and simply does nothing then.
                         pasteImage(image, path);
                     });
    QObject::connect(dialog, &QProcess::errorOccurred, dialog,
                     [dialog](QProcess::ProcessError failure) {
                         if (failure != QProcess::FailedToStart) {
                             return;
                         }
                         std::fprintf(stderr,
                                      "vshot-qt-ui: could not start the file dialog\n");
                         std::fflush(stderr);
                         dialog->deleteLater();
                     });
    dialog->start();
    return true;
}

bool OverlayController::pasteFromClipboard(QString *error)
{
    if (!canPaste()) {
        if (error != nullptr) {
            *error = uiTr("Paste needs a selection to paste onto.");
        }
        return false;
    }
    const ClipboardImage clipboard = readClipboardImage();
    if (clipboard.installed == false) {
        if (error != nullptr) {
            *error = uiTr("`wl-paste` was not found, so the clipboard cannot be read.");
        }
        return false;
    }
    if (!clipboard.image.isNull()) {
        return pasteImage(clipboard.image, clipboard.source);
    }
    if (error != nullptr) {
        // Naming what was actually wrong is the difference between "the
        // shortcut does nothing" and a user knowing to copy an image instead.
        *error = clipboard.offered
                     ? uiTr("The clipboard holds no image.")
                     : uiTr("The clipboard is empty.");
    }
    return false;
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
        if (isNumberAnnotation(annotation)) {
            // A selected badge restyles as a badge, not as a label: the style
            // row offers the four badge looks rather than the font picker.
            return QStringLiteral("number");
        }
        if (annotation.kind == Annotation::Kind::Text) {
            return QStringLiteral("text");
        }
        return annotation.tool;
    }
    // The eyedropper has no style of its own: it reads a pixel and hands it to
    // the tool it was armed from.  Pointing the style row at that tool is what
    // makes the pick land there -- every setter on the row goes through this --
    // and it puts the colour a pick would replace on screen before the click.
    if (tool_.has_value() && *tool_ == Tool::Picker) {
        return toolName(pickerReturnTool_);
    }
    // Nothing selected and nothing armed: there is no tool to edit, and the
    // row goes away rather than showing one the user has not chosen -- the
    // empty name matches no tool's controls, which is exactly what "no tool"
    // means here.  The styles the tools carry are kept either way, so arming
    // one brings its own back.
    return tool_.has_value() ? toolName(*tool_) : QString();
}

ToolStyle &OverlayController::toolStyle(const QString &tool)
{
    // `QHash::operator[]` default-constructs, which is the right value for a
    // name the loop in the constructor somehow missed.
    return toolStyles_[tool];
}

const ToolStyle &OverlayController::toolStyle(const QString &tool) const
{
    // A const read must never insert -- a painter path asks for its style on
    // every frame -- so a name with no entry falls back to the built-in style
    // instead of allocating one.
    static const ToolStyle fallback;
    const QHash<QString, ToolStyle>::const_iterator found = toolStyles_.constFind(tool);
    return found == toolStyles_.constEnd() ? fallback : found.value();
}

void OverlayController::applyStyleToSelected(
    const std::function<void(Annotation &)> &mutate)
{
    if (allSelected_ && !annotations_.isEmpty()) {
        // Ctrl+A picked every mark up, so a style change is a change to all of
        // them.  A mark the change does not touch (a font on a stroke) is left
        // exactly as it was, so this cannot count as an edit on its own.
        QVector<Annotation> next = annotations_;
        bool changed = false;
        for (int index = 0; index < next.size(); ++index) {
            mutate(next[index]);
            if (!annotationEquals(next.at(index), annotations_.at(index))) {
                changed = true;
            }
        }
        if (!changed) {
            return;
        }
        if (styleAdjustmentActive_) {
            annotations_ = std::move(next);
            styleAdjustmentChanged_ = true;
            updateAll();
            return;
        }
        const bool everyMark = allSelected_;
        mutateAnnotations(std::move(next));
        allSelected_ = everyMark; // the style change is not a new selection
        return;
    }
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
        if (annotation.kind == Annotation::Kind::Image) {
            // A pasted image is its own box: hit anywhere inside the rect it was
            // placed at, so it can be picked up and moved like any other mark.
            // Without this arm it would fall through to the stroke walk below
            // and never match -- a pasted image has no points -- which left the
            // paste impossible to select or drag.
            if (point.x >= bounds.x && point.x < bounds.right() &&
                point.y >= bounds.y && point.y < bounds.bottom()) {
                return index;
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
        if (annotation.tool == QStringLiteral("wave") && annotation.points.size() >= 2) {
            // The ink of a wave is the sampled polyline, not the straight line
            // between its two points: a click on a crest has to reach the wave,
            // or a mark it plainly covers could not be selected.
            const Point &first = annotation.points.constFirst();
            const Point &last = annotation.points.constLast();
            const QVector<QPointF> wave =
                wavePolyline(QPointF(first.x, first.y), QPointF(last.x, last.y),
                             effectiveWaveAmplitude(annotation),
                             effectiveWaveWavelength(annotation), 1.0);
            for (int segment = 1; segment < wave.size(); ++segment) {
                const auto toPoint = [](const QPointF &value) {
                    return Point{static_cast<std::int32_t>(std::lround(value.x())),
                                 static_cast<std::int32_t>(std::lround(value.y()))};
                };
                if (distanceToSegment(point, toPoint(wave.at(segment - 1)),
                                      toPoint(wave.at(segment))) <= radius + 4.0) {
                    return index;
                }
            }
            continue;
        }
        if (annotation.tool == QStringLiteral("bezier")) {
            // A closed path is a solid mark: its fill reaches the inside, so a
            // click there selects it, exactly as it does for a badge.
            if (annotation.closed &&
                bezierPath(annotation.points, true)
                    .contains(QPointF(point.x, point.y))) {
                return index;
            }
            // Otherwise the ink is the sampled curve, not the anchors: a click
            // on a bulge the handles pulled out has to reach the path.
            const QVector<QPointF> curve = bezierPolyline(annotation.points, annotation.closed);
            const auto toPoint = [](const QPointF &value) {
                return Point{static_cast<std::int32_t>(std::lround(value.x())),
                             static_cast<std::int32_t>(std::lround(value.y()))};
            };
            if (curve.size() == 1) {
                const Point only = toPoint(curve.constFirst());
                if (distanceToSegment(point, only, only) <= radius + 4.0) {
                    return index;
                }
            }
            for (int segment = 1; segment < curve.size(); ++segment) {
                if (distanceToSegment(point, toPoint(curve.at(segment - 1)),
                                      toPoint(curve.at(segment))) <= radius + 4.0) {
                    return index;
                }
            }
            continue;
        }
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

int OverlayController::annotationBorderAt(Point point) const
{
    return selectedAnnotation_ >= 0 ? annotationBorderOf(selectedAnnotation_, point) : 0;
}

int OverlayController::markUnderPointer() const
{
    // Only where a press would actually pick the mark up, so the frame is a
    // promise the press keeps.  The rim counts on its own -- a press there
    // picks the mark up with nothing held and nothing armed -- and the body
    // counts under the pick-up modifier, which is the state this frame exists
    // to show.
    if (gesture_->type != Gesture::Type::None) {
        return -1;
    }
    const int hit = annotationHitAt(pointer_);
    if (hit < 0) {
        return -1;
    }
    if (annotationBorderOf(hit, pointer_) != 0) {
        return hit;
    }
    return pickingMarks(lastModifiers_) ? hit : -1;
}

void OverlayController::refreshMarkHover()
{
    const int hovered = markUnderPointer();
    // While the pick-up modifier is held, the mark under the pointer is the one
    // a press would take, so it is also the one the editor treats as selected:
    // its outline and handles come up, and the pointer travelling across marks
    // moves the focus with it.  Without this the modifier only ever *framed* the
    // mark -- `selectedAnnotation_` was written by the press alone -- so holding
    // it over a mark showed a frame with no handles, and holding it while moving
    // onto a mark could never make that mark active at all.
    //
    // Only the pointer landing on a mark selects one: travelling off a mark
    // leaves the last one selected, so a frame the user has taken hold of does
    // not slip away the moment the pointer grazes its edge.
    //
    // The selection is set here rather than through `selectAnnotation`, which
    // ends in `updateAll`: a full repaint on every motion event is the repaint
    // storm the rect tracking below exists to avoid, and it would also clear
    // `markHovered_`, so the very frame this is deciding would be forgotten
    // before it could be drawn.  The two marks that changed -- the one losing
    // the selection and the one taking it -- are added to the repainted region
    // instead, in the same pass that already covers the hover frame.
    const int previousSelection = selectedAnnotation_;
    const bool selectionMoves =
        pickingMarks(lastModifiers_) && hovered >= 0 && hovered != selectedAnnotation_;
    if (selectionMoves) {
        selectedAnnotation_ = hovered;
        nudgeBase_.reset();
        allSelected_ = false;
    }
    if (hovered == markHovered_ && !selectionMoves) {
        return;
    }
    const int previous = markHovered_;
    markHovered_ = hovered;
    LogicalRect touched;
    bool any = false;
    for (int index : {previous, hovered, previousSelection, selectedAnnotation_}) {
        LogicalRect bounds;
        if (index >= 0 && index < annotations_.size()
            && annotationBounds(annotations_.at(index), &bounds)) {
            const LogicalRect grown =
                growBy(bounds, annotationReach(annotations_.at(index)) + kSelectionChrome);
            touched = any ? uniteLogical(touched, grown) : grown;
            any = true;
        }
    }
    if (any) {
        updateTouch(touched);
    }
}

int OverlayController::annotationBorderOf(int index, Point point) const
{
    if (index < 0 || index >= annotations_.size()) {
        return 0;
    }
    const Annotation &annotation = annotations_.at(index);
    if (annotation.kind == Annotation::Kind::Text) {
        return 0; // text annotations move but never resize
    }
    LogicalRect bounds;
    if (!annotationBounds(annotation, &bounds)) {
        return 0;
    }
    // The mark's outline as a rectangle in the mark's own frame: the bounds a
    // handle drags, which is the same box the selection chrome draws.
    const std::int64_t left = bounds.x;
    const std::int64_t top = bounds.y;
    const std::int64_t rightEdge = bounds.right() - 1;
    const std::int64_t bottomEdge = bounds.bottom() - 1;
    const std::int64_t right = bounds.right();
    const std::int64_t bottom = bounds.bottom();
    const auto close = [](std::int64_t value, std::int64_t edge) {
        return std::abs(value - edge) <= kBorderGrab;
    };
    const bool nearLeft = close(point.x, left);
    const bool nearRight = close(point.x, rightEdge);
    const bool nearTop = close(point.y, top);
    const bool nearBottom = close(point.y, bottomEdge);
    const bool betweenX = point.x >= left && point.x < right;
    const bool betweenY = point.y >= top && point.y < bottom;
    if ((nearLeft || nearRight) && (nearTop || nearBottom)) {
        // A corner, which stretches both ways at once.  Which diagonal it is
        // depends on the two edges together, not on which side of the mark the
        // pointer came from.
        return nearLeft == nearTop ? 1 : 3;
    }
    if (nearTop && betweenX) {
        return 2; // the top edge stretches vertically
    }
    if (nearBottom && betweenX) {
        return 6;
    }
    if (nearLeft && betweenY) {
        return 8; // the left edge stretches horizontally
    }
    if (nearRight && betweenY) {
        return 4;
    }
    return 0;
}

void OverlayController::selectAnnotation(int index)
{
    selectedAnnotation_ = index >= 0 && index < annotations_.size() ? index : -1;
    // Any explicit selection ends the nudge run and the all-marks selection:
    // they are the state of the keyboard's own walk, and this is the user
    // pointing at one mark instead.
    nudgeBase_.reset();
    allSelected_ = false;
    updateAll();
}

void OverlayController::deleteSelectedAnnotation()
{
    if (allSelected_ && !annotations_.isEmpty()) {
        // Ctrl+A picked every mark up; the delete key takes all of them, which
        // is the one way to clear a canvas without picking them off one by one.
        selectAnnotation(-1);
        mutateAnnotations(QVector<Annotation>());
        return;
    }
    if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size()) {
        return;
    }
    QVector<Annotation> next = annotations_;
    next.remove(selectedAnnotation_);
    selectedAnnotation_ = -1;
    mutateAnnotations(std::move(next));
}

void OverlayController::beginAnnotationDrag(Point point, bool resize, bool preserveAspect)
{
    if (selectedAnnotation_ < 0 || selectedAnnotation_ >= annotations_.size()) {
        return;
    }
    gesture_->type = resize ? Gesture::Type::ResizingAnnotation
                            : Gesture::Type::MovingAnnotation;
    gesture_->anchor = point;
    gesture_->current = point;
    gesture_->handle = resize ? annotationHandleAt(point) : 0;
    gesture_->preserveAspect = preserveAspect;
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
        const LogicalRect newBounds = resizeSelection(originalBounds, gesture_->handle, current,
                                                      gesture_->preserveAspect);
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
    // The mark may only travel inside the area it is drawn on. In pin-edit
    // mode that is the image, which moves with the user's drags — clamping
    // against the session bounds there would yank every mark back toward the
    // image's original position, which reads as the mark vanishing.
    const LogicalRect &limits = annotationLimits();
    if (annotationBounds(original, &bounds)) {
        const std::int64_t minDx = limits.x - bounds.x;
        const std::int64_t maxDx = limits.right() - bounds.right();
        const std::int64_t minDy = limits.y - bounds.y;
        const std::int64_t maxDy = limits.bottom() - bounds.bottom();
        // A mark wider or taller than the area it sits on cannot be confined
        // to it: move it freely rather than snapping it to one edge.
        if (minDx <= maxDx) {
            clampedDx = static_cast<int>(std::clamp<std::int64_t>(dx, minDx, maxDx));
        }
        if (minDy <= maxDy) {
            clampedDy = static_cast<int>(std::clamp<std::int64_t>(dy, minDy, maxDy));
        }
    }
    Annotation result = original;
    switch (original.kind) {
    case Annotation::Kind::Shape:
        result.rect.x = static_cast<std::int32_t>(result.rect.x + clampedDx);
        result.rect.y = static_cast<std::int32_t>(result.rect.y + clampedDy);
        break;
    case Annotation::Kind::Image:
        // A pasted image moves as a whole, the same way a shape does.
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
        if (isNumberAnnotation(result)) {
            // A badge's box is its content rather than a label box derived from
            // the text, so it has to travel with the badge -- the hit test and
            // the raster bounds are read from it.
            result.rect.x = static_cast<std::int32_t>(result.rect.x + clampedDx);
            result.rect.y = static_cast<std::int32_t>(result.rect.y + clampedDy);
        }
        break;
    case Annotation::Kind::Translation:
        // Each box travels with the frame it was read from, the same way the
        // bits of a stroke do.
        result.rect.x = static_cast<std::int32_t>(result.rect.x + clampedDx);
        result.rect.y = static_cast<std::int32_t>(result.rect.y + clampedDy);
        for (TranslatedLine &line : result.translation) {
            line.source.x = static_cast<std::int32_t>(line.source.x + clampedDx);
            line.source.y = static_cast<std::int32_t>(line.source.y + clampedDy);
            line.fill.x = static_cast<std::int32_t>(line.fill.x + clampedDx);
            line.fill.y = static_cast<std::int32_t>(line.fill.y + clampedDy);
        }
        break;
    }
    return result;
}

Annotation OverlayController::scaledAnnotation(const Annotation &original,
                                               const LogicalRect &newBounds) const
{
    Annotation result = original;
    if (original.kind == Annotation::Kind::Shape || original.kind == Annotation::Kind::Image) {
        // A shape's rect *is* its geometry, and a pasted image's rect is where
        // it sits and how big it is; both scale by taking the new rect.
        result.rect = newBounds;
        return result;
    }
    if (original.kind == Annotation::Kind::Translation) {
        // A translation is not stretched: its boxes were fitted to the text
        // they replace, and scaling them would undo the fit.  It is not
        // resizable -- the handles are never offered for it -- so this is only
        // here to keep the two ends of the switch in step.
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

bool OverlayController::canRequestLongCapture() const
{
    if (!longAllowed_ || finished_ || cancelled_ || !hasValidSelection()) {
        return false;
    }
    // A scrolling capture needs the region inside a single output; the CLI
    // resolves the output from the region and refuses anything wider.
    for (const OutputSession &output : session_.outputs) {
        const LogicalRect &surface = output.surface;
        if (selection_->x >= surface.x && selection_->y >= surface.y &&
            selection_->right() <= surface.right() &&
            selection_->bottom() <= surface.bottom()) {
            return true;
        }
    }
    return false;
}

void OverlayController::requestLongCapture()
{
    if (finished_ || cancelled_ || !canRequestLongCapture()) {
        return;
    }
    longRequested_ = true;
    terminal(false);
}

void OverlayController::beginPinEdit()
{
    if (!pinEdit_ || finished_ || cancelled_) {
        return;
    }
    // The editable canvas is the whole pin image: the session bounds (the
    // pinned image) is preselected, and the surface around it stays bare so
    // the toolbar can sit beside the image like a region-capture toolbar.
    selection_ = LogicalRect{session_.bounds.x, session_.bounds.y, session_.bounds.width,
                             session_.bounds.height};
    // The marks origin starts at the same place: the image is where the
    // session placed it, and the FP16 surface is already showing it there.
    marksOrigin_ = *selection_;
    // The marks the last edit committed, put back so they can be edited again
    // rather than merely seen.  They are read against the canvas the session
    // says they were made on, which for a pin is the image's own rect: the pin
    // may have been dragged since, and a mark placed against the screen would
    // then land somewhere the user never drew it.
    if (!session_.annotations.isEmpty()) {
        const std::uint32_t ratio = static_cast<std::uint32_t>(
            std::max(1.0, outputScale(session_.outputs.value(0))));
        QString markError;
        if (!parseMarks(session_.annotations, *selection_, ratio, &markError)) {
            // The session named marks the editor cannot place.  Reporting it is
            // the whole point of parsing strictly -- an editor that opened with
            // some of the user's marks silently missing would be worse -- and
            // the pin is still shown, so the failure is a message rather than a
            // refusal to open.
            const QString message = uiTr("The pin's marks could not be restored: %1").arg(markError);
            if (textResultCallback_) {
                textResultCallback_(TextOutcome::Failed, message);
            }
            qWarning("%s", qUtf8Printable(message));
        } else {
            // Restoring the marks is the state the user left the pin in, so it
            // is where undo stops rather than a step that can be undone away.
            undoStack_.clear();
            redoStack_.clear();
        }
    }
    editing_ = true;
    toolbarOutput_ = 0;
    showToolbar();
    // The surface covers the whole output so the toolbar has room beside the
    // image; only the image, its border and the toolbar itself should take
    // pointer input, or every click on the rest of the screen would land on the
    // editor instead of on the desktop behind it.
    scheduleInputMask();
}

void OverlayController::beginPinEditText()
{
    // The same editor opened on the text rather than on the marks: the pin
    // image is the canvas and the whole of it is what recognition reads, so the
    // characters come back where they were for the pointer to select a range.
    beginPinEdit();
    // A recognition that fails reports through `textResultCallback_` -- the
    // toolbar's `Text+` button is on screen by now and says so -- and the
    // editor simply stays in the ordinary pin-editing state. There is no
    // second error path here.
    beginTextSelection(nullptr);
}

void OverlayController::beginPresetEdit()
{
    if (!session_.selection.has_value() || finished_ || cancelled_) {
        return;
    }
    // Start where a finished drag would leave a region session: the selection
    // is the one the picker resolved, so the toolbar is up and the frozen
    // frame under it is the canvas the user will annotate and save.
    selection_ = *session_.selection;
    editing_ = true;
    toolbarOutput_ = outputContaining(*selection_);
    showToolbar();
    updateAll();
}

int OverlayController::outputContaining(const LogicalRect &rect) const
{
    const Point center{
        rect.x + static_cast<std::int32_t>(rect.width / 2),
        rect.y + static_cast<std::int32_t>(rect.height / 2),
    };
    for (int index = 0; index < session_.outputs.size(); ++index) {
        const LogicalRect &geometry = session_.outputs.at(index).geometry;
        if (center.x >= geometry.x && center.x < geometry.right() && center.y >= geometry.y &&
            center.y < geometry.bottom()) {
            return index;
        }
    }
    // A selection that lands on no output at all (a torn-down one) keeps the
    // toolbar on the first surface instead of nowhere.
    return 0;
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

void OverlayController::pin()
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
    pinResult_ = true;
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
    // Nothing a session did is written back to the config: everything here --
    // the tool, the colour, the width, the font size -- is the session's own
    // working state, not a preference.  The config is the *reset* value every
    // session starts from, and it changes only where the user can see and mean
    // it: the settings window (`vshot settings`) or a hand edit.  Saving here
    // would make one capture's improvisation silently redefine the next one's
    // starting point.
    hideToolbar();
    removeTextEditor();
    // A session that rendered a capture is still showing it, and the caller
    // does not have those pixels yet.  Stopping now would take the picture off
    // the screen before the caller's own copy of it is up -- for a pin, before
    // the pin exists at all -- which the user sees as the marks blinking out.
    // So the surface stays, showing the same picture, until the handoff above
    // says the caller is done with it.
    if (!cancelled && rendersCapture()) {
        beginHandoff();
        return;
    }
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
    textEditPixels_ = 0;
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

int OverlayController::liveStrokeBakes() const
{
    return liveStrokeBakes_;
}

QJsonDocument OverlayController::resultDocument(QString *error) const
{
    QJsonObject root;
    if (cancelled_) {
        root.insert(QStringLiteral("status"), QStringLiteral("cancelled"));
        return QJsonDocument(root);
    }
    root.insert(QStringLiteral("status"), QStringLiteral("ok"));
    // The Pin button finished the session asking for the image on the screen
    // rather than on disk.  The composition happens on the CLI side, so it is
    // told here; it is never set by the pin editor, which writes back to the
    // pin it is already showing.
    if (pinResult_) {
        root.insert(QStringLiteral("pin"), true);
    }
    QJsonObject selection;
    if (selection_.has_value()) {
        selection.insert(QStringLiteral("x"), static_cast<qint64>(selection_->x));
        selection.insert(QStringLiteral("y"), static_cast<qint64>(selection_->y));
        selection.insert(QStringLiteral("width"), static_cast<qint64>(selection_->width));
        selection.insert(QStringLiteral("height"), static_cast<qint64>(selection_->height));
    }
    root.insert(QStringLiteral("selection"), selection);
    // The scrolling-capture action ends the session like a confirmation, so
    // the answer travels here rather than in `status`.
    if (longRequested_) {
        root.insert(QStringLiteral("long"), true);
    }
    // Picking reports the click position as well: it runs on a live desktop,
    // so the caller re-resolves there which window the click actually landed
    // on before it captures the frame.
    if (pickMode_) {
        QJsonObject point;
        point.insert(QStringLiteral("x"), static_cast<qint64>(pointer_.x));
        point.insert(QStringLiteral("y"), static_cast<qint64>(pointer_.y));
        root.insert(QStringLiteral("point"), point);
    }
    // The standalone translate overlay reports what it produced and where it
    // wrote it, once the user accepted.
    if (translateMode_ && !resultImagePath_.isEmpty()) {
        root.insert(QStringLiteral("translated_text"), translatedText_);
        root.insert(QStringLiteral("image_path"), resultImagePath_);
    }

    // The same marks as data, for a caller that wants to reopen this image for
    // editing rather than only keep the flattened result.  They are relative to
    // the selection, which is the canvas a re-edit hands back: the pin editor
    // gets the pristine pixels and these marks together, and draws them where
    // they were.
    //
    // Written through `writeMarkAssets`, not `marksDocument`: a pasted image is
    // its pixels, and a pin-edit session is the one caller that can put them
    // somewhere the daemon will still be able to read after this process is
    // gone.  Every other caller has nowhere to write and gets the document
    // without the marks that have no other form, exactly as before.
    root.insert(QStringLiteral("marks"), writeMarkAssets(markAssetDirectory_));
    // The rendered capture goes back over the pixel channel rather than in the
    // JSON: it is the size of the selection, not of a label.  Qt is the only
    // renderer, so these images *are* the result -- the CLI takes them instead
    // of rasterizing the marks again, which is what used to put a committed mark
    // half a pixel away from the preview it was drawn from.
    //
    // Two of them, and the order is the protocol: the marks alone first, then
    // the flattened capture.  The CLI needs the layer for the HDR half, which an
    // opaque image cannot mark, and reading them in a fixed order is what lets
    // it take them without a second header field to tell them apart.
    if (pixelChannelAvailable()) {
        QImage composite;
        QImage marks;
        QString compositeError;
        if (produceComposite(&composite, &marks, &compositeError)) {
            for (const QImage &image : {marks, composite}) {
                QString sendError;
                if (!sendPixelImage(kPixelKindResult, image, &sendError)) {
                    if (error != nullptr) {
                        *error = sendError;
                    }
                    return QJsonDocument();
                }
            }
            root.insert(QStringLiteral("composite"), true);
        } else if (!compositeError.isEmpty()) {
            if (error != nullptr) {
                *error = compositeError;
            }
            return QJsonDocument();
        }
    }
    return QJsonDocument(root);
}

void OverlayController::setTerminalCallback(std::function<void()> callback)
{
    terminalCallback_ = std::move(callback);
}

// The rasterized form of one annotation, kept beside it between repaints.
//
// Every kind draws something different, so each one says for itself what its
// pixels depend on -- a mosaic's bounds, strength and source image; a label's
// text, font and size; a stroke's points.  The paint loop only asks the shared
// question "are the cached pixels still current?" through `paint`, so it never
// branches on the kind itself.  `Annotation` holds the cache.
class AnnotationRaster {
public:
    AnnotationRaster() = default;
    AnnotationRaster(const AnnotationRaster &) = delete;
    AnnotationRaster &operator=(const AnnotationRaster &) = delete;
    virtual ~AnnotationRaster() = default;

    // Draws the annotation into the overlay, rasterizing it first only when
    // something it draws has changed since the last time.  `outputIndex` names
    // the screen being painted: a session that spans several of them paints the
    // same marks on each, and one shared raster would be thrown away and rebuilt
    // every time the paint moved to the next screen.  Each output keeps its own.
    void paint(QPainter *painter, const Annotation &annotation, const OutputSession &output,
               const QSize &size, int outputIndex)
    {
        const int pad = padding(annotation);
        // Inflate before testing emptiness: a perfectly horizontal or vertical
        // stroke has a zero-thickness bounding box, which `isEmpty` rejects
        // even though there is a line to draw.
        //
        // The rect is deliberately not clipped to the canvas: trimming it to the
        // surface would change the raster's size as a mark slid over the edge of
        // the screen, and a size change rebuilds it.  The blit is clipped by the
        // painter anyway, so the pixels the user sees do not change.
        const QRect clip = bounds(annotation, output, size)
                               .adjusted(-pad, -pad, pad, pad)
                               .toAlignedRect();
        if (clip.isEmpty() || !clip.intersects(QRect(QPoint(0, 0), size))) {
            return;
        }
        // A repaint narrowed to what a pointer move can have changed must not
        // pay for the marks outside it: at 4K a full-frame blit is about 1.7 ms,
        // and a capture can easily carry a hundred marks.  The clip is in the
        // same logical coordinates the rect is.
        if (painter->hasClipping() && !painter->clipBoundingRect().intersects(QRectF(clip))) {
            return;
        }
        const QByteArray key = signature(annotation, output, size);
        // The raster holds device pixels, not logical ones: the preview painter
        // carries the device-pixel-ratio transform while every mark is
        // rasterized in logical coordinates, and the overlay draws with smooth
        // transforms off.  A raster built one-logical-pixel-per-pixel would
        // therefore be magnified with nearest-neighbour and blur the mark on a
        // high-DPI screen.  The ratio is part of the cache's validity, so a
        // capture that moves to another screen rebuilds it.
        const qreal ratio = deviceRatio(painter);
        const QSize device = (QSizeF(clip.size()) * ratio).toSize();
        Cache &cache = caches_[outputIndex];
        if (cache.image.size() != device || cache.ratio != ratio) {
            // Only a different size or screen ratio needs a new buffer: a
            // rebuild for a changed mark redraws into the pixels it already
            // holds instead of allocating an identical image again.
            cache.image = QImage(device, QImage::Format_ARGB32_Premultiplied);
            cache.image.setDevicePixelRatio(ratio);
            cache.ratio = ratio;
            cache.key.clear();
        }
        if (key != cache.key) {
            cache.image.fill(Qt::transparent);
            QPainter raster(&cache.image);
            raster.setRenderHint(QPainter::Antialiasing, true);
            // No `scale(ratio, ratio)` here: the cache carries the ratio as its
            // device-pixel ratio, and QPainter applies that transform itself
            // when it paints into the image, so the painter already works in
            // logical coordinates.  Scaling once more multiplied it by a second
            // ratio -- on a 2x screen every committed mark came out twice the
            // size, with the part past its own clip cut away, while the live
            // preview stayed right because it never goes through a raster.
            raster.translate(-clip.topLeft());
            draw(&raster, annotation, output, size);
            cache.key = key;
            ++cache.rebuilds;
            builtAtRatio_ = ratio;
        }
        painter->drawImage(clip.topLeft(), cache.image);
    }

    // How many times the pixels have been built, over every output.  Read
    // through `Annotation::rasterRebuilds` by the offline check.
    int rebuilds() const
    {
        int total = 0;
        for (const Cache &cache : caches_) {
            total += cache.rebuilds;
        }
        return total;
    }

    // The identity of the pixels this mark would rasterize for `output`.  The
    // cache compares it against what it already holds; a caller that is
    // compositing several marks into one image uses it the same way, to tell
    // whether anything it drew has changed.
    QByteArray key(const Annotation &annotation, const OutputSession &output,
                   const QSize &size) const
    {
        return signature(annotation, output, size);
    }

    // Draws the mark straight into `painter`, with no cache in the way.
    //
    // For a caller compositing marks into a raster of its own -- the magnifier
    // -- the cache is not merely unnecessary but harmful: its slot would hold
    // pixels built for this other size and this other frame, and the two would
    // evict each other on every paint, rebuilding marks that had not changed.
    // The pixels are the same ones `paint` would produce, drawn through the
    // same `draw`, so the composite cannot drift from what is on the screen.
    void drawInto(QPainter *painter, const Annotation &annotation, const OutputSession &output,
                  const QSize &size) const
    {
        // The same hint `paint` gives its own raster, so the composite's marks
        // are antialiased exactly as the screen's are.
        const bool antialiased = painter->testRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::Antialiasing, true);
        draw(painter, annotation, output, size);
        painter->setRenderHint(QPainter::Antialiasing, antialiased);
    }

    // The device-pixel ratio the pixels were last built at, or 0 before the
    // first build.  Read through `Annotation::rasterDeviceRatio`.
    qreal builtAtRatio() const { return builtAtRatio_; }

    // How far outside the rect its `bounds` reports a mark's pixels can reach,
    // in logical pixels.  The controller sizes the region a drag has to
    // invalidate with it, so it cannot stay behind `protected`.
    int reach(const Annotation &annotation) const { return padding(annotation); }

    // The device-pixel ratio a raster has to be built at for this painter.  The
    // marks are rasterized in logical coordinates, so the raster has to hold
    // the device pixels the painter will actually touch.
    static qreal deviceRatio(const QPainter *painter)
    {
        const QPaintDevice *device = painter != nullptr ? painter->device() : nullptr;
        if (device == nullptr) {
            return 1.0;
        }
        const qreal ratio = device->devicePixelRatioF();
        return ratio > 0.0 ? ratio : 1.0;
    }

protected:
    // Local rect the mark covers, pens excluded.
    virtual QRectF bounds(const Annotation &annotation, const OutputSession &output,
                          const QSize &size) const = 0;
    // Everything the raster depends on; equal keys mean the cached pixels hold.
    virtual QByteArray signature(const Annotation &annotation, const OutputSession &output,
                                 const QSize &size) const = 0;
    // Draws the mark in overlay-local coordinates; the painter is already
    // translated so those coordinates match the overlay's own.
    virtual void draw(QPainter *painter, const Annotation &annotation, const OutputSession &output,
                      const QSize &size) const = 0;
    // Room around `bounds` for antialiasing and any pen or head that reaches
    // past the geometry itself.
    virtual int padding(const Annotation &annotation) const
    {
        return static_cast<int>(annotation.width) / 2 + 2;
    }

    // The key prefix every kind shares: the output the pixels were rasterized
    // against and the surface size they were rasterized for.
    //
    // `output.geometry` is deliberately *not* here.  The mosaic and the mosaic
    // brush sample the source image through it, so it does belong in their keys
    // -- but in their own, as an offset from the mark, which is what actually
    // decides which pixels they average.  Putting it here keyed every mark on
    // where the frame happened to sit: `applyPinRect` rewrites `geometry` on
    // every daemon confirmation, so dragging a pin invalidated and fully
    // re-rasterized every mark on it, once per motion event.  A plain shape's
    // pixels depend only on its own size and style, and a translation moves the
    // mark and the geometry together, so the difference is what stays constant.
    static void writeContext(QDataStream &stream, const OutputSession &output, const QSize &size)
    {
        const LogicalRect &surface = surfaceOf(output);
        stream << size.width() << size.height() << output.id << output.scale << surface.x
               << surface.y << surface.width << surface.height << output.image.cacheKey();
    }

private:
    // One cached raster per output.  `key` is empty while the buffer holds
    // nothing current, and `rebuilds` counts the builds this output needed.
    struct Cache {
        QImage image;
        QByteArray key;
        qreal ratio = 1.0;
        int rebuilds = 0;
    };
    QHash<int, Cache> caches_;
    qreal builtAtRatio_ = 0.0;
};

// Rectangles, ellipses and the area mosaic.  The mosaic averages the source
// image, so it reads the output as well as the annotation.
class ShapeRaster final : public AnnotationRaster {
protected:
    QRectF bounds(const Annotation &annotation, const OutputSession &output,
                  const QSize &size) const override
    {
        return localRect(output, annotation.rect, size);
    }

    QByteArray signature(const Annotation &annotation, const OutputSession &output,
                         const QSize &size) const override
    {
        QByteArray data;
        QDataStream stream(&data, QIODevice::WriteOnly);
        writeContext(stream, output, size);
        stream << annotation.tool << annotation.dash << annotation.width
               << static_cast<quint32>(annotation.color.rgba()) << annotation.mask
               << annotation.strength;
        // A plain shape's pixels depend only on its size and style, so a pure
        // translation leaves the cached raster valid and the blit lands it at
        // the new place.  The area mosaic instead averages the source image at
        // its absolute position, so a move changes every block it draws and
        // where it sits has to stay part of the key.  It is keyed on the rect's
        // offset *from the frame*, which is what the sampling reads: translating
        // a mark and the frame together -- what dragging a pin does -- leaves
        // the offset, and so the pixels, alone.
        if (annotation.tool == QStringLiteral("mosaic")) {
            stream << static_cast<qint64>(annotation.rect.x) - output.geometry.x
                   << static_cast<qint64>(annotation.rect.y) - output.geometry.y;
        }
        stream << static_cast<quint64>(annotation.rect.width)
               << static_cast<quint64>(annotation.rect.height);
        return data;
    }

    void draw(QPainter *painter, const Annotation &annotation, const OutputSession &output,
              const QSize &size) const override
    {
        if (annotation.tool == QStringLiteral("mosaic")) {
            drawMosaicAnnotation(painter, output, annotation.rect, annotation.mask,
                                 annotation.strength, size);
            return;
        }
        painter->setPen(penForAnnotation(annotation));
        painter->setBrush(Qt::NoBrush);
        const QRectF rect = localRect(output, annotation.rect, size);
        if (annotation.tool == QStringLiteral("ellipse")) {
            painter->drawEllipse(rect);
        } else {
            // Qt is the only renderer, so the dashed rectangle is the same
            // `drawRect` as the solid one with a dashed pen: the two cannot
            // disagree about where the band sits, because there is only one
            // place it is decided.
            painter->drawRect(rect);
        }
    }
};

// Freehand pen strokes, arrows and the freehand mosaic brush.
class StrokeRaster final : public AnnotationRaster {
protected:
    QRectF bounds(const Annotation &annotation, const OutputSession &output,
                  const QSize &size) const override
    {
        LogicalRect rect;
        if (!annotationLogicalBounds(annotation, &rect)) {
            return QRectF();
        }
        return localRect(output, rect, size);
    }

    QByteArray signature(const Annotation &annotation, const OutputSession &output,
                         const QSize &size) const override
    {
        QByteArray data;
        QDataStream stream(&data, QIODevice::WriteOnly);
        writeContext(stream, output, size);
        // Every number the ink depends on belongs here, not just the ones a
        // stroke has always carried: a wave drawn with a new amplitude or
        // wavelength, or a pen path whose fill mode changed, redraws only when
        // its key moves -- and a key without them kept the pixels the old
        // numbers produced.
        stream << annotation.tool << annotation.dash << annotation.width
               << static_cast<quint32>(annotation.color.rgba()) << annotation.size
               << annotation.arrowStyle << annotation.strength << annotation.closed
               << annotation.amplitude << annotation.wavelength << annotation.fill;
        if (annotation.tool == QStringLiteral("mosaic")) {
            // The freehand mosaic brush averages the source image under the
            // path, so every point's position relative to the frame has to stay
            // in the key -- relative, so that translating the mark and the frame
            // together leaves the sampled pixels alone.
            for (const Point &point : annotation.points) {
                stream << (static_cast<qint64>(point.x) - output.geometry.x)
                       << (static_cast<qint64>(point.y) - output.geometry.y);
            }
        } else {
            // A stroke's pixels depend only on the shape of its path, not on
            // where it sits: keep the point count and each point's offset from
            // the first, so translating the whole stroke leaves the key alone.
            stream << annotation.points.size();
            if (!annotation.points.isEmpty()) {
                const Point &first = annotation.points.constFirst();
                for (qsizetype index = 1; index < annotation.points.size(); ++index) {
                    stream << (annotation.points.at(index).x - first.x)
                           << (annotation.points.at(index).y - first.y);
                }
            }
        }
        return data;
    }

    void draw(QPainter *painter, const Annotation &annotation, const OutputSession &output,
              const QSize &size) const override
    {
        if (annotation.points.isEmpty()) {
            return;
        }
        if (annotation.tool == QStringLiteral("mosaic")) {
            // Freehand mosaic brush: smear discs along the path.
            drawMosaicBrush(painter, output, annotation.points, annotation.width,
                            annotation.strength, size);
            return;
        }
        if (annotation.tool == QStringLiteral("bezier")) {
            // A pen path is a cubic per segment rather than a polyline, and a
            // closed one is filled as well as stroked.  The points are put in
            // this overlay's own coordinates first: the same helper then serves
            // the live preview and the cached raster.
            QVector<QPointF> at;
            at.reserve(annotation.points.size());
            for (const Point &point : annotation.points) {
                at.append(localPoint(output, point, size));
            }
            paintBezierInk(painter, at, annotation.closed, annotation.fill, annotation.color,
                           static_cast<int>(annotation.width));
            return;
        }
        const double scale = outputScale(output);
        QPolygonF polygon;
        QPen pen = penForAnnotation(annotation);
        if (annotation.tool == QStringLiteral("wave") && annotation.points.size() >= 2) {
            // A wave is the sine sample of the segment between its two points,
            // not the segment itself: sample it here the same way the live
            // preview does, and draw it solid.
            // The crest offset and the period are logical pixels like every
            // other number here, and the painter is the logical one: the `scale`
            // argument only says how finely to sample, one point per device
            // pixel.  Multiplying them by the scale as well drew a high-density
            // screen's wave twice as large as the padding allowed, so its crests
            // were cut off by their own raster clip.
            const QVector<QPointF> wave = wavePolyline(
                localPoint(output, annotation.points.constFirst(), size),
                localPoint(output, annotation.points.constLast(), size),
                effectiveWaveAmplitude(annotation), effectiveWaveWavelength(annotation), scale);
            polygon = QPolygonF(wave.begin(), wave.end());
            pen = wavePen(annotation);
        } else {
            for (const Point &point : annotation.points) {
                polygon.push_back(localPoint(output, point, size));
            }
        }
        painter->setPen(pen);
        painter->drawPolyline(polygon);
        if (annotation.tool != QStringLiteral("arrow") || polygon.size() < 2) {
            return;
        }
        // The head is always solid and scales with the annotation's size.
        painter->setPen(QPen(annotation.color, static_cast<double>(annotation.width),
                             Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF end = polygon.constLast();
        const QPointF start = polygon.at(polygon.size() - 2);
        const QLineF line(start, end);
        if (line.length() <= 1.0) {
            return;
        }
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

    int padding(const Annotation &annotation) const override
    {
        if (annotation.tool == QStringLiteral("mosaic")) {
            // The disc radius in local pixels.  The brush is stamped in device
            // space as clamp(width*scale/2, .., 512) doubled for the strongest
            // setting; scaling back down, that is at most twice width/2 for
            // every output scale, which is what this bounds from.
            const int base = std::clamp(static_cast<int>(annotation.width) / 2, 1, 512);
            return std::clamp(brushRadiusForStrength(annotation.strength, base), 1, 512) + 2;
        }
        if (annotation.tool == QStringLiteral("arrow")) {
            const int head = std::max(6, static_cast<int>(annotation.width) * 4) *
                static_cast<int>(annotation.size);
            return head + static_cast<int>(annotation.width) + 4;
        }
        if (annotation.tool == QStringLiteral("wave")) {
            // The wave's crests reach its amplitude off the line its two points
            // describe -- the box `annotationLogicalBounds` reports -- so the
            // room has to cover that plus the pen's own half width and a pixel
            // for the antialiased edge.  `waveReach` is the very number the live
            // drag sizes its damage rectangle with, so the two cannot drift
            // apart and leave a crest outside the region a step invalidates.
            return static_cast<int>(
                std::ceil(waveReach(effectiveWaveAmplitude(annotation),
                                    static_cast<double>(annotation.width))));
        }
        return AnnotationRaster::padding(annotation);
    }
};

// One text label, rasterized with the preview font so the bitmap matches the
// final render.
class TextRaster final : public AnnotationRaster {
protected:
    QRectF bounds(const Annotation &annotation, const OutputSession &output,
                  const QSize &size) const override
    {
        LogicalRect rect;
        if (!annotationLogicalBounds(annotation, &rect)) {
            return QRectF();
        }
        return localRect(output, rect, size);
    }

    QByteArray signature(const Annotation &annotation, const OutputSession &output,
                         const QSize &size) const override
    {
        QByteArray data;
        QDataStream stream(&data, QIODevice::WriteOnly);
        writeContext(stream, output, size);
        // The text's pixels depend only on its text, font, size and colour: a
        // move shifts where the cached bitmap is blitted, not what it holds.
        stream << annotation.text << annotation.font << annotation.textPixels
               << static_cast<quint32>(annotation.color.rgba());
        return data;
    }

    void draw(QPainter *painter, const Annotation &annotation, const OutputSession &output,
              const QSize &size) const override
    {
        LogicalRect rect;
        if (!annotationLogicalBounds(annotation, &rect)) {
            return;
        }
        painter->setFont(annotationFont(annotation));
        painter->setPen(annotation.color);
        // Top-left anchored inside the measured bounds, which is where the
        // re-edit hit test looks for the text.
        painter->drawText(localRect(output, rect, size), Qt::AlignLeft | Qt::AlignTop,
                          annotation.text);
    }

    int padding(const Annotation &) const override { return 2; }
};

// A numbered badge, rasterized like a label: its pixels depend on the count,
// the style, the colour and the width it is sized from -- not on where it sits,
// so dragging one reuses the cached bitmap.  It pads by the ordinary stroke
// reach, which is what covers the ring style's line.
class NumberRaster final : public AnnotationRaster {
protected:
    QRectF bounds(const Annotation &annotation, const OutputSession &output,
                  const QSize &size) const override
    {
        LogicalRect rect;
        if (!annotationLogicalBounds(annotation, &rect)) {
            return QRectF();
        }
        return localRect(output, rect, size);
    }

    QByteArray signature(const Annotation &annotation, const OutputSession &output,
                         const QSize &size) const override
    {
        QByteArray data;
        QDataStream stream(&data, QIODevice::WriteOnly);
        writeContext(stream, output, size);
        stream << annotation.number << static_cast<int>(annotation.numberStyle)
               << static_cast<quint32>(annotation.width)
               << static_cast<quint32>(annotation.color.rgba());
        return data;
    }

    void draw(QPainter *painter, const Annotation &annotation, const OutputSession &output,
              const QSize &size) const override
    {
        LogicalRect rect;
        if (!annotationLogicalBounds(annotation, &rect)) {
            return;
        }
        // The box *is* the badge's own square, so it is drawn into directly
        // rather than centred on a point again.
        paintNumberBadge(*painter, localRect(output, rect, size),
                         QString::number(annotation.number), annotation.numberStyle,
                         annotation.color);
    }
};

// A pasted image, drawn at the rect it was placed at.
class ImageRaster final : public AnnotationRaster {
protected:
    QRectF bounds(const Annotation &annotation, const OutputSession &output,
                  const QSize &size) const override
    {
        if (annotation.pixels.isNull()) {
            return QRectF();
        }
        return localRect(output, annotation.rect, size);
    }

    QByteArray signature(const Annotation &annotation, const OutputSession &output,
                         const QSize &size) const override
    {
        QByteArray data;
        QDataStream stream(&data, QIODevice::WriteOnly);
        writeContext(stream, output, size);
        // The pasted pixels are drawn at the mark's rect, so what the raster
        // holds depends on where that rect sits *within the frame*, not on where
        // the frame itself is: a pin drag moves the mark and the geometry
        // together and leaves the pixels alone.
        stream << annotation.pixels.cacheKey()
               << static_cast<qint64>(annotation.rect.x) - output.geometry.x
               << static_cast<qint64>(annotation.rect.y) - output.geometry.y
               << static_cast<quint64>(annotation.rect.width)
               << static_cast<quint64>(annotation.rect.height);
        return data;
    }

    void draw(QPainter *painter, const Annotation &annotation, const OutputSession &output,
              const QSize &size) const override
    {
        if (annotation.pixels.isNull()) {
            return;
        }
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter->drawImage(localRect(output, annotation.rect, size), annotation.pixels);
    }

    int padding(const Annotation &) const override { return 2; }
};

// A placed translation, drawn from the lines the annotation carries: the boxes,
// the fill colours and the fitted fonts were settled when the translation came
// back, so nothing here depends on the frame under it and the cached raster is
// valid for as long as the lines are.
class TranslationRaster final : public AnnotationRaster {
protected:
    QRectF bounds(const Annotation &annotation, const OutputSession &output,
                  const QSize &size) const override
    {
        LogicalRect rect;
        if (!annotationLogicalBounds(annotation, &rect)) {
            return QRectF();
        }
        return localRect(output, rect, size);
    }

    QByteArray signature(const Annotation &annotation, const OutputSession &output,
                         const QSize &size) const override
    {
        QByteArray data;
        QDataStream stream(&data, QIODevice::WriteOnly);
        writeContext(stream, output, size);
        stream << annotation.font << annotation.translation.size();
        for (const TranslatedLine &line : annotation.translation) {
            // The lines are drawn where they sit in the frame, so the key holds
            // their offsets from it -- a pin drag translates the lines and the
            // geometry by the same amount and the pixels do not change.
            stream << static_cast<qint64>(line.source.x) - output.geometry.x
                   << static_cast<qint64>(line.source.y) - output.geometry.y
                   << static_cast<quint64>(line.source.width)
                   << static_cast<quint64>(line.source.height) << line.text << line.family
                   << line.fontPixels
                   << (static_cast<qint64>(line.fill.x) - output.geometry.x)
                   << (static_cast<qint64>(line.fill.y) - output.geometry.y)
                   << static_cast<quint32>(line.fill.width)
                   << static_cast<quint32>(line.fill.height)
                   << static_cast<quint32>(line.background.rgba())
                   << static_cast<quint32>(line.textColor.rgba());
        }
        return data;
    }

    void draw(QPainter *painter, const Annotation &annotation, const OutputSession &output,
              const QSize &size) const override
    {
        paintTranslation(
            *painter, annotation.translation,
            [&](const TranslatedLine &line) { return localRect(output, line.fill, size); }, 1.0);
    }

    // The fills are exact, so the pad only has to cover antialiasing at their
    // edges rather than a pen that reaches outside the geometry.
    int padding(const Annotation &) const override { return 2; }
};

std::shared_ptr<AnnotationRaster> makeAnnotationRaster(const Annotation &annotation)
{
    switch (annotation.kind) {
    case Annotation::Kind::Shape:
        return std::make_shared<ShapeRaster>();
    case Annotation::Kind::Text:
        // A numbered badge is a text annotation by wire, but not by paint: its
        // bitmap is drawn from the badge painter, and it caches on its own.
        if (isNumberAnnotation(annotation)) {
            return std::make_shared<NumberRaster>();
        }
        return std::make_shared<TextRaster>();
    case Annotation::Kind::Image:
        return std::make_shared<ImageRaster>();
    case Annotation::Kind::Translation:
        return std::make_shared<TranslationRaster>();
    case Annotation::Kind::Stroke:
        break;
    }
    return std::make_shared<StrokeRaster>();
}

int annotationReach(const Annotation &annotation)
{
    const std::shared_ptr<AnnotationRaster> raster =
        annotation.raster != nullptr ? annotation.raster : makeAnnotationRaster(annotation);
    return raster->reach(annotation);
}

int Annotation::rasterRebuilds() const
{
    return raster != nullptr ? raster->rebuilds() : -1;
}

qreal Annotation::rasterDeviceRatio() const
{
    return raster != nullptr ? raster->builtAtRatio() : 0.0;
}

// Draws the region editor's base layer -- the frozen session image plus the
// translucent veil over it -- from a pre-composed device-pixel image.  Composing
// the two together once turns a repaint's two full-frame passes into a single
// 1:1 blit; the in-selection copy of the image is still drawn by the caller.
//
// The composite is only used when the painter maps the overlay onto its device
// with a plain integer scale and no offset, which is what makes the cached blit
// land pixel for pixel: the target rects become exact integer device rects, so
// drawImage samples the composite one pixel to one device pixel.  Any other
// transform (a fractionally scaled or offset painter) returns false so the
// caller draws the two operations directly, keeping the output identical.
bool drawCachedBaseLayer(QPainter *painter, const OutputSession &output, const QSize &size,
                         const QRectF &imageRect, QImage *cache, QByteArray *cacheKey)
{
    const QTransform transform = painter->combinedTransform();
    if (transform.type() > QTransform::TxScale || transform.dx() != 0.0 ||
        transform.dy() != 0.0 || transform.m11() != transform.m22()) {
        return false;
    }
    const qreal ratio = transform.m11();
    if (ratio < 1.0 || ratio != std::floor(ratio)) {
        return false;
    }
    QByteArray key;
    {
        QDataStream stream(&key, QIODevice::WriteOnly);
        stream << output.id << output.image.cacheKey() << output.scale
               << output.geometry.x << output.geometry.y << output.geometry.width
               << output.geometry.height << output.surface.x << output.surface.y
               << output.surface.width << output.surface.height << size.width() << size.height()
               << static_cast<double>(ratio);
    }
    if (cache->isNull() || key != *cacheKey) {
        QImage composite((QSizeF(size) * ratio).toSize(), QImage::Format_ARGB32_Premultiplied);
        composite.setDevicePixelRatio(ratio);
        composite.fill(Qt::transparent);
        QPainter builder(&composite);
        // The composite's painter carries the same device-pixel-ratio transform
        // as the caller's, so the two operations below land on the very pixels
        // the caller would have drawn them to.
        builder.setRenderHint(QPainter::SmoothPixmapTransform, false);
        builder.drawImage(imageRect, output.image);
        builder.fillRect(QRectF(QPointF(0, 0), QSizeF(size)), QColor(0, 0, 0, 80));
        *cache = std::move(composite);
        *cacheKey = key;
    }
    painter->drawImage(QPointF(0, 0), *cache);
    return true;
}

void OverlayController::paint(CaptureOverlay *overlay, QPainter *painter)
{
    const OutputSession &output = overlay->output();
    const QRectF target(0, 0, overlay->width(), overlay->height());
    // The editor shows the session bounds (the image) at the selection, which
    // the pin editor lets the user drag around; region capture pins the
    // selection onto the frozen output, so the two coincide there.
    //
    // In pin-edit mode the marks clip rect uses marksOrigin_ (the confirmed
    // daemon position), not the optimistic selection_: the FP16 helper surface
    // shows the image at the confirmed position, so clipping to the same rect
    // keeps marks and image in sync.  The optimistic selection_ is only for
    // computing the next incremental move request.
    const LogicalRect imageArea =
        pinEdit_ && marksOrigin_.has_value() ? *marksOrigin_ : output.geometry;
    const QRectF imageRect = localRect(output, imageArea, overlay->size());
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
    // Picking shows the desktop live and never paints the session's frame:
    // that would freeze the very screen the user is choosing from.  Everything
    // but the hovered window is veiled so the pick stands out.  The hovered
    // window's outline and label pill are drawn below; the click ends the
    // session, and the frame it is captured into comes from Rust after that.
    const bool livePick = pickMode_;
    // The backdrop case: VShot is showing the frozen frame on its own HDR
    // surface underneath this one, at the screen's own light levels.  The
    // overlay's part is then only the veil, cut open at the selection — drawing
    // the SDR frame here as well would paint over the better picture.
    const bool backdrop = output.backdrop;
    // In pin-edit mode the pinned window itself shows the image: the editor
    // only draws the marks on top, so there is exactly one copy on screen.
    if (livePick || backdrop) {
        QPainterPath veil;
        veil.addRect(target);
        if (selection_.has_value()) {
            LogicalRect visible;
            if (intersection(*selection_, output.geometry, &visible)) {
                QPainterPath hole;
                hole.addRect(localRect(output, visible, overlay->size()));
                veil = veil.subtracted(hole);
            }
        }
        painter->fillPath(veil, QColor(0, 0, 0, 80));
    } else if (!pinEdit_) {
        // The frozen image and the veil over it do not change between repaints
        // (only the selection does), so they are composed once and blitted;
        // drawing them directly walks the whole frame twice per repaint.  The
        // helper refuses any painter whose transform it cannot reproduce
        // exactly, in which case the two operations are drawn as before.
        if (!drawCachedBaseLayer(painter, output, overlay->size(), imageRect, &baseComposite_,
                                 &baseCompositeKey_)) {
            painter->drawImage(imageRect, output.image);
            // The dim-out only makes sense around a selectable region.
            painter->fillRect(target, QColor(0, 0, 0, 80));
        }
        if (selection_.has_value()) {
            LogicalRect visible;
            if (intersection(*selection_, output.geometry, &visible)) {
                painter->drawImage(localRect(output, visible, overlay->size()), output.image,
                                   sourceRect(output, visible));
            }
        }
    }

    // The recognized characters of the selection, drawn where they were: above
    // the dim veil and below the marks.  It is drawn on every frame and never
    // composited into `baseComposite_`, whose key does not include the
    // selection -- a layer baked into that cache would freeze the highlight the
    // moment the range moved.
    if (textMode_ && textLayer_.has_value() && textLayer_->count() > 0) {
        const int count = textLayer_->count();
        const int low = textAnchor_ >= 0 ? std::min(textAnchor_, textFocus_) : -1;
        const int high = textAnchor_ >= 0 ? std::max(textAnchor_, textFocus_) : -1;
        // One bar per line of the selection, spanning what is actually selected.
        // Drawing each character's own box shows a row of little rectangles
        // wherever the recognizer left a gap between glyphs, while a text
        // selection reads as one run.  Units come in reading order and carry
        // their line, so a run is a span of indices grouped by that number.
        if (low >= 0) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(kTextSelectionFill);
            int runStart = low;
            while (runStart <= high) {
                const int line = textLayer_->unit(runStart).line;
                QRectF bar = localRect(output, textLayer_->unit(runStart).rect, overlay->size());
                ++runStart;
                while (runStart <= high && textLayer_->unit(runStart).line == line) {
                    bar = bar.united(
                        localRect(output, textLayer_->unit(runStart).rect, overlay->size()));
                    ++runStart;
                }
                painter->drawRect(bar);
            }
        }
        // One outline per line, around the union of its units: the user can see
        // what was recognized without a grid of touching boxes.
        painter->setPen(QPen(kTextOutline, 1.0));
        painter->setBrush(Qt::NoBrush);
        int lineStart = 0;
        while (lineStart < count) {
            const int line = textLayer_->unit(lineStart).line;
            int lineEnd = lineStart;
            while (lineEnd + 1 < count && textLayer_->unit(lineEnd + 1).line == line) {
                ++lineEnd;
            }
            bool haveBounds = false;
            QRectF lineBounds;
            for (int index = lineStart; index <= lineEnd; ++index) {
                const QRectF box =
                    localRect(output, textLayer_->unit(index).rect, overlay->size());
                lineBounds = haveBounds ? lineBounds.united(box) : box;
                haveBounds = true;
            }
            if (haveBounds) {
                painter->drawRect(lineBounds);
            }
            lineStart = lineEnd + 1;
        }
    }

    // The standalone translate overlay: the translation drawn in place over the
    // frozen scene, inside the rectangle the user framed.  Clipped to that
    // rectangle so the fill a long line grows cannot stray onto the rest of the
    // frame -- and so the PNG the accept writes, which is exactly this crop,
    // shows the same thing.
    if (translateMode_ && translated_ && !translatedLines_.isEmpty() && selection_.has_value()) {
        painter->save();
        painter->setClipRect(localRect(output, *selection_, overlay->size()));
        paintTranslation(
            *painter, translatedLines_,
            [&](const TranslatedLine &line) { return localRect(output, line.fill, overlay->size()); },
            1.0);
        painter->restore();
    }

    painter->setRenderHint(QPainter::Antialiasing, true);
    const QRectF outputBounds = target;
    painter->setClipRect(outputBounds);
    // Annotations are clipped to the image: dragging the pin around must not
    // leave marks floating on the transparent canvas, and the renderer only
    // ever composites them onto the image.
    //
    // That clip is also what keeps the magnifier honest.  It is drawn after
    // every mark -- it has to be, or a mark over the cursor would hide the
    // pixel it exists to show -- and the marks are drawn inside this clip, so
    // a magnifier hung inside it can only ever cover marks.  Where the pin
    // editor has the image and nothing else, the loupe is bounded by the image
    // too, which is the right thing: there are no pixels outside it to read.
    if (pinEdit_) {
        painter->setClipRect(imageRect, Qt::IntersectClip);
    }
    painter->setBrush(Qt::NoBrush);
    auto drawAnnotation = [this, &output, overlay, painter](const Annotation &annotation) {
        const double scale = outputScale(output);
        const QPen annotationPen = penForAnnotation(annotation);
        if (annotation.kind == Annotation::Kind::Image) {
            if (annotation.pixels.isNull()) {
                return;
            }
            const QRectF target = localRect(output, annotation.rect, overlay->size());
            painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter->drawImage(target, annotation.pixels);
            return;
        }
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
            if (isNumberAnnotation(annotation)) {
                // The badge's box, drawn as the badge: same painter as the
                // cached raster and the bitmap, so the preview cannot drift from
                // the mark the user ends up with.
                paintNumberBadge(*painter, localRect(output, bounds, overlay->size()),
                                 QString::number(annotation.number), annotation.numberStyle,
                                 annotation.color);
                return;
            }
            QFont font = annotationFont(annotation);
            painter->setFont(font);
            painter->setPen(annotation.color);
            // Top-left anchored inside the measured bounds, which is where the
            // re-edit hit test looks for the text.
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
        if (annotation.tool == QStringLiteral("bezier")) {
            // Drawn through the very helper the committed mark's rasterizer
            // uses, so letting go changes nothing on screen.
            QVector<QPointF> at;
            at.reserve(annotation.points.size());
            for (const Point &point : annotation.points) {
                at.append(localPoint(output, point, overlay->size()));
            }
            paintBezierInk(painter, at, annotation.closed, annotation.fill, annotation.color,
                           static_cast<int>(annotation.width));
            return;
        }
        QPolygonF polygon;
        QPen pen = annotationPen;
        if (annotation.tool == QStringLiteral("wave") && annotation.points.size() >= 2) {
            // A wave is the sine sample of the segment between its two points,
            // not the segment itself: sample it here exactly as the committed
            // mark's rasterizer does -- solid, and from the same two points --
            // so letting go changes nothing on screen.  The crest offset and
            // period stay logical, as they are in the rasterizer: `scale` only
            // sets the sampling density.
            const QVector<QPointF> wave = wavePolyline(
                localPoint(output, annotation.points.constFirst(), overlay->size()),
                localPoint(output, annotation.points.constLast(), overlay->size()),
                effectiveWaveAmplitude(annotation), effectiveWaveWavelength(annotation), scale);
            polygon = QPolygonF(wave.begin(), wave.end());
            pen = wavePen(annotation);
        } else {
            for (const Point &point : annotation.points) {
                polygon.push_back(localPoint(output, point, overlay->size()));
            }
        }
        painter->setPen(pen);
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

    // Committed marks are drawn from their cached raster: each one redraws
    // itself only when something it draws changed, so a repaint (a selection
    // drag, a pointer move) blits the untouched ones instead of recomputing
    // them -- the mosaic in particular, which averages the source image.
    for (const Annotation &annotation : annotations_) {
        if (annotation.raster == nullptr) {
            annotation.raster = makeAnnotationRaster(annotation);
        }
        annotation.raster->paint(painter, annotation, output, overlay->size(),
                                 overlay->outputIndex());
    }
    if (gesture_->type == Gesture::Type::Bezier && gesture_->points.size() >= 2) {
        // The pen path so far, plus the rubber band from its last anchor to the
        // pointer.  The band is a straight segment: the curve the next segment
        // would take is not known until its anchor is placed, and a band drawn
        // through the last anchor's own handle would loop back on the anchor
        // while that handle is being dragged.
        drawAnnotation(previewAnnotation());
        const Point &anchor = gesture_->points.at(gesture_->points.size() - 2);
        const QPointF from = localPoint(output, anchor, overlay->size());
        const QPointF to = localPoint(output, gesture_->current, overlay->size());
        if (QLineF(from, to).length() > 0.0) {
            const ToolStyle &bandStyle = toolStyle(QStringLiteral("bezier"));
            painter->setPen(QPen(bandStyle.color, static_cast<double>(bandStyle.width),
                                 Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush);
            painter->drawLine(from, to);
        }
        // The anchors and the handles, drawn the way the selection's own handles
        // are.  A curve on its own says nothing about where an anchor sits or
        // which way its handle points, so there is no way to aim the next click:
        // the anchor is a square, its handle a dot on the line it pulls along.
        // White with a dark outline so the guides never read as the user's own
        // ink.
        for (int index = 0; index + 1 < gesture_->points.size(); index += 2) {
            const QPointF anchorAt =
                localPoint(output, gesture_->points.at(index), overlay->size());
            const QPointF handleAt =
                localPoint(output, gesture_->points.at(index + 1), overlay->size());
            if (QLineF(anchorAt, handleAt).length() <= 0.0) {
                continue;
            }
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(QColor(255, 255, 255, 190), 1.0, Qt::SolidLine));
            painter->drawLine(anchorAt, handleAt);
            painter->setBrush(Qt::white);
            painter->setPen(QPen(Qt::black, 1.0));
            painter->drawEllipse(handleAt, 3.0, 3.0);
        }
        painter->setBrush(Qt::white);
        painter->setPen(QPen(Qt::black, 1.0));
        for (int index = 0; index < gesture_->points.size(); index += 2) {
            const QPointF anchorAt =
                localPoint(output, gesture_->points.at(index), overlay->size());
            painter->drawRect(QRectF(anchorAt.x() - 4.0, anchorAt.y() - 4.0, 8.0, 8.0));
        }
        painter->setBrush(Qt::NoBrush);
    }
    if (gesture_->type == Gesture::Type::Drawing && !gesture_->points.isEmpty() && tool_.has_value()) {
        const Tool armed = *tool_;
        // Rectangle, ellipse, the area mosaic and the arrow all depend on two
        // points, so drawing them straight is already cheap.  The freehand pen
        // and the mosaic brush grow a point per move and build up through the
        // incremental raster instead; a translucent pen would double-blend
        // where consecutive round caps overlap, so it keeps the straight draw.
        // The same predicate decides how much of the surface a move invalidates,
        // so both read it from one place.
        if (drawsGrowingStroke()) {
            paintLiveStroke(painter, output, overlay->size(), overlay->outputIndex());
        } else {
            Annotation preview;
            preview.tool = toolName(armed);
            const ToolStyle &previewStyle = toolStyle(preview.tool);
            preview.color = previewStyle.color;
            preview.width = previewStyle.width;
            preview.fill = currentFill_;
            preview.dash = currentDash_;
            preview.size = arrowSize_;
            preview.arrowStyle = currentArrowStyle_;
            preview.mask = mosaicShape_;
            preview.strength = mosaicStrength_;
            if (armed == Tool::Rectangle || armed == Tool::Ellipse ||
                (armed == Tool::Mosaic && mosaicShape_ != QStringLiteral("brush"))) {
                preview.kind = Annotation::Kind::Shape;
                preview.rect = selectionBetween(gesture_->points.constFirst(),
                                                gesture_->points.constLast());
                if (armed == Tool::Mosaic) {
                    preview.tool = QStringLiteral("mosaic");
                }
            } else {
                preview.kind = Annotation::Kind::Stroke;
                preview.points = gesture_->points;
                if (armed == Tool::Mosaic) {
                    preview.tool = QStringLiteral("mosaic");
                }
            }
            // Shapes, the arrow and a translucent pen draw straight: the first
            // three are two-point previews, and the last would double-blend
            // where consecutive round caps overlap.
            drawAnnotation(preview);
        }
    }

    // Highlight the selected annotation with handles while it is being
    // manipulated.  The frame is up whatever tool is armed -- a mark can be
    // picked up with a drawing tool in hand -- but not while that tool is
    // mid-stroke, where the frame would chase the ink.
    //
    // While nothing is armed, or while the pick-up modifier is held.  The
    // outline and its eight handles are the chrome of the state that adjusts
    // marks, and that state is the unarmed one; under a drawing tool they would
    // sit on top of the ink being laid down -- a pen stroke's own start is under
    // the left-middle handle -- and be read as part of the picture the user is
    // annotating.  The pick-up modifier is the other way into that state, and it
    // deliberately leaves the tool armed: the whole point of it is that picking a
    // mark up is not a tool change.  So it has to be read here as what it is, or
    // the modifier that exists to hand the user a mark would be the one state in
    // which the mark's handles never appear.
    const bool chromeArmed = !tool_.has_value() || pickingMarks(lastModifiers_);
    if (chromeArmed && selectedAnnotation_ >= 0 &&
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
            if (annotation.kind != Annotation::Kind::Text &&
                annotation.kind != Annotation::Kind::Translation) {
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

    // The mark the pick-up modifier has put under the pointer, framed so the
    // user can see what a press would take before making it.  Without this the
    // state is invisible: the pointer changes shape, but a shape says a drag is
    // possible somewhere, not *which* mark the press would land on -- and with
    // several marks under the pointer the wrong one is a real answer.
    //
    // No handles: they are the selected mark's, and this mark is not selected
    // yet.  The frame is dashed the same way so the two read as one language --
    // this is the mark, the handles come when it is yours.
    if (markHovered_ >= 0 && markHovered_ < annotations_.size()
        && markHovered_ != selectedAnnotation_) {
        LogicalRect bounds;
        if (annotationBounds(annotations_.at(markHovered_), &bounds)) {
            painter->setPen(QPen(Qt::white, 1.0, Qt::DashLine));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(localRect(output, bounds, overlay->size()));
        }
    }

    if (selection_.has_value() && !pinEdit_) {
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

    // The pin editor's own frame around the image.  The daemon draws the pin's
    // rim underneath, but that is the pin's identifying edge -- it says "this is
    // a pin", not "this is what you are editing" -- and it is a different
    // process besides, so it cannot say what the editor is doing.  The user's
    // rule is that the editor draws the selection-style frame for whatever it is
    // annotating, and the pin image is what this session annotates.
    //
    // No handles.  The eight handles a region selection carries resize it, and
    // the CLI refuses any size change to a pin (`qt_overlay.rs`, "pin editing
    // only moves it"), so handles here would be a promise the editor cannot
    // keep.  What the pin can do is move, and the pointer already says so.
    //
    // Drawn after the marks clip is taken off -- the clip is the image rect, and
    // a two-pixel stroke centred on the image's edge would otherwise lose its
    // outer half -- and at `marksOrigin_`, the daemon-confirmed rect, so it stays
    // in step with the image rather than running ahead of it during a drag.
    //
    // Only while this pin is the live one.  The frame is Qt chrome and every
    // other pin is painted by a Wayland surface one layer below, so the moment
    // the user's attention moves to another pin this frame would be drawn on
    // top of it -- and the compositor orders a layer's surfaces by map time with
    // no way to restack them, so no amount of ordering on this side can put it
    // back underneath.  The daemon says which pin is live (`notePinActive`), and
    // the pin being edited is the live one until the user picks another.
    if (pinEdit_ && pinActive_ && marksOrigin_.has_value() && !session_.outputs.isEmpty()) {
        const QRectF frame =
            localRect(output, *marksOrigin_, overlay->size());
        painter->setPen(QPen(Qt::white, 2.0, Qt::SolidLine));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(frame);
    }

    // Window picking previews a whole window before it is committed, so its
    // pill names the window instead of just measuring it.
    const bool pickPreview = pickMode_ && !editing_ && selection_.has_value();
    // The pill belongs to the output the selection starts on.  Every overlay
    // paints the whole chrome and clips what is not its own, but the pill is
    // clamped into the surface rather than clipped, so without this the other
    // outputs would each grow a copy of it at their own edge.
    const bool ownsSelection = selection_.has_value() && selection_->x >= output.geometry.x &&
        selection_->x < output.geometry.right() && selection_->y >= output.geometry.y &&
        selection_->y < output.geometry.bottom();
    if (ownsSelection && !pinEdit_ &&
        (pickPreview || gesture_->type == Gesture::Type::Selecting || editing_)) {
        // The floating toolbar in this overlay's own coordinates: it is a child
        // widget, so it paints over whatever this paint() puts under it.
        QRectF toolbarBox;
        if (toolbar_ != nullptr && toolbar_->isVisible() && toolbar_->parentWidget() != nullptr) {
            QWidget *host = toolbar_->parentWidget();
            const QPoint origin = overlay->mapToGlobal(QPoint(0, 0));
            toolbarBox = QRectF(host->mapToGlobal(toolbar_->pos()) - origin,
                                QSizeF(toolbar_->size()));
        }
        drawSelectionPill(painter,
                          localPoint(output, Point{selection_->x, selection_->y}, overlay->size()),
                          localRect(output, *selection_, overlay->size()), selectionPillText(),
                          target, toolbarBox);
    }

    // The magnifier sits above every mark: it is a reading of the pixels the
    // user is aiming at, and a mark drawn over it would hide exactly the pixel
    // it exists to show.  It is drawn last for that reason.
    //
    // Any drag brings it up: every one of them is the user placing a point, and
    // the point is a pixel.  The right button brings it up on its own, and a
    // keyboard step flashes it, so aiming is covered whatever is moving.
    const Gesture::Type loupeGesture = gesture_->type;
    // Every drag but the ones that lay ink down.  A stroke is drawn where the
    // cursor is, and the loupe is a 120-pixel disc hung off that same cursor:
    // showing it while drawing covers the ink the user is placing, which is
    // exactly what pixel-level annotating needs to see.  Moving, resizing and
    // framing are the drags that aim at pixels already on the screen.
    const bool dragging = loupeGesture != Gesture::Type::None &&
        loupeGesture != Gesture::Type::Bezier && loupeGesture != Gesture::Type::Drawing;
    // The pin editor drags the pinned image itself around the screen, and a
    // magnifier there would follow the very picture it is magnifying: the loupe
    // belongs to picking a region out of a frozen frame, not to placing a pin.
    const bool pinDrag = pinEdit_ && loupeGesture == Gesture::Type::Moving;
    // The eyedropper's loupe follows an idle pointer -- that is the whole
    // instrument, and the readout under it is where the colour is read from --
    // and it stays off the bare canvas a pin editor leaves around its image,
    // where there is no pixel to take.
    const bool pickerLoupe = tool_.has_value() && *tool_ == Tool::Picker &&
        loupeGesture == Gesture::Type::None && (!pinEdit_ || canDrawAt(pointer_));
    if (!pinDrag && (magnifierVisible() || pickerLoupe || dragging) &&
        pointerOutput_ == overlay->outputIndex()) {
        // The clip goes back to the surface before the loupe is drawn.  The
        // disc hangs off the cursor by more than its own radius, so near the
        // edge of the picture it deliberately reaches past it -- and the pin
        // editor's image clip, taken above for the marks, cut it off there.
        // That is the one place a magnifier is most wanted, so the loupe is the
        // editor's chrome rather than a mark and is not bounded by the image.
        painter->setClipRect(target);
        drawLoupe(overlay, painter);
    }
    painter->restore();
}

// Draws the committed marks onto a copy of the session's own pixels.
//
// The editor's screen and the result are not the same raster: the screen is a
// window in logical pixels with a device ratio, and the result is the captured
// frame in the output's device pixels.  The mapping between them is one factor,
// `density` -- the output's device pixels per logical pixel -- which is exactly
// what `output.scale` carries for the output the marks were placed on.  So the
// output is rebuilt here with that scale, which makes every `localRect` and
// `localPoint` in the painters below land on the result's pixels with no
// arithmetic of its own.
//
// The painter's device ratio is set to the same factor rather than the mark
// being scaled, so each mark's cached raster is rebuilt at the result's
// resolution: a mark whose preview was cached at ratio 2 is rasterized again at
// the output's density instead of being blitted at the wrong one.  The cache
// belongs to the annotation and keys on the ratio, so a preview and a render at
// different densities cannot read each other's pixels.
QImage OverlayController::compositeAnnotations(const QImage &canvas, const QImage &source,
                                               const LogicalRect &origin, double density,
                                               QString *error) const
{
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return QImage();
    };
    if (canvas.isNull() || source.isNull()) {
        return fail(uiTr("There are no pixels to draw the annotations on."));
    }
    if (!(density > 0.0)) {
        return fail(uiTr("The capture's density is not a positive number."));
    }
    if (canvas.size() != source.size()) {
        return fail(uiTr("The annotations would be drawn on pixels of another size."));
    }
    if (annotations_.isEmpty()) {
        return canvas;
    }
    // The marks were placed on the output the capture came from, which is the
    // first one: a capture that spans several outputs is cropped from the scene
    // at the highest density, and that is what `density` names.  Only its scale
    // and its place matter here -- the pixels come in as an argument, not from
    // the session -- so a fresh record is enough and the session need not have
    // an output at all.  Its place is `origin`, the crop's own rect in the
    // overlay's logical pixels: a mark's coordinates are global, and the painter
    // draws in the crop's, so leaving the record at the origin would put every
    // mark at the selection's offset from the screen's corner.
    OutputSession output;
    output.scale = density;
    output.geometry = LogicalRect{origin.x, origin.y,
                                  static_cast<std::uint32_t>(canvas.width()),
                                  static_cast<std::uint32_t>(canvas.height())};
    output.surface = output.geometry;
    // What a mosaic samples.  For the flattened result this is the canvas
    // itself; for the marks alone it is the capture, which the canvas does not
    // hold.
    output.image = source;

    QImage composite = canvas.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (composite.isNull()) {
        return fail(uiTr("The capture's pixels could not be converted for drawing."));
    }
    // The device ratio is what maps the painter's logical coordinates onto the
    // result's pixels, and setting it on the image is what makes the painter
    // pick it up: `AnnotationRaster::paint` reads it back off the paint device,
    // so the cached rasters are built at the result's resolution rather than the
    // screen's.
    composite.setDevicePixelRatio(density);
    // Off the screen: this runs after the session ended, and a mark drawn
    // through an overlay's own size would be placed for the window rather than
    // for the result.
    QPainter painter(&composite);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    for (const Annotation &annotation : annotations_) {
        if (annotation.raster == nullptr) {
            annotation.raster = makeAnnotationRaster(annotation);
        }
        annotation.raster->paint(&painter, annotation, output, composite.size(), 0);
    }
    painter.end();
    // The result is a plain raster of device pixels; the ratio was this side's
    // business and no consumer of the bytes should scale by it.
    composite.setDevicePixelRatio(1.0);
    return composite;
}

// A mark's colour, read the way the CLI wrote it: `#rrggbb`, or `#rrggbbaa`
// when the alpha is not opaque.  The eight-digit form spells alpha last, which
// is what `colorText` writes and what `parseColorText` reads back.
QColor annotationColor(const QJsonValue &value, const QColor &fallback)
{
    if (!value.isString()) {
        return fallback;
    }
    const QColor color = parseColorText(value.toString());
    return color.isValid() ? color : fallback;
}

// One of a mark's small enumerations, read by name.  `allowed` is the set the
// wire may carry; anything else is refused rather than silently defaulted,
// because a mark the editor cannot reproduce exactly is worse than a session
// that says so.
bool annotationToken(const QJsonValue &value, const QStringList &allowed, const QString &label,
                     QString *out, QString *error)
{
    if (!value.isString() || !allowed.contains(value.toString())) {
        return jsonFail(error, label + QStringLiteral(" must be one of ") +
                                   allowed.join(QStringLiteral(", ")));
    }
    *out = value.toString();
    return true;
}

bool annotationWhole(const QJsonObject &object, const char *key, std::uint32_t minimum,
                     std::uint32_t maximum, std::uint32_t *out, const QString &label,
                     QString *error)
{
    std::int64_t value = 0;
    if (!jsonInteger(object.value(QLatin1String(key)), minimum, maximum, &value)) {
        return jsonFail(error, QStringLiteral("%1 must be an integer in %2..%3")
                              .arg(label)
                              .arg(minimum)
                              .arg(maximum));
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

// The canvas the marks were made on: a mark's coordinates only mean something
// against it, and one that fell outside would be painted off the edge and never
// seen again.  Refusing it is what makes a session from a different capture
// fail loudly rather than open with invisible marks.
//
// `x` and `y` arrive relative to the canvas's top-left, which is the frame
// `marksDocument` writes them in, so the bounds are the canvas's own size and
// not its place on the screen.
bool insideCanvas(std::int64_t x, std::int64_t y, const LogicalRect &canvas, const QString &label,
                  QString *error)
{
    if (x < 0 || y < 0 || x > static_cast<std::int64_t>(canvas.width) ||
        y > static_cast<std::int64_t>(canvas.height)) {
        return jsonFail(error, label + QStringLiteral(" lies outside the canvas"));
    }
    return true;
}

bool canvasRect(const QJsonObject &object, const LogicalRect &canvas, LogicalRect *out,
                const QString &label, QString *error)
{
    LogicalRect rect;
    if (!jsonRect(object, &rect, label, error)) {
        return false;
    }
    if (!insideCanvas(rect.x, rect.y, canvas, label, error) ||
        !insideCanvas(rect.right(), rect.bottom(), canvas, label, error)) {
        return false;
    }
    // Back into the overlay's global logical pixels, which is where the editor
    // keeps every mark it holds.
    rect.x += canvas.x;
    rect.y += canvas.y;
    *out = rect;
    return true;
}

// A rect written under a `rect` key, which is how every mark but a translation
// line spells its boxes.
bool annotationRect(const QJsonObject &object, const LogicalRect &canvas, LogicalRect *out,
                    const QString &label, QString *error)
{
    const QJsonValue value = object.value(QStringLiteral("rect"));
    if (!value.isObject()) {
        return jsonFail(error, label + QStringLiteral(" must carry a `rect` object"));
    }
    return canvasRect(value.toObject(), canvas, out, label, error);
}

bool annotationPoint(const QJsonObject &object, const LogicalRect &canvas, Point *out,
                     const QString &label, QString *error)
{
    std::int32_t x = 0;
    std::int32_t y = 0;
    if (!jsonSigned32(object, "x", &x) || !jsonSigned32(object, "y", &y)) {
        return jsonFail(error, label + QStringLiteral(" must have integer x and y"));
    }
    if (!insideCanvas(x, y, canvas, label, error)) {
        return false;
    }
    out->x = x + canvas.x;
    out->y = y + canvas.y;
    return true;
}

// One mark, read back from the shape `marksDocument` writes.
//
// The two directions are one protocol read from either end, so the names here
// are the ones that writer emits and nothing else.  A mark of a kind this editor
// does not know is refused rather than skipped: the writer only ever emits the
// kinds below, so seeing another means the wire is not this protocol, and
// quietly dropping it would open the pin with a mark the user drew missing.
//
// A pasted image arrives as a path to its pixels, not as pixels: it is written
// beside the session by `writeMarkAssets` and the mark names the file.  Reading
// it needs the file to still be there, which is why the session directory
// outlives the session that made it.
bool parseMark(const QJsonObject &mark, const LogicalRect &canvas, std::uint32_t deviceRatio,
               Annotation *out, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    const QJsonValue kindValue = mark.value(QStringLiteral("kind"));
    if (!kindValue.isString()) {
        return fail(uiTr("A mark must carry a `kind`."));
    }
    const QString kind = kindValue.toString();
    const QString label = QStringLiteral("mark `%1`").arg(kind);

    const QJsonValue toolValue = mark.value(QStringLiteral("tool"));
    if (!toolValue.isString()) {
        return fail(label + QStringLiteral(" must carry a `tool`"));
    }
    const QString tool = toolValue.toString();

    Annotation annotation;
    annotation.tool = tool;
    annotation.deviceRatio = deviceRatio;

    if (kind == QStringLiteral("shape")) {
        static const QStringList kShapeTools = {QStringLiteral("rectangle"),
                                                QStringLiteral("ellipse"),
                                                QStringLiteral("mosaic")};
        static const QStringList kDashes = {QStringLiteral("solid"), QStringLiteral("dashed"),
                                            QStringLiteral("dotted")};
        static const QStringList kMasks = {QStringLiteral("rect"), QStringLiteral("ellipse")};
        QString checked;
        if (!annotationToken(toolValue, kShapeTools, label + QStringLiteral(" tool"), &checked,
                             error)) {
            return false;
        }
        if (!annotationRect(mark, canvas, &annotation.rect, label, error)) {
            return false;
        }
        annotation.kind = Annotation::Kind::Shape;
        annotation.color = annotationColor(mark.value(QStringLiteral("color")), annotation.color);
        if (!annotationWhole(mark, "width", 1, static_cast<std::uint32_t>(kMaxWidth),
                             &annotation.width, label + QStringLiteral(" width"), error) ||
            !annotationWhole(mark, "strength", 1,
                             static_cast<std::uint32_t>(kMaxMosaicStrength), &annotation.strength,
                             label + QStringLiteral(" strength"), error)) {
            return false;
        }
        if (!annotationToken(mark.value(QStringLiteral("dash")), kDashes,
                             label + QStringLiteral(" dash"), &annotation.dash, error) ||
            !annotationToken(mark.value(QStringLiteral("mask")), kMasks,
                             label + QStringLiteral(" mask"), &annotation.mask, error)) {
            return false;
        }
    } else if (kind == QStringLiteral("stroke")) {
        // Every tool that lays down a freehand stroke has to be here, and the
        // mosaic brush is one of them: `finishDrawing` stores it as a Stroke
        // whenever the shape is the brush rather than a rect or an ellipse, and
        // its `tool` reads `"mosaic"`.  Leaving it out made the reader refuse a
        // document the writer had just produced -- and because `parseMarks` is
        // all-or-nothing, that one mark took every other mark on the pin down
        // with it, which is how a re-edit came up blank.
        static const QStringList kStrokeTools = {
            QStringLiteral("arrow"),  QStringLiteral("pen"),  QStringLiteral("draw"),
            QStringLiteral("line"),   QStringLiteral("wave"), QStringLiteral("bezier"),
            QStringLiteral("mosaic")};
        static const QStringList kDashes = {QStringLiteral("solid"), QStringLiteral("dashed"),
                                            QStringLiteral("dotted")};
        static const QStringList kArrowStyles = {QStringLiteral("open"),
                                                 QStringLiteral("filled")};
        static const QStringList kFills = {QStringLiteral("stroke"), QStringLiteral("fill"),
                                           QStringLiteral("both")};
        QString checked;
        if (!annotationToken(toolValue, kStrokeTools, label + QStringLiteral(" tool"), &checked,
                             error)) {
            return false;
        }
        const QJsonValue pointsValue = mark.value(QStringLiteral("points"));
        if (!pointsValue.isArray()) {
            return fail(label + QStringLiteral(" must carry a `points` array"));
        }
        const QJsonArray points = pointsValue.toArray();
        if (points.isEmpty()) {
            return fail(label + QStringLiteral(" must have at least one point"));
        }
        annotation.points.reserve(points.size());
        for (int index = 0; index < points.size(); ++index) {
            if (!points.at(index).isObject()) {
                return fail(label + QStringLiteral(" point %1 must be an object").arg(index));
            }
            Point point;
            if (!annotationPoint(points.at(index).toObject(), canvas, &point,
                                 label + QStringLiteral(" point %1").arg(index), error)) {
                return false;
            }
            annotation.points.push_back(point);
        }
        annotation.kind = Annotation::Kind::Stroke;
        annotation.color = annotationColor(mark.value(QStringLiteral("color")), annotation.color);
        if (!annotationWhole(mark, "width", 1, static_cast<std::uint32_t>(kMaxWidth),
                             &annotation.width, label + QStringLiteral(" width"), error) ||
            !annotationWhole(mark, "size", 1, static_cast<std::uint32_t>(kMaxArrowSize),
                             &annotation.size, label + QStringLiteral(" size"), error) ||
            !annotationWhole(mark, "strength", 1,
                             static_cast<std::uint32_t>(kMaxMosaicStrength), &annotation.strength,
                             label + QStringLiteral(" strength"), error) ||
            !annotationWhole(mark, "amplitude", 0, static_cast<std::uint32_t>(kMaxWaveSize),
                             &annotation.amplitude, label + QStringLiteral(" amplitude"), error) ||
            !annotationWhole(mark, "wavelength", 0,
                             static_cast<std::uint32_t>(kMaxWaveWavelength),
                             &annotation.wavelength, label + QStringLiteral(" wavelength"),
                             error)) {
            return false;
        }
        if (!annotationToken(mark.value(QStringLiteral("dash")), kDashes,
                             label + QStringLiteral(" dash"), &annotation.dash, error) ||
            !annotationToken(mark.value(QStringLiteral("arrow_style")), kArrowStyles,
                             label + QStringLiteral(" arrow_style"), &annotation.arrowStyle,
                             error) ||
            !annotationToken(mark.value(QStringLiteral("fill")), kFills,
                             label + QStringLiteral(" fill"), &annotation.fill, error)) {
            return false;
        }
        const QJsonValue closed = mark.value(QStringLiteral("closed"));
        if (!closed.isBool()) {
            return fail(label + QStringLiteral(" must carry a boolean `closed`"));
        }
        annotation.closed = closed.toBool();
    } else if (kind == QStringLiteral("text")) {
        const QJsonValue originValue = mark.value(QStringLiteral("origin"));
        if (!originValue.isObject()) {
            return fail(label + QStringLiteral(" must carry an `origin` object"));
        }
        if (!annotationPoint(originValue.toObject(), canvas, &annotation.origin, label, error)) {
            return false;
        }
        const QJsonValue textValue = mark.value(QStringLiteral("text"));
        if (!textValue.isString()) {
            return fail(label + QStringLiteral(" must carry its `text`"));
        }
        annotation.kind = Annotation::Kind::Text;
        annotation.color = annotationColor(mark.value(QStringLiteral("color")), annotation.color);
        annotation.font = mark.value(QStringLiteral("font")).toString();
        if (tool == QStringLiteral("number")) {
            const QJsonValue style = mark.value(QStringLiteral("numberStyle"));
            if (!style.isString()) {
                return fail(label + QStringLiteral(" must carry its `numberStyle`"));
            }
            annotation.numberStyle = numberStyleForName(style.toString());
            if (!annotationWhole(mark, "numberSize",
                                 static_cast<std::uint32_t>(kNumberMinDiameter),
                                 static_cast<std::uint32_t>(kNumberMaxDiameter),
                                 &annotation.numberSize, label + QStringLiteral(" numberSize"),
                                 error)) {
                return false;
            }
            bool ok = false;
            annotation.number = textValue.toString().toInt(&ok);
            if (!ok) {
                return fail(label + QStringLiteral(" must carry an integer count as its `text`"));
            }
            // The box is derived from the origin the badge was placed at rather
            // than read from the wire: the hit test, the drag clamp and the
            // raster cache are all sized from it, and a box that disagreed with
            // the diameter would make the badge undraggable.  `layoutNumberBox`
            // records the box's top-left as the origin, so the centre is that
            // origin plus half the diameter it is about to lay out.
            const int half = numberDiameter(annotation.numberSize) / 2;
            const Point centre{annotation.origin.x + half, annotation.origin.y + half};
            layoutNumberBox(annotation, centre);
        } else {
            annotation.text = textValue.toString();
            if (!annotationWhole(mark, "textPixels",
                                 static_cast<std::uint32_t>(kMinTextPixels),
                                 static_cast<std::uint32_t>(kMaxTextPixels),
                                 &annotation.textPixels, label + QStringLiteral(" textPixels"),
                                 error)) {
                return false;
            }
        }
    } else if (kind == QStringLiteral("image")) {
        // The pixels are named by path, not carried in the document; the mark
        // holds the image the path resolves to.  They are the *original* pixels
        // at their own size, and `rect` is the separate question of where the
        // mark sits and how far it was scaled to fit, so a resize later can go
        // back to the original instead of to an already-shrunk copy.
        if (!annotationRect(mark, canvas, &annotation.rect, label, error)) {
            return false;
        }
        const QJsonValue path = mark.value(QStringLiteral("pixels"));
        if (!path.isString() || path.toString().isEmpty()) {
            return fail(label + QStringLiteral(" must carry its `pixels` path"));
        }
        QImage pixels;
        if (!pixels.load(path.toString())) {
            return fail(label + QStringLiteral(" could not read its pixels from `%1`")
                            .arg(path.toString()));
        }
        annotation.kind = Annotation::Kind::Image;
        annotation.pixels = pixels;
    } else if (kind == QStringLiteral("translation")) {
        // A translation is its lines, and the lines are all numbers and strings,
        // so the whole mark is in the document and needs no asset.  Each line's
        // two boxes are read through the same point reader as everything else,
        // so a line that lands outside the canvas is refused here rather than
        // drawn off the image later.
        if (!annotationRect(mark, canvas, &annotation.rect, label, error)) {
            return false;
        }
        const QJsonValue linesValue = mark.value(QStringLiteral("lines"));
        if (!linesValue.isArray() || linesValue.toArray().isEmpty()) {
            return fail(label + QStringLiteral(" must carry a non-empty `lines` array"));
        }
        const QJsonArray lines = linesValue.toArray();
        annotation.translation.reserve(lines.size());
        for (int index = 0; index < lines.size(); ++index) {
            const QString lineLabel = label + QStringLiteral(" line %1").arg(index);
            if (!lines.at(index).isObject()) {
                return fail(lineLabel + QStringLiteral(" must be an object"));
            }
            const QJsonObject entry = lines.at(index).toObject();
            TranslatedLine line;
            if (!canvasRect(entry, canvas, &line.source, lineLabel, error) ||
                !canvasRect(entry, canvas, &line.fill, lineLabel, error)) {
                return false;
            }
            const QJsonValue text = entry.value(QStringLiteral("text"));
            if (!text.isString()) {
                return fail(lineLabel + QStringLiteral(" must carry its `text`"));
            }
            line.text = text.toString();
            line.family = entry.value(QStringLiteral("family")).toString();
            std::uint32_t fontPixels = 0;
            if (!annotationWhole(entry, "font_pixels", 1,
                                 static_cast<std::uint32_t>(kMaxTextPixels), &fontPixels,
                                 lineLabel + QStringLiteral(" font_pixels"), error)) {
                return false;
            }
            line.fontPixels = static_cast<int>(fontPixels);
            line.background =
                annotationColor(entry.value(QStringLiteral("background")), line.background);
            line.textColor =
                annotationColor(entry.value(QStringLiteral("text_color")), line.textColor);
            annotation.translation.push_back(line);
        }
        annotation.kind = Annotation::Kind::Translation;
        annotation.font = mark.value(QStringLiteral("font")).toString();
    } else {
        return fail(QStringLiteral("mark kind `%1` is not one this editor can reopen").arg(kind));
    }

    *out = annotation;
    return true;
}

// Reads the marks a session carried into annotations the editor can place.
//
// The wire shape is the one `marksDocument` writes and the editor's own result
// uses, so the two directions are one protocol read from either end.  A mark
// that cannot be rebuilt -- an unknown tool, a missing field, a rectangle off
// the canvas -- fails the whole session rather than being skipped: an editor
// that opened with some of the user's marks silently missing is worse than one
// that says it cannot open at all.
bool OverlayController::parseMarks(const QJsonArray &marks, const LogicalRect &canvas,
                                   std::uint32_t deviceRatio, QString *error)
{
    QVector<Annotation> restored;
    restored.reserve(marks.size());
    for (int index = 0; index < marks.size(); ++index) {
        if (!marks.at(index).isObject()) {
            if (error != nullptr) {
                *error = QStringLiteral("mark %1 is not an object").arg(index);
            }
            return false;
        }
        Annotation annotation;
        if (!parseMark(marks.at(index).toObject(), canvas, deviceRatio, &annotation, error)) {
            return false;
        }
        restored.push_back(annotation);
    }
    annotations_ = std::move(restored);
    return true;
}

// A logical rect as the wire's own object, offset by the canvas: the document is
// relative to the selection, which is the canvas a re-edit hands back.  Written
// flat rather than under a `rect` key, which is how a translation's two boxes
// per line are spelled; `parseMark` reads them back through `canvasRect`.
static QJsonObject rectJson(const LogicalRect &rect, const LogicalRect &canvas)
{
    QJsonObject value;
    value.insert(QStringLiteral("x"), static_cast<qint64>(rect.x) - canvas.x);
    value.insert(QStringLiteral("y"), static_cast<qint64>(rect.y) - canvas.y);
    value.insert(QStringLiteral("width"), static_cast<qint64>(rect.width));
    value.insert(QStringLiteral("height"), static_cast<qint64>(rect.height));
    return value;
}

QJsonArray OverlayController::marksDocument() const
{
    return marksDocumentInto(QString());
}

QJsonArray OverlayController::writeMarkAssets(const QString &directory) const
{
    return marksDocumentInto(directory);
}

QJsonArray OverlayController::marksDocumentInto(const QString &directory) const
{
    QJsonArray marks;
    if (!selection_.has_value()) {
        return marks;
    }
    // The marks are kept in the overlay's global logical pixels; the document
    // is relative to the selection, because the selection is the canvas a
    // re-edit hands back.
    const LogicalRect &canvas = *selection_;
    int asset = 0;
    for (const Annotation &annotation : annotations_) {
        QJsonObject value;
        if (annotation.kind == Annotation::Kind::Shape) {
            value.insert(QStringLiteral("kind"), QStringLiteral("shape"));
            value.insert(QStringLiteral("tool"), annotation.tool);
            value.insert(QStringLiteral("color"), colorText(annotation.color));
            value.insert(QStringLiteral("width"), static_cast<qint64>(annotation.width));
            value.insert(QStringLiteral("dash"), annotation.dash);
            value.insert(QStringLiteral("mask"), annotation.mask);
            value.insert(QStringLiteral("strength"), static_cast<qint64>(annotation.strength));
            QJsonObject rect;
            rect.insert(QStringLiteral("x"), static_cast<qint64>(annotation.rect.x - canvas.x));
            rect.insert(QStringLiteral("y"), static_cast<qint64>(annotation.rect.y - canvas.y));
            rect.insert(QStringLiteral("width"), static_cast<qint64>(annotation.rect.width));
            rect.insert(QStringLiteral("height"), static_cast<qint64>(annotation.rect.height));
            value.insert(QStringLiteral("rect"), rect);
        } else if (annotation.kind == Annotation::Kind::Stroke) {
            value.insert(QStringLiteral("kind"), QStringLiteral("stroke"));
            value.insert(QStringLiteral("tool"), annotation.tool);
            value.insert(QStringLiteral("color"), colorText(annotation.color));
            value.insert(QStringLiteral("width"), static_cast<qint64>(annotation.width));
            value.insert(QStringLiteral("dash"), annotation.dash);
            value.insert(QStringLiteral("size"), static_cast<qint64>(annotation.size));
            value.insert(QStringLiteral("arrow_style"), annotation.arrowStyle);
            value.insert(QStringLiteral("strength"), static_cast<qint64>(annotation.strength));
            value.insert(QStringLiteral("amplitude"), static_cast<qint64>(annotation.amplitude));
            value.insert(QStringLiteral("wavelength"), static_cast<qint64>(annotation.wavelength));
            value.insert(QStringLiteral("closed"), annotation.closed);
            value.insert(QStringLiteral("fill"), annotation.fill);
            QJsonArray points;
            for (const Point &point : annotation.points) {
                QJsonObject item;
                item.insert(QStringLiteral("x"), static_cast<qint64>(point.x - canvas.x));
                item.insert(QStringLiteral("y"), static_cast<qint64>(point.y - canvas.y));
                points.push_back(item);
            }
            value.insert(QStringLiteral("points"), points);
        } else if (annotation.kind == Annotation::Kind::Text) {
            const bool number = isNumberAnnotation(annotation);
            value.insert(QStringLiteral("kind"), QStringLiteral("text"));
            // Every text mark names its tool, not only a badge.  The reader
            // requires one -- a mark without a `tool` is refused, and a refusal
            // fails the whole document rather than the one mark -- so leaving it
            // off a plain label did not merely lose the label: it made every pin
            // carrying one impossible to reopen, marks and all.
            value.insert(QStringLiteral("tool"), annotation.tool);
            if (number) {
                value.insert(QStringLiteral("numberStyle"), numberStyleValue(annotation.numberStyle));
                value.insert(QStringLiteral("numberSize"),
                             static_cast<qint64>(annotation.numberSize));
            } else {
                value.insert(QStringLiteral("textPixels"),
                             static_cast<qint64>(annotation.textPixels));
            }
            QJsonObject origin;
            origin.insert(QStringLiteral("x"), static_cast<qint64>(annotation.origin.x - canvas.x));
            origin.insert(QStringLiteral("y"), static_cast<qint64>(annotation.origin.y - canvas.y));
            value.insert(QStringLiteral("origin"), origin);
            value.insert(QStringLiteral("text"),
                         number ? QString::number(annotation.number) : annotation.text);
            value.insert(QStringLiteral("color"), colorText(annotation.color));
            if (!annotation.font.isEmpty()) {
                value.insert(QStringLiteral("font"), annotation.font);
            }
        } else if (annotation.kind == Annotation::Kind::Image) {
            // A pasted image is its pixels: there is no smaller form that
            // rebuilds it, so the pixels travel.  They are written beside the
            // session and named by path -- a pasted screenshot is megabytes, and
            // this document goes over a newline-delimited socket to the daemon,
            // where inline data would be a single enormous line.
            //
            // Without a directory to write into the mark is left out, which is
            // what a caller that only wants the geometry gets.
            const QString path = writeMarkAsset(directory, annotation, &asset);
            if (path.isEmpty()) {
                continue;
            }
            value.insert(QStringLiteral("kind"), QStringLiteral("image"));
            value.insert(QStringLiteral("tool"), annotation.tool);
            value.insert(QStringLiteral("pixels"), path);
            // The pixels' own size, which is not the rect's: the rect is where
            // the image is placed and how much it was scaled to fit, and the
            // reader needs both to rebuild the mark and to resize it later
            // without going back through a scaled copy.
            value.insert(QStringLiteral("pixel_width"),
                         static_cast<qint64>(annotation.pixels.width()));
            value.insert(QStringLiteral("pixel_height"),
                         static_cast<qint64>(annotation.pixels.height()));
            QJsonObject rect;
            rect.insert(QStringLiteral("x"), static_cast<qint64>(annotation.rect.x - canvas.x));
            rect.insert(QStringLiteral("y"), static_cast<qint64>(annotation.rect.y - canvas.y));
            rect.insert(QStringLiteral("width"), static_cast<qint64>(annotation.rect.width));
            rect.insert(QStringLiteral("height"), static_cast<qint64>(annotation.rect.height));
            value.insert(QStringLiteral("rect"), rect);
        } else if (annotation.kind == Annotation::Kind::Translation) {
            // A translation is its lines, which are numbers and strings and
            // travel in the document itself; it needs no asset.  It is written
            // as its own kind so the reader can tell it from a shape, and its
            // boxes are relative to the canvas like every other rect.
            value.insert(QStringLiteral("kind"), QStringLiteral("translation"));
            value.insert(QStringLiteral("tool"), annotation.tool);
            if (!annotation.font.isEmpty()) {
                value.insert(QStringLiteral("font"), annotation.font);
            }
            QJsonArray lines;
            for (const TranslatedLine &line : annotation.translation) {
                QJsonObject entry;
                entry.insert(QStringLiteral("source"), rectJson(line.source, canvas));
                entry.insert(QStringLiteral("fill"), rectJson(line.fill, canvas));
                entry.insert(QStringLiteral("text"), line.text);
                entry.insert(QStringLiteral("family"), line.family);
                entry.insert(QStringLiteral("font_pixels"), static_cast<qint64>(line.fontPixels));
                entry.insert(QStringLiteral("background"), colorText(line.background));
                entry.insert(QStringLiteral("text_color"), colorText(line.textColor));
                lines.push_back(entry);
            }
            value.insert(QStringLiteral("lines"), lines);
            QJsonObject rect;
            rect.insert(QStringLiteral("x"), static_cast<qint64>(annotation.rect.x - canvas.x));
            rect.insert(QStringLiteral("y"), static_cast<qint64>(annotation.rect.y - canvas.y));
            rect.insert(QStringLiteral("width"), static_cast<qint64>(annotation.rect.width));
            rect.insert(QStringLiteral("height"), static_cast<qint64>(annotation.rect.height));
            value.insert(QStringLiteral("rect"), rect);
        } else {
            continue;
        }
        marks.push_back(value);
    }
    return marks;
}

// One mark's pixels as a file in `directory`, or an empty string when there is
// nowhere to write them.  The name is built from the mark's own place in the
// document rather than from its content: two identical pastes are two marks, and
// naming them by content would make one file serve both and lose the fact that
// they are separate things the user can move apart.
QString OverlayController::writeMarkAsset(const QString &directory, const Annotation &annotation,
                                          int *counter) const
{
    if (directory.isEmpty() || annotation.pixels.isNull() || counter == nullptr) {
        return QString();
    }
    const QString name = QStringLiteral("mark-%1.png").arg(++*counter);
    const QString path = QDir(directory).filePath(name);
    if (!annotation.pixels.save(path, "PNG")) {
        return QString();
    }
    return path;
}

bool OverlayController::produceComposite(QImage *composite, QImage *marks, QString *error) const
{
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (composite == nullptr) {
        return fail(uiTr("There is nowhere to put the rendered capture."));
    }
    if (!selection_.has_value()) {
        // A picking session never frames anything, and a cancelled one has no
        // marks worth rendering: neither is a failure.
        return false;
    }
    const int index = outputContaining(*selection_);
    if (index < 0 || index >= session_.outputs.size()) {
        return fail(uiTr("The selection is on no output."));
    }
    const OutputSession &output = session_.outputs.at(index);
    if (output.image.isNull()) {
        return fail(uiTr("The captured frame is not available."));
    }
    const double density = outputScale(output);
    // The same crop the translate mode writes: the selection in the output's
    // own device pixels, which is the size the result is.
    const QRect source = sourceRect(output, *selection_)
                             .intersected(QRect(0, 0, output.image.width(), output.image.height()));
    if (source.isEmpty()) {
        return fail(uiTr("The selection has no pixels on this output."));
    }
    const QImage crop = output.image.copy(source);
    if (crop.isNull()) {
        return fail(uiTr("The selection's pixels could not be read."));
    }
    const QImage rendered = compositeAnnotations(crop, crop, *selection_, density, error);
    if (rendered.isNull()) {
        return false;
    }
    *composite = rendered;
    if (marks != nullptr) {
        // The marks on their own: the same painter over a transparent canvas,
        // still reading the capture for the marks that sample it.  An opaque
        // image cannot be composited onto the HDR half -- it would replace the
        // light rather than mark it -- so the CLI needs the layer, not the
        // flattened result.
        //
        // Cleared explicitly: a `QImage` built from a size alone owns
        // uninitialized bytes, and a layer that started as whatever the last
        // allocation held would composite a translucent mark over noise.
        QImage blank(crop.size(), QImage::Format_ARGB32_Premultiplied);
        if (blank.isNull()) {
            return fail(uiTr("The capture's pixels could not be converted for drawing."));
        }
        blank.fill(Qt::transparent);
        const QImage layer =
            compositeAnnotations(blank, crop, *selection_, density, error);
        if (layer.isNull()) {
            return false;
        }
        *marks = layer;
    }
    return true;
}

void OverlayController::paintLiveStroke(QPainter *painter, const OutputSession &output,
                                        const QSize &size, int outputIndex)
{
    if (outputIndex != gesture_->liveOutput || gesture_->points.isEmpty()) {
        return;
    }
    const bool brush = tool_ == Tool::Mosaic;
    const QString liveTool = tool_.has_value() ? toolName(*tool_) : QStringLiteral("pen");
    const int widthLogical = std::max(1, static_cast<int>(toolStyle(liveTool).width));
    const double scale = outputScale(output);
    const double deviceRadius = brush
        ? std::clamp(brushRadiusForStrength(
                         mosaicStrength_,
                         std::clamp(static_cast<int>(widthLogical * scale / 2), 1, 512)),
                     1, 512)
        : 0.0;
    const double padding = liveStrokeMargin(brush, widthLogical, scale, mosaicStrength_);
    const double step = std::max(1.0, deviceRadius / 2.0);
    // The preview raster holds device pixels for the same reason the committed
    // rasters do; see `AnnotationRaster::deviceRatio`.
    const qreal ratio = AnnotationRaster::deviceRatio(painter);

    // A change of style, output or surface size invalidates the whole raster;
    // the growing point list deliberately does not, which is the point.
    QByteArray key;
    {
        QDataStream stream(&key, QIODevice::WriteOnly);
        const LogicalRect &surface = surfaceOf(output);
        const ToolStyle &liveStyle = toolStyle(liveTool);
        stream << size.width() << size.height() << output.id << output.scale << surface.x
               << surface.y << surface.width << surface.height << liveTool
               << static_cast<quint32>(liveStyle.color.rgba()) << liveStyle.width << currentDash_
               << mosaicStrength_ << mosaicShape_ << ratio;
    }
    if (key != gesture_->liveKey) {
        gesture_->liveKey = key;
        gesture_->liveRaster = QImage();
        gesture_->liveOrigin = QPoint();
        gesture_->liveBaked = 1;
        gesture_->liveLength = 0.0;
    }

    const int count = gesture_->points.size();
    // The local-space room the segments added since the last paint need.
    QRectF fresh;
    for (int i = gesture_->liveBaked; i < count; ++i) {
        const QPointF a = localPoint(output, gesture_->points.at(i - 1), size);
        const QPointF b = localPoint(output, gesture_->points.at(i), size);
        const QRectF segment = QRectF(a, b).normalized();
        fresh = fresh.isNull() ? segment : fresh.united(segment);
    }
    const QRect needed = fresh.isNull()
        ? QRect()
        : fresh.adjusted(-padding, -padding, padding, padding)
              .toAlignedRect()
              .intersected(QRect(QPoint(0, 0), size));

    if (needed.isEmpty()) {
        // Nothing new is visible on this overlay, but the path length still has
        // to advance so a later visible segment's dash phase lines up.
        for (int i = gesture_->liveBaked; i < count; ++i) {
            const QPointF a = localPoint(output, gesture_->points.at(i - 1), size);
            const QPointF b = localPoint(output, gesture_->points.at(i), size);
            gesture_->liveLength += std::hypot(b.x() - a.x(), b.y() - a.y());
        }
        liveStrokeBakes_ += std::max(0, count - gesture_->liveBaked);
        gesture_->liveBaked = count;
        if (!gesture_->liveRaster.isNull()) {
            painter->drawImage(gesture_->liveOrigin, gesture_->liveRaster);
        }
        return;
    }

    // Grow the raster only when the stroke reaches past it: the old pixels are
    // copied into the larger image at their original offset.  The image holds
    // device pixels, so the logical rect it covers is its size over the ratio.
    const QRect current(gesture_->liveOrigin,
                        (QSizeF(gesture_->liveRaster.size()) / ratio).toSize());
    if (gesture_->liveRaster.isNull()) {
        gesture_->liveRaster = QImage((QSizeF(needed.size()) * ratio).toSize(),
                                      QImage::Format_ARGB32_Premultiplied);
        gesture_->liveRaster.setDevicePixelRatio(ratio);
        gesture_->liveRaster.fill(Qt::transparent);
        gesture_->liveOrigin = needed.topLeft();
    } else if (!current.contains(needed)) {
        const QRect grown = current.united(needed);
        QImage resized((QSizeF(grown.size()) * ratio).toSize(),
                       QImage::Format_ARGB32_Premultiplied);
        resized.setDevicePixelRatio(ratio);
        resized.fill(Qt::transparent);
        {
            // Both images carry the same device-pixel ratio, so the copy is
            // drawn in logical coordinates by a painter that already has the
            // ratio's transform; scaling here would place the old pixels at
            // twice their offset and leave most of the grown raster empty.
            QPainter copy(&resized);
            copy.drawImage(current.topLeft() - grown.topLeft(), gesture_->liveRaster);
        }
        gesture_->liveRaster = resized;
        gesture_->liveOrigin = grown.topLeft();
    }

    {
        QPainter raster(&gesture_->liveRaster);
        raster.setRenderHint(QPainter::Antialiasing, true);
        // No `scale(ratio, ratio)`, for the same reason as the committed
        // rasters: `setDevicePixelRatio` is the transform, and a second one
        // would draw the stroke at twice its size.
        raster.translate(-gesture_->liveOrigin);
        if (brush) {
            for (int i = gesture_->liveBaked; i < count; ++i) {
                stampMosaicSegment(&raster, output, gesture_->points.at(i - 1),
                                   gesture_->points.at(i), i == 1, static_cast<int>(deviceRadius),
                                   static_cast<double>(scale), step, size);
            }
        } else {
            Annotation style;
            style.kind = Annotation::Kind::Stroke;
            style.tool = liveTool;
            const ToolStyle &strokeStyle = toolStyle(style.tool);
            style.color = strokeStyle.color;
            style.width = strokeStyle.width;
            style.dash = currentDash_;
            const QPen pen = penForAnnotation(style);
            const bool dashed = currentDash_ != QStringLiteral("solid");
            for (int i = gesture_->liveBaked; i < count; ++i) {
                const QPointF a = localPoint(output, gesture_->points.at(i - 1), size);
                const QPointF b = localPoint(output, gesture_->points.at(i), size);
                QPen segmentPen = pen;
                if (dashed) {
                    // Continue the dash pattern where the previous segment left
                    // it (`dashOffset` is measured in pen widths).  A solid pen
                    // must not be touched: setDashOffset would turn it into a
                    // custom-dash pen with no pattern, which draws nothing.
                    segmentPen.setDashOffset(gesture_->liveLength / widthLogical);
                }
                raster.setPen(segmentPen);
                raster.drawLine(a, b);
                gesture_->liveLength += std::hypot(b.x() - a.x(), b.y() - a.y());
            }
        }
    }
    liveStrokeBakes_ += std::max(0, count - gesture_->liveBaked);
    gesture_->liveBaked = count;
    painter->drawImage(gesture_->liveOrigin, gesture_->liveRaster);
}

void OverlayController::drawLoupe(CaptureOverlay *overlay, QPainter *painter)
{
    const OutputSession &output = overlay->output();
    const QPointF local = localPoint(output, pointer_, overlay->size());
    // The loupe reads the *picture*: the capture with the marks the user has
    // already made painted onto it.  Reading the bare frame -- as this once did
    // -- made the magnifier the one place on screen that disagreed with
    // everything else, which is the opposite of what it is for: it is a
    // pixel-level reading of what is being annotated, so a mark that is on the
    // screen has to be in it.  The frame is still what the sample is *taken*
    // from in the sense that matters -- the composite is a copy of it, never
    // the frame itself, so nothing here can reach the saved picture.
    const QImage *frame = loupeFrame(output);
    const int sourceWidth = frame != nullptr ? frame->width() : output.image.width();
    const int sourceHeight = frame != nullptr ? frame->height() : output.image.height();
    if (sourceWidth <= 0 || sourceHeight <= 0 || frame == nullptr) {
        return;
    }
    // The magnifier samples the *image*, so it has to count from where the image
    // actually is: in the pin editor that is the daemon-confirmed rect, not the
    // rect the session recorded when the editor opened, and counting from the
    // stale one made the loupe show the wrong part of the picture while a pin
    // was being dragged -- the "scrambled magnifier".
    const LogicalRect &image = pinEdit_ && marksOrigin_.has_value() ? *marksOrigin_ : output.geometry;
    // The frame holds device pixels and the pointer is in logical ones, so the
    // offset has to cross the output's scale -- the number it is, not a rounded
    // one: the pin editor's output is zoomed, and a scale truncated to 0 would
    // pin every sample to the frame's first pixel.
    const double scale = outputScale(output);
    // A region capture's frame is the *whole* frozen output, not the selection
    // the user framed, so counting from the output's origin alone would let the
    // loupe read the desktop beside the capture.  What the magnifier is for is
    // the pixels of the picture being annotated, so the sample is held inside
    // that picture: a cursor past its edge reads the edge pixel, which is the
    // same thing the loupe does at the frame's own border.  In the pin editor
    // the frame *is* the picture, and the two are the same rect.
    //
    // Only a *committed* picture bounds the sample.  While the user is still
    // framing the selection, `selection_` is the rect being dragged into
    // existence -- a press and a move to the same point leaves it one pixel
    // across -- and holding the loupe inside that would pin every sample to a
    // single pixel for the whole drag, which is the one time the magnifier is
    // read most.  Until the selection is made, the frame is the picture.
    const bool bounded = !pinEdit_ && editing_ && selection_.has_value();
    const LogicalRect &picture = bounded ? *selection_ : image;
    const int minX = std::clamp(static_cast<int>(std::floor((picture.x - image.x) * scale)), 0,
                                sourceWidth - 1);
    const int minY = std::clamp(static_cast<int>(std::floor((picture.y - image.y) * scale)), 0,
                                sourceHeight - 1);
    const int maxX = std::clamp(
        static_cast<int>(std::ceil((picture.x + static_cast<std::int64_t>(picture.width) - image.x) *
                                   scale)) -
            1,
        minX, sourceWidth - 1);
    const int maxY = std::clamp(
        static_cast<int>(std::ceil((picture.y + static_cast<std::int64_t>(picture.height) - image.y) *
                                   scale)) -
            1,
        minY, sourceHeight - 1);
    const int centerX =
        std::clamp(static_cast<int>(std::floor((pointer_.x - image.x) * scale)), minX, maxX);
    const int centerY =
        std::clamp(static_cast<int>(std::floor((pointer_.y - image.y) * scale)), minY, maxY);
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
    // The crop is the fixed window the loupe magnifies, centred on the cursor's
    // own pixel.  Near a screen edge that window runs off the frame; sampling
    // only the part still inside leaves the rest of the circle showing whatever
    // is underneath, which is the "scrambled magnifier".  Replicating the edge
    // pixels instead keeps the cursor's pixel dead centre and the whole circle
    // filled, which is the one thing the magnifier is read for.
    const QImage crop = loupeCrop(*frame, centerX, centerY);
    painter->drawImage(QRectF(center.x() - radius, center.y() - radius, 2.0 * radius,
                              2.0 * radius),
                       crop);
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
    // The pixel's own colour, as the picker's own reading of it.  The circle
    // and the pill sample the same pixel, so what the pill says is what the
    // magnifier shows.
    const QColor pixelColor = frame->pixelColor(centerX, centerY);
    painter->restore();
    // The pill hangs just under the loupe.  When there is no room below -- a
    // cursor near the bottom edge -- hanging it "above the anchor" would drop it
    // back inside the circle and cover the very pixels the magnifier exists to
    // show, so it is lifted clear over the top of the loupe instead.
    QPointF pillAnchor = center + QPointF(0, radius + 2.0);
    const int pillHeight = QFontMetrics(pillFont()).height() + 8;
    if (pillAnchor.y() + 10.0 + pillHeight > overlay->height()) {
        pillAnchor.setY(center.y() - radius - 12.0);
    }
    // Under the eyedropper the readout is the pixel itself, chip and hex: that
    // is the value the click is about to take, and a pair of numbers says
    // nothing about it.  Everywhere else the loupe is there to place a corner,
    // and the numbers are what is wanted.
    const bool picking = colorPickerVisible() && pixelColor.isValid();
    if (picking) {
        // The colour readout goes on the other side of the loupe from the
        // coordinates, so the two never overlap and neither covers the circle.
        QPointF colourAnchor = center + QPointF(0, radius + 2.0);
        if (pillAnchor.y() > center.y()) {
            colourAnchor = center + QPointF(0, radius + 2.0 + pillHeight + 2.0);
            if (colourAnchor.y() + 10.0 + 2 * pillHeight > overlay->height()) {
                colourAnchor.setY(center.y() - radius - 12.0);
            }
        } else {
            colourAnchor.setY(center.y() - radius - 12.0);
        }
        drawColorPill(overlay, painter, colourAnchor, pixelColor);
    } else {
        drawInfoPill(painter, pillAnchor, coordinates,
                     QRectF(0, 0, overlay->width(), overlay->height()));
    }
}

// The picker's readout: the pixel's code on a swatch of the pixel itself, with
// the keys that act on it on a line of their own below.
//
// Two lines rather than one, because the two halves are not the same kind of
// thing.  The top line is a *reading* -- it is the colour, shown, and it has to
// change as the cursor moves or the user is reading a swatch of where the
// pointer used to be -- while the bottom line is a fixed hint about the keys.
// Run together they made the swatch as wide as the hint, which put a band of
// the sampled colour across the screen; split, the swatch is only as wide as
// the code it prints.
//
// The text on the swatch is black or white, whichever the swatch's own
// luminance is further from: a fixed white would vanish on a white pixel, which
// is exactly the pixel a colour picker gets pointed at.
void OverlayController::drawColorPill(CaptureOverlay *overlay, QPainter *painter,
                                      const QPointF &anchor, const QColor &color)
{
    const QRectF bounds(0, 0, overlay->width(), overlay->height());
    const QFontMetrics metrics(pillFont());
    const QString code = color.name(QColor::HexRgb).toUpper();
    const QString hint = QStringLiteral("%1  %2")
                             .arg(shortcutHint(ShortcutAction::CopyColor, uiTr("copy")),
                                  shortcutHint(ShortcutAction::AdoptColor, uiTr("use")));

    const int codeWidth = metrics.horizontalAdvance(code);
    const int hintWidth = metrics.horizontalAdvance(hint);
    const int width = std::max(codeWidth, hintWidth) + 16;
    const int lineHeight = metrics.height() + 4;
    const int height = 2 * lineHeight;

    qreal x = anchor.x() - width / 2.0;
    x = std::clamp(x, bounds.left() + 2.0,
                   std::max(bounds.left() + 2.0, bounds.right() - width - 2.0));
    qreal y = anchor.y() + 10.0;
    if (y + height > bounds.bottom()) {
        y = anchor.y() - height - 10.0;
    }
    y = std::clamp(y, bounds.top() + 2.0,
                   std::max(bounds.top() + 2.0, bounds.bottom() - height - 2.0));
    const QRectF pill(x, y, width, height);

    // The swatch is opaque whatever the pixel's alpha: the readout is the
    // colour as it appears on screen, and the compositor has already put it
    // over whatever was behind it.
    QColor ground = color;
    ground.setAlpha(255);
    // Rec. 601 luma, which is what "how bright does this look" means for a
    // background: at 0.299/0.587/0.114 the crossover sits at the point where
    // black and white text are equally readable, so the choice is never a
    // guess.  The ink is pure black or pure white rather than a tint, because
    // the code has to stay legible at pill size.
    const double luma = (0.299 * ground.redF() + 0.587 * ground.greenF() +
                         0.114 * ground.blueF()) *
        255.0;
    const QColor ink = luma > 140.0 ? QColor(0, 0, 0) : QColor(255, 255, 255);

    painter->setFont(pillFont());
    painter->setPen(Qt::NoPen);
    painter->setBrush(ground);
    painter->drawRoundedRect(pill, 4, 4);
    painter->setPen(QPen(QColor(120, 120, 120), 1.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(pill.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);

    painter->setPen(ink);
    painter->drawText(QRectF(pill.left(), pill.top(), pill.width(), lineHeight),
                      Qt::AlignCenter, code);
    // The hint keeps the pill's dark ground rather than the swatch: it is a
    // caption, and a caption on the sampled colour would move and change
    // contrast every time the cursor did.
    const QRectF hintRect(pill.left(), pill.top() + lineHeight, pill.width(), lineHeight);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(20, 20, 20, 225));
    painter->drawRect(hintRect);
    painter->setPen(QPen(QColor(120, 120, 120), 1.0));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(hintRect.adjusted(0.5, 0.0, -0.5, -0.5));
    painter->setPen(Qt::white);
    painter->drawText(hintRect, Qt::AlignCenter, hint);
}

// The frame the magnifier and the colour readout sample: the frozen image of
// the output, which is what the overlay is showing.
const QImage *OverlayController::outputFrame(const OutputSession &output) const
{
    return output.image.isNull() ? nullptr : &output.image;
}

QByteArray OverlayController::loupeCompositeKey(const OutputSession &output) const
{
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    // The frame itself, and where it sits.  `geometry` is here rather than in
    // the marks' own caches: the composite is a picture of a *place*, so
    // sliding the pin's image under the marks has to move the marks with it
    // and a stale composite would leave them where they were.
    stream << output.id << output.image.cacheKey() << output.geometry.x << output.geometry.y
           << output.geometry.width << output.geometry.height << output.scale
           << (pinEdit_ && marksOrigin_.has_value());
    if (pinEdit_ && marksOrigin_.has_value()) {
        stream << marksOrigin_->x << marksOrigin_->y << marksOrigin_->width
               << marksOrigin_->height;
    }
    stream << annotations_.size();
    // Every mark's own identity, as the raster caches compute it: a mark that
    // has not changed contributes the same bytes, and one that has contributes
    // different ones.  This is what makes a style change or a drag rebuild the
    // composite without the editor having to remember to say so.
    for (const Annotation &annotation : annotations_) {
        if (annotation.raster == nullptr) {
            annotation.raster = makeAnnotationRaster(annotation);
        }
        stream << annotation.raster->key(annotation, output, output.image.size());
    }
    return key;
}

void OverlayController::invalidateLoupeComposite()
{
    loupeCompositeKey_.clear();
    loupeComposite_ = QImage();
    loupeCompositeOutput_ = -1;
}

const QImage *OverlayController::loupeFrame(const OutputSession &output)
{
    const QImage *frame = outputFrame(output);
    if (frame == nullptr) {
        return nullptr;
    }
    if (annotations_.isEmpty()) {
        // Nothing has been drawn, so the picture *is* the frame.  The loupe
        // then reads the capture's own pixels, which is the one reading that
        // cannot be wrong.
        return frame;
    }
    const QByteArray key = loupeCompositeKey(output);
    if (!loupeComposite_.isNull() && key == loupeCompositeKey_ &&
        static_cast<uint32_t>(loupeCompositeOutput_) == output.id) {
        return &loupeComposite_;
    }
    // Where the frame sits on the screen.  In the pin editor that is the
    // daemon-confirmed rect rather than the session's own record of it, which
    // is what the rest of the editor counts from too; the two agree once the
    // daemon has answered, and before that the confirmed one is the one the
    // picture is actually drawn at.
    const LogicalRect picture =
        pinEdit_ && marksOrigin_.has_value() ? *marksOrigin_ : output.geometry;
    // The frame's own size, so the composite is the picture pixel for pixel: a
    // mark lands on exactly the device pixels it lands on in the render.  The
    // cost is that a mark overhanging the picture is cut off at its edge --
    // which is also what the screen does, so the two agree.
    QImage composite(frame->size(), QImage::Format_ARGB32_Premultiplied);
    if (composite.isNull()) {
        return frame;
    }
    composite.fill(Qt::transparent);
    {
        QPainter painter(&composite);
        painter.drawImage(0, 0, *frame);
        // The marks were measured against the picture, so this is the record
        // they are drawn through: `surface` is the picture's own rect and the
        // size handed to the painter is its device size, which together make
        // every global coordinate land on the frame's own pixels.  The pixels
        // stay the *frame's*, not the composite's, so a mosaic samples the
        // capture rather than the marks already painted over it -- exactly as
        // the render does.
        OutputSession placed;
        placed.id = output.id;
        placed.scale = output.scale;
        placed.geometry = picture;
        placed.surface = picture;
        placed.image = *frame;
        for (const Annotation &annotation : annotations_) {
            if (annotation.raster == nullptr) {
                annotation.raster = makeAnnotationRaster(annotation);
            }
            annotation.raster->drawInto(&painter, annotation, placed, composite.size());
        }
    }
    loupeComposite_ = composite;
    loupeCompositeKey_ = key;
    loupeCompositeOutput_ = static_cast<int>(output.id);
    return &loupeComposite_;
}

bool OverlayController::magnifierVisible() const
{
    return magnifierHeld_ || magnifierTyped_;
}

bool CaptureOverlay::magnifierVisible() const
{
    return controller_ != nullptr && controller_->magnifierVisible();
}

bool CaptureOverlay::colorPickerVisible() const
{
    return controller_ != nullptr && controller_->colorPickerVisible();
}

bool OverlayController::colorPickerVisible() const
{
    // Only the right button's magnifier.  The loupe a drag brings up, and the
    // one a keyboard step flashes, are coordinate readouts for placing a mark:
    // they are there to say *where* the cursor is, and the colour under it is
    // not what the user is aiming at.  The right button is the one press whose
    // whole purpose is the pixel, so it is the one that carries the picker.
    return magnifierHeld_;
}

void OverlayController::flashMagnifier()
{
    magnifierTyped_ = true;
    if (magnifierTimer_ == nullptr) {
        magnifierTimer_ = new QTimer(this);
        magnifierTimer_->setSingleShot(true);
        QObject::connect(magnifierTimer_, &QTimer::timeout, this,
                         [this] { endMagnifierFlash(); });
    }
    // Two seconds: long enough to read a coordinate and step again, short
    // enough that a frame left up by a stray key press goes away on its own.
    magnifierTimer_->start(2000);
}

void OverlayController::endMagnifierFlash()
{
    if (!magnifierTyped_) {
        return;
    }
    magnifierTyped_ = false;
    if (magnifierTimer_ != nullptr) {
        magnifierTimer_->stop();
    }
    if (!magnifierHeld_) {
        updateAll();
    }
}

// The pixel the magnifier is centred on, in the output's own device pixels.
// The image pixel under the cursor is what the user is aiming at: the pill's
// coordinates, the hex code, and the circle all read from this one place, so
// they cannot disagree about which pixel is "under the cursor".
QColor OverlayController::pixelUnderCursor(int *pixelIndexX, int *pixelIndexY) const
{
    if (pixelIndexX != nullptr) {
        *pixelIndexX = -1;
    }
    if (pixelIndexY != nullptr) {
        *pixelIndexY = -1;
    }
    if (pointerOutput_ < 0 || pointerOutput_ >= session_.outputs.size()) {
        return QColor();
    }
    const OutputSession &output = session_.outputs.at(pointerOutput_);
    const QImage *frame = outputFrame(output);
    if (frame == nullptr || frame->isNull()) {
        return QColor();
    }
    const double scale = outputScale(output);
    const LogicalRect &image =
        pinEdit_ && marksOrigin_.has_value() ? *marksOrigin_ : output.geometry;
    const int x = std::clamp(
        static_cast<int>(std::floor((pointer_.x - image.x) * scale)), 0, frame->width() - 1);
    const int y = std::clamp(
        static_cast<int>(std::floor((pointer_.y - image.y) * scale)), 0, frame->height() - 1);
    if (pixelIndexX != nullptr) {
        *pixelIndexX = x;
    }
    if (pixelIndexY != nullptr) {
        *pixelIndexY = y;
    }
    return frame->pixelColor(x, y);
}

void OverlayController::copyColorUnderCursor()
{
    const QColor color = pixelUnderCursor(nullptr, nullptr);
    if (!color.isValid()) {
        return;
    }
    // The code as it is written in a stylesheet or a config file: the same
    // `#RRGGBB` the pill prints, so the copy and the readout cannot disagree.
    if (!writeClipboard(color.name(QColor::HexRgb).toUpper())) {
        std::fprintf(stderr, "vshot-qt-ui: could not copy the colour code\n");
        std::fflush(stderr);
    }
}

void OverlayController::adoptColorUnderCursor()
{
    const QColor color = pixelUnderCursor(nullptr, nullptr);
    if (!color.isValid()) {
        return;
    }
    // Only where there is a palette to take it: a tool with no colour of its
    // own -- the mosaic, whose look is its strength -- has nothing to set, and
    // silently restyling some other tool would be worse than doing nothing.
    const QString target = styleTargetTool();
    if (target == QStringLiteral("mosaic")) {
        return;
    }
    // Opaque, because the pixel is: the alpha a tool happens to be carrying is
    // the user's choice about the ink, not about this colour.
    QColor adopted = color;
    adopted.setAlpha(255);
    setCurrentColor(adopted);
}

void OverlayController::copyToClipboard()
{
    QImage composite;
    QString error;
    if (!produceComposite(&composite, nullptr, &error)) {
        // A session that has framed nothing has nothing to copy, and that is
        // not a failure worth a diagnostic; anything else is.
        if (!error.isEmpty()) {
            std::fprintf(stderr, "vshot-qt-ui: %s\n", error.toUtf8().constData());
            std::fflush(stderr);
        }
        return;
    }
    if (!runWlCopyImage(composite)) {
        std::fprintf(stderr, "vshot-qt-ui: could not copy the capture\n");
        std::fflush(stderr);
    }
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
    const LogicalRect &surface = surfaceOf(output());
    resize(static_cast<int>(surface.width), static_cast<int>(surface.height));
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

// Wayland has no "no input here" request: an unset input region means the whole
// surface is interactive, and Qt sends no request at all for an empty mask,
// which is exactly that default.  A region parked outside the surface is the
// portable way to say "click straight through", the same trick `PinSurface`
// uses.
//
// The mask is a round trip to the compositor and a pin drag recomputes it every
// frame, so an unchanged one is not re-sent.
void CaptureOverlay::setInputMask(const QRegion &mask)
{
    static const QRegion clickThrough(QRect(-8, -8, 1, 1));
    const QRegion wanted = mask.isEmpty() ? clickThrough : mask;
    if (wanted == inputMask_) {
        return;
    }
    winId();
    QWindow *window = windowHandle();
    if (window == nullptr) {
        return;
    }
    inputMask_ = wanted;
    window->setMask(wanted);
}

bool CaptureOverlay::showLayerSurface()
{    if (layerWindow_ == nullptr) {
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
    raise();
    // Exclusive keyboard interactivity only takes effect once the surface is
    // actually activated; without activateWindow() the compositor never routes
    // key events here and Escape/Enter/arrow keys are all silently dead.
    activateWindow();
    setFocus(Qt::OtherFocusReason);
    return true;
}

void CaptureOverlay::detachController()
{
    controller_ = nullptr;
    hide();
}

void CaptureOverlay::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    if (controller_ == nullptr) {
        return;
    }
    QPainter painter(this);
    controller_->paint(this, &painter);
}

void CaptureOverlay::mousePressEvent(QMouseEvent *event)
{
    if (controller_ != nullptr) {
        controller_->press(this, event->position(), event->button(), event->modifiers());
    }
    event->accept();
}

void CaptureOverlay::mouseMoveEvent(QMouseEvent *event)
{
    if (controller_ != nullptr) {
        controller_->move(this, event->position(), event->buttons(), event->modifiers());
    }
    event->accept();
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (controller_ != nullptr) {
        controller_->release(this, event->position(), event->button(), event->modifiers());
    }
    event->accept();
}

void CaptureOverlay::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (controller_ != nullptr) {
        controller_->doubleClick(this, event->position(), event->button());
    }
    event->accept();
}

void CaptureOverlay::keyPressEvent(QKeyEvent *event)
{
    if (controller_ != nullptr) {
        controller_->key(this, event->key(), event->modifiers());
    }
    event->accept();
}

void CaptureOverlay::closeEvent(QCloseEvent *event)
{
    // The compositor (or a stray close request) must not hang the Rust side:
    // treat an externally closed overlay as a cancelled session.
    if (controller_ != nullptr) {
        controller_->cancel();
    }
    event->accept();
}

void CaptureOverlay::wantKeyboard()
{
    if (layerWindow_ == nullptr || keyboardWanted_) {
        return;
    }
    auto *layer = LayerShellQt::Window::get(layerWindow_);
    if (layer == nullptr) {
        return;
    }
    keyboardWanted_ = true;
    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
    // The interactivity change only reaches the compositor with the next commit,
    // so the keyboard is asked for now rather than at some later repaint.
    layerWindow_->requestUpdate();
}

void CaptureOverlay::offerKeyboardBack()
{
    if (layerWindow_ == nullptr || !keyboardWanted_) {
        return;
    }
    auto *layer = LayerShellQt::Window::get(layerWindow_);
    if (layer == nullptr) {
        return;
    }
    keyboardWanted_ = false;
    // `None`, not `OnDemand`: the surface covers a whole output and would
    // otherwise still take the keyboard whenever the compositor felt like giving
    // it one.  The pointer coming back is what asks for it again.
    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
    layerWindow_->requestUpdate();
}

void CaptureOverlay::enterEvent(QEnterEvent *event)
{
    wantKeyboard();
    QWidget::enterEvent(event);
}

void CaptureOverlay::leaveEvent(QEvent *event)
{
    setCursor(Qt::CrossCursor);
    // The pointer leaving is the user having gone elsewhere, so the keyboard
    // goes back with it.  Without this the surface holds it for as long as it is
    // mapped -- the compositor routes every key to whichever surface has it, so
    // no other window could be typed into while the editor was open.
    offerKeyboardBack();
    QWidget::leaveEvent(event);
}

} // namespace vshot

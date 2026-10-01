// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// Offline checks for the annotation render cache and the freehand preview.
//
// Every committed mark keeps a rasterized copy of itself and redraws it only
// when something it draws changes.  That matters most for the mosaic, which
// averages the source image block by block: recomputing it on every repaint
// (a selection drag, a pointer move) is what made a busy capture stutter.  The
// check paints the real overlay offscreen and reads the per-mark rebuild count,
// so what is asserted is the cache the editor actually uses.
//
// The in-progress freehand stroke has the same problem in a different shape:
// re-stroking the whole path on every move is quadratic over a long scribble.
// It builds up through a raster that only grows by the points added since the
// last paint, and the check proves each segment is baked once while the result
// still matches the mark that is committed on release.
//
// Needs QApplication and the offscreen platform plugin; no compositor and no
// layer shell.  `QT_QPA_PLATFORM=offscreen` supplies the one screen the overlay
// is parented to.
//
// Built only with `-DVSHOT_BUILD_CHECKS=ON`; see the README's verification
// section.

#include "capture_overlay.hpp"
#include "config.hpp"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPainter>
#include <QPointF>
#include <QPointer>
#include <QRegion>
#include <QScreen>
#include <QString>
#include <QTemporaryDir>
#include <QWindow>
#include <Qt>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <QThread>
#include <unistd.h>

namespace {

int failures = 0;

// The pen tool's own numbers, spelled out here rather than imported from the
// overlay, the way the other checks spell out the ink colour they assert on.
// The path is three anchors whose handles run along the edges of the triangle
// they make, so the closed shape is that triangle with every side bowed outward
// and its middle is a large, plainly-inside area.  It sits low on the 1600x1200
// canvas of `largeSession`, well clear of the floating toolbar.
//
// kPenAlpha is the pen opacity the checks set; the fill is the stroke's colour
// at half that alpha, floored -- the overlay's own rule, spelled out here so a
// change to it cannot pass by moving both sides of the assertion.
constexpr int kPenAlpha = 200;
constexpr int kPenFillAlpha = kPenAlpha / 2;
constexpr int kPenRed = 255;
constexpr int kPenGreen = 30;
constexpr int kPenBlue = 30;
constexpr int kPenAnchorCount = 3;
const vshot::Point kPenAnchors[] = {{400, 800}, {1200, 800}, {800, 1100}};
const vshot::Point kPenHandles[] = {{640, 800}, {1080, 890}, {680, 1010}};
// A point deep inside the triangle the anchors make, far from every edge and
// every corner: where the closed path's fill is read, and where an open path has
// to leave the frame alone.
const QPoint kPenInside(800, 900);

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

QPointF penAnchor(int index)
{
    return QPointF(kPenAnchors[index].x, kPenAnchors[index].y);
}

QPointF penHandle(int index)
{
    return QPointF(kPenHandles[index].x, kPenHandles[index].y);
}

// The colour a source at `alpha` over `background` composites to, in the check's
// own arithmetic: premultiplied SourceOver, which is what the painter does.  The
// assertions below compare the rendered pixel against this rather than against a
// constant, so they say "half the stroke's alpha" and not "this exact byte".
QColor over(const QColor &source, int alpha, const QColor &background)
{
    const double a = alpha / 255.0;
    return QColor(qRound(source.red() * a + background.red() * (1.0 - a)),
                  qRound(source.green() * a + background.green() * (1.0 - a)),
                  qRound(source.blue() * a + background.blue() * (1.0 - a)));
}

// The largest channel difference between two colours.
int colorDistance(const QColor &first, const QColor &second)
{
    return std::max({std::abs(first.red() - second.red()),
                     std::abs(first.green() - second.green()),
                     std::abs(first.blue() - second.blue())});
}

// A one-output region session whose selection is already made, so the editor
// opens with the toolbar up.  The output carries real pixels: the mosaic reads
// them, so an empty frame would cache an empty raster and prove nothing.
vshot::Session editingSession()
{
    vshot::Session session;
    session.mode = QStringLiteral("region");
    session.bounds = vshot::LogicalRect{0, 0, 400, 400};
    vshot::OutputSession output;
    output.id = 1;
    output.name = QStringLiteral("CHECK-1");
    output.geometry = vshot::LogicalRect{0, 0, 400, 400};
    output.surface = output.geometry;
    output.scale = 1;
    output.pixelWidth = 400;
    output.pixelHeight = 400;
    output.image = QImage(400, 400, QImage::Format_ARGB32);
    // A coarse pattern rather than a flat fill, so the mosaic and the brush have
    // something to average: a uniform frame would hide a wrong sample point.
    for (int y = 0; y < 400; ++y) {
        for (int x = 0; x < 400; ++x) {
            output.image.setPixelColor(x, y,
                                       QColor(40 + (x / 8 * 7) % 60, 30 + (y / 8 * 53) % 200,
                                              30 + ((x + y) / 8 * 29) % 200));
        }
    }
    session.outputs.push_back(output);
    session.selection = vshot::LogicalRect{0, 0, 400, 400};
    return session;
}

// A one-output session whose every pixel names itself, so a magnified sample
// can be read back and checked against the pixel it should be showing.  No
// selection: the check drives one itself.
vshot::Session coordinateSession()
{
    vshot::Session session = editingSession();
    session.selection.reset();
    vshot::OutputSession &output = session.outputs[0];
    QImage image(400, 400, QImage::Format_RGBA8888);
    for (int y = 0; y < 400; ++y) {
        for (int x = 0; x < 400; ++x) {
            image.setPixelColor(x, y, QColor(x % 256, y % 256, (x * 7 + y * 13) % 256, 255));
        }
    }
    output.image = image;
    return session;
}

// A one-output session whose frame is solid black, so a green label's ink can be
// read back with a low threshold and compared between two renderings of it.
vshot::Session blackSession()
{
    vshot::Session session = editingSession();
    session.outputs[0].image.fill(QColor(0, 0, 0));
    return session;
}

// The same frame at twice the density: the magnifier reads device pixels, so a
// scaled output exercises a different crop from the logical one.
vshot::Session scaledCoordinateSession()
{
    vshot::Session session = editingSession();
    session.selection.reset();
    vshot::OutputSession &output = session.outputs[0];
    output.scale = 2;
    output.pixelWidth = 800;
    output.pixelHeight = 800;
    QImage image(800, 800, QImage::Format_RGBA8888);
    for (int y = 0; y < 800; ++y) {
        for (int x = 0; x < 800; ++x) {
            image.setPixelColor(x, y, QColor(x % 256, y % 256, (x * 7 + y * 13) % 256, 255));
        }
    }
    output.image = image;
    return session;
}

void paintOnce(vshot::CaptureOverlay *overlay, QImage *target)
{
    target->fill(Qt::transparent);
    overlay->render(target);
}

// One step of what the compositor actually asks the overlay for: Qt repaints
// only the region the previous step invalidated, on top of the pixels that are
// already there.  The check above full-renders after every step and so can
// never see a gap the narrow path leaves; this one keeps the pixels between
// steps, exactly like the live widget, and redraws just the asked-for rect.
void renderIncremental(vshot::OverlayController &controller, vshot::CaptureOverlay *overlay,
                       QImage *backing)
{
    const QRect claimed = controller.lastInteractiveUpdate();
    if (claimed.isNull() || claimed.isEmpty()) {
        // A full repaint is its own eraser: repaint everything, as the widget
        // would when the step asked for the whole surface.
        paintOnce(overlay, backing);
        return;
    }
    QPainter painter(backing);
    painter.setClipRect(claimed);
    overlay->render(&painter, QPoint(0, 0));
    painter.end();
}

// Pixels the two images disagree on, ignoring the areas the overlay's child
// widgets (the floating toolbar, the inline text editor) paint themselves.
int differingPixelsOutside(const QRegion &ignore, const QImage &first, const QImage &second)
{
    int diff = 0;
    for (int y = 0; y < first.height(); ++y) {
        for (int x = 0; x < first.width(); ++x) {
            if (ignore.contains(QPoint(x, y))) {
                continue;
            }
            const QColor a = first.pixelColor(x, y);
            const QColor b = second.pixelColor(x, y);
            if (std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()),
                          std::abs(a.blue() - b.blue())}) > 30) {
                ++diff;
            }
        }
    }
    return diff;
}

// Draws a rectangle drag with the current tool.
void drag(vshot::OverlayController &controller, vshot::CaptureOverlay *overlay,
          const QPointF &from, const QPointF &to)
{
    controller.press(overlay, from, Qt::LeftButton, Qt::NoModifier);
    controller.move(overlay, to, Qt::LeftButton, Qt::NoModifier);
    controller.release(overlay, to, Qt::LeftButton, Qt::NoModifier);
}

// The area the overlay's live child widgets cover, in overlay coordinates.  The
// floating toolbar and its buttons repaint themselves when their own state
// changes -- Qt drives that, not the controller's invalidated region -- so a
// difference under a child is not something the controller's rect can be blamed
// for.
QRegion childAreas(vshot::CaptureOverlay *overlay)
{
    QRegion areas;
    for (QWidget *child : overlay->findChildren<QWidget *>()) {
        if (child->isWindow() || !child->isVisible()) {
            continue;
        }
        areas += QRect(child->mapTo(overlay, QPoint()), child->size());
    }
    return areas;
}

// One interactive step: full-render the overlay, run the step, full-render it
// again, and prove every pixel the step changed lies inside the rect the step
// asked to be repainted.  A null rect means the step repainted the whole
// surface, so there is nothing to compare -- a full repaint is its own eraser
// and needs no narrow region to be correct.
void expectStepCoveredBy(vshot::OverlayController &controller, vshot::CaptureOverlay *overlay,
                         const std::function<void()> &step, const char *gesture)
{
    QImage before(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &before);
    step();
    QImage after(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &after);
    const QRect claimed = controller.lastInteractiveUpdate();
    if (claimed.isNull()) {
        return;
    }
    const QRegion allowed = QRegion(claimed) + childAreas(overlay);
    int outside = 0;
    for (int y = 0; y < after.height(); ++y) {
        for (int x = 0; x < after.width(); ++x) {
            if (before.pixel(x, y) != after.pixel(x, y) && !allowed.contains(QPoint(x, y))) {
                ++outside;
            }
        }
    }
    expect(outside == 0, gesture,
           QStringLiteral("%1 px changed outside the invalidated region").arg(outside));
}

// A pointer move with the left button down, which is the step most gestures are
// made of.
void expectStepCovered(vshot::OverlayController &controller, vshot::CaptureOverlay *overlay,
                       const QPointF &to, const char *gesture)
{
    expectStepCoveredBy(
        controller, overlay,
        [&] { controller.move(overlay, to, Qt::LeftButton, Qt::NoModifier); }, gesture);
}

// The same, for a pointer move with nothing held down.  The pen's rubber band is
// the one thing that follows those, and driving it as a drag would bend the
// curve instead of moving the band.
void expectHoverCovered(vshot::OverlayController &controller, vshot::CaptureOverlay *overlay,
                        const QPointF &to, const char *gesture)
{
    expectStepCoveredBy(controller, overlay,
                        [&] { controller.move(overlay, to, Qt::NoButton, Qt::NoModifier); },
                        gesture);
}

// Opens a `largeSession` overlay in edit state, ready to draw on.  The pen paths
// below sit low on its 1600x1200 canvas, clear of the floating toolbar.
bool openLargeOverlay(vshot::OverlayController &controller, vshot::CaptureOverlay **overlay)
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return false;
    }
    QString error;
    *overlay = controller.addOverlay(0, screen, &error);
    if (*overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return false;
    }
    (*overlay)->show();
    controller.beginPresetEdit();
    return true;
}

// Draws the pen path the checks below describe.  `closed` ends it by pressing
// back onto the first anchor, which fills it; otherwise it ends on a double
// click, whose first click places the last anchor and whose second arrives as a
// double-click event in place of a press -- exactly how Qt delivers the two
// clicks of a double click.
void drawPenPath(vshot::OverlayController &controller, vshot::CaptureOverlay *overlay, bool closed)
{
    controller.chooseTool(vshot::Tool::Bezier);
    const int dragged = closed ? kPenAnchorCount : kPenAnchorCount - 1;
    for (int index = 0; index < dragged; ++index) {
        controller.press(overlay, penAnchor(index), Qt::LeftButton, Qt::NoModifier);
        controller.move(overlay, penHandle(index), Qt::LeftButton, Qt::NoModifier);
        controller.release(overlay, penHandle(index), Qt::LeftButton, Qt::NoModifier);
    }
    if (closed) {
        controller.press(overlay, penAnchor(0), Qt::LeftButton, Qt::NoModifier);
        controller.release(overlay, penAnchor(0), Qt::LeftButton, Qt::NoModifier);
        return;
    }
    const QPointF last = penAnchor(kPenAnchorCount - 1);
    controller.press(overlay, last, Qt::LeftButton, Qt::NoModifier);
    controller.release(overlay, last, Qt::LeftButton, Qt::NoModifier);
    controller.doubleClick(overlay, last, Qt::LeftButton);
    controller.release(overlay, last, Qt::LeftButton, Qt::NoModifier);
}

// A committed mark rasterizes once and a repaint that changes nothing reuses
// it; moving it or restyling it is what rebuilds it.
void checkRepaintsReuseTheRaster()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();

    controller.chooseTool(vshot::Tool::Mosaic);
    drag(controller, overlay, QPointF(40, 40), QPointF(180, 140));
    expect(controller.annotations().size() == 1, "the mosaic lands as one annotation");
    if (controller.annotations().size() != 1) {
        return;
    }

    QImage target(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &target);
    expect(controller.annotations().at(0).rasterRebuilds() == 1,
           "the first paint rasterizes the mark",
           QStringLiteral("rebuilds=%1").arg(controller.annotations().at(0).rasterRebuilds()));
    paintOnce(overlay, &target);
    paintOnce(overlay, &target);
    expect(controller.annotations().at(0).rasterRebuilds() == 1,
           "repaints that change nothing reuse the cached raster",
           QStringLiteral("rebuilds=%1").arg(controller.annotations().at(0).rasterRebuilds()));

    // Moving the mark changes the source blocks it averages, so it must redraw.
    controller.chooseTool(std::nullopt);
    drag(controller, overlay, QPointF(110, 90), QPointF(150, 115));
    paintOnce(overlay, &target);
    expect(controller.annotations().at(0).rasterRebuilds() == 2,
           "moving the mark rebuilds its raster",
           QStringLiteral("rebuilds=%1").arg(controller.annotations().at(0).rasterRebuilds()));
    paintOnce(overlay, &target);
    expect(controller.annotations().at(0).rasterRebuilds() == 2,
           "the moved mark then settles into the cache",
           QStringLiteral("rebuilds=%1").arg(controller.annotations().at(0).rasterRebuilds()));

    // Restyling the selected mark changes what it draws, so it must redraw too.
    const std::uint32_t strength = controller.annotations().at(0).strength;
    controller.setMosaicStrength(strength == 3u ? 1u : 3u);
    paintOnce(overlay, &target);
    expect(controller.annotations().at(0).rasterRebuilds() == 3,
           "a style change rebuilds the raster",
           QStringLiteral("rebuilds=%1").arg(controller.annotations().at(0).rasterRebuilds()));
}

// Each mark owns its own cache: drawing a second mark leaves the first one's
// raster alone.
void checkEachMarkCachesOnItsOwn()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();

    controller.chooseTool(vshot::Tool::Mosaic);
    drag(controller, overlay, QPointF(40, 40), QPointF(140, 110));
    QImage target(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &target);
    expect(controller.annotations().size() == 1 &&
               controller.annotations().at(0).rasterRebuilds() == 1,
           "the first mark rasterizes once");

    controller.chooseTool(vshot::Tool::Pen);
    drag(controller, overlay, QPointF(220, 220), QPointF(300, 280));
    expect(controller.annotations().size() == 2, "the pen stroke lands as a second annotation");
    if (controller.annotations().size() != 2) {
        return;
    }
    paintOnce(overlay, &target);
    expect(controller.annotations().at(0).rasterRebuilds() == 1,
           "drawing a second mark leaves the first one's raster cached");
    expect(controller.annotations().at(1).rasterRebuilds() == 1,
           "the new mark rasterizes on its own");
    paintOnce(overlay, &target);
    expect(controller.annotations().at(0).rasterRebuilds() == 1 &&
               controller.annotations().at(1).rasterRebuilds() == 1,
           "both marks stay cached across further repaints");
}

// A cached raster still lands where the mark is: painting a bright stroke over
// a plain frame leaves bright pixels on the stroke, both on the paint that
// rasterizes it and on the repaint that blits the cache.
void checkCachedPixelsLandOnTheMark()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();

    controller.chooseTool(vshot::Tool::Pen);
    controller.setWidth(6);
    controller.setCurrentColor(QColor(255, 30, 30));
    drag(controller, overlay, QPointF(60, 200), QPointF(340, 200));
    const auto hasStrokePixels = [](const QImage &image) {
        for (int y = 180; y < 220; ++y) {
            for (int x = 40; x < 360; ++x) {
                if (x >= image.width() || y >= image.height()) {
                    continue;
                }
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.red() > 180 && pixel.green() < 120 && pixel.blue() < 120) {
                    return true;
                }
            }
        }
        return false;
    };
    QImage target(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &target);
    expect(hasStrokePixels(target), "the rasterized stroke paints where it was drawn");
    paintOnce(overlay, &target);
    expect(hasStrokePixels(target), "the cached stroke blits to the same place");
}

// Opens an overlay on `screen` in edit state and draws one pen stroke with the
// given colour, so both the mark's own colour and the document's spelling of it
// can be read back.  Returns false when the stroke did not land.
bool drawStrokeWithColor(vshot::OverlayController &controller, vshot::CaptureOverlay **overlay,
                         QScreen *screen, const QColor &color)
{
    QString error;
    *overlay = controller.addOverlay(0, screen, &error);
    if (*overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return false;
    }
    (*overlay)->show();
    controller.beginPresetEdit();
    controller.chooseTool(vshot::Tool::Pen);
    controller.setWidth(6);
    controller.setCurrentColor(color);
    drag(controller, *overlay, QPointF(60, 120), QPointF(340, 120));
    expect(controller.annotations().size() == 1, "the pen stroke lands as one annotation");
    return controller.annotations().size() == 1;
}

// The colour the picker hands the editor has to survive into the committed mark
// and out through the marks document.  Every place that writes an annotation's
// colour into that document goes through `colorText`, and the document is what
// the daemon keeps and hands back -- so a colour that lost its alpha between the
// picker and the JSON comes back wrong the next time the mark is edited, and
// the exact string is pinned because it is read back with `#rrggbbaa`, alpha
// last: a channel dropped, swapped or put in the wrong place changes which
// colour comes out.
void checkTranslucentColorSerializesWithAlpha()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    vshot::CaptureOverlay *overlay = nullptr;
    // A colour with no two channels equal and an alpha that is neither 0 nor
    // 255, so no single dropped or reordered channel can hide in the string.
    if (!drawStrokeWithColor(controller, &overlay, screen, QColor(17, 34, 204, 128))) {
        return;
    }
    const vshot::Annotation &mark = controller.annotations().at(0);
    expect(mark.color.alpha() == 128,
           "the committed stroke keeps the alpha the current colour carried",
           QStringLiteral("alpha=%1").arg(mark.color.alpha()));

    const QJsonArray marks = controller.marksDocument();
    expect(marks.size() == 1, "the document carries the stroke");
    if (marks.isEmpty()) {
        return;
    }
    const QString color = marks.at(0).toObject().value(QStringLiteral("color")).toString();
    expect(color == QStringLiteral("#1122cc80"),
           "a translucent colour serializes as #rrggbbaa, alpha in the last two digits",
           QStringLiteral("got %1").arg(color));
}

// The other half of the same contract: an opaque colour keeps the six-digit
// spelling, so adding support for alpha did not turn every colour in the
// document into eight digits.
void checkOpaqueColorSerializesWithoutAlpha()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    vshot::CaptureOverlay *overlay = nullptr;
    if (!drawStrokeWithColor(controller, &overlay, screen, QColor(17, 34, 204, 255))) {
        return;
    }
    expect(controller.annotations().at(0).color.alpha() == 255,
           "an opaque stroke stays fully opaque");

    const QJsonArray marks = controller.marksDocument();
    if (marks.isEmpty()) {
        expect(false, "the document carries the stroke");
        return;
    }
    const QString color = marks.at(0).toObject().value(QStringLiteral("color")).toString();
    expect(color == QStringLiteral("#1122cc"),
           "an opaque colour serializes as #rrggbb, not eight digits",
           QStringLiteral("got %1").arg(color));
}

// A serpentine of many moves, so the live raster is baked in many steps.
QVector<QPointF> serpentine()
{
    QVector<QPointF> path;
    for (int i = 0; i < 80; ++i) {
        const double t = i / 79.0;
        path.append(QPointF(30 + t * 340, 200 + std::sin(t * 18.0) * 70));
    }
    return path;
}

// Paints the stroke as the editor does -- one paint per move -- and keeps both
// the in-progress image and the committed one.
void renderFreehand(vshot::OverlayController &controller, vshot::CaptureOverlay *overlay,
                    const QVector<QPointF> &path, QImage *live, QImage *committed)
{
    *live = QImage(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    controller.press(overlay, path.constFirst(), Qt::LeftButton, Qt::NoModifier);
    for (int i = 1; i < path.size(); ++i) {
        controller.move(overlay, path.at(i), Qt::LeftButton, Qt::NoModifier);
        paintOnce(overlay, live);
    }
    controller.release(overlay, path.constLast(), Qt::LeftButton, Qt::NoModifier);
    *committed = QImage(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, committed);
}

int differingPixels(const QImage &first, const QImage &second)
{
    int diff = 0;
    for (int y = 0; y < first.height(); ++y) {
        for (int x = 0; x < first.width(); ++x) {
            const QColor a = first.pixelColor(x, y);
            const QColor b = second.pixelColor(x, y);
            if (std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()),
                          std::abs(a.blue() - b.blue())}) > 30) {
                ++diff;
            }
        }
    }
    return diff;
}

// The incremental preview must draw the same stroke as the committed mark, so
// letting go changes nothing on screen.  The live raster is baked one move at a
// time; only antialiasing at the shared joints differs, so the tolerance is a
// few pixels per vertex rather than none.
void checkLiveStrokeMatchesTheCommittedMark()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    const QVector<QPointF> path = serpentine();
    const int tolerance = 8 * path.size();

    const auto open = [&](vshot::OverlayController &controller, vshot::CaptureOverlay **overlay) {
        QString error;
        *overlay = controller.addOverlay(0, screen, &error);
        if (*overlay == nullptr) {
            expect(false, "the controller accepts an overlay", error);
            return false;
        }
        (*overlay)->show();
        controller.beginPresetEdit();
        return true;
    };

    {
        vshot::OverlayController controller(editingSession());
        vshot::CaptureOverlay *overlay = nullptr;
        if (!open(controller, &overlay)) {
            return;
        }
        controller.chooseTool(vshot::Tool::Pen);
        controller.setWidth(5);
        controller.setCurrentColor(QColor(255, 30, 30));
        QImage live;
        QImage committed;
        renderFreehand(controller, overlay, path, &live, &committed);
        const int diff = differingPixels(live, committed);
        expect(diff < tolerance, "the incremental preview matches the committed solid stroke",
               QStringLiteral("%1 pixels differ").arg(diff));
        expect(controller.liveStrokeBakes() == path.size() - 1,
               "each freehand segment is baked once, not on every paint",
               QStringLiteral("baked %1 for %2 segments")
                   .arg(controller.liveStrokeBakes())
                   .arg(path.size() - 1));
    }

    {
        vshot::OverlayController controller(editingSession());
        vshot::CaptureOverlay *overlay = nullptr;
        if (!open(controller, &overlay)) {
            return;
        }
        controller.chooseTool(vshot::Tool::Pen);
        controller.setWidth(5);
        controller.setCurrentColor(QColor(255, 30, 30));
        controller.setDash(QStringLiteral("dashed"));
        QImage live;
        QImage committed;
        renderFreehand(controller, overlay, path, &live, &committed);
        const int diff = differingPixels(live, committed);
        expect(diff < tolerance, "the incremental preview matches the committed dashed stroke",
               QStringLiteral("%1 pixels differ").arg(diff));
    }

    {
        vshot::OverlayController controller(editingSession());
        vshot::CaptureOverlay *overlay = nullptr;
        if (!open(controller, &overlay)) {
            return;
        }
        controller.chooseTool(vshot::Tool::Mosaic);
        controller.setMosaicShape(QStringLiteral("brush"));
        controller.setWidth(24);
        QImage live;
        QImage committed;
        renderFreehand(controller, overlay, path, &live, &committed);
        const int diff = differingPixels(live, committed);
        expect(diff < tolerance, "the incremental preview matches the committed mosaic brush",
               QStringLiteral("%1 pixels differ").arg(diff));
    }
}

// The configured default tool belongs to *annotation* mode, and a fresh region
// capture has a step before that: framing.  Arming the tool at the open made
// the region's very first press draw a mark instead of starting the rectangle,
// which reads as "region capture is broken" -- the frame never appears and the
// click inks a shape onto a live desktop.  The tool is not dropped: it is armed
// the moment the frame is finished, which is when annotation mode really
// begins.
//
// Both halves are asserted here, because either one alone is a different bug:
// unarmed at the open is what keeps framing working, and armed at
// `finishSelection` is what keeps the setting from doing nothing.
void checkTheDefaultToolIsArmedWhenAnnotationBegins()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    // The config the controller reads at construction, written the way the
    // settings window would leave it.
    const QString path = vshot::configFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        expect(false, "the probe config is written");
        return;
    }
    file.write(QByteArrayLiteral(R"({"editor": {"tool": "ellipse"}})"));
    file.close();

    // A region session with no selection: the state a fresh capture is in, where
    // the first press has to start the frame.
    {
        vshot::Session session = editingSession();
        session.selection.reset();
        vshot::OverlayController controller(std::move(session));
        QString error;
        vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
        if (overlay == nullptr) {
            expect(false, "the controller accepts an overlay", error);
            file.remove();
            return;
        }
        overlay->show();

        // Nothing armed yet, so the drag frames.  `finishSelection` is reached
        // through the press/move/release a real framing drag makes.
        drag(controller, overlay, QPointF(60, 60), QPointF(300, 260));
        expect(controller.selection().has_value(),
               "the region's first drag frames rather than drawing");
        expect(controller.annotations().isEmpty(), "and it leaves no mark behind",
               QStringLiteral("%1 mark(s)").arg(controller.annotations().size()));
        // The configured tool is *not* armed now the frame is made.  Opening
        // armed here would make the very next click ink, so the one gesture a
        // user makes after framing -- adjusting the frame they just drew --
        // would put a mark down instead.
        expect(controller.armedToolName().isEmpty(),
               "the frame being done does not arm the configured tool",
               controller.armedToolName());
        const auto framed = *controller.selection();
        // The next drag re-frames, and leaves nothing behind: the same drag
        // that made the first frame makes the second.
        drag(controller, overlay, QPointF(100, 100), QPointF(200, 180));
        expect(controller.annotations().isEmpty(),
               "an unarmed drag over a fresh frame re-frames rather than inking",
               QStringLiteral("%1 mark(s)").arg(controller.annotations().size()));
        expect(controller.selection().has_value() && controller.selection()->x == 100 &&
                   controller.selection()->y == 100 && controller.selection()->width <= 101 &&
                   controller.selection()->height <= 81 && controller.selection()->width >= 100 &&
                   controller.selection()->height >= 80,
               "and the frame it draws is the one the drag described",
               controller.selection().has_value()
                   ? QStringLiteral("%1,%2 %3x%4")
                         .arg(controller.selection()->x)
                         .arg(controller.selection()->y)
                         .arg(controller.selection()->width)
                         .arg(controller.selection()->height)
                   : QStringLiteral("no frame"));
        Q_UNUSED(framed);
    }

    // A session that opens with its selection already made -- the window
    // picker's follow-up and a pin re-entered for editing -- has no frame left
    // to drag, so it is the one case that does open on the configured tool.
    {
        vshot::Session session = editingSession();
        vshot::OverlayController controller(std::move(session));
        QString error;
        vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
        if (overlay == nullptr) {
            expect(false, "the controller accepts a preset-edit overlay", error);
            file.remove();
            return;
        }
        overlay->show();
        controller.beginPresetEdit();
        expect(controller.armedToolName() == QStringLiteral("ellipse"),
               "a session that opens with a frame arms the configured tool",
               controller.armedToolName());
        // And the frame it opened on is the one it keeps: arming must not
        // re-frame the capture.
        const auto framed = *controller.selection();
        drag(controller, overlay, QPointF(100, 100), QPointF(200, 180));
        expect(controller.annotations().size() == 1,
               "and the drag draws with it rather than framing again",
               QStringLiteral("%1 mark(s)").arg(controller.annotations().size()));
        if (controller.annotations().size() == 1) {
            expect(controller.annotations().constFirst().tool == QStringLiteral("ellipse"),
                   "and the mark is the tool the config named",
                   controller.annotations().constFirst().tool);
        }
        expect(controller.selection()->x == framed.x && controller.selection()->y == framed.y &&
                   controller.selection()->width == framed.width &&
                   controller.selection()->height == framed.height,
               "arming the tool does not move the frame the session opened on");
    }
    file.remove();
}

// A pure translation must not invalidate a cached raster: the mark's pixels do
// not change, only where they are blitted.  The mosaic is the deliberate
// exception -- it averages the source image under its absolute position -- and
// checkRepaintsReuseTheRaster keeps that pinned down.  A moved mark that kept a
// stale blit position would show up here as pixels left at the old place.
void checkPureMoveReusesTheRaster()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();

    QImage target(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    // The session frame is a coarse coloured pattern that can itself contain
    // red-ish pixels, so the stroke uses pure green -- a channel combination
    // the pattern (red >= 40, blue >= 30) can never produce -- and the position
    // test looks for that.
    const auto greenIn = [](const QImage &image, int from, int to) {
        for (int y = from; y < to; ++y) {
            for (int x = 40; x < 360; ++x) {
                if (x >= image.width() || y >= image.height()) {
                    continue;
                }
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.red() < 20 && pixel.green() > 200 && pixel.blue() < 20) {
                    return true;
                }
            }
        }
        return false;
    };

    // A freehand stroke: it must keep its raster when dragged, and the pixels
    // must land at the new place rather than staying behind.  The press point
    // is off the mark's edge handles so the drag translates instead of resizing.
    controller.chooseTool(vshot::Tool::Pen);
    controller.setWidth(6);
    controller.setCurrentColor(QColor(0, 255, 0));
    drag(controller, overlay, QPointF(60, 120), QPointF(340, 120));
    expect(controller.annotations().size() == 1, "the stroke lands as one annotation");
    if (controller.annotations().size() != 1) {
        return;
    }
    paintOnce(overlay, &target);
    const int strokeRebuilds = controller.annotations().at(0).rasterRebuilds();
    expect(strokeRebuilds == 1, "the stroke rasterizes once",
           QStringLiteral("rebuilds=%1").arg(strokeRebuilds));
    controller.chooseTool(std::nullopt);
    drag(controller, overlay, QPointF(150, 120), QPointF(150, 220));
    paintOnce(overlay, &target);
    expect(controller.annotations().at(0).rasterRebuilds() == strokeRebuilds,
           "translating a freehand stroke reuses its cached raster",
           QStringLiteral("rebuilds=%1").arg(controller.annotations().at(0).rasterRebuilds()));
    expect(greenIn(target, 200, 240) && !greenIn(target, 100, 140),
           "the translated stroke paints at its new place, not the old one");

    // A rectangle outline: same cache, same translation.
    controller.chooseTool(vshot::Tool::Rectangle);
    controller.setWidth(4);
    controller.setCurrentColor(QColor(30, 200, 30));
    drag(controller, overlay, QPointF(40, 300), QPointF(180, 380));
    expect(controller.annotations().size() == 2, "the rectangle lands as a second annotation");
    if (controller.annotations().size() != 2) {
        return;
    }
    paintOnce(overlay, &target);
    const int rectRebuilds = controller.annotations().at(1).rasterRebuilds();
    expect(rectRebuilds == 1, "the rectangle rasterizes once",
           QStringLiteral("rebuilds=%1").arg(rectRebuilds));
    controller.chooseTool(std::nullopt);
    drag(controller, overlay, QPointF(110, 340), QPointF(210, 340));
    paintOnce(overlay, &target);
    expect(controller.annotations().at(1).rasterRebuilds() == rectRebuilds,
           "translating a rectangle reuses its cached raster",
           QStringLiteral("rebuilds=%1").arg(controller.annotations().at(1).rasterRebuilds()));

    // A text label: its bitmap depends on the text and font, not on where the
    // label sits, so a move reuses it too.
    controller.chooseTool(vshot::Tool::Text);
    controller.press(overlay, QPointF(60, 60), Qt::LeftButton, Qt::NoModifier);
    QLineEdit *editor = overlay->findChild<QLineEdit *>();
    expect(editor != nullptr, "the text tool opens its inline editor");
    if (editor == nullptr) {
        return;
    }
    editor->setText(QStringLiteral("Hi"));
    controller.key(overlay, Qt::Key_Return, Qt::NoModifier);
    expect(controller.annotations().size() == 3, "the label lands as a third annotation");
    if (controller.annotations().size() != 3) {
        return;
    }
    paintOnce(overlay, &target);
    const int textRebuilds = controller.annotations().at(2).rasterRebuilds();
    expect(textRebuilds == 1, "the label rasterizes once",
           QStringLiteral("rebuilds=%1").arg(textRebuilds));
    controller.chooseTool(std::nullopt);
    drag(controller, overlay, QPointF(66, 66), QPointF(166, 66));
    paintOnce(overlay, &target);
    expect(controller.annotations().at(2).rasterRebuilds() == textRebuilds,
           "translating a label reuses its cached raster",
           QStringLiteral("rebuilds=%1").arg(controller.annotations().at(2).rasterRebuilds()));
}

// Two outputs side by side, each with its own frozen frame, so a mark can lie
// across the seam and be painted by both overlays.
vshot::Session twoOutputSession()
{
    vshot::Session session;
    session.mode = QStringLiteral("region");
    session.bounds = vshot::LogicalRect{0, 0, 800, 400};
    for (int index = 0; index < 2; ++index) {
        vshot::OutputSession output;
        output.id = static_cast<std::uint32_t>(index + 1);
        output.name = QStringLiteral("CHECK-%1").arg(index + 1);
        output.geometry = vshot::LogicalRect{index * 400, 0, 400, 400};
        output.surface = output.geometry;
        output.scale = 1;
        output.pixelWidth = 400;
        output.pixelHeight = 400;
        output.image = QImage(400, 400, QImage::Format_ARGB32);
        output.image.fill(QColor(80, 90, 100));
        session.outputs.push_back(output);
    }
    session.selection = vshot::LogicalRect{0, 0, 800, 400};
    return session;
}

// A session that spans two screens paints the same marks on both.  A single
// shared raster would be thrown away and rebuilt every time the paint moved from
// one screen to the other, so each output keeps its own: the second screen
// builds once, and both then stay cached however the repaints alternate.
void checkEachOutputKeepsItsOwnRaster()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(twoOutputSession());
    QString error;
    vshot::CaptureOverlay *first = controller.addOverlay(0, screen, &error);
    vshot::CaptureOverlay *second = controller.addOverlay(1, screen, &error);
    if (first == nullptr || second == nullptr) {
        expect(false, "the controller accepts two overlays", error);
        return;
    }
    first->show();
    second->show();
    controller.beginPresetEdit();

    controller.chooseTool(vshot::Tool::Rectangle);
    controller.setWidth(4);
    controller.setCurrentColor(QColor(255, 30, 30));
    // Straddles the seam at x = 400, so both screens paint it.
    drag(controller, first, QPointF(300, 100), QPointF(500, 300));
    expect(controller.annotations().size() == 1,
           "the straddling rectangle lands as one annotation");
    if (controller.annotations().size() != 1) {
        return;
    }

    QImage firstTarget(first->size(), QImage::Format_ARGB32_Premultiplied);
    QImage secondTarget(second->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(first, &firstTarget);
    paintOnce(second, &secondTarget);
    const int afterBoth = controller.annotations().at(0).rasterRebuilds();
    expect(afterBoth == 2, "each output rasterizes the mark once",
           QStringLiteral("rebuilds=%1").arg(afterBoth));

    // Alternating repaints must not throw either screen's raster away.
    paintOnce(first, &firstTarget);
    paintOnce(second, &secondTarget);
    paintOnce(first, &firstTarget);
    paintOnce(second, &secondTarget);
    expect(controller.annotations().at(0).rasterRebuilds() == afterBoth,
           "repainting both screens keeps both rasters",
           QStringLiteral("rebuilds=%1").arg(controller.annotations().at(0).rasterRebuilds()));
}

// A single roomy output: the edge test has to drag a mark right up against the
// canvas boundary, and a 400x400 session clamps the drag before it gets there.
vshot::Session largeSession()
{
    vshot::Session session;
    session.mode = QStringLiteral("region");
    session.bounds = vshot::LogicalRect{0, 0, 1600, 1200};
    vshot::OutputSession output;
    output.id = 1;
    output.name = QStringLiteral("CHECK-LARGE");
    output.geometry = vshot::LogicalRect{0, 0, 1600, 1200};
    output.surface = output.geometry;
    output.scale = 1;
    output.pixelWidth = 1600;
    output.pixelHeight = 1200;
    output.image = QImage(1600, 1200, QImage::Format_ARGB32);
    output.image.fill(QColor(80, 90, 100));
    session.outputs.push_back(output);
    session.selection = vshot::LogicalRect{0, 0, 1600, 1200};
    return session;
}

// Sliding a mark up against the edge of the canvas changes how much of it is
// visible, not the pixels it draws, so the raster is not rebuilt: the blit is
// clipped by the painter instead.  Trimming the raster to the canvas would
// change its size as the mark reached the edge, and a size change rebuilds it.
void checkEdgeOfCanvasKeepsTheRaster()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(largeSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();

    QImage target(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    controller.chooseTool(vshot::Tool::Rectangle);
    controller.setWidth(4);
    controller.setCurrentColor(QColor(0, 255, 0));
    // A 70x70 outline landing 30 pixels short of the corner: one drag of thirty
    // brings its far edges exactly onto the canvas boundary, where the pen's half
    // width and the raster's padding reach past it.
    drag(controller, overlay, QPointF(1500, 1100), QPointF(1569, 1169));
    expect(controller.annotations().size() == 1, "the rectangle lands as one annotation");
    if (controller.annotations().size() != 1) {
        return;
    }
    paintOnce(overlay, &target);
    const int settled = controller.annotations().at(0).rasterRebuilds();
    expect(settled == 1, "the rectangle rasterizes once",
           QStringLiteral("rebuilds=%1").arg(settled));

    controller.chooseTool(std::nullopt);
    const vshot::LogicalRect before = controller.annotations().at(0).rect;
    drag(controller, overlay, QPointF(1535, 1135), QPointF(1565, 1165));
    // Back to the drawing tool: the select tool's white outline and handles sit
    // exactly on the edges the colour probes below look at.
    controller.chooseTool(vshot::Tool::Rectangle);
    paintOnce(overlay, &target);
    const vshot::Annotation &mark = controller.annotations().at(0);
    expect(mark.rect.x == before.x + 30 && mark.rect.y == before.y + 30 &&
               mark.rect.width == before.width && mark.rect.height == before.height,
           "the drag moved the mark thirty pixels and changed nothing else",
           QStringLiteral("before=(%1,%2 %3x%4) after=(%5,%6 %7x%8)")
               .arg(before.x)
               .arg(before.y)
               .arg(before.width)
               .arg(before.height)
               .arg(mark.rect.x)
               .arg(mark.rect.y)
               .arg(mark.rect.width)
               .arg(mark.rect.height));
    expect(mark.rect.x + static_cast<std::int32_t>(mark.rect.width) >= 1600,
           "the mark ends up against the canvas edge");
    expect(mark.rasterRebuilds() == settled,
           "a mark pushed against the canvas edge keeps its raster",
           QStringLiteral("rebuilds=%1").arg(mark.rasterRebuilds()));

    // The raster is not trimmed to the canvas, so this is what proves the blit
    // still lands where the mark is: the drag moved it, and its near edges have
    // to be painted at the place it moved to.  The session frame is a flat grey,
    // so a green pixel is the mark's own.
    const auto greenAt = [&target](int x, int y) {
        if (x < 0 || y < 0 || x >= target.width() || y >= target.height()) {
            return false;
        }
        const QColor pixel = target.pixelColor(x, y);
        return pixel.red() < 40 && pixel.green() > 150 && pixel.blue() < 40;
    };
    expect(greenAt(mark.rect.x + 20, mark.rect.y) && greenAt(mark.rect.x, mark.rect.y + 20),
           "the edges of the mark are painted where it now is",
           QStringLiteral("top=%1 left=%2 at (%3,%4)")
               .arg(greenAt(mark.rect.x + 20, mark.rect.y) ? 1 : 0)
               .arg(greenAt(mark.rect.x, mark.rect.y + 20) ? 1 : 0)
               .arg(mark.rect.x)
               .arg(mark.rect.y));
    expect(!greenAt(before.x, before.y + 20) && !greenAt(before.x + 20, before.y),
           "nothing is left where the mark came from");
}

// The narrow repaints must still leave the screen correct: whatever an
// interactive step changed has to lie inside the rect that step asked to be
// repainted.  A rect that is too small leaves stale pixels behind -- a mark at
// the place it came from, a magnifier that outlived the gesture.  The check does
// not model Qt's backing store; it compares a full render of the overlay before
// a step with one after it and counts the changed pixels the invalidated region
// does not cover.
void checkInteractiveUpdateCoversTheChange()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();

    // 1. Drawing a two-point preview: the rectangle is redrawn whole from its
    // anchor on every move, so a step has to cover the outline it drew before as
    // well as the one it draws now.
    controller.chooseTool(vshot::Tool::Rectangle);
    controller.press(overlay, QPointF(60, 60), Qt::LeftButton, Qt::NoModifier);
    expectStepCovered(controller, overlay, QPointF(140, 120),
                      "a rectangle preview move invalidates where it drew");
    expectStepCovered(controller, overlay, QPointF(160, 160),
                      "a growing rectangle preview invalidates where it drew");
    expectStepCovered(controller, overlay, QPointF(150, 140),
                      "a shrinking rectangle preview invalidates where it drew");
    controller.release(overlay, QPointF(150, 140), Qt::LeftButton, Qt::NoModifier);
    expect(controller.annotations().size() == 1, "the rectangle lands as one annotation");
    if (controller.annotations().size() != 1) {
        return;
    }

    // 2. Moving a committed mark: the mark, its white selection chrome and the
    // magnifier all travel, so each step has to cover the old and the new place
    // of all three.
    controller.chooseTool(std::nullopt);
    const vshot::LogicalRect drawn = controller.annotations().at(0).rect;
    const QPointF centre(drawn.x + static_cast<int>(drawn.width) / 2,
                         drawn.y + static_cast<int>(drawn.height) / 2);
    controller.press(overlay, centre, Qt::LeftButton, Qt::NoModifier);
    expectStepCovered(controller, overlay, centre + QPointF(10, 8),
                      "moving a mark invalidates where it drew");
    expectStepCovered(controller, overlay, centre + QPointF(24, 20),
                      "a moving mark invalidates where it draws");
    expectStepCovered(controller, overlay, centre + QPointF(30, 30),
                      "the moved mark invalidates its last place");
    controller.release(overlay, centre + QPointF(30, 30), Qt::LeftButton, Qt::NoModifier);
    const vshot::LogicalRect moved = controller.annotations().at(0).rect;
    expect(moved.x == drawn.x + 30 && moved.y == drawn.y + 30 &&
               moved.width == drawn.width && moved.height == drawn.height,
           "the drag translated the mark by thirty pixels",
           QStringLiteral("from (%1,%2) to (%3,%4)")
               .arg(drawn.x)
               .arg(drawn.y)
               .arg(moved.x)
               .arg(moved.y));

    // 3. Resizing a committed mark: press on the bottom-right handle so the
    // gesture resizes rather than moves, and cover the chrome at both sizes.
    const QPointF corner(moved.x + static_cast<int>(moved.width) - 1,
                         moved.y + static_cast<int>(moved.height) - 1);
    controller.press(overlay, corner, Qt::LeftButton, Qt::NoModifier);
    expectStepCovered(controller, overlay, corner + QPointF(10, 10),
                      "resizing a mark invalidates where it drew");
    expectStepCovered(controller, overlay, corner + QPointF(20, 20),
                      "a resizing mark invalidates where it draws");
    controller.release(overlay, corner + QPointF(20, 20), Qt::LeftButton, Qt::NoModifier);
    const vshot::LogicalRect resized = controller.annotations().at(0).rect;
    expect(resized.width > moved.width && resized.height > moved.height,
           "the drag resized the mark instead of moving it",
           QStringLiteral("%1x%2 -> %3x%4")
               .arg(moved.width)
               .arg(moved.height)
               .arg(resized.width)
               .arg(resized.height));

    // 4. A freehand pen stroke: the preview grows through a raster one segment
    // at a time, and a step may only invalidate the segment it just added.
    controller.chooseTool(vshot::Tool::Pen);
    controller.setWidth(5);
    controller.press(overlay, QPointF(40, 250), Qt::LeftButton, Qt::NoModifier);
    expectStepCovered(controller, overlay, QPointF(80, 255),
                      "a pen stroke move invalidates where it drew");
    expectStepCovered(controller, overlay, QPointF(120, 245),
                      "a growing pen stroke invalidates where it drew");
    expectStepCovered(controller, overlay, QPointF(160, 262),
                      "a turning pen stroke invalidates where it drew");
    expectStepCovered(controller, overlay, QPointF(200, 250),
                      "the released pen stroke invalidates where it drew");
    controller.release(overlay, QPointF(200, 250), Qt::LeftButton, Qt::NoModifier);
    expect(controller.annotations().size() == 2, "the pen stroke lands as a second annotation");
    expect(controller.annotations().size() == 2 &&
               controller.annotations().at(1).tool == QStringLiteral("pen"),
           "the second mark is the pen stroke");

    // 5. A mosaic brush stroke: the growing-raster path again, but each step
    // smears a disc whose radius comes from the strength, not the cursor.
    controller.chooseTool(vshot::Tool::Mosaic);
    controller.setMosaicShape(QStringLiteral("brush"));
    controller.setWidth(24);
    controller.press(overlay, QPointF(270, 60), Qt::LeftButton, Qt::NoModifier);
    expectStepCovered(controller, overlay, QPointF(310, 72),
                      "a mosaic brush move invalidates where it drew");
    expectStepCovered(controller, overlay, QPointF(350, 55),
                      "a growing mosaic brush invalidates where it drew");
    expectStepCovered(controller, overlay, QPointF(370, 80),
                      "the released mosaic brush invalidates where it drew");
    controller.release(overlay, QPointF(370, 80), Qt::LeftButton, Qt::NoModifier);
    expect(controller.annotations().size() == 3, "the mosaic brush lands as a third annotation");
    expect(controller.annotations().size() == 3 &&
               controller.annotations().at(2).tool == QStringLiteral("mosaic"),
           "the third mark is the mosaic brush stroke");

    // 6 & 7. The capture selection itself.  The session's selection is the whole
    // canvas, and `moveSelection` clamps a selection to the canvas, so moving it
    // before it is shrunk would change nothing.  The resize gesture (listed
    // seventh) therefore runs first: shrinking from the top-left corner gives
    // the move gesture (listed sixth) room to travel.  Both drag the selection
    // chrome, the magnifier and the toolbar that follows the selection.
    controller.chooseTool(std::nullopt);
    const vshot::LogicalRect canvas = *controller.selection();
    const QPointF topLeft(canvas.x, canvas.y);
    controller.press(overlay, topLeft, Qt::LeftButton, Qt::NoModifier);
    expectStepCovered(controller, overlay, topLeft + QPointF(50, 50),
                      "resizing the capture selection invalidates where it drew");
    expectStepCovered(controller, overlay, topLeft + QPointF(100, 100),
                      "a resizing capture selection invalidates where it drew");
    controller.release(overlay, topLeft + QPointF(100, 100), Qt::LeftButton, Qt::NoModifier);
    const vshot::LogicalRect shrunk = *controller.selection();
    expect(shrunk.x == canvas.x + 100 && shrunk.y == canvas.y + 100 &&
               shrunk.width + 100 == canvas.width && shrunk.height + 100 == canvas.height,
           "the drag shrank the capture selection from its top-left corner",
           QStringLiteral("(%1,%2 %3x%4) -> (%5,%6 %7x%8)")
               .arg(canvas.x)
               .arg(canvas.y)
               .arg(canvas.width)
               .arg(canvas.height)
               .arg(shrunk.x)
               .arg(shrunk.y)
               .arg(shrunk.width)
               .arg(shrunk.height));

    // A point well inside the shrunk selection, off every handle and off every
    // mark, so the gesture is a move of the selection rather than a resize or a
    // mark pick-up.
    const QPointF grip(350, 350);
    controller.press(overlay, grip, Qt::MiddleButton, Qt::NoModifier);
    expectStepCoveredBy(
        controller, overlay,
        [&] { controller.move(overlay, grip - QPointF(20, 20), Qt::MiddleButton, Qt::NoModifier); },
        "moving the capture selection invalidates where it drew");
    expectStepCoveredBy(
        controller, overlay,
        [&] { controller.move(overlay, grip - QPointF(40, 40), Qt::MiddleButton, Qt::NoModifier); },
        "a moving capture selection invalidates where it drew");
    controller.release(overlay, grip - QPointF(40, 40), Qt::MiddleButton, Qt::NoModifier);
    const vshot::LogicalRect shifted = *controller.selection();
    expect(shifted.x == shrunk.x - 40 && shifted.y == shrunk.y - 40 &&
               shifted.width == shrunk.width && shifted.height == shrunk.height,
           "the drag moved the capture selection without resizing it",
           QStringLiteral("(%1,%2) -> (%3,%4)")
               .arg(shrunk.x)
               .arg(shrunk.y)
               .arg(shifted.x)
               .arg(shifted.y));
}

// The right button brings the magnifier up *without* ending the drag that is
// already under way.  It is how the user aims at a pixel while drawing -- the
// preview follows the cursor, and the loupe rides alongside it -- so a step
// that carried both buttons has to serve both.  It used to return as soon as
// the loupe had been moved, which swallowed every step of the gesture: the
// preview froze at the point the right button went down and only caught up
// once it was released and the pointer moved again.
void checkTheMagnifierDoesNotFreezeTheDragUnderIt()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();

    // A rectangle being drawn, with the right button joining mid-drag.
    controller.chooseTool(vshot::Tool::Rectangle);
    // The picture before anything is drawn on it, so "the preview is here" can
    // be read as "these pixels are not what the capture alone shows".
    QImage clean(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &clean);

    controller.press(overlay, QPointF(60, 60), Qt::LeftButton, Qt::NoModifier);
    controller.move(overlay, QPointF(140, 120), Qt::LeftButton, Qt::NoModifier);
    expect(!overlay->colorPickerVisible(), "the picker is not up before the right button goes down");

    controller.press(overlay, QPointF(140, 120), Qt::RightButton, Qt::NoModifier);
    expect(overlay->colorPickerVisible(), "the right button brings the picker up");

    // Both buttons down: the preview has to keep following the cursor, and the
    // step has to cover the ground between the two places it draws.
    const Qt::MouseButtons both = Qt::LeftButton | Qt::RightButton;
    expectStepCoveredBy(
        controller, overlay,
        [&] { controller.move(overlay, QPointF(200, 160), both, Qt::NoModifier); },
        "a move with the magnifier up still invalidates where the preview drew");
    expectStepCoveredBy(
        controller, overlay,
        [&] { controller.move(overlay, QPointF(260, 200), both, Qt::NoModifier); },
        "a growing preview under the magnifier invalidates where it drew");

    // And the preview really did follow the cursor.  Read from the frame *now*,
    // not from what the release commits: a release that commits the cursor's
    // own position papers over a preview that stood still for the whole drag,
    // and that is exactly the bug.  The corner the pointer last sat on has ink
    // on it, and it is far enough from the loupe -- which hangs off the cursor
    // by more than its own radius -- that none of the disc can account for it.
    QImage live(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &live);
    int ink = 0;
    for (int dy = -3; dy <= 3; ++dy) {
        for (int dx = -3; dx <= 3; ++dx) {
            const int x = 260 + dx;
            const int y = 200 + dy;
            if (x < 0 || y < 0 || x >= live.width() || y >= live.height()) {
                continue;
            }
            if (live.pixel(x, y) != clean.pixel(x, y)) {
                ++ink;
            }
        }
    }
    expect(ink > 0, "the preview reached the corner the cursor was on",
           QStringLiteral("%1 of 49 pixels at 260,200 carry ink").arg(ink));

    controller.release(overlay, QPointF(260, 200), Qt::RightButton, Qt::NoModifier);
    controller.release(overlay, QPointF(260, 200), Qt::LeftButton, Qt::NoModifier);
    expect(controller.annotations().size() == 1, "the rectangle lands as one annotation",
           QString::number(controller.annotations().size()));
}

// The wave's document form is the line's: `kind=stroke` with `tool=wave` and
// exactly the two points the drag made.  A reader rebuilds the wave from those
// two points and derives the crests itself, so a third point -- or any name but
// `wave` -- would come back as a different mark.
void checkWaveSerializesAsATwoPointStroke()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();

    controller.chooseTool(vshot::Tool::Wave);
    controller.setWidth(6);
    controller.setCurrentColor(QColor(255, 30, 30));
    const QPointF start(60, 200);
    const QPointF end(340, 200);
    drag(controller, overlay, start, end);
    expect(controller.annotations().size() == 1, "the wave lands as one annotation");
    if (controller.annotations().size() != 1) {
        return;
    }

    const QJsonArray marks = controller.marksDocument();
    expect(marks.size() == 1, "the document carries the wave");
    if (marks.isEmpty()) {
        return;
    }
    const QJsonObject mark = marks.at(0).toObject();
    expect(mark.value(QStringLiteral("kind")).toString() == QStringLiteral("stroke"),
           "the wave serializes as a stroke",
           mark.value(QStringLiteral("kind")).toString());
    expect(mark.value(QStringLiteral("tool")).toString() == QStringLiteral("wave"),
           "the wave's tool name is `wave`",
           mark.value(QStringLiteral("tool")).toString());
    const QJsonArray points = mark.value(QStringLiteral("points")).toArray();
    expect(points.size() == 2, "the wave carries exactly its two endpoints",
           QStringLiteral("points=%1").arg(points.size()));
    if (points.size() == 2) {
        const QJsonObject first = points.at(0).toObject();
        const QJsonObject last = points.at(1).toObject();
        expect(first.value(QStringLiteral("x")).toInt() == static_cast<int>(start.x()) &&
                   first.value(QStringLiteral("y")).toInt() == static_cast<int>(start.y()) &&
                   last.value(QStringLiteral("x")).toInt() == static_cast<int>(end.x()) &&
                   last.value(QStringLiteral("y")).toInt() == static_cast<int>(end.y()),
               "the two points are the ends of the drag",
               QStringLiteral("(%1,%2)-(%3,%4)")
                   .arg(first.value(QStringLiteral("x")).toInt())
                   .arg(first.value(QStringLiteral("y")).toInt())
                   .arg(last.value(QStringLiteral("x")).toInt())
                   .arg(last.value(QStringLiteral("y")).toInt()));
    }
}

// A numbered badge rides the text annotation all the way out: `kind=text` with
// `tool=number`, its count as a decimal string, and the two numbers a re-edit
// cannot guess -- its style and its diameter.  A badge that lost either would
// still read as "a text annotation" and would only be noticed the second time
// it was opened, when it came back the wrong shape or the wrong size.
void checkNumberSerializesWithItsStyleAndSize()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();

    controller.chooseTool(vshot::Tool::Number);
    // A badge's diameter is a value of its own now; the width slider no longer
    // reaches it, and 36 is what the six-pixel width used to produce.
    controller.setNumberSize(36);
    controller.setCurrentColor(QColor(255, 30, 30));
    const QPointF at(200, 200);
    controller.press(overlay, at, Qt::LeftButton, Qt::NoModifier);
    controller.release(overlay, at, Qt::LeftButton, Qt::NoModifier);
    expect(controller.annotations().size() == 1, "the badge lands as one annotation");
    if (controller.annotations().size() != 1) {
        return;
    }
    expect(controller.annotations().at(0).number == 1, "the first badge counts one",
           QStringLiteral("number=%1").arg(controller.annotations().at(0).number));

    const QJsonArray marks = controller.marksDocument();
    expect(marks.size() == 1, "the document carries the badge");
    if (marks.isEmpty()) {
        return;
    }
    const QJsonObject mark = marks.at(0).toObject();
    expect(mark.value(QStringLiteral("kind")).toString() == QStringLiteral("text"),
           "the badge serializes as a text annotation",
           mark.value(QStringLiteral("kind")).toString());
    expect(mark.value(QStringLiteral("tool")).toString() == QStringLiteral("number"),
           "the badge's tool name is `number`",
           mark.value(QStringLiteral("tool")).toString());
    expect(mark.value(QStringLiteral("text")).toString() == QStringLiteral("1"),
           "the badge's text is its count in decimal",
           mark.value(QStringLiteral("text")).toString());
    expect(mark.value(QStringLiteral("numberSize")).toInt() == 36,
           "the badge carries the diameter it was drawn at",
           QStringLiteral("got %1").arg(mark.value(QStringLiteral("numberSize")).toInt()));
    expect(mark.value(QStringLiteral("numberStyle")).toString() == QStringLiteral("filled_circle"),
           "the badge carries its style",
           mark.value(QStringLiteral("numberStyle")).toString());
}

// The undo comparison treats a badge's count and style as content: two badges at
// the same place that differ only in their number are different marks, and two
// that differ only in their style are too.  Leaving either out of
// `annotationEquals` silently collapses an undo step -- the editor would think
// nothing changed and drop the edit.
void checkNumberBadgesCompareByCountAndStyle()
{
    vshot::Annotation first;
    first.kind = vshot::Annotation::Kind::Text;
    first.tool = QStringLiteral("number");
    first.rect = vshot::LogicalRect{182, 182, 36, 36};
    first.origin = vshot::Point{182, 182};
    first.number = 1;
    first.numberStyle = vshot::NumberStyle::FilledCircle;

    const vshot::Annotation copy = first;
    expect(vshot::annotationEquals(first, copy), "a badge equals a copy of itself");

    vshot::Annotation otherNumber = first;
    otherNumber.number = 2;
    expect(!vshot::annotationEquals(first, otherNumber),
           "two badges at the same place with different counts are not equal");

    vshot::Annotation otherStyle = first;
    otherStyle.numberStyle = vshot::NumberStyle::Ring;
    expect(!vshot::annotationEquals(first, otherStyle),
           "two badges with the same count but different styles are not equal");
}

// The pen path's wire form: one stroke, `tool=bezier`, the anchors and their
// outgoing handles interleaved and absolute, and the closure as its own field.
// A closed path is filled before it is stroked, so a path that lost its closure
// -- or whose points came out as the incoming handles rather than the outgoing
// ones -- would still be "a stroke" and would only be noticed when it was drawn
// again.
void checkBezierSerializesWithItsClosure()
{
    vshot::OverlayController controller(largeSession());
    vshot::CaptureOverlay *overlay = nullptr;
    if (!openLargeOverlay(controller, &overlay)) {
        return;
    }
    controller.setWidth(6);
    controller.setCurrentColor(QColor(kPenRed, kPenGreen, kPenBlue, kPenAlpha));
    drawPenPath(controller, overlay, true);
    expect(controller.annotations().size() == 1, "the closed pen path lands as one annotation");
    if (controller.annotations().size() != 1) {
        return;
    }
    const vshot::Annotation &mark = controller.annotations().at(0);
    expect(mark.kind == vshot::Annotation::Kind::Stroke, "the pen path is a stroke annotation");
    expect(mark.tool == QStringLiteral("bezier"), "the pen path's tool is `bezier`", mark.tool);
    expect(mark.closed, "the pen path records that it was closed");
    expect(mark.points.size() == 2 * kPenAnchorCount,
           "the pen path carries one anchor and one outgoing handle per joint",
           QStringLiteral("points=%1").arg(mark.points.size()));
    if (mark.points.size() != 2 * kPenAnchorCount) {
        return;
    }
    // Every point is absolute, and the odd ones are the outgoing handles: the
    // incoming side is the mirror the renderer derives, so sending it would put
    // two control points where the wire format has room for one.
    bool placed = true;
    for (int index = 0; index < kPenAnchorCount; ++index) {
        placed = placed && mark.points.at(2 * index) == kPenAnchors[index] &&
            mark.points.at(2 * index + 1) == kPenHandles[index];
    }
    expect(placed, "the points are the anchors and the handles they were dragged to");

    // The marks document is what a daemon keeps and hands back, so the path's
    // closure has to survive it: an open curve and the closed one that fills it
    // are different marks, and a re-edit that lost the flag would fill a path
    // the user left open.
    const QJsonArray marks =
        controller.marksDocument();
    expect(marks.size() == 1, "the marks document carries the pen path");
    if (marks.isEmpty()) {
        return;
    }
    const QJsonObject value = marks.at(0).toObject();
    expect(value.value(QStringLiteral("kind")).toString() == QStringLiteral("stroke"),
           "the pen path serializes as a stroke",
           value.value(QStringLiteral("kind")).toString());
    expect(value.value(QStringLiteral("tool")).toString() == QStringLiteral("bezier"),
           "the pen path's tool name is `bezier`",
           value.value(QStringLiteral("tool")).toString());
    const QJsonArray points = value.value(QStringLiteral("points")).toArray();
    expect(points.size() == 2 * kPenAnchorCount && points.size() % 2 == 0,
           "the serialized points are the anchors and their handles, interleaved",
           QStringLiteral("points=%1").arg(points.size()));
    expect(value.value(QStringLiteral("closed")).toBool(),
           "the serialized path carries its closure");

    // The same three anchors, ended open on a double click: the one field that
    // differs is the closure, and it has to say so.
    vshot::OverlayController open(largeSession());
    vshot::CaptureOverlay *openOverlay = nullptr;
    if (!openLargeOverlay(open, &openOverlay)) {
        return;
    }
    open.setWidth(6);
    open.setCurrentColor(QColor(kPenRed, kPenGreen, kPenBlue, kPenAlpha));
    drawPenPath(open, openOverlay, false);
    expect(open.annotations().size() == 1, "the open pen path lands as one annotation");
    if (open.annotations().size() != 1) {
        return;
    }
    expect(!open.annotations().at(0).closed, "the open pen path is not closed");
    expect(open.annotations().at(0).points.size() == 2 * kPenAnchorCount,
           "the open pen path carries the same three anchors",
           QStringLiteral("points=%1").arg(open.annotations().at(0).points.size()));
    const QJsonArray openMarks = open.marksDocument();
    if (openMarks.isEmpty()) {
        expect(false, "the marks document carries the open pen path");
        return;
    }
    const QJsonObject openValue = openMarks.at(0).toObject();
    expect(openValue.value(QStringLiteral("tool")).toString() == QStringLiteral("bezier") &&
               !openValue.value(QStringLiteral("closed")).toBool(),
           "the open pen path serializes as an unclosed bezier");
}

// The whole point of keeping the marks as data: what the editor hands a daemon
// has to be enough to put the same marks back, so a re-edit opens on the
// picture the user left rather than on a blank canvas.  The two halves are
// `marksDocument` and `parseMarks`, and they are a protocol -- a field one
// writes and the other does not read is a mark that comes back subtly wrong,
// which nothing else here would catch.
void checkMarksRoundTripThroughASession()
{
    vshot::OverlayController source(largeSession());
    vshot::CaptureOverlay *overlay = nullptr;
    if (!openLargeOverlay(source, &overlay)) {
        return;
    }
    // One of each mark a session can carry, with a value in every field that
    // has one, so a field dropped on either side shows up as a difference.
    source.setCurrentColor(QColor(17, 34, 204, 128));
    source.chooseTool(vshot::Tool::Rectangle);
    source.setWidth(5);
    drag(source, overlay, QPointF(120, 140), QPointF(400, 300));
    source.setCurrentColor(QColor(9, 200, 30));
    source.chooseTool(vshot::Tool::Arrow);
    source.setWidth(7);
    drag(source, overlay, QPointF(500, 160), QPointF(760, 340));
    source.setCurrentColor(QColor(240, 120, 10));
    source.chooseTool(vshot::Tool::Wave);
    source.setWidth(4);
    // A period longer than the amplitude's own ceiling: the two share a limit
    // only by accident of the sliders, and a document written with a long wave
    // has to be one the reader will take back.
    source.setWaveWavelength(200);
    drag(source, overlay, QPointF(200, 600), QPointF(700, 600));
    // The mosaic brush is a Stroke, not a Shape, and the reader has to know it
    // under the name the writer gave it.  It is the one tool whose kind depends
    // on a second setting, so it is the one that can drift out of the list
    // unnoticed -- and a single unreadable mark fails the whole document.
    source.setCurrentColor(QColor(60, 60, 60));
    source.chooseTool(vshot::Tool::Mosaic);
    source.setMosaicShape(QStringLiteral("brush"));
    source.setWidth(20);
    drag(source, overlay, QPointF(150, 900), QPointF(600, 1050));
    source.setCurrentColor(QColor(255, 30, 30));
    source.chooseTool(vshot::Tool::Number);
    source.setNumberSize(36);
    source.press(overlay, QPointF(900, 700), Qt::LeftButton, Qt::NoModifier);
    source.release(overlay, QPointF(900, 700), Qt::LeftButton, Qt::NoModifier);
    expect(source.annotations().size() == 5, "five marks of five kinds are down",
           QString::number(source.annotations().size()));
    if (source.annotations().size() != 5) {
        return;
    }
    const QVector<vshot::Annotation> before = source.annotations();

    // Through the document, which is the only thing that crosses to the daemon.
    const QJsonArray marks = source.marksDocument();
    expect(marks.size() == 5, "the document carries all five marks",
           QString::number(marks.size()));
    if (marks.size() != 5) {
        return;
    }

    // And back into a fresh editor opened on the same canvas, which is what a
    // re-edit does: the session's own bounds are the canvas the marks were
    // measured against.
    vshot::Session reopened = largeSession();
    reopened.annotations = marks;
    reopened.mode = QStringLiteral("pin");
    reopened.bounds = vshot::LogicalRect{0, 0, 1600, 1200};
    vshot::OverlayController restored(reopened);
    restored.setPinEditMode(true);
    restored.beginPinEdit();
    const QVector<vshot::Annotation> after = restored.annotations();
    expect(after.size() == before.size(), "the restored editor carries the same marks",
           QStringLiteral("%1 of %2").arg(after.size()).arg(before.size()));
    if (after.size() != before.size()) {
        return;
    }
    for (int index = 0; index < before.size(); ++index) {
        expect(vshot::annotationEquals(before.at(index), after.at(index)),
               "a restored mark equals the one that was drawn",
               QStringLiteral("mark %1: %2 vs %3")
                   .arg(index)
                   .arg(before.at(index).tool, after.at(index).tool));
    }

    // The restored marks are the state the user left the pin in, so undo stops
    // there rather than peeling them off one at a time.
    restored.undo();
    expect(restored.annotations().size() == before.size(),
           "a reopened pin has nothing to undo back to",
           QString::number(restored.annotations().size()));

    // The document is relative to the canvas, so the same marks reopened on a
    // canvas somewhere else land at that canvas's own origin plus the offset
    // they were drawn at.  A reader that took the offsets for global pixels --
    // which is what a canvas at the origin hides -- would put every mark back
    // at the top-left corner of the screen instead.  Re-reading the document
    // back off the moved canvas is what proves it: relative to its own canvas
    // the same marks are the same offsets again.
    constexpr int shiftX = 300;
    constexpr int shiftY = 200;
    vshot::Session elsewhere = largeSession();
    elsewhere.annotations = marks;
    elsewhere.bounds = vshot::LogicalRect{shiftX, shiftY, 1600, 1200};
    elsewhere.selection = elsewhere.bounds;
    vshot::OverlayController moved(elsewhere);
    moved.setPinEditMode(true);
    moved.beginPinEdit();
    expect(moved.annotations().size() == before.size(),
           "an offset canvas carries the same marks",
           QStringLiteral("%1 of %2").arg(moved.annotations().size()).arg(before.size()));
    if (moved.annotations().size() != before.size()) {
        return;
    }
    expect(moved.marksDocument() == marks,
           "a mark reopens at its canvas's own origin",
           QString::fromUtf8(QJsonDocument(moved.marksDocument()).toJson(QJsonDocument::Compact)));
}

// The two marks a session cannot rebuild from geometry: a pasted image is its
// pixels and a placed translation is its lines, and neither has a smaller form
// the editor could re-derive.  They used to be dropped from the document
// outright -- the writer `continue`d past both -- so a pin carrying a pasted
// screenshot came back without it, and a text label, which is the other half of
// the same defect, failed the session instead: the writer left `tool` off a
// plain label and the reader refuses any mark without one, so one label made
// the whole document unreadable and *every* mark was lost.  What has to hold is
// that a re-edit opens on all of them, and that the image comes back at its own
// resolution rather than at the size it was scaled to.
void checkImageAndTranslationMarksSurviveAReEdit()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    QTemporaryDir dir;
    if (!dir.isValid()) {
        expect(false, "somewhere for the session's mark assets");
        return;
    }

    vshot::OverlayController source(largeSession());
    QString error;
    vshot::CaptureOverlay *overlay = source.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    source.beginPresetEdit();
    source.setMarkAssetDirectory(dir.path());

    // A label: the mark whose missing `tool` used to take the whole document
    // down with it, so it is first for a reason -- everything after it is what
    // the failure was hiding.
    source.setCurrentColor(QColor(10, 20, 30));
    source.chooseTool(vshot::Tool::Text);
    source.press(overlay, QPointF(200, 300), Qt::LeftButton, Qt::NoModifier);
    QLineEdit *editor = overlay->findChild<QLineEdit *>();
    if (editor == nullptr) {
        expect(false, "the text tool opens its inline editor");
        return;
    }
    editor->setText(QStringLiteral("kept"));
    source.key(overlay, Qt::Key_Return, Qt::NoModifier);

    // A pasted image, deliberately larger than the canvas, so it is shrunk to
    // fit on the way in: the mark's own pixels and the rect it was placed in
    // then disagree, which is exactly the pair the user asked to keep apart --
    // the rect is a placement and the pixels are the source a later resize has
    // to go back to.
    QImage seed(180, 130, QImage::Format_ARGB32);
    for (int y = 0; y < seed.height(); ++y) {
        for (int x = 0; x < seed.width(); ++x) {
            seed.setPixelColor(x, y, QColor((x * 7) % 256, (y * 11) % 256, 200, 255));
        }
    }
    const QImage pasted = seed.scaled(1800, 1300);
    expect(source.pasteImage(pasted, QStringLiteral("check")), "the image pastes");
    const QVector<vshot::Annotation> before = source.annotations();
    expect(before.size() == 2, "the label and the image are both down",
           QString::number(before.size()));
    if (before.size() != 2) {
        return;
    }
    const QImage &imageBefore = before.at(1).pixels;
    expect(imageBefore.width() == 1800 && imageBefore.height() == 1300,
           "the pasted image keeps its own resolution",
           QStringLiteral("%1x%2").arg(imageBefore.width()).arg(imageBefore.height()));
    expect(before.at(1).rect.width < 1800,
           "and the mark was placed smaller than that, so the two really differ",
           QStringLiteral("placed %1 wide").arg(before.at(1).rect.width));

    // Through the document that crosses to the daemon, with the assets written
    // beside it -- which is what a re-edit reads back.
    const QJsonArray marks = source.writeMarkAssets(dir.path());
    expect(marks.size() == 2, "the document carries both marks",
           QString::number(marks.size()));
    if (marks.size() != 2) {
        return;
    }
    const QJsonObject label = marks.at(0).toObject();
    expect(label.value(QStringLiteral("kind")).toString() == QStringLiteral("text") &&
               label.value(QStringLiteral("tool")).toString().isEmpty() == false,
           "a plain label names its tool",
           QString::fromUtf8(QJsonDocument(label).toJson(QJsonDocument::Compact)));
    const QJsonObject image = marks.at(1).toObject();
    const QString asset = image.value(QStringLiteral("pixels")).toString();
    expect(!asset.isEmpty() && QFileInfo::exists(asset),
           "the pasted image's pixels are written beside the session", asset);
    expect(image.value(QStringLiteral("pixel_width")).toInt() == 1800 &&
               image.value(QStringLiteral("pixel_height")).toInt() == 1300,
           "and the document records their own size, not the placed one",
           QString::fromUtf8(QJsonDocument(image).toJson(QJsonDocument::Compact)));

    // And back into a fresh editor opened on the same canvas, which is what a
    // re-edit does.
    vshot::Session reopened = largeSession();
    reopened.annotations = marks;
    reopened.mode = QStringLiteral("pin");
    reopened.bounds = vshot::LogicalRect{0, 0, 1600, 1200};
    vshot::OverlayController restored(reopened);
    restored.setPinEditMode(true);
    restored.beginPinEdit();
    const QVector<vshot::Annotation> after = restored.annotations();
    expect(after.size() == before.size(), "the restored editor carries both marks back",
           QStringLiteral("%1 of %2").arg(after.size()).arg(before.size()));
    if (after.size() != before.size()) {
        return;
    }
    // The label is pure geometry and compares as one mark.  The image cannot go
    // through `annotationEquals`: it compares pixel buffers by cache key, which
    // is a cheap identity test for the undo stack and deliberately not a
    // content test, and a mark that came back off a file is a different buffer
    // holding the same picture.  So the picture is compared, which is the thing
    // the user would notice.
    expect(vshot::annotationEquals(before.at(0), after.at(0)),
           "the restored label equals the one that was drawn",
           QStringLiteral("%1 vs %2").arg(before.at(0).tool, after.at(0).tool));
    expect(after.at(1).kind == before.at(1).kind && after.at(1).rect.x == before.at(1).rect.x &&
               after.at(1).rect.y == before.at(1).rect.y &&
               after.at(1).rect.width == before.at(1).rect.width &&
               after.at(1).rect.height == before.at(1).rect.height,
           "the restored image sits where the pasted one was placed",
           QStringLiteral("%1,%2 %3x%4 vs %5,%6 %7x%8")
               .arg(after.at(1).rect.x)
               .arg(after.at(1).rect.y)
               .arg(after.at(1).rect.width)
               .arg(after.at(1).rect.height)
               .arg(before.at(1).rect.x)
               .arg(before.at(1).rect.y)
               .arg(before.at(1).rect.width)
               .arg(before.at(1).rect.height));
    expect(after.at(1).pixels == before.at(1).pixels,
           "and its pixels come back unchanged",
           QStringLiteral("%1x%2 vs %3x%4")
               .arg(after.at(1).pixels.width())
               .arg(after.at(1).pixels.height())
               .arg(before.at(1).pixels.width())
               .arg(before.at(1).pixels.height()));
    // The point of keeping the original bytes: a later resize goes back to the
    // source, so what came back has to be the source and not the placed copy.
    expect(after.at(1).pixels.width() == 1800 && after.at(1).pixels.height() == 1300,
           "the image reopens at its own resolution, ready to be resized",
           QStringLiteral("%1x%2").arg(after.at(1).pixels.width()).arg(after.at(1).pixels.height()));
}

// The closure is content: an open path and the closed one that fills it are
// different marks.  Leaving it out of `annotationEquals` silently collapses an
// undo step -- the editor would think closing a path changed nothing and drop
// the edit.
void checkClosedPathsCompareByTheirClosure()
{
    vshot::Annotation open;
    open.kind = vshot::Annotation::Kind::Stroke;
    open.tool = QStringLiteral("bezier");
    open.points = {vshot::Point{400, 800}, vshot::Point{640, 800}, vshot::Point{1200, 800},
                   vshot::Point{1080, 890}, vshot::Point{800, 1100}, vshot::Point{680, 1010}};

    const vshot::Annotation copy = open;
    expect(vshot::annotationEquals(open, copy), "a pen path equals a copy of itself");

    vshot::Annotation closed = open;
    closed.closed = true;
    expect(!vshot::annotationEquals(open, closed),
           "an open pen path and the closed one that fills it are not equal");
}

// The fill is the stroke's colour at half its alpha, and the outline over it is
// more solid.  Both are read off the rendered overlay and compared against the
// composite the check computes for itself from the pixel the mark was drawn
// over: an unfilled path, or a fill as solid as its outline, is invisible in the
// model and only shows up here.
void checkBezierFillsAtHalfAlpha()
{
    vshot::OverlayController controller(largeSession());
    vshot::CaptureOverlay *overlay = nullptr;
    if (!openLargeOverlay(controller, &overlay)) {
        return;
    }
    // The frame and the toolbar before any mark, so the composite below is
    // measured against the very pixels the mark is drawn over.
    QImage background(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &background);
    const QColor ink(kPenRed, kPenGreen, kPenBlue);

    // Style the tool before drawing with it: the colour and the width belong to
    // the tool now, so setting them while another one is armed would style that
    // one instead.
    controller.chooseTool(vshot::Tool::Bezier);
    controller.setWidth(6);
    controller.setCurrentColor(QColor(kPenRed, kPenGreen, kPenBlue, kPenAlpha));
    drawPenPath(controller, overlay, true);
    expect(controller.annotations().size() == 1, "the closed pen path lands as one annotation");
    if (controller.annotations().size() != 1) {
        return;
    }
    QImage target(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &target);

    const QColor behind = background.pixelColor(kPenInside);
    const QColor fill = target.pixelColor(kPenInside);
    const QColor outline =
        target.pixelColor(QPoint(kPenAnchors[0].x, kPenAnchors[0].y));
    expect(colorDistance(fill, behind) > 20, "a closed pen path fills its inside",
           QStringLiteral("inside %1,%2,%3 vs background %4,%5,%6")
               .arg(fill.red())
               .arg(fill.green())
               .arg(fill.blue())
               .arg(behind.red())
               .arg(behind.green())
               .arg(behind.blue()));
    const QColor wanted = over(ink, kPenFillAlpha, behind);
    expect(colorDistance(fill, wanted) <= 3,
           "the fill is the stroke's colour at half its alpha",
           QStringLiteral("got %1,%2,%3 wanted %4,%5,%6")
               .arg(fill.red())
               .arg(fill.green())
               .arg(fill.blue())
               .arg(wanted.red())
               .arg(wanted.green())
               .arg(wanted.blue()));
    expect(colorDistance(outline, ink) < colorDistance(fill, ink),
           "the outline is more solid than the fill it encloses",
           QStringLiteral("outline %1,%2,%3 vs fill %4,%5,%6")
               .arg(outline.red())
               .arg(outline.green())
               .arg(outline.blue())
               .arg(fill.red())
               .arg(fill.green())
               .arg(fill.blue()));

    // The same three anchors, ended open: the middle the closed path painted is
    // the frame again, and the outline there is the stroke's colour at its own
    // alpha rather than at half of it.
    vshot::OverlayController open(largeSession());
    vshot::CaptureOverlay *openOverlay = nullptr;
    if (!openLargeOverlay(open, &openOverlay)) {
        return;
    }
    open.chooseTool(vshot::Tool::Bezier);
    open.setWidth(6);
    open.setCurrentColor(QColor(kPenRed, kPenGreen, kPenBlue, kPenAlpha));
    drawPenPath(open, openOverlay, false);
    expect(open.annotations().size() == 1, "the open pen path lands as one annotation");
    if (open.annotations().size() != 1) {
        return;
    }
    QImage openTarget(openOverlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(openOverlay, &openTarget);
    const QColor openBehind = background.pixelColor(kPenInside);
    expect(colorDistance(openTarget.pixelColor(kPenInside), openBehind) <= 2,
           "an open pen path leaves its inside clear",
           QStringLiteral("inside %1,%2,%3 vs background %4,%5,%6")
               .arg(openTarget.pixelColor(kPenInside).red())
               .arg(openTarget.pixelColor(kPenInside).green())
               .arg(openTarget.pixelColor(kPenInside).blue())
               .arg(openBehind.red())
               .arg(openBehind.green())
               .arg(openBehind.blue()));
    const QColor openOutline =
        openTarget.pixelColor(QPoint(kPenAnchors[0].x, kPenAnchors[0].y));
    expect(colorDistance(openOutline, over(ink, kPenAlpha, openBehind)) <= 3,
           "an open pen path strokes at the pen's own alpha",
           QStringLiteral("outline %1,%2,%3")
               .arg(openOutline.red())
               .arg(openOutline.green())
               .arg(openOutline.blue()));
}

// Every step of the pen gesture repaints what it changed.  The rubber band is
// the one step driven by a pointer move with nothing held down, and the closing
// press is the one step that turns a curve into a filled shape.
void checkBezierStepCoverage()
{
    vshot::OverlayController controller(largeSession());
    vshot::CaptureOverlay *overlay = nullptr;
    if (!openLargeOverlay(controller, &overlay)) {
        return;
    }
    controller.chooseTool(vshot::Tool::Bezier);
    controller.setWidth(6);

    // The step right after a press repaints the whole surface: a press leaves it
    // in a state only a full repaint describes, so there is no narrow region to
    // hold that step to.  Each block below therefore runs that step first and
    // asserts on the one after it, which is the step that has a rect to answer
    // for.
    controller.press(overlay, penAnchor(0), Qt::LeftButton, Qt::NoModifier);
    controller.move(overlay, penHandle(0), Qt::LeftButton, Qt::NoModifier);
    expectStepCovered(controller, overlay, penHandle(0) + QPointF(60, -40),
                      "a pen handle drag invalidates the bend it pulled out");
    controller.release(overlay, penHandle(0), Qt::LeftButton, Qt::NoModifier);

    controller.move(overlay, QPointF(900, 700), Qt::NoButton, Qt::NoModifier);
    expectHoverCovered(controller, overlay, QPointF(1000, 760),
                       "the pen rubber band invalidates where the pointer reaches");

    controller.press(overlay, penAnchor(1), Qt::LeftButton, Qt::NoModifier);
    controller.move(overlay, penHandle(1), Qt::LeftButton, Qt::NoModifier);
    expectStepCovered(controller, overlay, penHandle(1) + QPointF(60, 40),
                      "a second pen handle drag invalidates the bend it pulled out");
    controller.release(overlay, penHandle(1), Qt::LeftButton, Qt::NoModifier);

    controller.move(overlay, QPointF(1000, 1000), Qt::NoButton, Qt::NoModifier);
    expectHoverCovered(controller, overlay, penAnchor(2),
                       "the rubber band invalidates the last anchor it reaches for");

    controller.press(overlay, penAnchor(2), Qt::LeftButton, Qt::NoModifier);
    controller.move(overlay, penHandle(2), Qt::LeftButton, Qt::NoModifier);
    expectStepCovered(controller, overlay, penHandle(2) + QPointF(-60, -40),
                      "the third pen handle drag invalidates the bend it pulled out");
    controller.release(overlay, penHandle(2), Qt::LeftButton, Qt::NoModifier);

    // Closing the path is itself a press, so it repaints the whole surface and
    // has no narrow region to answer for; what it has to do is land the mark,
    // and land it closed.
    controller.press(overlay, penAnchor(0), Qt::LeftButton, Qt::NoModifier);
    controller.release(overlay, penAnchor(0), Qt::LeftButton, Qt::NoModifier);
    expect(controller.annotations().size() == 1, "the closed pen path lands as one annotation");
    expect(controller.annotations().size() == 1 && controller.annotations().at(0).closed,
           "the pen path that lands is the closed one");
}

// Writes the one key the loose-mode check needs into the config file the
// controller reads at construction.  Nothing else is written, so the built-in
// defaults are what the rest of the run sees and the file cannot be the reason
// a later assertion moves.
void writeSelectMode(const QString &mode)
{
    const QString path =
        qEnvironmentVariable("XDG_CONFIG_HOME") + QStringLiteral("/vshot/config.json");
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        expect(false, "the check has a config directory to write into", path);
        return;
    }
    QJsonObject editor;
    editor.insert(QStringLiteral("selectMode"), mode);
    QJsonObject root;
    root.insert(QStringLiteral("editor"), editor);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        expect(false, "the check can write a config file", path);
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
}

// `editor.selectMode` decides what a press does once a mark is selected.
// `precise` -- the default -- needs the press on the mark itself; `loose` moves
// it from anywhere on screen, and a press that never travels is a click that
// lets the mark go.  The stroke here is two pixels wide, so a second press
// landing on it would be luck: that is what the mode is for.
//
// Either way the drag is the middle button's: a left press that lands on
// nothing is otherwise how the capture's own selection is drawn, which is what
// a session with no tool armed is for.  The mark follows the *press*, not where
// the pointer travelled to, so the drag is started with the middle button and
// the motion that follows is plain -- which is also how the editor is actually
// used, since the button goes down before anything else happens.
void checkSelectModeDecidesWhatAPressPicksUp()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    const QPointF strokeStart(200, 200);
    const QPointF strokeMiddle(230, 205);
    const QPointF strokeEnd(240, 210);
    // Off the stroke, inside the capture selection and clear of its handles:
    // the press is on nothing at all.
    const QPointF nowhere(340, 330);
    const QPointF travel(30, 10);

    const auto run = [&](const QString &mode, bool loose) {
        writeSelectMode(mode);
        vshot::OverlayController controller(editingSession());
        QString error;
        vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
        if (overlay == nullptr) {
            expect(false, "the controller accepts an overlay", error);
            return;
        }
        overlay->show();
        controller.beginPresetEdit();

        controller.chooseTool(vshot::Tool::Pen);
        controller.setWidth(2);
        controller.press(overlay, strokeStart, Qt::LeftButton, Qt::NoModifier);
        controller.move(overlay, strokeMiddle, Qt::LeftButton, Qt::NoModifier);
        controller.release(overlay, strokeEnd, Qt::LeftButton, Qt::NoModifier);
        expect(controller.annotations().size() == 1, "the stroke lands as one mark");
        if (controller.annotations().size() != 1) {
            return;
        }
        const vshot::Point before = controller.annotations().at(0).points.constFirst();

        // A click on the stroke itself: the one press both modes act on.
        controller.chooseTool(std::nullopt);
        controller.press(overlay, strokeStart, Qt::LeftButton, Qt::NoModifier);
        controller.release(overlay, strokeStart, Qt::LeftButton, Qt::NoModifier);

        controller.press(overlay, nowhere, Qt::MiddleButton, Qt::NoModifier);
        controller.move(overlay, nowhere + travel, Qt::MiddleButton, Qt::NoModifier);
        controller.release(overlay, nowhere + travel, Qt::MiddleButton, Qt::NoModifier);
        const vshot::Point after = controller.annotations().at(0).points.constFirst();
        const bool moved = after.x == before.x + static_cast<int>(travel.x()) &&
            after.y == before.y + static_cast<int>(travel.y());
        const QString detail = QStringLiteral("(%1,%2) -> (%3,%4)")
                                   .arg(before.x)
                                   .arg(before.y)
                                   .arg(after.x)
                                   .arg(after.y);
        if (loose) {
            expect(moved, "a loose drag moves the selected mark from anywhere", detail);
        } else {
            expect(!moved, "a precise drag from nothing leaves the mark where it is", detail);
        }
        if (!loose) {
            return;
        }

        // A press that never travels is a click, and a click on nothing lets the
        // mark go: the drag after it has nothing to pick up, so the mark stays
        // where the first drag left it.
        controller.press(overlay, nowhere, Qt::LeftButton, Qt::NoModifier);
        controller.release(overlay, nowhere, Qt::LeftButton, Qt::NoModifier);
        controller.press(overlay, nowhere, Qt::MiddleButton, Qt::NoModifier);
        controller.move(overlay, nowhere + QPointF(20, 20), Qt::MiddleButton, Qt::NoModifier);
        controller.release(overlay, nowhere + QPointF(20, 20), Qt::MiddleButton, Qt::NoModifier);
        const vshot::Point dropped = controller.annotations().at(0).points.constFirst();
        expect(dropped.x == after.x && dropped.y == after.y,
               "a click on nothing drops the mark, so the next drag leaves it alone",
               QStringLiteral("(%1,%2) -> (%3,%4)")
                   .arg(after.x)
                   .arg(after.y)
                   .arg(dropped.x)
                   .arg(dropped.y));
    };

    run(QStringLiteral("loose"), true);
    run(QStringLiteral("precise"), false);
}

// The state the removed Select tool left behind: a modifier held *while* a press
// is made, which lets an existing mark be picked up without the armed tool being
// put down.
//
// The two halves of that are separate claims and both have to hold.  A mark has
// to move under the modifier even though a drawing tool is armed -- otherwise
// there is no way to adjust a mark without first disarming, which is the tool
// switch the user asked not to have.  And the tool has to still be armed
// afterwards: a pen that stopped drawing because the user nudged a mark with
// Shift would be a pen the user has to re-arm, and the modifier is supposed to
// be invisible to everything but the press it was held for.
//
// The handles are the exception, and are checked here too: they are small,
// deliberate targets that can only mean one thing, so they stretch whether or
// not the modifier is down, and whether or not a tool is armed.  A handle that
// needed the modifier would be unreachable in exactly the state where the user
// is drawing and wants to nudge the mark they just made.
void checkHoldingShiftPicksAMarkUpWithoutArmingATool()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }

    // The mark, as it was drawn: 180,190..300,260 in the session's own
    // coordinates, which are the overlay's too (the selection is 0,0..400,400).
    const QPointF corner(180, 190);
    const QPointF opposite(300, 260);
    // A point on the rectangle's rim rather than in its hollow middle: the
    // press has to land on the mark itself, and the middle of a hollow rect is
    // where the armed tool would be asked to draw.  Clear of the right edge's
    // own midpoint handle, which is a deliberate target of its own and would
    // answer the press as a stretch.
    const QPointF onTheRim(300, 205);
    const QPointF bodyMiddle(240, 225);
    const QPointF travel(30, 20);

    const auto rectOf = [](const vshot::OverlayController &controller) {
        return controller.annotations().constFirst().rect;
    };

    {
        vshot::OverlayController controller(editingSession());
        QString error;
        vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
        if (overlay == nullptr) {
            expect(false, "the controller accepts an overlay", error);
            return;
        }
        overlay->show();
        controller.beginPresetEdit();
        controller.chooseTool(vshot::Tool::Rectangle);
        drag(controller, overlay, corner, opposite);
        expect(controller.annotations().size() == 1, "the rectangle lands as one mark");
        if (controller.annotations().size() != 1) {
            return;
        }

        // The tool stays armed across the whole of this: that is the state the
        // user is in when they reach for a mark they drew a moment ago.
        controller.chooseTool(vshot::Tool::Pen);
        controller.press(overlay, bodyMiddle, Qt::LeftButton, Qt::ShiftModifier);
        controller.move(overlay, bodyMiddle + travel, Qt::LeftButton, Qt::ShiftModifier);
        controller.release(overlay, bodyMiddle + travel, Qt::LeftButton, Qt::ShiftModifier);
        const vshot::LogicalRect moved = rectOf(controller);
        expect(controller.annotations().size() == 1,
               "a Shift drag moves the mark instead of drawing another",
               QString::number(controller.annotations().size()));
        expect(moved.x == 210 && moved.y == 210,
               "the mark follows the Shift drag",
               QStringLiteral("rect at %1,%2 (wanted 210,210)").arg(moved.x).arg(moved.y));

        // And the pen is still the armed tool: a plain press now inks.  This is
        // the claim the modifier exists for -- it was a state, not a tool
        // change, so letting it go has to leave the user exactly where they
        // were.
        drag(controller, overlay, QPointF(60, 60), QPointF(110, 75));
        expect(controller.annotations().size() == 2,
               "the tool is still armed once the modifier is let go",
               QString::number(controller.annotations().size()));
    }

    // The handles, with nothing armed and with a tool armed: a stretch has to
    // work in both, because it is the one gesture that cannot be expressed any
    // other way.
    {
        vshot::OverlayController controller(editingSession());
        QString error;
        vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
        if (overlay == nullptr) {
            expect(false, "the controller accepts an overlay", error);
            return;
        }
        overlay->show();
        controller.beginPresetEdit();
        controller.chooseTool(vshot::Tool::Rectangle);
        drag(controller, overlay, corner, opposite);

        // Nothing armed, no modifier: a press on the mark picks it up, and the
        // corner handle then stretches it.  The handle is the far corner, so
        // the near one is what stays put.
        controller.chooseTool(std::nullopt);
        controller.press(overlay, bodyMiddle, Qt::LeftButton, Qt::NoModifier);
        controller.release(overlay, bodyMiddle, Qt::LeftButton, Qt::NoModifier);
        controller.press(overlay, QPointF(299, 259), Qt::LeftButton, Qt::NoModifier);
        controller.move(overlay, QPointF(340, 300), Qt::LeftButton, Qt::NoModifier);
        controller.release(overlay, QPointF(340, 300), Qt::LeftButton, Qt::NoModifier);
        const vshot::LogicalRect stretched = rectOf(controller);
        expect(stretched.x == 180 && stretched.y == 190 && stretched.width == 161
                   && stretched.height == 111,
               "a handle stretches the mark with nothing armed",
               QStringLiteral("%1,%2 %3x%4 (wanted 180,190 161x111)")
                   .arg(stretched.x)
                   .arg(stretched.y)
                   .arg(stretched.width)
                   .arg(stretched.height));

        // The same stretch with the pen armed and no modifier at all: the
        // handles are not gated on the pick-up modifier, so this has to move
        // the mark's corner rather than start a stroke on it.
        controller.chooseTool(vshot::Tool::Pen);
        controller.press(overlay, QPointF(180, 190), Qt::LeftButton, Qt::NoModifier);
        controller.move(overlay, QPointF(150, 160), Qt::LeftButton, Qt::NoModifier);
        controller.release(overlay, QPointF(150, 160), Qt::LeftButton, Qt::NoModifier);
        const vshot::LogicalRect pulled = rectOf(controller);
        expect(controller.annotations().size() == 1,
               "a handle with a tool armed stretches rather than inks",
               QString::number(controller.annotations().size()));
        expect(pulled.x == 150 && pulled.y == 160,
               "the handle drags the corner it was aimed at",
               QStringLiteral("rect at %1,%2 (wanted 150,160)").arg(pulled.x).arg(pulled.y));
    }

    // And the border of a mark is live on its own: a press that lands on the
    // rim picks the mark up with nothing held and nothing armed, because the
    // rim is a deliberate target that can only mean one thing.  The point is
    // off the handles, which is what tells the two apart -- a press on a handle
    // stretches, and this one moves.
    {
        vshot::OverlayController controller(editingSession());
        QString error;
        vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
        if (overlay == nullptr) {
            expect(false, "the controller accepts an overlay", error);
            return;
        }
        overlay->show();
        controller.beginPresetEdit();
        controller.chooseTool(vshot::Tool::Rectangle);
        drag(controller, overlay, corner, opposite);
        controller.chooseTool(std::nullopt);
        drag(controller, overlay, onTheRim, onTheRim + travel);
        const vshot::LogicalRect after = rectOf(controller);
        expect(after.x == 210 && after.y == 210,
               "the border drags the mark with nothing held",
               QStringLiteral("rect at %1,%2 (wanted 210,210)").arg(after.x).arg(after.y));
    }
}

// The pick-up state has to be *visible* before the press, not only after it.
// The pointer's shape is not enough: it says a drag is possible, not which mark
// would be taken, and a mark is a shape the user is aiming at rather than a
// point.  So the mark the pick-up modifier has put the pointer over wears a
// frame, and this is that frame: present under the modifier, gone without it,
// and gone again once the pointer leaves the mark.
//
// The frame is drawn where the mark is, so the check looks for ink on the mark's
// rim that a full render without the frame does not have -- and, just as
// importantly, for the *absence* of it in the two states that must not show it.
void checkThePickUpModifierFramesTheMarkUnderThePointer()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();
    controller.chooseTool(vshot::Tool::Rectangle);
    // A hollow rectangle, so its rim is the only thing there is to frame and the
    // middle of it is bare picture: a filled shape would give the frame nowhere
    // to show that the shape itself does not already cover.
    drag(controller, overlay, QPointF(180, 190), QPointF(300, 260));
    // Drawing leaves the new mark selected, and a selected mark already wears
    // the handles -- the hover frame is deliberately not drawn on top of them.
    // Ctrl+D is `select-none`, which is how a user gets to the same state.
    controller.key(overlay, Qt::Key_D, Qt::ControlModifier);
    controller.chooseTool(std::nullopt);

    // The mark's *body*, not its rim: the rim answers a press on its own and so
    // is framed whether the modifier is held or not, which would make the two
    // renders below identical and the check unable to tell them apart.
    const QPointF onTheMarkPoint(240, 225);
    const QPointF offTheMark(60, 60);

    // The frame is a difference, not a colour: the overlay's ground is the
    // frozen screen, which is opaque everywhere, so "is there ink here" cannot
    // be asked of one render.  It is asked of two -- the same pointer, with and
    // without the modifier -- and counted where they disagree.
    const auto framePixels = [&](const QPointF &at) {
        controller.move(overlay, at, Qt::NoButton, Qt::NoModifier);
        QImage plain(overlay->size(), QImage::Format_ARGB32_Premultiplied);
        paintOnce(overlay, &plain);
        controller.move(overlay, at, Qt::NoButton, Qt::ShiftModifier);
        QImage held(overlay->size(), QImage::Format_ARGB32_Premultiplied);
        paintOnce(overlay, &held);
        return differingPixelsOutside(childAreas(overlay), plain, held);
    };

    const int onTheMark = framePixels(onTheMarkPoint);
    expect(onTheMark > 0, "the pick-up modifier frames the mark under the pointer",
           QStringLiteral("%1 pixel(s) appear under the modifier").arg(onTheMark));

    const int away = framePixels(offTheMark);
    expect(away == 0, "the frame goes when the pointer leaves the mark",
           QStringLiteral("%1 pixel(s) off the mark, %2 on it").arg(away).arg(onTheMark));

    // The modifier arriving as a *key press*, which is how it actually arrives:
    // the pointer is already resting on the mark when the user reaches for
    // Shift, so a frame that only appeared on the next motion would not appear
    // at all for a user who presses the key and then presses the button.
    //
    // Nothing may be selected first.  Holding the modifier over a mark is now
    // what *selects* it, so the renders above left it selected and wearing its
    // handles -- and a mark that is already framed has nothing to show when the
    // key arrives.  Ctrl+D is `select-none`, which is how a user gets back to
    // the state this half is about: pointer on the mark, nothing taken yet.
    controller.move(overlay, offTheMark, Qt::NoButton, Qt::NoModifier);
    controller.key(overlay, Qt::Key_D, Qt::ControlModifier);
    controller.move(overlay, onTheMarkPoint, Qt::NoButton, Qt::NoModifier);
    QImage plainBefore(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &plainBefore);
    controller.key(overlay, Qt::Key_Shift, Qt::ShiftModifier);
    QImage plainAfter(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &plainAfter);
    const int onKeyPress = differingPixelsOutside(childAreas(overlay), plainBefore, plainAfter);
    expect(onKeyPress > 0, "pressing the pick-up modifier frames the mark already under the pointer",
           QStringLiteral("%1 pixel(s) appear").arg(onKeyPress));

    // The modifier does not only frame the mark: it hands it over as the
    // *selected* one, which is what the handles hang off.  That was the defect
    // the user reported -- a border with no handles -- and the reason a pixel
    // comparison cannot show it here is that with no tool armed the chrome is
    // drawn in every state, framed or not.  So it is asked of the Delete
    // binding instead, which acts on the selected mark alone: a mark the
    // modifier has taken is a mark that can be deleted.
    //
    // A second mark is what tells "the focus followed the pointer" from "the
    // first mark is still the active one": the pointer is taken onto it and the
    // key pressed, and the mark that goes has to be that one.
    controller.chooseTool(vshot::Tool::Rectangle);
    drag(controller, overlay, QPointF(120, 260), QPointF(180, 310));
    controller.chooseTool(std::nullopt);
    const int drawn = controller.annotations().size();
    controller.key(overlay, Qt::Key_D, Qt::ControlModifier);
    controller.move(overlay, offTheMark, Qt::NoButton, Qt::NoModifier);
    controller.key(overlay, Qt::Key_Shift, Qt::ShiftModifier);
    controller.move(overlay, QPointF(150, 285), Qt::NoButton, Qt::ShiftModifier);
    controller.key(overlay, Qt::Key_Delete, Qt::NoModifier);
    expect(controller.annotations().size() == drawn - 1,
           "the mark the modifier is over is the selected one",
           QStringLiteral("%1 mark(s) left of %2").arg(controller.annotations().size()).arg(drawn));
    const vshot::LogicalRect left = controller.annotations().constFirst().rect;
    expect(left.x == 180 && left.y == 190,
           "and the focus followed the pointer onto the second mark",
           QStringLiteral("the mark at %1,%2 survived (wanted 180,190)").arg(left.x).arg(left.y));
}


// Every move invalidates a rect; Qt then repaints only that rect on top of what
// is already on screen.  If any segment the stroke baked is not covered by a
// later step's rect, the pixels it should have painted are simply never drawn --
// the symptom being a scribble with holes that fills in only when the button is
// released (which full-repaints).  The check drives the gesture, repaints the
// way Qt does, and compares against a full render.
void checkLiveStrokeSurvivesIncrementalRepaint()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();
    controller.chooseTool(vshot::Tool::Pen);
    controller.setWidth(5);
    controller.setCurrentColor(QColor(255, 30, 30));

    const QVector<QPointF> path = serpentine();
    QImage backing(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    backing.fill(Qt::transparent);
    controller.press(overlay, path.constFirst(), Qt::LeftButton, Qt::NoModifier);
    paintOnce(overlay, &backing);
    for (int i = 1; i < path.size(); ++i) {
        controller.move(overlay, path.at(i), Qt::LeftButton, Qt::NoModifier);
        renderIncremental(controller, overlay, &backing);
    }
    QImage full(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &full);
    const int diff = differingPixelsOutside(childAreas(overlay), backing, full);
    expect(diff == 0, "the live stroke survives repainting only what each move invalidated",
           QStringLiteral("%1 px differ between the incremental and full repaint").arg(diff));
}

// The magnifier has to show the pixel under the cursor at its centre, whichever
// edge the cursor is near.  The formula is easy to get subtly wrong at the
// edges, where the sample window is clamped and has to be shifted the other way
// to keep the cursor's own pixel centred; that is exactly what "the magnifier
// looks scrambled while dragging" means.  This reads the painted circle back and
// matches every sampled pixel against the source pixel it should be showing.
void checkLoupeShowsTheCursorPixelEverywhere()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    const auto run = [screen](const vshot::Session &session, bool *ok) {
        vshot::OverlayController controller(session);
        QString error;
        vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
        if (overlay == nullptr) {
            expect(false, "the controller accepts an overlay", error);
            *ok = false;
            return;
        }
        overlay->show();
        const QSize size = overlay->size();
        const vshot::OutputSession &output = controller.session().outputs.at(0);
        const QImage &source = output.image;
        const int scale = static_cast<int>(output.scale > 0 ? output.scale : 1);
        constexpr int radius = 60; // (2 * kLoupeRadius + 1) * kLoupeZoom / 2
        constexpr int margin = 10;

        // Corners, edges and the middle, in logical pixels of the session's
        // 400x400 selection canvas.
        const QPoint pointers[] = {{0, 0},     {3, 3},     {7, 7},     {30, 30},
                                   {200, 200}, {399, 399}, {3, 399},   {399, 3},
                                   {150, 0},   {0, 150}};
        int wrong = 0;
        int checked = 0;
        for (const QPoint &pointer : pointers) {
            // One drag step pins the pointer and turns the magnifier on.
            controller.press(overlay, QPointF(pointer), Qt::LeftButton, Qt::NoModifier);
            controller.move(overlay, QPointF(pointer), Qt::LeftButton, Qt::NoModifier);
            QImage frame(size, QImage::Format_ARGB32_Premultiplied);
            paintOnce(overlay, &frame);

            // Same placement the overlay computes.
            QPointF center(pointer.x() + radius * 1.1, pointer.y() + radius * 1.1);
            if (center.x() + radius > size.width() - margin) {
                center.setX(pointer.x() - radius * 1.1);
            }
            if (center.y() + radius > size.height() - margin) {
                center.setY(pointer.y() - radius * 1.1);
            }
            const int centerX = std::clamp((pointer.x() - output.geometry.x) * scale, 0,
                                           source.width() - 1);
            const int centerY = std::clamp((pointer.y() - output.geometry.y) * scale, 0,
                                           source.height() - 1);
            // The magnified window, now always the full span centred on the
            // cursor pixel: the crop borrows the nearest edge pixel where the
            // window runs off the frame, so every sample below has an answer.
            const QPointF origin(center.x() - radius, center.y() - radius);

            for (int i : {3, 11}) {
                for (int j : {3, 11}) {
                    const QPoint at(static_cast<int>(origin.x()) + i * 8 + 4,
                                    static_cast<int>(origin.y()) + j * 8 + 4);
                    if (at.x() < 0 || at.y() < 0 || at.x() >= size.width() ||
                        at.y() >= size.height()) {
                        continue;
                    }
                    const QColor shown = frame.pixelColor(at);
                    const int wantX = std::clamp(centerX - 7 + i, 0, source.width() - 1);
                    const int wantY = std::clamp(centerY - 7 + j, 0, source.height() - 1);
                    const QColor want = source.pixelColor(wantX, wantY);
                    ++checked;
                    if (colorDistance(shown, want) > 2) {
                        ++wrong;
                        if (wrong <= 4) {
                            std::printf("loupe at (%d,%d) samples (%d,%d): got %d,%d,%d want "
                                        "%d,%d,%d\n",
                                        pointer.x(), pointer.y(), wantX, wantY, shown.red(),
                                        shown.green(), shown.blue(), want.red(), want.green(),
                                        want.blue());
                        }
                    }
                }
            }
        }
        expect(checked > 0, "the magnifier was sampled", QStringLiteral("no samples taken"));
        expect(wrong == 0, "the magnifier shows the pixel under the cursor at every edge",
               QStringLiteral("%1 of %2 samples wrong").arg(wrong).arg(checked));
        *ok = wrong == 0 && checked > 0;
    };

    bool ok = true;
    run(coordinateSession(), &ok);
    run(scaledCoordinateSession(), &ok);
}

// The colour picker: the right button's magnifier, and only the right button's.
//
// The loupe is drawn the same way whichever press brought it up, so the pixels
// alone cannot tell a picker from a coordinate readout -- and the difference
// matters, because the picker carries two keys that act on the colour under the
// cursor.  A drag loupe that also carried them would be a loupe that answered
// "A" while the user was drawing, and "A" is cursor-left.  So the state is asked
// for directly, and the two presses are put side by side.
void checkOnlyTheRightButtonBringsUpTheColourPicker()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();

    const QPointF spot(200, 200);
    expect(!overlay->colorPickerVisible() && !overlay->magnifierVisible(),
           "nothing is up before a button goes down");

    // A left drag: the loupe comes up to say where the cursor is, and the
    // picker does not.  The loupe here is drawn off the gesture rather than
    // from `magnifierVisible`, so the flag stays false while it is on screen --
    // which is exactly the distinction the colour keys are gated on.
    controller.press(overlay, spot, Qt::LeftButton, Qt::NoModifier);
    controller.move(overlay, spot + QPointF(4, 4), Qt::LeftButton, Qt::NoModifier);
    expect(!overlay->colorPickerVisible(), "a drag's loupe is not the colour picker");
    QImage dragFrame(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &dragFrame);
    expect(dragFrame.pixelColor(static_cast<int>(spot.x() + 66),
                                static_cast<int>(spot.y() + 66))
               != QColor(Qt::transparent),
           "a drag still brings a loupe up, it just is not the picker");
    controller.release(overlay, spot + QPointF(4, 4), Qt::LeftButton, Qt::NoModifier);
    expect(!overlay->colorPickerVisible(), "letting the button go puts the picker away");

    // The right button: the picker, and the picker follows the pointer for as
    // long as it is held.  What it is aimed at is the pixel under the cursor
    // *now*, not the one the button went down on, so a step has to repaint.
    controller.press(overlay, spot, Qt::RightButton, Qt::NoModifier);
    expect(overlay->colorPickerVisible(), "the right button brings the picker up");
    expect(overlay->magnifierVisible(), "the picker is a magnifier");

    // Incrementally, the way the widget is actually driven: the loupe's old
    // place has to be *erased* and the new one painted, and a full render after
    // every step cannot tell that from a step that did nothing at all.  That is
    // exactly how a follow that never fired passed this check once -- the
    // handler asked for `Qt::NoButton`, which a motion carrying a held button
    // never reports, so the whole branch was dead and only the full re-render
    // kept the loupe looking right.
    //
    // So: a full render at the new position, then the same step driven the way
    // the widget drives it, and the two have to agree.  A loupe left behind at
    // the old place, or one that never arrived, is a difference either way.
    const QPointF moved = spot + QPointF(100, 100);
    controller.move(overlay, moved, Qt::RightButton, Qt::NoModifier);
    QImage full(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &full);

    // Back to the start, rendered in full so the incremental run below begins
    // from pixels that are certainly right, then forward again a step at a time.
    controller.move(overlay, spot, Qt::RightButton, Qt::NoModifier);
    QImage backing(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &backing);
    controller.move(overlay, moved, Qt::RightButton, Qt::NoModifier);
    const QRect claimed = controller.lastInteractiveUpdate();
    expect(!claimed.isNull() && !claimed.isEmpty(),
           "a step of the held picker asks for a repaint of its own",
           QStringLiteral("claimed %1x%2").arg(claimed.width()).arg(claimed.height()));
    renderIncremental(controller, overlay, &backing);
    const int strayed = differingPixelsOutside(childAreas(overlay), full, backing);
    expect(strayed == 0,
           "a step of the held picker leaves the picture a full repaint would",
           QStringLiteral("%1 pixel(s) differ -- the loupe is not keeping up").arg(strayed));

    controller.release(overlay, moved, Qt::RightButton, Qt::NoModifier);
    expect(!overlay->colorPickerVisible() && !overlay->magnifierVisible(),
           "letting the right button go puts the picker away");
}

// A tool whose press picks a start and whose release picks an end holds the
// button for the whole of the stroke, so the mouse cannot place the far end
// exactly -- which is the one situation the keyboard cursor exists for.  The
// walk has to reach the *live* gesture: a step that moved only the editor's own
// cursor would leave the preview behind, and one that nudged a committed mark
// would move the wrong thing entirely (there is no committed mark yet).
//
// The anchor is the part that must not move.  A stroke is drawn from where the
// press landed, and walking the cursor chooses where the other end goes; an
// implementation that moved both would drag the whole shape across the canvas,
// which is a different edit from the one the pointer would have made.
void checkAStrokeInProgressFollowsTheCursorKeys()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();
    controller.chooseTool(vshot::Tool::Rectangle);

    // The press anchors, and the button stays down from here on -- the whole
    // point of the check is that the keys work while it is.
    const QPointF anchor(100, 100);
    controller.press(overlay, anchor, Qt::LeftButton, Qt::NoModifier);
    controller.key(overlay, Qt::Key_D, Qt::NoModifier);
    controller.key(overlay, Qt::Key_S, Qt::NoModifier);

    // The walk starts from the pointer, which the press left at the anchor, so
    // one step right and one down puts the far corner on 101,101 -- and the
    // shape is the box between it and the anchor that has not moved.
    const QVector<vshot::Annotation> marks = controller.annotations();
    expect(marks.isEmpty(), "a stroke in progress is not a mark yet",
           QString::number(marks.size()));

    // The release carries the pointer position like any other event, and the
    // button has not moved the mouse -- so it lands where the press left it,
    // and the far corner the keys chose is what the release commits.
    controller.release(overlay, anchor, Qt::LeftButton, Qt::NoModifier);
    const QVector<vshot::Annotation> committed = controller.annotations();
    expect(committed.size() == 1, "the release commits the stroke the keys shaped",
           QString::number(committed.size()));
    if (committed.size() != 1) {
        return;
    }
    const vshot::Annotation &mark = committed.constFirst();
    expect(mark.rect.x == 100 && mark.rect.y == 100,
           "the anchor stays where the press put it",
           QStringLiteral("%1,%2").arg(mark.rect.x).arg(mark.rect.y));
    expect(mark.rect.width == 2 && mark.rect.height == 2,
           "the far corner followed the cursor keys",
           QStringLiteral("%1x%2").arg(mark.rect.width).arg(mark.rect.height));
}

// The motion the compositor reports after VShot warps the pointer is VShot's
// own request coming back, not the user moving the mouse -- and reading it as a
// move ends the magnifier flash the step just raised, so the loupe blinks on
// every step of a walk and whether it survives the last one is a race.
//
// The echo is recognised by where it lands: a warp puts the pointer exactly
// where the walk put the cursor, so the motion arrives on that very pixel.  The
// check drives both halves -- the walk, and then the motion the compositor
// would report for it -- and requires the flash to still be up afterwards.
void checkThePointerWarpEchoDoesNotPutTheMagnifierOut()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();
    // A session with no CLI behind it writes no request and remembers no
    // target, so the echo would have nothing to be recognised against.
    controller.enablePointerWarp();

    // The session has no pointer of its own, so the walk starts at the middle
    // of its bounds: 200,200 in a 400x400 scene, and one step right lands on
    // 201,200 -- the pixel the compositor will report the pointer at.
    controller.key(overlay, Qt::Key_Right, Qt::NoModifier);
    expect(overlay->magnifierVisible(), "a step of the walk puts the magnifier up");

    const QPointF echoed(201, 200);
    controller.move(overlay, echoed, Qt::NoButton, Qt::NoModifier);
    expect(overlay->magnifierVisible(),
           "the motion the warp's own request produces leaves the magnifier up");

    // A hand on the mouse is the other case, and it still takes the cursor back
    // and puts the loupe away -- otherwise the flash would follow the pointer
    // around and never go out.
    controller.move(overlay, echoed + QPointF(3, 0), Qt::NoButton, Qt::NoModifier);
    expect(!overlay->magnifierVisible(),
           "a motion that is not the echo puts the magnifier out");
}

// The magnifier's own key, on its own.  Every other way the loupe comes up ends
// by repainting -- a cursor step draws the frame it moved to, a press redraws
// for its gesture -- so this is the one path where raising the flag is the
// whole of the work, and leaving the repaint out made the key look dead: the
// loupe was up, and nothing on screen said so until the next event redrew.
//
// What is asserted is the repaint itself, not the pixels: `render` paints from
// the state whenever it is called, so a frame read back through it shows the
// loupe whether or not anything asked for one.  Only the widget's own paint
// event tells the two apart, and that is what the filter below counts.
class PaintCounter : public QObject {
public:
    explicit PaintCounter(QObject *parent) : QObject(parent) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Paint) {
            ++paints;
        }
        return QObject::eventFilter(watched, event);
    }

public:
    int paints = 0;
};

void checkTheMagnifierKeyDrawsTheMagnifier()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();
    controller.chooseTool(std::nullopt);

    PaintCounter counter(overlay);
    overlay->installEventFilter(&counter);

    expect(!overlay->magnifierVisible(), "nothing is up before the key is pressed");

    // Whatever the window and the tool change queued is drained first, so the
    // count below is the key's own doing and not the open's.
    QCoreApplication::processEvents();
    counter.paints = 0;

    controller.key(overlay, Qt::Key_M, Qt::NoModifier);
    expect(overlay->magnifierVisible(), "the magnifier key puts the magnifier up");

    // The repaint is what the key owes the screen, and it arrives through the
    // event loop rather than during the call.
    QCoreApplication::processEvents();
    expect(counter.paints > 0, "the magnifier key asks for a repaint",
           QStringLiteral("%1 paint event(s)").arg(counter.paints));
}

// The magnifier reads the *picture*, not the bare capture: a mark on the screen
// has to be in it, or the one place the user goes to see a pixel at 8x would be
// the one place that disagrees with everything else.
//
// A solid green rectangle over a black frame is the case that cannot be argued
// with: the frame under it is black everywhere, so a loupe showing the capture
// alone is black wherever it is aimed, and a loupe showing the picture is green
// wherever the cursor is over the mark.  Both readings are taken, so a check
// that only ever saw green -- a loupe painted flat, say -- would fail too.
void checkTheMagnifierShowsTheMarks()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(blackSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();
    controller.chooseTool(vshot::Tool::Rectangle);
    controller.setWidth(4);
    controller.setCurrentColor(QColor(0, 255, 0));
    // 160,160..240,240, so its own centre is far from the frame's.
    drag(controller, overlay, QPointF(160, 160), QPointF(240, 240));
    controller.chooseTool(std::nullopt);
    expect(controller.annotations().size() == 1, "the rectangle lands as one mark");
    if (controller.annotations().size() != 1) {
        return;
    }

    // The loupe hangs below and to the right of the cursor, so the cursor's own
    // pixel is at its centre and the samples around it are the neighbours.
    const auto loupeAt = [&](const QPointF &pointer, QImage *frame) {
        controller.press(overlay, pointer, Qt::LeftButton, Qt::NoModifier);
        controller.move(overlay, pointer, Qt::LeftButton, Qt::NoModifier);
        *frame = QImage(overlay->size(), QImage::Format_ARGB32_Premultiplied);
        paintOnce(overlay, frame);
        controller.release(overlay, pointer, Qt::LeftButton, Qt::NoModifier);
    };
    constexpr int radius = 60; // (2 * kLoupeRadius + 1) * kLoupeZoom / 2
    const auto greenInLoupe = [&](const QPointF &pointer) {
        QImage frame;
        loupeAt(pointer, &frame);
        const QPointF center(pointer.x() + radius * 1.1, pointer.y() + radius * 1.1);
        // A small square around the centre, inside the circle and clear of the
        // crosshair the loupe draws over it.
        for (int dy = -20; dy <= 20; dy += 4) {
            for (int dx = -20; dx <= 20; dx += 4) {
                const QColor pixel =
                    frame.pixelColor(static_cast<int>(center.x()) + dx,
                                     static_cast<int>(center.y()) + dy);
                if (pixel.green() > 200 && pixel.red() < 60 && pixel.blue() < 60) {
                    return true;
                }
            }
        }
        return false;
    };

    // Over the mark: the rim is the ink, and the cursor sits on it.
    expect(greenInLoupe(QPointF(200, 160)), "the magnifier shows the mark under the cursor");
    // Over the bare frame, well outside the mark: the picture there *is* the
    // capture, so the loupe must not be green -- otherwise the reading above
    // would prove nothing about what the loupe samples.
    expect(!greenInLoupe(QPointF(80, 80)), "and shows the capture where no mark is");
}

// The pill under the picker's loupe: the colour on one line and the shortcut
// hint on another, and the colour line *is* the colour.
//
// That last part is the whole point of the two lines.  One line carrying both
// meant the hint had to be legible on whatever colour the cursor happened to be
// over, which is a background the pill cannot choose; splitting them lets the
// colour line be the sampled colour itself, with ink picked to read on it.  So
// the check samples the pill's ground and requires it to be the pixel under the
// cursor, and samples the ink over it and requires it to be black or white --
// a tint would fail on some colour, which is the failure this exists to catch.
// The keyboard cursor has to move the *real* pointer, not only the editor's own
// idea of where it is.  Everything the editor draws reads the editor's cursor,
// so a walk that never asked the compositor would look perfect and still leave
// the arrow on the screen where it started -- which is exactly the bug: the
// user steps ten pixels, the loupe says so, and the pointer they can see has
// not moved.
//
// The request goes to the CLI over the pipe the session arrived on, because
// moving a pointer is the CLI's job (it holds the injection backends).  This
// checks the helper's half of that: that a walk writes the request, with the
// position the editor moved to, and that a session which was never told there
// is a CLI behind it writes nothing.
void checkWalkingTheCursorAsksTheCliToMoveThePointer()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }

    // The helper writes to stdout, so the check has to stand in for the CLI by
    // reading it.  A pipe of the check's own, made the process's stdout for the
    // duration: the controller writes with `fwrite(stdout)`, and there is no
    // seam to pass a stream through.
    const auto capture = [screen](bool tellTheHelperThereIsACli) {
        fflush(stdout);
        const int saved = dup(STDOUT_FILENO);
        int pipeEnds[2] = {-1, -1};
        if (saved < 0 || pipe(pipeEnds) != 0) {
            return QByteArray();
        }
        dup2(pipeEnds[1], STDOUT_FILENO);
        close(pipeEnds[1]);

        {
            vshot::OverlayController controller(editingSession());
            QString error;
            vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
            if (overlay != nullptr) {
                overlay->show();
                controller.beginPresetEdit();
                if (tellTheHelperThereIsACli) {
                    controller.enablePointerWarp();
                }
                controller.key(overlay, Qt::Key_Right, Qt::NoModifier);
                controller.key(overlay, Qt::Key_Down, Qt::NoModifier);
                // The second step is inside the throttle's window, so it is
                // held back and sent by a timer -- which needs the event loop.
                // A held key repeats the same way, so this is what the real
                // session does; without it the check would only ever see the
                // first step and the pointer would be left a pixel behind.
                QThread::msleep(80);
                QCoreApplication::processEvents();
            }
        }
        fflush(stdout);
        dup2(saved, STDOUT_FILENO);
        close(saved);

        QByteArray written;
        char buffer[4096];
        const int flags = fcntl(pipeEnds[0], F_GETFL, 0);
        fcntl(pipeEnds[0], F_SETFL, flags | O_NONBLOCK);
        while (true) {
            const ssize_t got = ::read(pipeEnds[0], buffer, sizeof(buffer));
            if (got <= 0) {
                break;
            }
            written.append(buffer, static_cast<int>(got));
        }
        close(pipeEnds[0]);
        return written;
    };

    const QByteArray asked = capture(true);
    // The session has no pointer of its own, so the walk starts at the middle of
    // its bounds -- 200,200 in a 400x400 scene -- and one step right and one
    // down lands on 201,201.  Asserted as a number rather than as "some
    // position was sent", because a request that always said 0,0 would pass
    // that and move the pointer to the corner of the desktop.
    const QByteArray expected = QByteArrayLiteral(
        "{\"request\":\"pointer\",\"x\":201,\"y\":201}");
    expect(asked.contains(expected),
           "walking the cursor asks the CLI to move the pointer where it went",
           QStringLiteral("wanted %1 in %2").arg(QString::fromUtf8(expected),
                                                 QString::fromUtf8(asked)));

    const QByteArray silent = capture(false);
    expect(silent.isEmpty(),
           "a helper that was not told there is a CLI writes nothing to its stdout",
           QString::fromUtf8(silent));
}

void checkTheColourPillShowsTheColourAndReadsOnIt()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }

    // The darkest and the brightest pixel the session has, so the ink has to go
    // both ways: a rule that only ever chose black would pass on one and fail on
    // the other.
    const auto run = [screen](const QColor &ground) {
        vshot::Session session = coordinateSession();
        // A canvas that is *not* the sampled colour, with the sampled colour on
        // the one pixel under the cursor.  Filling the frame with it instead
        // would make the swatch and the canvas the same colour, and the check
        // could not tell the pill from the picture behind it.
        session.outputs[0].image.fill(QColor(90, 120, 150));
        session.outputs[0].image.setPixelColor(60, 60, ground);
        vshot::OverlayController controller(std::move(session));
        QString error;
        vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
        if (overlay == nullptr) {
            expect(false, "the controller accepts an overlay", error);
            return;
        }
        overlay->show();
        controller.beginPresetEdit();

        const QPointF spot(60, 60);
        controller.press(overlay, spot, Qt::RightButton, Qt::NoModifier);
        controller.move(overlay, spot, Qt::NoButton, Qt::NoModifier);
        QImage frame(overlay->size(), QImage::Format_ARGB32_Premultiplied);
        paintOnce(overlay, &frame);
        controller.release(overlay, spot, Qt::RightButton, Qt::NoModifier);

        // The pill is found rather than placed.  Its exact geometry is the
        // renderer's business, and a check that recomputed it would be testing
        // its own arithmetic against itself; what matters is that the pill is on
        // screen at all, carrying the sampled colour with the code written on
        // it.
        //
        // The swatch is opaque and the canvas around the overlay is not, so the
        // colour under the cursor *is* the swatch wherever it appears -- nothing
        // else on the overlay paints a solid field of it.  The row is chosen by
        // how much ink it carries rather than by how wide its band is: the
        // widest band is the row above the glyphs, which has none.
        const double groundLuma = (ground.red() + ground.green() + ground.blue()) / 3.0;
        // The end the swatch is further from is the one that reads on it: a
        // fixed white would vanish on a white pixel, which is the pixel a
        // colour picker gets pointed at.
        const bool wantLight = groundLuma < 127.5;
        const auto isGround = [&ground](const QColor &pixel) {
            return std::max({std::abs(pixel.red() - ground.red()),
                             std::abs(pixel.green() - ground.green()),
                             std::abs(pixel.blue() - ground.blue())}) <= 2;
        };

        // Below the loupe's disc, which is where the pill hangs.  The scan has
        // to start there: the loupe magnifies the sampled pixel to fill a large
        // part of the disc, so a search over the whole frame finds *that* first
        // and reads the code off the magnified crop.
        constexpr int radius = 60;
        const QPointF center(spot.x() + radius * 1.1, spot.y() + radius * 1.1);
        int bestRow = -1;
        int bestLeft = 0;
        int bestRight = 0;
        int bestBand = 0;
        int inkPixels = 0;
        int wrong = 0;
        for (int py = static_cast<int>(center.y() + radius) + 2; py < frame.height(); ++py) {
            // The row's swatch, as the span between its first and last pixel of
            // the colour.  A *run* will not do: the code is written across the
            // middle of the band, so the longest unbroken run is one glyph's
            // gap rather than the swatch.  The span is the swatch, and what is
            // inside it that is not the swatch is the ink.
            int first = -1;
            int last = -1;
            int band = 0;
            for (int px = 0; px < frame.width(); ++px) {
                if (!isGround(frame.pixelColor(px, py))) {
                    continue;
                }
                if (first < 0) {
                    first = px;
                }
                last = px;
                ++band;
            }
            if (band < 20) {
                continue;
            }
            int ink = 0;
            int bad = 0;
            for (int px = first; px <= last; ++px) {
                const QColor pixel = frame.pixelColor(px, py);
                const double luma = (pixel.red() + pixel.green() + pixel.blue()) / 3.0;
                if (std::abs(luma - groundLuma) < 60.0) {
                    continue; // the swatch, or close enough to be antialiasing
                }
                ++ink;
                if ((luma > groundLuma) != wantLight) {
                    ++bad;
                }
            }
            if (ink > inkPixels) {
                inkPixels = ink;
                wrong = bad;
                bestRow = py;
                bestLeft = first;
                bestRight = last;
                bestBand = band;
            }
        }

        expect(bestBand > 20, "the colour line is a band of the colour under the cursor",
               QStringLiteral("no run of %1,%2,%3 wider than 20px")
                   .arg(ground.red())
                   .arg(ground.green())
                   .arg(ground.blue()));
        expect(inkPixels > 0, "the colour line has the code written on it",
               QStringLiteral("ground %1,%2,%3")
                   .arg(ground.red())
                   .arg(ground.green())
                   .arg(ground.blue()));
        expect(wrong == 0, "the code is written in whichever of black and white reads on it",
               QStringLiteral("%1 of %2 inked pixels at row %3 (%4..%5) went the wrong way")
                   .arg(wrong)
                   .arg(inkPixels)
                   .arg(bestRow)
                   .arg(bestLeft)
                   .arg(bestRight));
    };

    run(QColor(0, 0, 0));
    run(QColor(255, 255, 255));
}

// Where the label is typed has to be where it lands.  The inline editor is a
// QLineEdit child widget; whatever frame or padding it carries shifts its
// glyphs away from the origin the committed label is drawn at, so the label
// visibly jumps the moment the editor is accepted.  This measures the ink of
// the typed text and of the committed label and requires them to agree.
void checkTextEditorMatchesTheCommittedLabel()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(blackSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();
    controller.chooseTool(vshot::Tool::Text);
    controller.setCurrentColor(QColor(0, 255, 0));

    const QPoint origin(60, 60);
    controller.press(overlay, QPointF(origin), Qt::LeftButton, Qt::NoModifier);
    QLineEdit *editor = overlay->findChild<QLineEdit *>();
    expect(editor != nullptr, "the text tool opens its inline editor");
    if (editor == nullptr) {
        return;
    }
    editor->setText(QStringLiteral("Hi"));

    const auto inkBounds = [](const QImage &image) {
        QRect box;
        for (int y = 0; y < 120 && y < image.height(); ++y) {
            for (int x = 0; x < 240 && x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                if (c.green() > 60 && c.green() > c.red() * 2 && c.green() > c.blue() * 2) {
                    box = box.isNull() ? QRect(x, y, 1, 1) : box.united(QRect(x, y, 1, 1));
                }
            }
        }
        return box;
    };

    QImage whileEditing(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &whileEditing);
    const QRect editingInk = inkBounds(whileEditing);

    controller.key(overlay, Qt::Key_Return, Qt::NoModifier);
    expect(controller.annotations().size() == 1, "the label lands as one annotation");
    QImage committed(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &committed);
    const QRect committedInk = inkBounds(committed);

    expect(!editingInk.isNull() && !committedInk.isNull(), "the typed and committed labels both paint");
    if (!editingInk.isNull() && !committedInk.isNull()) {
        // A pixel of slack for the antialiased edge, which the two renderings
        // need not sample identically; the frame and clamp that used to push
        // the editor's text twenty-odd pixels off are what this pins down.
        const auto near = [](int a, int b) { return std::abs(a - b) <= 1; };
        const bool same = near(editingInk.x(), committedInk.x()) &&
                          near(editingInk.y(), committedInk.y()) &&
                          near(editingInk.width(), committedInk.width()) &&
                          near(editingInk.height(), committedInk.height());
        expect(same, "the typed label sits where the committed one lands",
               QStringLiteral("editing ink %1,%2 %3x%4 vs committed %5,%6 %7x%8")
                   .arg(editingInk.x())
                   .arg(editingInk.y())
                   .arg(editingInk.width())
                   .arg(editingInk.height())
                   .arg(committedInk.x())
                   .arg(committedInk.y())
                   .arg(committedInk.width())
                   .arg(committedInk.height()));
    }
}

// A pin-edit session whose image has been dragged: the session's recorded
// geometry is still the rect the editor opened on, while the image (the
// session bounds) has moved.  That is exactly the state the editor is in after
// the daemon confirms a move, and it is where an overlay's surface, the image
// rect and the stale output geometry all disagree.
vshot::Session pinEditSession()
{
    vshot::Session session;
    session.mode = QStringLiteral("pin-edit");
    session.bounds = vshot::LogicalRect{150, 150, 240, 180};
    vshot::OutputSession output;
    output.id = 1;
    output.name = QStringLiteral("CHECK-1");
    output.geometry = vshot::LogicalRect{40, 40, 240, 180};
    output.surface = vshot::LogicalRect{0, 0, 400, 400};
    output.scale = 1;
    output.pixelWidth = 240;
    output.pixelHeight = 180;
    QImage image(240, 180, QImage::Format_RGBA8888);
    for (int y = 0; y < 180; ++y) {
        for (int x = 0; x < 240; ++x) {
            image.setPixelColor(x, y, QColor(x % 256, y % 256, (x * 7 + y * 13) % 256, 255));
        }
    }
    output.image = image;
    session.outputs.push_back(output);
    return session;
}

// A stand-in for the pin daemon: it answers every `move` with the position it
// was asked for, and counts the requests, so a move that should never have been
// sent is visible as a move rather than swallowed. Owns the server and the
// socket path, which the controller dials.
class StubPinDaemon {
public:
    explicit StubPinDaemon(QTemporaryDir *dir)
        : path_(dir->filePath(QStringLiteral("pin.sock")))
    {
        QLocalServer::removeServer(path_);
        listening_ = server_.listen(path_);
        if (!listening_) {
            return;
        }
        QObject::connect(&server_, &QLocalServer::newConnection, &server_, [this] {
            while (QLocalSocket *socket = server_.nextPendingConnection()) {
                client_ = socket;
                QObject::connect(socket, &QLocalSocket::readyRead, socket, [this, socket] {
                    buffer_ += socket->readAll();
                    qsizetype newline = -1;
                    while ((newline = buffer_.indexOf('\n')) >= 0) {
                        const QByteArray line = buffer_.left(newline);
                        buffer_.remove(0, newline + 1);
                        const QJsonObject request = QJsonDocument::fromJson(line).object();
                        const QString command =
                            request.value(QStringLiteral("cmd")).toString();
                        if (command == QStringLiteral("watch-active")) {
                            // The daemon answers the subscription at once with
                            // the pin the open edit is on.
                            reportActive(1);
                            continue;
                        }
                        if (command == QStringLiteral("raise")) {
                            ++raises_;
                            QJsonObject reply;
                            reply.insert(QStringLiteral("raised"),
                                         request.value(QStringLiteral("id")));
                            QByteArray out =
                                QJsonDocument(reply).toJson(QJsonDocument::Compact);
                            out.append('\n');
                            socket->write(out);
                            socket->flush();
                            continue;
                        }
                        if (command != QStringLiteral("move")) {
                            continue;
                        }
                        ++moves_;
                        QJsonObject reply{
                            {QStringLiteral("ok"), true},
                            {QStringLiteral("x"), request.value(QStringLiteral("x"))},
                            {QStringLiteral("y"), request.value(QStringLiteral("y"))},
                            {QStringLiteral("width"), 240},
                            {QStringLiteral("height"), 180}};
                        QByteArray out = QJsonDocument(reply).toJson(QJsonDocument::Compact);
                        out.append('\n');
                        socket->write(out);
                        socket->flush();
                    }
                });
            }
        });
    }

    bool listening() const { return listening_; }
    QString error() const { return server_.errorString(); }
    const QString &path() const { return path_; }
    int moves() const { return moves_; }
    int raises() const { return raises_; }
    bool hasClient() const { return client_ != nullptr; }
    // Says which pin is the live one, the way the daemon does once an edit is
    // open: a line with no `ok`, so it is not a move's answer.
    void reportActive(quint64 id)
    {
        if (client_ == nullptr) {
            return;
        }
        QJsonObject notice;
        notice.insert(QStringLiteral("active"), static_cast<qint64>(id));
        QByteArray out = QJsonDocument(notice).toJson(QJsonDocument::Compact);
        out.append('\n');
        client_->write(out);
        client_->flush();
    }

private:
    QLocalServer server_;
    QString path_;
    QByteArray buffer_;
    QPointer<QLocalSocket> client_;
    int moves_ = 0;
    int raises_ = 0;
    bool listening_ = false;
};

// A drag on a pinned image is confirmed by the daemon over and over -- one
// reply per motion event -- and each confirmation rewrites the session's own
// record of where the frame sits.  That record used to be part of every mark's
// raster key, so a single drag threw away and rebuilt every mark on the pin,
// once per motion: re-entering a pin to edit it was the only session that had
// marks to rebuild, which is why the lag showed up there and nowhere else.
// The key is the mark's own content now, and this is the check that says so:
// the daemon moves the image under the marks and the rasters are left alone.
//
// The mosaic is the deliberate exception and is checked separately: it samples
// the frame, so a move that changes the pixels under it has to redraw.  What
// this pins down is that a *plain* mark does not.
void checkPinConfirmationKeepsTheRasters()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    QTemporaryDir dir;
    StubPinDaemon daemon(&dir);
    if (!daemon.listening()) {
        expect(false, "the stand-in daemon listens", daemon.error());
        return;
    }

    vshot::OverlayController controller(pinEditSession());
    controller.setPinEditMode(true);
    controller.setPinTarget(1, daemon.path());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPinEdit();

    // Three marks of the kinds whose pixels are their own: a shape, a freehand
    // stroke and a label.  Each rasterizes once, and none of them samples the
    // frame, so a confirmation may not touch any of them.
    controller.setCurrentColor(QColor(20, 200, 40));
    controller.chooseTool(vshot::Tool::Rectangle);
    controller.setWidth(4);
    drag(controller, overlay, QPointF(180, 180), QPointF(280, 250));
    controller.chooseTool(vshot::Tool::Pen);
    controller.setWidth(5);
    drag(controller, overlay, QPointF(190, 270), QPointF(300, 300));
    controller.chooseTool(vshot::Tool::Text);
    controller.press(overlay, QPointF(200, 200), Qt::LeftButton, Qt::NoModifier);
    QLineEdit *editor = overlay->findChild<QLineEdit *>();
    if (editor == nullptr) {
        expect(false, "the text tool opens its inline editor");
        return;
    }
    editor->setText(QStringLiteral("Pin"));
    controller.key(overlay, Qt::Key_Return, Qt::NoModifier);
    controller.chooseTool(std::nullopt);
    expect(controller.annotations().size() == 3, "three marks are down",
           QString::number(controller.annotations().size()));
    if (controller.annotations().size() != 3) {
        return;
    }

    QImage target(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    paintOnce(overlay, &target);
    QVector<int> rebuilds;
    for (const vshot::Annotation &annotation : controller.annotations()) {
        rebuilds.push_back(annotation.rasterRebuilds());
    }
    for (int index = 0; index < rebuilds.size(); ++index) {
        expect(rebuilds.at(index) == 1, "a mark rasterizes once before the drag",
               QStringLiteral("mark %1: rebuilds=%2").arg(index).arg(rebuilds.at(index)));
    }

    // A drag on the image: the daemon confirms every step, and the confirmation
    // moves the frame out from under the marks.  The marks travel with it --
    // that is what `applyPinRect` is for -- but their own pixels do not change.
    // The middle button is what moves the image in here, and the press lands on
    // a clear part of it, so this is the frame being moved rather than a mark.
    controller.press(overlay, QPointF(350, 200), Qt::MiddleButton, Qt::NoModifier);
    for (int step = 1; step <= 4; ++step) {
        controller.move(overlay, QPointF(350 - step * 8, 200 - step * 6), Qt::MiddleButton,
                        Qt::NoModifier);
        QCoreApplication::processEvents();
    }
    controller.release(overlay, QPointF(318, 176), Qt::MiddleButton, Qt::NoModifier);
    // The daemon socket connects asynchronously, so let the confirmations land
    // before reading the counts back.
    QElapsedTimer drain;
    drain.start();
    while (drain.elapsed() < 200) {
        QCoreApplication::processEvents();
    }
    expect(daemon.moves() > 0, "the drag asked the daemon to move the pin",
           QStringLiteral("%1 move(s) sent").arg(daemon.moves()));
    paintOnce(overlay, &target);
    for (int index = 0; index < rebuilds.size(); ++index) {
        expect(controller.annotations().at(index).rasterRebuilds() == rebuilds.at(index),
               "a daemon confirmation does not rebuild the mark's raster",
               QStringLiteral("mark %1: %2 -> %3")
                   .arg(index)
                   .arg(rebuilds.at(index))
                   .arg(controller.annotations().at(index).rasterRebuilds()));
    }
}

// The bare canvas around a pinned image is not part of the image, so a drag
// that starts there must not move it.  It did: the press and the motion were
// both turned into global points by the *clamping* conversion, which folds a
// point outside the image onto its nearest edge -- so the "is the pointer on
// the image" test was answered by the clamp rather than by the pointer, and
// every click on the surrounding canvas read as a click on the image.
void checkDraggingOffTheImageLeavesItWhereItIs()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    QTemporaryDir dir;
    StubPinDaemon daemon(&dir);
    if (!daemon.listening()) {
        expect(false, "the stand-in daemon listens", daemon.error());
        return;
    }

    vshot::OverlayController controller(pinEditSession());
    controller.setPinEditMode(true);
    controller.setPinTarget(1, daemon.path());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPinEdit();
    controller.chooseTool(std::nullopt);

    // The image is 150,150..390,330; these are on the canvas around it, in each
    // direction, and far enough out that a drag cannot be a rounding artefact.
    const QPointF outside[] = {
        QPointF(60, 60),   // above and left
        QPointF(60, 240),  // left
        QPointF(350, 350), // below, past the image's bottom edge
        QPointF(240, 60),  // above
    };
    const auto before = controller.selection();
    if (!before.has_value()) {
        expect(false, "the pin editor opens with the image selected");
        return;
    }
    for (const QPointF &start : outside) {
        controller.press(overlay, start, Qt::LeftButton, Qt::NoModifier);
        controller.move(overlay, start + QPointF(24, 18), Qt::LeftButton, Qt::NoModifier);
        controller.move(overlay, start + QPointF(48, 36), Qt::LeftButton, Qt::NoModifier);
        controller.release(overlay, start + QPointF(48, 36), Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::processEvents();
        const auto after = controller.selection();
        expect(after.has_value() && after->x == before->x && after->y == before->y,
               "a drag starting off the image leaves it where it was",
               QStringLiteral("from (%1,%2): %3,%4 -> %5,%6")
                   .arg(start.x())
                   .arg(start.y())
                   .arg(before->x)
                   .arg(before->y)
                   .arg(after.has_value() ? after->x : -1)
                   .arg(after.has_value() ? after->y : -1));
    }
    expect(daemon.moves() == 0, "no move is sent for a drag off the image",
           QStringLiteral("%1 move(s) sent").arg(daemon.moves()));

    // A bare left drag on the image does nothing: the image is only moved by
    // the middle button, the same rule the region selection's body follows.
    // Without this the cursor over the picture would be a move cursor and every
    // stray press on the image would shove the pin.
    controller.press(overlay, QPointF(200, 200), Qt::LeftButton, Qt::NoModifier);
    controller.move(overlay, QPointF(224, 218), Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::processEvents();
    const auto unmoved = controller.selection();
    expect(unmoved.has_value() && unmoved->x == before->x && unmoved->y == before->y,
           "a drag on the image without the middle button leaves it where it was",
           QStringLiteral("%1,%2").arg(unmoved.has_value() ? unmoved->x : -1)
               .arg(unmoved.has_value() ? unmoved->y : -1));
    controller.release(overlay, QPointF(224, 218), Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::processEvents();

    // ... and a middle drag on the image still moves it, so the checks above are
    // about the button and not about the drag being broken.
    controller.press(overlay, QPointF(200, 200), Qt::MiddleButton, Qt::NoModifier);
    controller.move(overlay, QPointF(224, 218), Qt::MiddleButton, Qt::NoModifier);
    QCoreApplication::processEvents();
    const auto dragged = controller.selection();
    expect(dragged.has_value() && dragged->x != before->x,
           "a drag on the image still moves it",
           QStringLiteral("%1,%2").arg(dragged.has_value() ? dragged->x : -1)
               .arg(dragged.has_value() ? dragged->y : -1));
    controller.release(overlay, QPointF(224, 218), Qt::MiddleButton, Qt::NoModifier);
}

// The pin editor walks the same keyboard cursor the region editor does -- the
// letters move the pointer, the arrows nudge the selected mark -- so it asks
// for the same warp over the same pipe.  This is the half the helper owns: that
// the walk writes the request at all from a pin-edit session, whose surface is
// one output rather than the desktop.  What the CLI does with the position is
// its own check (`the_pin_editor_is_answered_on_its_request_pipe_too`), and the
// helper sends it in the same global logical pixels either way.
void checkThePinEditorAsksForThePointerWarpToo()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    QTemporaryDir dir;
    StubPinDaemon daemon(&dir);
    if (!daemon.listening()) {
        expect(false, "the stand-in daemon listens", daemon.error());
        return;
    }

    fflush(stdout);
    const int saved = dup(STDOUT_FILENO);
    int pipeEnds[2] = {-1, -1};
    if (saved < 0 || pipe(pipeEnds) != 0) {
        expect(false, "the check can stand in for the CLI's pipe");
        return;
    }
    dup2(pipeEnds[1], STDOUT_FILENO);
    close(pipeEnds[1]);

    {
        vshot::OverlayController controller(pinEditSession());
        controller.setPinEditMode(true);
        controller.setPinTarget(1, daemon.path());
        QString error;
        vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
        if (overlay == nullptr) {
            expect(false, "the controller accepts a pin-edit overlay", error);
        } else {
            overlay->show();
            controller.beginPinEdit();
            // The pin's own marks are the selection, so a press would move the
            // image: no mark is selected, and the letters walk the cursor.
            controller.enablePointerWarp();
            controller.key(overlay, Qt::Key_D, Qt::NoModifier);
            controller.key(overlay, Qt::Key_D, Qt::NoModifier);
            // The second step lands inside the throttle's window and is sent by
            // a timer, which needs the event loop.
            QThread::msleep(80);
            QCoreApplication::processEvents();
        }
    }
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);

    QByteArray written;
    char buffer[4096];
    const int flags = fcntl(pipeEnds[0], F_GETFL, 0);
    fcntl(pipeEnds[0], F_SETFL, flags | O_NONBLOCK);
    while (true) {
        const ssize_t got = ::read(pipeEnds[0], buffer, sizeof(buffer));
        if (got <= 0) {
            break;
        }
        written.append(buffer, static_cast<int>(got));
    }
    close(pipeEnds[0]);

    // The pin image is selected from the moment the editor opens, so the walk
    // starts at the middle of the *image* -- 150+120, 150+90 -- and two steps
    // right land on 272,240.  Asserted as a position rather than as "something
    // was sent", because a request that always said 0,0 would pass that and put
    // the pointer in the corner of the desktop.
    const QByteArray expected =
        QByteArrayLiteral("{\"request\":\"pointer\",\"x\":272,\"y\":240}");
    expect(written.contains(expected),
           "walking the cursor in the pin editor asks the CLI to move the pointer",
           QStringLiteral("wanted %1 in %2").arg(QString::fromUtf8(expected),
                                                 QString::fromUtf8(written)));
}


// The pin's own border is the pin: the daemon centres the stroke on the image's
// edge, so half of it stands on the canvas beside the image, and a user aiming
// at the rim means to move the pin.  A press there moves it, and takes no ink:
// a mark placed outside the picture would be clipped away by the renderer.
void checkThePinsBorderMovesItButTakesNoInk()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    QTemporaryDir dir;
    StubPinDaemon daemon(&dir);
    if (!daemon.listening()) {
        expect(false, "the stand-in daemon listens", daemon.error());
        return;
    }

    vshot::Session session = pinEditSession();
    // A four-pixel border, so its outer band is two logical pixels wide: wide
    // enough to aim at deliberately rather than by a rounding accident.
    session.pinBorderWidth = 4;
    vshot::OverlayController controller(std::move(session));
    controller.setPinEditMode(true);
    controller.setPinTarget(1, daemon.path());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPinEdit();
    controller.chooseTool(std::nullopt);

    const auto before = controller.selection();
    if (!before.has_value()) {
        expect(false, "the pin editor opens with the image selected");
        return;
    }
    // The image is 150,150..390,330.  (148,240) is one pixel left of its left
    // edge, i.e. inside the band the border's outer half occupies.  The drag
    // goes left and up: the image may not leave the overlay's own surface, and
    // there is room in that direction but none to the right.
    constexpr int travelX = -24;
    constexpr int travelY = -18;
    controller.press(overlay, QPointF(148, 240), Qt::LeftButton, Qt::NoModifier);
    for (int step = 1; step <= 3; ++step) {
        controller.move(overlay, QPointF(148 + step * travelX / 3, 240 + step * travelY / 3),
                        Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::processEvents();
    }
    // The daemon socket connects asynchronously, so the first positions queue
    // behind it; give the round trip a moment before counting.
    QElapsedTimer drain;
    drain.start();
    while (drain.elapsed() < 200) {
        QCoreApplication::processEvents();
    }
    const auto dragged = controller.selection();
    // The pin lands exactly where the pointer asked: the drag started two
    // pixels off the image, and clamping either end of it would lose that.
    expect(dragged.has_value() && dragged->x == before->x + travelX &&
               dragged->y == before->y + travelY,
           "a drag starting on the pin's border moves it by the pointer's own travel",
           QStringLiteral("%1,%2 -> %3,%4 (wanted %5,%6)")
               .arg(before->x)
               .arg(before->y)
               .arg(dragged.has_value() ? dragged->x : -1)
               .arg(dragged.has_value() ? dragged->y : -1)
               .arg(before->x + travelX)
               .arg(before->y + travelY));
    expect(daemon.moves() > 0, "the border drag asks the daemon to move the pin",
           QStringLiteral("%1 move(s) sent").arg(daemon.moves()));
    controller.release(overlay, QPointF(148 + travelX, 240 + travelY), Qt::LeftButton,
                       Qt::NoModifier);

    // A drawing tool on the border takes no ink: the mark would sit outside the
    // picture, and the renderer clips it away, so the press must not start a
    // stroke at all.  The border travelled with the pin, so these points are
    // read off where the image now is rather than where it started.
    const QPointF border{before->x + travelX - 2.0, before->y + travelY + 90.0};
    const int marksBefore = controller.annotations().size();
    controller.chooseTool(vshot::Tool::Pen);
    controller.press(overlay, border, Qt::LeftButton, Qt::NoModifier);
    controller.move(overlay, border + QPointF(-28, -20), Qt::LeftButton, Qt::NoModifier);
    controller.release(overlay, border + QPointF(-28, -20), Qt::LeftButton, Qt::NoModifier);
    expect(controller.annotations().size() == marksBefore,
           "a pen on the pin's border leaves no mark",
           QStringLiteral("%1 -> %2 marks")
               .arg(marksBefore)
               .arg(controller.annotations().size()));

    // ... and a pen on the image itself still draws, so the check above is
    // about the border and not about drawing being broken.
    const QPointF onImage{before->x + travelX + 50.0, before->y + travelY + 50.0};
    controller.press(overlay, onImage, Qt::LeftButton, Qt::NoModifier);
    controller.move(overlay, onImage + QPointF(40, 30), Qt::LeftButton, Qt::NoModifier);
    controller.release(overlay, onImage + QPointF(40, 30), Qt::LeftButton, Qt::NoModifier);
    expect(controller.annotations().size() == marksBefore + 1,
           "a pen on the image still draws",
           QStringLiteral("%1 -> %2 marks")
               .arg(marksBefore)
               .arg(controller.annotations().size()));
}

// The editor's frame around the pin it is annotating is Qt chrome, and every
// other pin on the screen is painted by a Wayland surface one layer below it.
// The compositor orders a layer's surfaces by map time and offers no restack, so
// a frame drawn for a pin the user has moved on from would be painted on top of
// every other pin -- there is no ordering on the editor's side that could put it
// back underneath. The daemon is the one that knows whose turn it is (the pin
// holds the keyboard and the pointer is over it), so it says so and the frame
// goes while the edit itself carries on.
void checkThePinFrameGoesWhenAnotherPinTakesOver()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    QTemporaryDir dir;
    StubPinDaemon daemon(&dir);
    if (!daemon.listening()) {
        expect(false, "the stand-in daemon listens", daemon.error());
        return;
    }

    vshot::OverlayController controller(pinEditSession());
    controller.setPinEditMode(true);
    controller.setPinTarget(1, daemon.path());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPinEdit();
    controller.chooseTool(std::nullopt);

    const auto selection = controller.selection();
    if (!selection.has_value()) {
        expect(false, "the pin editor opens with the image selected");
        return;
    }
    // Counted rather than sampled at one point: the frame is a two-pixel stroke
    // centred on the image's edge, and the toolbar sits somewhere over the
    // canvas, so which single pixel is on the stroke is not something this check
    // should have to know.  White pixels in the band around the image can only
    // be the stroke -- the image itself is a colour ramp, and the chrome's own
    // ink is off-white.
    const auto framePixels = [&] {
        QImage painted(overlay->size(), QImage::Format_ARGB32_Premultiplied);
        paintOnce(overlay, &painted);
        const QRect band(selection->x - 3, selection->y - 3, selection->width + 6,
                         selection->height + 6);
        int white = 0;
        for (int y = std::max(0, band.top()); y <= std::min(painted.height() - 1, band.bottom());
             ++y) {
            for (int x = std::max(0, band.left());
                 x <= std::min(painted.width() - 1, band.right()); ++x) {
                const QColor pixel = painted.pixelColor(x, y);
                if (pixel.red() > 240 && pixel.green() > 240 && pixel.blue() > 240) {
                    ++white;
                }
            }
        }
        return white;
    };
    const auto frameIsDrawn = [&](bool wanted, const char *what) {
        const int white = framePixels();
        return expect((white > 200) == wanted, what,
                      QStringLiteral("%1 white pixel(s) on the image's edge").arg(white));
    };
    // The editor dials the daemon as the session is set up, but the connect is
    // asynchronous: give the event loop the turn it needs, or a report sent now
    // would go nowhere and the check would pass or fail on a race rather than on
    // the frame.
    QElapsedTimer connected;
    connected.start();
    while (!daemon.hasClient() && connected.elapsed() < 500) {
        QCoreApplication::processEvents();
    }
    expect(daemon.hasClient(), "the editor is connected to the daemon before anything moves");
    frameIsDrawn(true, "the pin being edited draws its frame");

    // Another pin takes the keyboard: the daemon says the live pin is no longer
    // this one, and the frame goes with it.
    daemon.reportActive(2);
    QCoreApplication::processEvents();
    frameIsDrawn(false, "... and loses it when another pin becomes the live one");

    // The edit is still open -- the marks are the reason the editor is here, and
    // losing the pointer to another pin is not the user ending it -- so the
    // toolbar stays and the image is still the editable canvas.
    expect(controller.isPinEdit() && controller.selection().has_value(),
           "losing the frame does not end the edit");
    const int marksBefore = controller.annotations().size();
    controller.chooseTool(vshot::Tool::Pen);
    const QPointF onImage(selection->x + 50.0, selection->y + 50.0);
    drag(controller, overlay, onImage, onImage + QPointF(30, 20));
    expect(controller.annotations().size() == marksBefore + 1,
           "the image still takes ink while the frame is away");

    // The pointer comes back to the pin being edited, and the frame comes with
    // it.  Coming back is a click on the image, and a click on a pin means "this
    // one": the pin has to go back on top of the stack as well as get its frame
    // back, or the editor would be annotating a picture the other pins cover.
    // The click cannot reach the pin's own surface to do it -- the editor's
    // layer surface holds the keyboard and covers the output -- so the editor
    // asks the daemon, and this is the ask.
    // The ask travels asynchronously, so let the earlier press's reach the
    // daemon before counting, or this would be counting two of them.
    QCoreApplication::processEvents();
    const int raisesBefore = daemon.raises();
    controller.press(overlay, onImage, Qt::LeftButton, Qt::NoModifier);
    controller.release(overlay, onImage, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::processEvents();
    expect(daemon.raises() == raisesBefore + 1,
           "a press on the image asks the daemon to put the pin back on top",
           QStringLiteral("%1 -> %2 raise(s)").arg(raisesBefore).arg(daemon.raises()));
    daemon.reportActive(1);
    QCoreApplication::processEvents();
    frameIsDrawn(true, "the frame comes back when the pin is the live one again");
}

// A pin that comes back for a second edit opens on the marks the first edit
// left, and those marks have to be as adjustable as ones drawn in this sitting:
// the whole point of keeping them as data is that the user can pick one up and
// move it rather than only look at it.
void checkRestoredMarksCanBeSelected()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    QTemporaryDir dir;
    StubPinDaemon daemon(&dir);
    if (!daemon.listening()) {
        expect(false, "the stand-in daemon listens", daemon.error());
        return;
    }

    // First sitting: draw a rectangle and a pen path on the pin, then read back
    // the document the daemon would have been handed.
    QJsonArray marks;
    {
        vshot::OverlayController first(pinEditSession());
        first.setPinEditMode(true);
        first.setPinTarget(1, daemon.path());
        QString error;
        vshot::CaptureOverlay *overlay = first.addOverlay(0, screen, &error);
        if (overlay == nullptr) {
            expect(false, "the controller accepts an overlay", error);
            return;
        }
        overlay->show();
        first.beginPinEdit();
        first.chooseTool(vshot::Tool::Rectangle);
        drag(first, overlay, QPointF(180, 190), QPointF(300, 260));
        first.chooseTool(vshot::Tool::Pen);
        drag(first, overlay, QPointF(200, 300), QPointF(330, 310));
        expect(first.annotations().size() == 2, "the first sitting leaves two marks",
               QString::number(first.annotations().size()));
        marks = first.marksDocument();
    }
    expect(marks.size() == 2, "the document carries both marks",
           QString::number(marks.size()));
    if (marks.size() != 2) {
        return;
    }

    // Second sitting: the daemon hands the same marks back with the pristine
    // image, exactly as `startEdit` does.
    vshot::Session reopened = pinEditSession();
    reopened.annotations = marks;
    vshot::OverlayController second(std::move(reopened));
    second.setPinEditMode(true);
    second.setPinTarget(1, daemon.path());
    QString error;
    vshot::CaptureOverlay *overlay = second.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    second.beginPinEdit();
    expect(second.annotations().size() == 2, "the second sitting opens on both marks",
           QString::number(second.annotations().size()));
    if (second.annotations().size() != 2) {
        return;
    }

    // The rectangle is where it was drawn: 180,190..300,260 in the image's own
    // coordinates, and the pin sits at 150,150, so a press in its middle lands
    // on the restored mark.
    second.chooseTool(std::nullopt);
    second.press(overlay, QPointF(240, 225), Qt::LeftButton, Qt::NoModifier);
    second.move(overlay, QPointF(270, 245), Qt::LeftButton, Qt::NoModifier);
    second.release(overlay, QPointF(270, 245), Qt::LeftButton, Qt::NoModifier);

    const QJsonArray moved = second.marksDocument();
    expect(moved.size() == 2, "the marks survive the drag",
           QString::number(moved.size()));
    if (moved.size() != 2) {
        return;
    }
    const QJsonObject rect = moved.at(0).toObject().value(QStringLiteral("rect")).toObject();
    const auto field = [](const QJsonObject &object, const char *name) {
        return object.value(QLatin1String(name)).toInt();
    };
    // Relative to the canvas (the image), so the drawn rect reads 30,40 and a
    // drag of +30,+20 has to show up as 60,60.
    expect(field(rect, "x") == 60 && field(rect, "y") == 60,
           "a restored mark can be picked up and dragged",
           QStringLiteral("rect at %1,%2 (wanted 60,60)")
               .arg(field(rect, "x"))
               .arg(field(rect, "y")));
}

// The pin editor's surface covers the whole output so the toolbar has somewhere
// to sit beside the image, but the editor only owns the chrome it draws.  An
// input region that covered the whole surface would take every click on the
// screen, so the desktop behind the editor -- the pin daemon's own windows, the
// user's other applications -- would never see one.
void checkPinEditTakesInputOnlyOverItsChrome()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    QTemporaryDir dir;
    StubPinDaemon daemon(&dir);
    if (!daemon.listening()) {
        expect(false, "the stand-in daemon listens", daemon.error());
        return;
    }

    vshot::Session session = pinEditSession();
    // A four-pixel border: the band outside the image is part of the pin.
    session.pinBorderWidth = 4;
    vshot::OverlayController controller(std::move(session));
    controller.setPinEditMode(true);
    controller.setPinTarget(1, daemon.path());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPinEdit();
    // The mask is coalesced into the event loop, so let it run before asking.
    QCoreApplication::processEvents();

    const QRegion mask = overlay->windowHandle() != nullptr
        ? overlay->windowHandle()->mask()
        : QRegion();
    // The image is 150,150..390,330 and the border reaches two pixels past it
    // on each side.
    expect(mask.contains(QPoint(270, 240)), "the pinned image takes a click");
    expect(mask.contains(QPoint(148, 240)), "the pin's border takes a click");
    expect(mask.contains(QPoint(150, 150)), "the image's corner takes a click");
    // The toolbar is the other thing the editor owns, and it is what the
    // surface was grown for.
    QWidget *toolbar = nullptr;
    for (QWidget *child : overlay->findChildren<QWidget *>()) {
        if (child->objectName() == QStringLiteral("vshotToolbar") && child->isVisible()) {
            toolbar = child;
            break;
        }
    }
    if (toolbar == nullptr) {
        expect(false, "the editor shows a toolbar");
        return;
    }
    expect(mask.contains(toolbar->geometry().center()), "the toolbar takes a click");

    // Everything else on the screen belongs to whatever is behind the editor.
    const QPoint outside[] = {
        QPoint(20, 20),    // the far corner of the surface
        QPoint(60, 240),   // level with the image, well to its left
        QPoint(270, 370),  // below the image, past the border
        QPoint(370, 20),   // above and to the right
    };
    for (const QPoint &point : outside) {
        expect(!mask.contains(point), "the editor passes a click outside its chrome through",
               QStringLiteral("point %1,%2").arg(point.x()).arg(point.y()));
    }
}

// Every step of a pin-editor gesture has to repaint what it changed, on the
// canvas as well as on the image: the marks are drawn against the image's
// *current* rect, which is not the rect the session recorded.
void checkPinEditStepsCoverTheirChange()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(pinEditSession());
    controller.setPinEditMode(true);
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPinEdit();

    // Dragging the image: the selection chrome and the magnifier travel with it.
    controller.chooseTool(std::nullopt);
    controller.press(overlay, QPointF(200, 200), Qt::LeftButton, Qt::NoModifier);
    expectStepCovered(controller, overlay, QPointF(210, 208),
                      "dragging a pin invalidates where it drew");
    expectStepCovered(controller, overlay, QPointF(220, 216),
                      "a moving pin invalidates where it draws");
    expectStepCovered(controller, overlay, QPointF(215, 212),
                      "a moving pin invalidates its last place");
    controller.release(overlay, QPointF(215, 212), Qt::LeftButton, Qt::NoModifier);

    // Drawing on the image: the growing pen preview has to cover each new
    // segment, even where the image stands outside the session's recorded rect.
    controller.chooseTool(vshot::Tool::Pen);
    controller.setWidth(5);
    controller.press(overlay, QPointF(180, 180), Qt::LeftButton, Qt::NoModifier);
    expectStepCovered(controller, overlay, QPointF(280, 210),
                      "a pen stroke on a pin invalidates where it drew");
    expectStepCovered(controller, overlay, QPointF(360, 280),
                      "a growing pen stroke on a pin invalidates where it drew");
    controller.release(overlay, QPointF(360, 280), Qt::LeftButton, Qt::NoModifier);
}

// The magnifier samples the image, so in the pin editor it has to count from
// where the image is -- the daemon-confirmed rect -- and not from the rect the
// session recorded when the editor opened.  Counting from the stale one showed
// the wrong part of the picture as soon as a pin had been dragged: the
// "scrambled magnifier" the pin drag was blamed for.
//
// The loupe is raised with the right button rather than by the drag itself: a
// drag of the pinned image is the one gesture the magnifier deliberately stays
// out of (see `checkThePinDragRaisesNoMagnifier`), and the right button is the
// press whose whole purpose is the pixel.  `pinEditSession` already has the two
// rects apart -- the image is at 150,150 while the output's geometry still says
// 40,40 -- which is exactly the state a confirmation leaves behind.
void checkMovedPinLoupeFollowsTheImage()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(pinEditSession());
    controller.setPinEditMode(true);
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPinEdit();
    controller.chooseTool(std::nullopt);

    const vshot::OutputSession &output = controller.session().outputs.at(0);
    const QImage &source = output.image;
    const QSize size = overlay->size();
    constexpr int radius = 60;
    constexpr int margin = 10;

    // A pointer whose whole magnifier lands inside the image (and so inside the
    // image clip the overlay draws it through).
    const QPoint pointer(200, 200);
    controller.press(overlay, QPointF(pointer), Qt::RightButton, Qt::NoModifier);
    // The overlay's own paint(), not the widget: the floating toolbar is a
    // child that `render` would draw with it, and it is not what this reads.
    QImage frame(size, QImage::Format_ARGB32_Premultiplied);
    frame.fill(Qt::transparent);
    {
        QPainter painter(&frame);
        controller.paint(overlay, &painter);
    }
    controller.release(overlay, QPointF(pointer), Qt::RightButton, Qt::NoModifier);

    QPointF center(pointer.x() + radius * 1.1, pointer.y() + radius * 1.1);
    if (center.x() + radius > size.width() - margin) {
        center.setX(pointer.x() - radius * 1.1);
    }
    if (center.y() + radius > size.height() - margin) {
        center.setY(pointer.y() - radius * 1.1);
    }
    // The image's current rect is the session bounds, not output.geometry: the
    // sample counts from the image's origin, which is where the pin actually is.
    const int centerX = std::clamp(pointer.x() - controller.session().bounds.x, 0,
                                   source.width() - 1);
    const int centerY = std::clamp(pointer.y() - controller.session().bounds.y, 0,
                                   source.height() - 1);
    // The disc is placed in *overlay* logical pixels -- the surface is the whole
    // output and the image sits inside it -- while the sample indices above are
    // in the image's own pixels.  Reading one against the other is what the
    // sample below has to keep apart.
    const QPointF origin(center.x() - radius, center.y() - radius);

    int wrong = 0;
    // The lower half of the disc, and clear of the crosshair: the readout pill
    // hangs off the loupe and, when there is no room below it, is lifted over
    // the top instead -- where it covers the very pixels a sample taken there
    // would be reading.  The lower half is the part no pill ever takes.
    for (int i : {3, 11}) {
        for (int j : {9, 12}) {
            const QPoint at(static_cast<int>(origin.x()) + i * 8 + 4,
                            static_cast<int>(origin.y()) + j * 8 + 4);
            const QColor shown = frame.pixelColor(at);
            const int wantX = std::clamp(centerX - 7 + i, 0, source.width() - 1);
            const int wantY = std::clamp(centerY - 7 + j, 0, source.height() - 1);
            const QColor want = source.pixelColor(wantX, wantY);
            if (colorDistance(shown, want) > 2) {
                ++wrong;
            }
        }
    }
    expect(wrong == 0, "the magnifier follows an image that has been dragged",
           QStringLiteral("%1 of 4 samples wrong").arg(wrong));
}

// Writes a pin-edit session whose image is `pixels` wide and `logical` wide on
// screen, with a raw frame beside it, and returns the path to the session JSON.
// The image is 90 device pixels tall across 99 logical ones, and its output
// covers exactly the image, as the pin editor's widened surface does.
QString writeZoomedPinSession(const QString &dir, int pixels, int logical,
                              double declaredScale)
{
    const QString raw = dir + QStringLiteral("/pin.rgba");
    const QString path = dir + QStringLiteral("/session.json");
    QImage frame(pixels, 90, QImage::Format_RGBA8888);
    for (int y = 0; y < 90; ++y) {
        for (int x = 0; x < pixels; ++x) {
            frame.setPixelColor(x, y, QColor(x % 256, y % 256, (x * 3 + y * 5) % 256, 255));
        }
    }
    QFile rawFile(raw);
    if (!rawFile.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        rawFile.write(reinterpret_cast<const char *>(frame.constBits()),
                      static_cast<qint64>(frame.sizeInBytes())) !=
            static_cast<qint64>(frame.sizeInBytes())) {
        return QString();
    }

    QJsonObject rect;
    rect.insert(QStringLiteral("x"), 300);
    rect.insert(QStringLiteral("y"), 200);
    rect.insert(QStringLiteral("width"), logical);
    rect.insert(QStringLiteral("height"), 99);
    QJsonObject output;
    output.insert(QStringLiteral("id"), 0);
    output.insert(QStringLiteral("name"), QStringLiteral("DP-1"));
    output.insert(QStringLiteral("x"), 300);
    output.insert(QStringLiteral("y"), 200);
    output.insert(QStringLiteral("width"), logical);
    output.insert(QStringLiteral("height"), 99);
    QJsonObject surface = output;
    surface.remove(QStringLiteral("id"));
    surface.remove(QStringLiteral("name"));
    output.insert(QStringLiteral("surface"), surface);
    // The ratio the pin's own pixels are shown at, as the Rust side writes it:
    // a real number, not a whole one.
    output.insert(QStringLiteral("scale"), declaredScale);
    output.insert(QStringLiteral("pixel_width"), pixels);
    output.insert(QStringLiteral("pixel_height"), 90);
    output.insert(QStringLiteral("path"), raw);
    QJsonArray outputs;
    outputs.append(output);

    QJsonObject session;
    session.insert(QStringLiteral("version"), 1);
    session.insert(QStringLiteral("mode"), QStringLiteral("pin-edit"));
    session.insert(QStringLiteral("bounds"), rect);
    session.insert(QStringLiteral("id"), 7);
    session.insert(QStringLiteral("socket"), QStringLiteral("/tmp/vshot-check.sock"));
    session.insert(QStringLiteral("outputs"), outputs);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(QJsonDocument(session).toJson(QJsonDocument::Compact)) < 0) {
        return QString();
    }
    return path;
}

// The daemon hands a re-edit the marks it kept and the width of the pin's own
// border, and both have to survive the session reader: the marks are what the
// editor opens on, and the border width is what tells it the rim is part of the
// pin.  Both are optional, so a first edit -- a session with neither -- still
// has to load.
void checkPinEditSessionCarriesItsMarksAndBorder()
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        expect(false, "a temporary directory for a pin-edit session");
        return;
    }
    const QString path = writeZoomedPinSession(dir.path(), 160, 176, 160.0 / 176.0);
    if (path.isEmpty()) {
        expect(false, "the pin-edit session is written");
        return;
    }

    // The same session with the two optional fields added, as `startEdit`
    // writes them for a pin that has been annotated before.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        expect(false, "the session can be read back");
        return;
    }
    QJsonObject session = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    const QJsonObject mark{{QStringLiteral("kind"), QStringLiteral("shape")},
                           {QStringLiteral("tool"), QStringLiteral("rectangle")},
                           {QStringLiteral("color"), QStringLiteral("#ff4040")},
                           {QStringLiteral("width"), 2},
                           {QStringLiteral("dash"), QStringLiteral("solid")},
                           {QStringLiteral("mask"), QStringLiteral("rect")},
                           {QStringLiteral("strength"), 2},
                           {QStringLiteral("rect"),
                            QJsonObject{{QStringLiteral("x"), 10},
                                        {QStringLiteral("y"), 12},
                                        {QStringLiteral("width"), 40},
                                        {QStringLiteral("height"), 30}}}};
    session.insert(QStringLiteral("annotations"), QJsonArray{mark});
    session.insert(QStringLiteral("border_width"), 6);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(QJsonDocument(session).toJson(QJsonDocument::Compact)) < 0) {
        expect(false, "the session with marks can be written");
        return;
    }
    file.close();

    vshot::Session loaded;
    QString error;
    expect(vshot::loadSession(path, &loaded, &error),
           "a session carrying marks loads", error);
    expect(loaded.annotations.size() == 1, "the marks reach the editor",
           QString::number(loaded.annotations.size()));
    expect(loaded.pinBorderWidth == 6, "the pin's border width reaches the editor",
           QString::number(loaded.pinBorderWidth));

    // And a session that names neither -- a pin's very first edit -- is still a
    // session: the fields are optional, not required.
    QJsonObject bare = session;
    bare.remove(QStringLiteral("annotations"));
    bare.remove(QStringLiteral("border_width"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(QJsonDocument(bare).toJson(QJsonDocument::Compact)) < 0) {
        expect(false, "the bare session can be written");
        return;
    }
    file.close();
    vshot::Session plain;
    QString plainError;
    expect(vshot::loadSession(path, &plain, &plainError),
           "a pin's first edit needs neither field", plainError);
    expect(plain.annotations.isEmpty() && plain.pinBorderWidth == 0,
           "and reads them as absent rather than as a failure");
}

// A pin the user has zoomed is annotated on an image that is not a whole number
// of device pixels per logical pixel: a 160-pixel pin shown across 176 logical
// pixels is 0.909.  The session used to declare that ratio rounded to 1, so its
// pixel dimensions no longer matched its logical size and the editor refused
// the session outright -- clicking a zoomed pin did nothing at all, and the
// marks the editor did manage to make were rasterized at the wrong size and in
// the wrong place, which is what read as the annotation jumping when the pin
// was clicked.  The ratio is now carried as it is, so the session loads and the
// marks are drawn at the size the image is really shown at.
void checkZoomedPinEditSessionLoads()
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        expect(false, "a temporary directory for a zoomed pin-edit session");
        return;
    }
    // The 1.1x zoom the wheel's first notch gives on a 160-pixel pin.
    constexpr int kPixels = 160;
    constexpr int kLogical = 176;
    const double scale = static_cast<double>(kPixels) / kLogical;
    const QString path = writeZoomedPinSession(dir.path(), kPixels, kLogical, scale);
    if (path.isEmpty()) {
        expect(false, "the zoomed pin-edit session and its frame are written");
        return;
    }

    vshot::Session session;
    QString error;
    const bool loaded = vshot::loadSession(path, &session, &error);
    expect(loaded, "a zoomed pin's session loads rather than being refused", error);
    if (!loaded) {
        return;
    }
    expect(std::abs(session.outputs.at(0).scale - scale) < 1e-9,
           "and keeps the ratio it declared rather than rounding it to a whole one",
           QStringLiteral("scale %1").arg(session.outputs.at(0).scale));
    expect(session.outputs.at(0).image.size() == QSize(kPixels, 90),
           "and loads the frame at its own pixel size, not the logical one");
    expect(session.bounds.x == 300 && session.bounds.y == 200 &&
               session.bounds.width == kLogical && session.bounds.height == 99,
           "and keeps the image's logical rect as the session bounds",
           QStringLiteral("bounds %1,%2 %3x%4")
               .arg(session.bounds.x)
               .arg(session.bounds.y)
               .arg(session.bounds.width)
               .arg(session.bounds.height));

    // The same session with the ratio truncated to 1 is the one that used to be
    // written, and it must still be refused: the dimensions it declares no
    // longer agree with the frame it names, so drawing it would place every
    // mark against the wrong pixels.
    const QString truncated =
        writeZoomedPinSession(dir.path(), kPixels, kLogical, 1.0);
    vshot::Session refused;
    QString refusal;
    expect(!vshot::loadSession(truncated, &refused, &refusal) &&
               refusal.contains(QStringLiteral("do not match")),
           "a zoomed pin's session with the ratio rounded away is still refused",
           refusal);
}

// A drag's latency is dominated by how it talks to the daemon.  A fresh socket
// per motion event costs a connect, an accept and a fresh object on both sides,
// and the editor used to allow exactly one request in flight, so the next
// position had to wait for the whole round trip before it was even sent.  This
// drives the editor against a stand-in daemon and pins down that a drag keeps
// one connection and pipelines its positions over it.
void checkPinDragKeepsOneConnection()
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        expect(false, "a temporary directory for the stand-in daemon");
        return;
    }
    const QString path = dir.filePath(QStringLiteral("pin.sock"));
    QLocalServer::removeServer(path);
    QLocalServer server;
    if (!server.listen(path)) {
        expect(false, "the stand-in daemon listens", server.errorString());
        return;
    }

    int connections = 0;
    QVector<QPoint> requested;
    QByteArray buffer;
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        while (QLocalSocket *socket = server.nextPendingConnection()) {
            ++connections;
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [&, socket] {
                buffer += socket->readAll();
                qsizetype newline = -1;
                while ((newline = buffer.indexOf('\n')) >= 0) {
                    const QByteArray line = buffer.left(newline);
                    buffer.remove(0, newline + 1);
                    const QJsonObject request = QJsonDocument::fromJson(line).object();
                    const int x = request.value(QStringLiteral("x")).toInt();
                    const int y = request.value(QStringLiteral("y")).toInt();
                    requested.append(QPoint(x, y));
                    // Echo the position back the way the daemon answers.
                    QJsonObject reply{{QStringLiteral("ok"), true},
                                      {QStringLiteral("x"), x},
                                      {QStringLiteral("y"), y},
                                      {QStringLiteral("width"), 240},
                                      {QStringLiteral("height"), 180}};
                    QByteArray out = QJsonDocument(reply).toJson(QJsonDocument::Compact);
                    out.append('\n');
                    socket->write(out);
                    socket->flush();
                }
            });
        }
    });

    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(pinEditSession());
    controller.setPinEditMode(true);
    controller.setPinTarget(1, path);
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPinEdit();
    controller.chooseTool(std::nullopt);

    // Drag the pin up and to the left, inside its clamp, one motion event at a
    // time with the event loop turning between them so the socket can breathe.
    // The middle button is held because that is what starts a move of the image.
    const QPointF from(200, 200);
    controller.press(overlay, from, Qt::MiddleButton, Qt::NoModifier);
    QPointF to = from;
    for (int step = 1; step <= 12; ++step) {
        to = from + QPointF(-step * 4, -step * 2);
        controller.move(overlay, to, Qt::MiddleButton, Qt::NoModifier);
        QCoreApplication::processEvents();
    }
    controller.release(overlay, to, Qt::MiddleButton, Qt::NoModifier);
    QElapsedTimer drain;
    drain.start();
    while (drain.elapsed() < 300) {
        QCoreApplication::processEvents();
    }

    expect(connections == 1, "a pin drag keeps one connection to the daemon",
           QStringLiteral("%1 connections opened").arg(connections));
    expect(requested.size() >= 3, "the drag pipelines its positions over it",
           QStringLiteral("%1 positions sent").arg(requested.size()));
    expect(controller.selection().has_value() &&
               controller.selection()->x == requested.constLast().x() &&
               controller.selection()->y == requested.constLast().y(),
           "the daemon's answer anchors the editor",
           QStringLiteral("selection %1,%2 vs last request %3,%4")
               .arg(controller.selection().has_value() ? controller.selection()->x : -1)
               .arg(controller.selection().has_value() ? controller.selection()->y : -1)
               .arg(requested.isEmpty() ? -1 : requested.constLast().x())
               .arg(requested.isEmpty() ? -1 : requested.constLast().y()));
}

// The overlays outlive the controller: `main` deletes them after the
// controller's scope has ended.  So the controller must not delete them too,
// and must leave nothing pointing at itself, or the caller's delete (or the
// next repaint) runs through freed memory.
void checkOverlayOutlivesItsController()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::CaptureOverlay *overlay = nullptr;
    {
        vshot::OverlayController controller(editingSession());
        QString error;
        overlay = controller.addOverlay(0, screen, &error);
        if (overlay == nullptr) {
            expect(false, "the controller accepts an overlay", error);
            return;
        }
        overlay->show();
        controller.beginPresetEdit();
        QCoreApplication::processEvents();
    }
    // The controller is gone.  The overlay is hidden, and deleting it here --
    // the one delete, as `main` does it -- must be safe.
    expect(overlay != nullptr && !overlay->isVisible(),
           "a controller that dies hides the overlays it was driving");
    delete overlay;
    expect(true, "the overlay the caller owns is deleted once");
}

// The magnifier follows a gesture that is picking something out of the frozen
// frame.  A pinned image is not that: the pointer is placing a picture that is
// already the right pixels, so a loupe there magnifies the very thing it is
// over and says nothing the user can act on.
//
// What the check reads is the *difference* a drag makes, rather than the pixels
// themselves.  The editor draws the pinned image's outline and the loupe draws
// a ring, a crosshair and a readout pill, so counting lit pixels would count
// all of that; and the loupe magnifies whatever is under it, so matching the
// pixels against the frame would have to know exactly which of them the ring
// and the pill cover.  Painting the same pointer twice -- once with the button
// down and travelling, once with no gesture at all -- and taking the two apart
// leaves the one thing the drag is responsible for.  The pen stroke drawn right
// after is the control: it makes the same reading non-zero, so "nothing" means
// the drag drew nothing and not that the overlay never paints.
void checkThePinDragRaisesNoMagnifier()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.setPinEditMode(true);
    controller.beginPinEdit();

    // The overlay's own paint(), not the widget: the floating toolbar is a
    // child that `render` would draw with it, and it is not what this reads.
    const auto painted = [&] {
        QImage target(overlay->size(), QImage::Format_ARGB32_Premultiplied);
        target.fill(Qt::transparent);
        QPainter painter(&target);
        controller.paint(overlay, &painter);
        painter.end();
        return target;
    };
    const auto changed = [](const QImage &before, const QImage &after) {
        int count = 0;
        for (int y = 0; y < after.height(); ++y) {
            for (int x = 0; x < after.width(); ++x) {
                if (before.pixel(x, y) != after.pixel(x, y)) {
                    ++count;
                }
            }
        }
        return count;
    };

    // The pointer's own resting place, painted with nothing under way.  Both
    // readings below are taken against this, so what they hold is the gesture.
    controller.chooseTool(std::nullopt);
    const QPoint pointer(180, 180);
    controller.move(overlay, QPointF(pointer), Qt::NoButton, Qt::NoModifier);
    const QImage resting = painted();

    // A press inside the image, then a travel: the drag that moves the pin.
    // The middle button is the one that places a pinned image -- a bare left
    // press there is a click that starts no gesture at all -- so it is the one
    // that raises the gesture the magnifier has to stay out of.
    controller.press(overlay, QPointF(150, 150), Qt::MiddleButton, Qt::NoModifier);
    controller.move(overlay, QPointF(pointer), Qt::MiddleButton, Qt::NoModifier);
    const int dragged = changed(resting, painted());
    expect(dragged == 0, "dragging a pinned image raises no magnifier",
           QStringLiteral("%1 px the drag put on the surface").arg(dragged));
    controller.release(overlay, QPointF(pointer), Qt::MiddleButton, Qt::NoModifier);

    // The same editor does paint at that pointer: a pen stroke is drawn on the
    // surface as it grows, which is what makes the reading above mean "no
    // magnifier" rather than "this overlay never paints".
    controller.move(overlay, QPointF(pointer), Qt::NoButton, Qt::NoModifier);
    const QImage beforeStroke = painted();
    controller.chooseTool(vshot::Tool::Pen);
    controller.press(overlay, QPointF(pointer), Qt::LeftButton, Qt::NoModifier);
    controller.move(overlay, QPointF(pointer.x() + 40, pointer.y() + 30), Qt::LeftButton,
                    Qt::NoModifier);
    const int stroke = changed(beforeStroke, painted());
    expect(stroke > 0, "the pin editor still draws the marks it is given",
           QStringLiteral("%1 px the stroke put on the surface").arg(stroke));
    controller.release(overlay, QPointF(pointer.x() + 40, pointer.y() + 30), Qt::LeftButton,
                       Qt::NoModifier);
}

// The eyedropper: one click reads the pixel under it, hands the colour to the
// tool that was armed when it was chosen, and leaves the session on that tool.
// The pick takes the colour only -- the tool keeps the opacity the user set --
// and what it took is reported back, so a caller can say so.
void checkTheEyedropperTakesThePixelItIsPointedAt()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    // A translucent pen, so "the pick keeps the opacity" is a claim the check
    // can see: a pick that took the whole pixel would make the pen opaque.
    controller.chooseTool(vshot::Tool::Pen);
    QColor pen = controller.toolStyle(QStringLiteral("pen")).color;
    pen.setAlpha(120);
    controller.setCurrentColor(pen);
    expect(!controller.pickedColor().isValid(), "nothing has been picked before the first click");

    // The point the loupe and the pick both map through: the overlay's local
    // point floors to this pixel, and the check reads the very same one.
    const QPointF local(100.5, 60.5);
    const QColor under = overlay->output().image.pixelColor(100, 60);
    controller.chooseTool(vshot::Tool::Picker);
    expect(controller.currentTool() == std::optional<vshot::Tool>(vshot::Tool::Picker),
           "the eyedropper arms");
    expect(controller.pickerTarget() == vshot::Tool::Pen,
           "and knows which tool its colour is for");
    controller.press(overlay, local, Qt::LeftButton, Qt::NoModifier);
    controller.release(overlay, local, Qt::LeftButton, Qt::NoModifier);

    const QColor taken = controller.toolStyle(QStringLiteral("pen")).color;
    expect(taken.rgb() == under.rgb(), "the pen takes the pixel the click was on",
           QStringLiteral("pen %1, pixel %2").arg(taken.name(), under.name()));
    expect(taken.alpha() == 120, "and keeps the opacity it was drawn with",
           QStringLiteral("alpha %1").arg(taken.alpha()));
    expect(controller.pickedColor().rgb() == under.rgb(),
           "the pick is reported back as it was taken");
    expect(controller.currentTool() == std::optional<vshot::Tool>(vshot::Tool::Pen),
           "and the pick hands the session back to the pen");
    expect(controller.annotations().isEmpty(), "the picker draws no mark",
           QStringLiteral("%1 of them").arg(controller.annotations().size()));

    // With nothing armed -- the state a session starts in, and what the old
    // Select tool became -- there is no tool to hand the colour to, and it
    // still has somewhere to go: the pen is the session's own ink, and the pick
    // leaves the user on it.
    controller.chooseTool(std::nullopt);
    controller.chooseTool(vshot::Tool::Picker);
    expect(controller.pickerTarget() == vshot::Tool::Pen,
           "a pick with no tool armed is for the pen");
    controller.press(overlay, local, Qt::LeftButton, Qt::NoModifier);
    expect(controller.currentTool() == std::optional<vshot::Tool>(vshot::Tool::Pen),
           "and leaves the session on it");
}

// The eyedropper's loupe follows an idle pointer -- that is the whole
// instrument, and the readout under it is the colour a click would take -- and
// it goes with the tool: a drawing tool ignores an idle pointer, so switching
// to one takes the magnified frame off the surface.  The step that moves it
// repaints the box it left as well as the one it entered, which is what keeps
// a stale loupe from being left behind.

// The eyedropper's loupe follows an idle pointer -- that is the whole
// instrument, and the readout under it is the colour a click would take -- and
// it goes with the tool: a drawing tool ignores an idle pointer, so switching
// to one takes the magnified frame off the surface.  The step that moves it
// repaints the box it left as well as the one it entered, which is what keeps
// a stale loupe from being left behind.
void checkTheEyedropperLoupeFollowsThePointer()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession());
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    // The overlay's own paint(), not the widget: the floating toolbar is a child
    // that would be rendered with it, and it is not what this check is about.
    const auto painted = [&] {
        QImage target(overlay->size(), QImage::Format_ARGB32_Premultiplied);
        target.fill(Qt::transparent);
        QPainter painter(&target);
        controller.paint(overlay, &painter);
        painter.end();
        return target;
    };
    const auto changedBox = [](const QImage &before, const QImage &after, int *count) {
        QRect box;
        int pixels = 0;
        for (int y = 0; y < after.height(); ++y) {
            for (int x = 0; x < after.width(); ++x) {
                if (before.pixel(x, y) == after.pixel(x, y)) {
                    continue;
                }
                ++pixels;
                box = box.isNull() ? QRect(x, y, 1, 1) : box.united(QRect(x, y, 1, 1));
            }
        }
        if (count != nullptr) {
            *count = pixels;
        }
        return box;
    };

    // A drawing tool ignores an idle pointer: the frozen frame is what it is,
    // and the crosshair is the cursor's business.
    controller.chooseTool(vshot::Tool::Pen);
    controller.move(overlay, QPointF(100, 100), Qt::NoButton, Qt::NoModifier);
    const QImage idle = painted();
    controller.move(overlay, QPointF(260, 200), Qt::NoButton, Qt::NoModifier);
    int idlePixels = 0;
    changedBox(idle, painted(), &idlePixels);
    expect(idlePixels == 0, "an idle pointer paints nothing under a drawing tool",
           QStringLiteral("%1 px changed").arg(idlePixels));

    // The loupe hangs beside the pointer and its readout under it, so every
    // pixel the eyedropper's hover puts on the surface is near the pointer.
    controller.chooseTool(vshot::Tool::Picker);
    controller.move(overlay, QPointF(120, 120), Qt::NoButton, Qt::NoModifier);
    controller.chooseTool(vshot::Tool::Pen);
    const QImage beforeHover = painted();
    controller.chooseTool(vshot::Tool::Picker);
    int loupePixels = 0;
    const QRect loupe = changedBox(beforeHover, painted(), &loupePixels);
    expect(loupePixels > 0, "the eyedropper's loupe follows the pointer",
           QStringLiteral("%1 px").arg(loupePixels));
    constexpr int reach = 200;
    const QRect aroundPointer(120 - reach, 120 - reach, 2 * reach, 2 * reach);
    expect(aroundPointer.contains(loupe), "and stays beside the pointer",
           QStringLiteral("loupe %1,%2 %3x%4")
               .arg(loupe.x())
               .arg(loupe.y())
               .arg(loupe.width())
               .arg(loupe.height()));

    // The step's own repaint rect has to cover both where the loupe was and
    // where it went; a rect that covered only the new place would leave the old
    // magnified frame on the surface.
    controller.move(overlay, QPointF(200, 160), Qt::NoButton, Qt::NoModifier);
    expectStepCoveredBy(controller, overlay,
                        [&] {
                            controller.move(overlay, QPointF(300, 280), Qt::NoButton,
                                            Qt::NoModifier);
                        },
                        "the eyedropper's hover repaints the box it left and the one it entered");
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // The controller reads the config at construction, so the run needs a
    // config directory of its own: the check that switches `editor.selectMode`
    // writes into it, and every other check reads the built-in defaults out of
    // it rather than whatever the machine running it happens to have.
    QTemporaryDir configHome;
    if (!configHome.isValid()) {
        std::printf("FAIL  no temporary directory to keep the config in\n");
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());

    checkRepaintsReuseTheRaster();
    checkTheDefaultToolIsArmedWhenAnnotationBegins();
    checkEachMarkCachesOnItsOwn();
    checkCachedPixelsLandOnTheMark();
    checkTranslucentColorSerializesWithAlpha();
    checkOpaqueColorSerializesWithoutAlpha();
    checkPureMoveReusesTheRaster();
    checkPinConfirmationKeepsTheRasters();
    checkEachOutputKeepsItsOwnRaster();
    checkEdgeOfCanvasKeepsTheRaster();
    checkLiveStrokeMatchesTheCommittedMark();
    checkTextEditorMatchesTheCommittedLabel();
    checkLoupeShowsTheCursorPixelEverywhere();
    checkDraggingOffTheImageLeavesItWhereItIs();
    checkThePinsBorderMovesItButTakesNoInk();
    checkThePinFrameGoesWhenAnotherPinTakesOver();
    checkRestoredMarksCanBeSelected();
    checkPinEditTakesInputOnlyOverItsChrome();
    checkPinEditStepsCoverTheirChange();
    checkMovedPinLoupeFollowsTheImage();
    checkZoomedPinEditSessionLoads();
    checkPinEditSessionCarriesItsMarksAndBorder();
    checkPinDragKeepsOneConnection();
    checkOverlayOutlivesItsController();
    checkLiveStrokeSurvivesIncrementalRepaint();
    checkInteractiveUpdateCoversTheChange();
    checkTheMagnifierDoesNotFreezeTheDragUnderIt();
    checkWaveSerializesAsATwoPointStroke();
    checkNumberSerializesWithItsStyleAndSize();
    checkNumberBadgesCompareByCountAndStyle();
    checkBezierSerializesWithItsClosure();
    checkClosedPathsCompareByTheirClosure();
    checkMarksRoundTripThroughASession();
    checkImageAndTranslationMarksSurviveAReEdit();
    checkBezierFillsAtHalfAlpha();
    checkBezierStepCoverage();
    checkSelectModeDecidesWhatAPressPicksUp();
    checkThePinDragRaisesNoMagnifier();
    checkTheEyedropperTakesThePixelItIsPointedAt();
    checkTheEyedropperLoupeFollowsThePointer();
    checkHoldingShiftPicksAMarkUpWithoutArmingATool();
    checkThePickUpModifierFramesTheMarkUnderThePointer();
    checkOnlyTheRightButtonBringsUpTheColourPicker();
    checkWalkingTheCursorAsksTheCliToMoveThePointer();
    checkAStrokeInProgressFollowsTheCursorKeys();
    checkThePointerWarpEchoDoesNotPutTheMagnifierOut();
    checkTheMagnifierKeyDrawsTheMagnifier();
    checkTheMagnifierShowsTheMarks();
    checkThePinEditorAsksForThePointerWarpToo();
    checkTheColourPillShowsTheColourAndReadsOnIt();

    if (failures != 0) {
        std::printf("\n%d annotation cache checks failed\n", failures);
        return 1;
    }
    std::printf("\nall annotation cache checks passed\n");
    return 0;
}

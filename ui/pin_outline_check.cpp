// Offline check for the pin outline: the stroke a pinned image carries, and
// how it says which pin the keyboard would act on. It paints a real
// PinSurface offscreen and samples the border pixels, so what is checked is
// the pixels Qt actually produces rather than the constants' values.
//
// The property worth locking down is that the outline is a solid, fully
// opaque stroke of one colour -- light grey while a pin is idle, black while
// it is the pin this output last clicked -- with both states sharing one
// geometry, so a focus change can only ever recolour and never shift the edge
// or change its weight. A two-tone ring with a translucent outer edge used to
// serve the same purpose and read as noise on light content.
//
// Built only with `-DVSHOT_BUILD_CHECKS=ON`. Unlike the density check this one
// needs Qt Widgets and the offscreen platform plugin (run it with
// QT_QPA_PLATFORM=offscreen), but still no compositor and no layer shell: the
// surface is rendered straight into a QImage through QWidget::render(), and
// the layer-shell call that would need a compositor is simply never made.

#include "pin_surface.hpp"

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

#include <cstdio>

namespace {

int failures = 0;

// The colours the outline is defined by. Spelled out here on purpose: a check
// that imported them from the surface would pass for any value at all.
constexpr int kIdleR = 192;
constexpr int kIdleG = 192;
constexpr int kIdleB = 192;
constexpr int kActiveR = 0;
constexpr int kActiveG = 0;
constexpr int kActiveB = 0;

void expectPixel(const char *what, const QImage &image, const QPoint &logical, int r, int g, int b,
                 int a)
{
    // Sampling through the device ratio keeps the same logical coordinates
    // meaningful when the check is run at QT_SCALE_FACTOR=2.
    const qreal ratio = image.devicePixelRatio();
    const QPoint device(qRound(logical.x() * ratio), qRound(logical.y() * ratio));
    const QColor got = image.pixelColor(device);
    const bool ok = got.red() == r && got.green() == g && got.blue() == b && got.alpha() == a;
    if (ok) {
        std::printf("ok    %-48s rgba(%d, %d, %d, %d)\n", what, got.red(), got.green(),
                    got.blue(), got.alpha());
        return;
    }
    std::printf("FAIL  %-48s rgba(%d, %d, %d, %d), wanted rgba(%d, %d, %d, %d)\n", what,
                got.red(), got.green(), got.blue(), got.alpha(), r, g, b, a);
    ++failures;
}

void expectIdle(const char *what, const QImage &image, const QPoint &logical)
{
    expectPixel(what, image, logical, kIdleR, kIdleG, kIdleB, 255);
}

void expectActive(const char *what, const QImage &image, const QPoint &logical)
{
    expectPixel(what, image, logical, kActiveR, kActiveG, kActiveB, 255);
}

void expectTransparent(const char *what, const QImage &image, const QPoint &logical)
{
    expectPixel(what, image, logical, 0, 0, 0, 0);
}

QImage solid(int width, int height, QColor color)
{
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    return image;
}

// Paints the whole surface into an image of its own device resolution, the way
// the compositor would show it. The canvas carries the surface's device ratio,
// so QPainter's own device transform does the logical-to-device mapping: a
// painter.scale() on top of it would double the ratio and push the whole
// surface out of frame.
QImage paint(vshot::PinSurface &surface)
{
    const qreal ratio = surface.devicePixelRatioF();
    QImage canvas(surface.size() * ratio, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    canvas.setDevicePixelRatio(ratio);
    QPainter painter(&canvas);
    surface.render(&painter);
    painter.end();
    return canvas;
}

void click(vshot::PinSurface &surface, const QPoint &logical, const QPoint &global)
{
    // The release matters as much as the press: the press starts a drag, and a
    // surface that is still dragging ignores the pointer, which would make
    // every pointer assertion below pass or fail for the wrong reason.
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(logical), QPointF(global), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&surface, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(logical), QPointF(global), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&surface, &release);
}

// A pointer that moves onto a pin and off the surface again, which is what the
// edge rides on: the surface only sees the pointer while it is over a pin,
// because that is its input region.
void moveOnto(vshot::PinSurface &surface, const QPoint &logical, const QPoint &global)
{
    const QPointF local(logical);
    const QPointF screen(global);
    QEnterEvent enter(local, local, screen);
    QApplication::sendEvent(&surface, &enter);
}

void moveAway(vshot::PinSurface &surface)
{
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&surface, &leave);
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        std::printf("FAIL  no screen\n");
        return 1;
    }
    const QPoint base = screen->geometry().topLeft();
    std::printf("screen %s: %dx%d, %.2f device pixels per logical pixel\n",
                qPrintable(screen->name()), screen->geometry().width(),
                screen->geometry().height(), screen->devicePixelRatio());

    // No argument means no PNGs at all: QDir("") is the working directory, so
    // it would otherwise drop the dumps next to whatever the check was run
    // from, the same as the other checks avoid.
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

    vshot::PinSurface surface(screen);
    surface.resize(420, 300);
    surface.show();
    // The offscreen platform never activates a window by itself, and
    // QWidget::activateWindow() only asks the platform for it (requestActivate,
    // which offscreen drops on the floor), so the harness has to say so
    // directly -- otherwise no FocusIn ever arrives and the focused state
    // cannot be painted at all. Qt marks this entry point deprecated in favour
    // of the request-based one, which is precisely the one that cannot work
    // here.
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    QApplication::setActiveWindow(&surface);
    QT_WARNING_POP

    // Three pins: a white one (the hard case for a light stroke), a black one,
    // and a single pixel -- the smallest rect the stroke has to survive, where
    // an inward stroke would have no room at all.
    vshot::PinSurface::Item white;
    white.id = 1;
    white.image = solid(160, 100, Qt::white);
    white.origin = base + QPoint(20, 20);
    vshot::PinSurface::Item black;
    black.id = 2;
    black.image = solid(160, 100, Qt::black);
    black.origin = base + QPoint(20, 160);
    vshot::PinSurface::Item dot;
    dot.id = 3;
    dot.image = solid(1, 1, QColor(255, 0, 0));
    dot.origin = base + QPoint(220, 20);
    surface.setPins({white, black, dot});

    // The 2px stroke is centred on the image edge, so it covers exactly one
    // logical pixel outside the rect: 20..19 on the way in, 20+160..180 on the
    // way out, with a whole-pixel edge on both lines so the stroke is crisp
    // instead of fading over two rows.
    const QImage idle = paint(surface);
    leave(QStringLiteral("pin-outline-idle.png"), idle);
    expectIdle("idle: stroke above the white pin", idle, QPoint(60, 19));
    expectIdle("idle: stroke left of the white pin", idle, QPoint(19, 60));
    expectIdle("idle: stroke below the white pin", idle, QPoint(60, 120));
    expectIdle("idle: stroke right of the white pin", idle, QPoint(180, 60));
    expectPixel("idle: the image itself is untouched", idle, QPoint(60, 60), 255, 255, 255, 255);
    expectIdle("idle: stroke above the black pin", idle, QPoint(60, 159));
    expectPixel("idle: the black image is untouched", idle, QPoint(60, 200), 0, 0, 0, 255);
    expectTransparent("idle: nothing painted beyond the stroke", idle, QPoint(60, 17));
    expectIdle("idle: the one-pixel pin has a stroke", idle, QPoint(219, 20));
    expectTransparent("idle: the one-pixel stroke is its own size", idle, QPoint(218, 20));

    click(surface, QPoint(60, 60), base + QPoint(60, 60));
    // Focus is what turns the picked pin black, so it has to be real: without
    // it the picked pin reads as idle and the rest of the check is meaningless.
    if (surface.hasFocus()) {
        std::printf("ok    clicking a pin hands its surface the keyboard\n");
    } else {
        std::printf("FAIL  clicking a pin left the surface unfocused\n");
        ++failures;
    }

    const QImage active = paint(surface);
    leave(QStringLiteral("pin-outline-active.png"), active);
    expectActive("focused: stroke above the picked pin", active, QPoint(60, 19));
    expectActive("focused: stroke left of the picked pin", active, QPoint(19, 60));
    expectActive("focused: stroke below the picked pin", active, QPoint(60, 120));
    expectActive("focused: stroke right of the picked pin", active, QPoint(180, 60));
    expectPixel("focused: the image itself is untouched", active, QPoint(60, 60), 255, 255, 255,
                255);
    expectIdle("focused: the other pin stays idle", active, QPoint(60, 159));
    expectIdle("focused: the one-pixel pin stays idle", active, QPoint(219, 20));

    // Picking another pin has to move the black edge with it and leave the pin
    // that lost the focus grey again -- neither pin repaints on its own, the
    // recolour falls out of the stroke's colour depending on the state.
    click(surface, QPoint(220, 20), base + QPoint(220, 20));
    const QImage moved = paint(surface);
    leave(QStringLiteral("pin-outline-moved.png"), moved);
    expectActive("re-picked: the one-pixel pin is black", moved, QPoint(219, 20));
    expectIdle("re-picked: the pin that lost it is grey", moved, QPoint(60, 19));

    // The black edge is a claim about who a key press would reach, so losing the
    // keyboard has to take it back -- clicking a window does exactly this, and
    // only the compositor can say it happened. Deactivating the window is how
    // that arrives here, and it must land: a stale claim is worse than none.
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    QApplication::setActiveWindow(nullptr);
    QT_WARNING_POP
    if (surface.hasFocus()) {
        std::printf("FAIL  deactivating the surface left the widget focused\n");
        ++failures;
    }
    const QImage blurred = paint(surface);
    leave(QStringLiteral("pin-outline-unfocused.png"), blurred);
    expectIdle("unfocused: the picked pin is grey again", blurred, QPoint(219, 20));
    expectIdle("unfocused: the other pin is grey too", blurred, QPoint(60, 19));

    // Hovering a pin without the keyboard is not a claim the surface can make:
    // the edge would promise a key press would land there and it would not.
    moveOnto(surface, QPoint(60, 60), base + QPoint(60, 60));
    expectIdle("unfocused: hovering does not make a pin active", paint(surface), QPoint(60, 19));

    // The pick follows the pointer. Two compositors in the field never tell a
    // layer surface that it stopped being focused, so a pick that survives the
    // pointer leaving is a black edge the user cannot get rid of by clicking a
    // window -- and the pointer leaving is the signal that always arrives.
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    QApplication::setActiveWindow(&surface);
    QT_WARNING_POP
    surface.setFocus(Qt::MouseFocusReason);
    if (!surface.hasFocus()) {
        // Otherwise every assertion below would be about a keyboard the surface
        // does not have, and would pass for the wrong reason.
        std::printf("FAIL  could not hand the surface its keyboard back\n");
        ++failures;
    }
    moveOnto(surface, QPoint(60, 60), base + QPoint(60, 60));
    expectActive("keyboard back: the hovered pin is black", paint(surface), QPoint(60, 19));
    moveAway(surface);
    expectIdle("pointer away: the pin gives the edge up", paint(surface), QPoint(60, 19));
    moveOnto(surface, QPoint(60, 60), base + QPoint(60, 60));
    expectActive("pointer back: so does the pin under it", paint(surface), QPoint(60, 19));
    moveOnto(surface, QPoint(220, 20), base + QPoint(220, 20));
    const QImage hovered = paint(surface);
    expectActive("moving between pins moves the black edge", hovered, QPoint(219, 20));
    expectIdle("and the pin it left is grey", hovered, QPoint(60, 19));

    std::printf("--- result ---------------------------------------------------------\n");
    std::printf("%s (%d failure(s))\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// Offline check for the pin outline: the stroke a pinned image carries, how it
// says which pin the keyboard would act on, and the look the config file can
// change -- the corner radius, the shadow and the stroke's width and colour.
// It paints a real PinSurface offscreen and samples the border pixels, so what
// is checked is the pixels Qt actually produces rather than the constants'
// values.
//
// The property worth locking down is that the stroke is a solid, fully opaque
// line of one colour -- light grey while a pin is idle, black while it is the
// pin this output last clicked -- with both states sharing one geometry, so a
// focus change can only ever recolour and never shift the edge or change its
// weight. A two-tone ring with a translucent outer edge used to serve the same
// purpose and read as noise on light content.
//
// The shadow is the one thing that deliberately paints outside the pin, and the
// geometry half of this check turns it off so that "nothing is painted beyond
// the stroke" stays a meaningful measurement; the look half turns it back on.
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
    // The geometry half of this check is about the stroke, so the shadow -- the
    // one thing that paints outside a pin on purpose -- is off for it.  The
    // look half at the end turns it back on and checks it separately.
    vshot::PinSurface::Style plain;
    plain.shadow.enabled = false;
    surface.setStyle(plain);
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
    // A pin put back on the place it was captured from: it is placed by the
    // daemon's coordinates like any other pin, and this check is here to keep
    // its rim from being skipped.
    vshot::PinSurface::Item inPlace;
    inPlace.id = 4;
    inPlace.image = solid(160, 100, QColor(0, 128, 255));
    inPlace.origin = base + QPoint(230, 170);
    surface.setPins({white, black, dot, inPlace});

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
    // A pin put back on the place it was captured from is drawn like any other
    // pin: the rim is what tells the user the thing on the desktop is a pin and
    // not the window it covers, so it is the last thing to leave the picture.
    expectIdle("in place: a pin back on its own place keeps its rim", idle, QPoint(229, 220));
    expectIdle("in place: the rim covers the image's own edge", idle, QPoint(230, 220));
    expectIdle("in place: and its top edge too", idle, QPoint(300, 169));
    expectIdle("in place: the rim is on every side", idle, QPoint(300, 170));

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

    std::printf("--- the look the config file sets ----------------------------------\n");
    // What the settings window and the `pin` section of the config file can
    // change, checked as pixels: a rounded pin, a wider stroke, the user's own
    // two colours, and the shadow that is on by default.
    //
    // The pins are put back to their default state first, since the walk above
    // left the pointer hovering one of them.
    moveAway(surface);
    vshot::PinSurface::Style styled;
    styled.radius = 16;
    styled.shadow.enabled = true;
    styled.borderWidth = 4;
    styled.borderColor = QColor(0, 200, 0);
    styled.activeBorderColor = QColor(0, 0, 255);
    surface.setStyle(styled);
    const QImage rounded = paint(surface);
    leave(QStringLiteral("pin-outline-styled.png"), rounded);

    // The corner is now cut away: the pixel just inside the rect's top-left
    // corner is no longer the image, and the one further in still is.  That is
    // what "rounded" means as a pixel.  (The shadow is on in this phase, so
    // what the corner holds is shadow rather than nothing.)
    const QColor corner = rounded.pixelColor(
        QPoint(qRound(21 * rounded.devicePixelRatio()),
               qRound(21 * rounded.devicePixelRatio())));
    expectPixel("and the image is there a little further in", rounded, QPoint(30, 30), 255, 255,
                255, 255);
    if (corner.red() == 255 && corner.green() == 255 && corner.blue() == 255) {
        std::printf("FAIL  %-48s the corner is still the image\n",
                    "a rounded pin has its corner cut away");
        ++failures;
    } else {
        std::printf("ok    %-48s rgba(%d, %d, %d, %d)\n",
                    "a rounded pin has its corner cut away", corner.red(), corner.green(),
                    corner.blue(), corner.alpha());
    }

    // The stroke is the user's own colour and the width they asked for: 4px
    // centred on the edge covers 2 outside and 2 inside.
    expectPixel("the idle stroke uses the configured colour", rounded, QPoint(60, 18), 0, 200, 0,
                255);
    expectPixel("a 4px stroke reaches 2px inside the edge", rounded, QPoint(60, 21), 0, 200, 0,
                255);
    // Two pixels further out is past the stroke: whatever is there, it is not
    // the stroke's colour.
    const QColor beyond = rounded.pixelColor(
        QPoint(qRound(60 * rounded.devicePixelRatio()),
               qRound(16 * rounded.devicePixelRatio())));
    if (beyond.green() > 150 && beyond.red() < 100) {
        std::printf("FAIL  %-48s the stroke reaches further than its width\n",
                    "a 4px stroke stops where it should");
        ++failures;
    } else {
        std::printf("ok    %-48s rgba(%d, %d, %d, %d)\n", "a 4px stroke stops where it should",
                    beyond.red(), beyond.green(), beyond.blue(), beyond.alpha());
    }

    // The shadow is the one thing painted outside the pin: with it on there is
    // something below the stroke, and with it off there is not.
    moveOnto(surface, QPoint(60, 60), base + QPoint(60, 60));
    const QImage withShadow = paint(surface);
    vshot::PinSurface::Style noShadow = styled;
    noShadow.shadow.enabled = false;
    surface.setStyle(noShadow);
    const QImage withoutShadow = paint(surface);
    leave(QStringLiteral("pin-outline-shadow.png"), withShadow);
    leave(QStringLiteral("pin-outline-no-shadow.png"), withoutShadow);
    // Sampled below the pin, past the stroke: the shadow's own pixels live
    // there, and with the shadow off that band is empty.
    const QPoint below(60, 128);
    expectPixel("the shadow switched off paints nothing at all", withoutShadow, below, 0, 0, 0, 0);
    const QColor shadowPixel = withShadow.pixelColor(
        QPoint(qRound(below.x() * withShadow.devicePixelRatio()),
               qRound(below.y() * withShadow.devicePixelRatio())));
    if (shadowPixel.alpha() > 0) {
        std::printf("ok    %-48s alpha %d\n", "a shadow paints below the pin",
                    shadowPixel.alpha());
    } else {
        std::printf("FAIL  %-48s nothing painted below the pin\n",
                    "a shadow paints below the pin");
        ++failures;
    }

    // The shadow's own numbers are the user's, so each has to reach the pixels.
    // Measured as weight -- the sum of the alphas over a band -- rather than as
    // one pixel, because a shadow is a falloff and any single pixel of it moves
    // when the blur changes.  The band is the gap between the white pin and the
    // black one below it: everything else on this surface is opaque and would
    // drown the shadow out.
    const auto weight = [](const QImage &image, int fromY, int toY) {
        long total = 0;
        for (int y = fromY; y < toY && y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                total += qAlpha(image.pixel(x, y));
            }
        }
        return total;
    };
    const int bandTop = 122;
    const int bandBottom = 156;
    vshot::PinSurface::Style faint = styled;
    faint.shadow.opacity = 40;
    surface.setStyle(faint);
    const QImage faintShadow = paint(surface);
    if (weight(faintShadow, bandTop, bandBottom) < weight(withShadow, bandTop, bandBottom)) {
        std::printf("ok    %-48s %ld against %ld\n", "a lower opacity is a lighter shadow",
                    weight(faintShadow, bandTop, bandBottom),
                    weight(withShadow, bandTop, bandBottom));
    } else {
        std::printf("FAIL  %-48s %ld against %ld\n", "a lower opacity is a lighter shadow",
                    weight(faintShadow, bandTop, bandBottom),
                    weight(withShadow, bandTop, bandBottom));
        ++failures;
    }

    // A bigger size is a softer, further-reaching shadow.  Measured as the
    // number of partially transparent pixels on the whole surface, because a
    // shadow is the only thing here that is neither nothing nor opaque: a wider
    // reach covers more pixels, and no single column or band can be used -- the
    // pins below the white one cast their own shadows into the same rows.
    const auto shadowPixels = [](const QImage &image) {
        long count = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const int alpha = qAlpha(image.pixel(x, y));
                if (alpha > 0 && alpha < 255) {
                    ++count;
                }
            }
        }
        return count;
    };
    vshot::PinSurface::Style wide = styled;
    wide.shadow.size = 30;
    surface.setStyle(wide);
    const QImage wideShadow = paint(surface);
    const long nearPixels = shadowPixels(withShadow);
    const long farPixels = shadowPixels(wideShadow);
    if (farPixels > nearPixels) {
        std::printf("ok    %-48s %ld px against %ld px\n", "a larger size reaches further out",
                    farPixels, nearPixels);
    } else {
        std::printf("FAIL  %-48s %ld px against %ld px\n", "a larger size reaches further out",
                    farPixels, nearPixels);
        ++failures;
    }

    // The offset is baked into the silhouette the blur is built from, so it
    // belongs in the cache key as much as the size does: the shadow's own image
    // depends on it, and a surface that kept the image of the offset it was
    // first drawn with would show the same shadow whatever the config said.
    // Measured as weight in the band below the pin, where a dropped silhouette
    // puts more of its falloff.
    // The two paints are back to back with nothing else changed between them:
    // that is the case the key has to cover, since any other difference rebuilds
    // the image for its own reasons and hides the one being measured here.
    vshot::PinSurface::Style near = styled;
    near.shadow.size = 24;
    near.shadow.offset = 3;
    surface.setStyle(near);
    const QImage nearShadow = paint(surface);
    vshot::PinSurface::Style dropped = near;
    dropped.shadow.offset = 40;
    surface.setStyle(dropped);
    const QImage droppedShadow = paint(surface);
    const long nearWeight = weight(nearShadow, bandTop, bandBottom);
    const long droppedWeight = weight(droppedShadow, bandTop, bandBottom);
    if (droppedWeight > nearWeight) {
        std::printf("ok    %-48s %ld against %ld\n", "a dropped shadow lands further down",
                    droppedWeight, nearWeight);
    } else {
        std::printf("FAIL  %-48s %ld against %ld\n", "a dropped shadow lands further down",
                    droppedWeight, nearWeight);
        ++failures;
    }
    surface.setStyle(styled);

    // And a size of zero paints nothing, the same as switching it off: the
    // offset is what a blur-less shadow would still have to draw, and there is
    // nothing to draw with.
    vshot::PinSurface::Style flat = styled;
    flat.shadow.size = 0;
    surface.setStyle(flat);
    expectPixel("a shadow with no blur paints nothing", paint(surface), below, 0, 0, 0, 0);
    surface.setStyle(styled);

    // The active pin takes the second configured colour: the pointer is on the
    // white pin, so it is the one wearing the active stroke.
    expectPixel("the active stroke uses its own configured colour", withShadow, QPoint(60, 18), 0,
                0, 255, 255);
    expectPixel("the pin that is not picked keeps the idle colour", withShadow, QPoint(60, 159),
                0, 200, 0, 255);
    // A pin put back on its own place takes the configured rim and the shadow
    // with every other pin: neither is skipped for it.
    expectPixel("in place: the configured rim is drawn on it", rounded, QPoint(229, 220), 0, 200, 0,
                255);
    expectPixel("in place: the image still reaches its edge", rounded, QPoint(233, 220), 0, 128, 255,
                255);
    {
        const QColor shadow = withShadow.pixelColor(
            QPoint(qRound(300 * withShadow.devicePixelRatio()),
                   qRound(272 * withShadow.devicePixelRatio())));
        if (shadow.alpha() > 0) {
            std::printf("ok    %-48s alpha %d\n", "in place: and it casts its shadow",
                        shadow.alpha());
        } else {
            std::printf("FAIL  %-48s nothing painted below the pin\n",
                        "in place: and it casts its shadow");
            ++failures;
        }
    }

    // A pin with no stroke at all is the image and nothing else, and a pin with
    // square corners has its corner pixel back -- both are settings the user
    // can pick, so both are checked.
    vshot::PinSurface::Style bare;
    bare.shadow.enabled = false;
    bare.borderWidth = 0;
    surface.setStyle(bare);
    const QImage square = paint(surface);
    expectPixel("a square pin has its corner pixel", square, QPoint(21, 21), 255, 255, 255, 255);
    expectTransparent("and a pin with no stroke paints nothing outside itself", square,
                      QPoint(60, 19));

    // A stroke the user set to a transparent colour is a pin with no stroke:
    // `#00000000` is how the settings window spells "no rim", and the alpha is
    // the whole of what it says.  Qt's pen already drops a zero-alpha stroke, so
    // this is the SDR half of the pair -- the HDR copy the helper draws has to
    // agree with it, which is what `a_transparent_rim_colour_is_not_a_black_rim`
    // checks on the other side of the wire.
    vshot::PinSurface::Style invisible;
    invisible.shadow.enabled = false;
    invisible.borderWidth = 2;
    invisible.borderColor = QColor(0, 0, 0, 0);
    invisible.activeBorderColor = QColor(0, 0, 0, 0);
    surface.setStyle(invisible);
    const QImage rimless = paint(surface);
    expectTransparent("a transparent stroke paints nothing above the pin", rimless,
                      QPoint(60, 19));
    expectTransparent("nor to the left of it", rimless, QPoint(19, 60));
    expectPixel("and the image keeps its own edge", rimless, QPoint(20, 60), 255, 255, 255, 255);

    // Half alpha is a half-visible stroke, not a missing one: the colour is the
    // user's black at half opacity over nothing, so what lands is a black pixel
    // that is not fully opaque.
    vshot::PinSurface::Style half;
    half.shadow.enabled = false;
    half.borderWidth = 2;
    half.borderColor = QColor(0, 0, 0, 128);
    half.activeBorderColor = QColor(0, 0, 0, 128);
    surface.setStyle(half);
    const QImage translucent = paint(surface);
    {
        const QColor got = translucent.pixelColor(
            QPoint(qRound(60 * translucent.devicePixelRatio()),
                   qRound(19 * translucent.devicePixelRatio())));
        if (got.alpha() > 0 && got.alpha() < 255) {
            std::printf("ok    %-48s alpha %d\n", "a half-transparent stroke is half painted",
                        got.alpha());
        } else {
            std::printf("FAIL  %-48s alpha %d\n", "a half-transparent stroke is half painted",
                        got.alpha());
            ++failures;
        }
    }
    surface.setStyle(plain);

    // The corner tags are a fixed size and are anchored to a pin's own corner, so
    // on a pin smaller than a tag the tag reaches past the pin's rect.  A repaint
    // region computed from the pins alone therefore left the tag's outer pixels
    // behind whenever the pin was zoomed or dragged -- a ghost only a tagged pin
    // could show.  What is checked is the property itself: every pixel that
    // differs between two frames has to lie inside the region the surface says it
    // repainted, because a ghost is exactly a change outside that region.
    {
        vshot::PinSurface tagged(screen);
        tagged.resize(420, 300);
        tagged.show();
        vshot::PinSurface::Item pin;
        pin.id = 1;
        pin.image = solid(16, 16, Qt::white);
        pin.origin = base + QPoint(200, 170);
        pin.capturedHdr = true;
        tagged.setPins({pin});
        // The pointer on the pin is what puts the `HDR` marker up; the badge is
        // asked for directly, the way a save's outcome would ask for it.  Both
        // hang outside a pin this small.
        moveOnto(tagged, QPoint(208, 178), base + QPoint(208, 178));
        tagged.showMessage(1, QStringLiteral("Copied Hex"));
        const QImage before = paint(tagged);

        // Left and up, so the old tags' overhang is not swallowed by the box a
        // region built from the two pin rects alone would span anyway.
        pin.origin = base + QPoint(60, 60);
        tagged.setPins({pin});
        const QImage after = paint(tagged);

        const qreal ratio = after.devicePixelRatio();
        const QRect asked = tagged.repaintRegion();
        const QRect covered(qRound(asked.x() * ratio), qRound(asked.y() * ratio),
                            qRound(asked.width() * ratio), qRound(asked.height() * ratio));
        long stray = 0;
        for (int y = 0; y < after.height(); ++y) {
            for (int x = 0; x < after.width(); ++x) {
                if (after.pixel(x, y) != before.pixel(x, y) && !covered.contains(x, y)) {
                    ++stray;
                }
            }
        }
        if (stray == 0) {
            std::printf("ok    %-48s %dx%d %d,%d\n",
                        "a small pin's move repaints its corner tags", covered.width(),
                        covered.height(), covered.x(), covered.y());
        } else {
            std::printf("FAIL  %-48s %ld pixel(s) changed outside the repaint\n",
                        "a small pin's move repaints its corner tags", stray);
            ++failures;
        }
    }

    std::printf("--- result ---------------------------------------------------------\n");
    std::printf("%s (%d failure(s))\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}

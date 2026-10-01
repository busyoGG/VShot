// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// Offline check for the HDR marker: the `HDR` tag a pinned HDR capture carries
// while the pointer is over it.  It paints a real PinSurface offscreen and
// counts the pixels Qt actually produces, so what is checked is the tag rather
// than the branch that decides to draw it.
//
// What is worth locking down is *which* pin gets the tag and what it says.  A
// pinned HDR capture and the SDR copy the daemon keeps beside it are the same
// picture to the eye, so the tag is the only thing on screen that tells them
// apart -- and it belongs to the one pin under the pointer, not to the stack,
// not to the first HDR pin there is, and not to an SDR pin at all.  The ink
// carries the other half of the answer: white while this surface has left the
// pixels to the helper, muted grey while it is showing the SDR copy itself.
//
// Built only with `-DVSHOT_BUILD_CHECKS=ON`.  Like the outline check it needs
// Qt Widgets and the offscreen platform plugin (run it with
// QT_QPA_PLATFORM=offscreen) but no compositor and no layer shell: the surface
// is rendered straight into a QImage through QWidget::render(), and the
// layer-shell call that would need a compositor is never made.

#include "pin_surface.hpp"

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

#include <algorithm>
#include <cstdio>

namespace {

int failures = 0;

// The pin images are this exact grey, so every other value inside a pin's rect
// is something the surface drew on top of it.
constexpr int kPinLevel = 128;

// The tag's two inks.  The white one is the HDR state, the grey one the
// fallback; both are spelled out here on purpose, because a check that imported
// them from the surface would pass for any value at all.  The thresholds sit
// just inside each ink, so a pixel of either is counted as that ink and nothing
// blended between them is mistaken for one.
constexpr int kShownInk = 255;
constexpr int kFallbackInk = 192;
constexpr int kBrightFrom = 200;
constexpr int kMutedFrom = 180;

// What a rect of the painted surface holds: pixels the surface drew over the
// image (`changed`), the brightest of them -- which is the tag's ink at the
// core of a stroke or a glyph -- and how many are the bright ink, the muted ink
// and the tag's own box.
struct Counts {
    long changed = 0;
    long bright = 0;
    long muted = 0;
    long dark = 0;
    int maxLevel = 0;
};

// One pixel, for the geometry: a tag is only worth counting inside a rect that
// is known to be the pin's.
void expectPixel(const char *what, const QImage &image, const QPoint &logical, int r, int g, int b,
                 int a)
{
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

void expectTransparent(const char *what, const QImage &image, const QPoint &logical)
{
    expectPixel(what, image, logical, 0, 0, 0, 0);
}

Counts countIn(const QImage &image, const QRect &logical)
{
    Counts counts;
    // Sampling through the device ratio keeps the same logical coordinates
    // meaningful when the check is run at QT_SCALE_FACTOR=2.
    const qreal ratio = image.devicePixelRatio();
    const QRect device(qRound(logical.x() * ratio), qRound(logical.y() * ratio),
                       qRound(logical.width() * ratio), qRound(logical.height() * ratio));
    for (int y = device.top(); y < device.bottom(); ++y) {
        for (int x = device.left(); x < device.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() == 0) {
                // Transparent: no pin here at all, so nothing to count.
                continue;
            }
            const int level = std::max(pixel.red(), std::max(pixel.green(), pixel.blue()));
            if (level == kPinLevel && pixel.alpha() == 255) {
                continue;
            }
            ++counts.changed;
            counts.maxLevel = std::max(counts.maxLevel, level);
            if (level >= kBrightFrom) {
                ++counts.bright;
            } else if (level >= kMutedFrom) {
                ++counts.muted;
            } else if (level <= 60) {
                ++counts.dark;
            }
        }
    }
    return counts;
}

// The state a pin's rect is in: nothing drawn over it, or the tag drawn in one
// known ink.
//
// The ink is checked through the brightest pixel of the rect alone, because
// that is exactly the ink: a glyph's core and a one-pixel stroke are drawn in
// it, and antialiasing only ever blends away from it.  Counting pixels per ink
// instead would trip over the corners, where a pixel that is half stroke and
// half box reads as whatever those two happen to blend to.
void expectTag(const char *what, const QImage &image, const QRect &rect, int ink)
{
    const Counts counts = countIn(image, rect);
    const bool ok = counts.changed > 0 && counts.dark > 0 && counts.maxLevel == ink;
    if (ok) {
        std::printf("ok    %-48s changed=%ld max=%d bright=%ld muted=%ld dark=%ld\n", what,
                    counts.changed, counts.maxLevel, counts.bright, counts.muted, counts.dark);
        return;
    }
    std::printf(
        "FAIL  %-48s changed=%ld max=%d bright=%ld muted=%ld dark=%ld (wanted ink %d)\n", what,
        counts.changed, counts.maxLevel, counts.bright, counts.muted, counts.dark, ink);
    ++failures;
}

void expectNothing(const char *what, const QImage &image, const QRect &rect)
{
    const Counts counts = countIn(image, rect);
    if (counts.changed == 0) {
        std::printf("ok    %-48s nothing painted over the pin\n", what);
        return;
    }
    std::printf("FAIL  %-48s changed=%ld, wanted nothing\n", what, counts.changed);
    ++failures;
}

QImage solid(int width, int height, QColor color)
{
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(color);
    return image;
}

// Paints the whole surface into an image of its own device resolution, the way
// the compositor would show it.  The canvas carries the surface's device ratio,
// so QPainter's own device transform does the logical-to-device mapping.
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

// A pointer that arrives on a pin and then moves: the surface only sees the
// pointer while it is over a pin, because that is its input region, so a hover
// is an enter followed by a move.
void hover(vshot::PinSurface &surface, const QPoint &logical, const QPoint &global)
{
    const QPointF local(logical);
    const QPointF screen(global);
    QEnterEvent enter(local, local, screen);
    QApplication::sendEvent(&surface, &enter);
    QMouseEvent move(QEvent::MouseMove, local, screen, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&surface, &move);
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
    // it would otherwise drop the dumps next to whatever the check was run from.
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
    // Nothing but the tag may be painted over the images: the shadow paints
    // outside a pin and the rim paints over its edge, and either would make
    // "the only non-image pixels are the tag" a wrong measurement.
    vshot::PinSurface::Style bare;
    bare.shadow.enabled = false;
    bare.borderWidth = 0;
    surface.setStyle(bare);

    // Three pins of the same grey.  The marker belongs to a pin's *own* state
    // -- it is an HDR capture whatever surface is showing it -- and what this
    // checks is which pin under the pointer carries it and in what ink.  Which
    // pins the helper has taken the pixels of is a separate question, and it is
    // the subject of the two paint checks below.
    const QImage grey = solid(160, 100, QColor(kPinLevel, kPinLevel, kPinLevel));
    const QRect hdrRect = QRect(base + QPoint(20, 20), grey.size());
    const QRect sdrRect = QRect(base + QPoint(20, 150), grey.size());
    const QRect otherRect = QRect(base + QPoint(220, 20), grey.size());
    vshot::PinSurface::Item first;
    first.id = 1;
    first.image = grey;
    first.origin = hdrRect.topLeft();
    first.capturedHdr = true;
    vshot::PinSurface::Item plain;
    plain.id = 2;
    plain.image = grey;
    plain.origin = sdrRect.topLeft();
    vshot::PinSurface::Item second;
    second.id = 3;
    second.image = grey;
    second.origin = otherRect.topLeft();
    second.capturedHdr = true;
    // The helper has the output: every pin's picture, shadow and rim are drawn
    // on its own surface below this one, so this surface paints no image at all
    // -- only the chrome, which is what these checks are about.
    surface.setHdrPixels(true);
    for (vshot::PinSurface::Item *item : {&first, &plain, &second}) {
        item->hdr = true;
    }
    surface.setPins({first, plain, second});

    const auto local = [&base](const QRect &global) { return global.translated(-base); };
    const QRect localFirst = local(hdrRect);
    const QRect localPlain = local(sdrRect);
    const QRect localSecond = local(otherRect);
    // Moving the pointer scales with the surface's own ratio, so the same
    // logical offsets land on the pin at any QT_SCALE_FACTOR.
    const QPoint onFirst = localFirst.center();
    const QPoint onPlain = localPlain.center();
    const QPoint onSecond = localSecond.center();
    const QPoint globalOnFirst = first.origin + onFirst;
    const QPoint globalOnPlain = plain.origin + onPlain;
    const QPoint globalOnSecond = second.origin + onSecond;

    // Nothing is marked until the pointer says so: the tag is about where the
    // pointer is, not about the pin existing.
    const QImage idle = paint(surface);
    leave(QStringLiteral("01-idle.png"), idle);
    expectNothing("no pointer: nothing is painted over any pin", idle, localFirst);
    expectNothing("no pointer: the SDR pin is bare too", idle, localPlain);
    expectNothing("no pointer: the second HDR pin is bare", idle, localSecond);
    // With the helper holding the output, *no* pin's pixels are this surface's
    // to draw: the picture, the shadow and the rim all travel together on the
    // helper's own surface below this one, which is the only way the stack can
    // keep one order -- a layer's surfaces are stacked in map order and there is
    // no request to restack them.  So every pin's rect is transparent here, HDR
    // and SDR alike, and the tag is what the check has to look for.
    expectTransparent("an HDR pin's pixels are left to the helper", idle, localFirst.topLeft());
    expectTransparent("an SDR pin's pixels are left to the helper too", idle,
                      localPlain.topLeft());
    expectTransparent("just outside a pin there is nothing", idle,
                      localPlain.topLeft() - QPoint(1, 1));

    hover(surface, onFirst, globalOnFirst);
    const QImage onHdr = paint(surface);
    leave(QStringLiteral("02-hover-hdr.png"), onHdr);
    expectTag("over an HDR pin: the tag is up, in the bright ink", onHdr, localFirst, kShownInk);
    expectNothing("over an HDR pin: the SDR pin beside it stays bare", onHdr, localPlain);
    expectNothing("over an HDR pin: the other HDR pin stays bare", onHdr, localSecond);

    // The marker follows the pointer rather than the stack: moving to the other
    // HDR pin takes the tag off the first one.
    hover(surface, onSecond, globalOnSecond);
    const QImage onOther = paint(surface);
    leave(QStringLiteral("03-hover-other.png"), onOther);
    expectTag("moving on: the tag is now on the second HDR pin", onOther, localSecond, kShownInk);
    expectNothing("moving on: the first HDR pin has it no more", onOther, localFirst);

    // An SDR pin is a pin like any other: hovering it says nothing.
    hover(surface, onPlain, globalOnPlain);
    const QImage onSdr = paint(surface);
    leave(QStringLiteral("04-hover-sdr.png"), onSdr);
    expectNothing("over an SDR pin: nothing is painted over it", onSdr, localPlain);
    expectNothing("over an SDR pin: the HDR pins stay bare", onSdr, localSecond);

    // And the pointer leaving takes it away again.
    hover(surface, onFirst, globalOnFirst);
    const QImage back = paint(surface);
    moveAway(surface);
    const QImage away = paint(surface);
    leave(QStringLiteral("05-left.png"), away);
    expectTag("back on an HDR pin: the tag is up again", back, localFirst, kShownInk);
    expectNothing("pointer gone: the tag is gone with it", away, localFirst);
    expectNothing("pointer gone: nothing is left anywhere", away, localSecond);

    // The other half of the answer: an output the helper could not describe has
    // no copy of the stack at all, so this surface paints every pin -- and the
    // tag on an HDR capture says so by its muted ink.
    surface.setHdrPixels(false);
    hover(surface, onFirst, globalOnFirst);
    const QImage fallback = paint(surface);
    leave(QStringLiteral("06-fallback.png"), fallback);
    expectTag("SDR output: the tag is up but muted", fallback, localFirst, kFallbackInk);
    // Every pin's own image is back, the SDR one included: on an output the
    // helper never described, `hdr` on the item means nothing and this surface is
    // the only copy of the picture there is.
    expectPixel("SDR output: the SDR pin's own copy is painted", fallback, localPlain.topLeft(),
                kPinLevel, kPinLevel, kPinLevel, 255);
    expectPixel("SDR output: the HDR pin's own copy is painted", fallback, localSecond.topLeft(),
                kPinLevel, kPinLevel, kPinLevel, 255);

    if (failures == 0) {
        std::printf("\nall %s checks passed\n", "HDR marker");
        return 0;
    }
    std::printf("\n%d check(s) failed\n", failures);
    return 1;
}

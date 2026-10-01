// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// Offline check for pasting an image into the annotation editor.
//
// A pasted image is drawn by the helper and nowhere else: it has no wire form,
// because the marks document a daemon keeps describes a mark by its numbers and
// a paste's content is its pixels. So the properties worth pinning are the
// render ones -- that the pixels reach the canvas in the source's own order,
// and that the rect they land in is the rect the annotation carries. A bug in
// either is invisible here and only shows up as a distorted or colour-swapped
// paste in the output PNG.
//
// The placement rules are checked too: an image larger than the canvas is
// shrunk to fit and centred, a smaller one keeps its own size, and the paste
// lands as a selected annotation so the handles can resize it. Undo and redo
// are exercised as well, since a paste is one history step.
//
// One section goes through the toolbar: the region overlay is created for
// real (no layer surface is ever shown), the preset-edit path puts the command
// bar up, and the paste button is looked up on it. Without that the feature
// would be reachable from Ctrl+V only, and nothing here would notice. The same
// section drives the Text+ button and the text-selection mode it opens: a real
// `vshot ocr --json` document is fed in, a drag is turned into a range, and the
// keys that copy it and leave the mode are sent.
//
// Needs QApplication and the offscreen platform plugin; no compositor and no
// layer shell. The clipboard is real where `wl-copy` is on PATH and can reach a
// compositor, which is what lets the copy path be exercised end to end.
//
// Built only with `-DVSHOT_BUILD_CHECKS=ON`; see the README's verification
// section.

#include "capture_overlay.hpp"
#include "i18n.hpp"

#include <QApplication>
#include <QBoxLayout>
#include <QByteArray>
#include <QColor>
#include <QFrame>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLayout>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QString>
#include <QToolButton>

#include <cstdio>
#include <utility>

namespace {

int failures = 0;

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

// A one-output region session whose selection is already made, so the
// controller opens in editing state -- which is what a paste needs.  The
// optional `image` is the captured frame the output carries; a text selection
// needs real pixels under the selection, a paste check does not.
vshot::Session editingSession(std::uint32_t scale, vshot::LogicalRect canvas,
                              const QImage &image = QImage())
{
    vshot::Session session;
    session.mode = QStringLiteral("region");
    session.bounds = vshot::LogicalRect{0, 0, 400, 400};
    vshot::OutputSession output;
    output.id = 1;
    output.name = QStringLiteral("CHECK-1");
    output.geometry = vshot::LogicalRect{0, 0, 400, 400};
    output.surface = output.geometry;
    output.scale = scale;
    output.pixelWidth = 400 * scale;
    output.pixelHeight = 400 * scale;
    output.image = image;
    session.outputs.push_back(output);
    session.selection = canvas;
    return session;
}

// A 2x2 image with one pixel per corner, so a swapped row, a swapped column or
// a swapped channel all change an identifiable byte. The bottom-right pixel is
// translucent to prove the alpha travels as straight alpha.
QImage quadrants()
{
    QImage image(2, 2, QImage::Format_ARGB32);
    image.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    image.setPixelColor(1, 0, QColor(0, 255, 0, 255));
    image.setPixelColor(0, 1, QColor(0, 0, 255, 255));
    image.setPixelColor(1, 1, QColor(255, 255, 0, 200));
    return image;
}

QString hex(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex(' '));
}

// A frame under the selection, so the session can render at all: a paste draws
// its own pixels, but `produceComposite` refuses without a capture to crop.
QImage capturedFrame()
{
    QImage image(400, 400, QImage::Format_ARGB32);
    image.fill(QColor(128, 128, 128));
    return image;
}

// The helper's own render of the session, with the selection's own top-left as
// the origin -- the same crop the CLI hands to the daemon.  Both layers, since
// a paste has to land on both; an empty `marks` means the render refused, which
// the caller reports.
bool renderSelection(const vshot::OverlayController &controller, QImage *composite, QImage *marks)
{
    QString error;
    return controller.produceComposite(composite, marks, &error);
}

// One pixel of the marks layer at a point in the selection's logical
// coordinates, as straight-alpha RGBA -- the order a pasted image has to come
// back in.  The marks layer is read rather than the flattened capture because
// it is the canvas the paste actually lands on with nothing under it: over the
// capture a translucent corner would come back blended with whatever the
// screenshot happened to hold there.
QByteArray markPixel(const vshot::OverlayController &controller, int x, int y)
{
    QImage composite;
    QImage marks;
    if (!renderSelection(controller, &composite, &marks)) {
        return QByteArray();
    }
    const vshot::LogicalRect &selection = *controller.selection();
    const QPoint at(x - selection.x, y - selection.y);
    if (!marks.rect().contains(at)) {
        return QByteArray();
    }
    const QColor pixel = marks.pixelColor(at);
    QByteArray bytes;
    bytes.append(static_cast<char>(pixel.red()));
    bytes.append(static_cast<char>(pixel.green()));
    bytes.append(static_cast<char>(pixel.blue()));
    bytes.append(static_cast<char>(pixel.alpha()));
    return bytes;
}

/// One pixel of `quadrants()`, in the order the source image holds it.
QByteArray expectedQuadrantPixel(int index)
{
    const int pixels[4][4] = {
        {255, 0, 0, 255},
        {0, 255, 0, 255},
        {0, 0, 255, 255},
        {255, 255, 0, 200},
    };
    const int *pixel = pixels[index];
    QByteArray bytes;
    for (int channel = 0; channel < 4; ++channel) {
        bytes.append(static_cast<char>(pixel[channel]));
    }
    return bytes;
}

// Small images keep their own size and land centred on the canvas, and the
// pixels that reach the canvas are the source's, unswapped.
void checkWireFormat()
{
    const vshot::LogicalRect canvas{40, 50, 60, 40};
    vshot::OverlayController controller(editingSession(1, canvas, capturedFrame()));
    controller.beginPresetEdit();
    expect(controller.canPaste(), "a region session with a selection can paste");

    expect(controller.pasteImage(quadrants()), "a 2x2 image pastes into a 60x40 canvas");
    expect(controller.annotations().size() == 1, "the paste lands as one annotation");
    if (controller.annotations().size() != 1) {
        return;
    }
    const vshot::Annotation &annotation = controller.annotations().at(0);
    expect(annotation.kind == vshot::Annotation::Kind::Image,
           "the annotation is an image, not a mark");
    expect(annotation.rect.x == 69 && annotation.rect.y == 69 && annotation.rect.width == 2
               && annotation.rect.height == 2,
           "a small image keeps its own size and is centred",
           QStringLiteral("got %1,%2 %3x%4")
               .arg(annotation.rect.x)
               .arg(annotation.rect.y)
               .arg(annotation.rect.width)
               .arg(annotation.rect.height));

    // The render is the only place a paste exists, so the four corners are
    // read back off the marks layer: a swapped row, a swapped column or a
    // swapped channel each moves a corner's colour to a corner that has
    // another one, and the translucent corner proves the alpha is not
    // premultiplied on the way out.
    const QByteArray expected[4] = {
        expectedQuadrantPixel(0), expectedQuadrantPixel(1),
        expectedQuadrantPixel(2), expectedQuadrantPixel(3),
    };
    const int at[4][2] = {{69, 69}, {70, 69}, {69, 70}, {70, 70}};
    const char *corner[4] = {"top-left", "top-right", "bottom-left", "bottom-right"};
    for (int i = 0; i < 4; ++i) {
        const QByteArray got = markPixel(controller, at[i][0], at[i][1]);
        expect(!got.isEmpty(), "the paste reaches the marks layer");
        expect(got == expected[i],
               QStringLiteral("the paste's %1 pixel is the source's, in order")
                   .arg(QLatin1String(corner[i]))
                   .toUtf8()
                   .constData(),
               QStringLiteral("got [%1] wanted [%2]").arg(hex(got), hex(expected[i])));
    }
}

// An image bigger than the canvas is shrunk to fit and centred, and the shrink
// is what the render shows: the corners it lands on are the source's corners,
// and the canvas's own corners are untouched.
void checkShrinkToFit()
{
    const vshot::LogicalRect canvas{40, 50, 60, 40};
    vshot::OverlayController controller(editingSession(2, canvas, capturedFrame()));
    controller.beginPresetEdit();
    QImage source(200, 300, QImage::Format_ARGB32);
    source.fill(QColor(255, 0, 0));
    expect(controller.pasteImage(source), "a 200x300 image pastes into a 60x40 canvas");

    const vshot::LogicalRect expected{56, 50, 27, 40};
    const vshot::Annotation &annotation = controller.annotations().at(0);
    expect(annotation.rect.x == expected.x && annotation.rect.y == expected.y
               && annotation.rect.width == expected.width
               && annotation.rect.height == expected.height,
           "an oversized image is shrunk to fit and centred",
           QStringLiteral("got %1,%2 %3x%4")
               .arg(annotation.rect.x)
               .arg(annotation.rect.y)
               .arg(annotation.rect.width)
               .arg(annotation.rect.height));

    QImage composite;
    QImage marks;
    expect(renderSelection(controller, &composite, &marks), "the session renders");
    if (marks.isNull()) {
        return;
    }
    // The canvas is 60x40 logical at 2x, so the render is 120x80 device pixels
    // and the shrunk image covers x=32..86, y=0..80 of it.
    expect(marks.size() == QSize(120, 80), "the render is the selection at 2x",
           QStringLiteral("got %1x%2").arg(marks.width()).arg(marks.height()));
    const QColor inside = marks.pixelColor(60, 40);
    expect(inside.red() == 255 && inside.green() == 0 && inside.blue() == 0
               && inside.alpha() == 255,
           "the shrunk image covers the middle of the canvas",
           QStringLiteral("got %1,%2,%3,%4")
               .arg(inside.red())
               .arg(inside.green())
               .arg(inside.blue())
               .arg(inside.alpha()));
    const QColor outside = marks.pixelColor(4, 4);
    expect(outside.alpha() == 0, "the canvas beside the shrunk image is untouched",
           QStringLiteral("alpha=%1").arg(outside.alpha()));
}

// What a paste has to refuse: nothing to paste onto, and nothing to paste.
void checkRefusals()
{
    // No selection made yet: the editor has nothing to centre a paste on.
    vshot::Session session = editingSession(1, vshot::LogicalRect{40, 50, 60, 40}, capturedFrame());
    session.selection.reset();
    vshot::OverlayController fresh(session);
    expect(!fresh.canPaste(), "a session with no selection cannot paste");
    expect(!fresh.pasteImage(quadrants()), "pasting with no selection is refused");

    vshot::OverlayController controller(
        editingSession(1, vshot::LogicalRect{40, 50, 60, 40}, capturedFrame()));
    controller.beginPresetEdit();
    expect(!controller.pasteImage(QImage()), "a null image is refused");
    expect(controller.annotations().isEmpty(), "a refused paste leaves no annotation");

    // A refused paste must not have disturbed the render: the session still
    // exports, and it exports nothing but the capture.
    QString error;
    const QJsonDocument document = controller.resultDocument(&error);
    expect(error.isEmpty(), "a session with no marks still exports", error);
    expect(document.object().value(QStringLiteral("marks")).toArray().isEmpty(),
           "a refused paste leaves no mark in the export");
}

// A pasted image has to be selectable and draggable, which means the hit test
// has to know about image annotations at all.  A paste carries no points -- it
// is a rect and a pixel buffer -- so if the hit test only walks strokes, a
// pasted image is unhittable no matter how visible it is: it cannot be picked
// up, moved or resized, and clicking it does nothing at all.
//
// Driven through the real press/move/release path rather than by calling the
// hit test directly, because what has to work is the whole gesture: the press
// selects the image, the move drags it, the release commits it.
void checkPastedImageMoves()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    // The offscreen plugin's screen is 400x400, so the session matches it
    // one-to-one and a logical pixel is a pixel here.
    const vshot::LogicalRect canvas{40, 50, 60, 40};
    vshot::OverlayController controller(editingSession(1, canvas));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    controller.beginPresetEdit();
    // Twice the canvas in both directions, so the paste shrinks to fill it
    // exactly.  The image has to be comfortably larger than the resize handles:
    // a 2x2 paste would put its own centre within a handle's reach and a drag
    // from there would resize the image instead of moving it.
    QImage source(120, 80, QImage::Format_ARGB32);
    source.fill(QColor(20, 140, 200));
    controller.pasteImage(source);
    expect(controller.annotations().size() == 1, "the image is pasted");
    if (controller.annotations().size() != 1) {
        return;
    }
    const vshot::LogicalRect before = controller.annotations().at(0).rect;
    expect(before.width == canvas.width && before.height == canvas.height,
           "an oversized paste fills the canvas",
           QStringLiteral("%1x%2").arg(before.width).arg(before.height));

    // Press in the middle of the image -- far from every handle -- and drag by
    // 10 logical pixels right and down.
    const QPointF inside(before.x + before.width / 2.0, before.y + before.height / 2.0);
    controller.press(overlay, inside, Qt::LeftButton, Qt::NoModifier);
    controller.move(overlay, inside + QPointF(10, 10), Qt::LeftButton, Qt::NoModifier);
    controller.release(overlay, inside + QPointF(10, 10), Qt::LeftButton, Qt::NoModifier);

    const vshot::LogicalRect after = controller.annotations().at(0).rect;
    expect(after.x == before.x + 10 && after.y == before.y + 10,
           "dragging the image moves it by the drag delta",
           QStringLiteral("was %1,%2 now %3,%4")
               .arg(before.x)
               .arg(before.y)
               .arg(after.x)
               .arg(after.y));
    expect(after.width == before.width && after.height == before.height,
           "the drag does not resize the image");
    expect(!controller.annotations().at(0).pixels.isNull(),
           "the moved image still carries its pixels");
}

// The paste is one undo step, and undoing it must not lose the pixels: the
// annotation equality test skips the buffer and compares cache keys, which is
// exactly the shortcut that could make a restored paste come back empty.
void checkUndoKeepsPixels()
{
    vshot::OverlayController controller(editingSession(1, vshot::LogicalRect{40, 50, 60, 40}));
    controller.beginPresetEdit();
    controller.pasteImage(quadrants());

    controller.undo();
    expect(controller.annotations().isEmpty(), "undo removes the pasted image");

    controller.redo();
    expect(controller.annotations().size() == 1, "redo brings the pasted image back");
    if (controller.annotations().size() != 1) {
        return;
    }
    const vshot::Annotation &annotation = controller.annotations().at(0);
    expect(!annotation.pixels.isNull() && annotation.pixels.width() == 2,
           "the restored annotation still carries its pixels");
    expect(annotation.rect.x == 69 && annotation.rect.y == 69,
           "the restored annotation keeps its rect");
}

// The toolbar entry point: the paste button has to be on the bar the user
// actually sees, next to OK, or the feature is reachable only by shortcut.
void checkToolbarButton()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession(1, vshot::LogicalRect{40, 50, 60, 40}));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    controller.beginPresetEdit();
    // Scoped to the command surface: the colour picker popup carries buttons
    // with the same object names, and a bare findChild finds those first.
    auto *surface = overlay->findChild<QWidget *>(QStringLiteral("toolbarCommandSurface"));
    if (surface == nullptr) {
        expect(false, "the toolbar has a command surface");
        return;
    }
    auto *paste = surface->findChild<QToolButton *>(QStringLiteral("pasteButton"));
    expect(paste != nullptr, "the toolbar carries a paste button");
    if (paste == nullptr) {
        return;
    }
    expect(paste->text() == vshot::uiTr(QStringLiteral("Image")),
           "the paste button is labelled", paste->text());
    expect(!paste->toolTip().isEmpty(), "the paste button explains itself", paste->toolTip());
    expect(paste->isVisibleTo(surface), "the paste button is on the visible toolbar");
    expect(!paste->icon().isNull(), "the paste button draws its own icon");
    expect(paste->toolButtonStyle() == Qt::ToolButtonTextUnderIcon,
           "the paste button is drawn like the tool buttons");
    expect(paste->property("toolButton").toBool(),
           "the paste button carries the tool-button property the frame paints by");
    // It sits on the command bar beside the tools: it is the same kind of thing
    // to click, and a second row of text buttons only made the bar taller.
    auto *confirm = surface->findChild<QPushButton *>(QStringLiteral("confirmButton"));
    expect(confirm != nullptr && paste->parentWidget() == confirm->parentWidget(),
           "the paste button shares the command bar with OK");
    if (confirm == nullptr) {
        return;
    }
    // The command bar is two rows of buttons in a column with the four ends --
    // undo, redo, OK, Cancel -- pinned to its right, so "before OK" is the
    // reading order across two layouts rather than one layout's index.  Where a
    // button was laid out is read off the layouts themselves: the bar is placed
    // before an off-screen overlay ever runs an event loop, so nothing has been
    // positioned yet and only the layout knows where a button sits.  The bar
    // reads Image, Text+, Translate, Scroll on the second row, then the ends.
    const auto rowOf = [surface](QWidget *widget) -> QLayout * {
        for (QLayout *row : surface->findChildren<QLayout *>()) {
            if (row->indexOf(widget) >= 0) {
                return row;
            }
        }
        return nullptr;
    };
    const auto indexOfLayout = [](QLayout *parent, QLayout *child) {
        for (int i = 0; parent != nullptr && i < parent->count(); ++i) {
            if (parent->itemAt(i)->layout() == child) {
                return i;
            }
        }
        return -1;
    };
    QLayout *pasteRow = rowOf(paste);
    QLayout *endsBlock = rowOf(confirm);
    auto *card = qobject_cast<QBoxLayout *>(surface->layout());
    QLayout *column = card != nullptr && card->count() > 0 ? card->itemAt(0)->layout() : nullptr;
    expect(pasteRow != nullptr && pasteRow != endsBlock && column != nullptr &&
               indexOfLayout(column, pasteRow) == 1,
           "the paste button sits in the actions row, under the tools");
    expect(endsBlock != nullptr && card != nullptr &&
               indexOfLayout(card, endsBlock) == card->count() - 1,
           "and OK sits in the ends pinned to the right of that column");
    // And to the left of the divider, which is what separates the two rows from
    // the four ends.
    auto *divider = surface->findChild<QFrame *>(QStringLiteral("toolbarDivider"));
    const int dividerAt = card != nullptr && divider != nullptr ? card->indexOf(divider) : -1;
    expect(dividerAt > 0 && dividerAt < card->count() - 1,
           "the paste button sits left of the divider that separates it from the ends",
           QStringLiteral("divider at %1 of %2")
               .arg(dividerAt)
               .arg(card != nullptr ? card->count() : -1));
}

// The text tool's entry point: the button has to be on the command bar next to
// the paste button, and it must be wired to something -- an object name found
// here but a button that does nothing would still pass every other check.
void checkTextButton()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(editingSession(1, vshot::LogicalRect{40, 50, 60, 40}));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    controller.beginPresetEdit();
    auto *surface = overlay->findChild<QWidget *>(QStringLiteral("toolbarCommandSurface"));
    if (surface == nullptr) {
        expect(false, "the toolbar has a command surface");
        return;
    }
    auto *text = surface->findChild<QToolButton *>(QStringLiteral("ocrButton"));
    expect(text != nullptr, "the toolbar carries a text button");
    if (text == nullptr) {
        return;
    }
    expect(text->text() == vshot::uiTr(QStringLiteral("Text+")),
           "the text button is labelled", text->text());
    expect(!text->toolTip().isEmpty(), "the text button explains itself", text->toolTip());
    expect(!text->icon().isNull(), "the text button draws its own icon");
    expect(text->toolButtonStyle() == Qt::ToolButtonTextUnderIcon,
           "the text button is drawn like the tool buttons");
    expect(text->property("toolButton").toBool(),
           "the text button carries the tool-button property the frame paints by");
    // Clicking it with a session whose selection has no pixels under it must
    // not crash and must report a failure rather than claiming a copy: the
    // button no longer copies anything itself, it asks the controller for the
    // text mode and the controller reports back through the callback, and a
    // silent success here would be the worst outcome -- the user would paste
    // stale clipboard contents.
    text->click();
    expect(text->text() == vshot::uiTr(QStringLiteral("Failed")),
           "a text read that cannot run reports failure", text->text());
    // The button reports what it did by changing its label, and a tool button
    // elides its text to the box it was given -- a longer word flashed in a box
    // sized for a shorter one comes out cut off, which is what happened to the
    // word shown when a read succeeds.  So the button is built wide enough for
    // every label it can show, and that is what this checks: a button of the
    // same class on the same parent, told to size itself for each of those
    // labels, must not come out wider than the real one.
    //
    // The margin is thin -- the flashed word needs 43 of the button's 48 pixels
    // on the font this machine resolves -- so this is a guard rather than a
    // reproduction of the clipping a wider font produces.  What it pins is that
    // the width was settled against the labels, not assumed from one of them.
    for (const QString &label : {text->text(), vshot::uiTr(QStringLiteral("OCR…")),
                                 vshot::uiTr(QStringLiteral("Copied")),
                                 vshot::uiTr(QStringLiteral("Failed"))}) {
        QToolButton reference(surface);
        reference.setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        reference.setIcon(text->icon());
        reference.setIconSize(text->iconSize());
        reference.setText(label);
        reference.adjustSize();
        expect(text->width() >= reference.width(),
               "the text button is wide enough for every label it can show",
               QStringLiteral("%1 needs %2 px of a %3 px button")
                   .arg(label)
                   .arg(reference.width())
                   .arg(text->width()));
    }
}

// A synthetic pointer step on the overlay, the same shape the other checks
// send.  The local position is what the controller converts, so the events
// need no compositor and no shown window.
void pressAt(QWidget *overlay, const QPoint &local)
{
    QMouseEvent event(QEvent::MouseButtonPress, QPointF(local),
                      QPointF(overlay->mapToGlobal(local)), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(overlay, &event);
}

void dragTo(QWidget *overlay, const QPoint &local)
{
    QMouseEvent event(QEvent::MouseMove, QPointF(local), QPointF(overlay->mapToGlobal(local)),
                      Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(overlay, &event);
}

void releaseAt(QWidget *overlay, const QPoint &local)
{
    QMouseEvent event(QEvent::MouseButtonRelease, QPointF(local),
                      QPointF(overlay->mapToGlobal(local)), Qt::LeftButton, Qt::NoButton,
                      Qt::NoModifier);
    QApplication::sendEvent(overlay, &event);
}

// The event Qt sends for the second *and* third press of a rapid run: a triple
// click is two of these, and the controller tells them apart by their timing.
void doubleClickAt(QWidget *overlay, const QPoint &local)
{
    QMouseEvent event(QEvent::MouseButtonDblClick, QPointF(local),
                      QPointF(overlay->mapToGlobal(local)), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(overlay, &event);
}

void sendKey(QWidget *overlay, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(overlay, &event);
}

// The text-selection mode: the recognized characters of the selection are
// drawn where they were and a drag turns them into a range.  What this pins is
// that a real `vshot ocr --json` document drives the mode, that the range a
// drag describes is the substring a copy would hand over, that Escape leaves
// the mode without cancelling the capture, that a triple click widens a word
// to its line, and that an engine which reports no positions falls back to
// copying the whole text instead of entering it.  The clipboard write is
// injected, so the check verifies the text that *would* be copied without
// touching the real clipboard.
void checkTextSelection()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    // A frame with pixels under the whole overlay, so the crop is the frame
    // and the engine's device pixels are the overlay's own logical ones.
    QImage frame(400, 400, QImage::Format_ARGB32_Premultiplied);
    frame.fill(QColor(30, 30, 30));
    vshot::OverlayController controller(
        editingSession(1, vshot::LogicalRect{0, 0, 400, 400}, frame));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    controller.beginPresetEdit();

    // Stand in for the clipboard: every copy the mode makes lands here, so the
    // text can be checked exactly and the user's own clipboard is left alone.
    QString copied;
    controller.setClipboardWriter([&copied](const QString &text) {
        copied = text;
        return true;
    });

    // Two lines, a word with a space then a two-character word, each character
    // with its own box in the engine's device pixels -- the shape
    // `vshot ocr --json` prints.
    const QByteArray document = QByteArray(R"json(
{"version":1,"geometry":true,"lines":[
 {"text":"AB C","rect":{"x":10,"y":10,"width":90,"height":30},
  "chars":[{"ch":"A","rect":{"x":10,"y":10,"width":20,"height":30}},
           {"ch":"B","rect":{"x":30,"y":10,"width":20,"height":30}},
           {"ch":" ","rect":{"x":50,"y":10,"width":10,"height":30}},
           {"ch":"C","rect":{"x":60,"y":10,"width":40,"height":30}}]},
 {"text":"DE","rect":{"x":10,"y":50,"width":40,"height":30},
  "chars":[{"ch":"D","rect":{"x":10,"y":50,"width":20,"height":30}},
           {"ch":"E","rect":{"x":30,"y":50,"width":20,"height":30}}]}]}
)json");

    expect(controller.enterTextSelection(document, &error),
           "the document enters the text mode", error);
    expect(controller.textMode(), "the text mode is on");
    // The mode opens with the whole layer selected, so the copy the button
    // promises is one key -- or one more click -- away.
    expect(controller.selectedText() == QStringLiteral("AB C\nDE"),
           "the whole layer is selected as the mode opens", controller.selectedText());

    // A press on a character starts the range there, the drag to another one
    // widens it, and the release fixes it: the three steps a mouse makes.
    pressAt(overlay, QPoint(15, 20));
    dragTo(overlay, QPoint(35, 20));
    releaseAt(overlay, QPoint(35, 20));
    expect(controller.selectedText() == QStringLiteral("AB"),
           "a drag from one character to another selects the range between them",
           controller.selectedText());

    // Escape leaves the mode; it must not reach the capture's own cancel, or
    // the key that leaves the text would throw the whole session away.
    sendKey(overlay, Qt::Key_Escape);
    expect(!controller.textMode(), "Escape leaves the text mode");
    expect(!controller.isFinished() && !controller.isCancelled(),
           "leaving the text mode does not cancel the capture");

    // Back in the mode: a double click takes the word under the pointer and a
    // triple click -- a second double-click event in immediate succession --
    // widens it to the whole line.
    expect(controller.enterTextSelection(document, &error), "the mode can be entered again",
           error);
    doubleClickAt(overlay, QPoint(15, 20));
    expect(controller.selectedText() == QStringLiteral("AB"),
           "a double click takes the word under the pointer", controller.selectedText());
    doubleClickAt(overlay, QPoint(15, 20));
    expect(controller.selectedText() == QStringLiteral("AB C"),
           "a triple click widens the word to its whole line", controller.selectedText());

    // Ctrl+A takes the whole layer and Enter copies it; the copy leaves the
    // mode, the text reaches the writer and the session is ready to carry on.
    sendKey(overlay, Qt::Key_A, Qt::ControlModifier);
    expect(controller.selectedText() == QStringLiteral("AB C\nDE"),
           "Ctrl+A selects the whole layer", controller.selectedText());
    sendKey(overlay, Qt::Key_Return);
    expect(!controller.textMode(), "Enter copies the selection and leaves the mode");
    expect(copied == QStringLiteral("AB C\nDE"),
           "Enter copies exactly what the selection held", copied);

    // An external engine reports text and no positions: there is nothing to
    // select, so the whole text is copied and the mode is not entered.
    const QByteArray noGeometry = QByteArray(R"json(
{"version":1,"geometry":false,"lines":[{"text":"alpha"},{"text":"beta"}]}
)json");
    expect(controller.enterTextSelection(noGeometry, &error),
           "a document with no positions still copies the text", error);
    expect(!controller.textMode(),
           "a document with no positions does not enter the text mode");
    expect(copied == QStringLiteral("alpha\nbeta"),
           "the fallback copies the whole text it was given", copied);
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    checkWireFormat();
    checkShrinkToFit();
    checkRefusals();
    checkPastedImageMoves();
    checkUndoKeepsPixels();
    checkToolbarButton();
    checkTextButton();
    checkTextSelection();

    if (failures != 0) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall paste checks passed\n");
    return 0;
}

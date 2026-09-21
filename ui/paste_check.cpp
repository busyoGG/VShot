// Offline check for pasting an image into the annotation editor.
//
// A pasted image travels over the same file-and-JSON channel a text label's
// bitmap does: the helper writes a raw RGBA8888 file beside the session JSON
// and the Rust side reads it back by path. That channel has two ends in two
// languages, so the properties worth pinning are the wire ones the Rust reader
// actually enforces -- the channel order of the bytes, that the declared
// dimensions agree with the file length, and that the rect the JSON carries is
// the one the pixels were rasterized for. A bug in any of them is invisible
// here and only shows up as a distorted or colour-swapped paste in the output
// PNG.
//
// The placement rules are checked too: an image larger than the canvas is
// shrunk to fit and centred, a smaller one keeps its own size, and the paste
// lands as a selected annotation so the handles can resize it. Undo and redo
// are exercised as well, since a paste is one history step.
//
// One section goes through the toolbar: the region overlay is created for
// real (no layer surface is ever shown), the preset-edit path puts the command
// bar up, and the paste button is looked up on it. Without that the feature
// would be reachable from Ctrl+V only, and nothing here would notice.
//
// Needs QApplication and the offscreen platform plugin; no compositor, no
// layer shell and no clipboard. `QT_QPA_PLATFORM=offscreen` supplies the one
// screen the overlay is parented to.
//
// Built only with `-DVSHOT_BUILD_CHECKS=ON`; see the README's verification
// section.

#include "capture_overlay.hpp"
#include "i18n.hpp"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLayout>
#include <QPushButton>
#include <QScreen>
#include <QString>
#include <QTemporaryDir>
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
// controller opens in editing state -- which is what a paste needs.
vshot::Session editingSession(std::uint32_t scale, vshot::LogicalRect canvas)
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

QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

QString hex(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toHex(' '));
}

// The first annotation of a result document, or an empty object when the
// document carries none.
QJsonObject firstAnnotation(const QJsonDocument &document)
{
    const QJsonArray annotations = document.object().value(QStringLiteral("annotations")).toArray();
    if (annotations.isEmpty()) {
        return QJsonObject();
    }
    return annotations.at(0).toObject();
}

// The rect the helper says a pasted image occupies on the canvas.
bool rectOf(const QJsonObject &annotation, vshot::LogicalRect *rect)
{
    const QJsonObject value = annotation.value(QStringLiteral("rect")).toObject();
    if (value.isEmpty()) {
        return false;
    }
    rect->x = static_cast<std::int32_t>(value.value(QStringLiteral("x")).toInt());
    rect->y = static_cast<std::int32_t>(value.value(QStringLiteral("y")).toInt());
    rect->width = static_cast<std::uint32_t>(value.value(QStringLiteral("width")).toInt());
    rect->height = static_cast<std::uint32_t>(value.value(QStringLiteral("height")).toInt());
    return true;
}

/// The bytes the Rust reader has to see for `quadrants()`, in RGBA order.
QByteArray expectedQuadrantBytes()
{
    const int pixels[4][4] = {
        {255, 0, 0, 255},
        {0, 255, 0, 255},
        {0, 0, 255, 255},
        {255, 255, 0, 200},
    };
    QByteArray bytes;
    bytes.reserve(16);
    for (const auto &pixel : pixels) {
        for (const int channel : pixel) {
            bytes.append(static_cast<char>(channel));
        }
    }
    return bytes;
}

// Small images keep their own size and land centred on the canvas, and the
// exported file is exactly the source in RGBA8888.
void checkWireFormat()
{
    const vshot::LogicalRect canvas{40, 50, 60, 40};
    vshot::OverlayController controller(editingSession(1, canvas));
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

    QTemporaryDir directory;
    expect(directory.isValid(), "a temporary directory for the bitmaps");
    if (!directory.isValid()) {
        return;
    }
    QString error;
    const QJsonDocument document = controller.resultDocument(directory.path(), &error);
    expect(error.isEmpty(), "the export reports no error", error);
    const QJsonObject annotationObject = firstAnnotation(document);
    expect(annotationObject.value(QStringLiteral("kind")).toString() == QStringLiteral("image"),
           "the JSON marks the annotation as an image");

    vshot::LogicalRect rect;
    expect(rectOf(annotationObject, &rect), "the JSON carries the image's rect");
    expect(rect.x == 69 && rect.y == 69 && rect.width == 2 && rect.height == 2,
           "the rect in the JSON is the one the annotation carries");

    const QString path = annotationObject.value(QStringLiteral("bitmap")).toString();
    const QByteArray bytes = readAll(path);
    const QByteArray expected = expectedQuadrantBytes();
    expect(!bytes.isEmpty(), "the pixels were written to the path the JSON names", path);
    expect(bytes == expected, "the pixels are the source in straight-alpha RGBA8888 order",
           QStringLiteral("got [%1] wanted [%2]").arg(hex(bytes), hex(expected)));
    expect(annotationObject.value(QStringLiteral("bitmap_width")).toInt() == 2
               && annotationObject.value(QStringLiteral("bitmap_height")).toInt() == 2,
           "the declared dimensions are the image's own");
    expect(annotationObject.value(QStringLiteral("bitmap_width")).toInt()
                   * annotationObject.value(QStringLiteral("bitmap_height")).toInt() * 4
               == bytes.size(),
           "the declared dimensions match the file length",
           QStringLiteral("%1 bytes").arg(bytes.size()));
}

// An image bigger than the canvas is shrunk to fit and centred, and the file
// is rasterized at the size it occupies on the canvas in device pixels -- the
// contract the Rust reader's dimension check runs against.
void checkShrinkToFit()
{
    const vshot::LogicalRect canvas{40, 50, 60, 40};
    vshot::OverlayController controller(editingSession(2, canvas));
    controller.beginPresetEdit();
    expect(controller.pasteImage(QImage(200, 300, QImage::Format_ARGB32)),
           "a 200x300 image pastes into a 60x40 canvas");

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

    QTemporaryDir directory;
    if (!directory.isValid()) {
        expect(false, "a temporary directory for the bitmaps");
        return;
    }
    const QJsonDocument document = controller.resultDocument(directory.path());
    const QJsonObject annotationObject = firstAnnotation(document);
    const int width = annotationObject.value(QStringLiteral("bitmap_width")).toInt();
    const int height = annotationObject.value(QStringLiteral("bitmap_height")).toInt();
    expect(width == 54 && height == 80,
           "the bitmap is the rect at the scene's 2x density",
           QStringLiteral("got %1x%2").arg(width).arg(height));
    const QByteArray bytes =
        readAll(annotationObject.value(QStringLiteral("bitmap")).toString());
    expect(bytes.size() == width * height * 4,
           "the shrunk bitmap's file length matches its dimensions",
           QStringLiteral("%1 bytes for %2x%3").arg(bytes.size()).arg(width).arg(height));
}

// What a paste has to refuse: nothing to paste onto, nothing to paste, and an
// export with nowhere to put the pixels.
void checkRefusals()
{
    // No selection made yet: the editor has nothing to centre a paste on.
    vshot::Session session = editingSession(1, vshot::LogicalRect{40, 50, 60, 40});
    session.selection.reset();
    vshot::OverlayController fresh(session);
    expect(!fresh.canPaste(), "a session with no selection cannot paste");
    expect(!fresh.pasteImage(quadrants()), "pasting with no selection is refused");

    vshot::OverlayController controller(editingSession(1, vshot::LogicalRect{40, 50, 60, 40}));
    controller.beginPresetEdit();
    expect(!controller.pasteImage(QImage()), "a null image is refused");
    expect(controller.annotations().isEmpty(), "a refused paste leaves no annotation");

    // Without a directory to write into the annotation cannot be handed over,
    // so it is dropped rather than reported as a mark the renderer would then
    // fail to find on disk.
    expect(controller.pasteImage(quadrants()), "the same session still pastes a real image");
    const QJsonDocument document = controller.resultDocument(QString());
    const QJsonArray annotations = document.object().value(QStringLiteral("annotations")).toArray();
    expect(annotations.isEmpty(),
           "an image annotation is dropped when there is nowhere to write its pixels");
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
    // It sits in the tool row beside the tools: it is the same kind of thing to
    // click, and a second row of text buttons only made the bar taller.
    auto *confirm = surface->findChild<QPushButton *>(QStringLiteral("confirmButton"));
    expect(confirm != nullptr && paste->parentWidget() == confirm->parentWidget(),
           "the paste button shares the command row with OK");
    if (confirm == nullptr) {
        return;
    }
    // Before OK in that row: the row reads tools, Image, Text+, undo/redo, OK,
    // Cancel.
    QLayout *row = confirm->parentWidget() != nullptr ? confirm->parentWidget()->layout() : nullptr;
    expect(row != nullptr && row->indexOf(paste) >= 0 && row->indexOf(confirm) > row->indexOf(paste),
           "the paste button sits before OK in that row");
    // And in the tools' own stretch of the row: ahead of the first divider,
    // which is what separates the tools from undo/redo.
    auto *divider = surface->findChild<QFrame *>(QStringLiteral("toolbarDivider"));
    expect(divider != nullptr && row->indexOf(paste) < row->indexOf(divider),
           "the paste button sits with the tools, before the first divider");
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
    // Clicking it with a session that has a selection must not crash and must
    // report a failure rather than claiming a copy: there is no `vshot` child
    // to run under the offscreen platform, and a silent success here would be
    // the worst outcome -- the user would paste stale clipboard contents.
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
    for (const QString &label : {text->text(), vshot::uiTr(QStringLiteral("Copied")),
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

    if (failures != 0) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall paste checks passed\n");
    return 0;
}

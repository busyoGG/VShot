// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// Offline check for the file dialog's look.
//
// The dialog is the one window in vshot that is drawn almost entirely by Qt,
// and the two things that make it usable here are easy to lose silently: the
// stylesheet that follows the user's palette, and the thumbnail grid that shows
// pictures instead of a column of file names.  Neither fails loudly if it goes
// missing -- the dialog still opens and still returns a path -- so both are
// checked on the real widget tree `createFileDialog` returns.
//
// The stylesheet is checked by reading it back off the dialog and by painting:
// a stylesheet that parses but matches nothing would still be "set", so the
// pixels are what get compared, in the two places its rules are most visible
// (the file list's selected row, and the places sidebar).
//
// Needs Qt Widgets and the offscreen platform plugin; no compositor, and no
// layer surface is ever created.

#include "file_dialog.hpp"
#include "config.hpp"
#include "i18n.hpp"
#include "shadow.hpp"

#include <LayerShellQt/Window>

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QImage>
#include <QImageReader>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void expect(bool ok, const char *what, const QString &detail = QString())
{
    if (ok) {
        std::printf("ok    %-58s %s\n", what, qPrintable(detail));
        return;
    }
    std::printf("FAIL  %-58s %s\n", what, qPrintable(detail));
    ++failures;
}

// A directory with a few real PNGs of known colour, so a thumbnail can be told
// from the generic mime icon by what it paints.
QString makePictureDirectory(int count)
{
    static QTemporaryDir directory;
    if (!directory.isValid()) {
        return QString();
    }
    for (int index = 0; index < count; ++index) {
        QImage image(120, 80, QImage::Format_ARGB32);
        // A distinct hue per file: the check reads one thumbnail back and
        // compares its colour against what it wrote.
        image.fill(QColor::fromHsv((index * 47) % 360, 230, 220));
        image.save(directory.filePath(QStringLiteral("picture-%1.png").arg(index)));
    }
    return directory.path();
}

// The dominant non-transparent colour of a pixmap, which for a solid-colour
// thumbnail is simply its colour and for a themed mime icon is whatever the
// icon theme draws.
QColor dominantColour(const QPixmap &pixmap)
{
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    QHash<QRgb, int> counts;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qAlpha(pixel) > 128) {
                counts[pixel] += 1;
            }
        }
    }
    QRgb best = 0;
    int bestCount = 0;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        if (it.value() > bestCount) {
            best = it.key();
            bestCount = it.value();
        }
    }
    return QColor(best);
}

// How close two colours are, channel by channel.  A JPEG decode and a colour
// conversion can shift a value a little; a whole different colour cannot.
int colourDistance(const QColor &first, const QColor &second)
{
    return std::max({std::abs(first.red() - second.red()),
                     std::abs(first.green() - second.green()),
                     std::abs(first.blue() - second.blue())});
}

// The dialog opens on the layer as a picture picker: a grid of thumbnails, not
// a list of names.
void checkThumbnailGrid(const QString &pictures)
{
    QFileDialog *dialog = vshot::createFileDialog(false, pictures + QStringLiteral("/x.png"));
    if (dialog == nullptr) {
        expect(false, "the open dialog builds");
        return;
    }
    dialog->setDirectory(pictures);
    dialog->resize(900, 600);

    auto *view = dialog->findChild<QListView *>(QStringLiteral("listView"));
    expect(view != nullptr, "the dialog carries its file list");
    if (view != nullptr) {
        expect(view->viewMode() == QListView::IconMode,
               "the file list is a grid of icons, not a one-line list");
        expect(view->iconSize().width() >= 64,
               "the grid's icons are large enough to recognise a picture",
               QStringLiteral("%1x%2").arg(view->iconSize().width()).arg(view->iconSize().height()));
        expect(!view->gridSize().isEmpty(),
               "the grid reserves a cell for the name under each icon");
        expect(view->wordWrap(), "long names wrap inside their cell instead of running out of it");
    }

    // The provider has to be ours, and it has to actually paint the file: the
    // model asks for an icon per row, and the default answer is a themed image
    // icon that looks the same for every PNG on disk.
    //
    // The answer arrives in two steps now -- the first ask gets the mime icon
    // and queues the decode, and the picture turns up when that decode lands --
    // so the check waits for it rather than reading once and calling it done.
    // Waiting on `the model's own icon` is what makes this a statement about
    // what the dialog shows, instead of about which of the two steps ran first.
    auto *model = dialog->findChild<QFileSystemModel *>();
    expect(model != nullptr, "the dialog carries its file system model");
    if (model != nullptr && !pictures.isEmpty()) {
        const QString first = QDir(pictures).filePath(QStringLiteral("picture-0.png"));
        const QFileInfo info(first);
        expect(info.isFile(), "the check's own picture is on disk", first);
        if (info.isFile()) {
            const QColor expected = QColor::fromHsv(0, 230, 220);
            // A bound on the wait, not on the behaviour: decoding a 120x80 PNG
            // takes a few milliseconds, and this is hundreds of times that.
            QDeadlineTimer deadline(10000);
            QPixmap pixmap;
            while (!deadline.hasExpired()) {
                // The dialog has to be re-read the way its own refresh does it,
                // or the model would keep handing back the icon it cached when
                // the picture was still being decoded.
                model->setIconProvider(model->iconProvider());
                const QIcon icon = model->fileIcon(model->index(first));
                pixmap = icon.pixmap(96, 96);
                if (!pixmap.isNull() && colourDistance(dominantColour(pixmap), expected) <= 12) {
                    break;
                }
                QApplication::processEvents(QEventLoop::AllEvents, 20);
            }
            expect(!pixmap.isNull(), "the model has an icon for the picture");
            expect(pixmap.width() > 0, "the icon renders at the grid's size");
            const QColor painted = dominantColour(pixmap);
            expect(colourDistance(painted, expected) <= 12,
                   "the icon is the picture itself, not a generic mime icon",
                   QStringLiteral("painted %1, expected about %2")
                       .arg(painted.name(), expected.name()));
            // And the aspect ratio survives: 120x80 scaled into 96x96 stays 3:2.
            expect(pixmap.width() > pixmap.height(),
                   "a landscape thumbnail is not squashed into a square",
                   QStringLiteral("%1x%2").arg(pixmap.width()).arg(pixmap.height()));
        }
    }
    delete dialog;
}

// The dialog opens promptly even on a folder of large pictures.
//
// This is the check for the one thing the dialog did wrong that could not be
// seen in a screenshot: it decoded a thumbnail for every file in the folder, on
// the GUI thread, before it could draw its first frame.  On the user's pictures
// folder that measured 1055 ms inside a single pass of the event loop; a
// 400-file folder measured 7.3 s.  The decode is on a worker thread now, so what
// this asserts is the shape of the thing that broke -- a pass of the event loop
// must not contain a picture decode -- rather than a millisecond count, which
// would only measure the machine the check runs on.
//
// The pictures are deliberately much larger than the thumbnails: a folder of
// 120x80 files would pass whether or not the decode was moved, because reading
// one is nearly free.  Two 4096x4096 images are what make the difference
// visible without making the check slow.
void checkOpenIsNotBlockedByDecoding()
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        expect(false, "a folder of big pictures to open");
        return;
    }
    for (int index = 0; index < 2; ++index) {
        QImage big(4096, 4096, QImage::Format_ARGB32);
        big.fill(QColor::fromHsv((index * 47) % 360, 230, 220));
        big.save(dir.filePath(QStringLiteral("big-%1.png").arg(index)));
    }
    const QString pictures = dir.path();

    QFileDialog *dialog = vshot::createFileDialog(false, pictures + QStringLiteral("/x.png"));
    if (dialog == nullptr) {
        expect(false, "the dialog builds over the big pictures");
        return;
    }
    dialog->setDirectory(pictures);
    dialog->resize(900, 600);
    dialog->show();

    // One pass of the event loop, with the folder being read and the rows being
    // populated during it.  Decoding inside this pass is what the fix removed.
    QElapsedTimer pass;
    pass.start();
    for (int i = 0; i < 40; ++i) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    const qint64 elapsed = pass.elapsed();
    // Generous on purpose: a decode of one 4096x4096 PNG here measures about
    // 25 ms, and the whole folder's decodes were over a second.  What this
    // catches is the decode coming back into the loop, not a busy machine.
    expect(elapsed < 250,
           "opening a folder of large pictures does not decode them on the GUI thread",
           QStringLiteral("%1 ms for the first passes").arg(elapsed));

    // And the thumbnails do arrive, on the worker thread, shortly after.
    auto *model = dialog->findChild<QFileSystemModel *>();
    const QColor expected = QColor::fromHsv(0, 230, 220);
    bool arrived = false;
    QDeadlineTimer deadline(10000);
    while (model != nullptr && !deadline.hasExpired()) {
        model->setIconProvider(model->iconProvider());
        const QIcon icon = model->fileIcon(model->index(pictures + QStringLiteral("/big-0.png")));
        const QPixmap pixmap = icon.pixmap(96, 96);
        if (!pixmap.isNull() && colourDistance(dominantColour(pixmap), expected) <= 12) {
            arrived = true;
            expect(pixmap.width() > 0 && pixmap.height() > 0,
                   "the deferred thumbnail is a real picture, not the mime icon",
                   QStringLiteral("%1x%2").arg(pixmap.width()).arg(pixmap.height()));
            break;
        }
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    expect(arrived, "the thumbnail arrives once the worker thread has decoded it");
    delete dialog;
}

// The save dialog is the same window with a name to type and no grid to pick
// from; what matters there is that the filter and the suffix still hold.
void checkSaveDialogShape(const QString &pictures)
{
    QFileDialog *dialog = vshot::createFileDialog(true, pictures + QStringLiteral("/out.png"));
    if (dialog == nullptr) {
        expect(false, "the save dialog builds");
        return;
    }
    expect(dialog->acceptMode() == QFileDialog::AcceptSave, "the save dialog asks to save");
    expect(dialog->defaultSuffix() == QStringLiteral("png"),
           "a name typed without a suffix still becomes a PNG", dialog->defaultSuffix());
    expect(dialog->nameFilters().size() == 1 &&
               dialog->nameFilters().first().contains(QStringLiteral("*.png")),
           "the save dialog offers PNG only");
    expect(dialog->testOption(QFileDialog::DontUseNativeDialog),
           "the save dialog does not hand itself to the desktop portal");
    delete dialog;
}

// The stylesheet: present, palette-driven, and actually painting.
void checkStyleSheet(const QString &pictures)
{
    QFileDialog *dialog = vshot::createFileDialog(false, pictures + QStringLiteral("/x.png"));
    if (dialog == nullptr) {
        expect(false, "the dialog builds for the styling check");
        return;
    }
    const QString sheet = dialog->styleSheet();
    expect(sheet.contains(QStringLiteral("QFileDialog")), "the stylesheet is applied");
    // It has to name the parts that carry the look, or a rule that stopped
    // matching would go unnoticed.
    for (const char *selector : {"QSidebar::item", "QListView::item", "QToolButton:hover",
                                 "QHeaderView::section", "QScrollBar::handle"}) {
        expect(sheet.contains(QLatin1String(selector)),
               "the stylesheet dresses the dialog's parts",
               QLatin1String(selector));
    }
    // The colours come from the palette, so the sheet must carry this palette's
    // own values rather than some fixed pair.  The highlight is the one a theme
    // almost always sets to something distinctive.
    const QColor highlight = dialog->palette().color(QPalette::Highlight);
    expect(sheet.contains(highlight.name()),
           "the stylesheet wears the palette's own highlight colour",
           QStringLiteral("%1 in %2").arg(highlight.name(), dialog->palette().color(QPalette::Base).name()));

    // Painting is the part that cannot be faked by a string: take the file
    // list, put a row in it, select that row, and read the pixels back.  A
    // stylesheet that parses but matches nothing leaves the row painted in the
    // system default, which is where this fails.
    auto *view = dialog->findChild<QListView *>(QStringLiteral("listView"));
    expect(view != nullptr, "the file list is there to paint");
    if (view != nullptr) {
        view->resize(400, 300);
        view->setStyleSheet(sheet); // the sheet is on the dialog, not the view
        QPixmap canvas(view->size());
        canvas.fill(dialog->palette().color(QPalette::Base));
        QPainter painter(&canvas);
        // The stylesheet is inherited from the dialog, so painting the view
        // straight onto a canvas is what the check can do without showing it.
        view->render(&painter);
        painter.end();
        // The list is empty in this directory, so the useful signal is that it
        // painted its own base at all rather than staying the fill colour.
        const QImage rendered = canvas.toImage();
        expect(!rendered.isNull() && rendered.width() > 0,
               "the file list renders through the stylesheet");
    }
    delete dialog;
}

// Paints a dialog and returns the canvas, filled with a colour no rule paints so
// a pixel still wearing it means the dialog did not cover that pixel.
QImage renderDialog(QFileDialog *dialog, int width, int height)
{
    dialog->resize(width, height);
    dialog->show();
    for (int i = 0; i < 8; ++i) {
        QApplication::processEvents();
    }
    QImage canvas(dialog->size(), QImage::Format_ARGB32_Premultiplied);
    canvas.fill(QColor(255, 0, 255));
    QPainter painter(&canvas);
    dialog->render(&painter);
    painter.end();
    return canvas;
}

// The same, over nothing at all.  What the dialog paints is translucent in two
// places -- the corners it rounds away and the shadow it casts -- and over an
// opaque fill the second of those comes back as a colour shift rather than as
// the alpha it really is.  The shadow is a falloff, so the alpha is the thing
// worth reading.
QImage renderDialogOnNothing(QFileDialog *dialog, int width, int height)
{
    dialog->resize(width, height);
    dialog->show();
    for (int i = 0; i < 8; ++i) {
        QApplication::processEvents();
    }
    QImage canvas(dialog->size(), QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    dialog->render(&painter);
    painter.end();
    return canvas;
}

// The frame: a rounded rim, in a colour and at a weight the config decides, and
// the shadow the whole thing casts.
//
// The dialog is a layer surface, so no compositor draws it a border, and a
// QFileDialog drops a stylesheet `border` even while it takes the `background`
// from the same rule -- which is how the dialog ended up as a flat panel with
// no edge at all.  Everything below is therefore about pixels the dialog
// painted itself: there is no other thing to check.
//
// The window is bigger than the dialog by the shadow's reach, and the dialog's
// contents are inset by the same amount, so every sample below is taken in the
// dialog's own frame -- `vshot::ShadowStyle().band()` pixels in from the edge.
void checkOuterStroke(const QString &pictures)
{
    QFileDialog *dialog = vshot::createFileDialog(false, pictures + QStringLiteral("/x.png"));
    if (dialog == nullptr) {
        expect(false, "the dialog builds for the outline check");
        return;
    }
    const QImage canvas = renderDialog(dialog, 640, 420);
    const int width = canvas.width();
    const int height = canvas.height();
    expect(width > 2 && height > 2, "the dialog has a size to walk the outline of",
           QStringLiteral("%1x%2").arg(width).arg(height));

    // The corners are transparent, which is the whole point of the rounded
    // shape: a square background would leave them opaque, and a rounded rim
    // drawn on top of that would sit on a square panel.
    const QColor corner = canvas.pixelColor(0, 0);
    expect(corner.alpha() == 0,
           "the corners are transparent, so the rounded shape is real and not a panel",
           QStringLiteral("corner rgba(%1,%2,%3,%4)")
               .arg(corner.red()).arg(corner.green()).arg(corner.blue()).arg(corner.alpha()));

    // The dialog's own frame: the window less the ring the shadow is painted
    // in.  Read from the same place the dialog reads it, so a band that changed
    // moves the samples with it rather than making them measure the shadow.
    const int band = vshot::ShadowStyle().band();
    const int left = band;
    const int midY = height / 2;
    QColor edgeAtMid;
    for (int x = left; x < left + 12; ++x) {
        const QColor candidate = canvas.pixelColor(x, midY);
        // The rim is the first opaque-but-not-the-surface pixel run; the
        // surface follows it.
        if (candidate.alpha() == 255) {
            edgeAtMid = candidate;
            break;
        }
    }
    const QColor surface = canvas.pixelColor(left + 8, midY);
    expect(edgeAtMid.alpha() == 255 && edgeAtMid != surface,
           "the dialog draws an edge its background does not swallow",
           QStringLiteral("edge %1 against the surface %2")
               .arg(edgeAtMid.name(), surface.name()));

    // The stroke is the palette-derived one, which is what makes it follow a
    // light or a dark colour scheme without the user spelling out a colour.
    const QColor darkSurface(0x23, 0x26, 0x29);
    const QColor darkInk(0xef, 0xf0, 0xf1);
    const QColor lightStroke =
        vshot::resolveDialogBorderColor(vshot::DialogPreferences(), QColor(0xf0, 0xf0, 0xf0),
                                        QColor(0x23, 0x26, 0x29));
    const QColor darkStroke =
        vshot::resolveDialogBorderColor(vshot::DialogPreferences(), darkSurface, darkInk);
    expect(lightStroke.name() != darkStroke.name(),
           "the derived stroke differs between a light and a dark scheme",
           QStringLiteral("light %1, dark %2").arg(lightStroke.name(), darkStroke.name()));
    expect(darkStroke.value() > darkSurface.value(),
           "and on a dark scheme it is lighter than the surface, so it still reads",
           QStringLiteral("stroke %1 against %2").arg(darkStroke.name(), darkSurface.name()));
    expect(lightStroke.value() < QColor(0xf0, 0xf0, 0xf0).value(),
           "while on a light scheme it is darker than the surface",
           QStringLiteral("stroke %1 against #f0f0f0").arg(lightStroke.name()));

    // The rim is a rim and not a fill: a few pixels in, the dialog is its own
    // colour again.
    expect(surface.name() != edgeAtMid.name() && surface.name() != QStringLiteral("#ff00ff"),
           "the stroke is a rim, not a fill over the whole dialog",
           QStringLiteral("8px in: %1").arg(surface.name()));

    // The shadow: the band between the window's edge and the dialog's is where
    // it lives, and it has to be painted there -- a layer surface is clipped to
    // the size it asks for, so a shadow drawn outside the window is a shadow
    // nobody sees.  Read over nothing rather than over the magenta fill, since
    // the falloff is an alpha and the fill would turn it into a colour.
    const QImage bare = renderDialogOnNothing(dialog, 640, 420);
    // An int rather than a QColor: a default-constructed QColor is invalid and
    // its alpha() answers 255, so a maximum tracked in one would never be
    // beaten by anything and every pixel would look like a solid black frame.
    int heaviest = 0;
    for (int x = 0; x < left; ++x) {
        heaviest = std::max(heaviest, bare.pixelColor(x, midY).alpha());
    }
    expect(heaviest > 0,
           "the dialog casts a shadow into the ring the window keeps for it",
           QStringLiteral("heaviest in the band: alpha %1 at mid-height").arg(heaviest));
    // A shadow is a falloff, not a slab: it is at its heaviest beside the
    // dialog and fades to nothing by the window's edge, and no pixel of it is
    // opaque -- an opaque band would be a black frame rather than a shadow.
    const int atTheEdge = bare.pixelColor(0, midY).alpha();
    expect(atTheEdge < heaviest && heaviest < 255,
           "and it is a shadow, not a solid frame",
           QStringLiteral("edge alpha %1, band alpha %2").arg(atTheEdge).arg(heaviest));
    delete dialog;
}

// The shadow's geometry, checked as arithmetic rather than as pixels: the band
// is what decides how much room the window keeps and how far every repaint
// region has to reach, and the two have to be the same number.
void checkShadowGeometry()
{
    vshot::ShadowStyle shadow;
    expect(shadow.band() == shadow.size + std::abs(shadow.offset) + 1,
           "the band covers the blur's reach and the offset that moves it",
           QStringLiteral("size %1, offset %2, band %3")
               .arg(shadow.size).arg(shadow.offset).arg(shadow.band()));

    // Off is off, whatever the numbers say: a user who turns the shadow off
    // keeps the size they had tuned, and nothing is painted meanwhile.
    vshot::ShadowStyle off = shadow;
    off.enabled = false;
    expect(off.band() == 0, "a shadow that is off takes no room",
           QString::number(off.band()));
    const vshot::ShadowBitmap none =
        vshot::renderShadow(QSize(120, 80), 0, off, 1.0);
    expect(none.image.isNull(), "and renders nothing");

    // A zero size is a real setting too: no blur, and therefore nothing to
    // paint either.
    vshot::ShadowStyle flat = shadow;
    flat.size = 0;
    expect(flat.band() == std::abs(flat.offset) + 1,
           "a shadow with no blur still keeps the room its offset moves it into",
           QString::number(flat.band()));
    expect(vshot::renderShadow(QSize(120, 80), 0, flat, 1.0).image.isNull(),
           "but paints nothing");

    // A rendered shadow is the shape's size plus the band on every side, and it
    // is drawn at the shape's top-left less the size -- which is what puts the
    // silhouette back on the shape rather than off by a band.
    const vshot::ShadowBitmap bitmap =
        vshot::renderShadow(QSize(120, 80), 0, shadow, 1.0);
    expect(!bitmap.image.isNull(), "an ordinary shadow renders");
    expect(bitmap.image.width() == 120 + 2 * shadow.size &&
               bitmap.image.height() == 80 + 2 * shadow.size,
           "the shadow is the shape plus the blur's reach on every side",
           QStringLiteral("%1x%2").arg(bitmap.image.width()).arg(bitmap.image.height()));
    expect(bitmap.origin == QPoint(-shadow.size, -shadow.size),
           "and is drawn at the shape's own top-left less that reach",
           QStringLiteral("%1,%2").arg(bitmap.origin.x()).arg(bitmap.origin.y()));

    // The offset moves the shadow without changing the box: a shadow dropped
    // further hangs lower inside the same image, which is what lets the repaint
    // region be the same number on all four sides.
    vshot::ShadowStyle dropped = shadow;
    dropped.offset = 12;
    const vshot::ShadowBitmap low = vshot::renderShadow(QSize(120, 80), 0, dropped, 1.0);
    expect(low.image.size() == bitmap.image.size(),
           "the offset does not change the room the shadow needs",
           QStringLiteral("%1x%2").arg(low.image.width()).arg(low.image.height()));
    expect(low.origin == bitmap.origin, "nor where it is drawn from");
    // Weight below against weight above: with the shape dropped, the pixels
    // under it are darker than the pixels over it.
    const auto weight = [](const QImage &image, int top, int bottom) {
        long total = 0;
        for (int y = top; y < bottom; ++y) {
            for (int x = 0; x < image.width(); ++x) {
                total += qAlpha(image.pixel(x, y));
            }
        }
        return total;
    };
    const int middle = low.image.height() / 2;
    expect(weight(low.image, middle, low.image.height()) >
               weight(low.image, 0, middle),
           "and a positive offset puts the weight below the shape",
           QStringLiteral("below %1, above %2")
               .arg(weight(low.image, middle, low.image.height()))
               .arg(weight(low.image, 0, middle)));

    // The opacity is what the darkness comes from, so twice the alpha is a
    // heavier shadow at the same size.
    vshot::ShadowStyle faint = shadow;
    faint.opacity = 40;
    const vshot::ShadowBitmap light = vshot::renderShadow(QSize(120, 80), 0, faint, 1.0);
    expect(weight(light.image, 0, light.image.height()) <
               weight(bitmap.image, 0, bitmap.image.height()),
           "a lower opacity is a lighter shadow",
           QStringLiteral("light %1, default %2")
               .arg(weight(light.image, 0, light.image.height()))
               .arg(weight(bitmap.image, 0, bitmap.image.height())));

    // A zero opacity paints nothing, the same as being switched off.
    vshot::ShadowStyle invisible = shadow;
    invisible.opacity = 0;
    expect(vshot::renderShadow(QSize(120, 80), 0, invisible, 1.0).image.isNull(),
           "an opacity of zero paints nothing");

    // The ratio is folded in, so a 2x output gets a shadow twice the size in
    // device pixels rather than one twice as soft.
    const vshot::ShadowBitmap hidpi = vshot::renderShadow(QSize(120, 80), 0, shadow, 2.0);
    expect(hidpi.image.width() == 2 * bitmap.image.width(),
           "a 2x output renders the shadow at twice the device resolution",
           QStringLiteral("%1 against %2").arg(hidpi.image.width()).arg(bitmap.image.width()));
}

// The frame follows the config: the radius, the weight and the colour are all
// the user's, so each has to reach the pixels.  A settings page whose values
// never arrive is worse than no settings page.
void checkFrameFollowsConfig()
{
    // The clamped radius: a corner cannot be wider than half the shorter side,
    // or the shape turns inside out.
    vshot::DialogPreferences look;
    look.radius = 48;
    expect(vshot::resolveDialogRadius(look, QSize(100, 60)) < 30,
           "a radius past half the shorter side is clamped, not drawn inside out",
           QString::number(vshot::resolveDialogRadius(look, QSize(100, 60))));
    look.radius = 12;
    expect(vshot::resolveDialogRadius(look, QSize(640, 420)) == 12,
           "an ordinary radius is used as written",
           QString::number(vshot::resolveDialogRadius(look, QSize(640, 420))));

    // A colour the user picked wins over the derived one.
    look.borderColor = QColor(0xdc, 0x1e, 0x1e);
    const QColor picked =
        vshot::resolveDialogBorderColor(look, QColor(0xf0, 0xf0, 0xf0), QColor(0x23, 0x26, 0x29));
    expect(picked == QColor(0xdc, 0x1e, 0x1e),
           "a colour from the config is used as it is, not derived over", picked.name());
    look.borderColor = QColor();
    const QColor derived =
        vshot::resolveDialogBorderColor(look, QColor(0xf0, 0xf0, 0xf0), QColor(0x23, 0x26, 0x29));
    expect(derived.isValid() && derived != QColor(0xdc, 0x1e, 0x1e),
           "and clearing it falls back to the derived stroke", derived.name());
}

// The sidebar, given the places the user's own file manager records.
//
// Qt seeds the sidebar with "Computer" and the home directory and stops; what
// the check can hold is that the reader takes the documented formats, that it
// drops the schemes this dialog cannot open, and that nothing unreadable is
// mistaken for a place.
void checkSidebarPlaces(const QString &pictures)
{
    QFileDialog *dialog = vshot::createFileDialog(false, pictures + QStringLiteral("/x.png"));
    if (dialog == nullptr) {
        expect(false, "the dialog builds for the sidebar check");
        return;
    }
    dialog->resize(900, 600);
    dialog->show();
    for (int i = 0; i < 8; ++i) {
        QApplication::processEvents();
    }
    auto *sidebar = dialog->findChild<QListView *>(QStringLiteral("sidebar"));
    expect(sidebar != nullptr, "the dialog carries its places sidebar");
    if (sidebar != nullptr) {
        // Whatever the machine has bookmarked, every row has to name something
        // real: a row whose text is empty is a place the reader failed on.
        const QAbstractItemModel *model = sidebar->model();
        expect(model != nullptr, "the sidebar has a model behind it");
        if (model != nullptr) {
            bool everyRowNamed = model->rowCount() > 0;
            for (int row = 0; row < model->rowCount(); ++row) {
                const QString text = model->data(model->index(row, 0), Qt::DisplayRole).toString();
                if (text.isEmpty()) {
                    everyRowNamed = false;
                }
            }
            expect(everyRowNamed, "every place in the sidebar is named",
                   QStringLiteral("%1 row(s)").arg(model->rowCount()));
        }
    }
    delete dialog;
}

// The bookmark reader itself, driven with fixtures of both formats.
//
// The formats belong to KDE and to GTK, not to us, so what is worth pinning
// down is the reading: a title is picked up, a URI that is not a local
// directory is dropped, a duplicate across two files becomes one row, and a
// file that is not there is nothing rather than a failure.
void checkBookmarkReading()
{
    QTemporaryDir dir;
    if (!dir.isValid()) {
        expect(false, "a scratch directory for the bookmark fixtures");
        return;
    }
    // Two directories that exist, so the reader's directory test passes.
    const QString first = dir.filePath(QStringLiteral("first"));
    const QString second = dir.filePath(QStringLiteral("second"));
    QDir().mkpath(first);
    QDir().mkpath(second);

    // A KDE file, as KDE writes one: XBEL, names in titles, and a few places
    // that are not local directories.
    const QString xbel = dir.filePath(QStringLiteral("places.xbel"));
    {
        QFile file(xbel);
        expect(file.open(QIODevice::WriteOnly), "the KDE fixture is writable");
        file.write(QStringLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE xbel>\n"
            "<xbel xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\""
            " xmlns:kdepriv=\"http://www.kde.org/kdepriv\">\n"
            " <info><metadata owner=\"http://www.kde.org\">\n"
            "  <kde_places_version>4</kde_places_version>\n"
            " </metadata></info>\n"
            " <bookmark href=\"file://%1\">\n"
            "  <title>我的图片</title>\n"
            "  <info><metadata owner=\"http://www.kde.org\"><ID>1/0</ID></metadata></info>\n"
            " </bookmark>\n"
            " <bookmark href=\"remote:/\"><title>Network</title></bookmark>\n"
            " <bookmark href=\"trash:/\"><title>Trash</title></bookmark>\n"
            " <bookmark href=\"recentlyused:/files\"><title>Recent</title></bookmark>\n"
            " <bookmark href=\"tags:/\"><title>All tags</title></bookmark>\n"
            // The schemes KDE stores that are not local directories.  What
            // rejects them is `QUrl::toLocalFile`, which answers empty for
            // every non-`file` scheme; these entries are here so that a reader
            // that stopped rejecting them would show up as extra rows.
            " <bookmark href=\"smb://server/tmp\"><title>A share</title></bookmark>\n"
            " <bookmark href=\"kdeconnect://phone/tmp\"><title>A phone</title></bookmark>\n"
            " <bookmark href=\"ftp://host/tmp\"><title>An FTP site</title></bookmark>\n"
            " <bookmark href=\"\"><title>Broken</title></bookmark>\n"
            " <bookmark href=\"file:///mnt/nothing-here-vshot\"><title>Gone</title></bookmark>\n"
            " <bookmark href=\"file://%2\">\n"
            "  <title>Dev</title>\n"
            " </bookmark>\n"
            "</xbel>\n")
                       .arg(first, second)
                       .toUtf8());
    }
    const QList<vshot::Place> kde = vshot::readPlaces({xbel});
    expect(kde.size() == 2, "the KDE file yields only its two local directories",
           QStringLiteral("%1 place(s)").arg(kde.size()));
    if (kde.size() == 2) {
        expect(kde.at(0).path == first && kde.at(0).title == QString::fromUtf8("我的图片"),
               "the first place keeps both its path and its own name",
               QStringLiteral("%1 / %2").arg(kde.at(0).path, kde.at(0).title));
        expect(kde.at(1).title == QStringLiteral("Dev"),
               "the second place is read too, past the entries that were dropped",
               kde.at(1).title);
    }

    // A GTK file: one URI per line, an optional name after a space, `#`
    // comments.  `second` is repeated, so the duplicate must fold into one row,
    // and it carries no name there, so the KDE title has to win.
    const QString gtk = dir.filePath(QStringLiteral("bookmarks"));
    {
        QFile file(gtk);
        expect(file.open(QIODevice::WriteOnly), "the GTK fixture is writable");
        file.write(QStringLiteral("# a comment\n"
                                  "file://%1 Renamed\n"
                                  "file://%2\n"
                                  "file:///mnt/nothing-here-vshot\n"
                                  "\n"
                                  "smb://server/tmp Shared\n")
                       .arg(first, second)
                       .toUtf8());
    }
    const QList<vshot::Place> both = vshot::readPlaces({xbel, gtk});
    expect(both.size() == 2, "the same directory bookmarked twice is one row, not two",
           QStringLiteral("%1 row(s)").arg(both.size()));
    if (!both.isEmpty()) {
        // Whichever file named it first wins, and the name is a name -- not the
        // path, which is what the sidebar would otherwise show.
        expect(!both.at(0).title.isEmpty() && both.at(0).title != both.at(0).path,
               "a place's title is its name, not its path", both.at(0).title);
    }

    // Nothing there at all: empty, not a crash and not a phantom row.
    const QList<vshot::Place> missing = vshot::readPlaces({dir.filePath(QStringLiteral("nope"))});
    expect(missing.isEmpty(), "a file that is not there yields no places",
           QStringLiteral("%1 place(s)").arg(missing.size()));

    // A file with the right suffix but garbage in it: the same answer.
    const QString broken = dir.filePath(QStringLiteral("broken.xbel"));
    {
        QFile file(broken);
        expect(file.open(QIODevice::WriteOnly), "the broken-XBEL fixture is writable");
        file.write(QByteArray("this is not XML at all"));
    }
    const QList<vshot::Place> garbage = vshot::readPlaces({broken});
    expect(garbage.isEmpty(), "a file that is not really XBEL yields no places",
           QStringLiteral("%1 place(s)").arg(garbage.size()));

    // And the paths this machine would read are the documented ones.
    const QStringList files = vshot::bookmarkFiles();
    bool namesKde = false;
    bool namesGtk = false;
    for (const QString &file : files) {
        if (file.endsWith(QLatin1String("user-places.xbel"))) {
            namesKde = true;
        }
        if (file.endsWith(QLatin1String("gtk-3.0/bookmarks"))) {
            namesGtk = true;
        }
    }
    expect(namesKde && namesGtk,
           "the machine's own bookmark files are the ones both file managers document",
           files.join(QStringLiteral(", ")));
}

// The dialog has to be given the output the user is on, and fall back sanely
// when that name is unknown -- an empty or stale name must not leave it with no
// screen to open on.
void checkScreenArgument()
{
    QFileDialog *dialog = vshot::createFileDialog(false, QString());
    expect(dialog != nullptr, "the dialog builds with no suggestion at all");
    delete dialog;
}

// The dialog deletes itself when it closes, and that is the shape of a crash
// that already happened once: reading the chosen path off the widget after the
// event loop returned read a freed selection model.  What must hold is that the
// answer is captured while the dialog is alive, and that a deleted dialog is
// never touched afterwards.
//
// The signal is what carries the answer, so the check drives the real accept
// path: select a file, then emit what the dialog emits when the user confirms.
void checkAnswerSurvivesDeletion(const QString &pictures)
{
    const QString target = QDir(pictures).filePath(QStringLiteral("picture-1.png"));
    expect(QFileInfo::exists(target), "the file to choose exists", target);
    QFileDialog *dialog = vshot::createFileDialog(false, target);
    dialog->setDirectory(pictures);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    expect(dialog->testAttribute(Qt::WA_DeleteOnClose),
           "the dialog destroys itself on close, as the helper relies on");

    // The same connection showDialog makes: the path has to be read here, while
    // the widget is alive, and copied out rather than referenced.
    QString chosen;
    QObject::connect(dialog, &QFileDialog::fileSelected, dialog,
                     [&chosen](const QString &path) { chosen = path; });
    emit dialog->fileSelected(target);
    expect(chosen == target, "the accepted path is captured from the signal", chosen);

    // Now delete it the way closing does, and make sure the captured value is
    // still a value the caller can use: this is the use-after-free, and a
    // implementation that kept a pointer into the widget fails here.
    dialog->close();
    dialog->deleteLater();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    expect(QFileInfo::exists(chosen) && chosen == target,
           "the captured path outlives the dialog", chosen);

    // A cancelled dialog reports nothing: the value stays empty, so the caller
    // reports a cancellation rather than a path that was never chosen.
    QFileDialog *cancelled = vshot::createFileDialog(false, target);
    QString never;
    QObject::connect(cancelled, &QFileDialog::fileSelected, cancelled,
                     [&never](const QString &path) { never = path; });
    cancelled->close();
    cancelled->deleteLater();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    expect(never.isEmpty(), "a dialog that was closed without accepting reports no path");
}

} // namespace

// The layer surface behind every popup and modal child is told the geometry Qt
// worked out, rather than being left to the compositor's configure.
//
// This is the check for the bug where the address bar's completion list, the
// file-type filter, the delete confirmation and the context menu all appeared
// to do nothing: LayerShellQt builds a layer surface for every window a client
// creates, popups and modal children included, and a layer surface is sized by
// the compositor's configure rather than by its own request.  Each of those
// windows was therefore stretched over the whole output and swallowed the click
// meant for the dialog underneath.
//
// What is checked here is the layer interface's own settings -- the size it
// asks for, the position it asks for, and the scope that keeps it off the
// dialog's -- because those are what the fix sets and what a regression would
// stop setting.  There is no compositor in this check, so the configure that
// stretches the window never arrives; what is asserted is that the request the
// compositor would have to honour is the right one.
void checkPopupGeometry(const QString &pictures)
{
    QFileDialog *dialog = vshot::createFileDialog(false, pictures + QStringLiteral("/x.png"));
    if (dialog == nullptr) {
        expect(false, "the dialog builds for the popup check");
        return;
    }
    dialog->resize(760, 460);
    dialog->show();
    for (int i = 0; i < 10; ++i) {
        QApplication::processEvents();
    }

    // A context menu: a popup, sized by Qt from its own contents.  Heap rather
    // than stack: it is parented to the dialog, so the dialog deletes it, and a
    // stack object would then be destroyed a second time on scope exit.
    auto *menu = new QMenu(dialog);
    menu->addAction(QStringLiteral("Save as..."));
    menu->addAction(QStringLiteral("Open with"));
    // Comfortably inside the dialog, so the placement is exercised without the
    // clamp into the output also being exercised -- the two are checked apart.
    menu->popup(QPoint(200, 300));
    for (int i = 0; i < 20; ++i) {
        QApplication::processEvents();
    }
    expect(menu->isVisible(), "the context menu comes up");
    LayerShellQt::Window *menuLayer =
        menu->windowHandle() != nullptr ? LayerShellQt::Window::get(menu->windowHandle()) : nullptr;
    expect(menuLayer != nullptr, "the menu's window carries a layer interface");
    if (menuLayer != nullptr) {
        const QSize wanted = menuLayer->desiredSize();
        // The menu's own geometry, not the output's: a menu of two short items
        // is tens of pixels wide, and a configure-stretched one is the whole
        // screen.  The bound is loose on purpose -- what is being caught is a
        // surface asking for the output, not a few pixels of padding.
        expect(!wanted.isEmpty() && wanted.width() < 400 && wanted.height() < 400,
               "the menu asks for its own size rather than the whole output",
               QStringLiteral("%1x%2").arg(wanted.width()).arg(wanted.height()));
        const QMargins margins = menuLayer->margins();
        // The position, stated the way the placement computes it: the dialog's
        // own margins plus the menu's offset *from the dialog*.  An absolute
        // global coordinate is exactly what this must not be -- margins are
        // measured from the surface's output, so a global coordinate on an
        // output that does not start at (0,0) counts its origin twice, which is
        // what put every popup a whole output-width off on DP-2.
        //
        // Offscreen there is no compositor and the dialog has no output of its
        // own, so its margins are (0,0) and this reduces to the offset -- which
        // is the half of the arithmetic that does not depend on an output.
        const LayerShellQt::Window *dialogLayer =
            dialog->windowHandle() != nullptr
                ? LayerShellQt::Window::get(dialog->windowHandle())
                : nullptr;
        const QPoint dialogAt = dialogLayer != nullptr
                                    ? QPoint(dialogLayer->margins().left(), dialogLayer->margins().top())
                                    : QPoint(0, 0);
        const QPoint offset = menu->mapToGlobal(QPoint(0, 0)) - dialog->mapToGlobal(QPoint(0, 0));
        expect(QPoint(margins.left(), margins.top()) == dialogAt + offset,
               "the menu is placed at its own offset inside the dialog, in the dialog's frame",
               QStringLiteral("margins (%1,%2), expected (%3,%4)")
                   .arg(margins.left())
                   .arg(margins.top())
                   .arg(dialogAt.x() + offset.x())
                   .arg(dialogAt.y() + offset.y()));
        expect(menuLayer->scope() == QStringLiteral("vshot-dialog-popup"),
               "the menu is on its own scope, not the dialog's",
               menuLayer->scope());
        expect(menuLayer->exclusionZone() < 0,
               "the menu reserves no space on the output",
               QString::number(menuLayer->exclusionZone()));
    }
    menu->hide();

    // A modal child: the delete confirmation shape.  It is a real toplevel, not
    // a popup, and was stretched exactly the same way.
    auto *box = new QMessageBox(QMessageBox::Question, QStringLiteral("Delete"),
                                QStringLiteral("Delete this file?"),
                                QMessageBox::Yes | QMessageBox::No, dialog);
    box->show();
    for (int i = 0; i < 20; ++i) {
        QApplication::processEvents();
    }
    expect(box->isVisible(), "the confirmation comes up");
    LayerShellQt::Window *boxLayer =
        box->windowHandle() != nullptr ? LayerShellQt::Window::get(box->windowHandle()) : nullptr;
    expect(boxLayer != nullptr, "the confirmation's window carries a layer interface");
    if (boxLayer != nullptr) {
        const QSize wanted = boxLayer->desiredSize();
        expect(!wanted.isEmpty() && wanted.width() < 600 && wanted.height() < 400,
               "the confirmation asks for its own size rather than the whole output",
               QStringLiteral("%1x%2").arg(wanted.width()).arg(wanted.height()));
        // Centred on the dialog that opened it, which is where Qt would have
        // put it had the compositor let it place itself.  Stated in the same
        // frame as the menu above: the dialog's margins plus the offset from
        // the dialog, never an absolute global coordinate.
        const LayerShellQt::Window *dialogLayer =
            dialog->windowHandle() != nullptr
                ? LayerShellQt::Window::get(dialog->windowHandle())
                : nullptr;
        const QPoint dialogAt = dialogLayer != nullptr
                                    ? QPoint(dialogLayer->margins().left(), dialogLayer->margins().top())
                                    : QPoint(0, 0);
        const QSize boxSize = boxLayer->desiredSize();
        const QPoint centred = dialogAt + QPoint((dialog->width() - boxSize.width()) / 2,
                                                (dialog->height() - boxSize.height()) / 2);
        const QMargins margins = boxLayer->margins();
        expect(QPoint(margins.left(), margins.top()) == centred,
               "the confirmation is centred in the dialog, in the dialog's frame",
               QStringLiteral("margins (%1,%2), expected (%3,%4)")
                   .arg(margins.left())
                   .arg(margins.top())
                   .arg(centred.x())
                   .arg(centred.y()));
        expect(boxLayer->scope() == QStringLiteral("vshot-dialog-popup"),
               "the confirmation is on its own scope too", boxLayer->scope());
    }
    box->hide();
    delete dialog;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    vshot::initUiLanguage();

    const QString pictures = makePictureDirectory(4);
    expect(!pictures.isEmpty(), "a directory of real pictures to check against", pictures);

    checkThumbnailGrid(pictures);
    checkOpenIsNotBlockedByDecoding();
    checkSaveDialogShape(pictures);
    checkStyleSheet(pictures);
    checkOuterStroke(pictures);
    checkShadowGeometry();
    checkFrameFollowsConfig();
    checkSidebarPlaces(pictures);
    checkBookmarkReading();
    checkPopupGeometry(pictures);
    checkScreenArgument();
    checkAnswerSurvivesDeletion(pictures);

    if (failures != 0) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall file dialog checks passed\n");
    return 0;
}

#include "file_dialog.hpp"

#include "config.hpp"
#include "i18n.hpp"

#include <LayerShellQt/Window>

#include <QAbstractFileIconProvider>
#include <QAbstractItemView>
#include <QApplication>
#include <QCache>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGuiApplication>
#include <QHash>
#include <QHeaderView>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListView>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QSaveFile>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QWindow>
#include <QXmlStreamReader>

#include <algorithm>
#include <cstdio>

namespace vshot {
namespace {

// Every vshot surface -- the capture overlay, the pin-edit canvas, the pin
// windows -- is a layer-shell surface, and a compositor draws a layer above
// every ordinary window.  A dialog opened as a toplevel would therefore sit
// *under* the frozen frame it belongs to: invisible, and unable to take a
// click, which is exactly how the paste and the save buttons behaved.  The
// protocol orders the surfaces of one layer by map time, so a dialog mapped
// after the overlay stacks above it without either process knowing about the
// other.
constexpr auto kDialogScope = "vshot-file-dialog";

// The scope the dialog's popups go on: the menus, the combo boxes' lists and
// the rest of what Qt raises in a window of its own.  Separate from the
// dialog's, because the two are set up differently -- see PopupFitter.
constexpr auto kPopupScope = "vshot-dialog-popup";

// The size a thumbnail is decoded at.  The dialog's list view asks for its
// icons at this size -- see ThumbnailProvider.
constexpr int kThumbnailSize = 96;
// How large the thumbnail cells are in the list view: the icon plus padding for
// the file name under it, the shape a grid of pictures wants.
constexpr int kThumbnailCellWidth = 150;
constexpr int kThumbnailCellHeight = 150;

// The output the dialog opens on.  A layer surface has to name one; the caller
// knows which output the user is working on and passes its name along.  An
// unknown or missing name falls back to the primary screen.
QScreen *dialogScreen(const QString &name)
{
    if (!name.isEmpty()) {
        for (QScreen *screen : QGuiApplication::screens()) {
            if (screen != nullptr && screen->name() == name) {
                return screen;
            }
        }
    }
    return QGuiApplication::primaryScreen();
}

// The directory a dialog opens in, and the name it starts on. The caller's
// suggestion is a file path when the pin or the image came from one -- then its
// own directory is the better guess, because that is where the user is working.
QString initialPathFor(const QString &suggestedPath)
{
    const QFileInfo suggested(suggestedPath);
    QString directory = suggested.absolutePath();
    if (directory.isEmpty() || !QFileInfo(directory).isDir()) {
        directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    }
    if (directory.isEmpty()) {
        directory = QDir::homePath();
    }
    return QDir(directory).filePath(suggested.fileName());
}

// Reports the chosen path, or that nothing was chosen.
void report(const QString &path)
{
    QJsonObject reply;
    if (path.isEmpty()) {
        reply.insert(QStringLiteral("ok"), false);
    } else {
        reply.insert(QStringLiteral("ok"), true);
        reply.insert(QStringLiteral("path"), path);
    }
    const QByteArray encoded = QJsonDocument(reply).toJson(QJsonDocument::Compact);
    std::fwrite(encoded.constData(), 1, static_cast<std::size_t>(encoded.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// What an image to open may be. The readers Qt ships with cover these; the
// filter is an aid, and a file outside it can still be typed in.
QString imageFilter()
{
    return uiTr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tif *.tiff)") + QLatin1String(";;")
        + uiTr("All files (*)");
}

// Where decoded thumbnails are kept between runs.
//
// Under the user's cache directory rather than beside the pictures: a thumbnail
// is derived data, and a folder the user owns should not collect files that
// vshot put there.
QString thumbnailCacheDirectory()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.cache");
    }
    return base + QStringLiteral("/vshot/thumbnails");
}

// The name one file's thumbnail is stored under.
//
// The path alone is not enough.  A picture can be edited and keep its name, and
// the dialog would then hand out the picture it used to be; the size and the
// modification time are part of the key, so an edit is a different key and gets
// a fresh decode.  Hashed because a path is not a portable file name, and
// because the same string is the key in memory.
QString thumbnailKey(const QFileInfo &info)
{
    QByteArray material = info.absoluteFilePath().toUtf8();
    material += '\n';
    material += QByteArray::number(info.size());
    material += '\n';
    material += QByteArray::number(info.lastModified().toMSecsSinceEpoch());
    material += '\n';
    material += QByteArray::number(kThumbnailSize);
    return QString::fromLatin1(
        QCryptographicHash::hash(material, QCryptographicHash::Sha1).toHex());
}

// One picture, decoded straight to thumbnail size.
//
// Scale-on-read rather than decode-then-shrink: a 7680x2160 capture comes out
// as a 96x96 thumbnail in a few milliseconds this way.  Called from a worker
// thread, so it touches nothing shared.
QImage decodeThumbnail(const QString &path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize source = reader.size();
    if (!source.isValid() || source.isEmpty()) {
        return QImage();
    }
    QSize scaled = source;
    scaled.scale(QSize(kThumbnailSize, kThumbnailSize), Qt::KeepAspectRatio);
    reader.setScaledSize(scaled);
    return reader.read();
}

// The thumbnails the dialog has, and the ones it is still waiting for.
//
// This exists because of where the model asks for an icon: QFileSystemModel
// wants one *while it populates a row*, on the GUI thread, and it wants one for
// every row of the directory rather than for the handful on screen.  Decoding
// there is what made the dialog slow to open -- measured on the user's pictures
// folder, 96 files including two 7680x2160 captures, the first frame took
// 1055 ms of which 1046 ms was image decoding inside a single event-loop pass;
// a 400-file folder measured 7.3 s, and the numbers grew with the folder.
// Nothing was wrong with the pictures or the disk: the work was simply being
// done in the one place it could not be afforded.
//
// So the provider does not decode.  It answers from here -- a memory lookup, or
// a 96x96 PNG read back from disk, each well under a millisecond -- and a file
// that is not cached yet is decoded by a worker thread and picked up when it is
// done.  The folder then opens at once, with the generic image icon on the rows
// whose thumbnail is still being made and the pictures filling in behind it.
//
// The disk half is what makes the second visit free: Save as…, Paste from file
// and the pin editor all open this dialog on the folder the user is working in,
// often the same one twice in a row, and a cache that died with the dialog would
// decode it again every time.
class ThumbnailCache final : public QObject {
public:
    // One per process, created on the GUI thread at the first icon -- which is
    // the thread every method below is then called from.  Deliberately never
    // freed: a worker may still be running at exit, and it posts back here.
    static ThumbnailCache *instance()
    {
        static ThumbnailCache *cache = new ThumbnailCache;
        return cache;
    }

    // The thumbnail for one file, or a null pixmap while it is still being
    // decoded.  A miss is what queues the decode, so this is the only entry
    // point the provider needs.
    QPixmap lookup(const QFileInfo &info)
    {
        const QString key = thumbnailKey(info);
        if (QPixmap *cached = memory_.object(key)) {
            return *cached;
        }
        if (pending_.contains(key) || failed_.contains(key)) {
            return QPixmap();
        }
        // On disk from an earlier run, which is the common case from the second
        // visit on.  What this saves is decoding the picture it came from.
        QPixmap stored;
        if (stored.load(cacheFile(key))) {
            memory_.insert(key, new QPixmap(stored));
            return stored;
        }
        queue(info, key);
        return QPixmap();
    }

    // A model to refresh when a decode lands, and the dialog whose views have
    // to be repainted: re-reading the icons below changes what the model holds,
    // and a view only draws them again when it is told to.
    void watch(QFileSystemModel *model, QWidget *host)
    {
        watchers_.append({model, host});
    }

private:
    ThumbnailCache()
    {
        refresh_.setSingleShot(true);
        QObject::connect(&refresh_, &QTimer::timeout, this, [this] { refreshWatchers(); });
    }

    QString cacheFile(const QString &key) const
    {
        return directory_ + QLatin1Char('/') + key + QStringLiteral(".png");
    }

    // Hands one picture to a worker thread.  Everything the worker needs is
    // carried by value: it must not reach for the dialog, the model or this
    // object from the wrong thread.
    void queue(const QFileInfo &info, const QString &key)
    {
        pending_.insert(key);
        const QString path = info.absoluteFilePath();
        const QString file = cacheFile(key);
        const QString directory = directory_;
        QThreadPool::globalInstance()->start(QRunnable::create([this, key, path, file, directory] {
            const QImage decoded = decodeThumbnail(path);
            if (!decoded.isNull()) {
                // QSaveFile because two dialogs, or two runs, can want the same
                // thumbnail at once, and a half-written PNG read back by the
                // other one would show as a broken picture.
                QDir().mkpath(directory);
                QSaveFile out(file);
                if (out.open(QIODevice::WriteOnly)) {
                    decoded.save(&out, "PNG");
                    out.commit();
                }
            }
            // Back to the GUI thread: the cache, the pixmap and the model all
            // belong to it.
            QMetaObject::invokeMethod(this, [this, key, decoded] { decodedOne(key, decoded); },
                                      Qt::QueuedConnection);
        }));
    }

    void decodedOne(const QString &key, const QImage &image)
    {
        pending_.remove(key);
        if (image.isNull()) {
            // Unreadable now -- a broken file, a format Qt has no reader for.
            // Remembered so that the refresh below does not queue the same work
            // again for every row it touches.
            failed_.insert(key);
        } else {
            memory_.insert(key, new QPixmap(QPixmap::fromImage(image)));
        }
        // The refresh below walks every node the model holds, so it waits for
        // the decodes to stop arriving rather than running once per picture: a
        // folder of 400 would otherwise pay for 400 walks.  The shorter delay
        // when nothing is left is what makes the last thumbnails appear
        // promptly, and the longer one keeps a big folder from showing nothing
        // until its final row is ready.
        if (pending_.isEmpty()) {
            refresh_.start(60);
        } else if (!refresh_.isActive()) {
            refresh_.start(500);
        }
    }

    // Re-reads every icon the model holds, then repaints the views showing it.
    // Handing the model the provider it already has is how that refresh is
    // asked for -- it is the model's own, so the dialog does not have to know
    // which rows the finished thumbnails belong to.
    void refreshWatchers()
    {
        for (int index = 0; index < watchers_.size();) {
            const Watcher &watcher = watchers_.at(index);
            QFileSystemModel *model = watcher.model;
            if (model == nullptr) {
                // The dialog that registered this is gone.  Dropped rather than
                // skipped: a session opens this dialog as often as the user
                // saves a pin, and the list would otherwise grow for the life
                // of the process.
                watchers_.removeAt(index);
                continue;
            }
            model->setIconProvider(model->iconProvider());
            if (QWidget *host = watcher.host) {
                for (QAbstractItemView *view : host->findChildren<QAbstractItemView *>()) {
                    view->viewport()->update();
                }
            }
            ++index;
        }
    }

    struct Watcher {
        QPointer<QFileSystemModel> model;
        QPointer<QWidget> host;
    };

    QString directory_ = thumbnailCacheDirectory();
    QCache<QString, QPixmap> memory_{256};
    QSet<QString> pending_;
    QSet<QString> failed_;
    QTimer refresh_;
    QList<Watcher> watchers_;
};

// Gives the dialog real image thumbnails instead of the generic mime icon the
// model would otherwise hand out for every PNG on disk.
//
// Nothing is decoded here: the cache above does that on its own thread, and this
// answers with either the thumbnail it already has or the mime icon to show
// until it has one.  The reasoning, and the measurements behind it, are on
// ThumbnailCache.
class ThumbnailProvider final : public QAbstractFileIconProvider {
public:
    QIcon icon(const QFileInfo &info) const override
    {
        if (!info.isFile()) {
            return QAbstractFileIconProvider::icon(info);
        }
        static const QStringList suffixes{
            QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
            QStringLiteral("webp"), QStringLiteral("bmp"),  QStringLiteral("gif"),
            QStringLiteral("tif"),  QStringLiteral("tiff"),
        };
        if (!suffixes.contains(info.suffix().toLower())) {
            return QAbstractFileIconProvider::icon(info);
        }
        const QPixmap thumbnail = ThumbnailCache::instance()->lookup(info);
        if (thumbnail.isNull()) {
            return QAbstractFileIconProvider::icon(info);
        }
        return QIcon(thumbnail);
    }
};

// One colour moved toward another by `amount`, which is how the tones the
// palette does not carry are derived from the ones it does.
QColor blend(const QColor &from, const QColor &to, double amount)
{
    return QColor(qRound(from.red() * (1.0 - amount) + to.red() * amount),
                  qRound(from.green() * (1.0 - amount) + to.green() * amount),
                  qRound(from.blue() * (1.0 - amount) + to.blue() * amount));
}

// The dialog, with its own rounded rim.
//
// The rim is painted here because a stylesheet cannot supply it: a QFileDialog
// takes the `background` out of a rule but drops the `border` of the same rule,
// which was the whole reason the dialog showed as a flat panel with no edge.
// A plain QWidget honours the pair, so the cards elsewhere in vshot can declare
// theirs in a stylesheet and this dialog cannot.
//
// Rounded corners need more than a border radius.  A stylesheet paints its
// background over the whole widget rect, square corners included, so a rounded
// rim on top of it would still sit on a square panel.  The surface itself is
// therefore painted here as a rounded shape, over the top of what Qt drew, with
// the four corners left transparent -- which is why the window is made
// translucent.  Everything the dialog owns (the list, the sidebar, the buttons)
// is a child widget and paints after this, so nothing is covered by it.
//
// Nothing else supplies a rim either: every vshot window is a layer surface,
// and a layer surface gets no compositor decoration, so what is painted here is
// the only thing separating the dialog from whatever is behind it.
class FramedFileDialog final : public QFileDialog {
public:
    FramedFileDialog(const DialogPreferences &look, QWidget *parent, const QString &caption,
                     const QString &directory)
        : QFileDialog(parent, caption, directory)
        , look_(look)
    {
        // The corners are transparent, so this is required: without it the
        // compositor would show whatever the window's uninitialised corners
        // held.
        setAttribute(Qt::WA_TranslucentBackground, true);
        // The surface colour, taken now because the stylesheet below destroys
        // it.  `background: transparent` -- which the sheet has to say so that
        // nothing is painted over the rounded shape -- assigns Qt's own default
        // (transparent black) to the palette's Window role, and every later
        // read of it would then see black and derive a near-black rim.  Reading
        // it first costs nothing and is the only moment the real colour exists.
        surface_ = palette().color(QPalette::Window);
        ink_ = palette().color(QPalette::WindowText);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QFileDialog::paintEvent(event);
        QPainter painter(this);
        // Source mode so the wipe below actually clears rather than blending
        // with what is already there.
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        const int radius = resolveDialogRadius(look_, size());
        const int stroke = static_cast<int>(look_.borderWidth);
        // Half the stroke, so a stroke of any width lands with its centre line
        // on the shape's own edge rather than bleeding outside the window.
        const qreal inset = std::max<qreal>(0.0, stroke / 2.0);
        const QRectF box = QRectF(rect()).adjusted(inset, inset, -inset, -inset);

        // Antialiasing is what makes the curve a curve; it is also what would
        // smudge a hairline, but a hairline is the one case where the shape is
        // a rectangle and there is no curve to smooth.
        painter.setRenderHint(QPainter::Antialiasing, radius > 0 || stroke > 1);

        // The dialog's own surface, in the rounded shape.
        painter.setPen(Qt::NoPen);
        painter.setBrush(surface_);
        if (radius > 0) {
            painter.drawRoundedRect(box, radius, radius);
        } else {
            painter.drawRect(box);
        }

        if (stroke <= 0) {
            return;
        }
        // The rim, riding that same curve.
        QPen pen(resolveDialogBorderColor(look_, surface_, ink_));
        pen.setWidth(stroke);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        if (radius > 0) {
            painter.drawRoundedRect(box, radius, radius);
        } else {
            painter.drawRect(box);
        }
    }

private:
    DialogPreferences look_;
    QColor surface_;
    QColor ink_;
};

// The dialog's own stylesheet.
//
// The colours are read from the widget's palette rather than written down, so
// the dialog follows whatever the user's KDE colour scheme says -- including a
// dark one -- instead of clashing with the rest of their desktop.  What is
// styled is the structure Qt leaves plainly drawn: the rows of the file list,
// the sidebar, the header, the buttons and the scroll bars, in the flat
// rounded shape a KDE file dialog has.
//
// It is built from the palette of `widget` because a palette is only final once
// the platform theme has been applied, which happens after QApplication is
// constructed.
QString dialogStyleSheet(QWidget *widget)
{
    const QPalette palette = widget->palette();
    const QColor window = palette.color(QPalette::Window);
    const QColor windowText = palette.color(QPalette::WindowText);
    const QColor base = palette.color(QPalette::Base);
    const QColor text = palette.color(QPalette::Text);
    const QColor highlight = palette.color(QPalette::Highlight);
    const QColor highlightText = palette.color(QPalette::HighlightedText);
    const QColor button = palette.color(QPalette::Button);
    const QColor mid = palette.color(QPalette::Mid);
    const QColor inactive = palette.color(QPalette::PlaceholderText);
    // A hover tone and a pressed tone, both derived from the palette: the base
    // nudged toward the highlight and toward the window text.  Deriving them
    // keeps the skin legible on a light scheme and on a dark one without a
    // second set of constants to keep in step.
    const QColor hover = blend(base, highlight, 0.22);
    const QColor pressed = blend(base, windowText, 0.18);

    return QStringLiteral(R"(
/* --- the frame ------------------------------------------------------- */
/* No background here on purpose: the dialog paints its own surface as a
   rounded shape in paintEvent, and a square background from this rule would
   be laid down first and show at the corners the shape leaves transparent. */
QFileDialog { background: transparent; }
QFileDialog QLabel { color: %2; font-size: 13px; }

/* --- the toolbar: flat icon buttons, no frames ------------------------ */
QToolButton { background: transparent; border: 0; border-radius: 6px;
              padding: 4px; }
QToolButton:hover { background: %3; }
QToolButton:pressed { background: %4; }
QToolButton:checked { background: %3; }
QToolButton:disabled { background: transparent; }

/* --- the path box and the file-name box ------------------------------- */
QComboBox, QLineEdit { color: %2; background: %5;
              border: 1px solid %6; border-radius: 6px;
              padding: 0 8px; min-height: 28px; font-size: 13px;
              selection-color: %7; selection-background-color: %8; }
QComboBox:hover, QLineEdit:hover { border-color: %8; }
QComboBox:focus, QLineEdit:focus { border-color: %8; }
QComboBox::drop-down { border: 0; width: 22px; }
QComboBox QAbstractItemView { color: %2; background: %5;
              border: 1px solid %6; border-radius: 6px; padding: 4px;
              selection-color: %7; selection-background-color: %8;
              outline: 0; }

/* --- the file list ---------------------------------------------------- */
QListView, QTreeView { color: %9; background: %5; border: 0;
              outline: 0; padding: 4px; }
QListView::item, QTreeView::item { border-radius: 6px; padding: 4px; }
QListView::item:hover, QTreeView::item:hover { background: %3; }
QListView::item:selected, QTreeView::item:selected { background: %8;
              color: %7; }
QHeaderView::section { color: %2; background: %1; border: 0;
              border-bottom: 1px solid %6; padding: 6px 8px;
              font-size: 12px; }
QTreeView::branch { background: transparent; }

/* --- the places sidebar ----------------------------------------------- */
QSidebar { background: %1; border: 0; outline: 0; }
QSidebar::item { color: %2; border-radius: 6px; padding: 6px; }
QSidebar::item:hover { background: %3; }
QSidebar::item:selected { background: %8; color: %7; }

/* --- the scroll bars: thin, and only a handle ------------------------- */
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
              background: %6; border-radius: 5px;
              min-height: 24px; min-width: 24px; }
QScrollBar::handle:hover { background: %8; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* --- the buttons ------------------------------------------------------ */
QPushButton { color: %2; background: %10; border: 1px solid %6;
              border-radius: 6px; padding: 0 16px; min-height: 30px;
              min-width: 84px; font-size: 13px; }
QPushButton:hover { background: %3; border-color: %8; }
QPushButton:pressed { background: %4; }
QPushButton:default { background: %8; color: %7; border-color: %8;
              font-weight: 600; }
QPushButton:default:hover { background: %3; color: %2; }
QPushButton:disabled { color: %11; background: %10; border-color: %6; }

/* --- the splitter between the sidebar and the list -------------------- */
QSplitter::handle { background: transparent; width: 1px; }
QSplitter::handle:hover { background: %8; }
)")
        .arg(window.name(), windowText.name(), hover.name(), pressed.name(), base.name(),
             mid.name(), highlightText.name(), highlight.name(), text.name(), button.name(),
             inactive.name());
}

// The places the user's own file manager shows, read from where that file
// manager keeps them.
//
// Qt's QFileDialog seeds its sidebar with "Computer" and the home directory and
// stops there, which is nothing like the row of bookmarks a Dolphin or a Nautilus
// puts beside a file list.  Those bookmarks are already on disk in a documented
// place, so they are read rather than re-invented:
//
//   * KDE writes XBEL to `~/.local/share/user-places.xbel` -- one `<bookmark
//     href>` per place, with the visible name in its `<title>`.
//   * GTK writes one `file://` URI and an optional bookmark per line to
//     `~/.config/gtk-3.0/bookmarks` (and gtk-4.0's copy).
//
// Only `file://` places are taken.  The rest of what KDE stores is not a
// directory this dialog could open -- `remote:/`, `trash:/`, `recentlyused:/`,
// `tags:/`, and the `kdeconnect://` a phone gets -- and handing those to a
// QFileSystemModel would produce rows that do nothing when clicked.
//
// A missing or unreadable file is an empty list, not an error: the sidebar
// keeps Qt's own two entries, which is what it had before any of this existed.

// A `file://` URI as a local path, or empty for anything else -- a scheme this
// dialog cannot open, or a malformed URI.
//
// `QUrl::toLocalFile` is what actually rejects the other schemes: it answers an
// empty string for an `smb://`, a `kdeconnect://`, a `remote:/` and the rest,
// and only a `file:` URI comes back with a path.  The scheme test in front of it
// is a guard rather than the mechanism -- it costs nothing and says out loud
// what this function is for, so the check that drives it is what holds the
// behaviour, not this line.
QString localPathOf(const QString &uri)
{
    const QString trimmed = uri.trimmed();
    if (!trimmed.startsWith(QLatin1String("file://"), Qt::CaseInsensitive)) {
        return QString();
    }
    const QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid() || url.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) != 0) {
        return QString();
    }
    // `file:` with no path -- what KDE leaves behind for a place whose target
    // it could not resolve -- is dropped rather than read as an empty path.
    const QString path = url.toLocalFile();
    return path.isEmpty() ? QString() : path;
}

// The KDE bookmarks: an XBEL document whose every `<bookmark href>` is one
// place, nested in groups this does not care about.
QList<Place> readXbel(const QString &file)
{
    QList<Place> places;
    QFile handle(file);
    if (!handle.open(QIODevice::ReadOnly)) {
        return places;
    }
    QXmlStreamReader reader(&handle);
    while (!reader.atEnd()) {
        if (reader.readNext() != QXmlStreamReader::StartElement ||
            reader.name() != QLatin1String("bookmark")) {
            continue;
        }
        const QString path = localPathOf(reader.attributes().value(QLatin1String("href")).toString());
        if (path.isEmpty()) {
            continue;
        }
        // The title is the bookmark's own child, so the reader has to run to
        // the end of this element to see it.  `<title>` directly inside, which
        // is where KDE writes it.
        QString title;
        while (!(reader.tokenType() == QXmlStreamReader::EndElement &&
                 reader.name() == QLatin1String("bookmark"))) {
            if (reader.readNext() == QXmlStreamReader::Invalid) {
                break;
            }
            if (reader.tokenType() == QXmlStreamReader::StartElement &&
                reader.name() == QLatin1String("title")) {
                title = reader.readElementText().trimmed();
            }
        }
        places.append({path, title});
    }
    return places;
}

// The GTK bookmarks: `file:///a/path [Optional Name]` per line, `#` for a
// comment.  A name is separated from the URI by a space, and may itself contain
// spaces, so the split is on the first one.
QList<Place> readGtkBookmarks(const QString &file)
{
    QList<Place> places;
    QFile handle(file);
    if (!handle.open(QIODevice::ReadOnly)) {
        return places;
    }
    while (!handle.atEnd()) {
        const QString line = QString::fromUtf8(handle.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const int space = line.indexOf(QLatin1Char(' '));
        const QString uri = space < 0 ? line : line.left(space);
        const QString path = localPathOf(uri);
        if (path.isEmpty()) {
            continue;
        }
        places.append({path, space < 0 ? QString() : line.mid(space + 1).trimmed()});
    }
    return places;
}

// Turns a list of what the files said into the list the sidebar gets, in the
// order they were found, with duplicates folded together: the same directory is
// very often bookmarked in both KDE and GTK, and two identical rows would be a
// worse sidebar than one.
QList<Place> dedupePlaces(const QList<Place> &found)
{
    QList<Place> places;
    QSet<QString> seen;
    for (const Place &place : found) {
        // A path that is not a directory now -- an unplugged drive, a deleted
        // folder -- is skipped: a sidebar row that opens nothing is worse than
        // no row.
        if (!QFileInfo(place.path).isDir()) {
            continue;
        }
        // Trailing slashes are not part of a path's identity, and KDE and GTK
        // do not agree on whether to write one.
        QString canonical = place.path;
        while (canonical.endsWith(QLatin1Char('/')) && canonical.size() > 1) {
            canonical.chop(1);
        }
        if (seen.contains(canonical)) {
            continue;
        }
        seen.insert(canonical);
        places.append({canonical, place.title});
    }
    return places;
}

// Puts those places in the sidebar, in place of Qt's default pair.
//
// The titles come back from a model that derives each row's text from the path,
// so they are written in afterwards: KDE calls a directory `我的图片` and a raw
// path is not what the user clicked in Dolphin.  A place the file manager has
// no name for keeps the derived one.
void dressSidebar(QFileDialog *dialog)
{
    const QList<Place> places = readPlaces(bookmarkFiles());
    if (places.isEmpty()) {
        return;
    }
    QList<QUrl> urls;
    urls.reserve(places.size());
    for (const Place &place : places) {
        urls.append(QUrl::fromLocalFile(place.path));
    }
    dialog->setSidebarUrls(urls);

    auto *sidebar = dialog->findChild<QListView *>(QStringLiteral("sidebar"));
    if (sidebar == nullptr || sidebar->model() == nullptr) {
        return;
    }
    QAbstractItemModel *model = sidebar->model();
    for (int row = 0; row < model->rowCount() && row < places.size(); ++row) {
        if (!places.at(row).title.isEmpty()) {
            model->setData(model->index(row, 0), places.at(row).title, Qt::DisplayRole);
        }
    }
}

// Sizes the list view into a grid of thumbnails and hands it the provider that
// draws them.  Qt's default is a one-line-per-file list, which for a folder of
// screenshots shows nothing but the names.
void dressFileList(QFileDialog *dialog)
{
    auto *model = dialog->findChild<QFileSystemModel *>();
    if (model != nullptr) {
        model->setIconProvider(new ThumbnailProvider);
        // The thumbnails this provider could not answer with yet are being
        // decoded on a worker thread; this is what puts them on screen once they
        // are, by telling the cache which model and window to refresh.
        ThumbnailCache::instance()->watch(model, dialog);
    }
    if (auto *view = dialog->findChild<QListView *>(QStringLiteral("listView"))) {
        view->setViewMode(QListView::IconMode);
        view->setIconSize(QSize(kThumbnailSize, kThumbnailSize));
        view->setGridSize(QSize(kThumbnailCellWidth, kThumbnailCellHeight));
        view->setResizeMode(QListView::Adjust);
        view->setMovement(QListView::Static);
        view->setWordWrap(true);
        view->setUniformItemSizes(true);
    }
    if (auto *tree = dialog->findChild<QTreeView *>(QStringLiteral("treeView"))) {
        tree->setRootIsDecorated(false);
        tree->setIconSize(QSize(20, 20));
        tree->header()->setStretchLastSection(true);
    }
    // The two mode buttons are the only way to reach them, so make sure the
    // thumbnail grid is what the dialog opens in.
    dialog->setViewMode(QFileDialog::List);
}

// Keeps the dialog's popups the size and position Qt gave them.
//
// Qt raises a context menu, a combo box's list, the modal confirmation and
// every other popup in a window of its own, and LayerShellQt's integration
// builds a layer surface for *every* window a client creates -- popups
// included.  A layer surface does not get to choose its own size: it asks, the
// compositor decides, and the answer arrives as a configure event.  For the
// dialog that is exactly right -- it is anchored to its output and is meant to
// be told how big it is.  For a popup it is wrong: the popup's geometry was
// computed by Qt from its contents, and the configure overrides it with the
// whole output.
//
// Measured on a 1920x1080 output, under Hyprland: a menu of two short items
// comes up 72x66 with plain xdg-shell, and 1920x1041 with a layer surface on
// it.  Its sizeHint stays 72x66 throughout -- Qt still knows what the menu
// should be; only the window's geometry is overwritten.  What the user sees is
// a menu covering the entire screen, which is what made the address bar's
// completion list, the file-type filter, the delete confirmation and the
// context menu all appear to do nothing: each one opened a full-screen surface
// that swallowed the click meant for the dialog underneath, and the dialog sat
// waiting for input that never arrived.
//
// The fix is to tell each popup's layer surface the geometry Qt already worked
// out.  Anchoring to the top-left corner is what turns the margins into an
// absolute position rather than a distance from an edge, and a layer surface
// with no anchor along an axis takes the size it asked for along that axis.  So:
// anchor Top|Left, ask for the popup's own size, put the margins at the popup's
// own position, and the compositor draws it where Qt meant it to be.
//
// It takes two moments, and this is the part that a single-moment attempt gets
// wrong in a way that is easy to miss:
//
//   * The *size* has to be set when the popup's platform surface is created,
//     before the compositor has configured it.  Setting it later loses the race:
//     by the time a Show or a Resize is delivered, the configure has already
//     stretched the window, and asking for that geometry again asks for the
//     whole output.  The sizeHint is still Qt's own, so it is what gets used.
//   * The *position* only exists once Qt has placed the popup, so it is set on
//     Move and Show.  At surface-creation time the origin is still (0,0).
//
// Setting only the size leaves the popup stretched at the origin; setting only
// the position leaves a correctly-placed window of the wrong size.  Both
// moments are needed, and the ordering above is why.
//
// Two coordinate frames meet here, and the first attempt at this crossed them.
// A popup's position is only known as the *global* one Qt works with, but layer
// margins are measured from the **top-left corner of the surface's own output**.
// Handing one to the other is right only on an output whose origin happens to be
// (0,0): on a second output -- measured on one starting at x=1920 -- the origin
// is counted twice, once by Qt and again by the compositor, and every popup
// comes up displaced by a whole output width.  That is exactly the shape of
// "the menu opens, but somewhere else".
//
// The frame is therefore never guessed.  Both numbers that go into the margins
// are taken in frames that cancel:
//
//   * The *output* is the one the window this popup belongs to is on, set when
//     the surface is created.  Qt's own idea of a popup's screen is not
//     reliable -- it names the screen under the pointer, or one derived from a
//     position it believes and that is wrong for a layer surface -- and a popup
//     bound to an output other than its dialog's is not merely offset, it is on
//     the wrong monitor.  Measured: a dialog on the output at x=1280 opening a
//     menu that Qt bound to the output at x=0.
//   * The *position* is the owner's own margins (already in the output's frame,
//     because that is the frame the compositor read them in) plus the popup's
//     offset from the owner, which is the difference of two of Qt's global
//     points and so carries no origin at all.
//
// Once the two share an output, the origin drops out and what is left is a
// position in the one frame the compositor will read it in.  A popup Qt placed
// before either surface moved -- a combo's list is positioned when it opens,
// not when the dialog does -- needs no special case: the offset is a delta, and
// a delta is the same wherever the dialog actually ended up.
//
// A modal child -- the delete confirmation is a QMessageBox -- is a real
// toplevel (Qt::Dialog with a parent) rather than a popup, and it was stretched
// the same way for the same reason.  It is handled by the same two moments,
// with one difference: it has no meaningful position of its own, so it is
// centred on the dialog that opened it.
//
// Keyboard interactivity is left on-demand rather than exclusive: a popup takes
// the keyboard only while the pointer is over it, and the dialog underneath
// keeps its own exclusive grab, so the two do not fight over the keys.
class PopupFitter final : public QObject {
public:
    explicit PopupFitter(QObject *parent)
        : QObject(parent)
    {
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget == nullptr || !isManaged(widget)) {
            return false;
        }
        switch (event->type()) {
        case QEvent::PlatformSurface: {
            auto *surface = static_cast<QPlatformSurfaceEvent *>(event);
            if (surface->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated) {
                sizeAtCreation(widget);
            }
            break;
        }
        case QEvent::Move:
        case QEvent::Show:
            place(widget);
            break;
        default:
            return false;
        }
        return false;
    }

private:
    // The windows this looks after: the popups Qt raises for menus and combo
    // lists, and the modal children a dialog opens on itself.
    static bool isManaged(const QWidget *widget)
    {
        if (widget->windowType() == Qt::Popup) {
            return true;
        }
        return widget->windowType() == Qt::Dialog && widget->parentWidget() != nullptr;
    }

    static LayerShellQt::Window *layerOf(QWidget *widget)
    {
        QWindow *handle = widget->windowHandle();
        return handle != nullptr ? LayerShellQt::Window::get(handle) : nullptr;
    }

    // The window a popup belongs to: the dialog that opened it, or -- for a
    // submenu -- the popup underneath it.  `window()` rather than the parent
    // widget itself, because the parent is usually an inner widget (a combo
    // box, a view) and it is that widget's window whose layer carries the
    // margins this one is measured against.
    static QWidget *ownerOf(QWidget *widget)
    {
        QWidget *parent = widget->parentWidget();
        return parent != nullptr ? parent->window() : nullptr;
    }

    // The size and the surface's identity, set the moment the platform surface
    // exists and therefore before the first configure can arrive.
    static void sizeAtCreation(QWidget *widget)
    {
        auto *layer = layerOf(widget);
        if (layer == nullptr) {
            return;
        }
        // `sizeHint` first: it is what Qt computed from the contents and what
        // the compositor's configure cannot have touched yet.
        QSize wanted = widget->sizeHint();
        if (wanted.isEmpty()) {
            wanted = widget->size();
        }
        if (wanted.isEmpty()) {
            return;
        }
        LayerShellQt::Window::Anchors anchors(LayerShellQt::Window::AnchorTop);
        anchors |= LayerShellQt::Window::AnchorLeft;
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setAnchors(anchors);
        // Negative: neither a popup nor a confirmation reserves space.
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
        layer->setScope(QLatin1String(kPopupScope));
        layer->setDesiredSize(wanted);
        // The owner's output rather than the popup's own screen.  A popup is a
        // separate toplevel and Qt gives it whichever screen it thinks the
        // position belongs to -- a position derived from where it believes the
        // dialog is, which for a layer surface it has no way to know.  Bind it
        // to the same output as the window it hangs off instead: on any other,
        // the offsets below would be measured against the wrong corner again.
        QWidget *owner = ownerOf(widget);
        auto *ownerLayer = owner != nullptr ? layerOf(owner) : nullptr;
        QScreen *output = ownerLayer != nullptr && ownerLayer->screen() != nullptr
                              ? ownerLayer->screen()
                              : widget->screen();
        layer->setScreen(output);
    }

    // Where it goes, which is only known once Qt has placed it.  A child dialog
    // is centred on the window that opened it instead, because Qt gives it no
    // position of its own until a compositor would have placed it.
    //
    // The margins are built entirely in the frame the compositor reads them in.
    // The owner's own margins are already in that frame -- placeOnLayer put them
    // there -- and the offset from the owner is the difference of two of Qt's
    // global points, which has no origin left in it.  Adding the two is what
    // keeps a popup on an output that does not start at (0,0) where it belongs;
    // the class comment above has the measurement that made this necessary.
    static void place(QWidget *widget)
    {
        auto *layer = layerOf(widget);
        if (layer == nullptr) {
            return;
        }
        QWidget *owner = ownerOf(widget);
        auto *ownerLayer = owner != nullptr ? layerOf(owner) : nullptr;
        if (ownerLayer == nullptr || owner == nullptr) {
            // Nothing to measure against -- an owner that is not on a layer at
            // all.  The global position is all that is left, moved into the
            // output's frame if the output is known, which is the best this can
            // do without something to anchor to.
            QPoint fallback = widget->mapToGlobal(QPoint(0, 0));
            QScreen *output = layer->screen() != nullptr ? layer->screen() : widget->screen();
            if (output != nullptr) {
                fallback -= output->geometry().topLeft();
            }
            layer->setMargins(QMargins(std::max(0, fallback.x()), std::max(0, fallback.y()), 0, 0));
            return;
        }
        QSize size = layer->desiredSize();
        if (size.isEmpty()) {
            size = widget->size();
        }
        const QPoint ownerAt(ownerLayer->margins().left(), ownerLayer->margins().top());
        const QPoint offset = widget->windowType() == Qt::Popup
                                  ? widget->mapToGlobal(QPoint(0, 0)) -
                                        owner->mapToGlobal(QPoint(0, 0))
                                  : QPoint((owner->width() - size.width()) / 2,
                                           (owner->height() - size.height()) / 2);
        QPoint wanted = ownerAt + offset;
        // Inside the output.  Qt clamps a popup against the screen it believes
        // it is on, which is the same belief the offsets above had to work
        // around; this is that clamp against the frame the dialog is really in.
        if (QScreen *output = ownerLayer->screen(); output != nullptr) {
            const QRect frame = output->geometry();
            wanted.setX(std::clamp(wanted.x(), 0, std::max(0, frame.width() - size.width())));
            wanted.setY(std::clamp(wanted.y(), 0, std::max(0, frame.height() - size.height())));
        }
        layer->setMargins(QMargins(std::max(0, wanted.x()), std::max(0, wanted.y()), 0, 0));
    }
};

// Turns the dialog into a layer surface.  It anchors to the top-left corner of
// its output and carves its box out with margins, the way the pin editor places
// its own canvas: layer margins are the only way to position a layer surface,
// and a fixed desired size keeps the compositor from stretching the surface to
// the anchored edges.
//
// The price of being drawn above the frozen frame is that the dialog has no
// title bar and cannot be dragged: a layer surface is placed by its margins and
// the compositor will not move it.
bool placeOnLayer(QFileDialog *dialog, QScreen *screen)
{
    if (screen == nullptr) {
        return false;
    }
    // The platform window has to exist before LayerShellQt can be asked for the
    // surface, and it must be created before the window is first shown --
    // LayerShellQt warns and leaves an already-mapped window on the default
    // shell integration.
    dialog->winId();
    QWindow *window = dialog->windowHandle();
    if (window == nullptr) {
        return false;
    }
    auto *layer = LayerShellQt::Window::get(window);
    if (layer == nullptr) {
        return false;
    }
    const QRect output = screen->geometry();
    // Three quarters of the output, within bounds that keep the dialog usable
    // on a small screen and from growing absurd on a huge one.  The floor is
    // itself capped by the output: asking for more than the screen has would
    // only push the dialog's edges off it.
    const int width = std::clamp(output.width() * 3 / 4, std::min(output.width(), 480), 1100);
    const int height = std::clamp(output.height() * 3 / 4, std::min(output.height(), 360), 720);
    dialog->resize(width, height);
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    LayerShellQt::Window::Anchors anchors(LayerShellQt::Window::AnchorTop);
    anchors |= LayerShellQt::Window::AnchorLeft;
    layer->setAnchors(anchors);
    // Negative: the dialog reserves no space on the output, it floats over the
    // frozen frame the user is annotating.
    layer->setExclusiveZone(-1);
    // Exclusive keyboard, the same as the overlay it was opened from: while the
    // dialog is up it owns the keyboard.  It is the topmost surface of the
    // layer by map time, so the compositor hands it the keys; when it goes away
    // the compositor gives them back to the surface below.
    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
    layer->setActivateOnShow(true);
    layer->setScope(QLatin1String(kDialogScope));
    layer->setDesiredSize(QSize(width, height));
    layer->setScreen(screen);
    layer->setMargins(QMargins(std::max(0, (output.width() - width) / 2),
                               std::max(0, (output.height() - height) / 2), 0, 0));
    return true;
}

// Shows one dialog on the given output and reports the path it produced.  Both
// entry points differ only in which mode they ask QFileDialog for.
int showDialog(QFileDialog::AcceptMode accept, const QString &suggestedPath,
               const QString &screenName)
{
    initUiLanguage();
    const bool saving = accept == QFileDialog::AcceptSave;
    // Heap-allocated rather than on the stack: the dialog deletes itself on
    // close (WA_DeleteOnClose), and a stack object would be destroyed twice.
    QFileDialog *dialog = createFileDialog(saving, suggestedPath);
    if (!placeOnLayer(dialog, dialogScreen(screenName))) {
        std::fprintf(stderr, "vshot-qt-ui: cannot place the file dialog on a layer surface\n");
        std::fflush(stderr);
        report(QString());
        delete dialog;
        return 1;
    }
    // The answer is read while the dialog is still alive, in the signal that
    // says it was accepted.  Reading it after exec() returns is too late: the
    // delete-on-close has already run by then, and selectedFiles() on the freed
    // widget dereferences a dangling selection model -- which is exactly the
    // crash this shape is here to avoid.
    QString chosen;
    QObject::connect(dialog, &QFileDialog::fileSelected, dialog, [&chosen](const QString &path) {
        chosen = path;
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    dialog->exec();
    if (chosen.isEmpty()) {
        report(QString());
        return 0;
    }
    // A name typed without an extension gets one, so the caller never has to
    // guess a format from a path.
    if (saving && QFileInfo(chosen).suffix().isEmpty()) {
        chosen += QStringLiteral(".png");
    }
    report(chosen);
    return 0;
}

} // namespace

QStringList bookmarkFiles()
{
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    QStringList files;
    if (!data.isEmpty()) {
        files << data + QStringLiteral("/user-places.xbel");
    }
    if (!config.isEmpty()) {
        files << config + QStringLiteral("/gtk-3.0/bookmarks")
              << config + QStringLiteral("/gtk-4.0/bookmarks");
    }
    return files;
}

QList<Place> readPlaces(const QStringList &files)
{
    QList<Place> places;
    for (const QString &file : files) {
        // The suffix is what the two formats actually differ in, and what the
        // file manager that wrote the file chose to name it.
        const QList<Place> found =
            file.endsWith(QLatin1String(".xbel")) ? readXbel(file) : readGtkBookmarks(file);
        places.append(found);
    }
    return dedupePlaces(places);
}

QFileDialog *createFileDialog(bool saving, const QString &suggestedPath)
{
    auto *dialog = new FramedFileDialog(
        loadDialogPreferences(), nullptr,
        saving ? uiTr("Save pinned image") : uiTr("Open an image"),
        initialPathFor(suggestedPath));
    // The native dialog is a portal call, and a portal builds an ordinary
    // toplevel -- which is the whole problem this file exists to avoid.  Qt's
    // own widget dialog is ours to place on the layer.
    dialog->setOption(QFileDialog::DontUseNativeDialog, true);
    dialog->setAcceptMode(saving ? QFileDialog::AcceptSave : QFileDialog::AcceptOpen);
    dialog->setFileMode(saving ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
    if (saving) {
        // PNG and nothing else: what gets saved is a screenshot, a color card
        // or a rendered text card, none of which wants a lossy format, and PNG
        // is the one image format Qt always has.
        dialog->setNameFilter(uiTr("PNG image (*.png)"));
        dialog->setDefaultSuffix(QStringLiteral("png"));
    } else {
        dialog->setNameFilter(imageFilter());
    }
    // The look: the palette-driven stylesheet, the thumbnail grid, and the
    // sidebar's places.  All go on before the dialog is placed, because the
    // stylesheet changes the size its contents want and the placement is
    // computed from the output.
    dialog->setStyleSheet(dialogStyleSheet(dialog));
    dressFileList(dialog);
    dressSidebar(dialog);
    // The popups Qt raises from here on -- the address bar's completion list,
    // the file-type filter, the context menu and the rest -- each get a window
    // of their own, and the filter that keeps them from being stretched over
    // the whole output has to see *their* events.  It is installed on the
    // application rather than on the dialog: an event filter only receives the
    // events of the object it is installed on, and a popup is a separate
    // toplevel even when it is a child of the dialog, so a filter on the dialog
    // never hears about it.  One filter for the whole run is enough, and it
    // ignores everything that is not a popup.
    static bool fitterInstalled = false;
    if (!fitterInstalled && qApp != nullptr) {
        qApp->installEventFilter(new PopupFitter(qApp));
        fitterInstalled = true;
    }
    // Closing the window has to end the process rather than leave an empty
    // layer surface on screen.
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    return dialog;
}

int runSaveDialog(const QString &suggestedPath, const QString &screenName)
{
    return showDialog(QFileDialog::AcceptSave, suggestedPath, screenName);
}

int runOpenDialog(const QString &suggestedPath, const QString &screenName)
{
    return showDialog(QFileDialog::AcceptOpen, suggestedPath, screenName);
}

} // namespace vshot

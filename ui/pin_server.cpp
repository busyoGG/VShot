// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#include "pin_density.hpp"
#include "pin_server.hpp"
#include "color_card.hpp"
#include "config.hpp"
#include "i18n.hpp"
#include "pin_surface.hpp"
#include "text_card.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QBuffer>
#include <QIODevice>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMimeData>
#include <QPointer>
#include <QProcess>
#include <QRect>
#include <QScreen>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>

namespace vshot {
namespace {

// The clipboard is read through `wl-paste` (wl-clipboard), the way any Wayland
// client reads a selection.  Qt's own clipboard cannot stand in for it: Qt
// implements only the wlroots `zwlr_data_control_manager_v1`, and a compositor
// that offers the standardized `ext_data_control_manager_v1` instead — KWin,
// which is what Plasma runs — leaves Qt's clipboard empty even while `wl-paste`
// hands the same text over.  This is the reading counterpart of the `wl-copy`
// the capture side already writes the clipboard with.
constexpr int kClipboardTimeoutMs = 5000;

// One `wl-paste` run.  `false` means the program could not be started at all,
// which is a different failure from an empty clipboard; `ok` says whether the
// request itself succeeded.
bool runWlPaste(const QStringList &arguments, QByteArray *bytes, bool *ok)
{
    QProcess process;
    process.setProgram(QStringLiteral("wl-paste"));
    process.setArguments(arguments);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start();
    if (!process.waitForStarted(kClipboardTimeoutMs)) {
        return false;
    }
    const bool finished = process.waitForFinished(kClipboardTimeoutMs);
    if (!finished) {
        // A clipboard owner that never answers must not hold the daemon's event
        // loop for longer than this.
        process.kill();
        process.waitForFinished(kClipboardTimeoutMs);
    }
    *bytes = process.readAllStandardOutput();
    *ok = finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    return true;
}

// Puts `text` on the clipboard through `wl-copy`, the writing counterpart of
// the `wl-paste` above.  Qt's own clipboard is no more usable for writing a
// selection than for reading one.
//
// `wl-copy` forks and the child stays alive as the selection owner, so only the
// short-lived process started here is waited for: the copy outlives this call,
// and the daemon keeps its event loop.
bool runWlCopy(const QString &text)
{
    QProcess process;
    process.setProgram(QStringLiteral("wl-copy"));
    // `--` ends the options: a value is content, never a switch.
    process.setArguments({QStringLiteral("--")});
    process.start();
    if (!process.waitForStarted(kClipboardTimeoutMs)) {
        return false;
    }
    process.write(text.toUtf8());
    process.closeWriteChannel();
    const bool finished = process.waitForFinished(kClipboardTimeoutMs);
    if (!finished) {
        process.kill();
        process.waitForFinished(kClipboardTimeoutMs);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

// The same, for pixels: `wl-copy --type image/png` reads the encoding from the
// bytes themselves, so nothing has to be declared beyond the type and the
// result is an image a paste target can take.  The overlay has its own copy of
// this; the two processes share no code.
//
// A pinned image is a few megabytes, and `wl-copy` reads it from the pipe while
// it is being written, so the write has to happen with an event loop running --
// which this process has, but a nested one inside a synchronous wait would
// re-enter the pin stack's own handlers.  The bytes are small enough for a
// single write on any image a pin can hold, and `wl-copy` keeps reading until
// the channel closes, so closing it right after is what ends the transfer.
bool runWlCopyImage(const QImage &image)
{
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
    if (!process.waitForStarted(kClipboardTimeoutMs)) {
        return false;
    }
    process.write(bytes);
    process.closeWriteChannel();
    const bool finished = process.waitForFinished(kClipboardTimeoutMs);
    if (!finished) {
        process.kill();
        process.waitForFinished(kClipboardTimeoutMs);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

// The URLs of a `text/uri-list` payload: one per line, `#` starts a comment.
QList<QUrl> uriListUrls(const QByteArray &payload)
{
    QList<QUrl> urls;
    for (const QByteArray &line : payload.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith('#')) {
            continue;
        }
        urls.append(QUrl::fromEncoded(trimmed));
    }
    return urls;
}

// A QMimeData filled from raw clipboard bytes: `setData` is protected, and the
// card renderer takes the same kind of object the Qt clipboard used to hand
// over.
class RawMimeData : public QMimeData
{
public:
    void set(const QString &type, const QByteArray &bytes)
    {
        setData(type, bytes);
    }
};

// The image encodings worth asking for, best first; any other `image/*` the
// clipboard offers is taken after these.
constexpr const char *kClipboardImageTypes[] = {
    "image/png", "image/jpeg", "image/webp", "image/bmp", "image/tiff",
};

// The types that carry text a card can be rendered from, best first.
constexpr const char *kClipboardTextTypes[] = {
    "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT",
};

// What the clipboard is offering, as far as it can be read.  Every part the
// resolution in `addClipboardPin` needs is fetched up front, because the mimes
// are alternatives: pixels, then a copied file's path, then text.
struct ClipboardPayload {
    bool installed = true; // `wl-paste` could be run at all
    bool offered = false;  // something is copied
    QStringList types;
    QString imageType;
    QByteArray image;
    QByteArray uriList;
    QString text;
    QByteArray html;
    // The X11/Qt convention for a copied color, present when a picker or a
    // toolkit put one there; the text types usually carry the same color.
    QByteArray color;
};

ClipboardPayload readClipboard()
{
    ClipboardPayload payload;
    QByteArray listed;
    bool ok = false;
    if (!runWlPaste({QStringLiteral("--list-types")}, &listed, &ok)) {
        payload.installed = false;
        return payload;
    }
    if (!ok) {
        // Nothing is copied: `wl-paste` exits non-zero and says so.
        return payload;
    }
    payload.offered = true;
    for (const QByteArray &line : listed.split('\n')) {
        const QString type = QString::fromUtf8(line).trimmed();
        if (!type.isEmpty() && !payload.types.contains(type)) {
            payload.types.append(type);
        }
    }

    for (const char *candidate : kClipboardImageTypes) {
        const QString type = QLatin1String(candidate);
        if (payload.types.contains(type)) {
            payload.imageType = type;
            break;
        }
    }
    if (payload.imageType.isEmpty()) {
        for (const QString &type : payload.types) {
            if (type.startsWith(QLatin1String("image/"))) {
                payload.imageType = type;
                break;
            }
        }
    }

    // `--no-newline` keeps the transfer byte-exact: wl-paste appends a newline
    // to text types otherwise, which would turn into a blank line in a card.
    const auto fetch = [&payload](const QString &type) -> QByteArray {
        if (!payload.types.contains(type)) {
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

    if (!payload.imageType.isEmpty()) {
        payload.image = fetch(payload.imageType);
    }
    payload.uriList = fetch(QStringLiteral("text/uri-list"));
    for (const char *candidate : kClipboardTextTypes) {
        const QByteArray bytes = fetch(QLatin1String(candidate));
        if (!bytes.isEmpty()) {
            payload.text = QString::fromUtf8(bytes);
            break;
        }
    }
    payload.html = fetch(QStringLiteral("text/html"));
    payload.color = fetch(QStringLiteral("application/x-color"));
    return payload;
}

// Self-pipe so a termination signal can wake the Qt event loop safely; the
// handler itself only does an async-signal-safe write().
int g_terminateFd[2] = {-1, -1};

void onTerminateSignal(int)
{
    const char marker = 't';
    const ssize_t written = ::write(g_terminateFd[1], &marker, 1);
    static_cast<void>(written);
}

void installTerminateNotifier(QObject *context, std::function<void()> onTerminate)
{
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, g_terminateFd) != 0) {
        return;
    }
    struct sigaction action {};
    action.sa_handler = onTerminateSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    ::sigaction(SIGTERM, &action, nullptr);
    ::sigaction(SIGINT, &action, nullptr);
    // SIGPIPE would otherwise kill the daemon when a client goes away.
    ::signal(SIGPIPE, SIG_IGN);
    auto *notifier = new QSocketNotifier(g_terminateFd[0], QSocketNotifier::Read, context);
    QObject::connect(notifier, &QSocketNotifier::activated, context,
                     [notifier, onTerminate = std::move(onTerminate)] {
                         notifier->setEnabled(false);
                         onTerminate();
                     });
}

// Writes one reply.  The connection is deliberately left open: a client that
// drives a drag sends many requests over one socket, and hanging up after each
// reply would make it pay a connect and an accept per motion event.  A client
// that only wanted one answer (the CLI) simply closes its end, which the
// server turns into a deleteLater.
void respond(QLocalSocket *socket, const QJsonObject &payload)
{
    const QByteArray encoded = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    socket->write(encoded);
    socket->write("\n", 1);
    socket->flush();
}

// After the last pin is gone the daemon owns no surfaces, so it exits and
// lets the next pin command spawn a fresh daemon. The grace period serves a
// concurrent add (or a reply still in flight) before quitting.
constexpr int kIdleQuitMs = 500;

// How long a reply that waits for a presented frame is held before it is sent
// anyway.  The repaint it reports has already been committed when the callback
// is asked for, so the answer is one frame away; past this, the surface is not
// going to draw one at all -- an output with nothing visible on it, a
// compositor that never calls back -- and the client is answered rather than
// left waiting on a frame that is not coming.
constexpr int kFrameWaitMs = 250;

constexpr double kMinScale = 0.1;
constexpr double kMaxScale = 8.0;
// Keep this many logical pixels of the image on some output so a pin can
// never be dragged out of reach.
constexpr int kGrabMargin = 32;

// Fallback output when nothing better is known: Qt's primary screen. The
// daemon cannot see the pointer (a windowless process reports it at 0,0), so
// the CLI resolves the output the user is on with compositor metadata and
// passes it along; see `src/capture/active_output.rs`.
QScreen *fallbackScreen()
{
    return QGuiApplication::primaryScreen();
}

// The Qt screen the CLI's `output_name` names, if any. Qt names its screens
// after the compositor's outputs, which is why the name is the better half of
// the answer: it matches on every compositor that speaks xdg-output, KWin
// included, without any geometry to line up.
QScreen *screenFromName(const QJsonObject &request)
{
    const QString name = request.value(QStringLiteral("output_name")).toString();
    if (name.isEmpty()) {
        return nullptr;
    }
    for (QScreen *screen : QGuiApplication::screens()) {
        if (screen != nullptr && screen->name() == name) {
            return screen;
        }
    }
    return nullptr;
}

// The Qt screen matching the output rect the CLI reported, if any. A
// compositor whose rect does not line up with Qt's screens simply gets the
// fallback.
QScreen *screenFromGeometry(const QJsonObject &request)
{
    const QJsonValue value = request.value(QStringLiteral("output"));
    if (!value.isObject()) {
        return nullptr;
    }
    const QJsonObject rect = value.toObject();
    bool xOk = false;
    bool yOk = false;
    bool widthOk = false;
    bool heightOk = false;
    const int x = rect.value(QStringLiteral("x")).toVariant().toInt(&xOk);
    const int y = rect.value(QStringLiteral("y")).toVariant().toInt(&yOk);
    const int width = rect.value(QStringLiteral("width")).toVariant().toInt(&widthOk);
    const int height = rect.value(QStringLiteral("height")).toVariant().toInt(&heightOk);
    if (!xOk || !yOk || !widthOk || !heightOk || width <= 0 || height <= 0) {
        return nullptr;
    }
    const QRect wanted(x, y, width, height);
    for (QScreen *screen : QGuiApplication::screens()) {
        if (screen != nullptr && screen->geometry() == wanted) {
            return screen;
        }
    }
    return nullptr;
}

// The point the CLI reported as the capture's own top-left, when it reported
// one.  A capture made from a place on the desktop pins back onto that place,
// so pinning a window over itself is seamless; `false` means the request had no
// place to keep and the daemon centres the pin instead.
bool pointFromRequest(const QJsonObject &request, QPoint *out)
{
    const QJsonValue value = request.value(QStringLiteral("at"));
    if (!value.isObject()) {
        return false;
    }
    const QJsonObject point = value.toObject();
    bool xOk = false;
    bool yOk = false;
    const int x = point.value(QStringLiteral("x")).toVariant().toInt(&xOk);
    const int y = point.value(QStringLiteral("y")).toVariant().toInt(&yOk);
    if (!xOk || !yOk) {
        return false;
    }
    *out = QPoint(x, y);
    return true;
}

// The output the CLI asked to pin on, by name first and by geometry second.
// `nullptr` means the request named no output this daemon can find, which
// leaves the choice to `fallbackScreen()`.
QScreen *screenFromRequest(const QJsonObject &request)
{
    QScreen *named = screenFromName(request);
    return named != nullptr ? named : screenFromGeometry(request);
}

// One pinned image. Image, scale and global position live here rather than in a
// widget: a pin may span several outputs, and every surface that shows it needs
// the same shared state.
struct Pin {
    quint64 id = 0;
    QImage image;
    // Device pixels per logical pixel of `image`. A capture brings its own
    // (the scale of the output it came from); a bare file falls back to the
    // density of the output it lands on, i.e. one image pixel per device pixel.
    int density = 1;
    // Logical pixels per source pixel: the natural size, 1/density, times the
    // user's zoom factor.
    double scale = 1.0;
    // Global logical top-left of the image.
    QPoint origin;
    QString label;
    // Where the pixels came from, empty when they came from the clipboard
    // rather than a file. `label` is what to call the pin in a log line; this
    // is what the save dialog offers as its starting name, so a re-save lands
    // beside the original instead of in the Pictures directory.
    QString sourcePath;
    // The formats a pinned color card shows, empty for every other pin. Kept
    // here rather than re-derived when the menu opens: the card was rendered
    // from these rows, so the menu copies exactly what the card printed.
    QVector<ColorRow> colorRows;
    // This capture's HDR half, as PQ codes in a file this daemon owns, or empty
    // for a pin that has none.  The pixels above are the tone map of it, so the
    // pin's size and every hit test are unchanged; this file is what the surface
    // helper is handed as the pin's picture.  A pin whose pixels are replaced
    // from an SDR editor loses it.
    QString hdrPath;
    // The HDR half as it was before any edit, kept beside `original` and for the
    // same reason: `hdrPath` becomes the *flattened* HDR once an edit lands, and
    // that picture has the marks baked into it.  Drawing it under an open editor
    // would put a duplicate of every mark behind the editable ones, and
    // compositing the marks onto it again would draw each of them twice.  So the
    // pristine file is kept and the editor and the helper are both handed it
    // while an edit is open.
    //
    // It is the whole record of the HDR half -- the `VSHTPQ02` header names the
    // white and the six chromaticity coordinates, and the words follow it -- so
    // nothing has to be kept beside it for the picture to be shown exactly.
    QString hdrBasePath;
    // This pin's picture as the surface helper reads it, a PNG in the helper's
    // own directory, or empty for a pin whose picture is its PQ half above.
    //
    // The helper draws every pin, not only the HDR ones -- see `PinHdr` -- and
    // this is how a pin that is not HDR gets there: it reads the PNG and encodes
    // it against the output's own reference white.  It holds `image`, the
    // flattened result, because that is what the screen shows.
    QString picturePath;
    // The same for a pin whose pixels an edit is replacing: the pristine picture
    // the editor's marks belong to.  Written when the edit opens and dropped when
    // it lands, exactly as `hdrBasePath` is and for the same reason -- the editor
    // draws its marks itself, so a helper painting the flattened picture
    // underneath would put a duplicate of each mark behind the editable ones.
    QString pictureBasePath;
    // The pin's own pixels as they were before any edit, and the marks the last
    // edit left on them, exactly as the editor reported them.
    //
    // The pin's `image` is the flattened result -- marks and all -- which is
    // what the screen shows and what a save writes.  Re-editing it would mean
    // drawing on top of the last edit's ink with no way back, so the daemon
    // keeps both halves of what the editor needs to open again: the picture the
    // marks were placed on, and the marks themselves as data.  `edited` is what
    // says whether the two are worth sending; a pin that has never been
    // annotated has neither and opens blank.
    QImage original;
    QJsonArray marks;
    bool edited = false;
    // A directory this pin owns for the pixels of the marks it holds, made on
    // the first mark that has any.  A pasted image is its pixels and the marks
    // name them by path, but the file the editor wrote lives in the directory
    // that edit ran in -- gone by the time the pin is read back, and gone
    // immediately for a pin added from a region session, whose session
    // directory the CLI deletes as soon as the request is answered.  So every
    // such file is copied here, where it lives as long as the pin does.
    std::unique_ptr<QTemporaryDir> assets;
    // Names the copies, so two marks cannot land on one file.
    quint64 assetSequence = 0;

    // Takes the marks an editor reported, copying the pixels they name into a
    // directory this pin owns and re-pointing the marks at the copies.
    //
    // The editor writes each mark's pixels beside the session it was given, and
    // that directory belongs to the edit: the CLI removes it the moment the
    // request is answered.  The daemon keeps the marks, so the files have to
    // move somewhere that outlives the request, and this is that somewhere.
    // A mark with no `pixels` -- every kind but a pasted image -- is left as it
    // is; the path is the only field that is rewritten.
    QJsonArray keepMarks(const QJsonArray &marks)
    {
        QJsonArray kept;
        for (const QJsonValue &value : marks) {
            if (!value.isObject()) {
                kept.push_back(value);
                continue;
            }
            QJsonObject mark = value.toObject();
            const QString path = mark.value(QStringLiteral("pixels")).toString();
            if (path.isEmpty()) {
                kept.push_back(mark);
                continue;
            }
            const QString copy = assetPath();
            if (copy.isEmpty() || !QFile::copy(path, copy)) {
                // The pixels could not be taken.  The mark is dropped rather
                // than kept naming a file that will not be there: a pasted
                // image with no pixels is a mark the editor would refuse on the
                // next open, and one refusal fails the whole session.
                continue;
            }
            mark.insert(QStringLiteral("pixels"), copy);
            kept.push_back(mark);
        }
        return kept;
    }

    // A fresh name inside this pin's asset directory, made on first use, or an
    // empty string when no directory could be made.
    QString assetPath()
    {
        if (assets == nullptr) {
            auto directory = std::make_unique<QTemporaryDir>(
                QDir::tempPath() + QStringLiteral("/vshot-pin-XXXXXX"));
            if (!directory->isValid()) {
                return QString();
            }
            assets = std::move(directory);
        }
        return assets->filePath(QStringLiteral("mark-%1.png").arg(++assetSequence));
    }

    // The pixels an edit should start from: the pristine capture on a pin that
    // has been annotated before, its own image on one that has not.
    const QImage &editBase() const { return edited && !original.isNull() ? original : image; }

    // The HDR half an edit should start from, on the same rule.  Empty for a pin
    // with no HDR half at all, which is every SDR pin and every pin whose capture
    // carried no light above white.
    const QString &editHdrBase() const
    {
        return edited && !hdrBasePath.isEmpty() ? hdrBasePath : hdrPath;
    }

    // The file the helper draws this pin from, empty when this side has none to
    // hand it -- see `syncHdr`, which is where that is decided.
    const QString &picture() const { return hdrPath.isEmpty() ? picturePath : hdrPath; }

    // The picture the helper should draw for the pin an edit is open on.
    //
    // The editor draws every mark itself, live and editable, so the stack under
    // it has to show the pristine picture those marks belong to: a flattened one
    // would put a duplicate of each mark on screen, one that does not move when
    // the mark is dragged and does not go when it is deleted.
    //
    // Which file that is depends on where the pristine pixels live.  A capture
    // with an HDR half already has them in a file of their own -- `hdrPath` on
    // the first edit, `hdrBasePath` on every one after.  Every other pin's are
    // in `original`, which is not a file, so an edit writes one when it opens
    // and names it here.  A pin with neither is one this side could not hand
    // over at all, and the empty answer keeps it out of the stack.
    const QString &editPicture() const
    {
        if (!hdrPath.isEmpty()) {
            return edited && !hdrBasePath.isEmpty() ? hdrBasePath : hdrPath;
        }
        return pictureBasePath.isEmpty() ? picturePath : pictureBasePath;
    }

    QSize displaySize() const
    {
        return QSize(std::max(1, qRound(image.width() * scale)),
                     std::max(1, qRound(image.height() * scale)));
    }

    QSize naturalSize() const
    {
        return QSize(std::max(1, qRound(image.width() / static_cast<double>(density))),
                     std::max(1, qRound(image.height() / static_cast<double>(density))));
    }

    QRect globalRect() const { return QRect(origin, displaySize()); }
};

// Device pixels per logical pixel of an output.
int screenDensity(QScreen *screen)
{
    if (screen == nullptr) {
        return 1;
    }
    return std::clamp(qRound(screen->devicePixelRatio()), 1, 4);
}

// Native pixel size of an output: its logical geometry times its density.
QSize screenNativeSize(QScreen *screen)
{
    if (screen == nullptr) {
        return QSize();
    }
    return screen->geometry().size() * screenDensity(screen);
}

// A density is a whole number of device pixels per logical pixel. A record may
// write it either way ("2" as well as "2.0"), so it is read as a number; a
// fractional output scale is not expressible as a device density and is left
// to the other sources.
bool densityValue(const QString &token, int *out)
{
    bool ok = false;
    const double value = token.toDouble(&ok);
    if (!ok) {
        return false;
    }
    const int rounded = qRound(value);
    if (rounded < 1 || rounded > 4 || qAbs(value - rounded) > 0.05) {
        return false;
    }
    *out = rounded;
    return true;
}

// Density a decoded image declares about itself. This is the fallback for the
// callers that have no PNG bytes left to look at, and for the formats whose
// declaration the PNG chunks cannot show in the first place (a JPEG carries its
// density in the JFIF header). Here only the decoded value is left, so a 96 DPI
// reading is indistinguishable from the 3780 dots per metre Qt reports for a
// PNG that declares nothing at all, and it has to be left to the other
// sources: only `pngDeclaredDensity` can recognise a 1x declaration. A print
// resolution is not a device density either, so the value has to be a
// near-exact multiple of 96 DPI.
int declaredDensity(const QImage &image)
{
    const int dotsPerMeter = image.dotsPerMeterX();
    if (dotsPerMeter <= 0) {
        return 0;
    }
    const double ratio = dotsPerMeter * 0.0254 / 96.0;
    const int rounded = qRound(ratio);
    if (rounded < 2 || rounded > 4 || qAbs(ratio - rounded) > 0.05) {
        return 0;
    }
    return rounded;
}

// Where a screenshot tool records the output it last captured. Such a tool is
// the only party that knows which screen an image came from — grim, satty and
// spectacle write no density into the image — so this record is how the source
// is recovered. Overridable for a different layout or an isolated test.
QString sourceRecordPath()
{
    const QByteArray override = qgetenv("VSHOT_PIN_SOURCE_FILE");
    if (!override.isEmpty()) {
        return QString::fromLocal8Bit(override);
    }
    return QStringLiteral("/tmp/screenshot-path");
}

QString readSmallFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    // A record file holds a line or two; anything bigger is not one.
    if (file.size() > 64 * 1024) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

// Reads a density out of a record file. A bare number stands on its own; a
// `<path> <density>` pair is only used when the path names the image being
// pinned, so an older capture's scale is never applied to this one.
bool parseRecordedDensity(const QString &text, const QString &wanted, int *out)
{
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList fields = line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.isEmpty()) {
            continue;
        }
        if (fields.size() == 1) {
            continue; // a bare number only means something in a per-image file
        }
        if (wanted.isEmpty() || QFileInfo(fields.at(0)).absoluteFilePath() != wanted) {
            continue;
        }
        if (densityValue(fields.at(1), out)) {
            return true;
        }
    }
    return false;
}

// The scale the image's producer recorded, if any: a `<image>.scale` sidecar
// holding one number, else the screenshot tool's record of its last capture.
int recordedDensity(const QString &sourcePath)
{
    if (sourcePath.isEmpty()) {
        return 0;
    }
    int value = 0;
    const QString sidecar = readSmallFile(sourcePath + QStringLiteral(".scale"));
    if (!sidecar.isEmpty()) {
        const QStringList fields =
            sidecar.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (!fields.isEmpty() && densityValue(fields.last(), &value)) {
            return value;
        }
    }
    const QString wanted = QFileInfo(sourcePath).absoluteFilePath();
    const QString record = sourceRecordPath();
    if (!record.isEmpty() && parseRecordedDensity(readSmallFile(record), wanted, &value)) {
        return value;
    }
    return 0;
}

// Density to assume for an image that does not state one.
//
// An image cannot hold more pixels than the screen it was captured on, so an
// image that does not fit the target's native resolution was not captured
// there: it came from a bigger or denser output. Pinning it one-to-one would
// make it larger than it ever was on screen, so the density of the screen that
// could have produced it is used instead — the smallest one that still holds
// every pixel. An image that does fit keeps the target's density, which is
// exactly one image pixel per screen pixel.
int inferDensity(const QImage &image, QScreen *target)
{
    const int targetDensity = screenDensity(target);
    if (image.isNull() || target == nullptr) {
        return targetDensity;
    }
    const QSize targetNative = screenNativeSize(target);
    if (image.width() <= targetNative.width() && image.height() <= targetNative.height()) {
        return targetDensity;
    }
    int inferred = 0;
    qint64 smallest = std::numeric_limits<qint64>::max();
    for (QScreen *screen : QGuiApplication::screens()) {
        const QSize native = screenNativeSize(screen);
        if (native.isEmpty() || image.width() > native.width() ||
            image.height() > native.height()) {
            continue;
        }
        const qint64 pixels = static_cast<qint64>(native.width()) * native.height();
        if (pixels < smallest) {
            smallest = pixels;
            inferred = screenDensity(screen);
        }
    }
    return inferred > 0 ? inferred : targetDensity;
}

// A density and the source that decided it, so `VSHOT_PIN_DEBUG` can say why a
// pin came out the size it did.
struct PinDensity {
    int value = 1;
    const char *source = "the fallback";
};

// Device pixels per logical pixel of the image being pinned, so the pin takes
// the same room on screen it had where it came from. In order: what vshot's own
// capture stated, what the PNG declares in its chunks, what the decoded image
// declares, what the producer recorded, and else what the image's size and the
// target output imply. `sourceBytes` is the PNG the image was decoded from when
// the caller has it (a clipboard payload); `sourcePath` is where it came from,
// read for its chunks when the bytes are not at hand.
PinDensity resolveDensity(const QJsonObject &request, QScreen *target, const QImage &image,
                          const QString &sourcePath, const QByteArray &sourceBytes)
{
    bool ok = false;
    const int stated = request.value(QStringLiteral("density")).toVariant().toInt(&ok);
    if (ok && stated > 0) {
        return {std::clamp(stated, 1, 4), "the request (--density or VSHOT_PIN_DENSITY)"};
    }
    // The PNG's own chunks first: this is the one source that can say "1x",
    // which is what a capture on a scale-1 output declares.
    if (const int declared = pngDeclaredDensity(sourceBytes); declared > 0) {
        return {declared, "the PNG's own chunks (clipboard payload)"};
    }
    if (const int declared = pngDeclaredDensityOfFile(sourcePath); declared > 0) {
        return {declared, "the PNG's own chunks (the file)"};
    }
    if (const int declared = declaredDensity(image); declared > 0) {
        return {declared, "the decoded PNG density"};
    }
    if (const int recorded = recordedDensity(sourcePath); recorded > 0) {
        return {recorded, "the producer's record"};
    }
    return {inferDensity(image, target), "the image size and the target output"};
}

// Why `path` is not a PQ image the surface helper could read, or an empty
// string when it is.  This is the one thing the daemon can check about an HDR
// half it never decodes, and it has to check something: the helper leaves a
// pin's rect transparent, so a half it cannot read would leave that rect
// painted by neither side and the pin would read as a hole in the screen.
//
// The magic names the layout, and the layout names the gamut: a file carrying
// another magic was written by a VShot whose format this build does not know --
// an older helper against a newer daemon, which is what a stale binary beside a
// fresh one produces -- and an older helper would draw these codes as if they
// were BT.2020, shifting every colour of a wide-gamut capture.
//
// The reason travels back rather than a bare `false`, because a refusal the user
// never hears about is indistinguishable from a pin that simply has no HDR half:
// the picture comes out dim and there is nothing to say why.  Naming the magic
// found and the magic expected is what tells a stale helper apart from a
// truncated or foreign file.
constexpr int kPqHeader = 44;

QString pqRejection(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QStringLiteral("it cannot be opened: %1").arg(file.errorString());
    }
    const QByteArray header = file.read(kPqHeader);
    if (header.size() != kPqHeader) {
        return QStringLiteral("it holds %1 bytes, short of the %2-byte header")
            .arg(file.size())
            .arg(kPqHeader);
    }
    if (!header.startsWith(QByteArrayLiteral("VSHTPQ02"))) {
        return QStringLiteral("it starts with `%1` where this build writes `VSHTPQ02`")
            .arg(QString::fromLatin1(header.left(8)));
    }
    const auto *bytes = reinterpret_cast<const uchar *>(header.constData());
    const quint32 width = qFromLittleEndian<quint32>(bytes + 8);
    const quint32 height = qFromLittleEndian<quint32>(bytes + 12);
    if (width == 0 || height == 0) {
        return QStringLiteral("its header says %1x%2").arg(width).arg(height);
    }
    const qint64 expected = kPqHeader + static_cast<qint64>(width) * height * 4;
    if (file.size() != expected) {
        return QStringLiteral("it holds %1 bytes where its %2x%3 header calls for %4")
            .arg(file.size())
            .arg(width)
            .arg(height)
            .arg(expected);
    }
    return QString();
}

// Where the HDR surface helper may live: next to this executable (installed
// layouts) or under the crate's own `target/` one and two levels up (the
// in-tree `build-qt/` + `cargo build` development layouts).  `VSHOT_HDR_HELPER`
// overrides the search the way `VSHOT_QT_HELPER` does on the other side.
QString hdrHelperProgram()
{
    const QByteArray override = qgetenv("VSHOT_HDR_HELPER");
    if (!override.isEmpty()) {
        const QString path = QString::fromLocal8Bit(override);
        return QFileInfo(path).isExecutable() ? path : QString();
    }
    const QString directory = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        directory + QStringLiteral("/vshot"),
        directory + QStringLiteral("/../target/release/vshot"),
        directory + QStringLiteral("/../target/debug/vshot"),
        directory + QStringLiteral("/../../target/release/vshot"),
        directory + QStringLiteral("/../../target/debug/vshot"),
    };
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isExecutable()) {
            return info.absoluteFilePath();
        }
    }
    return QString();
}

// The whole pin stack: a helper process that shows every pinned picture, its
// shadow and its rim, on layer surfaces of its own.
//
// A pin surface is a Qt window, and Qt builds a window's colour description from
// a `QColorSpace` -- named BT.2020, with no luminances.  The compositor leaves a
// surface's pixels alone only when its description is the output's own, and that
// description carries the output's luminances, so a Qt window holding PQ codes
// is converted and tone-mapped down to SDR.  The helper is a plain Wayland
// client of our own: it can put the output's own description on a surface, whose
// ten-bit buffer is then a passthrough, so the pixels reach the panel as the
// light they stand for.
//
// It draws *every* pin, not only the HDR ones, and that is what keeps the stack
// in one order.  The helper's surfaces sit on the same layer as ours and the
// compositor stacks a layer in map order with no restack request, so they have
// to be up first -- and anything the Qt surfaces painted afterwards would land
// above every pin the helper draws, including the pins in front of it: a pin's
// rim showing over a pin that was meant to cover it.  An SDR pin therefore
// travels as a PNG and is encoded against this output's own reference white
// (`PinHdr::takePicture`), and the Qt surfaces keep only what belongs to no
// single pin in the stack: the badges, the menus, the `HDR` tag and the input
// mask.
//
// The helper's surfaces have to be mapped before the daemon's first one: the
// process is started as the daemon starts, and `ensureMapped` -- called before
// the first surface of ours is mapped -- waits for it to say it is on screen.
// Nothing else waits on it: a helper that never comes up simply leaves every pin
// an ordinary SDR one painted by the Qt surface itself.
class PinHdr
{
public:
    PinHdr()
    {
        program_ = hdrHelperProgram();
        if (program_.isEmpty() || !directory_.isValid()) {
            return;
        }
        socketPath_ = directory_.filePath(QStringLiteral("hdr.sock"));
        process_.setProgram(program_);
        process_.setArguments({QStringLiteral("--pin-hdr-server"), socketPath_});
        process_.setStandardInputFile(QProcess::nullDevice());
        process_.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        // Started here so it has the whole first command's round trip to get on
        // screen; waited for in `ensureMapped`, which runs before the first
        // surface of ours is mapped.
        process_.start();
    }

    ~PinHdr()
    {
        if (process_.state() == QProcess::NotRunning) {
            return;
        }
        // Closing the connection is what tells the helper to unmap and exit; a
        // signal would leave its layer surfaces behind on some compositors.
        if (socket_.state() == QLocalSocket::ConnectedState) {
            socket_.write("{\"cmd\":\"quit\"}\n");
            socket_.flush();
        }
        socket_.disconnectFromServer();
        if (!process_.waitForFinished(750)) {
            process_.terminate();
            if (!process_.waitForFinished(750)) {
                process_.kill();
                process_.waitForFinished(750);
            }
        }
    }

    // Makes sure the helper is on screen and has answered with the outputs it
    // could put an HDR surface on.  Called once, before the first Qt surface of
    // ours is mapped; afterwards the answer is a flag.
    void ensureMapped()
    {
        if (attempted_) {
            return;
        }
        attempted_ = true;
        if (program_.isEmpty() || process_.state() != QProcess::Running) {
            return;
        }
        // The helper binds its socket before it accepts anything, so the file
        // appearing is the signal that connecting will work; a helper that dies
        // first (a compositor with no layer shell, say) is not waited for.
        QElapsedTimer timer;
        timer.start();
        while (!QFile::exists(socketPath_)) {
            if (process_.state() == QProcess::NotRunning || timer.elapsed() > kSocketWaitMs) {
                stop();
                return;
            }
            QThread::msleep(5);
        }
        socket_.connectToServer(socketPath_);
        if (!socket_.waitForConnected(kConnectWaitMs)) {
            stop();
            return;
        }
        waitForMapped();
        if (!mapped_) {
            // Nothing will arrive late to land above the surfaces this daemon is
            // about to map.
            stop();
            return;
        }
        // The helper answers every stack it composes with `{"ok":true}`, and
        // every stack it cannot compose with `{"ok":false,"error":...}`.  Those
        // lines are read in `drainReplies`/`waitForReply` rather than here, so
        // that the one caller that has to act on the answer gets it: a lambda on
        // this signal would race the blocking wait for the same bytes.
    }

    bool mapped() const { return mapped_; }

    // Whether the helper's surface on `name` carries that output's own colour
    // description.  Only there can a ten-bit buffer be read as HDR.
    bool isHdrOutput(const QString &name) const { return outputs_.contains(name); }

    // Copies one PQ image into the helper's directory and answers the copy's
    // path: the CLI's file is gone the moment it is answered, and the helper
    // reads the pixels by path, so the copy is what lives as long as the pin.
    //
    // An empty answer means the pin has no HDR half, which is ordinary -- a
    // file, a clipboard image, a capture on an SDR output.  A half that was
    // offered and refused is not, so the reason is left in `reason` for the
    // caller to report; without that the two read alike and the pin just comes
    // out dim.
    QString takeImage(const QString &source, QString *reason = nullptr)
    {
        if (reason != nullptr) {
            reason->clear();
        }
        if (source.isEmpty()) {
            return QString();
        }
        if (!mapped_ || !directory_.isValid()) {
            // No helper at all is not a refusal: there is simply no surface to
            // put an HDR half on, and the pin's SDR picture is complete without
            // it.  Failing the pin here would lose the capture on every
            // compositor the helper cannot come up on.
            return QString();
        }
        const QString rejected = pqRejection(source);
        if (!rejected.isEmpty()) {
            if (reason != nullptr) {
                *reason = rejected;
            }
            return QString();
        }
        const QString destination =
            directory_.filePath(QStringLiteral("pin-%1.pq").arg(++sequence_));
        if (!QFile::copy(source, destination)) {
            if (reason != nullptr) {
                *reason = QStringLiteral("it could not be copied beside the helper");
            }
            return QString();
        }
        return destination;
    }

    // Writes one SDR picture into the helper's directory as a PNG and answers
    // its path, or an empty string when there is no helper or the file could not
    // be written.
    //
    // Every pin the helper draws needs one, because the helper is a single
    // surface below the daemon's and a layer's surfaces are stacked in map order
    // with no restack request: whatever the Qt surface painted would composite
    // above every pin the helper draws, including the ones in front of it.  So
    // the whole stack is the helper's, and this is how a pin that is not HDR
    // gets there -- the helper reads the PNG and encodes it against the output's
    // own reference white.
    //
    // Nothing is cached across calls: a pin's pixels change only when an edit
    // replaces them, and an edit sends the whole stack anyway.
    QString takePicture(const QImage &image)
    {
        if (image.isNull() || !mapped_ || !directory_.isValid()) {
            return QString();
        }
        const QString destination =
            directory_.filePath(QStringLiteral("pin-%1.png").arg(++sequence_));
        return image.save(destination, "PNG") ? destination : QString();
    }

    // The same for a picture the caller already has as a PNG file -- what the
    // editor wrote when it replaced a pin's pixels.  Copying it is both cheaper
    // than decoding and re-encoding, and exact: an image the daemon read back
    // and saved again could come out a byte different from the one the editor
    // drew, and this file is the one the user's marks were flattened into.
    QString takePictureFile(const QString &source)
    {
        if (source.isEmpty() || !mapped_ || !directory_.isValid()) {
            return QString();
        }
        const QString destination =
            directory_.filePath(QStringLiteral("pin-%1.png").arg(++sequence_));
        return QFile::copy(source, destination) ? destination : QString();
    }

    // Hands the helper the whole stack of pins, in the daemon's own paint order,
    // together with the look they are drawn with.  Answers whether anything was
    // sent, since only a stack the helper was actually handed can be refused by
    // it.
    //
    // `active` is not in the array: which pin shows the live rim is what the
    // pointer is doing rather than what the stack holds, and a moved pointer is
    // not a stack change.  It is compared with the rest so a change of it still
    // reaches the helper.
    bool sync(const QJsonArray &pins, const QJsonObject &style, quint64 active)
    {
        if (!mapped_ || socket_.state() != QLocalSocket::ConnectedState) {
            return false;
        }
        // The same stack, the same look and the same live pin again -- a pin
        // coming to the front, a stack the daemon repeats -- is nothing for the
        // helper to do.  Its picture is derived from these three alone, so
        // asking for it twice would have it recompose the whole output for an
        // image identical to the one already on it.
        if (sent_ && pins == sent_.value() && style == sentStyle_ && active == sentActive_) {
            return false;
        }
        sent_ = pins;
        sentStyle_ = style;
        sentActive_ = active;
        QJsonObject command;
        command.insert(QStringLiteral("cmd"), QStringLiteral("pins"));
        command.insert(QStringLiteral("style"), style);
        command.insert(QStringLiteral("pins"), pins);
        command.insert(QStringLiteral("active"), static_cast<qint64>(active));
        QByteArray line = QJsonDocument(command).toJson(QJsonDocument::Compact);
        line.append('\n');
        socket_.write(line);
        socket_.flush();
        return true;
    }

    // The helper's answer to the stack just handed over, or an empty string
    // when it composed it.  It composes the whole stack in one go and refuses
    // the whole command when any file in it cannot be read, so the answer is
    // about the stack and not about one pin.
    //
    // This is the only place a mismatch the daemon cannot see for itself shows
    // up: the daemon checks the magic it writes, so a helper built against
    // another .pq layout is refused over there and nowhere else.  A refusal
    // leaves every HDR pin showing the last picture the helper managed, which is
    // what makes it worth carrying back to whoever asked rather than dropping.
    //
    // It waits, because the answer has to be in hand before the request that
    // caused it is answered -- otherwise there is nothing left to put it in.
    // The wait is bounded, and a helper that is slow rather than refusing is not
    // an error: the pin is composed all the same, just later than this reply.
    QString refusalAfterSync()
    {
        QElapsedTimer timer;
        timer.start();
        QByteArray buffer;
        while (socket_.state() == QLocalSocket::ConnectedState) {
            if (socket_.bytesAvailable() == 0) {
                if (timer.hasExpired(kReplyWaitMs)) {
                    return QString();
                }
                if (!socket_.waitForReadyRead(50)) {
                    continue;
                }
            }
            buffer.append(socket_.readAll());
            qsizetype newline = -1;
            while ((newline = buffer.indexOf('\n')) >= 0) {
                const QByteArray line = buffer.left(newline);
                buffer.remove(0, newline + 1);
                const QJsonDocument document = QJsonDocument::fromJson(line);
                if (!document.isObject()) {
                    continue;
                }
                const QJsonObject reply = document.object();
                // The `mapped` event is not an answer to a stack; everything
                // else the helper sends is.
                if (reply.contains(QStringLiteral("event"))) {
                    continue;
                }
                if (reply.value(QStringLiteral("ok")).toBool()) {
                    return QString();
                }
                return reply.value(QStringLiteral("error")).toString();
            }
        }
        return QString();
    }

    // Reads and drops whatever the helper has already answered, so a refusal
    // left over from an earlier stack is never mistaken for this one's.  Called
    // before a stack is handed over.
    void discardReplies()
    {
        if (socket_.state() == QLocalSocket::ConnectedState) {
            socket_.readAll();
        }
    }

private:
    // Waits for the helper's `mapped` line and remembers the outputs it named.
    void waitForMapped()
    {
        QByteArray buffer;
        QElapsedTimer timer;
        timer.start();
        while (socket_.state() == QLocalSocket::ConnectedState) {
            if (socket_.bytesAvailable() == 0) {
                if (timer.hasExpired(kMappedWaitMs)) {
                    return;
                }
                if (!socket_.waitForReadyRead(250)) {
                    continue;
                }
            }
            buffer.append(socket_.readAll());
            int newline = -1;
            while ((newline = buffer.indexOf('\n')) >= 0) {
                const QByteArray line = buffer.left(newline);
                buffer.remove(0, newline + 1);
                const QJsonDocument document = QJsonDocument::fromJson(line);
                if (!document.isObject()) {
                    continue;
                }
                const QJsonObject object = document.object();
                if (object.value(QStringLiteral("event")).toString()
                    != QStringLiteral("mapped")) {
                    continue;
                }
                const QJsonArray outputs = object.value(QStringLiteral("outputs")).toArray();
                for (const QJsonValue &value : outputs) {
                    outputs_.append(value.toString());
                }
                mapped_ = true;
                return;
            }
        }
    }

    // Gives up on a helper that never answered.
    void stop()
    {
        socket_.disconnectFromServer();
        if (process_.state() != QProcess::NotRunning) {
            process_.terminate();
            if (!process_.waitForFinished(250)) {
                process_.kill();
            }
        }
    }

    static constexpr int kSocketWaitMs = 2000;
    static constexpr int kConnectWaitMs = 1000;
    static constexpr int kMappedWaitMs = 2000;
    // How long the answer to one stack is waited for before the request that
    // caused it is answered without it.  A helper composing a full 4K output
    // takes a fraction of this; past it, the helper is not refusing but slow.
    static constexpr int kReplyWaitMs = 400;

    QString program_;
    QString socketPath_;
    QTemporaryDir directory_;
    QProcess process_;
    QLocalSocket socket_;
    QStringList outputs_;
    // The pin stack the helper was last handed, so an unchanged one is not
    // handed over again, and the look and the live pin that went with it.
    std::optional<QJsonArray> sent_;
    QJsonObject sentStyle_;
    quint64 sentActive_ = 0;
    bool attempted_ = false;
    bool mapped_ = false;
    quint64 sequence_ = 0;
};

// Owns every pinned surface and dispatches daemon commands. It inherits
// QObject only to reuse the functor-based connect() lifetime; it declares no
// signals or slots of its own, so the build stays moc-free.
class PinServer final : public QObject {
public:
    PinServer(QLocalServer *server, QString socketPath)
        : server_(server)
        , socketPath_(std::move(socketPath))
        , debug_(qEnvironmentVariableIsSet("VSHOT_PIN_DEBUG"))
    {
        idleQuit_ = new QTimer(this);
        idleQuit_->setSingleShot(true);
        connect(idleQuit_, &QTimer::timeout, this, [this] {
            // addImage stops the timer, so a timeout really means empty.
            if (pins_.isEmpty()) {
                QCoreApplication::quit();
            }
        });
        // The backstop for a held reply: a compositor that never hands back a
        // frame callback must not leave a client waiting for ever.  The wait is
        // short because a frame is what it is waiting for -- the repaint was
        // committed before the callback was asked for, so the answer is one
        // frame away or the surface is not going to draw one at all.
        frameDeadline_ = new QTimer(this);
        frameDeadline_->setSingleShot(true);
        connect(frameDeadline_, &QTimer::timeout, this, [this] { releaseHeldReplies(); });
    }

    void handleNewConnection()
    {
        while (QLocalSocket *socket = server_->nextPendingConnection()) {
            connect(socket, &QLocalSocket::readyRead, this,
                    [this, socket] { readRequest(socket); });
            connect(socket, &QLocalSocket::disconnected, socket, [this, socket] {
                // The client is gone; whatever it left mid-request goes with it.
                buffer_.remove(socket);
                reportedActive_.remove(socket);
                socket->deleteLater();
            });
        }
    }

    // Keeps the stack rendering on every output. The compositor can add or
    // remove outputs at any time, and a surface belongs to exactly one of them,
    // so the mapping has to follow. An output that comes and goes while nothing
    // is pinned has nothing to show and gets no surface.
    void watchScreens()
    {
        connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *screen) {
            if (surfaces_.isEmpty()) {
                return;
            }
            addSurface(screen);
            syncAll();
        });
        connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen *screen) {
            if (surfaces_.isEmpty()) {
                return;
            }
            dropSurface(screen);
            for (Pin *pin : pins_) {
                pin->origin = clampOrigin(*pin, pin->origin);
            }
            syncAll();
        });
    }

    // Unmaps every surface before the process goes away. Leaving a mapped
    // layer surface behind can wedge the compositor's output frames.
    void shutdownAll()
    {
        const QVector<Pin *> pins = pins_;
        pins_.clear();
        byId_.clear();
        editingPinId_ = 0;
        notifyActive();
        destroySurfaces();
        for (Pin *pin : pins) {
            delete pin;
        }
    }

private:
    // One answer held until the frame carrying the change it reports has been
    // presented.  The socket is a QPointer: a client that gave up and closed
    // while the daemon waited must not be written to.
    struct HeldReply
    {
        QPointer<QLocalSocket> socket;
        QJsonObject reply;
    };

    // A surface put a frame on the screen, so whatever the daemon was holding
    // for that frame is now true and can be answered.
    //
    // Only the first frame after the answers were queued releases them: the
    // request went out with the repaint this batch caused, and that repaint is
    // the one the client is waiting to see.  A later frame says nothing new.
    void framePresented()
    {
        if (pendingReplies_.isEmpty()) {
            return;
        }
        frameDeadline_->stop();
        const QVector<HeldReply> held = std::move(pendingReplies_);
        pendingReplies_.clear();
        for (const HeldReply &entry : held) {
            if (entry.socket != nullptr) {
                respond(entry.socket, entry.reply);
            }
        }
    }

    // Answers anything still held, so a compositor that never delivers a frame
    // callback -- a surface with nothing to draw, an output that went away --
    // cannot leave a client waiting on a reply that will never come.  The
    // change is applied either way; only the confirmation is early.
    void releaseHeldReplies()
    {
        framePresented();
    }

    static QJsonObject okReply()
    {
        return QJsonObject{{QStringLiteral("ok"), true}};
    }

    // Arms the idle quit when no pins are left. Called after the last pin is
    // gone and when an add fails on an empty daemon.
    void armIdleQuit()
    {
        if (pins_.isEmpty()) {
            idleQuit_->start(kIdleQuitMs);
        }
    }

    static QJsonObject error(const QString &message)
    {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"), message}};
    }

    // Requests are newline-terminated single JSON objects.  One connection may
    // carry as many as the client sends: a drag pipelines its positions, so the
    // loop below drains every complete line before it answers.
    void readRequest(QLocalSocket *socket)
    {
        buffer_[socket] += socket->readAll();
        QElapsedTimer clock;
        if (debug_) {
            clock.start();
        }
        // Positions are applied as they are read but the stack is rendered once,
        // after the whole batch: a client that pipelines several drag positions
        // gets one repaint and one answer each, and every answer carries the
        // position that repaint actually landed on.
        struct Moved
        {
            QLocalSocket *socket;
            quint64 id;
            // Whether this particular move asked for its answer to wait for a
            // presented frame.  A client with more than one move in flight has
            // to be able to tell which answer is the one that ends its handoff,
            // so the ask is echoed in the reply.
            bool ack;
        };
        QVector<Moved> moved;
        // Adds whose HDR half only the helper can accept or refuse: they wait
        // for the one answer that covers the stack rendered below.
        struct Deferred
        {
            QLocalSocket *socket;
            QJsonObject reply;
            bool ack;
        };
        QVector<Deferred> renders;
        // Whether anything in this batch changed the stack by adding a pin.  An
        // add leaves nothing in `moved` -- it is not a move -- and only an add
        // that carried an HDR half leaves anything in `renders`, so without this
        // an ordinary add would never render: the pin would sit in the model,
        // invisible, until some later command happened to compose the stack,
        // which is what made an SDR pin appear only once an HDR one was pinned
        // after it.  Set from the reply rather than from the command, because
        // the stack is `addPin`'s to change and a refusal changes nothing.
        bool added = false;
        qsizetype newline = -1;
        while ((newline = buffer_[socket].indexOf('\n')) >= 0) {
            const QByteArray line = buffer_[socket].left(newline);
            buffer_[socket].remove(0, newline + 1);

            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                respond(socket, error(QStringLiteral("invalid pin request JSON: %1")
                                          .arg(parseError.errorString())));
                continue;
            }
            const QJsonObject request = document.object();
            const QString command = request.value(QStringLiteral("cmd")).toString();
            // A client that wants to be told which pin is the live one as it
            // changes.  Only the pin editor asks, and only because its frame is
            // Qt chrome drawn above every pin surface: a frame left up for a pin
            // the user has moved on from would sit on top of every other pin,
            // and the compositor offers no way to put it back underneath.  The
            // answer is a line of its own -- no `ok`, so it is never mistaken
            // for a move's reply by a client that reads the socket for those --
            // and it goes out on every change, which is why it is asked for
            // rather than pushed at everyone who connects.
            if (command == QStringLiteral("watch-active")) {
                const quint64 live = liveEditingPin();
                reportedActive_.insert(socket, live);
                respond(socket, activeNotice(live));
                continue;
            }
            // The pin editor putting the pin it is annotating back on top.
            //
            // It has to ask because the pin's own surface cannot hear the click:
            // the editor's layer surface holds the keyboard and covers the
            // output, so the pointer never reaches the pin surface underneath,
            // and that surface is where `bringToFront` is normally reached from.
            // Clicking a pin that happens to be under the editor is the user
            // saying which pin they mean, and for the one being edited that is
            // exactly the gesture that has to bring it back to the front.
            //
            // Handled here rather than in `dispatch` because it has nothing to
            // render: the stack it changes is `bringToFront`'s to hand over.
            // The answer carries no `ok`, so a client reading this socket for
            // its move replies never mistakes it for one.
            if (command == QStringLiteral("raise")) {
                const quint64 id = request.value(QStringLiteral("id")).toVariant().toULongLong();
                Pin *pin = id != 0 ? byId_.value(id, nullptr) : nullptr;
                if (pin != nullptr) {
                    bringToFront(pin);
                }
                QJsonObject reply;
                reply.insert(QStringLiteral("raised"), static_cast<qint64>(pin != nullptr ? id : 0));
                respond(socket, reply);
                continue;
            }
            // A client that asks to be told when its change is on the screen,
            // not merely applied.  `ack` is that ask: the reply waits for the
            // compositor's frame callback for the repaint this batch causes.
            // Only ever set by a client that is about to stop drawing, and only
            // on the request it is about to stop on -- a drag pipelined ahead of
            // it carries the ask on that one move alone.
            const bool ack = request.value(QStringLiteral("ack")).toBool();
            if (command == QStringLiteral("move")) {
                quint64 id = 0;
                const QJsonObject reply = movePin(request, &id);
                if (reply.value(QStringLiteral("ok")).toBool()) {
                    moved.append({socket, id, ack});
                } else {
                    respond(socket, reply);
                    armIdleQuit();
                }
                continue;
            }
            // An `add` whose HDR half the helper will refuse has to hear about
            // it in the same reply, and the answer only exists once the stack
            // has been handed over.  So the add is not answered here: it is
            // answered below, after the render, like a move.  `addPin` itself
            // does not render, so nothing is composed twice.
            if (command == QStringLiteral("add")) {
                const QJsonObject reply = addPin(request);
                if (reply.value(QStringLiteral("ok")).toBool()) {
                    added = true;
                }
                // Only an add that brought an HDR half has anything to wait for
                // from the helper: its answer to the stack the pin lands in.
                // Every other add is answered here -- unless the client asked
                // for a frame, which is the pin editor handing over the picture
                // it is still drawing, and then the answer waits for the stack
                // below the same way a move's does.
                const bool deferred =
                    reply.value(QStringLiteral("ok")).toBool()
                    && (!request.value(QStringLiteral("hdr")).toString().isEmpty() || ack);
                if (deferred) {
                    renders.append({socket, reply, ack});
                } else {
                    respond(socket, reply);
                    if (!reply.value(QStringLiteral("ok")).toBool(true)) {
                        armIdleQuit();
                    }
                }
                continue;
            }
            const bool quit = command == QStringLiteral("quit");
            // The command is asked what it changed before it is asked to do it:
            // only the two adds change the stack, and every other command
            // renders for itself -- a show, a hide or a zoom does not wait for
            // the batch to end, and must not be made to.
            const bool adds = command == QStringLiteral("add-clipboard");
            const QJsonObject reply = dispatch(request);
            if (adds && reply.value(QStringLiteral("ok")).toBool()) {
                added = true;
            }
            respond(socket, reply);
            // A daemon that owns nothing has no reason to stay resident, and a
            // failed first add (an empty clipboard, an unreadable file) would
            // otherwise leave one running forever with nothing pinned.
            if (!reply.value(QStringLiteral("ok")).toBool(true)) {
                armIdleQuit();
            }
            if (quit) {
                shutdownAll();
                QCoreApplication::quit();
                return;
            }
        }
        if (buffer_.value(socket).isEmpty()) {
            buffer_.remove(socket);
        }
        // Nothing to render means nothing to answer, and an add that is not
        // waiting on the helper was already answered above.  An add that landed
        // is the exception: it changed the stack, and the stack is rendered
        // here, once, for the whole batch.
        if (moved.isEmpty() && renders.isEmpty() && !added) {
            return;
        }
        // Only wait for the helper's verdict when it was actually handed a new
        // stack.  A stack it already has is nothing for it to answer, and
        // blocking on a reply that is never coming is a stall: it used to cost
        // every motion event of every drag the full deadline.
        //
        // Whatever the helper has already said is dropped first, so the verdict
        // read below is this stack's and not an earlier one's: `syncAll` is
        // called from places that never read the answer -- a zoom, a show -- and
        // their replies would otherwise be waiting in the buffer to be mistaken
        // for this one's.
        hdr_.discardReplies();
        const bool handedHdr = syncAll();
        // The stack has been handed to every surface and queued for painting;
        // nothing is on the screen yet.  A client that asked to be told when it
        // is -- the pin editor, which has to stop drawing without leaving a gap
        // -- has its answer held until the compositor's own frame callback
        // arrives, so it is released only once the picture it was drawing over
        // is really there.  Every other client is answered at once, as before.
        const auto answer = [this](QLocalSocket *socket, const QJsonObject &reply, bool ack) {
            // The ask is echoed back so the client knows which of its answers
            // is the one that waited for the frame: a move pipelined behind
            // another gets an ordinary reply, and only this one ends a handoff.
            QJsonObject sent = reply;
            if (ack) {
                sent.insert(QStringLiteral("ack"), true);
                pendingReplies_.append({socket, sent});
            } else {
                respond(socket, sent);
            }
        };
        for (const Moved &entry : moved) {
            const Pin *pin = byId_.value(entry.id, nullptr);
            if (pin == nullptr) {
                respond(entry.socket,
                        error(QStringLiteral("pin %1 no longer exists").arg(entry.id)));
                continue;
            }
            answer(entry.socket, moveReply(*pin), entry.ack);
        }
        // One stack was handed over, so one answer came back: the same verdict
        // covers every add in this batch that carried an HDR half.  The pin
        // stays pinned either way -- it still has its SDR picture, which is
        // what the user has to look at while fixing the mismatch.
        //
        // Read only when there is an add to put it in.  The verdict exists to
        // answer a client that just pinned something and needs to know the HDR
        // half did not take; a batch of pure moves has nobody to tell, and
        // waiting for the helper to compose before answering them is a stall on
        // every motion event of a drag -- which is exactly what the drag cannot
        // afford.
        const QString refused = (handedHdr && !renders.isEmpty()) ? hdr_.refusalAfterSync()
                                                                  : QString();
        for (const Deferred &entry : renders) {
            if (refused.isEmpty()) {
                answer(entry.socket, entry.reply, entry.ack);
            } else {
                answer(entry.socket,
                       error(QStringLiteral("the HDR surface helper refused the pin "
                                            "stack: %1")
                                 .arg(refused)),
                       entry.ack);
            }
        }
        if (!pendingReplies_.isEmpty()) {
            // Armed after the answers are queued, so a frame landing while they
            // are being assembled cannot release them early: the repaint this
            // batch caused is committed by `syncAll` above, and the callback
            // asked for here is the one that follows it.
            for (const QPointer<PinSurface> &surface : surfaces_) {
                if (surface != nullptr) {
                    surface->requestPainted();
                }
            }
            frameDeadline_->start(kFrameWaitMs);
        }
        if (debug_) {
            std::fprintf(stderr, "vshot-pin: %lld move(s) applied and rendered in %lld ms\n",
                         static_cast<long long>(moved.size()),
                         static_cast<long long>(clock.elapsed()));
            std::fflush(stderr);
        }
    }

    QJsonObject dispatch(const QJsonObject &request)
    {
        const QString command = request.value(QStringLiteral("cmd")).toString();
        if (command == QStringLiteral("add")) {
            return addPin(request);
        }
        if (command == QStringLiteral("add-clipboard")) {
            return addClipboardPin(request);
        }
        if (command == QStringLiteral("move")) {
            // `readRequest` handles moves itself: it applies them and answers
            // once the batch has been rendered.
            return error(QStringLiteral("pin move must be answered by the reader"));
        }
        if (command == QStringLiteral("save")) {
            return savePin(request);
        }
        if (command == QStringLiteral("toggle")) {
            setVisible(!allVisible_);
            return okReply();
        }
        if (command == QStringLiteral("show")) {
            setVisible(true);
            return okReply();
        }
        if (command == QStringLiteral("hide")) {
            setVisible(false);
            return okReply();
        }
        if (command == QStringLiteral("close")) {
            const QVector<Pin *> pins = pins_;
            pins_.clear();
            byId_.clear();
            editingPinId_ = 0;
            // An open editor is annotating a pin that no longer exists; its
            // frame must go with the pin rather than stay over the desktop.
            notifyActive();
            destroySurfaces();
            for (Pin *pin : pins) {
                delete pin;
            }
            // Nothing is pinned any more, so the daemon has no reason to live.
            armIdleQuit();
            return okReply();
        }
        if (command == QStringLiteral("quit")) {
            return okReply();
        }
        if (command == QStringLiteral("list")) {
            QJsonObject reply = okReply();
            reply.insert(QStringLiteral("count"), static_cast<qint64>(pins_.size()));
            reply.insert(QStringLiteral("visible"), allVisible_);
            return reply;
        }
        return error(QStringLiteral("unknown pin command `%1`").arg(command));
    }

    QJsonObject addPin(const QJsonObject &request)
    {
        const QString path = request.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) {
            return error(QStringLiteral("pin add requires a `path`"));
        }
        const QImage image(path);
        if (image.isNull()) {
            return error(QStringLiteral("cannot load pin image `%1`").arg(path));
        }
        return addImage(image, path, path, QByteArray(), screenFromRequest(request), request);
    }

    // Pins whatever the clipboard holds. Resolution order: a color that came
    // with the structured `application/x-color` payload, embedded image data
    // (screenshots, "copy image"), image files referenced by a copied file
    // (URI list), a single plain-text local path, a color written out as text,
    // then clipboard text rendered as a card (HTML, markdown, code, or plain).
    QJsonObject addClipboardPin(const QJsonObject &request)
    {
        QScreen *target = screenFromRequest(request);
        const ClipboardPayload clipboard = readClipboard();
        if (!clipboard.installed) {
            return error(QStringLiteral(
                "`wl-paste` was not found, so the clipboard cannot be read; it comes from the \
wl-clipboard package"));
        }
        if (!clipboard.offered) {
            return error(QStringLiteral("the clipboard is empty"));
        }
        // A structured color outranks the image, the one case where the two
        // disagree: color pickers commonly put a one-pixel image of the color
        // on the clipboard beside it, and pinning that pixel alone would show
        // a single dot. The card carries the same color and more.
        if (QColor color; colorFromX11Payload(clipboard.color, &color)) {
            return addColorPin(color, target, request);
        }
        if (!clipboard.image.isEmpty()) {
            const QImage image = QImage::fromData(clipboard.image);
            if (!image.isNull()) {
                // Raw image data carries no path, so only its own chunks can
                // name a source; the record file cannot be matched. The bytes
                // go along because they are the only place a 1x declaration
                // (96 DPI) survives -- the decoded image cannot show it.
                return addImage(image, QStringLiteral("clipboard"), QString(), clipboard.image,
                                target, request);
            }
        }
        for (const QUrl &url : uriListUrls(clipboard.uriList)) {
            if (!url.isLocalFile()) {
                continue;
            }
            const QString path = url.toLocalFile();
            const QImage fileImage(path);
            if (!fileImage.isNull()) {
                return addImage(fileImage, path, path, QByteArray(), target, request);
            }
        }
        const QString text = clipboard.text.trimmed();
        if (!text.isEmpty() && !text.contains(QLatin1Char('\n')) && QFileInfo::exists(text)) {
            const QImage pathImage(text);
            if (!pathImage.isNull()) {
                return addImage(pathImage, text, text, QByteArray(), target, request);
            }
        }
        // Text that is nothing but a color is a copied color rather than a
        // snippet: the color card shows it and every format it converts to.
        // Checked after the file above, so a copied path never looks like one.
        if (QColor color; colorFromLiteral(text, &color)) {
            return addColorPin(color, target, request);
        }
        // A clipboard that offers only HTML has no plain text to fall back on,
        // and the card renderer can draw the markup itself.
        const QString card =
            text.isEmpty() ? QString::fromUtf8(clipboard.html).trimmed() : text;
        if (!card.isEmpty()) {
            // Cards are rasterized for the density the pin will use, so both
            // the target output and a stated density have to be resolved
            // before rendering; otherwise text would be resampled.
            QScreen *screen = target != nullptr ? target : fallbackScreen();
            if (screen == nullptr) {
                return error(QStringLiteral("no screen is available to pin onto"));
            }
            const int density =
                resolveDensity(request, screen, QImage(), QString(), QByteArray()).value;
            // The renderer reads the payload to tell HTML from plain text, so
            // what the clipboard offered is handed over as it was.
            RawMimeData mime;
            if (!clipboard.text.isEmpty()) {
                mime.set(QStringLiteral("text/plain"), clipboard.text.toUtf8());
            }
            if (!clipboard.html.isEmpty()) {
                mime.set(QStringLiteral("text/html"), clipboard.html);
            }
            const QImage rendered = renderTextCard(&mime, card, density);
            if (!rendered.isNull()) {
                return addImage(rendered, QStringLiteral("clipboard text"), QString(), QByteArray(),
                                target, request);
            }
        }
        return error(QStringLiteral("the clipboard contains no pinnable image or text"));
    }

    // Renders a copied color as a card and pins it: the color as a swatch
    // beside its own value in every format the clipboard could want back
    // (hex, RGB, HSL, HSV, CMYK). Cards are rasterized at the density the pin
    // will use, so the values stay sharp on a HiDPI output -- the same rule
    // the text cards follow.
    QJsonObject addColorPin(const QColor &color, QScreen *target, const QJsonObject &request)
    {
        QScreen *screen = target != nullptr ? target : fallbackScreen();
        if (screen == nullptr) {
            return error(QStringLiteral("no screen is available to pin onto"));
        }
        const int density =
            resolveDensity(request, screen, QImage(), QString(), QByteArray()).value;
        const QImage rendered = renderColorCard(color, density);
        if (rendered.isNull()) {
            return error(QStringLiteral("could not render the clipboard color"));
        }
        // The rows the card was drawn from: a right-click on the finished pin
        // turns them into the menu that puts one format back on the clipboard.
        return addImage(rendered, QStringLiteral("clipboard color"), QString(), QByteArray(), target,
                        request, colorCardRows(color));
    }

    // `sourcePath` is where the pixels came from, empty for data with no file
    // behind it: it is what lets a recorded capture scale be matched.
    // `sourceBytes` is that file's PNG when the caller read it already, or a
    // clipboard payload: it is what the declared density is read from, a chunk
    // the decoded image cannot show once the pixels have been unpacked.
    QJsonObject addImage(const QImage &image, const QString &label, const QString &sourcePath,
                         const QByteArray &sourceBytes, QScreen *requested,
                         const QJsonObject &request,
                         const QVector<ColorRow> &colorRows = QVector<ColorRow>())
    {
        if (image.isNull()) {
            armIdleQuit();
            return error(QStringLiteral("cannot pin an empty image"));
        }
        QScreen *screen = requested != nullptr ? requested : fallbackScreen();
        if (screen == nullptr) {
            armIdleQuit();
            return error(QStringLiteral("no screen is available to pin onto"));
        }
        auto *pin = new Pin;
        pin->id = nextId_++;
        pin->image = image;
        pin->label = label;
        pin->sourcePath = sourcePath;
        pin->colorRows = colorRows;
        // The HDR half, when the request carries one: a private file the CLI
        // wrote, which is gone by the time this reply lands.  Taken below, once
        // the helper is known to be up, since the copy lives in its directory.
        const QString hdrSource = request.value(QStringLiteral("hdr")).toString();
        const PinDensity density = resolveDensity(request, screen, image, sourcePath, sourceBytes);
        pin->density = density.value;
        // Why a pin came out the size it did is the first question when one
        // looks wrong, and only the daemon can answer it: the CLI sends the
        // request, not the decision.
        if (debug_) {
            qWarning("pin %llu: %dx%d px -> density %d from %s; screen %s at %.2g device "
                     "pixels per logical pixel",
                     static_cast<unsigned long long>(pin->id), image.width(), image.height(),
                     pin->density, density.source, qPrintable(screen->name()),
                     screen->devicePixelRatio());
        }
        // Natural size: one image pixel per logical pixel of an output with
        // the same density, so a 4K capture takes the room it did on the 4K
        // output even when it lands on a 1080p one.
        pin->scale = 1.0 / pin->density;
        // Whatever the density, the opening width must fit the output (never
        // upscaling past the natural one), so a pin arrives readable rather
        // than running off the sides; the wheel zooms from there. The height
        // is deliberately not fitted: a stitched long capture is legitimately
        // taller than any screen, and shrinking it until it fits would
        // override the density the size came from and render the whole thing
        // unreadably small.
        const QRect bounds = screen->geometry();
        if (!bounds.isEmpty()) {
            const double fit =
                static_cast<double>(bounds.width()) / pin->image.width();
            pin->scale = std::min(pin->scale, fit);
        }

        // A capture that came from a place on the desktop goes back to exactly
        // that place, so pinning a window over itself needs no dragging.  It is
        // drawn like any other pin -- rim, shadow and all, as the config says:
        // the rim is what tells the user the thing sitting on the desktop is a
        // pin and not the window.  Anything else -- a file, a clipboard image, a
        // synthetic card -- has no place of its own: it lands on the output the
        // user is looking at, one cascade step apart from the pins already
        // there so repeated pins stay distinguishable, and vertically centred
        // while it fits; an image taller than the output opens at its top edge,
        // so a long capture starts at its beginning instead of showing its
        // middle.
        QPoint where;
        const bool putBack = pointFromRequest(request, &where);
        if (!putBack) {
            const int offset = static_cast<int>(pins_.size() % 6) * 28;
            const QSize size = pin->displaySize();
            const int top = size.height() > bounds.height()
                                ? bounds.top()
                                : bounds.top() + (bounds.height() - size.height()) / 2;
            where = QPoint(bounds.left() + (bounds.width() - size.width()) / 2, top) +
                    QPoint(offset, offset);
        }
        pin->origin = clampOrigin(*pin, where);
        if (debug_) {
            qWarning("pin %llu: %s at %d,%d (%dx%d logical)",
                     static_cast<unsigned long long>(pin->id),
                     putBack ? "put back where it was taken from" : "placed",
                     pin->origin.x(), pin->origin.y(), pin->displaySize().width(),
                     pin->displaySize().height());
        }

        // The surfaces only exist while there is something to paint: an empty
        // daemon holds no layer surface of its own.
        ensureSurfaces();
        if (surfaces_.isEmpty()) {
            delete pin;
            armIdleQuit();
            return error(QStringLiteral("could not create a layer-shell pin surface"));
        }
        // The look is taken from the file as it is now: the daemon may have been
        // up since before the user changed it.
        reloadStyle();
        // `ensureSurfaces` has made the helper known, so its directory exists and
        // the HDR half can be taken; without a helper the pin is an SDR one.
        //
        // A half the CLI offered and this side cannot read is a mismatch between
        // two builds, and it is refused rather than quietly dropped: letting it
        // through would fall the pin back to its SDR copy in silence, which is
        // the dim picture the user has no way to explain.  The check is the
        // helper's to make -- it is the side that reads the file -- and its
        // answer arrives with the stack below.
        if (!hdrSource.isEmpty()) {
            QString refused;
            pin->hdrPath = hdr_.takeImage(hdrSource, &refused);
            if (!refused.isEmpty()) {
                delete pin;
                armIdleQuit();
                return error(QStringLiteral("cannot use the HDR half of `%1`: %2")
                                 .arg(hdrSource)
                                 .arg(refused));
            }
        }
        // The helper draws this pin too -- the whole stack is its picture -- so a
        // capture without an HDR half is handed over as a PNG.  The picture is
        // what the screen shows, which is `image`: on a capture pinned from an
        // editing session that is the flattening of its marks, and the marks
        // themselves travel beside it for the next edit to open on.
        //
        // Written from the decoded pixels rather than copied from `sourcePath`,
        // even when the request named a file: the helper reads PNG alone, and a
        // pin made from a JPEG or from a rendered card has no PNG of its own at
        // all.  One encode of the pixels is the same picture in every case.
        if (pin->hdrPath.isEmpty()) {
            pin->picturePath = hdr_.takePicture(pin->image);
        }
        // The marks the capture was pinned with, when it came out of an editing
        // session that had any: they are what a second edit opens on, so the
        // pin can be annotated again on the user's own marks rather than on the
        // pixels they were flattened into.  A capture nothing was drawn on --
        // a file, the clipboard, a bare region -- carries none, and the pin
        // opens blank the way it always did.
        //
        // The picture the marks were drawn on comes with them, and is what the
        // editor is handed: `image` is the flattening, so editing from it would
        // paint every mark a second time over its own baked copy.  A request
        // that carried marks without a base cannot be edited again, so the pin
        // stays unannotated rather than opening on a picture the marks do not
        // belong to.
        const QJsonValue marks = request.value(QStringLiteral("annotations"));
        const QString basePath = request.value(QStringLiteral("base")).toString();
        if (marks.isArray() && !marks.toArray().isEmpty() && !basePath.isEmpty()) {
            const QImage base(basePath);
            if (!base.isNull()) {
                pin->original = base;
                pin->marks = pin->keepMarks(marks.toArray());
                pin->edited = true;
            }
        }
        // Appended, so it is painted last: a new pin lands in front of the pins
        // that were already there.
        pins_.push_back(pin);
        byId_.insert(pin->id, pin);
        idleQuit_->stop();
        // The stack is rendered by `readRequest` once the whole batch has been
        // read, the way a drag's positions are, so that a client pinning several
        // images at once gets one composition of the output rather than one per
        // image.
        return okReply();
    }

    // Replaces the pixels of an existing pin and/or moves it. Coordinates are
    // global logical pixels (the editor's frame of reference).  Applies the
    // change only: it does not render and does not answer -- the batch that
    // carried it does both once, in `readRequest`, and `moveReply` carries the
    // rect the pin actually landed on, after clamping, so a pipelined drag
    // repaints the stack once for several positions.
    QJsonObject movePin(const QJsonObject &request, quint64 *movedId)
    {
        bool idOk = false;
        const quint64 id = request.value(QStringLiteral("id")).toVariant().toULongLong(&idOk);
        bool xOk = false;
        bool yOk = false;
        const int x = request.value(QStringLiteral("x")).toVariant().toInt(&xOk);
        const int y = request.value(QStringLiteral("y")).toVariant().toInt(&yOk);
        if (!idOk || !xOk || !yOk) {
            return error(QStringLiteral("pin move requires `id`, `x` and `y`"));
        }
        Pin *pin = byId_.value(id, nullptr);
        if (pin == nullptr) {
            return error(QStringLiteral("pin %1 no longer exists").arg(id));
        }
        const QString path = request.value(QStringLiteral("path")).toString();
        const QString hdr = request.value(QStringLiteral("hdr")).toString();
        // Read before anything is replaced: whether this pin has been edited
        // before is what decides, below, whether the HDR file being dropped is
        // the pristine capture (keep it) or the previous edit's flattening (throw
        // it away).  `edited` is set by the block that replaces the pixels, so it
        // can no longer answer that question by the time the HDR half is handled.
        const bool wasEdited = pin->edited;
        if (!path.isEmpty()) {
            const QImage image(path);
            if (image.isNull()) {
                return error(QStringLiteral("cannot load replacement image `%1`").arg(path));
            }
            // Keep the on-screen size the user arranged, even though the new
            // pixels may have a different density.
            const QSize display = pin->displaySize();
            // The picture the edit started from becomes the pin's pristine copy
            // the first time its pixels are replaced; a later edit reuses the
            // one already held, so the base stays the capture itself rather
            // than the previous edit's flattening.
            if (!pin->edited) {
                pin->original = pin->image;
            }
            pin->image = image;
            // The marks the editor reports, relative to the image it was given.
            // They are what a later edit opens on; the flattened pixels above
            // are what the screen shows and what a save writes.  Their own
            // pixels are taken into this pin's directory as they arrive, since
            // the editor's session directory goes with the request.
            pin->marks = pin->keepMarks(request.value(QStringLiteral("annotations")).toArray());
            pin->edited = true;
            if (image.width() > 0) {
                pin->scale = std::clamp(static_cast<double>(display.width()) / image.width(),
                                        kMinScale, kMaxScale);
            }
            // The pixels were replaced, so whatever HDR half the pin had is no
            // longer theirs -- unless this same request brings its replacement
            // along, which is what an HDR pin's edit does.  The file being
            // dropped is the *pristine* one the first time, and it is kept
            // rather than removed: it is what the next edit draws on and what an
            // open editor shows behind its marks.  The file the previous edit
            // left, if any, is the one that goes.
            if (hdr.isEmpty() && !pin->hdrPath.isEmpty()) {
                if (wasEdited) {
                    QFile::remove(pin->hdrPath);
                } else {
                    pin->hdrBasePath = pin->hdrPath;
                }
                pin->hdrPath.clear();
            }
        }
        if (!hdr.isEmpty()) {
            // A half this side cannot read is left off rather than refused here:
            // the helper is the side that reads it, and its answer to the stack
            // -- which is what the move reply carries -- is the one that counts.
            // Keeping the pristine file while the edit's own flattening replaces
            // it is what lets the pin be edited again on the capture rather than
            // on the last edit's ink; only the flattening from the edit before
            // this one is discarded.
            if (!pin->hdrPath.isEmpty()) {
                if (wasEdited) {
                    QFile::remove(pin->hdrPath);
                } else {
                    pin->hdrBasePath = pin->hdrPath;
                }
            }
            pin->hdrPath = hdr_.takeImage(hdr);
        }
        // A pin that ends this request with no HDR half has nothing left for the
        // pristine one to be the base *of*: an edit that replaced the pixels from
        // the SDR editor only -- which is what a pin whose capture turned out to
        // hold no light above white sends -- takes the whole half away, base and
        // all.  Leaving it behind would keep a file alive for a pin that no
        // longer shows one.
        if (pin->hdrPath.isEmpty() && !pin->hdrBasePath.isEmpty()) {
            QFile::remove(pin->hdrBasePath);
            pin->hdrBasePath.clear();
        }
        // The helper draws this pin too, and its pixels have just been replaced.
        // The editor wrote the flattened result as a PNG of its own, so the
        // daemon takes that file rather than re-encoding the image it just read
        // from it.  `pictureBasePath` is left alone: while an edit is open the
        // stack shows the pristine picture the marks belong to -- the editor
        // draws every mark itself, and a flattened one underneath would put a
        // baked copy of each behind the live one -- and it is the file that
        // `picturePath` takes over from once the edit ends.
        if (!path.isEmpty() && pin->hdrPath.isEmpty()) {
            const QString written = hdr_.takePictureFile(path);
            if (!written.isEmpty()) {
                QFile::remove(pin->picturePath);
                pin->picturePath = written;
            }
        }
        pin->origin = clampOrigin(*pin, QPoint(x, y));
        if (movedId != nullptr) {
            *movedId = id;
        }
        return okReply();
    }

    // Where the pin ended up, which is what a move answers with.
    static QJsonObject moveReply(const Pin &pin)
    {
        const QRect landed = pin.globalRect();
        QJsonObject reply = okReply();
        reply.insert(QStringLiteral("x"), static_cast<qint64>(landed.x()));
        reply.insert(QStringLiteral("y"), static_cast<qint64>(landed.y()));
        reply.insert(QStringLiteral("width"), static_cast<qint64>(landed.width()));
        reply.insert(QStringLiteral("height"), static_cast<qint64>(landed.height()));
        return reply;
    }

    // The picture the helper draws for a pin, for as long as this pin is the one
    // an edit is open on: the pristine pixels the editor's marks belong to.
    //
    // The helper draws every pin, so this is the file the stack names for it
    // while `editingPinId_` is its id.  It is written when the edit opens and
    // dropped when it ends, exactly as `hdrBasePath` is and for the same reason:
    // the editor draws every mark itself, and a helper painting the flattened
    // picture underneath would put a duplicate of each mark on screen -- one that
    // does not move when the mark is dragged and does not go when it is deleted.
    //
    // A capture with an HDR half needs nothing written: its pristine pixels are
    // already a file, and `editHdrBase` is what the stack names for it.
    void openEditPicture(Pin *pin)
    {
        if (pin == nullptr || !pin->hdrPath.isEmpty()) {
            return;
        }
        closeEditPicture(pin);
        pin->pictureBasePath = hdr_.takePicture(pin->editBase());
    }

    // The edit is over: the pin's flattened pixels are the ones to draw again, so
    // the pristine PNG has no reader left.  Called on every path an edit can end
    // by -- the apply child exiting, and the pin going away under an open editor.
    void closeEditPicture(Pin *pin)
    {
        if (pin == nullptr || pin->pictureBasePath.isEmpty()) {
            return;
        }
        QFile::remove(pin->pictureBasePath);
        pin->pictureBasePath.clear();
    }

    // One edit round: export the pin's pixels, describe the pin-edit session,
    // and run `vshot pin --apply` in the background. That process shows the
    // annotation editor, renders the result in Rust, and sends `move` back to
    // this daemon. `textMode` opens the editor on the pin's recognized text
    // instead of on its marks; the Space key asks for the ordinary editor, the
    // menu's `Recognize text…` row for the text one.
    void startEdit(Pin *pin, bool textMode)
    {
        if (editingPinId_ != 0) {
            return; // one edit session at a time
        }
        // The editor draws its own overlay above the pins but leaves the image
        // itself to the pin stack, so the pin being edited has to be the front
        // one or an overlapping pin would cover what is being annotated.
        bringToFront(pin);
        const QRect globalRect = pin->globalRect();
        QScreen *screen = QGuiApplication::screenAt(globalRect.center());
        if (screen == nullptr) {
            screen = fallbackScreen();
        }
        if (screen == nullptr) {
            return;
        }
        // The directory must outlive the detached child; heap-allocate it and
        // hand ownership to the edit session record below.
        auto *directory = new QTemporaryDir(QDir::tempPath() +
                                            QStringLiteral("/vshot-pin-edit-XXXXXX"));
        directory->setAutoRemove(true);
        if (!directory->isValid()) {
            delete directory;
            return;
        }
        const QString imagePath = directory->filePath(QStringLiteral("pin.png"));
        // What the editor draws on: the pin's own pixels the first time, and the
        // pristine capture the marks were placed on every time after.  Editing
        // the flattened result would put new marks on top of old ink with no way
        // back to either.
        const QImage base = pin->editBase();
        if (!base.save(imagePath, "PNG")) {
            delete directory;
            return;
        }

        // Field layout must match the Rust-written region session: the rect
        // fields sit directly on the output object (not nested). `surface`
        // repeats the pin rect here; the Rust editor widens it to the screen.
        QJsonObject output;
        output.insert(QStringLiteral("id"), 0);
        output.insert(QStringLiteral("name"), screen->name());
        output.insert(QStringLiteral("x"), static_cast<qint64>(globalRect.x()));
        output.insert(QStringLiteral("y"), static_cast<qint64>(globalRect.y()));
        output.insert(QStringLiteral("width"), static_cast<qint64>(globalRect.width()));
        output.insert(QStringLiteral("height"), static_cast<qint64>(globalRect.height()));
        QJsonObject surface;
        surface.insert(QStringLiteral("x"), static_cast<qint64>(globalRect.x()));
        surface.insert(QStringLiteral("y"), static_cast<qint64>(globalRect.y()));
        surface.insert(QStringLiteral("width"), static_cast<qint64>(globalRect.width()));
        surface.insert(QStringLiteral("height"), static_cast<qint64>(globalRect.height()));
        output.insert(QStringLiteral("surface"), surface);
        output.insert(QStringLiteral("scale"), 1);
        output.insert(QStringLiteral("pixel_width"),
                      static_cast<qint64>(base.width()));
        output.insert(QStringLiteral("pixel_height"),
                      static_cast<qint64>(base.height()));
        output.insert(QStringLiteral("path"), imagePath);
        // An HDR pin carries a second file, and the editor has to put its marks
        // on that half too or the pin would drop back to SDR the moment it is
        // annotated.  The path stays valid for the whole edit: the file lives in
        // the helper's directory, which the daemon owns.
        //
        // The *pristine* half, not the one the last edit flattened: the editor
        // composites the marks it was handed onto this file, so handing it a
        // picture that already carries them would draw every mark a second time.
        const QString &hdrBase = pin->editHdrBase();
        if (!hdrBase.isEmpty()) {
            output.insert(QStringLiteral("hdr"), hdrBase);
        }

        QJsonObject bounds;
        bounds.insert(QStringLiteral("x"), static_cast<qint64>(globalRect.x()));
        bounds.insert(QStringLiteral("y"), static_cast<qint64>(globalRect.y()));
        bounds.insert(QStringLiteral("width"), static_cast<qint64>(globalRect.width()));
        bounds.insert(QStringLiteral("height"), static_cast<qint64>(globalRect.height()));

        QJsonObject session;
        session.insert(QStringLiteral("version"), 1);
        session.insert(QStringLiteral("mode"), QStringLiteral("pin-edit"));
        // Which part of the editor to open on. Absent for the Space-key edit,
        // which opens the ordinary annotation editor; only the menu's
        // `Recognize text…` row asks for the text mode.
        if (textMode) {
            session.insert(QStringLiteral("action"), QStringLiteral("text"));
        }
        session.insert(QStringLiteral("bounds"), bounds);
        session.insert(QStringLiteral("id"), static_cast<qint64>(pin->id));
        // How wide this pin's border is drawn, so the editor can treat the rim
        // as part of the pin: the stroke is centred on the image's edge and
        // reaches half this far outside it, and a drag that starts there should
        // move the pin rather than read as a click on the bare canvas.
        session.insert(QStringLiteral("border_width"), static_cast<qint64>(style_.borderWidth));
        // The editor drives the real pin window while editing (moving it with
        // the pin's own code path instead of rendering a second copy of the
        // image), which it does over this daemon socket.
        session.insert(QStringLiteral("socket"), socketPath_);
        // The marks the last edit left, so the editor opens on them and they
        // stay editable.  Absent on a pin that has never been annotated, which
        // is what makes the editor open blank for a first edit.
        if (!pin->marks.isEmpty()) {
            session.insert(QStringLiteral("annotations"), pin->marks);
        }
        session.insert(QStringLiteral("outputs"), QJsonArray{output});

        // The desktop, as the layout places every output.  The editor's
        // keyboard cursor walks a pointer of its own and asks the CLI to move
        // the real one there, and a pointer position is expressed in this
        // space -- the CLI subtracts this origin and scales to this size.  The
        // session's own `bounds` is the pin, which is not the screen, so the
        // desktop has to travel separately or a warp on a multi-monitor layout
        // would land at a fraction of where it belongs.
        QRect desktop;
        for (QScreen *each : QGuiApplication::screens()) {
            if (each == nullptr) {
                continue;
            }
            desktop = desktop.isNull() ? each->geometry() : desktop.united(each->geometry());
        }
        if (!desktop.isNull()) {
            QJsonObject layout;
            layout.insert(QStringLiteral("x"), static_cast<qint64>(desktop.x()));
            layout.insert(QStringLiteral("y"), static_cast<qint64>(desktop.y()));
            layout.insert(QStringLiteral("width"), static_cast<qint64>(desktop.width()));
            layout.insert(QStringLiteral("height"), static_cast<qint64>(desktop.height()));
            session.insert(QStringLiteral("desktop"), layout);
        }

        const QString sessionPath = directory->filePath(QStringLiteral("session.json"));
        {
            QFile file(sessionPath);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                delete directory;
                return;
            }
            file.write(QJsonDocument(session).toJson(QJsonDocument::Compact));
        }

        editingPinId_ = pin->id;
        // The editor opens on a pin that has just been brought to the front and
        // is about to take the keyboard, so it starts live: the frame is drawn
        // from the first paint rather than after the surface's first report.
        // Nothing has to be told -- the editor's connection is not up yet, and
        // the answer is written to it the moment it is.

        QString cliPath = QString::fromLocal8Bit(qgetenv("VSHOT_BIN"));
        if (cliPath.isEmpty()) {
            char buffer[4096];
            const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
            if (length > 0) {
                buffer[length] = '\0';
                const QDir helperDir = QFileInfo(QString::fromLocal8Bit(buffer)).dir();
                // The helper lives next to vshot (installed layout), in
                // build-qt/ next to the repo root, or in a cargo target/
                // layout; cover all of them plus the same two levels up.
                for (const QString &candidate :
                     {helperDir.filePath(QStringLiteral("vshot")),
                      helperDir.filePath(QStringLiteral("../vshot")),
                      helperDir.filePath(QStringLiteral("../../vshot")),
                      helperDir.filePath(QStringLiteral("../target/release/vshot")),
                      helperDir.filePath(QStringLiteral("../../target/release/vshot")),
                      helperDir.filePath(QStringLiteral("../../../target/release/vshot"))}) {
                    if (QFileInfo::exists(candidate)) {
                        cliPath = QFileInfo(candidate).absoluteFilePath();
                        break;
                    }
                }
            }
        }
        if (cliPath.isEmpty()) {
            // Detached daemons have stderr discarded, but log anyway for
            // attached runs; a silent return here looks like "Space does
            // nothing" to the user.
            std::fprintf(stderr, "vshot-qt-ui: cannot locate the vshot CLI for pin editing; \
                                  set VSHOT_BIN\n");
            std::fflush(stderr);
            editingPinId_ = 0;
            delete directory;
            return;
        }
        // Run the apply child detached (no inherited pipes), but keep a
        // QProcess handle only as a watcher so we can drop the editing flag
        // when it exits; the temp dir dies right after.
        QProcess *watcher = new QProcess(this);
        // The id rather than the pin: a pin the user closed while the editor was
        // open is deleted by the time this fires, and a pointer to it would be
        // dangling.  Clearing the flag on an id that is no longer editing is
        // harmless, and the pin being gone already cleared it.
        const quint64 editing = pin->id;
        connect(watcher, &QProcess::finished, watcher, [this, watcher, directory, editing] {
            if (editingPinId_ == editing) {
                editingPinId_ = 0;
            }
            // The editor has drawn its marks into the pin's pixels by now, so
            // the surface goes back to painting the pin itself rather than the
            // picture the marks were placed on -- and the helper back to the file
            // that holds those pixels rather than the pristine one.  The pristine
            // PNG has no reader left either way, whether or not this was the edit
            // that was still open.
            closeEditPicture(byId_.value(editing, nullptr));
            syncAll();
            notifyActive();
            delete directory;
            watcher->deleteLater();
        });
        watcher->setProgram(cliPath);
        watcher->setArguments({QStringLiteral("pin"), QStringLiteral("--apply"), sessionPath});
        watcher->setStandardInputFile(QProcess::nullDevice());
        watcher->start();
        // The editor draws the marks live from here on, so the stack under it
        // stops drawing the pin's flattened pixels and shows the picture they
        // were placed on instead -- for the helper as much as for the Qt surface.
        // Done after the child is started so a failure to start it does not leave
        // the pin showing its base with nobody drawing the marks.
        openEditPicture(pin);
        syncAll();
    }

    // What the save dialog opens with: the file the pin came from, so a re-save
    // lands beside the original, and otherwise a timestamped `vshot-<date>.png`
    // in the same shape the CLI writes.
    static QString suggestedSaveName(const Pin &pin)
    {
        if (!pin.sourcePath.isEmpty()) {
            const QString name = QFileInfo(pin.sourcePath).fileName();
            if (!name.isEmpty()) {
                return name;
            }
        }
        return QStringLiteral("vshot-%1.png")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    }

    // The helper executable, i.e. this program. It is needed to run the save
    // dialog: that mode cannot live in this process, because this one is a
    // layer-shell client and a layer surface cannot parent a popup.
    QString helperPath() const
    {
        const QString override = QString::fromLocal8Bit(qgetenv("VSHOT_QT_HELPER"));
        if (!override.isEmpty()) {
            return override;
        }
        char buffer[4096];
        const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (length <= 0) {
            return QString();
        }
        buffer[length] = '\0';
        return QString::fromLocal8Bit(buffer);
    }

    // Saves one pin's pixels to a file the user picks. The dialog runs in a
    // process of its own -- this daemon has an event loop of its own to keep,
    // and the answer comes back over the pipe -- but it is a layer surface just
    // like the pins are, which is what puts it above them instead of behind
    // them.
    QJsonObject savePin(const QJsonObject &request)
    {
        Pin *pin = byId_.value(static_cast<quint64>(request.value(QStringLiteral("id")).toDouble()),
                               nullptr);
        if (pin == nullptr) {
            return error(QStringLiteral("save names a pin that is not pinned"));
        }
        const QString suggested = request.value(QStringLiteral("suggested")).toString();
        const QString helper = helperPath();
        if (helper.isEmpty()) {
            return error(QStringLiteral("cannot locate vshot-qt-ui for the save dialog; set "
                                        "VSHOT_QT_HELPER"));
        }
        // The dialog opens on the output the pin is on, so it lands in front of
        // the user rather than on whichever screen the compositor favours.
        QScreen *pinScreen = QGuiApplication::screenAt(pin->globalRect().center());
        const QString outputName = pinScreen != nullptr ? pinScreen->name() : QString();
        // The dialog is modal to nothing and the daemon must keep drawing, so
        // it runs detached and its reply comes back through a signal. The pin
        // id is carried in the closure: the user may have closed the pin by the
        // time the dialog closes, and a save then has nowhere to report to.
        const quint64 id = pin->id;
        auto *dialog = new QProcess(this);
        dialog->setProgram(helper);
        dialog->setArguments({QStringLiteral("--save-dialog"), suggested, outputName});
        dialog->setStandardInputFile(QProcess::nullDevice());
        connect(dialog, &QProcess::finished, this, [this, dialog, id](int code, QProcess::ExitStatus) {
            const QByteArray out = dialog->readAllStandardOutput();
            dialog->deleteLater();
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(out.trimmed(), &parseError);
            if (code != 0 || parseError.error != QJsonParseError::NoError || !document.isObject()
                || !document.object().value(QStringLiteral("ok")).toBool()) {
                return; // cancelled, or the dialog could not run at all
            }
            const QString path = document.object().value(QStringLiteral("path")).toString();
            Pin *target = byId_.value(id, nullptr);
            if (target == nullptr || path.isEmpty()) {
                return;
            }
            const bool written = target->image.save(path, "PNG");
            if (!written || debug_) {
                qWarning("pin %llu: %s `%s`", static_cast<unsigned long long>(id),
                         written ? "saved" : "could not save", qPrintable(path));
            }
            // The badge is a corner label on the image, so it names the file
            // rather than spelling out where it went: a full path would be
            // wider than most pins and get clipped to something unreadable.
            announce(id, written ? uiTr("Saved %1").arg(QFileInfo(path).fileName())
                                 : uiTr("Could not save the image"));
        });
        // A dialog that never starts (the helper went missing between the two
        // halves of the round trip) still has to say so, or `Save as…` looks
        // like it did nothing at all. Only `FailedToStart` is handled: it is
        // the one error that comes without a `finished` afterwards, so the
        // two cannot both report the same failure.
        connect(dialog, &QProcess::errorOccurred, this,
                [this, dialog, id](QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart) {
                        return;
                    }
                    qWarning("pin %llu: the save dialog could not be started",
                             static_cast<unsigned long long>(id));
                    announce(id, uiTr("Could not save the image"));
                    dialog->deleteLater();
                });
        dialog->start();
        return okReply();
    }

    // Puts `text` on `id`'s corner in every surface that shows that pin. A save
    // runs in another process, so this is how its outcome reaches the user long
    // after the menu that started it has closed.
    void announce(quint64 id, const QString &text)
    {
        for (const QPointer<PinSurface> &surface : surfaces_) {
            if (surface != nullptr) {
                surface->showMessage(id, text);
            }
        }
    }

    // Global logical top-left that keeps the image reachable: at least
    // kGrabMargin of it must stay on the output it overlaps most. When the
    // image is on no output at all, the nearest one is used, so a drag that
    // overshoots lands back at that output's edge instead of being lost.
    QPoint clampOrigin(const Pin &pin, QPoint candidate) const
    {
        const QSize size = pin.displaySize();
        const QRect rect(candidate, size);
        const QList<QScreen *> screens = QGuiApplication::screens();
        QRect best;
        qint64 bestScore = std::numeric_limits<qint64>::min();
        for (QScreen *screen : screens) {
            if (screen == nullptr) {
                continue;
            }
            const QRect bounds = screen->geometry();
            const QRect visible = bounds.intersected(rect);
            const qint64 area = static_cast<qint64>(std::max(0, visible.width())) *
                                std::max(0, visible.height());
            // Overlap decides first; with no overlap anywhere, the smallest
            // gap wins. Any overlap (positive) beats any gap (negative).
            const qint64 score = area > 0 ? area : -chebyshevGap(bounds, rect);
            if (score > bestScore) {
                bestScore = score;
                best = bounds;
            }
        }
        if (best.isNull()) {
            return candidate;
        }
        const int minX = std::min(best.left() - size.width() + kGrabMargin,
                                  best.right() + 1 - kGrabMargin);
        const int maxX = std::max(best.left() - size.width() + kGrabMargin,
                                  best.right() + 1 - kGrabMargin);
        const int minY = std::min(best.top() - size.height() + kGrabMargin,
                                  best.bottom() + 1 - kGrabMargin);
        const int maxY = std::max(best.top() - size.height() + kGrabMargin,
                                  best.bottom() + 1 - kGrabMargin);
        return QPoint(std::clamp(candidate.x(), minX, maxX),
                      std::clamp(candidate.y(), minY, maxY));
    }

    // How far apart two rects are along their worst axis; 0 when they touch.
    static int chebyshevGap(const QRect &a, const QRect &b)
    {
        const int dx = std::max(0, std::max(a.left() - b.right(), b.left() - a.right()));
        const int dy = std::max(0, std::max(a.top() - b.bottom(), b.top() - a.bottom()));
        return std::max(dx, dy);
    }

    // How the pins are drawn, read from the config file.  An absent colour
    // means "the built-in one", the same resolution the dialog's rim does.
    //
    // This is re-read rather than cached for the daemon's whole life: the
    // daemon stays up for as long as anything is pinned, so a user who edits
    // the file (or saves in the settings window) gets the look they just asked
    // for on the next pin they add.  Every surface is handed the same style, so
    // two outputs can never disagree about what a pin looks like.
    void reloadStyle()
    {
        const PinPreferences preferences = loadPinPreferences();
        style_.radius = preferences.radius;
        style_.shadow = preferences.shadow;
        style_.borderWidth = preferences.borderWidth;
        style_.borderColor = resolvePinBorderColor(preferences, false);
        style_.activeBorderColor = resolvePinBorderColor(preferences, true);
        for (const QPointer<PinSurface> &surface : surfaces_) {
            if (surface != nullptr) {
                surface->setStyle(style_);
            }
        }
        // The helper draws the HDR half of a pin, rim included, so a new look is
        // something it has to be told about as well.
        syncHdr();
    }

    // Creates the stack's surface on one output.
    void addSurface(QScreen *screen)
    {
        if (screen == nullptr || surfaces_.contains(screen)) {
            return;
        }
        auto *surface = new PinSurface(screen);
        surface->setStyle(style_);
        // Every gesture names the pin it is about: the surface paints and
        // hit-tests the whole stack, so the daemon looks the pin up by id.
        surface->setPickCallback([this](quint64 id) {
            bringToFront(byId_.value(id, nullptr));
        });
        // The surface is the only place that knows whether it still holds the
        // keyboard and which pin the pointer last picked, which together decide
        // whose rim is the live one.  Every pin's rim is drawn by the helper, so
        // that answer has to travel.
        surface->setActiveCallback([this](quint64 id) {
            if (id == hdrActiveId_) {
                return;
            }
            hdrActiveId_ = id;
            syncHdr();
            // The editor's frame follows the same answer: it is Qt chrome drawn
            // above every pin surface, so it may only be up while its own pin is
            // the live one.
            notifyActive();
        });
        // A frame this surface put on the screen.  A reply the daemon is
        // holding until the change it carries is visible -- the pin editor's
        // handoff -- is released by these.
        surface->setPaintedCallback([this] { framePresented(); });
        surface->setDragCallback([this](quint64 id, QPoint topLeft) {
            Pin *pin = byId_.value(id, nullptr);
            if (pin == nullptr) {
                return;
            }
            pin->origin = clampOrigin(*pin, topLeft);
            syncAll();
        });
        surface->setZoomCallback([this](quint64 id, double factor) {
            zoomPin(byId_.value(id, nullptr), factor);
        });
        surface->setCloseCallback([this](quint64 id) {
            if (Pin *pin = byId_.value(id, nullptr)) {
                removePin(pin);
            }
        });
        surface->setEditCallback([this](quint64 id) {
            if (Pin *pin = byId_.value(id, nullptr)) {
                startEdit(pin, false);
            }
        });
        // A menu pick is a clipboard write and nothing else: the surface
        // collects the gesture and the format, the daemon owns the clipboard.
        surface->setCopyCallback([this](quint64 id, const QString &value) {
            const bool copied = runWlCopy(value);
            if (debug_ || !copied) {
                qWarning("pin %llu: %s `%s`", static_cast<unsigned long long>(id),
                         copied ? "copied" : "could not copy", qPrintable(value));
            }
            return copied;
        });
        // `Copy image` puts the pin's own pixels on the clipboard -- the
        // flattened result, marks and all, which is what the screen shows and
        // what a save writes.  The surface holds a copy of those pixels for
        // painting, but the pin is the only place the daemon's own is kept, so
        // the encode happens here.
        surface->setCopyImageCallback([this](quint64 id) {
            const Pin *pin = byId_.value(id, nullptr);
            if (pin == nullptr || pin->image.isNull()) {
                return false;
            }
            const bool copied = runWlCopyImage(pin->image);
            if (debug_ || !copied) {
                qWarning("pin %llu: %s its image (%dx%d)", static_cast<unsigned long long>(id),
                         copied ? "copied" : "could not copy", pin->image.width(),
                         pin->image.height());
            }
            return copied;
        });
        // `Save as…` runs the dialog and writes the file; the surface only
        // reports the outcome, through the same badge a copy uses.
        surface->setSaveCallback([this](quint64 id) {
            QJsonObject request;
            request.insert(QStringLiteral("id"), static_cast<qint64>(id));
            if (const Pin *pin = byId_.value(id, nullptr)) {
                request.insert(QStringLiteral("suggested"), suggestedSaveName(*pin));
            }
            savePin(request);
        });
        // `Recognize text…` reuses the Space-key edit path, only asking the
        // editor to open on the pin's text instead of on its marks.
        surface->setRecognizeCallback([this](quint64 id) {
            if (Pin *pin = byId_.value(id, nullptr)) {
                startEdit(pin, true);
            }
        });
        // `Reset zoom` puts a pin back at the size it arrived at, whatever the
        // wheel has done to it since; the surface reports the new factor
        // through the same badge the wheel uses.
        surface->setResetZoomCallback([this](quint64 id) {
            resetZoomPin(byId_.value(id, nullptr));
        });
        if (!surface->showLayerSurface()) {
            delete surface;
            return;
        }
        surfaces_.insert(screen, surface);
        QObject::connect(surface, &QObject::destroyed, this, [this, screen] {
            surfaces_.remove(screen);
        });
    }

    // Creates one surface per output, for the first pin that arrives: an empty
    // daemon maps nothing of its own.
    void ensureSurfaces()
    {
        if (!surfaces_.isEmpty()) {
            return;
        }
        // The helper's surfaces and ours share a layer, and the compositor
        // stacks a layer in map order with no restack request: the helper has to
        // be on the screen before the first surface of ours, whatever kind of
        // pin this is.  A helper that does not come up leaves every pin SDR.
        hdr_.ensureMapped();
        for (QScreen *screen : QGuiApplication::screens()) {
            addSurface(screen);
        }
    }

    // Moves a pin to the top of the stack, so the surfaces paint it last and it
    // covers the pins it overlaps. Every pin shares one surface per output,
    // which is exactly what makes the order the daemon's to change: the
    // compositor orders the surfaces of a layer by map time and offers no
    // request to restack them, so with one surface per pin this would have to
    // be a remap — and remapping loses the keyboard focus a click just gave.
    //
    // A pin's own surface cannot do this while an edit is open on that pin: the
    // editor's layer surface holds the keyboard and covers the output, so the
    // pointer reaches the editor and never the pin surface underneath.  The
    // editor asks instead, over the pin socket, which is what
    // `{"cmd":"raise"}` is for.
    void bringToFront(Pin *pin)
    {
        if (pin == nullptr || pins_.isEmpty() || pins_.constLast() == pin) {
            return;
        }
        pins_.removeAll(pin);
        pins_.push_back(pin);
        syncAll();
    }

    // Retires one surface and breaks every connection into the daemon first:
    // the widget is destroyed asynchronously (WA_DeleteOnClose), and by then the
    // pins it references may already be gone.
    void detachSurface(PinSurface *surface)
    {
        if (surface == nullptr) {
            return;
        }
        surface->setPickCallback({});
        surface->setCloseCallback({});
        surface->setEditCallback({});
        surface->setDragCallback({});
        surface->setZoomCallback({});
        surface->setCopyCallback({});
        surface->setCopyImageCallback({});
        surface->setSaveCallback({});
        surface->setRecognizeCallback({});
        surface->setResetZoomCallback({});
        QObject::disconnect(surface, nullptr, this, nullptr);
        surface->setPinnedVisible(false);
        surface->hide();
        surface->close();
    }

    void dropSurface(QScreen *screen)
    {
        detachSurface(surfaces_.take(screen).data());
    }

    void destroySurfaces()
    {
        const QList<QPointer<PinSurface>> surfaces = surfaces_.values();
        surfaces_.clear();
        for (const QPointer<PinSurface> &surface : surfaces) {
            detachSurface(surface.data());
        }
    }

    void removePin(Pin *pin)
    {
        if (pin == nullptr) {
            return;
        }
        if (editingPinId_ == pin->id) {
            editingPinId_ = 0;
        }
        // The pin that was being annotated is gone, so the editor is annotating
        // nothing the daemon knows about and must stop drawing its frame.
        notifyActive();
        closeEditPicture(pin);
        pins_.removeAll(pin);
        if (byId_.value(pin->id, nullptr) == pin) {
            byId_.remove(pin->id);
        }
        // The halves are files of ours; the pin going away takes every one of
        // them -- the HDR half, the pristine one an edit would have started
        // from, and the two PNGs the helper is handed.
        const QStringList owned = {pin->hdrPath, pin->hdrBasePath, pin->picturePath,
                                   pin->pictureBasePath};
        for (const QString &path : owned) {
            if (!path.isEmpty()) {
                QFile::remove(path);
            }
        }
        delete pin;
        if (pins_.isEmpty()) {
            // Nothing is left to paint, so the surfaces go as well: they are the
            // daemon's own layer surfaces and have no reason to outlive the last
            // pin.
            destroySurfaces();
            // ... and the helper is told to clear, so no picture survives the
            // pin it belonged to.
            syncHdr();
            armIdleQuit();
            return;
        }
        syncAll();
    }

    // The pin an open edit session is annotating while it is still the live one,
    // 0 otherwise.  A pin is live while its surface holds the keyboard and the
    // pointer is over it -- the same answer `hdrActiveId_` already carries for
    // the helper's rim.
    //
    // 0 is not "no live pin" here so much as "no pin surface has the keyboard",
    // which is the ordinary state of an open editor: the editor's own surface is
    // the one holding it.  Only another pin taking the keyboard -- a click on
    // one -- says the edit has stopped being what the user is working on.
    quint64 liveEditingPin() const
    {
        if (editingPinId_ == 0) {
            return 0;
        }
        return hdrActiveId_ == 0 || hdrActiveId_ == editingPinId_ ? editingPinId_ : 0;
    }

    // Tells every client that asked which pin is the live one, when the answer
    // changes.
    //
    // The editor draws its frame around the image in Qt, and every other pin is
    // painted by a Wayland surface one layer below -- the compositor orders a
    // layer's surfaces by map time and offers no restack, so a frame drawn for a
    // pin that is no longer live would sit on top of every other pin on the
    // screen.
    //
    // Only on a change, and only to clients in the map: `syncAll` runs on every
    // motion event of a drag, and a line per client per event would put the
    // editor's move replies behind a growing queue of answers it has no use for.
    void notifyActive()
    {
        const quint64 live = liveEditingPin();
        const QJsonObject notice = activeNotice(live);
        for (auto it = reportedActive_.begin(); it != reportedActive_.end(); ++it) {
            if (it.value() == live || it.key() == nullptr) {
                continue;
            }
            it.value() = live;
            respond(it.key(), notice);
        }
    }

    static QJsonObject activeNotice(quint64 live)
    {
        QJsonObject notice;
        notice.insert(QStringLiteral("active"), static_cast<qint64>(live));
        return notice;
    }

    // Hands every surface the whole stack, in paint order: the daemon owns the
    // order, and a surface only needs to be told which entries changed.
    //
    // The answer is whether the helper was actually given something new to
    // compose, and it is what decides whether a caller may wait for the helper's
    // verdict: a stack the helper already has is nothing for it to answer, and
    // waiting for a reply that is never coming is a stall, not a check.
    bool syncAll()
    {
        QVector<PinSurface::Item> items;
        items.reserve(pins_.size());
        for (const Pin *pin : pins_) {
            items.append(itemFor(*pin));
        }
        for (const QPointer<PinSurface> &surface : surfaces_) {
            if (surface != nullptr) {
                // A surface created while everything is hidden never got the
                // hide command, so it would paint the stack the next pin adds.
                if (surface->isPinnedVisible() != allVisible_) {
                    surface->setPinnedVisible(allVisible_);
                }
                // Only an output the helper described can show a helper-drawn
                // pin; on any other this surface paints the image itself.
                const QScreen *screen = surface->screen();
                surface->setHdrPixels(screen != nullptr
                                      && hdr_.isHdrOutput(screen->name()));
                surface->setPins(items);
            }
        }
        return syncHdr();
    }

    // Hands the helper the whole stack: it draws every picture, every shadow
    // and every rim, in the daemon's own paint order, and the Qt surfaces leave
    // their rects transparent.  Answers whether it actually handed a stack over:
    // an unchanged stack is not sent again, and only a stack the helper was
    // handed can come back refused.
    bool syncHdr()
    {
        if (!hdr_.mapped()) {
            return false;
        }
        QJsonObject style;
        style.insert(QStringLiteral("radius"), static_cast<qint64>(style_.radius));
        if (style_.shadow.enabled && style_.shadow.size > 0 && style_.shadow.opacity > 0) {
            QJsonObject shadow;
            shadow.insert(QStringLiteral("size"), style_.shadow.size);
            shadow.insert(QStringLiteral("offset"), style_.shadow.offset);
            shadow.insert(QStringLiteral("opacity"), style_.shadow.opacity);
            style.insert(QStringLiteral("shadow"), shadow);
        } else {
            style.insert(QStringLiteral("shadow"), QJsonValue::Null);
        }
        if (style_.borderWidth > 0) {
            QJsonObject border;
            border.insert(QStringLiteral("width"), static_cast<qint64>(style_.borderWidth));
            border.insert(QStringLiteral("color"), rgbArray(style_.borderColor));
            border.insert(QStringLiteral("active"), rgbArray(style_.activeBorderColor));
            style.insert(QStringLiteral("border"), border);
        } else {
            style.insert(QStringLiteral("border"), QJsonValue::Null);
        }
        QJsonArray array;
        for (const Pin *pin : pins_) {
            // The pin being edited is drawn from its pristine picture, so the
            // editor's marks land over the image they were placed on rather
            // than over the last edit's flattening of them.  Every other pin is
            // drawn from the file that holds its current pixels -- the PQ half
            // when the capture had one, the PNG the daemon wrote when it did
            // not.
            const QString &picture =
                pin->id == editingPinId_ ? pin->editPicture() : pin->picture();
            // A pin this side could not hand over is one the helper cannot draw,
            // and it is left out rather than named with an empty path: an entry
            // with no file would make the helper refuse the whole stack, and
            // every other pin would keep the last picture it managed.  That
            // happens when there is no helper directory to write a PNG into, and
            // then `item.hdr` is false everywhere and the Qt surfaces paint the
            // stack themselves.
            if (picture.isEmpty()) {
                continue;
            }
            QJsonObject entry;
            entry.insert(QStringLiteral("id"), static_cast<qint64>(pin->id));
            entry.insert(pin->hdrPath.isEmpty() ? QStringLiteral("png") : QStringLiteral("hdr"),
                         picture);
            entry.insert(QStringLiteral("x"), pin->origin.x());
            entry.insert(QStringLiteral("y"), pin->origin.y());
            entry.insert(QStringLiteral("scale"), pin->scale);
            entry.insert(QStringLiteral("visible"), allVisible_);
            array.append(entry);
        }
        return hdr_.sync(array, style, hdrActiveId_);
    }

    // One colour as the four channels the helper reads, 0-255.  The alpha is
    // part of the colour: a rim the user set to `#00000000` has to reach the
    // helper as a transparent one, or the HDR copy of a pin would draw a black
    // rim where the SDR copy draws none.
    static QJsonArray rgbArray(const QColor &color)
    {
        return QJsonArray{color.red(), color.green(), color.blue(), color.alpha()};
    }

    // How one pinned image looks on an output. A pin may span several outputs,
    // so its global state is handed over as it is and each surface paints the
    // part that overlaps it.
    PinSurface::Item itemFor(const Pin &pin) const
    {
        PinSurface::Item item;
        item.id = pin.id;
        // The pin being edited shows the picture its marks belong to, not the
        // flattening: the editor draws every mark itself, live and editable, so
        // a surface still painting the baked copy underneath would put a
        // duplicate of each mark on screen -- one that does not move when the
        // mark is dragged and does not go when it is deleted.  Every other pin
        // shows its flattened image, which is what a pin is.
        item.image = pin.id == editingPinId_ ? pin.editBase() : pin.image;
        item.density = pin.density;
        item.scale = pin.scale;
        item.origin = pin.origin;
        item.colorRows = pin.colorRows;
        // Whether the surface leaves the picture, the shadow and the rim to the
        // helper is its own to decide -- it depends on this output -- but whether
        // there *is* a helper picture is the pin's.  It is not only the HDR pins:
        // the helper's surface is below this one, so anything painted here would
        // composite over every pin the helper draws, whatever the daemon's own
        // order said.  On an output the helper could not take, this surface
        // paints `image` -- already the pristine picture on a pin being edited,
        // so its marks are never baked in twice -- and draws its own rim, and the
        // stack is the SDR one it always was.
        item.hdr = !pin.picture().isEmpty();
        // The tag belongs to the capture, not to whichever surface is showing
        // it: an HDR pin the helper could not take is still an HDR capture, and
        // the tag is what says so -- in a muted ink, which is the visible
        // difference between the two.
        item.capturedHdr = !pin.hdrPath.isEmpty();
        return item;
    }

    // Multiplicative zoom keeps the image center in place. The surface that
    // reported the gesture shows the factor it just applied.
    void zoomPin(Pin *pin, double factor)
    {
        if (pin == nullptr) {
            return;
        }
        const double next = std::clamp(pin->scale * factor, kMinScale, kMaxScale);
        if (next == pin->scale) {
            return;
        }
        const QRect previous(pin->origin, pin->displaySize());
        const QPoint center = previous.center();
        pin->scale = next;
        const QSize resized = pin->displaySize();
        pin->origin = clampOrigin(
            *pin, center - QPoint(resized.width() / 2, resized.height() / 2));
        syncAll();
    }

    // Back to the size the pin arrived at: one image pixel per logical pixel of
    // an output with the pin's own density, which is what `addImage` set and
    // what every wheel step since has multiplied.  Same center-keeping as a
    // wheel step, so a pin that has been zoomed stays where the user put it.
    void resetZoomPin(Pin *pin)
    {
        if (pin == nullptr) {
            return;
        }
        const double next = 1.0 / std::max(1, pin->density);
        if (next == pin->scale) {
            return;
        }
        const QRect previous(pin->origin, pin->displaySize());
        const QPoint center = previous.center();
        pin->scale = next;
        const QSize resized = pin->displaySize();
        pin->origin = clampOrigin(
            *pin, center - QPoint(resized.width() / 2, resized.height() / 2));
        syncAll();
    }

    void setVisible(bool visible)
    {
        allVisible_ = visible;
        for (const QPointer<PinSurface> &surface : surfaces_) {
            if (surface != nullptr) {
                surface->setPinnedVisible(visible);
            }
        }
        syncHdr();
    }

    QLocalServer *server_;
    QString socketPath_;
    // `VSHOT_PIN_DEBUG` logs the density decision behind every pinned image,
    // the way `VSHOT_PIXEL_DEBUG` logs the detection behind a window rect.
    bool debug_ = false;
    QHash<QLocalSocket *, QByteArray> buffer_;
    // The pins, back to front: the last one is painted last, i.e. it is the one
    // on top where they overlap.
    QVector<Pin *> pins_;
    QHash<quint64, Pin *> byId_;
    // One rendering surface per output, each painting the whole stack. QPointer:
    // a surface can be dismissed by the compositor on its own (an output going
    // away), which would leave a bare pointer behind.
    QHash<QScreen *, QPointer<PinSurface>> surfaces_;
    // The look every surface is drawing, kept so a surface created later (a new
    // output, or the first pin after the daemon has been idle) starts from the
    // same style as the ones already up.
    PinSurface::Style style_;
    // The pin whose rim the helper should draw as the live one, 0 for none.
    // The pin whose rim the helper should draw as the live one, 0 for none.
    // Tracked here rather than inside the stack because it is what the pointer
    // is doing: the surface that holds the keyboard is the only one that knows,
    // and it reports every change.
    quint64 hdrActiveId_ = 0;
    // The clients that asked to be told which pin is the live one, each with the
    // answer it was last given.  Only the pin editor asks; keeping the last
    // answer here is what makes a change a change, rather than a line written to
    // every client on every sync of a drag.  Emptied with the connections it
    // describes, on disconnect.
    QHash<QLocalSocket *, quint64> reportedActive_;
    // The HDR half of the stack: the helper process that draws pinned HDR
    // images on surfaces of its own.  Started with the daemon, so it is always
    // under the Qt surfaces this daemon maps.
    PinHdr hdr_;
    quint64 nextId_ = 1;
    // The pin an edit session is open on, by id, or 0 for none.  An id rather
    // than a pointer because the surfaces ask `itemFor` what to paint on every
    // sync, and that question has to be answerable from a `const` daemon.
    quint64 editingPinId_ = 0;
    bool allVisible_ = true;
    class QTimer *idleQuit_ = nullptr;
    // Answers held until the frame that carries their change has been
    // presented, and the backstop that releases them if it never is.
    QVector<HeldReply> pendingReplies_;
    class QTimer *frameDeadline_ = nullptr;
};

} // namespace

int runPinServer(const QString &socketPath)
{
    QLocalServer::removeServer(socketPath);
    auto *server = new QLocalServer;
    if (!server->listen(socketPath)) {
        std::fprintf(stderr, "vshot-qt-ui: cannot listen on pin socket `%s`: %s\n",
                     socketPath.toUtf8().constData(),
                     server->errorString().toUtf8().constData());
        return 1;
    }
    std::fprintf(stderr, "vshot-qt-ui: pin server listening on `%s`\n",
                 socketPath.toUtf8().constData());
    std::fflush(stderr);

    PinServer controller(server, socketPath);
    QObject::connect(server, &QLocalServer::newConnection, &controller,
                     &PinServer::handleNewConnection);
    // close() also removes the listening socket file; otherwise the next
    // spawn would inherit a stale entry (removeServer covers that, but a
    // dangling socket is confusing to users).
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, server, [server, &controller] {
        controller.shutdownAll();
        server->close();
    });
    controller.watchScreens();
    // A signal-killed client can leave a mapped layer surface behind, which
    // is fatal for the compositor's frame loop, so SIGTERM/SIGINT unmap first.
    installTerminateNotifier(&controller, [] { QCoreApplication::quit(); });
    return QCoreApplication::exec();
}

} // namespace vshot

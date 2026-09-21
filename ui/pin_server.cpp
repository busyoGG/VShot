#include "pin_density.hpp"
#include "pin_server.hpp"
#include "color_card.hpp"
#include "i18n.hpp"
#include "pin_surface.hpp"
#include "text_card.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMimeData>
#include <QPointer>
#include <QProcess>
#include <QRect>
#include <QScreen>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <functional>
#include <limits>
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

void respond(QLocalSocket *socket, const QJsonObject &payload)
{
    const QByteArray encoded = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    socket->write(encoded);
    socket->write("\n", 1);
    socket->flush();
    socket->disconnectFromServer();
}

// After the last pin is gone the daemon owns no surfaces, so it exits and
// lets the next pin command spawn a fresh daemon. The grace period serves a
// concurrent add (or a reply still in flight) before quitting.
constexpr int kIdleQuitMs = 500;

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
    }

    void handleNewConnection()
    {
        while (QLocalSocket *socket = server_->nextPendingConnection()) {
            connect(socket, &QLocalSocket::readyRead, this,
                    [this, socket] { readRequest(socket); });
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
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
        editingPin_ = nullptr;
        destroySurfaces();
        for (Pin *pin : pins) {
            delete pin;
        }
    }

private:
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

    // Requests are newline-terminated single JSON objects.
    void readRequest(QLocalSocket *socket)
    {
        buffer_[socket] += socket->readAll();
        const qsizetype newline = buffer_[socket].indexOf('\n');
        if (newline < 0) {
            return; // still streaming
        }
        const QByteArray line = buffer_[socket].left(newline);
        buffer_.remove(socket);

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            respond(socket, error(QStringLiteral("invalid pin request JSON: %1")
                                      .arg(parseError.errorString())));
            return;
        }
        const QJsonObject request = document.object();
        const bool quit =
            request.value(QStringLiteral("cmd")).toString() == QStringLiteral("quit");
        const QJsonObject reply = dispatch(request);
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
            return movePin(request);
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
            editingPin_ = nullptr;
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

        // Land on the output the user is looking at, one cascade step apart
        // from the pins already there so repeated pins stay distinguishable.
        const int offset = static_cast<int>(pins_.size() % 6) * 28;
        const QSize size = pin->displaySize();
        // Vertically centred while the pin fits; an image taller than the
        // output opens at its top edge, so a long capture starts at its
        // beginning instead of showing its middle.
        const int top = size.height() > bounds.height()
                            ? bounds.top()
                            : bounds.top() + (bounds.height() - size.height()) / 2;
        pin->origin = QPoint(bounds.left() + (bounds.width() - size.width()) / 2, top) +
                      QPoint(offset, offset);
        pin->origin = clampOrigin(*pin, pin->origin);

        // The surfaces only exist while there is something to paint: an empty
        // daemon holds no layer surface of its own.
        ensureSurfaces();
        if (surfaces_.isEmpty()) {
            delete pin;
            armIdleQuit();
            return error(QStringLiteral("could not create a layer-shell pin surface"));
        }
        // Appended, so it is painted last: a new pin lands in front of the pins
        // that were already there.
        pins_.push_back(pin);
        byId_.insert(pin->id, pin);
        idleQuit_->stop();
        syncAll();
        return okReply();
    }

    // Replaces the pixels of an existing pin and/or moves it. Coordinates are
    // global logical pixels (the editor's frame of reference); the response
    // carries the rect the pin actually landed on, after clamping, so the
    // caller can follow it.
    QJsonObject movePin(const QJsonObject &request)
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
        if (!path.isEmpty()) {
            const QImage image(path);
            if (image.isNull()) {
                return error(QStringLiteral("cannot load replacement image `%1`").arg(path));
            }
            // Keep the on-screen size the user arranged, even though the new
            // pixels may have a different density.
            const QSize display = pin->displaySize();
            pin->image = image;
            if (image.width() > 0) {
                pin->scale = std::clamp(static_cast<double>(display.width()) / image.width(),
                                        kMinScale, kMaxScale);
            }
        }
        pin->origin = clampOrigin(*pin, QPoint(x, y));
        syncAll();
        const QRect landed = pin->globalRect();
        QJsonObject reply = okReply();
        reply.insert(QStringLiteral("x"), static_cast<qint64>(landed.x()));
        reply.insert(QStringLiteral("y"), static_cast<qint64>(landed.y()));
        reply.insert(QStringLiteral("width"), static_cast<qint64>(landed.width()));
        reply.insert(QStringLiteral("height"), static_cast<qint64>(landed.height()));
        return reply;
    }

    // One Space-triggered edit round: export the pin's pixels, describe the
    // pin-edit session, and run `vshot pin --apply` in the background. That
    // process shows the annotation editor, renders the result in Rust, and
    // sends `move` back to this daemon.
    void startEdit(Pin *pin)
    {
        if (editingPin_ != nullptr) {
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
        if (!pin->image.save(imagePath, "PNG")) {
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
                      static_cast<qint64>(pin->image.width()));
        output.insert(QStringLiteral("pixel_height"),
                      static_cast<qint64>(pin->image.height()));
        output.insert(QStringLiteral("path"), imagePath);

        QJsonObject bounds;
        bounds.insert(QStringLiteral("x"), static_cast<qint64>(globalRect.x()));
        bounds.insert(QStringLiteral("y"), static_cast<qint64>(globalRect.y()));
        bounds.insert(QStringLiteral("width"), static_cast<qint64>(globalRect.width()));
        bounds.insert(QStringLiteral("height"), static_cast<qint64>(globalRect.height()));

        QJsonObject session;
        session.insert(QStringLiteral("version"), 1);
        session.insert(QStringLiteral("mode"), QStringLiteral("pin-edit"));
        session.insert(QStringLiteral("bounds"), bounds);
        session.insert(QStringLiteral("id"), static_cast<qint64>(pin->id));
        // The editor drives the real pin window while editing (moving it with
        // the pin's own code path instead of rendering a second copy of the
        // image), which it does over this daemon socket.
        session.insert(QStringLiteral("socket"), socketPath_);
        session.insert(QStringLiteral("outputs"), QJsonArray{output});

        const QString sessionPath = directory->filePath(QStringLiteral("session.json"));
        {
            QFile file(sessionPath);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                delete directory;
                return;
            }
            file.write(QJsonDocument(session).toJson(QJsonDocument::Compact));
        }

        editingPin_ = pin;

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
            editingPin_ = nullptr;
            delete directory;
            return;
        }
        // Run the apply child detached (no inherited pipes), but keep a
        // QProcess handle only as a watcher so we can drop the editing flag
        // when it exits; the temp dir dies right after.
        QProcess *watcher = new QProcess(this);
        connect(watcher, &QProcess::finished, watcher, [this, watcher, directory] {
            editingPin_ = nullptr;
            delete directory;
            watcher->deleteLater();
        });
        watcher->setProgram(cliPath);
        watcher->setArguments({QStringLiteral("pin"), QStringLiteral("--apply"), sessionPath});
        watcher->setStandardInputFile(QProcess::nullDevice());
        watcher->start();
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

    // Creates the stack's surface on one output.
    void addSurface(QScreen *screen)
    {
        if (screen == nullptr || surfaces_.contains(screen)) {
            return;
        }
        auto *surface = new PinSurface(screen);
        // Every gesture names the pin it is about: the surface paints and
        // hit-tests the whole stack, so the daemon looks the pin up by id.
        surface->setPickCallback([this](quint64 id) {
            bringToFront(byId_.value(id, nullptr));
        });
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
                startEdit(pin);
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
        surface->setSaveCallback({});
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
        if (editingPin_ == pin) {
            editingPin_ = nullptr;
        }
        pins_.removeAll(pin);
        if (byId_.value(pin->id, nullptr) == pin) {
            byId_.remove(pin->id);
        }
        delete pin;
        if (pins_.isEmpty()) {
            // Nothing is left to paint, so the surfaces go as well: they are the
            // daemon's own layer surfaces and have no reason to outlive the last
            // pin.
            destroySurfaces();
            armIdleQuit();
            return;
        }
        syncAll();
    }

    // Hands every surface the whole stack, in paint order: the daemon owns the
    // order, and a surface only needs to be told which entries changed.
    void syncAll()
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
                surface->setPins(items);
            }
        }
    }

    // How one pinned image looks on an output. A pin may span several outputs,
    // so its global state is handed over as it is and each surface paints the
    // part that overlaps it.
    static PinSurface::Item itemFor(const Pin &pin)
    {
        PinSurface::Item item;
        item.id = pin.id;
        item.image = pin.image;
        item.density = pin.density;
        item.scale = pin.scale;
        item.origin = pin.origin;
        item.colorRows = pin.colorRows;
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

    void setVisible(bool visible)
    {
        allVisible_ = visible;
        for (const QPointer<PinSurface> &surface : surfaces_) {
            if (surface != nullptr) {
                surface->setPinnedVisible(visible);
            }
        }
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
    quint64 nextId_ = 1;
    Pin *editingPin_ = nullptr;
    bool allVisible_ = true;
    class QTimer *idleQuit_ = nullptr;
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

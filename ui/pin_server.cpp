#include "pin_server.hpp"
#include "pin_window.hpp"
#include "text_card.hpp"

#include <QClipboard>
#include <QCoreApplication>
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
// the CLI resolves the focused output with compositor metadata and passes the
// rect along; see `src/capture/active_output.rs`.
QScreen *fallbackScreen()
{
    return QGuiApplication::primaryScreen();
}

// The Qt screen matching the output rect the CLI reported, if any. A
// compositor whose rect does not line up with Qt's screens simply gets the
// fallback.
QScreen *screenFromRequest(const QJsonObject &request)
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

// One pinned image. Image, scale and global position live here rather than in
// a widget: a pin may span several outputs, and every surface has to render
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
    // One surface per output the daemon renders on; normally all of them.
    // QPointer: a surface can be dismissed by the compositor on its own (an
    // output going away), which would leave a bare pointer behind.
    QHash<QScreen *, QPointer<PinWindow>> surfaces;

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

// Density an image declares about itself. PNG carries it as pixels per metre
// (the `pHYs` chunk). This is only consulted for a genuine multi-pixel
// declaration: Qt reports 3780 dots per metre (96 DPI) for a PNG that declares
// nothing at all, and that default would otherwise mask the real source. A
// print resolution is not a device density either, so the value has to be a
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

// Device pixels per logical pixel of the image being pinned, so the pin takes
// the same room on screen it had where it came from. In order: what vshot's own
// capture stated, what the image declares, what the producer recorded, and else
// what the image's size and the target output imply.
int resolveDensity(const QJsonObject &request, QScreen *target, const QImage &image,
                   const QString &sourcePath)
{
    bool ok = false;
    const int stated = request.value(QStringLiteral("density")).toVariant().toInt(&ok);
    if (ok && stated > 0) {
        return std::clamp(stated, 1, 4);
    }
    if (const int declared = declaredDensity(image); declared > 0) {
        return declared;
    }
    if (const int recorded = recordedDensity(sourcePath); recorded > 0) {
        return recorded;
    }
    return inferDensity(image, target);
}

// Owns every pinned surface and dispatches daemon commands. It inherits
// QObject only to reuse the functor-based connect() lifetime; it declares no
// signals or slots of its own, so the build stays moc-free.
class PinServer final : public QObject {
public:
    PinServer(QLocalServer *server, QString socketPath)
        : server_(server)
        , socketPath_(std::move(socketPath))
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

    // Keeps every pin rendering on every output. The compositor can add or
    // remove outputs at any time, and a surface belongs to exactly one of
    // them, so the mapping has to follow.
    void watchScreens()
    {
        connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *screen) {
            for (Pin *pin : pins_) {
                addSurface(pin, screen);
                syncGeometry(pin);
            }
        });
        connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen *screen) {
            for (Pin *pin : pins_) {
                dropSurface(pin, screen);
                pin->origin = clampOrigin(*pin, pin->origin);
                syncGeometry(pin);
            }
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
        for (Pin *pin : pins) {
            destroySurfaces(pin);
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
            for (Pin *pin : pins) {
                destroySurfaces(pin);
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
        return addImage(image, path, path, screenFromRequest(request), request);
    }

    // Pins the image currently on the clipboard. Resolution order: embedded
    // image data (screenshots, "copy image"), then image files referenced by
    // a copied file (URI list), then a single plain-text local path, then
    // clipboard text rendered as a card (HTML, markdown, code, or plain).
    QJsonObject addClipboardPin(const QJsonObject &request)
    {
        QScreen *target = screenFromRequest(request);
        QClipboard *clipboard = QGuiApplication::clipboard();
        if (clipboard == nullptr) {
            return error(QStringLiteral("no clipboard is available"));
        }
        const QImage image = clipboard->image();
        if (!image.isNull()) {
            // Raw image data carries no path, so only its own declaration can
            // name a source; the record file cannot be matched.
            return addImage(image, QStringLiteral("clipboard"), QString(), target, request);
        }
        const QMimeData *mime = clipboard->mimeData();
        if (mime != nullptr) {
            const QList<QUrl> urls = mime->urls();
            for (const QUrl &url : urls) {
                if (!url.isLocalFile()) {
                    continue;
                }
                const QString path = url.toLocalFile();
                const QImage fileImage(path);
                if (!fileImage.isNull()) {
                    return addImage(fileImage, path, path, target, request);
                }
            }
        }
        const QString text = clipboard->text().trimmed();
        if (!text.isEmpty() && !text.contains(QLatin1Char('\n')) && QFileInfo::exists(text)) {
            const QImage pathImage(text);
            if (!pathImage.isNull()) {
                return addImage(pathImage, text, text, target, request);
            }
        }
        if (!text.isEmpty()) {
            // Cards are rasterized for the density the pin will use, so both
            // the target output and a stated density have to be resolved
            // before rendering; otherwise text would be resampled.
            QScreen *screen = target != nullptr ? target : fallbackScreen();
            if (screen == nullptr) {
                return error(QStringLiteral("no screen is available to pin onto"));
            }
            const int density = resolveDensity(request, screen, QImage(), QString());
            const QImage card = renderTextCard(mime, text, density);
            if (!card.isNull()) {
                return addImage(card, QStringLiteral("clipboard text"), QString(), target,
                                request);
            }
        }
        return error(QStringLiteral("the clipboard contains no pinnable image or text"));
    }

    // `sourcePath` is where the pixels came from, empty for data with no file
    // behind it: it is what lets a recorded capture scale be matched.
    QJsonObject addImage(const QImage &image, const QString &label, const QString &sourcePath,
                         QScreen *requested, const QJsonObject &request)
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
        pin->density = resolveDensity(request, screen, image, sourcePath);
        // Natural size: one image pixel per logical pixel of an output with
        // the same density, so a 4K capture takes the room it did on the 4K
        // output even when it lands on a 1080p one.
        pin->scale = 1.0 / pin->density;
        // Whatever the density, the opening size must fit the output entirely
        // (never upscaling past the natural one) so a pin always arrives fully
        // visible; the wheel zooms from there.
        const QSize boundsSize = screen->geometry().size();
        if (!boundsSize.isEmpty()) {
            const double fit = std::min(
                static_cast<double>(boundsSize.width()) / pin->image.width(),
                static_cast<double>(boundsSize.height()) / pin->image.height());
            pin->scale = std::min(pin->scale, fit);
        }

        // Land on the output the user is looking at, one cascade step apart
        // from the pins already there so repeated pins stay distinguishable.
        const int offset = static_cast<int>(pins_.size() % 6) * 28;
        const QRect bounds = screen->geometry();
        pin->origin = QPoint((bounds.width() - pin->displaySize().width()) / 2,
                             (bounds.height() - pin->displaySize().height()) / 2) +
                      bounds.topLeft() + QPoint(offset, offset);
        pin->origin = clampOrigin(*pin, pin->origin);

        buildSurfaces(pin);
        if (pin->surfaces.isEmpty()) {
            delete pin;
            armIdleQuit();
            return error(QStringLiteral("could not create a layer-shell pin surface"));
        }
        pins_.push_back(pin);
        byId_.insert(pin->id, pin);
        idleQuit_->stop();
        syncGeometry(pin);
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
            for (const QPointer<PinWindow> &surface : pin->surfaces) {
                if (surface != nullptr) {
                    surface->setSourceImage(image);
                }
            }
        }
        pin->origin = clampOrigin(*pin, QPoint(x, y));
        syncGeometry(pin);
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
        const QRect globalRect = pin->globalRect();
        QScreen *screen = QGuiApplication::screenAt(globalRect.center());
        if (screen == nullptr) {
            screen = pin->surfaces.isEmpty() ? fallbackScreen() : pin->surfaces.constBegin().key();
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

    void addSurface(Pin *pin, QScreen *screen)
    {
        if (screen == nullptr || pin->surfaces.contains(screen)) {
            return;
        }
        auto *surface = new PinWindow(pin->image, pin->density, screen);
        surface->setLabel(pin->label);
        surface->setScale(pin->scale);
        surface->setGlobalOrigin(pin->origin);
        surface->setCloseCallback([this, pin] { removePin(pin); });
        surface->setEditCallback([this, pin] { startEdit(pin); });
        // Both gestures belong to the pin, not to the surface that caught
        // them: the daemon moves and rescales every surface at once.
        surface->setDragCallback([this, pin](QPoint topLeft) {
            pin->origin = clampOrigin(*pin, topLeft);
            syncGeometry(pin);
        });
        surface->setZoomCallback([this, pin, surface](double factor, QPoint cursor) {
            zoomPin(pin, surface, factor, cursor);
        });
        if (!surface->showLayerSurface()) {
            delete surface;
            return;
        }
        pin->surfaces.insert(screen, surface);
        QObject::connect(surface, &QObject::destroyed, this, [this, pin, screen] {
            pin->surfaces.remove(screen);
        });
    }

    void buildSurfaces(Pin *pin)
    {
        for (QScreen *screen : QGuiApplication::screens()) {
            addSurface(pin, screen);
        }
    }

    // Retires one surface and breaks every connection into the daemon first:
    // the widget is destroyed asynchronously (WA_DeleteOnClose), and by then
    // the pin it references may already be gone.
    void detachSurface(PinWindow *surface)
    {
        if (surface == nullptr) {
            return;
        }
        surface->setCloseCallback({});
        surface->setEditCallback({});
        surface->setDragCallback({});
        surface->setZoomCallback({});
        QObject::disconnect(surface, nullptr, this, nullptr);
        surface->setPinnedVisible(false);
        surface->hide();
        surface->close();
    }

    void dropSurface(Pin *pin, QScreen *screen)
    {
        detachSurface(pin->surfaces.take(screen).data());
    }

    void destroySurfaces(Pin *pin)
    {
        const QList<QPointer<PinWindow>> surfaces = pin->surfaces.values();
        pin->surfaces.clear();
        for (const QPointer<PinWindow> &surface : surfaces) {
            detachSurface(surface.data());
        }
    }

    void removePin(Pin *pin)
    {
        if (editingPin_ == pin) {
            editingPin_ = nullptr;
        }
        pins_.removeAll(pin);
        if (byId_.value(pin->id, nullptr) == pin) {
            byId_.remove(pin->id);
        }
        // Surfaces first: their destroyed handlers still look at the pin.
        destroySurfaces(pin);
        delete pin;
        armIdleQuit();
    }

    // Applies the pin's shared state to every surface it renders on.
    void syncGeometry(Pin *pin)
    {
        for (auto it = pin->surfaces.cbegin(); it != pin->surfaces.cend(); ++it) {
            PinWindow *surface = it.value();
            if (surface == nullptr) {
                continue;
            }
            surface->setScale(pin->scale);
            surface->setGlobalOrigin(pin->origin);
            surface->setPinnedVisible(allVisible_);
        }
    }

    // Multiplicative zoom anchored on the cursor: the point under the pointer
    // stays put. Only the surface that reported the gesture shows the badge.
    void zoomPin(Pin *pin, PinWindow *source, double factor, QPoint globalCursor)
    {
        const double next = std::clamp(pin->scale * factor, kMinScale, kMaxScale);
        if (next == pin->scale) {
            return;
        }
        const QPoint anchor = globalCursor - pin->origin;
        const double ratio = next / pin->scale;
        pin->scale = next;
        pin->origin = clampOrigin(*pin,
                                  globalCursor - QPoint(qRound(anchor.x() * ratio),
                                                        qRound(anchor.y() * ratio)));
        syncGeometry(pin);
        if (source != nullptr) {
            source->showZoomBadge();
        }
    }

    void setVisible(bool visible)
    {
        allVisible_ = visible;
        for (Pin *pin : pins_) {
            syncGeometry(pin);
        }
    }

    QLocalServer *server_;
    QString socketPath_;
    QHash<QLocalSocket *, QByteArray> buffer_;
    QVector<Pin *> pins_;
    QHash<quint64, Pin *> byId_;
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

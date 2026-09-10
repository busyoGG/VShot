#include "pin_server.hpp"
#include "pin_window.hpp"
#include "text_card.hpp"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMimeData>
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

// Owns every pinned surface and dispatches daemon commands. It inherits
// QObject only to reuse the functor-based connect() lifetime; it declares no
// signals or slots of its own, so the build stays moc-free.
class PinServer final : public QObject {
public:
    explicit PinServer(QLocalServer *server)
        : server_(server)
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

    // Unmaps every surface before the process goes away. Leaving a mapped
    // layer surface behind can wedge the compositor's output frames.
    void shutdownAll()
    {
        const QVector<PinWindow *> pins = pins_;
        pins_.clear();
        for (PinWindow *pin : pins) {
            pin->setPinnedVisible(false);
            pin->hide();
            pin->close();
        }
    }

    void setPinId(PinWindow *pin, quint64 id) { pinIds_[pin] = id; }

private:
    static QJsonObject okReply()
    {
        return QJsonObject{{QStringLiteral("ok"), true}};
    }

    // Arms the idle quit when no pins are left. Called after the last pin's
    // destroyed signal and when an add fails on an empty daemon.
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
        respond(socket, dispatch(request));
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
            return addClipboardPin();
        }
        if (command == QStringLiteral("replace")) {
            return replacePin(request);
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
            const QVector<PinWindow *> pins = pins_;
            pins_.clear();
            for (PinWindow *pin : pins) {
                pin->close(); // WA_DeleteOnClose destroys the surface
            }
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
        return addImage(image, path);
    }

    // Pins the image currently on the clipboard. Resolution order: embedded
    // image data (screenshots, "copy image"), then image files referenced by
    // a copied file (URI list), then a single plain-text local path, then
    // clipboard text rendered as a card (HTML, markdown, code, or plain).
    QJsonObject addClipboardPin()
    {
        QClipboard *clipboard = QGuiApplication::clipboard();
        if (clipboard == nullptr) {
            return error(QStringLiteral("no clipboard is available"));
        }
        const QImage image = clipboard->image();
        if (!image.isNull()) {
            return addImage(image, QStringLiteral("clipboard"));
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
                    return addImage(fileImage, path);
                }
            }
        }
        const QString text = clipboard->text().trimmed();
        if (!text.isEmpty() && !text.contains(QLatin1Char('\n')) && QFileInfo::exists(text)) {
            const QImage pathImage(text);
            if (!pathImage.isNull()) {
                return addImage(pathImage, text);
            }
        }
        if (!text.isEmpty()) {
            QScreen *screen = QGuiApplication::primaryScreen();
            if (screen == nullptr) {
                return error(QStringLiteral("no screen is available to pin onto"));
            }
            const int ratio =
                std::clamp(qRound(screen->devicePixelRatio()), 1, 4);
            const QImage card = renderTextCard(mime, text, ratio);
            if (!card.isNull()) {
                return addImage(card, QStringLiteral("clipboard text"));
            }
        }
        return error(QStringLiteral("the clipboard contains no pinnable image or text"));
    }

    QJsonObject addImage(const QImage &image, const QString &label)
    {
        if (image.isNull()) {
            armIdleQuit();
            return error(QStringLiteral("cannot pin an empty image"));
        }
        QScreen *screen = QGuiApplication::primaryScreen();
        if (screen == nullptr) {
            armIdleQuit();
            return error(QStringLiteral("no screen is available to pin onto"));
        }
        auto *pin = new PinWindow(image, screen);
        pin->setLabel(label);
        pin->setEditCallback([this, pin] { startEdit(pin); });
        pin->setCloseCallback([this, pin] { pinIds_.remove(pin); });
        QObject::connect(pin, &QObject::destroyed, this, [this, pin] {
            pins_.removeAll(pin);
            pinIds_.remove(pin);
            armIdleQuit();
        });
        if (!pin->showLayerSurface()) {
            pin->deleteLater();
            return error(QStringLiteral("could not create a layer-shell pin surface"));
        }
        pins_.push_back(pin);
        pinIds_[pin] = nextId_++;
        idleQuit_->stop();
        // Light cascade so repeated pins remain distinguishable.
        const int offset = static_cast<int>(pins_.size() - 1) % 6 * 28;
        pin->placeCentered(QPoint(offset, offset));
        pin->setPinnedVisible(allVisible_);
        return okReply();
    }

    // Replaces the pixels of an existing pin (pin-edit round trip).
    QJsonObject replacePin(const QJsonObject &request)
    {
        bool idOk = false;
        const quint64 id = request.value(QStringLiteral("id")).toVariant().toULongLong(&idOk);
        const QString path = request.value(QStringLiteral("path")).toString();
        if (!idOk || path.isEmpty()) {
            return error(QStringLiteral("pin replace requires `id` and `path`"));
        }
        PinWindow *target = nullptr;
        for (auto it = pinIds_.cbegin(); it != pinIds_.cend(); ++it) {
            if (it.value() == id) {
                target = it.key();
                break;
            }
        }
        if (target == nullptr) {
            return error(QStringLiteral("pin %1 no longer exists").arg(id));
        }
        const QImage image(path);
        if (image.isNull()) {
            return error(QStringLiteral("cannot load replacement image `%1`").arg(path));
        }
        target->setSourceImage(image);
        return okReply();
    }

    // One Space-triggered edit round: export the pin's pixels, describe the
    // pin-edit session, and run `vshot pin --apply` in the background. That
    // process shows the annotation editor, renders the result in Rust, and
    // sends `replace` back to this daemon.
    void startEdit(PinWindow *pin)
    {
        if (editingPin_ != nullptr) {
            return; // one edit session at a time
        }
        const auto idIt = pinIds_.constFind(pin);
        if (idIt == pinIds_.constEnd()) {
            return;
        }
        QScreen *screen = pin->screen();
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
        if (!pin->sourceImage().save(imagePath, "PNG")) {
            delete directory;
            return;
        }

        const QRect display = pin->displayRect();
        const QRect globalRect = display.translated(screen->geometry().topLeft());

        // Field layout must match the Rust-written region session: the rect
        // fields sit directly on the output object (not nested).
        QJsonObject output;
        output.insert(QStringLiteral("id"), 0);
        output.insert(QStringLiteral("name"), screen->name());
        output.insert(QStringLiteral("x"), static_cast<qint64>(globalRect.x()));
        output.insert(QStringLiteral("y"), static_cast<qint64>(globalRect.y()));
        output.insert(QStringLiteral("width"), static_cast<qint64>(globalRect.width()));
        output.insert(QStringLiteral("height"), static_cast<qint64>(globalRect.height()));
        output.insert(QStringLiteral("scale"), 1);
        output.insert(QStringLiteral("pixel_width"),
                      static_cast<qint64>(pin->sourceImage().width()));
        output.insert(QStringLiteral("pixel_height"),
                      static_cast<qint64>(pin->sourceImage().height()));
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
        session.insert(QStringLiteral("id"), static_cast<qint64>(idIt.value()));
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
        connect(pin, &QObject::destroyed, this, [this] { editingPin_ = nullptr; });

        QString cliPath = QString::fromLocal8Bit(qgetenv("VSHOT_BIN"));
        if (cliPath.isEmpty()) {
            char buffer[4096];
            const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
            if (length > 0) {
                buffer[length] = '\0';
                const QDir helperDir = QFileInfo(QString::fromLocal8Bit(buffer)).dir();
                // The helper lives next to vshot (installed) or in build-qt/
                // (in-tree), so look one and two levels up as well.
                for (const QString &candidate :
                     {helperDir.filePath(QStringLiteral("vshot")),
                      helperDir.filePath(QStringLiteral("../vshot")),
                      helperDir.filePath(QStringLiteral("../../vshot"))}) {
                    if (QFileInfo::exists(candidate)) {
                        cliPath = QFileInfo(candidate).absoluteFilePath();
                        break;
                    }
                }
            }
        }
        if (cliPath.isEmpty()) {
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

    void setVisible(bool visible)
    {
        allVisible_ = visible;
        for (PinWindow *pin : pins_) {
            pin->setPinnedVisible(visible);
        }
    }

    QLocalServer *server_;
    QHash<QLocalSocket *, QByteArray> buffer_;
    QVector<PinWindow *> pins_;
    QHash<PinWindow *, quint64> pinIds_;
    quint64 nextId_ = 1;
    PinWindow *editingPin_ = nullptr;
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

    PinServer controller(server);
    QObject::connect(server, &QLocalServer::newConnection, &controller,
                     &PinServer::handleNewConnection);
    // close() also removes the listening socket file; otherwise the next
    // spawn would inherit a stale entry (removeServer covers that, but a
    // dangling socket is confusing to users).
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, server, [server, &controller] {
        controller.shutdownAll();
        server->close();
    });
    // A signal-killed client can leave a mapped layer surface behind, which
    // is fatal for the compositor's frame loop, so SIGTERM/SIGINT unmap first.
    installTerminateNotifier(&controller, [] { QCoreApplication::quit(); });
    return QCoreApplication::exec();
}

} // namespace vshot

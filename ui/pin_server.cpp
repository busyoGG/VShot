#include "pin_server.hpp"
#include "pin_window.hpp"

#include <QClipboard>
#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMimeData>
#include <QRect>
#include <QScreen>
#include <QSocketNotifier>
#include <QUrl>

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

// Owns every pinned surface and dispatches daemon commands. It inherits
// QObject only to reuse the functor-based connect() lifetime; it declares no
// signals or slots of its own, so the build stays moc-free.
class PinServer final : public QObject {
public:
    explicit PinServer(QLocalServer *server)
        : server_(server)
    {
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

private:
    static QJsonObject okReply()
    {
        return QJsonObject{{QStringLiteral("ok"), true}};
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
    // a copied file (URI list), then a single plain-text local path.
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
        return error(QStringLiteral("the clipboard does not contain an image"));
    }

    QJsonObject addImage(const QImage &image, const QString &label)
    {
        if (image.isNull()) {
            return error(QStringLiteral("cannot pin an empty image"));
        }
        QScreen *screen = QGuiApplication::primaryScreen();
        if (screen == nullptr) {
            return error(QStringLiteral("no screen is available to pin onto"));
        }
        auto *pin = new PinWindow(image, screen);
        pin->setLabel(label);
        QObject::connect(pin, &QObject::destroyed, this, [this, pin] {
            pins_.removeAll(pin);
        });
        if (!pin->showLayerSurface()) {
            pin->deleteLater();
            return error(QStringLiteral("could not create a layer-shell pin surface"));
        }
        pins_.push_back(pin);
        // Light cascade so repeated pins remain distinguishable.
        const int offset = static_cast<int>(pins_.size() - 1) % 6 * 28;
        pin->placeCentered(QPoint(offset, offset));
        pin->setPinnedVisible(allVisible_);
        return okReply();
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
    bool allVisible_ = true;
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

#include "capture_overlay.hpp"
#include "i18n.hpp"
#include "pin_edit.hpp"
#include "pin_server.hpp"
#include "session_protocol.hpp"

#include <QApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>

#include <cstdio>
#include <memory>

namespace {

void reportError(const QString &message)
{
    std::fprintf(stderr, "vshot-qt-ui: %s\n", message.toUtf8().constData());
}

QScreen *screenForOutput(const vshot::OutputSession &output)
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        if (screen != nullptr && screen->name() == output.name) {
            return screen;
        }
    }
    for (QScreen *screen : screens) {
        if (screen == nullptr) {
            continue;
        }
        const QRect geometry = screen->geometry();
        if (geometry.x() == output.geometry.x && geometry.y() == output.geometry.y &&
            geometry.width() == static_cast<int>(output.geometry.width) &&
            geometry.height() == static_cast<int>(output.geometry.height)) {
            return screen;
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");
    // wlroots compositors only expose the clipboard through the unstable
    // zwlr_data_control_v1 protocol, which Qt keeps behind this opt-in flag.
    // Without it the pin daemon cannot read `wl-copy` content.
    if (!qEnvironmentVariableIsSet("QT_WAYLAND_USE_DATA_CONTROL")) {
        qputenv("QT_WAYLAND_USE_DATA_CONTROL", "1");
    }

    // QApplication consumes Qt-managed command-line options it recognizes
    // (including the session-management option that shares our `--session`
    // spelling), so parse our own arguments before constructing it.
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--pin-server")) {
        const QString socketPath = QString::fromLocal8Bit(argv[2]);
        if (!socketPath.startsWith(QLatin1Char('/'))) {
            std::fprintf(stderr, "vshot-qt-ui: pin socket path must be absolute\n");
            return 2;
        }
        QApplication app(argc, argv);
        QApplication::setQuitOnLastWindowClosed(false);
        vshot::initUiLanguage();
        return vshot::runPinServer(socketPath);
    }
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--pin-edit")) {
        const QString sessionPath = QString::fromLocal8Bit(argv[2]);
        if (!QFileInfo(sessionPath).isAbsolute()) {
            reportError(QStringLiteral("pin-edit session path must be absolute"));
            return 2;
        }
        QApplication app(argc, argv);
        QApplication::setQuitOnLastWindowClosed(true);
        vshot::initUiLanguage();
        return vshot::runPinEdit(sessionPath);
    }
    if (argc != 3 || QString::fromLocal8Bit(argv[1]) != QStringLiteral("--session")) {
        reportError(QStringLiteral("usage: vshot-qt-ui --session <absolute-json-path>\n"
                                   "       vshot-qt-ui --pin-edit <absolute-json-path>\n"
                                   "       vshot-qt-ui --pin-server <absolute-socket-path>"));
        return 2;
    }
    const QString sessionPath = QString::fromLocal8Bit(argv[2]);
    if (!QFileInfo(sessionPath).isAbsolute()) {
        reportError(QStringLiteral("session path must be absolute"));
        return 2;
    }

    QApplication app(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    // Resolve the UI language before any widget text is generated.
    vshot::initUiLanguage();

    vshot::Session session;
    QString error;
    if (!vshot::loadSession(sessionPath, &session, &error)) {
        reportError(error);
        return 1;
    }

    QJsonDocument result;
    int exitCode = 0;
    QVector<vshot::CaptureOverlay *> overlays;
    {
        vshot::OverlayController controller(std::move(session));
        controller.setTerminalCallback([&app] {
            app.quit();
        });

        for (int index = 0; index < controller.outputCount(); ++index) {
            const vshot::OutputSession &output = controller.session().outputs.at(index);
            QScreen *screen = screenForOutput(output);
            if (screen == nullptr) {
                reportError(QStringLiteral("no Qt screen matches output `%1`").arg(output.name));
                exitCode = 1;
                break;
            }
            vshot::CaptureOverlay *overlay = controller.addOverlay(index, screen, &error);
            if (overlay == nullptr) {
                reportError(error);
                exitCode = 1;
                break;
            }
            overlays.push_back(overlay);
        }

        if (exitCode == 0) {
            for (vshot::CaptureOverlay *overlay : overlays) {
                if (!overlay->showLayerSurface()) {
                    reportError(QStringLiteral("could not initialize LayerShellQt overlay for `%1`")
                                    .arg(overlay->output().name));
                    exitCode = 1;
                    break;
                }
            }
        }
        if (exitCode == 0) {
            app.exec();
            if (!controller.isFinished()) {
                controller.cancel();
            }
            QString resultError;
            result = controller.resultDocument(QFileInfo(sessionPath).absolutePath(),
                                               &resultError);
            if (result.isNull() && !resultError.isEmpty()) {
                reportError(resultError);
                exitCode = 1;
            }
        }
    }

    qDeleteAll(overlays);
    if (exitCode != 0) {
        return exitCode;
    }
    const QByteArray encodedResult = result.toJson(QJsonDocument::Compact);
    std::fwrite(encodedResult.constData(), 1, static_cast<std::size_t>(encodedResult.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return 0;
}

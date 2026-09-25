// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#include "capture_overlay.hpp"
#include "hint_window.hpp"
#include "i18n.hpp"
#include "pin_edit.hpp"
#include "pin_server.hpp"
#include "file_dialog.hpp"
#include "session_protocol.hpp"
#include "settings_window.hpp"

#include <QApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QStandardPaths>

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
    // QApplication consumes Qt-managed command-line options it recognizes
    // (including the session-management option that shares our `--session`
    // spelling), so parse our own arguments before constructing it -- and
    // before any platform-level environment is set, because which shell
    // integration is wanted depends on the mode.
    const bool settingsMode =
        argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--settings");
    // A file dialog is a layer surface like every other vshot window, so it
    // keeps the layer-shell integration.  It has to: the frozen frame it is
    // opened over is a layer surface, and a compositor draws a layer above
    // every ordinary window, so a dialog opened as a toplevel would sit behind
    // the frame -- invisible and unable to take a click.  Its arguments are the
    // suggested path and, optionally, the output to open on.
    const bool saveMode =
        argc >= 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--save-dialog");
    const bool openMode =
        argc >= 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--open-dialog");
    // The settings window is the exception: it is a plain toplevel dialog, and
    // a dialog under the layer-shell integration is created and then never
    // mapped, because there is no layer surface to map it as.  So the override
    // is set for every mode but that one -- and cleared there, so a session
    // that exports it globally cannot hide the window with no error to show
    // for it.
    if (settingsMode) {
        qunsetenv("QT_WAYLAND_SHELL_INTEGRATION");
    } else {
        qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");
    }
    // The clipboard is read with `wl-paste` rather than through Qt's own
    // Wayland clipboard: Qt implements only the wlroots
    // `zwlr_data_control_v1`, which a KWin session does not offer, and its
    // standard path hands over nothing to a client that has no focus.

    if (settingsMode) {
        QApplication app(argc, argv);
        // The desktop file this window's icon comes from.  Wayland has no
        // per-window icon: a compositor takes the application id the client
        // declares, finds `<id>.desktop` in the installed data directories and
        // draws whatever `Icon=` names.  The id has to be set before the window
        // is created.
        //
        // It is declared only when that file is actually installed.  The id is
        // not just an icon lookup: Qt also hands it to the host portal, which
        // resolves it against the same data directories and fails loudly when
        // there is no such file -- "Failed to register with host portal ...
        // App info not found for 'vshot-settings'" on stderr -- which is what a
        // build run straight out of `build-qt/` or `target/` would print.
        // Without the id the window is simply left with the compositor's
        // generic placeholder, which is the honest answer for a copy that has
        // no desktop entry.
        const QString desktopFile = QStringLiteral("vshot-settings.desktop");
        if (!QStandardPaths::locate(QStandardPaths::ApplicationsLocation, desktopFile)
                 .isEmpty()) {
            QGuiApplication::setDesktopFileName(QStringLiteral("vshot-settings"));
        }
        QApplication::setQuitOnLastWindowClosed(true);
        vshot::initUiLanguage();
        return vshot::runSettingsWindow();
    }
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
    if (saveMode || openMode) {
        const QString suggested = QString::fromLocal8Bit(argv[2]);
        const QString screenName = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString();
        QApplication app(argc, argv);
        QApplication::setQuitOnLastWindowClosed(true);
        return saveMode ? vshot::runSaveDialog(suggested, screenName)
                        : vshot::runOpenDialog(suggested, screenName);
    }
    if (argc != 3 || QString::fromLocal8Bit(argv[1]) != QStringLiteral("--session")) {
        reportError(QStringLiteral("usage: vshot-qt-ui --session <absolute-json-path>\n"
                                   "       vshot-qt-ui --settings\n"
                                   "       vshot-qt-ui --pin-edit <absolute-json-path>\n"
                                   "       vshot-qt-ui --save-dialog <suggested-path> [output]\n"
                                   "       vshot-qt-ui --open-dialog <suggested-path> [output]\n"
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

    // A scrolling capture's overlay is not a capture surface: it draws no
    // frozen frame, leaves the desktop live, and only reports progress and
    // takes the two keys that end the capture.  That is its own small window
    // rather than the full-screen region editor.
    if (session.mode == QStringLiteral("long-shot")) {
        vshot::HintWindow hint(session);
        hint.setTerminalCallback([&app] { app.quit(); });
        if (!hint.showLayerSurface()) {
            reportError(QStringLiteral("could not initialize the LayerShellQt hint overlay"));
            return 1;
        }
        app.exec();
        const bool cancelled = hint.isCancelled();
        QJsonObject reply;
        reply.insert(QStringLiteral("done"), !cancelled);
        reply.insert(QStringLiteral("cancelled"), cancelled);
        const QByteArray encodedReply = QJsonDocument(reply).toJson(QJsonDocument::Compact);
        std::fwrite(encodedReply.constData(), 1, static_cast<std::size_t>(encodedReply.size()),
                    stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
        return 0;
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
            // A region session may arrive with its selection already made (the
            // window picker resolved one): open with the toolbar up instead of
            // waiting for a drag that will never come.  A picking session, in
            // turn, keeps asking the CLI for the windows it should highlight,
            // because the desktop it runs on is live.
            controller.beginPresetEdit();
            controller.enableCandidateRefresh();
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

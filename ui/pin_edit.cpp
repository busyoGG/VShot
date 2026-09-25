// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#include "pin_edit.hpp"
#include "capture_overlay.hpp"
#include "i18n.hpp"
#include "session_protocol.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRect>
#include <QScreen>

#include <cstdio>

namespace vshot {
namespace {

void reportError(const QString &message)
{
    std::fprintf(stderr, "vshot-qt-ui: %s\n", message.toUtf8().constData());
}

QScreen *screenForRect(const QRect &rect)
{
    // The editor surface covers the pin, which lives on exactly one output;
    // pick the screen containing the rect center, falling back to the first
    // intersecting one.
    const QPoint center = rect.center();
    for (QScreen *screen : QGuiApplication::screens()) {
        if (screen != nullptr && screen->geometry().contains(center)) {
            return screen;
        }
    }
    for (QScreen *screen : QGuiApplication::screens()) {
        if (screen != nullptr && screen->geometry().intersects(rect)) {
            return screen;
        }
    }
    return nullptr;
}

} // namespace

int runPinEdit(const QString &sessionPath)
{
    Session session;
    QString error;
    if (!loadSession(sessionPath, &session, &error)) {
        reportError(error);
        return 1;
    }
    if (session.mode != QStringLiteral("pin-edit")) {
        reportError(QStringLiteral("pin-edit requires a `pin-edit` session"));
        return 1;
    }

    const QRect pinRect(session.bounds.x, session.bounds.y,
                        static_cast<int>(session.bounds.width),
                        static_cast<int>(session.bounds.height));
    // The editor surface is the whole screen the pin sits on: the toolbar and
    // its popups live on the transparent canvas around the pinned image,
    // exactly like the toolbar beside a region selection.
    QScreen *screen = screenForRect(pinRect);
    if (screen == nullptr) {
        reportError(QStringLiteral("no Qt screen matches the pin-edit window"));
        return 1;
    }
    const QRect surface = screen->geometry();
    // The session's own geometry is the pinned image rect; only the surface
    // grows. Both are global logical pixels.
    Session surfaceSession = session;
    for (OutputSession &output : surfaceSession.outputs) {
        output.surface = LogicalRect{surface.x(), surface.y(),
                                     static_cast<std::uint32_t>(surface.width()),
                                     static_cast<std::uint32_t>(surface.height())};
    }

    OverlayController controller(std::move(surfaceSession));
    controller.setPinEditMode(true);
    // The editor drives the real pin window over the daemon socket instead of
    // painting a second copy of the image.
    controller.setPinTarget(session.pinId, session.pinSocket);
    controller.setTerminalCallback([] { QCoreApplication::quit(); });

    QString overlayError;
    CaptureOverlay *overlay = controller.addOverlay(0, screen, &overlayError);
    if (overlay == nullptr) {
        reportError(overlayError);
        return 1;
    }
    // A plain toplevel cannot be positioned under Wayland (the compositor
    // decides), so the editor is a layer surface anchored top-left with
    // margins, covering the output. Exclusive keyboard: the editor owns all
    // input while it is open.
    if (!overlay->showLayerSurfaceAt(surface.x(), surface.y(), surface.width(),
                                     surface.height())) {
        reportError(QStringLiteral("could not create the pin-edit surface"));
        return 1;
    }
    // Jump straight into annotation editing with the whole image selected.
    controller.beginPinEdit();

    QCoreApplication::exec();
    if (!controller.isFinished()) {
        controller.cancel();
    }

    // The helper already reports the pin image's final rect as the selection:
    // the daemon repositions the pin there and renders the annotations
    // relative to it, so nothing needs rewriting here.
    QJsonDocument result;
    QString resultError;
    result = controller.resultDocument(QFileInfo(sessionPath).absolutePath(), &resultError);
    if (result.isNull() && !resultError.isEmpty()) {
        reportError(resultError);
        return 1;
    }
    const QByteArray encoded = result.toJson(QJsonDocument::Compact);
    std::fwrite(encoded.constData(), 1, static_cast<std::size_t>(encoded.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return 0;
}

} // namespace vshot

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

    QScreen *screen = screenForRect(QRect(session.bounds.x, session.bounds.y,
                                          static_cast<int>(session.bounds.width),
                                          static_cast<int>(session.bounds.height)));
    if (screen == nullptr) {
        reportError(QStringLiteral("no Qt screen matches the pin-edit window"));
        return 1;
    }

    OverlayController controller(std::move(session));
    controller.setPinEditMode(true);
    controller.setTerminalCallback([] { QCoreApplication::quit(); });

    QString overlayError;
    CaptureOverlay *overlay = controller.addOverlay(0, screen, &overlayError);
    if (overlay == nullptr) {
        reportError(overlayError);
        return 1;
    }
    // A plain toplevel cannot be positioned under Wayland (the compositor
    // decides), so the editor is a layer surface anchored top-left with
    // margins, exactly over the pin. Exclusive keyboard: the editor owns all
    // input while it is open.
    if (!overlay->showLayerSurfaceAt(session.bounds.x, session.bounds.y,
                                     static_cast<int>(session.bounds.width),
                                     static_cast<int>(session.bounds.height))) {
        reportError(QStringLiteral("could not create the pin-edit surface"));
        return 1;
    }
    // Jump straight into annotation editing with the whole image selected.
    controller.beginPinEdit();

    QCoreApplication::exec();
    if (!controller.isFinished()) {
        controller.cancel();
    }

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

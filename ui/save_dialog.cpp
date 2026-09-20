#include "save_dialog.hpp"

#include "i18n.hpp"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <cstdio>

namespace vshot {
namespace {

// The directory a dialog opens in, and the name it starts on. The caller's
// suggestion is a file path when the pin came from one -- then its own
// directory is the better guess, because that is where the user is working.
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

} // namespace

int runSaveDialog(const QString &suggestedPath)
{
    initUiLanguage();
    // PNG and nothing else: what gets saved is a screenshot, a color card or a
    // rendered text card, none of which wants a lossy format, and PNG is the
    // one image format Qt always has.
    const QString path = QFileDialog::getSaveFileName(nullptr, uiTr("Save pinned image"),
                                                      initialPathFor(suggestedPath),
                                                      uiTr("PNG image (*.png)"));
    if (path.isEmpty()) {
        report(QString());
        return 0;
    }
    // A name typed without an extension gets one, so the caller never has to
    // guess a format from a path.
    report(QFileInfo(path).suffix().isEmpty() ? path + QStringLiteral(".png") : path);
    return 0;
}

} // namespace vshot

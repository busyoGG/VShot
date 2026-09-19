#include "config.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace vshot {
namespace {

constexpr int kMaxWidth = 64;
constexpr int kMaxTextSize = 64;
constexpr int kMaxArrowSize = 8;
constexpr int kMaxMosaicStrength = 3;

/// Reads a non-negative integer, clamped into `[1, max]`; anything absent or
/// of the wrong type keeps `fallback`.
std::uint32_t readBounded(const QJsonObject &object, const QString &key, std::uint32_t fallback,
                          std::uint32_t max)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return fallback;
    }
    const double raw = value.toDouble();
    if (!std::isfinite(raw)) {
        return fallback;
    }
    const auto rounded = static_cast<long long>(raw);
    if (rounded < 1) {
        return fallback;
    }
    return static_cast<std::uint32_t>(std::min<long long>(rounded, max));
}

/// Reads one of `allowed`; anything else keeps `fallback`.  The values come
/// from a file the user can edit, so an unknown one is a typo to ignore
/// rather than something to carry into the editor.
QString readChoice(const QJsonObject &object, const QString &key, const QString &fallback,
                   const QStringList &allowed)
{
    const QJsonValue value = object.value(key);
    if (!value.isString()) {
        return fallback;
    }
    const QString text = value.toString();
    return allowed.contains(text) ? text : fallback;
}

QString readString(const QJsonObject &object, const QString &key, const QString &fallback)
{
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : fallback;
}

/// A color is stored as `#rrggbb` or `#rrggbbaa`, which is what the editor's
/// own hex field accepts, so a value copied out of the UI can be pasted back
/// into the file unchanged.
QColor readColor(const QJsonObject &object, const QString &key, const QColor &fallback)
{
    const QJsonValue value = object.value(key);
    if (!value.isString()) {
        return fallback;
    }
    const QColor color(value.toString());
    return color.isValid() ? color : fallback;
}

QString colorText(const QColor &color)
{
    // `#rrggbb` when opaque, `#rrggbbaa` when not: the shorter form is what
    // people write by hand.
    return color.alpha() == 255 ? color.name(QColor::HexRgb) : color.name(QColor::HexArgb);
}

} // namespace

QString configFilePath()
{
    // `AppConfigLocation` would append the application name, which here is
    // `vshot-qt-ui` -- a different directory from the `vshot` the CLI side
    // uses, and the two sides have to agree on one file.  So the directory is
    // built from `GenericConfigLocation` (which honours XDG_CONFIG_HOME and
    // falls back to `$HOME/.config`) plus the name both sides share.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty()) {
        return QString();
    }
    return base + QStringLiteral("/vshot/config.json");
}

EditorPreferences loadEditorPreferences()
{
    EditorPreferences preferences;
    const QString path = configFilePath();
    if (path.isEmpty()) {
        return preferences;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // No config yet is the normal first run, not a problem worth a word.
        return preferences;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return preferences;
    }
    const QJsonObject object = document.object();
    const QJsonValue editorValue = object.value(QStringLiteral("editor"));
    if (!editorValue.isObject()) {
        return preferences;
    }
    const QJsonObject editor = editorValue.toObject();

    preferences.tool = readChoice(
        editor, QStringLiteral("tool"), preferences.tool,
        {QStringLiteral("select"), QStringLiteral("rectangle"), QStringLiteral("ellipse"),
         QStringLiteral("arrow"), QStringLiteral("pen"), QStringLiteral("text"),
         QStringLiteral("mosaic")});
    preferences.color = readColor(editor, QStringLiteral("color"), preferences.color);
    preferences.font = readString(editor, QStringLiteral("font"), preferences.font);
    preferences.width =
        readBounded(editor, QStringLiteral("width"), preferences.width, kMaxWidth);
    preferences.textSize =
        readBounded(editor, QStringLiteral("textSize"), preferences.textSize, kMaxTextSize);
    preferences.dash = readChoice(editor, QStringLiteral("dash"), preferences.dash,
                                  {QStringLiteral("solid"), QStringLiteral("dashed"),
                                   QStringLiteral("dotted")});
    preferences.arrowSize = readBounded(editor, QStringLiteral("arrowSize"),
                                        preferences.arrowSize, kMaxArrowSize);
    preferences.arrowStyle = readChoice(editor, QStringLiteral("arrowStyle"),
                                        preferences.arrowStyle,
                                        {QStringLiteral("open"), QStringLiteral("filled")});
    preferences.mosaicShape = readChoice(editor, QStringLiteral("mosaicShape"),
                                         preferences.mosaicShape,
                                         {QStringLiteral("rect"), QStringLiteral("ellipse"),
                                          QStringLiteral("brush")});
    preferences.mosaicStrength = readBounded(editor, QStringLiteral("mosaicStrength"),
                                             preferences.mosaicStrength, kMaxMosaicStrength);
    return preferences;
}

void saveEditorPreferences(const EditorPreferences &preferences)
{
    const QString path = configFilePath();
    if (path.isEmpty()) {
        return;
    }
    QJsonObject editor;
    editor.insert(QStringLiteral("tool"), preferences.tool);
    editor.insert(QStringLiteral("color"), colorText(preferences.color));
    editor.insert(QStringLiteral("font"), preferences.font);
    editor.insert(QStringLiteral("width"), static_cast<double>(preferences.width));
    editor.insert(QStringLiteral("textSize"), static_cast<double>(preferences.textSize));
    editor.insert(QStringLiteral("dash"), preferences.dash);
    editor.insert(QStringLiteral("arrowSize"), static_cast<double>(preferences.arrowSize));
    editor.insert(QStringLiteral("arrowStyle"), preferences.arrowStyle);
    editor.insert(QStringLiteral("mosaicShape"), preferences.mosaicShape);
    editor.insert(QStringLiteral("mosaicStrength"),
                  static_cast<double>(preferences.mosaicStrength));

    QJsonObject root;
    root.insert(QStringLiteral("editor"), editor);

    const QFileInfo info(path);
    QDir directory = info.absoluteDir();
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        return;
    }
    // `QSaveFile` writes to a temporary and renames, so a crash mid-write
    // cannot leave a half-written file that the next run would refuse.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.commit();
}

} // namespace vshot

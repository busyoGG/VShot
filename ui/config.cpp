#include "config.hpp"

#include "text_size.hpp"

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
#include <utility>

namespace vshot {
namespace {

/// The accepted values for each enumerated field.  They live here rather than
/// at the call sites because two readers depend on them: the loader below, and
/// the settings window's combo boxes, which must offer exactly what the loader
/// will accept.
const QStringList kToolNames = {QStringLiteral("select"), QStringLiteral("rectangle"),
                                QStringLiteral("ellipse"), QStringLiteral("arrow"),
                                QStringLiteral("pen"), QStringLiteral("text"),
                                QStringLiteral("mosaic")};
const QStringList kDashNames = {QStringLiteral("solid"), QStringLiteral("dashed"),
                                QStringLiteral("dotted")};
const QStringList kArrowStyleNames = {QStringLiteral("open"), QStringLiteral("filled")};
const QStringList kMosaicShapeNames = {QStringLiteral("rect"), QStringLiteral("ellipse"),
                                       QStringLiteral("brush")};
const QStringList kCompressionNames = {QStringLiteral("none"), QStringLiteral("fastest"),
                                       QStringLiteral("fast"), QStringLiteral("balanced"),
                                       QStringLiteral("high")};
const QStringList kInjectNames = {QStringLiteral("auto"), QStringLiteral("wlr"),
                                  QStringLiteral("portal"), QStringLiteral("uinput")};

constexpr int kMaxWidth = 64;
// The text size is a pixel height, and its range comes from `ui/text_size.hpp`
// (7 px, one glyph cell, to 448 px, the legacy scale's maximum).  It is not
// repeated here as a literal so the two cannot drift.
constexpr int kMaxArrowSize = 8;
constexpr int kMaxMosaicStrength = 3;
constexpr int kMaxDensity = 4;
// The dialog's rim.  A radius past half the window's shorter side would stop
// being a corner and start being a lozenge, so the ceiling is well inside that;
// the stroke stops before it eats the dialog's own margins.  Both live in
// `config.hpp` where the settings window can see them.
// A radius larger than any pin can carry.  What is actually painted is clamped
// to half the shorter side of the image where the pin is drawn -- the size is
// the daemon's and changes with every wheel step, so it cannot be checked here.
// The value itself is in `config.hpp`, where the settings window can see it.

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

/// Like [`readBounded`] but a zero is a value rather than an absent one.
///
/// The pin's corner radius and its rim width are both settings whose most
/// useful value is 0 -- square corners, no rim -- so "the file says 0" and "the
/// file says nothing" cannot be the same answer.  An absent key keeps the
/// fallback; a written 0 keeps the 0.
std::uint32_t readWhole(const QJsonObject &object, const QString &key, std::uint32_t fallback,
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
    if (rounded < 0) {
        return fallback;
    }
    return static_cast<std::uint32_t>(std::min<long long>(rounded, max));
}

/// Reads a boolean, keeping `fallback` for anything that is not one.
bool readFlag(const QJsonObject &object, const QString &key, bool fallback)
{
    const QJsonValue value = object.value(key);
    return value.isBool() ? value.toBool() : fallback;
}

/// Reads an integer that may be negative, clamped into `[-max, max]`.
///
/// The shadow's offset is the one value in the file with a meaning on both
/// sides of zero -- it is what says whether the light comes from above or from
/// below -- so it cannot go through the non-negative readers above.
int readSigned(const QJsonObject &object, const QString &key, int fallback, int max)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return fallback;
    }
    const double raw = value.toDouble();
    if (!std::isfinite(raw)) {
        return fallback;
    }
    return static_cast<int>(std::clamp<long long>(static_cast<long long>(raw), -max, max));
}

/// Like [`readBounded`] but for a value that may legitimately be zero, which
/// the `cli` section uses for "the file says nothing".
std::uint32_t readOptional(const QJsonObject &object, const QString &key, std::uint32_t max)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return 0;
    }
    const double raw = value.toDouble();
    if (!std::isfinite(raw)) {
        return 0;
    }
    const auto rounded = static_cast<long long>(raw);
    if (rounded < 1) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::min<long long>(rounded, max));
}

std::uint64_t readOptionalWide(const QJsonObject &object, const QString &key, std::uint64_t max)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return 0;
    }
    const double raw = value.toDouble();
    if (!std::isfinite(raw)) {
        return 0;
    }
    const auto rounded = static_cast<long long>(raw);
    if (rounded < 1) {
        return 0;
    }
    return static_cast<std::uint64_t>(std::min<long long>(rounded, static_cast<long long>(max)));
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

/// The `#rrggbbaa` spelling of a color that is not opaque.
QString colorTextWithAlpha(const QColor &color)
{
    return QStringLiteral("#%1%2%3%4")
        .arg(color.red(), 2, 16, QLatin1Char('0'))
        .arg(color.green(), 2, 16, QLatin1Char('0'))
        .arg(color.blue(), 2, 16, QLatin1Char('0'))
        .arg(color.alpha(), 2, 16, QLatin1Char('0'));
}

/// A color is stored as `#rrggbb`, or `#rrggbbaa` when it is not opaque.
///
/// This is the CSS spelling, and it is parsed here by hand rather than handed
/// to `QColor(QString)`: Qt reads an eight-digit literal as `#aarrggbb`
/// instead, so `#ff8800ff` -- which every other tool calls opaque orange --
/// would come out as purple.  The file is meant to be written by hand, so the
/// spelling has to be the one people already know.
QColor readColor(const QJsonObject &object, const QString &key, const QColor &fallback)
{
    const QJsonValue value = object.value(key);
    if (!value.isString()) {
        return fallback;
    }
    const QColor color = parseColorText(value.toString());
    return color.isValid() ? color : fallback;
}

/// The whole file as it is on disk, so a save can merge into it instead of
/// replacing it.  An unreadable or malformed file reads as empty, which turns
/// the next save into a plain write.
QJsonObject readRoot()
{
    const QString path = configFilePath();
    if (path.isEmpty()) {
        return QJsonObject();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QJsonObject();
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return QJsonObject();
    }
    return document.object();
}

/// Merges `section` into `root`'s key of the same name, leaving keys the
/// incoming object does not mention as they were.
void mergeSection(QJsonObject &root, const QString &name, const QJsonObject &section)
{
    QJsonObject merged = root.value(name).toObject();
    for (auto entry = section.constBegin(); entry != section.constEnd(); ++entry) {
        merged.insert(entry.key(), entry.value());
    }
    root.insert(name, merged);
}

/// The `editor` keys this build has retired, dropped from the file on the next
/// save so a stale one cannot be read as though it still meant something.
///
/// `textSize` is the legacy glyph multiple, read only to migrate a file written
/// before the size became a pixel height (see `readEditor`).  Leaving it beside
/// the new `textPixels` would be a second, older answer to the same question,
/// and the next reader would have to guess which one wins.
void dropRetiredEditorKeys(QJsonObject &root)
{
    QJsonObject editor = root.value(QStringLiteral("editor")).toObject();
    if (editor.isEmpty()) {
        return;
    }
    if (editor.contains(QStringLiteral("textPixels"))) {
        editor.remove(QStringLiteral("textSize"));
        root.insert(QStringLiteral("editor"), editor);
    }
}

/// The `cli` leaves this build owns, as `(section, key)` pairs; `section` is
/// empty for a key directly under `cli`.
///
/// A plain merge cannot express "the user cleared this": the value the window
/// left empty is simply absent from the incoming object, and merging would put
/// the old one back.  So these leaves are dropped from the file before the
/// merge, which makes the incoming section authoritative for them — while every
/// key *not* listed here (a newer vshot's, or one added by hand, at any depth)
/// still survives.  The `editor` section needs no such list: the editor always
/// writes all of it.
const std::pair<const char *, const char *> kOwnedCliKeys[] = {
    {"", "png-compression"}, {"", "monitor"},
    {"long", "notches"},     {"long", "max-height"},
    {"long", "max-frames"},  {"long", "timeout"},
    {"long", "ignore-top"},  {"long", "inject"},
    {"pin", "density"},
};

/// Removes `key` from `object`, leaving `object` possibly empty for the caller
/// to prune.
void removeLeaf(QJsonObject &object, const QString &key)
{
    object.remove(key);
}

/// Writes the `cli` section: this build's leaves replaced, everything else —
/// including keys nested beside them that this build does not know — kept.
void writeCliSection(QJsonObject &root, const QJsonObject &cli)
{
    QJsonObject merged = root.value(QStringLiteral("cli")).toObject();
    for (const auto &[section, key] : kOwnedCliKeys) {
        const QString leaf = QString::fromLatin1(key);
        if (section[0] == '\0') {
            removeLeaf(merged, leaf);
            continue;
        }
        const QString parent = QString::fromLatin1(section);
        QJsonObject nested = merged.value(parent).toObject();
        nested.remove(leaf);
        if (nested.isEmpty()) {
            merged.remove(parent);
        } else {
            merged.insert(parent, nested);
        }
    }
    for (auto entry = cli.constBegin(); entry != cli.constEnd(); ++entry) {
        // A nested section merges into whatever survived above rather than
        // replacing it, so `long.future-key` is not lost to a save.
        if (entry.value().isObject()) {
            QJsonObject nested = merged.value(entry.key()).toObject();
            const QJsonObject incoming = entry.value().toObject();
            for (auto nestedEntry = incoming.constBegin(); nestedEntry != incoming.constEnd();
                 ++nestedEntry) {
                nested.insert(nestedEntry.key(), nestedEntry.value());
            }
            merged.insert(entry.key(), nested);
        } else {
            merged.insert(entry.key(), entry.value());
        }
    }
    if (merged.isEmpty()) {
        root.remove(QStringLiteral("cli"));
    } else {
        root.insert(QStringLiteral("cli"), merged);
    }
}

EditorPreferences readEditor(const QJsonObject &editor)
{
    EditorPreferences preferences;
    preferences.tool =
        readChoice(editor, QStringLiteral("tool"), preferences.tool, kToolNames);
    preferences.color = readColor(editor, QStringLiteral("color"), preferences.color);
    preferences.font = readString(editor, QStringLiteral("font"), preferences.font);
    preferences.width =
        readBounded(editor, QStringLiteral("width"), preferences.width, kMaxWidth);
    // The size is a pixel height, and that is what `textPixels` holds.
    //
    // A file written before this was true stores the legacy glyph multiple in
    // `textSize` instead; it is migrated on read rather than reinterpreted, so
    // a stored `2` (which meant 14 px) does not become two pixels and get
    // clamped up to the 7 px floor.  The key was renamed rather than reused
    // because the two meanings overlap on 7-64, where a value is a legal size
    // under either reading and no heuristic could tell them apart.
    if (editor.contains(QStringLiteral("textPixels"))) {
        preferences.textSize = readBounded(editor, QStringLiteral("textPixels"),
                                           preferences.textSize, kMaxTextPixels);
    } else {
        const QJsonValue legacy = editor.value(QStringLiteral("textSize"));
        if (legacy.isDouble() && std::isfinite(legacy.toDouble())) {
            const auto multiple = static_cast<long long>(legacy.toDouble());
            if (multiple >= 1) {
                preferences.textSize = static_cast<std::uint32_t>(
                    vshot::scaleToPixels(static_cast<std::uint32_t>(
                        std::min<long long>(multiple, kMaxTextPixels))));
            }
        }
    }
    preferences.dash = readChoice(editor, QStringLiteral("dash"), preferences.dash, kDashNames);
    preferences.arrowSize = readBounded(editor, QStringLiteral("arrowSize"),
                                        preferences.arrowSize, kMaxArrowSize);
    preferences.arrowStyle = readChoice(editor, QStringLiteral("arrowStyle"),
                                        preferences.arrowStyle, kArrowStyleNames);
    preferences.mosaicShape = readChoice(editor, QStringLiteral("mosaicShape"),
                                         preferences.mosaicShape, kMosaicShapeNames);
    preferences.mosaicStrength = readBounded(editor, QStringLiteral("mosaicStrength"),
                                             preferences.mosaicStrength, kMaxMosaicStrength);
    return preferences;
}

CliPreferences readCli(const QJsonObject &cli)
{
    CliPreferences preferences;
    preferences.pngCompression =
        readChoice(cli, QStringLiteral("png-compression"), QString(), kCompressionNames);
    preferences.monitor = readString(cli, QStringLiteral("monitor"), QString());
    const QJsonObject longSection = cli.value(QStringLiteral("long")).toObject();
    preferences.longInject =
        readChoice(longSection, QStringLiteral("inject"), QString(), kInjectNames);
    preferences.longNotches = readOptional(longSection, QStringLiteral("notches"), 1000);
    preferences.longMaxHeight = readOptional(longSection, QStringLiteral("max-height"), 1'000'000);
    preferences.longMaxFrames = readOptional(longSection, QStringLiteral("max-frames"), 1'000'000);
    preferences.longTimeout = readOptionalWide(longSection, QStringLiteral("timeout"), 86'400);
    preferences.longIgnoreTop = readOptional(longSection, QStringLiteral("ignore-top"), 100'000);

    const QJsonObject pinSection = cli.value(QStringLiteral("pin")).toObject();
    preferences.pinDensity = readOptional(pinSection, QStringLiteral("density"), kMaxDensity);
    return preferences;
}

/// The `editor` section as JSON.  Every field is written, because the editor's
/// style is a complete picture rather than a set of overrides.
QJsonObject editorJson(const EditorPreferences &preferences)
{
    QJsonObject editor;
    editor.insert(QStringLiteral("tool"), preferences.tool);
    editor.insert(QStringLiteral("color"), colorText(preferences.color));
    editor.insert(QStringLiteral("font"), preferences.font);
    editor.insert(QStringLiteral("width"), static_cast<double>(preferences.width));
    // `textPixels`, not the legacy `textSize`: the two name different things
    // (a pixel height against a glyph multiple) and a file carrying both
    // meanings under one key could not be read back correctly.  See readEditor.
    editor.insert(QStringLiteral("textPixels"), static_cast<double>(preferences.textSize));
    editor.insert(QStringLiteral("dash"), preferences.dash);
    editor.insert(QStringLiteral("arrowSize"), static_cast<double>(preferences.arrowSize));
    editor.insert(QStringLiteral("arrowStyle"), preferences.arrowStyle);
    editor.insert(QStringLiteral("mosaicShape"), preferences.mosaicShape);
    editor.insert(QStringLiteral("mosaicStrength"),
                  static_cast<double>(preferences.mosaicStrength));
    return editor;
}

/// The `cli` section as JSON.  Only the entries that carry a value are
/// written: an empty string or a zero is how the settings window says "let the
/// built-in default stand", and writing them out would freeze today's default
/// into the file.
QJsonObject cliJson(const CliPreferences &preferences)
{
    QJsonObject cli;
    if (!preferences.pngCompression.isEmpty()) {
        cli.insert(QStringLiteral("png-compression"), preferences.pngCompression);
    }
    if (!preferences.monitor.isEmpty()) {
        cli.insert(QStringLiteral("monitor"), preferences.monitor);
    }
    QJsonObject longSection;
    if (preferences.longNotches > 0) {
        longSection.insert(QStringLiteral("notches"), static_cast<double>(preferences.longNotches));
    }
    if (preferences.longMaxHeight > 0) {
        longSection.insert(QStringLiteral("max-height"),
                           static_cast<double>(preferences.longMaxHeight));
    }
    if (preferences.longMaxFrames > 0) {
        longSection.insert(QStringLiteral("max-frames"),
                           static_cast<double>(preferences.longMaxFrames));
    }
    if (preferences.longTimeout > 0) {
        longSection.insert(QStringLiteral("timeout"), static_cast<double>(preferences.longTimeout));
    }
    if (preferences.longIgnoreTop > 0) {
        longSection.insert(QStringLiteral("ignore-top"),
                           static_cast<double>(preferences.longIgnoreTop));
    }
    if (!preferences.longInject.isEmpty()) {
        longSection.insert(QStringLiteral("inject"), preferences.longInject);
    }
    if (!longSection.isEmpty()) {
        cli.insert(QStringLiteral("long"), longSection);
    }
    if (preferences.pinDensity > 0) {
        QJsonObject pinSection;
        pinSection.insert(QStringLiteral("density"), static_cast<double>(preferences.pinDensity));
        cli.insert(QStringLiteral("pin"), pinSection);
    }
    return cli;
}

bool writeRoot(const QJsonObject &root)
{
    const QString path = configFilePath();
    if (path.isEmpty()) {
        return false;
    }
    const QFileInfo info(path);
    QDir directory = info.absoluteDir();
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        return false;
    }
    // `QSaveFile` writes to a temporary and renames, so a crash mid-write
    // cannot leave a half-written file that the next run would refuse.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
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

QString colorText(const QColor &color)
{
    // `#rrggbb` when opaque, `#rrggbbaa` when not: the shorter form is what
    // people write by hand, and it matches what the editor's own hex field
    // accepts.
    return color.alpha() == 255 ? color.name(QColor::HexRgb) : colorTextWithAlpha(color);
}

QColor parseColorText(const QString &text)
{
    QString digits = text.trimmed();
    if (digits.startsWith(QLatin1Char('#'))) {
        digits.remove(0, 1);
    }
    const auto isHex = [](QChar character) {
        return (character >= QLatin1Char('0') && character <= QLatin1Char('9')) ||
               (character >= QLatin1Char('a') && character <= QLatin1Char('f')) ||
               (character >= QLatin1Char('A') && character <= QLatin1Char('F'));
    };
    for (const QChar character : digits) {
        if (!isHex(character)) {
            return QColor();
        }
    }
    const auto nibble = [&digits](int index) {
        return digits.mid(index, 1).toInt(nullptr, 16);
    };
    const auto byte = [&digits](int index) {
        return digits.mid(index * 2, 2).toInt(nullptr, 16);
    };
    switch (digits.size()) {
    case 3:
        // `#f80` is `#ff8800`, each digit doubled.  `QChar::digitValue` is not
        // used here: it answers only for decimal digits, so `f` would read as
        // -1 and the color would come out black.
        return QColor(nibble(0) * 17, nibble(1) * 17, nibble(2) * 17);
    case 6:
        return QColor(byte(0), byte(1), byte(2));
    case 8:
        return QColor(byte(0), byte(1), byte(2), byte(3));
    default:
        return QColor();
    }
}

/// The shadow, read out of one section.  The keys are spelled the same in the
/// `pin` and the `dialog` section, and every one of them is optional: an absent
/// key keeps the default, so a hand-written section only has to say what it
/// wants different.
///
/// A zero size is a real setting -- it means no blur reach at all -- but the
/// shadow's *off* state is the boolean, not a zero: that way a user can turn
/// the shadow off and back on without having lost the size they had tuned.
ShadowStyle readShadow(const QJsonObject &section, const ShadowStyle &fallback)
{
    ShadowStyle shadow = fallback;
    shadow.enabled = readFlag(section, QStringLiteral("shadow"), shadow.enabled);
    shadow.size = static_cast<int>(
        readWhole(section, QStringLiteral("shadowSize"), static_cast<std::uint32_t>(shadow.size),
                  kMaxShadowSize));
    // Signed, and the one value here that may be negative: a negative offset
    // puts the shadow above the shape instead of below it.
    shadow.offset = static_cast<int>(
        readSigned(section, QStringLiteral("shadowOffset"), shadow.offset, kMaxShadowOffset));
    shadow.opacity = static_cast<int>(
        readWhole(section, QStringLiteral("shadowOpacity"),
                  static_cast<std::uint32_t>(shadow.opacity), kMaxShadowOpacity));
    return shadow;
}

void writeShadow(QJsonObject &section, const ShadowStyle &shadow)
{
    section.insert(QStringLiteral("shadow"), shadow.enabled);
    section.insert(QStringLiteral("shadowSize"), shadow.size);
    section.insert(QStringLiteral("shadowOffset"), shadow.offset);
    section.insert(QStringLiteral("shadowOpacity"), shadow.opacity);
}

/// The `dialog` section of the file.  Like the editor's, it is written whole:
/// it describes a complete look rather than a set of overrides.
DialogPreferences readDialog(const QJsonObject &dialog)
{
    DialogPreferences preferences;
    preferences.radius = readBounded(dialog, QStringLiteral("radius"), preferences.radius,
                                     kMaxDialogRadius);
    preferences.borderWidth = readBounded(dialog, QStringLiteral("borderWidth"),
                                          preferences.borderWidth, kMaxDialogBorderWidth);
    // Absent, not merely unparseable, is what means "derive it": an invalid or
    // missing colour leaves the invalid QColor in place, and that is the
    // signal the dialog reads to pick a palette colour of its own.
    preferences.borderColor =
        readColor(dialog, QStringLiteral("borderColor"), preferences.borderColor);
    preferences.shadow = readShadow(dialog, preferences.shadow);
    return preferences;
}

QJsonObject dialogJson(const DialogPreferences &preferences)
{
    QJsonObject dialog;
    dialog.insert(QStringLiteral("radius"), static_cast<double>(preferences.radius));
    dialog.insert(QStringLiteral("borderWidth"),
                  static_cast<double>(preferences.borderWidth));
    // Only when it is a real colour: its absence is how the file says "derive
    // one from the palette", and writing a null or a placeholder would turn
    // that into a colour the dialog then has to invent a meaning for.
    if (preferences.borderColor.isValid()) {
        dialog.insert(QStringLiteral("borderColor"), colorText(preferences.borderColor));
    }
    writeShadow(dialog, preferences.shadow);
    return dialog;
}

/// The `pin` section of the file.  Written whole, like the other two: it
/// describes a complete look rather than a set of overrides, and a key the user
/// cleared has to leave the file rather than be merged back in.
PinPreferences readPin(const QJsonObject &pin)
{
    PinPreferences preferences;
    // The stored radius is a wish, clamped only against a ceiling that keeps a
    // nonsense value out of the file.  What a pin can actually carry depends on
    // its own size, which changes with every zoom step, so that clamp is
    // applied where the pin is drawn rather than here.
    preferences.radius =
        readWhole(pin, QStringLiteral("radius"), preferences.radius, kMaxPinRadius);
    preferences.shadow = readShadow(pin, preferences.shadow);
    preferences.borderWidth = readWhole(pin, QStringLiteral("borderWidth"),
                                        preferences.borderWidth, kMaxPinBorderWidth);
    // Invalid means "use the built-in colour", exactly as in the dialog's rim:
    // the stroke has a good default and the file only has to say something when
    // the user wants a different one.
    preferences.borderColor =
        readColor(pin, QStringLiteral("borderColor"), preferences.borderColor);
    preferences.activeBorderColor =
        readColor(pin, QStringLiteral("activeBorderColor"), preferences.activeBorderColor);
    return preferences;
}

QJsonObject pinJson(const PinPreferences &preferences)
{
    QJsonObject pin;
    pin.insert(QStringLiteral("radius"), static_cast<double>(preferences.radius));
    writeShadow(pin, preferences.shadow);
    pin.insert(QStringLiteral("borderWidth"), static_cast<double>(preferences.borderWidth));
    if (preferences.borderColor.isValid()) {
        pin.insert(QStringLiteral("borderColor"), colorText(preferences.borderColor));
    }
    if (preferences.activeBorderColor.isValid()) {
        pin.insert(QStringLiteral("activeBorderColor"),
                   colorText(preferences.activeBorderColor));
    }
    return pin;
}

Config loadConfig()
{
    Config config;
    const QJsonObject root = readRoot();
    config.editor = readEditor(root.value(QStringLiteral("editor")).toObject());
    config.cli = readCli(root.value(QStringLiteral("cli")).toObject());
    config.dialog = readDialog(root.value(QStringLiteral("dialog")).toObject());
    config.pin = readPin(root.value(QStringLiteral("pin")).toObject());
    return config;
}

bool saveConfig(const Config &config)
{
    if (configFilePath().isEmpty()) {
        return false;
    }
    QJsonObject root = readRoot();
    mergeSection(root, QStringLiteral("editor"), editorJson(config.editor));
    dropRetiredEditorKeys(root);
    writeCliSection(root, cliJson(config.cli));
    // Wholesale, like `editor`: the section is a complete look, and a key the
    // user removed by clearing the colour has to disappear rather than be
    // merged back in.
    root.insert(QStringLiteral("dialog"), dialogJson(config.dialog));
    root.insert(QStringLiteral("pin"), pinJson(config.pin));
    return writeRoot(root);
}

EditorPreferences loadEditorPreferences()
{
    return loadConfig().editor;
}

bool saveEditorPreferences(const EditorPreferences &preferences)
{
    if (configFilePath().isEmpty()) {
        return false;
    }
    QJsonObject root = readRoot();
    mergeSection(root, QStringLiteral("editor"), editorJson(preferences));
    dropRetiredEditorKeys(root);
    return writeRoot(root);
}

DialogPreferences loadDialogPreferences()
{
    return loadConfig().dialog;
}

PinPreferences loadPinPreferences()
{
    return loadConfig().pin;
}

/// The rim colour a pin uses in one of its two states: the user's own when the
/// file carries one, and otherwise the built-in.  A single reader for both
/// states, so "which colour is this pin" cannot be answered two ways.
QColor resolvePinBorderColor(const PinPreferences &preferences, bool active)
{
    const QColor &chosen = active ? preferences.activeBorderColor : preferences.borderColor;
    if (chosen.isValid()) {
        return chosen;
    }
    return active ? QColor(0, 0, 0) : QColor(192, 192, 192);
}

bool saveDialogPreferences(const DialogPreferences &preferences)
{
    if (configFilePath().isEmpty()) {
        return false;
    }
    QJsonObject root = readRoot();
    root.insert(QStringLiteral("dialog"), dialogJson(preferences));
    return writeRoot(root);
}

int resolveDialogRadius(const DialogPreferences &preferences, const QSize &size)
{
    // Half the shorter side is where a corner becomes a lozenge; one pixel
    // short of that keeps a rectangle a rectangle at any stored value.
    const int most = std::max(0, std::min(size.width(), size.height()) / 2 - 1);
    return std::min(static_cast<int>(preferences.radius), most);
}

QColor resolveDialogBorderColor(const DialogPreferences &preferences, const QColor &surface,
                                const QColor &text)
{
    if (preferences.borderColor.isValid()) {
        return preferences.borderColor;
    }
    // A third of the way from the dialog's own colour to its text: darker on a
    // light scheme, lighter on a dark one, so the edge reads as an edge either
    // way without a second constant to keep in step.
    const auto blend = [](const QColor &from, const QColor &to, double amount) {
        return QColor(qRound(from.red() * (1.0 - amount) + to.red() * amount),
                      qRound(from.green() * (1.0 - amount) + to.green() * amount),
                      qRound(from.blue() * (1.0 - amount) + to.blue() * amount));
    };
    return blend(surface, text, 0.35);
}

const QStringList &toolNames()
{
    return kToolNames;
}

const QStringList &dashNames()
{
    return kDashNames;
}

const QStringList &arrowStyleNames()
{
    return kArrowStyleNames;
}

const QStringList &mosaicShapeNames()
{
    return kMosaicShapeNames;
}

const QStringList &compressionNames()
{
    return kCompressionNames;
}

const QStringList &injectNames()
{
    return kInjectNames;
}

} // namespace vshot

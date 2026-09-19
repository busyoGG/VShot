// Offline check for the shared config file: what a save does to the parts of
// the file it did not write.
//
// This is where the interesting failures live. Reading is forgiving by design
// (a missing or malformed file is the built-in defaults), but writing is not:
// the annotation editor saves its style every time a session ends, and the
// settings window saves both sections at once. If either save replaced the file
// instead of merging into it, one section would silently wipe the other — and
// the user would only find out when their defaults stopped applying.
//
// The checks run against a config file in a temporary directory, which is what
// makes the whole thing honest: XDG_CONFIG_HOME is what both vshot's own
// config.rs and this file resolve their path from, so setting it to a scratch
// directory exercises the real path, the real parse and the real write. No
// compositor, no widgets: Qt Core is enough.
//
// Built only with `-DVSHOT_BUILD_CHECKS=ON`.

#include "config.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;

void expect(bool ok, const char *what, const QString &detail = QString())
{
    if (ok) {
        std::printf("ok    %-56s %s\n", what, qPrintable(detail));
        return;
    }
    std::printf("FAIL  %-56s %s\n", what, qPrintable(detail));
    ++failures;
}

QString configPath()
{
    return vshot::configFilePath();
}

void writeConfig(const QString &text)
{
    QDir().mkpath(QFileInfo(configPath()).absolutePath());
    QFile file(configPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::printf("FAIL  could not write the probe config at %s\n",
                    qPrintable(configPath()));
        ++failures;
        return;
    }
    file.write(text.toUtf8());
}

QJsonObject readConfig()
{
    QFile file(configPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return QJsonObject();
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError) {
        return QJsonObject();
    }
    return document.object();
}

/// Walks a `/`-separated path through nested objects, one segment per level.
QJsonValue valueAt(const QJsonObject &object, const char *path)
{
    QJsonValue value = object;
    const QStringList segments = QString::fromLatin1(path).split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        if (!value.isObject()) {
            return QJsonValue();
        }
        value = value.toObject().value(segment);
    }
    return value;
}

QString textAt(const QJsonObject &object, const char *path)
{
    return valueAt(object, path).toString();
}

double numberAt(const QJsonObject &object, const char *path)
{
    return valueAt(object, path).toDouble();
}

/// Whether a `/`-separated path exists at all, which is how "the section was
/// pruned rather than left empty" is told apart from "it is there but empty".
bool containsAt(const QJsonObject &object, const char *path)
{
    QJsonValue value = object;
    const QStringList segments = QString::fromLatin1(path).split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        if (!value.isObject() || !value.toObject().contains(segment)) {
            return false;
        }
        value = value.toObject().value(segment);
    }
    return true;
}

/// Runs one save and reports the file it produced.
QJsonObject afterSave(const std::function<void()> &save)
{
    save();
    return readConfig();
}

void checkDefaultsWhenTheFileIsMissing()
{
    std::printf("--- a missing file is the built-in defaults ------------------------\n");
    QFile::remove(configPath());
    const vshot::Config config = vshot::loadConfig();
    expect(config.editor.tool == QStringLiteral("select"),
           "the editor tool falls back to select", config.editor.tool);
    expect(config.editor.color == QColor(255, 64, 64, 255), "the color falls back to #ff4040ff",
           config.editor.color.name(QColor::HexArgb));
    expect(config.editor.width == 2, "the width falls back to 2");
    expect(config.cli.pngCompression.isEmpty(), "no compression default is remembered");
    expect(config.cli.longTimeout == 0, "no scroll timeout is remembered");
    expect(config.cli.pinDensity == 0, "no pin density is remembered");
}

void checkUnknownKeysAreIgnored()
{
    std::printf("--- an unknown key costs only itself ------------------------------\n");
    // This is the shape a newer vshot writes, or a user's own note. Refusing
    // the whole section over one unrecognized name would drop every default in
    // it, which is exactly what used to happen on the CLI side.
    writeConfig(QStringLiteral(R"({
        "cli": {"png-compression": "high", "not-a-vshot-key": 7,
                "long": {"notches": 3, "also-not-mine": true}}
    })"));
    const vshot::Config config = vshot::loadConfig();
    expect(config.cli.pngCompression == QStringLiteral("high"),
           "a known key beside an unknown one still reads", config.cli.pngCompression);
    expect(config.cli.longNotches == 3, "a known nested key still reads");
    expect(config.editor.tool == QStringLiteral("select"),
           "the absent editor section is still the defaults");
}

void checkBadValuesFallBackFieldByField()
{
    std::printf("--- one bad value costs only its own field -------------------------\n");
    writeConfig(QStringLiteral(R"({
        "editor": {"tool": "scribble", "width": 900, "dash": "dashed", "color": "not-a-color"},
        "cli": {"png-compression": "slowest", "monitor": "DP-3"}
    })"));
    const vshot::Config config = vshot::loadConfig();
    expect(config.editor.tool == QStringLiteral("select"),
           "an unknown tool name falls back", config.editor.tool);
    expect(config.editor.width == 64, "an out-of-range width is clamped", QString::number(config.editor.width));
    expect(config.editor.dash == QStringLiteral("dashed"), "a good value beside bad ones survives");
    expect(config.editor.color == QColor(255, 64, 64, 255), "an unparseable color falls back");
    expect(config.cli.pngCompression.isEmpty(), "an unknown compression name falls back");
    expect(config.cli.monitor == QStringLiteral("DP-3"), "a good monitor name survives");
}

void checkEditorSaveKeepsTheCliSection()
{
    std::printf("--- saving the editor style keeps the CLI section ------------------\n");
    writeConfig(QStringLiteral(R"({
        "editor": {"tool": "arrow", "color": "#00ff00"},
        "cli": {"png-compression": "high", "monitor": "DP-2"},
        "some-future-section": {"keep": true}
    })"));
    vshot::EditorPreferences editor;
    editor.tool = QStringLiteral("mosaic");
    editor.color = QColor(0, 128, 255);
    editor.width = 7;

    const QJsonObject root = afterSave([&] { vshot::saveEditorPreferences(editor); });
    expect(textAt(root, "cli/png-compression") == QStringLiteral("high"),
           "the compression default is still there", textAt(root, "cli/png-compression"));
    expect(textAt(root, "cli/monitor") == QStringLiteral("DP-2"),
           "the monitor default is still there");
    expect(textAt(root, "some-future-section/keep").isEmpty() &&
               containsAt(root, "some-future-section"),
           "an unknown section is kept whole");
    expect(textAt(root, "editor/tool") == QStringLiteral("mosaic"),
           "the editor's new tool was written");
    expect(textAt(root, "editor/dash") == QStringLiteral("solid"),
           "the editor writes its whole section, defaults included");
    expect(numberAt(root, "editor/width") == 7, "the editor's new width was written");
}

void checkSettingsSaveKeepsWhatItDoesNotOwn()
{
    std::printf("--- saving both sections keeps what this build does not own --------\n");
    writeConfig(QStringLiteral(R"({
        "editor": {"tool": "arrow"},
        "cli": {"png-compression": "high", "future-key": 7,
                "long": {"notches": 2, "future-long-key": 9},
                "pin": {"density": 2, "future-pin-key": 3}}
    })"));
    vshot::Config config = vshot::loadConfig();
    config.cli.pngCompression = QStringLiteral("balanced");
    config.cli.monitor = QStringLiteral("DP-3");
    config.cli.longNotches = 5;
    config.cli.pinDensity = 0; // cleared in the window

    const QJsonObject root = afterSave([&] { vshot::saveConfig(config); });
    expect(textAt(root, "cli/png-compression") == QStringLiteral("balanced"),
           "the changed compression default was written");
    expect(textAt(root, "cli/monitor") == QStringLiteral("DP-3"), "the new monitor default was written");
    expect(numberAt(root, "cli/long/notches") == 5, "the changed notches default was written");
    expect(numberAt(root, "cli/future-key") == 7, "an unknown top-level cli key is kept");
    expect(numberAt(root, "cli/long/future-long-key") == 9, "an unknown nested cli key is kept");
    expect(numberAt(root, "cli/pin/future-pin-key") == 3,
           "an unknown pin key is kept beside a cleared density");
}

void checkClearingAValueRemovesIt()
{
    std::printf("--- clearing a value actually clears it ----------------------------\n");
    writeConfig(QStringLiteral(R"({
        "cli": {"png-compression": "high", "monitor": "DP-2",
                "long": {"notches": 2, "max-height": 9000, "timeout": 30},
                "pin": {"density": 2}}
    })"));
    // What the settings window produces when the user picks the built-in
    // default everywhere: every owned value absent.
    vshot::Config config = vshot::loadConfig();
    config.cli = vshot::CliPreferences{};
    const QJsonObject root = afterSave([&] { vshot::saveConfig(config); });

    expect(!root.contains(QStringLiteral("cli")),
           "clearing everything leaves no empty cli section behind");
    const vshot::Config reread = vshot::loadConfig();
    expect(reread.cli.pngCompression.isEmpty() && reread.cli.longNotches == 0 &&
               reread.cli.pinDensity == 0,
           "the cleared defaults read back as unset");
}

void checkRoundTripOfEveryField()
{
    std::printf("--- every field survives a write and a read ------------------------\n");
    QFile::remove(configPath());
    vshot::Config written;
    written.editor.tool = QStringLiteral("text");
    written.editor.color = QColor(12, 34, 56, 200);
    written.editor.font = QStringLiteral("Noto Sans");
    written.editor.width = 9;
    written.editor.textSize = 11;
    written.editor.dash = QStringLiteral("dotted");
    written.editor.arrowSize = 4;
    written.editor.arrowStyle = QStringLiteral("filled");
    written.editor.mosaicShape = QStringLiteral("brush");
    written.editor.mosaicStrength = 3;
    written.cli.pngCompression = QStringLiteral("fastest");
    written.cli.monitor = QStringLiteral("HDMI-A-1");
    written.cli.longNotches = 4;
    written.cli.longMaxHeight = 12345;
    written.cli.longMaxFrames = 678;
    written.cli.longTimeout = 99;
    written.cli.longIgnoreTop = 42;
    written.cli.longInject = QStringLiteral("uinput");
    written.cli.pinDensity = 3;

    vshot::saveConfig(written);
    const vshot::Config read = vshot::loadConfig();

    expect(read.editor.tool == written.editor.tool, "editor.tool round-trips", read.editor.tool);
    expect(read.editor.color == written.editor.color, "editor.color round-trips",
           read.editor.color.name(QColor::HexArgb));
    expect(read.editor.font == written.editor.font, "editor.font round-trips");
    expect(read.editor.width == written.editor.width, "editor.width round-trips");
    expect(read.editor.textSize == written.editor.textSize, "editor.textSize round-trips");
    expect(read.editor.dash == written.editor.dash, "editor.dash round-trips");
    expect(read.editor.arrowSize == written.editor.arrowSize, "editor.arrowSize round-trips");
    expect(read.editor.arrowStyle == written.editor.arrowStyle, "editor.arrowStyle round-trips");
    expect(read.editor.mosaicShape == written.editor.mosaicShape, "editor.mosaicShape round-trips");
    expect(read.editor.mosaicStrength == written.editor.mosaicStrength,
           "editor.mosaicStrength round-trips");
    expect(read.cli.pngCompression == written.cli.pngCompression, "cli.png-compression round-trips");
    expect(read.cli.monitor == written.cli.monitor, "cli.monitor round-trips");
    expect(read.cli.longNotches == written.cli.longNotches, "cli.long.notches round-trips");
    expect(read.cli.longMaxHeight == written.cli.longMaxHeight, "cli.long.max-height round-trips");
    expect(read.cli.longMaxFrames == written.cli.longMaxFrames, "cli.long.max-frames round-trips");
    expect(read.cli.longTimeout == written.cli.longTimeout, "cli.long.timeout round-trips");
    expect(read.cli.longIgnoreTop == written.cli.longIgnoreTop, "cli.long.ignore-top round-trips");
    expect(read.cli.longInject == written.cli.longInject, "cli.long.inject round-trips");
    expect(read.cli.pinDensity == written.cli.pinDensity, "cli.pin.density round-trips");

    // The names the settings window offers have to be the names the loader
    // accepts, or a value picked in the UI would be silently dropped on the
    // next read.
    for (const QString &value : vshot::compressionNames()) {
        vshot::Config probe = written;
        probe.cli.pngCompression = value;
        vshot::saveConfig(probe);
        expect(vshot::loadConfig().cli.pngCompression == value,
               "the settings window's compression names all load back", value);
    }
    for (const QString &value : vshot::toolNames()) {
        vshot::Config probe = written;
        probe.editor.tool = value;
        vshot::saveConfig(probe);
        expect(vshot::loadConfig().editor.tool == value,
               "the settings window's tool names all load back", value);
    }
    for (const QString &value : vshot::injectNames()) {
        vshot::Config probe = written;
        probe.cli.longInject = value;
        vshot::saveConfig(probe);
        expect(vshot::loadConfig().cli.longInject == value,
               "the settings window's scroll backends all load back", value);
    }
}

void checkColorsUseTheCssSpelling()
{
    std::printf("--- colors are the spelling a person writes ------------------------\n");
    // Qt reads `#rrggbbaa` as `#aarrggbb`, so a value the README documents as
    // opaque orange would come out purple if the parse were handed to QColor.
    const QColor orange = vshot::parseColorText(QStringLiteral("#ff8800ff"));
    expect(orange == QColor(255, 136, 0, 255), "an eight-digit value is #rrggbbaa",
           orange.name(QColor::HexArgb));
    expect(vshot::parseColorText(QStringLiteral("#ff8800")) == QColor(255, 136, 0),
           "a six-digit value is opaque");
    expect(vshot::parseColorText(QStringLiteral("#f80")) == QColor(255, 136, 0),
           "a three-digit value expands");
    expect(vshot::parseColorText(QStringLiteral("#ff880080")) == QColor(255, 136, 0, 128),
           "the alpha is the last pair");
    expect(!vshot::parseColorText(QStringLiteral("not-a-color")).isValid(),
           "a non-color is invalid rather than black");
    expect(!vshot::parseColorText(QStringLiteral("#12345")).isValid(),
           "a five-digit value is invalid");
    expect(!vshot::parseColorText(QStringLiteral("#gggggg")).isValid(),
           "non-hex digits are invalid");

    // And what is written back has to be what was read, in the same spelling.
    expect(vshot::colorText(QColor(255, 136, 0)) == QStringLiteral("#ff8800"),
           "an opaque color writes six digits");
    expect(vshot::colorText(QColor(255, 136, 0, 128)) == QStringLiteral("#ff880080"),
           "a translucent color writes the alpha last");

    // The round trip through the file is what actually matters.
    QFile::remove(configPath());
    vshot::Config written;
    written.editor.color = QColor(255, 136, 0, 128);
    vshot::saveConfig(written);
    expect(vshot::loadConfig().editor.color == QColor(255, 136, 0, 128),
           "a translucent color survives the file");

    writeConfig(QStringLiteral(R"({"editor": {"color": "#ff8800ff"}})"));
    expect(vshot::loadConfig().editor.color == QColor(255, 136, 0, 255),
           "the documented spelling in a hand-written file reads as written",
           vshot::loadConfig().editor.color.name(QColor::HexArgb));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // The scratch directory has to be in place before anything resolves the
    // path, which is why the environment is set here rather than passed in.
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        std::printf("FAIL  no temporary directory to run in\n");
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", scratch.path().toUtf8());
    std::printf("config: %s\n", qPrintable(configPath()));
    if (configPath().isEmpty() || !configPath().startsWith(scratch.path())) {
        std::printf("FAIL  the config path does not follow XDG_CONFIG_HOME\n");
        return 1;
    }

    checkDefaultsWhenTheFileIsMissing();
    checkUnknownKeysAreIgnored();
    checkBadValuesFallBackFieldByField();
    checkColorsUseTheCssSpelling();
    checkEditorSaveKeepsTheCliSection();
    checkSettingsSaveKeepsWhatItDoesNotOwn();
    checkClearingAValueRemovesIt();
    checkRoundTripOfEveryField();

    std::printf("--- result ---------------------------------------------------------\n");
    std::printf("%s (%d failure(s))\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}

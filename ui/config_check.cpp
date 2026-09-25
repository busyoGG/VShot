// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

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
    expect(config.cli.recordEncoder.isEmpty(), "no encoder default is remembered");
    expect(config.cli.recordFps == 0, "no frame rate default is remembered");
    expect(!config.cli.recordPortal, "the portal is off unless the file says otherwise");
    expect(!config.cli.recordMicEnabled,
           "a recording is silent unless the file asks for a microphone");
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
                "pin": {"density": 2, "future-pin-key": 3}},
        "dialog": {"radius": 7, "future-dialog-key": 5}
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
    // The `dialog` and `pin` sections are written whole, so a key this build
    // does not know is *not* kept in them -- unlike `cli`, which is merged leaf
    // by leaf.  Saying so here is what stops a later change from quietly making
    // the two behave the same way in one direction or the other.
    expect(!root.contains(QStringLiteral("dialog")) ||
               !root.value(QStringLiteral("dialog"))
                    .toObject()
                    .contains(QStringLiteral("future-dialog-key")),
           "a section written whole does not carry unknown keys forward");
}

void checkClearingAValueRemovesIt()
{
    std::printf("--- clearing a value actually clears it ----------------------------\n");
    writeConfig(QStringLiteral(R"({
        "cli": {"png-compression": "high", "monitor": "DP-2",
                "long": {"notches": 2, "max-height": 9000, "timeout": 30},
                "pin": {"density": 2},
                "record": {"encoder": "hevc", "fps": 30, "portal": true, "mic": ""}}
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
               reread.cli.pinDensity == 0 && reread.cli.recordEncoder.isEmpty() &&
               reread.cli.recordFps == 0 && !reread.cli.recordPortal &&
               !reread.cli.recordMicEnabled,
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
    // A font height in pixels, and deliberately not a whole glyph multiple:
    // the config file must not snap it to one.
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
    written.cli.recordEncoder = QStringLiteral("hevc");
    written.cli.recordFps = 120;
    written.cli.recordPortal = true;
    written.cli.recordMicEnabled = true;
    written.cli.recordMic = QStringLiteral("alsa_input.pci-0000_2f_00.4.analog-stereo");
    written.pin.radius = 12;
    written.pin.shadow.enabled = false;
    written.pin.shadow.size = 21;
    written.pin.shadow.offset = -7;
    written.pin.shadow.opacity = 200;
    written.dialog.shadow.enabled = true;
    written.dialog.shadow.size = 9;
    written.dialog.shadow.offset = 5;
    written.dialog.shadow.opacity = 77;
    written.pin.borderWidth = 3;
    written.pin.borderColor = QColor(1, 2, 3, 255);
    written.pin.activeBorderColor = QColor(4, 5, 6, 200);

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
    expect(read.cli.recordEncoder == written.cli.recordEncoder,
           "cli.record.encoder round-trips", read.cli.recordEncoder);
    expect(read.cli.recordFps == written.cli.recordFps, "cli.record.fps round-trips",
           QString::number(read.cli.recordFps));
    expect(read.cli.recordPortal, "cli.record.portal round-trips");
    expect(read.cli.recordMicEnabled && read.cli.recordMic == written.cli.recordMic,
           "cli.record.mic round-trips", read.cli.recordMic);
    expect(read.pin.radius == written.pin.radius, "pin.radius round-trips",
           QString::number(read.pin.radius));
    expect(read.pin.shadow.enabled == written.pin.shadow.enabled,
           "pin.shadow round-trips");
    expect(read.pin.shadow.size == written.pin.shadow.size &&
               read.pin.shadow.offset == written.pin.shadow.offset &&
               read.pin.shadow.opacity == written.pin.shadow.opacity,
           "the pin shadow's size, offset and opacity round-trip",
           QStringLiteral("%1/%2/%3")
               .arg(read.pin.shadow.size)
               .arg(read.pin.shadow.offset)
               .arg(read.pin.shadow.opacity));
    expect(read.dialog.shadow.enabled == written.dialog.shadow.enabled &&
               read.dialog.shadow.size == written.dialog.shadow.size &&
               read.dialog.shadow.offset == written.dialog.shadow.offset &&
               read.dialog.shadow.opacity == written.dialog.shadow.opacity,
           "the dialog shadow round-trips, and is not the pin's",
           QStringLiteral("%1/%2/%3/%4")
               .arg(read.dialog.shadow.enabled ? 1 : 0)
               .arg(read.dialog.shadow.size)
               .arg(read.dialog.shadow.offset)
               .arg(read.dialog.shadow.opacity));
    expect(read.pin.shadow.size != read.dialog.shadow.size,
           "the two sections' shadows are read apart");
    expect(read.pin.borderWidth == written.pin.borderWidth, "pin.borderWidth round-trips",
           QString::number(read.pin.borderWidth));
    expect(read.pin.borderColor == written.pin.borderColor, "pin.borderColor round-trips",
           read.pin.borderColor.name(QColor::HexArgb));
    expect(read.pin.activeBorderColor == written.pin.activeBorderColor,
           "pin.activeBorderColor round-trips",
           read.pin.activeBorderColor.name(QColor::HexArgb));
    // Zero is a real setting for both of these -- square corners, no rim -- and
    // it has to come back as zero rather than as the built-in default.
    {
        vshot::Config bare = written;
        bare.pin.radius = 0;
        bare.pin.borderWidth = 0;
        bare.pin.shadow.enabled = true;
        vshot::saveConfig(bare);
        const vshot::Config zeroed = vshot::loadConfig();
        expect(zeroed.pin.radius == 0 && zeroed.pin.borderWidth == 0 &&
                   zeroed.pin.shadow.enabled,
               "a written zero is a setting, not an absent one",
               QStringLiteral("radius %1, width %2")
                   .arg(zeroed.pin.radius)
                   .arg(zeroed.pin.borderWidth));
    }

    // The microphone key has two spellings that are not the same thing: no key
    // at all is silence, and the empty string is the session's default input. A
    // save has to keep them apart.  A frame rate outside the range `vshot
    // record --fps` takes reads as "the file said nothing" rather than as a
    // clamped rate, because that is what the CLI does with one.
    {
        vshot::Config probe = written;
        probe.cli.recordMicEnabled = true;
        probe.cli.recordMic.clear();
        probe.cli.recordFps = 400;
        vshot::saveConfig(probe);
        const vshot::Config defaulted = vshot::loadConfig();
        expect(defaulted.cli.recordMicEnabled && defaulted.cli.recordMic.isEmpty(),
               "the session's default input is a written empty string",
               defaulted.cli.recordMic.isEmpty() ? QStringLiteral("empty")
                                                 : defaulted.cli.recordMic);
        expect(defaulted.cli.recordFps == 0, "a frame rate out of range reads as unset",
               QString::number(defaulted.cli.recordFps));

        probe.cli.recordMicEnabled = false;
        vshot::saveConfig(probe);
        expect(!vshot::loadConfig().cli.recordMicEnabled,
               "silence is the absent key, not an empty one");
    }

    // The names the settings window offers have to be the names the loader
    // accepts, or a value picked in the UI would be silently dropped on the
    // next read.
    for (const QString &value : vshot::encoderNames()) {
        vshot::Config probe = written;
        probe.cli.recordEncoder = value;
        vshot::saveConfig(probe);
        expect(vshot::loadConfig().cli.recordEncoder == value,
               "the settings window's encoder names all load back", value);
    }
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

void checkTheLegacyTextSizeIsMigrated()
{
    std::printf("--- an old textSize is migrated, not reinterpreted ---------------\n");
    // The size used to be the legacy glyph multiple (1-64); it is now a pixel
    // height (7-448).  A file written before that stores `2` meaning 14 px, and
    // reading it as two pixels would clamp it up to the 7 px floor -- a label
    // the user set at 14 px would silently shrink.  So the old key is
    // converted.  This matters because the two meanings overlap on 7-64, where
    // a value is legal under either reading and no heuristic could separate
    // them, which is why the new size lives under a different key.
    writeConfig(QStringLiteral(R"({"editor": {"textSize": 2}})"));
    expect(vshot::loadConfig().editor.textSize == 14,
           "the old default 2 reads as the 14 px it meant",
           QString::number(vshot::loadConfig().editor.textSize));

    writeConfig(QStringLiteral(R"({"editor": {"textSize": 3}})"));
    expect(vshot::loadConfig().editor.textSize == 21, "an old 3 reads as 21 px",
           QString::number(vshot::loadConfig().editor.textSize));

    writeConfig(QStringLiteral(R"({"editor": {"textSize": 64}})"));
    expect(vshot::loadConfig().editor.textSize == 448, "the old maximum reads as 448 px",
           QString::number(vshot::loadConfig().editor.textSize));

    // The new key wins, so a file mid-write or hand-edited with both keys is
    // read the way this build meant it.
    writeConfig(QStringLiteral(R"({"editor": {"textPixels": 30, "textSize": 2}})"));
    expect(vshot::loadConfig().editor.textSize == 30,
           "the pixel key wins when a file carries both",
           QString::number(vshot::loadConfig().editor.textSize));

    // And the retired key is dropped on the next save, so the file does not
    // keep a second, older answer to the same question.
    writeConfig(QStringLiteral(R"({"editor": {"textSize": 3}})"));
    vshot::Config config = vshot::loadConfig();
    vshot::saveConfig(config);
    const QJsonObject root = readConfig();
    expect(!containsAt(root, "editor/textSize"),
           "the retired key is gone after a save");
    expect(numberAt(root, "editor/textPixels") == 21, "the migrated size was written",
           QString::number(numberAt(root, "editor/textPixels")));
    expect(vshot::loadConfig().editor.textSize == 21, "and it reads back unchanged");

    // A pixel value that happens to sit in the old range must survive: writing
    // 30 px then reading it must not turn it into 210 px.
    writeConfig(QStringLiteral(R"({"editor": {"textPixels": 30}})"));
    expect(vshot::loadConfig().editor.textSize == 30,
           "a pixel size in the old range is not multiplied",
           QString::number(vshot::loadConfig().editor.textSize));
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
    checkTheLegacyTextSizeIsMigrated();
    checkEditorSaveKeepsTheCliSection();
    checkSettingsSaveKeepsWhatItDoesNotOwn();
    checkClearingAValueRemovesIt();
    checkRoundTripOfEveryField();

    std::printf("--- result ---------------------------------------------------------\n");
    std::printf("%s (%d failure(s))\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}

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
#include "shortcuts.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeySequence>
#include <QString>
#include <QTemporaryDir>

#include <cmath>
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

bool booleanAt(const QJsonObject &object, const char *path)
{
    return valueAt(object, path).toBool();
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
    expect(config.editor.tool.isEmpty(),
           "the editor opens with no tool armed", config.editor.tool);
    expect(config.editor.selectMode == QStringLiteral("precise"),
           "a press has to land on the mark unless the file says otherwise",
           config.editor.selectMode);
    expect(config.editor.color == QColor(255, 64, 64, 255), "the color falls back to #ff4040ff",
           config.editor.color.name(QColor::HexArgb));
    expect(config.editor.width == 2, "the width falls back to 2");
    expect(config.cli.pngCompression.isEmpty(), "no compression default is remembered");
    expect(config.cli.longTimeout == 0, "no scroll timeout is remembered");
    expect(config.cli.pinDensity == 0, "no pin density is remembered");
    expect(config.cli.recordEncoder.isEmpty(), "no encoder default is remembered");
    expect(config.cli.recordEncoderBackend.isEmpty(),
           "no encoder-backend default is remembered");
    expect(config.cli.recordFps == 0, "no frame rate default is remembered");
    expect(!config.cli.recordPortal, "the portal is off unless the file says otherwise");
    expect(!config.cli.recordMicEnabled,
           "a recording is silent unless the file asks for a microphone");
    expect(config.cli.recordFollow.isEmpty(), "nothing is followed unless the file says so");
    expect(config.cli.recordNotify, "the recording notification is on unless the file says off");
    expect(config.cli.replayWindow == 0, "no replay window is remembered");
    expect(config.cli.replayGop == 0, "no replay gop is remembered");
    expect(config.cli.replayEncoder.isEmpty(), "no replay encoder is remembered");
    expect(config.cli.replayEncoderBackend.isEmpty(),
           "no replay encoder-backend is remembered");
    expect(config.cli.replayFps == 0, "no replay frame rate is remembered");
    expect(!config.cli.replayPortal, "the replay portal is off unless the file says otherwise");
    expect(!config.cli.replayMicEnabled,
           "a replay is silent unless the file asks for a microphone");
    expect(config.cli.replayFollow.isEmpty(), "a replay follows nothing unless the file says so");
    expect(config.cli.replaySaveDir.isEmpty(), "no replay save directory is remembered");
    expect(config.cli.replayNotify, "the replay notification is on unless the file says off");
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
    expect(config.editor.tool.isEmpty(),
           "the absent editor section is still the defaults");
}

void checkBadValuesFallBackFieldByField()
{
    std::printf("--- one bad value costs only its own field -------------------------\n");
    writeConfig(QStringLiteral(R"({
        "editor": {"tool": "scribble", "width": 900, "dash": "dashed", "color": "not-a-color",
                   "selectMode": "sometimes"},
        "cli": {"png-compression": "slowest", "monitor": "DP-3"}
    })"));
    const vshot::Config config = vshot::loadConfig();
    expect(config.editor.tool.isEmpty(),
           "an unknown tool name falls back to nothing armed", config.editor.tool);
    expect(config.editor.selectMode == QStringLiteral("precise"),
           "an unknown select mode falls back", config.editor.selectMode);
    expect(config.editor.width == 64, "an out-of-range width is clamped", QString::number(config.editor.width));
    expect(config.editor.dash == QStringLiteral("dashed"), "a good value beside bad ones survives");
    expect(config.editor.color == QColor(255, 64, 64, 255), "an unparseable color falls back");
    expect(config.cli.pngCompression.isEmpty(), "an unknown compression name falls back");
    expect(config.cli.monitor == QStringLiteral("DP-3"), "a good monitor name survives");
}

// A config file written before the Select tool went away names it, and the state
// it named is the one the editor still opens in: nothing armed.  Reading the
// name as a typo would quietly move the user onto the first tool in the list.
void checkTheRetiredSelectToolStillMeansNothingArmed()
{
    std::printf("--- the retired select tool still means nothing armed ---------------\n");
    writeConfig(QStringLiteral(R"({"editor": {"tool": "select"}})"));
    expect(vshot::loadConfig().editor.tool.isEmpty(),
           "a config naming the retired Select tool opens unarmed",
           vshot::loadConfig().editor.tool);
}

void checkTheHdrFormatRoundTripsAndIsCleared()
{
    std::printf("--- the HDR format is remembered and can be cleared ----------------\n");
    // The HDR half's format, on the same terms as the compression level: an
    // absent key or an empty value means "let the built-in default stand", and
    // only a name this build knows is read at all.
    writeConfig(QStringLiteral(R"({"cli": {"hdr-format": "hdr", "future": 1}})"));
    expect(vshot::loadConfig().cli.hdrFormat == QStringLiteral("hdr"),
           "a known HDR format name is read", vshot::loadConfig().cli.hdrFormat);

    writeConfig(QStringLiteral(R"({"cli": {"hdr-format": "webp", "future": 1}})"));
    expect(vshot::loadConfig().cli.hdrFormat.isEmpty(),
           "an unknown HDR format name falls back to the built-in default");

    vshot::Config config = vshot::loadConfig();
    config.cli.hdrFormat = QStringLiteral("avif");
    QJsonObject root = afterSave([&] { vshot::saveConfig(config); });
    expect(textAt(root, "cli/hdr-format") == QStringLiteral("avif"),
           "the chosen HDR format was written", textAt(root, "cli/hdr-format"));
    expect(numberAt(root, "cli/future") == 1, "an unknown key beside it is still kept");

    config.cli.hdrFormat.clear();
    root = afterSave([&] { vshot::saveConfig(config); });
    expect(textAt(root, "cli/hdr-format").isEmpty() && !containsAt(root, "cli/hdr-format"),
           "clearing it removes the key rather than writing an empty one");
}

void checkTheToneMapRoundTripsAndIsCleared()
{
    std::printf("--- the tone map is remembered and can be cleared ------------------\n");
    // The map down to SDR, on the same terms as the format above: an absent key
    // means "let the built-in default stand", and only a name this build knows
    // is read at all.
    writeConfig(QStringLiteral(R"({"cli": {"tone-map": "normalize", "tone-map-white": 0.72}})"));
    expect(vshot::loadConfig().cli.toneMap == QStringLiteral("normalize"),
           "a known tone-map name is read", vshot::loadConfig().cli.toneMap);
    expect(std::abs(vshot::loadConfig().cli.toneMapWhite - 0.72) < 1e-6,
           "the white level is read", QString::number(vshot::loadConfig().cli.toneMapWhite));

    writeConfig(QStringLiteral(R"({"cli": {"tone-map": "soft-knee"}})"));
    expect(vshot::loadConfig().cli.toneMap.isEmpty(),
           "an unknown tone-map name falls back to the built-in default");

    // A level outside the span the map accepts is clamped rather than dropped:
    // it is a number the user meant, and the map has a defined answer for it.
    writeConfig(QStringLiteral(R"({"cli": {"tone-map-white": 0.2}})"));
    expect(std::abs(vshot::loadConfig().cli.toneMapWhite - vshot::kMinToneMapWhite) < 1e-6,
           "a level below the span is clamped up",
           QString::number(vshot::loadConfig().cli.toneMapWhite));
    writeConfig(QStringLiteral(R"({"cli": {"tone-map-white": 4}})"));
    expect(std::abs(vshot::loadConfig().cli.toneMapWhite - vshot::kMaxToneMapWhite) < 1e-6,
           "a level above the span is clamped down",
           QString::number(vshot::loadConfig().cli.toneMapWhite));
    writeConfig(QStringLiteral(R"({"cli": {"tone-map-white": "high"}})"));
    expect(vshot::loadConfig().cli.toneMapWhite == 0.0,
           "a level that is not a number falls back to the built-in default");

    vshot::Config config = vshot::loadConfig();
    config.cli.toneMap = QStringLiteral("fixed");
    config.cli.toneMapWhite = 0.72;
    QJsonObject root = afterSave([&] { vshot::saveConfig(config); });
    expect(textAt(root, "cli/tone-map") == QStringLiteral("fixed"),
           "the chosen tone map was written", textAt(root, "cli/tone-map"));
    expect(numberAt(root, "cli/tone-map-white") > 0.71 &&
               numberAt(root, "cli/tone-map-white") < 0.73,
           "the white level was written", QString::number(numberAt(root, "cli/tone-map-white")));

    // Zero is how the settings window says "the built-in default stands", so it
    // must not be written: a level of zero is not one the map would ever use.
    config.cli.toneMapWhite = 0.0;
    root = afterSave([&] { vshot::saveConfig(config); });
    expect(!containsAt(root, "cli/tone-map-white"),
           "a white level of zero writes no key rather than a zero");

    config.cli.toneMap.clear();
    root = afterSave([&] { vshot::saveConfig(config); });
    expect(!containsAt(root, "cli/tone-map"),
           "clearing the tone map removes the key rather than writing an empty one");
}

void checkTheHdrAreaTestRoundTripsAndKeepsItsZero()
{
    std::printf("--- the HDR area test is remembered and its zero survives ----------\n");
    // The test is on when the file says nothing, so an absent key and a `true`
    // are the same thing, and only "off" needs a key of its own.
    writeConfig(QStringLiteral(R"({"cli": {}})"));
    expect(vshot::loadConfig().cli.hdrAreaTest, "the area test is on when the file says nothing");
    writeConfig(QStringLiteral(R"({"cli": {"hdr-area-test": false}})"));
    expect(!vshot::loadConfig().cli.hdrAreaTest, "an area test switched off is read");

    // A ratio of zero is the state that means "every capture of an HDR output
    // is HDR content", so it is a value and not an absent key -- the one place
    // in this section where zero is meaningful.  A file that says nothing has to
    // read back as *not* zero, which is what the negative sentinel is for.
    writeConfig(QStringLiteral(R"({"cli": {"hdr-area-ratio": 0}})"));
    expect(std::abs(vshot::loadConfig().cli.hdrAreaRatio) < 1e-9,
           "a ratio of zero is read as zero",
           QString::number(vshot::loadConfig().cli.hdrAreaRatio));
    writeConfig(QStringLiteral(R"({"cli": {}})"));
    expect(vshot::loadConfig().cli.hdrAreaRatio < 0.0,
           "an absent ratio reads as the sentinel, not as zero");
    writeConfig(QStringLiteral(R"({"cli": {"hdr-area-ratio": 2}})"));
    expect(std::abs(vshot::loadConfig().cli.hdrAreaRatio - 1.0) < 1e-9,
           "a ratio past a whole frame is clamped",
           QString::number(vshot::loadConfig().cli.hdrAreaRatio));

    vshot::Config config = vshot::loadConfig();
    config.cli.hdrAreaTest = false;
    config.cli.hdrAreaRatio = 0.0;
    QJsonObject root = afterSave([&] { vshot::saveConfig(config); });
    expect(booleanAt(root, "cli/hdr-area-test") == false, "the switch being off was written");
    expect(containsAt(root, "cli/hdr-area-ratio") && numberAt(root, "cli/hdr-area-ratio") == 0.0,
           "a ratio of zero was written rather than dropped",
           QString::number(numberAt(root, "cli/hdr-area-ratio")));

    // The switch being on is the default and needs no key, and a ratio nobody
    // chose is the sentinel and needs none either.
    config.cli.hdrAreaTest = true;
    config.cli.hdrAreaRatio = -1.0;
    root = afterSave([&] { vshot::saveConfig(config); });
    expect(!containsAt(root, "cli/hdr-area-test"),
           "an area test left on writes no key rather than a true");
    expect(!containsAt(root, "cli/hdr-area-ratio"), "an unset ratio writes no key");
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

void checkClearingOneSessionLeavesTheOther()
{
    std::printf("--- clearing the recording rows leaves the replay ones ------------\n");
    // The two cards share a page but own different keys: a save that clears the
    // recording defaults must not touch `cli.replay`, because the settings
    // window writes one struct and a crossed owner would wipe the other
    // session's setup.
    writeConfig(QStringLiteral(R"({
        "cli": {"record": {"encoder": "hevc", "fps": 30},
                "replay": {"window": 60, "encoder": "av1", "save-dir": "/tmp/clips"}}
    })"));
    vshot::Config config = vshot::loadConfig();
    // Clear only the recording fields, the way choosing "built-in default" on
    // each recording row does.
    config.cli.recordEncoder.clear();
    config.cli.recordEncoderBackend.clear();
    config.cli.recordFps = 0;
    config.cli.recordPortal = false;
    config.cli.recordMicEnabled = false;
    config.cli.recordFollow.clear();

    const QJsonObject root = afterSave([&] { vshot::saveConfig(config); });
    expect(!root.value(QStringLiteral("cli"))
                .toObject()
                .contains(QStringLiteral("record")),
           "clearing the recording rows drops the record section");
    expect(numberAt(root, "cli/replay/window") == 60,
           "the replay window survives a recording-only save");
    expect(textAt(root, "cli/replay/encoder") == QStringLiteral("av1"),
           "the replay encoder survives", textAt(root, "cli/replay/encoder"));
    expect(textAt(root, "cli/replay/save-dir") == QStringLiteral("/tmp/clips"),
           "the replay save directory survives");

    // And the reverse: a replay-only clear leaves the recording section whole.
    writeConfig(QStringLiteral(R"({
        "cli": {"record": {"encoder": "hevc", "fps": 30},
                "replay": {"window": 60, "encoder": "av1"}}
    })"));
    vshot::Config second = vshot::loadConfig();
    second.cli.replayWindow = 0;
    second.cli.replayGop = 0;
    second.cli.replayEncoder.clear();
    second.cli.replayEncoderBackend.clear();
    second.cli.replayFps = 0;
    second.cli.replayPortal = false;
    second.cli.replayMicEnabled = false;
    second.cli.replayFollow.clear();
    second.cli.replaySaveDir.clear();

    const QJsonObject reread = afterSave([&] { vshot::saveConfig(second); });
    expect(!reread.value(QStringLiteral("cli"))
                .toObject()
                .contains(QStringLiteral("replay")),
           "clearing the replay rows drops the replay section");
    expect(textAt(reread, "cli/record/encoder") == QStringLiteral("hevc"),
           "the recording encoder survives a replay-only save");
    expect(numberAt(reread, "cli/record/fps") == 30,
           "the recording frame rate survives");
}

void checkClearingAValueRemovesIt()
{
    std::printf("--- clearing a value actually clears it ----------------------------\n");
    writeConfig(QStringLiteral(R"({
        "cli": {"png-compression": "high", "monitor": "DP-2",
                "element-fallback": "components",
                "long": {"notches": 2, "max-height": 9000, "timeout": 30},
                "pin": {"density": 2},
                "record": {"encoder": "hevc", "encoder-backend": "nvenc", "fps": 30,
                           "portal": true, "mic": "", "follow": ["game"], "notify": false},
                "replay": {"window": 30, "gop": 2, "encoder": "av1", "encoder-backend": "vaapi",
                           "fps": 24, "portal": true, "mic": "", "follow": ["game"],
                           "save-dir": "/tmp/clips", "notify": false}}
    })"));
    // What the settings window produces when the user picks the built-in
    // default everywhere: every owned value absent.
    vshot::Config config = vshot::loadConfig();
    config.cli = vshot::CliPreferences{};
    const QJsonObject root = afterSave([&] { vshot::saveConfig(config); });

    expect(!root.contains(QStringLiteral("cli")),
           "clearing everything leaves no empty cli section behind");
    const vshot::Config reread = vshot::loadConfig();
    expect(reread.cli.pngCompression.isEmpty() &&
               reread.cli.elementFallback == QStringLiteral("lines") &&
               reread.cli.longNotches == 0 &&
               reread.cli.pinDensity == 0 && reread.cli.recordEncoder.isEmpty() &&
               reread.cli.recordFps == 0 && !reread.cli.recordPortal &&
               !reread.cli.recordMicEnabled && reread.cli.recordFollow.isEmpty() &&
               reread.cli.recordNotify,
           "the cleared recording defaults read back as unset");
    expect(reread.cli.replayWindow == 0 && reread.cli.replayGop == 0 &&
               reread.cli.replayEncoder.isEmpty() && reread.cli.replayFps == 0 &&
               !reread.cli.replayPortal && !reread.cli.replayMicEnabled &&
               reread.cli.replayFollow.isEmpty() && reread.cli.replaySaveDir.isEmpty() &&
               reread.cli.replayNotify,
           "the cleared replay defaults read back as unset");
}

void checkTheLookNumbersKeepTheLargestValueTheyAreOffered()
{
    std::printf("--- the look numbers and the ranges the window offers --------------\n");
    // Every look value used to be capped -- a pin's corner at 512, its rim at
    // 8, the dialog's at 48 and 8, a shadow's reach at 64 -- and the reason the
    // caps live in `config.hpp` is the one this asserts: a value the settings
    // window cannot show comes back clamped the next time that page is saved,
    // so the window's range and the loader's have to be one range.
    //
    // The four frame numbers have no ceiling left (see `kMaxFrameValue`), which
    // in practice means the largest value the window offers is the largest the
    // loader keeps.  The shadow's two still have one, because they are the only
    // look values that cost an allocation as they grow -- so for those the
    // assertion is both that the ceiling survives and that it is still a
    // ceiling.
    writeConfig(QStringLiteral(R"({
        "pin": {"radius": %1, "borderWidth": %1,
                "shadow": true, "shadowSize": %2, "shadowOffset": -%2},
        "dialog": {"radius": %1, "borderWidth": %1,
                   "shadow": true, "shadowSize": %2, "shadowOffset": -%2}
    })")
                    .arg(vshot::kMaxFrameValue)
                    .arg(vshot::kMaxShadowSize));
    const vshot::Config widest = vshot::loadConfig();
    const auto frame = static_cast<std::uint32_t>(vshot::kMaxFrameValue);
    expect(widest.pin.radius == frame && widest.pin.borderWidth == frame &&
               widest.dialog.radius == frame && widest.dialog.borderWidth == frame,
           "a corner and a rim keep the largest value the window offers",
           QStringLiteral("pin %1/%2, dialog %3/%4")
               .arg(widest.pin.radius)
               .arg(widest.pin.borderWidth)
               .arg(widest.dialog.radius)
               .arg(widest.dialog.borderWidth));
    expect(widest.pin.shadow.size == vshot::kMaxShadowSize &&
               widest.pin.shadow.offset == -vshot::kMaxShadowSize &&
               widest.dialog.shadow.size == vshot::kMaxShadowSize &&
               widest.dialog.shadow.offset == -vshot::kMaxShadowSize,
           "a shadow's reach and drop keep the largest value offered",
           QStringLiteral("pin %1/%2, dialog %3/%4")
               .arg(widest.pin.shadow.size)
               .arg(widest.pin.shadow.offset)
               .arg(widest.dialog.shadow.size)
               .arg(widest.dialog.shadow.offset));

    // And past it is clamped rather than kept: the ceiling is what keeps the
    // blur's own allocation bounded, so a hand-written number over it has to
    // come back as the ceiling and not as itself.
    writeConfig(QStringLiteral(R"({"pin": {"shadowSize": %1, "shadowOffset": -%1}})")
                    .arg(vshot::kMaxShadowSize + 1000));
    const vshot::Config over = vshot::loadConfig();
    expect(over.pin.shadow.size == vshot::kMaxShadowSize &&
               over.pin.shadow.offset == -vshot::kMaxShadowSize,
           "and a shadow past its ceiling comes back as the ceiling",
           QStringLiteral("size %1, offset %2")
               .arg(over.pin.shadow.size)
               .arg(over.pin.shadow.offset));
}

void checkRoundTripOfEveryField()
{
    std::printf("--- every field survives a write and a read ------------------------\n");
    QFile::remove(configPath());
    vshot::Config written;
    written.editor.tool = QStringLiteral("text");
    written.editor.selectMode = QStringLiteral("loose");
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
    written.cli.elementFallback = QStringLiteral("components");
    written.cli.monitor = QStringLiteral("HDMI-A-1");
    written.cli.longNotches = 4;
    written.cli.longMaxHeight = 12345;
    written.cli.longMaxFrames = 678;
    written.cli.longTimeout = 99;
    written.cli.longIgnoreTop = 42;
    written.cli.longInject = QStringLiteral("uinput");
    written.cli.pinDensity = 3;
    written.cli.recordEncoder = QStringLiteral("hevc");
    written.cli.recordEncoderBackend = QStringLiteral("nvenc");
    written.cli.recordFps = 120;
    written.cli.recordBitrate = 150;
    written.cli.recordQualitySet = true;
    written.cli.recordQuality = 0;
    written.cli.recordPortal = true;
    written.cli.recordMicEnabled = true;
    written.cli.recordMic = QStringLiteral("alsa_input.pci-0000_2f_00.4.analog-stereo");
    written.cli.recordFollow = {QStringLiteral("game"), QStringLiteral("chat")};
    written.cli.recordNotify = false;
    written.cli.replayWindow = 45;
    written.cli.replayGop = 3;
    written.cli.replayEncoder = QStringLiteral("av1");
    written.cli.replayEncoderBackend = QStringLiteral("vaapi");
    written.cli.replayFps = 24;
    written.cli.replayBitrate = 60;
    written.cli.replayQualitySet = true;
    written.cli.replayQuality = 200;
    written.cli.replayPortal = true;
    written.cli.replayMicEnabled = true;
    written.cli.replayMic = QStringLiteral("alsa_input.usb");
    written.cli.replayFollow = {QStringLiteral("game")};
    written.cli.replaySaveDir = QStringLiteral("/tmp/clips");
    written.cli.replayNotify = false;
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
    expect(read.editor.selectMode == written.editor.selectMode,
           "editor.selectMode round-trips", read.editor.selectMode);
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
    expect(read.cli.elementFallback == written.cli.elementFallback,
           "cli.element-fallback round-trips", read.cli.elementFallback);
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
    expect(read.cli.recordEncoderBackend == written.cli.recordEncoderBackend,
           "cli.record.encoder-backend round-trips", read.cli.recordEncoderBackend);
    expect(read.cli.recordFps == written.cli.recordFps, "cli.record.fps round-trips",
           QString::number(read.cli.recordFps));
    expect(read.cli.recordBitrate == written.cli.recordBitrate,
           "cli.record.bitrate round-trips", QString::number(read.cli.recordBitrate));
    // A remembered level of *zero* is the case the pair of fields exists for: it
    // is a level, and a reader that spelled "unset" with a zero would lose it.
    expect(read.cli.recordQualitySet && read.cli.recordQuality == 0,
           "cli.record.quality round-trips a level of zero",
           QString::number(read.cli.recordQuality));
    expect(read.cli.recordPortal, "cli.record.portal round-trips");
    expect(read.cli.recordMicEnabled && read.cli.recordMic == written.cli.recordMic,
           "cli.record.mic round-trips", read.cli.recordMic);
    expect(read.cli.recordFollow == written.cli.recordFollow, "cli.record.follow round-trips",
           read.cli.recordFollow.join(QLatin1Char(',')));
    expect(!read.cli.recordNotify, "cli.record.notify round-trips");
    expect(read.cli.replayWindow == written.cli.replayWindow, "cli.replay.window round-trips",
           QString::number(read.cli.replayWindow));
    expect(read.cli.replayGop == written.cli.replayGop, "cli.replay.gop round-trips",
           QString::number(read.cli.replayGop));
    expect(read.cli.replayEncoder == written.cli.replayEncoder, "cli.replay.encoder round-trips",
           read.cli.replayEncoder);
    expect(read.cli.replayEncoderBackend == written.cli.replayEncoderBackend,
           "cli.replay.encoder-backend round-trips", read.cli.replayEncoderBackend);
    expect(read.cli.replayFps == written.cli.replayFps, "cli.replay.fps round-trips",
           QString::number(read.cli.replayFps));
    expect(read.cli.replayBitrate == written.cli.replayBitrate,
           "cli.replay.bitrate round-trips", QString::number(read.cli.replayBitrate));
    expect(read.cli.replayQualitySet && read.cli.replayQuality == written.cli.replayQuality,
           "cli.replay.quality round-trips", QString::number(read.cli.replayQuality));
    expect(read.cli.replayPortal, "cli.replay.portal round-trips");
    expect(read.cli.replayMicEnabled && read.cli.replayMic == written.cli.replayMic,
           "cli.replay.mic round-trips", read.cli.replayMic);
    expect(read.cli.replayFollow == written.cli.replayFollow, "cli.replay.follow round-trips",
           read.cli.replayFollow.join(QLatin1Char(',')));
    expect(read.cli.replaySaveDir == written.cli.replaySaveDir,
           "cli.replay.save-dir round-trips", read.cli.replaySaveDir);
    expect(!read.cli.replayNotify, "cli.replay.notify round-trips");
    // The recording and replay sections are read apart: a follow list or a
    // codec in one must not appear in the other.
    expect(read.cli.recordFollow != read.cli.replayFollow &&
               read.cli.recordEncoder != read.cli.replayEncoder,
           "the two sections' values are read apart");
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

    // The level's two fields: a number outside the widest encoder's range reads
    // as "the file said nothing", and a file that says nothing about the level
    // must not come back claiming a level of zero -- which is a level.
    {
        vshot::Config probe = written;
        probe.cli.recordQualitySet = false;
        probe.cli.recordQuality = 0;
        probe.cli.replayQualitySet = true;
        probe.cli.replayQuality = vshot::kMaxQuality + 1;
        vshot::saveConfig(probe);
        const vshot::Config defaulted = vshot::loadConfig();
        expect(!defaulted.cli.recordQualitySet,
               "an unnamed level is the absent key, not a level of zero");
        expect(!defaulted.cli.replayQualitySet, "a level out of range reads as unset",
               QString::number(defaulted.cli.replayQuality));
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
    for (const QString &value : vshot::encoderBackendNames()) {
        vshot::Config probe = written;
        probe.cli.recordEncoderBackend = value;
        vshot::saveConfig(probe);
        expect(vshot::loadConfig().cli.recordEncoderBackend == value,
               "the settings window's encoder backends all load back", value);
    }
    for (const QString &value : vshot::compressionNames()) {
        vshot::Config probe = written;
        probe.cli.pngCompression = value;
        vshot::saveConfig(probe);
        expect(vshot::loadConfig().cli.pngCompression == value,
               "the settings window's compression names all load back", value);
    }
    for (const QString &value : vshot::hdrFormatNames()) {
        vshot::Config probe = written;
        probe.cli.hdrFormat = value;
        vshot::saveConfig(probe);
        expect(vshot::loadConfig().cli.hdrFormat == value,
               "the settings window's HDR format names all load back", value);
    }
    for (const QString &value : vshot::toneMapNames()) {
        vshot::Config probe = written;
        probe.cli.toneMap = value;
        vshot::saveConfig(probe);
        expect(vshot::loadConfig().cli.toneMap == value,
               "the settings window's tone-map names all load back", value);
    }
    for (const QString &value : vshot::elementFallbackNames()) {
        vshot::Config probe = written;
        probe.cli.elementFallback = value;
        vshot::saveConfig(probe);
        expect(vshot::loadConfig().cli.elementFallback == value,
               "the settings window's element readers all load back", value);
    }
    for (const QString &value : vshot::toolNames()) {
        vshot::Config probe = written;
        probe.editor.tool = value;
        vshot::saveConfig(probe);
        expect(vshot::loadConfig().editor.tool == value,
               "the settings window's tool names all load back", value);
    }
    for (const QString &value : vshot::selectModeNames()) {
        vshot::Config probe = written;
        probe.editor.selectMode = value;
        vshot::saveConfig(probe);
        expect(vshot::loadConfig().editor.selectMode == value,
               "the settings window's select modes all load back", value);
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

// The editor's key bindings are the one part of the config file a user is
// likely to edit by hand and the one part whose mistakes are invisible: a
// binding that does not parse reads as "the default", so a typo costs the user
// the key they asked for and gives them back one they did not. What is checked
// here is that a binding the file spells is the one the editor reads, that
// clearing one is not the same as never having set it, and that a save leaves
// everything else in the file alone.
void checkTheShortcutDefaultsAreReachable()
{
    std::printf("--- every default binding is reachable -----------------------------\n");
    QFile::remove(configPath());
    const vshot::ShortcutPreferences keys = vshot::loadShortcutPreferences();
    // One press per action, built the way a key event carries it. The point is
    // not that the spelling round-trips -- it is that the key a user presses
    // for an action is the one the editor hears, which is a different claim
    // from "the string came back unchanged".
    struct Probe {
        vshot::ShortcutAction action;
        int modifiers;
        int key;
    };
    const Probe probes[] = {
        {vshot::ShortcutAction::Confirm, 0, Qt::Key_Return},
        {vshot::ShortcutAction::Confirm, 0, Qt::Key_Enter},
        {vshot::ShortcutAction::Cancel, 0, Qt::Key_Escape},
        {vshot::ShortcutAction::Undo, Qt::ControlModifier, Qt::Key_Z},
        {vshot::ShortcutAction::Redo, Qt::ControlModifier, Qt::Key_Y},
        {vshot::ShortcutAction::Redo, Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_Z},
        {vshot::ShortcutAction::Copy, Qt::ControlModifier, Qt::Key_S},
        {vshot::ShortcutAction::CopyText, Qt::ControlModifier, Qt::Key_C},
        {vshot::ShortcutAction::Paste, Qt::ControlModifier, Qt::Key_V},
        {vshot::ShortcutAction::SelectAll, Qt::ControlModifier, Qt::Key_A},
        {vshot::ShortcutAction::SelectNone, Qt::ControlModifier, Qt::Key_D},
        {vshot::ShortcutAction::NextMark, 0, Qt::Key_Tab},
        // Shift+Tab arrives as Key_Backtab with Shift still set, which is the
        // whole reason this one is worth checking rather than assuming.
        {vshot::ShortcutAction::PreviousMark, Qt::ShiftModifier, Qt::Key_Backtab},
        {vshot::ShortcutAction::PreviousMark, Qt::ControlModifier | Qt::ShiftModifier,
         Qt::Key_Backtab},
        {vshot::ShortcutAction::Delete, 0, Qt::Key_Delete},
        {vshot::ShortcutAction::Delete, 0, Qt::Key_Backspace},
        {vshot::ShortcutAction::CopyColor, 0, Qt::Key_C},
        {vshot::ShortcutAction::AdoptColor, 0, Qt::Key_V},
        {vshot::ShortcutAction::ShowMagnifier, 0, Qt::Key_M},
        {vshot::ShortcutAction::CursorLeft, 0, Qt::Key_Left},
        {vshot::ShortcutAction::CursorLeft, 0, Qt::Key_A},
        {vshot::ShortcutAction::CursorRight, 0, Qt::Key_D},
        {vshot::ShortcutAction::CursorUp, 0, Qt::Key_W},
        {vshot::ShortcutAction::CursorDown, 0, Qt::Key_S},
    };
    for (const Probe &probe : probes) {
        const QKeySequence pressed(
            static_cast<int>(static_cast<int>(probe.modifiers) | static_cast<int>(probe.key)));
        expect(keys.matches(probe.action, pressed),
               "the default binding hears the key it is bound to",
               QStringLiteral("%1 <- %2").arg(vshot::shortcutBinding(probe.action).id,
                                              pressed.toString()));
    }
    // The held modifiers are read from the state of the keyboard rather than
    // from a key press, and Qt will not parse a lone "Shift" -- so they get
    // their own reader and their own check.  There used to be a third, the
    // drag-selection modifier, but the drag it gated is the middle button's
    // now: a button cannot be read from the keyboard's state at all, so there
    // is nothing left here for it to be.
    expect(keys.held(vshot::ShortcutAction::PreserveAspect, Qt::ShiftModifier),
           "the aspect-ratio modifier is read while it is held");
    expect(keys.held(vshot::ShortcutAction::CoarseStep, Qt::ControlModifier),
           "the coarse step modifier is read while it is held");
    expect(keys.held(vshot::ShortcutAction::SelectMark, Qt::ControlModifier),
           "the pick-up modifier is read while it is held");
    // The pick-up key is the coarse step's key as well, and this is the one pair
    // here that shares: the walk reads Ctrl off an arrow key, the pick-up reads
    // it off a mouse press, and no gesture is both.  What has to be pinned down
    // is that each of them still answers on its own -- a table that let the
    // second Ctrl entry overwrite the first would leave the walk stepping one
    // pixel, or the pick-up taking nothing.
    expect(keys.held(vshot::ShortcutAction::CoarseStep, Qt::ControlModifier),
           "and the coarse step has not lost its key to it");
    // The aspect key is a different key, and answers to nothing else: Shift
    // squares a shape, Ctrl takes a mark, and a press can mean both at once.
    expect(!keys.held(vshot::ShortcutAction::SelectMark, Qt::ShiftModifier),
           "the aspect key does not pick a mark up");
    expect(!keys.held(vshot::ShortcutAction::PreserveAspect, Qt::ControlModifier),
           "the pick-up key does not constrain a shape");
    // A modifier that is *not* the one an action wants must not count as held:
    // Alt is neither of these two keys, so it is held for neither.
    expect(!keys.held(vshot::ShortcutAction::SelectMark, Qt::AltModifier),
           "a modifier the pick-up does not answer to does not count as held");
    expect(keys.held(vshot::ShortcutAction::PreserveAspect,
                     Qt::ControlModifier | Qt::ShiftModifier),
           "the bound modifier still counts when another is down beside it");
}

void checkAnUnrelatedKeyIsNotABinding()
{
    std::printf("--- a key that is bound to nothing hears nothing -------------------\n");
    QFile::remove(configPath());
    const vshot::ShortcutPreferences keys = vshot::loadShortcutPreferences();
    // Ctrl+F is bound to nothing. It has to stay that way, or every unbound key
    // would fire some action's default.
    const QKeySequence find(static_cast<int>(Qt::ControlModifier | Qt::Key_F));
    bool any = false;
    for (int index = 0; index < static_cast<int>(vshot::ShortcutAction::kActionCount); ++index) {
        any = any || keys.matches(static_cast<vshot::ShortcutAction>(index), find);
    }
    expect(!any, "Ctrl+F is bound to nothing and fires nothing", find.toString());

    // And the modifiers alone -- a key event's bits without a key behind them.
    const QKeySequence ctrlOnly(static_cast<int>(Qt::ControlModifier));
    bool anyModifier = false;
    for (int index = 0; index < static_cast<int>(vshot::ShortcutAction::kActionCount); ++index) {
        anyModifier = anyModifier
            || keys.matches(static_cast<vshot::ShortcutAction>(index), ctrlOnly);
    }
    expect(!anyModifier, "pressing a modifier on its own fires nothing");
}

void checkAReboundKeyIsTheOneHeard()
{
    std::printf("--- a rebound binding is the one the editor hears ------------------\n");
    writeConfig(QStringLiteral(R"({"shortcuts": {"copy": "Ctrl+K", "undo": "F2"}})"));
    const vshot::ShortcutPreferences keys = vshot::loadShortcutPreferences();
    const auto pressed = [](int modifiers, int key) {
        return QKeySequence(static_cast<int>(modifiers | key));
    };
    expect(keys.matches(vshot::ShortcutAction::Copy,
                        pressed(Qt::ControlModifier, Qt::Key_K)),
           "the new key for copy is heard", keys.textFor(vshot::ShortcutAction::Copy));
    expect(!keys.matches(vshot::ShortcutAction::Copy,
                         pressed(Qt::ControlModifier, Qt::Key_S)),
           "the key it replaced is not heard any more");
    expect(keys.matches(vshot::ShortcutAction::Undo, pressed(0, Qt::Key_F2)),
           "a plain letter can be bound on its own", keys.textFor(vshot::ShortcutAction::Undo));
    // Everything the file did not mention keeps its default.
    expect(keys.matches(vshot::ShortcutAction::Redo,
                        pressed(Qt::ControlModifier, Qt::Key_Y)),
           "an action the file does not name keeps its default");

    // A binding that does not parse is not "unbind" -- it is a typo, and the
    // user is better off with the default than with nothing.
    writeConfig(QStringLiteral(R"({"shortcuts": {"copy": "Ctrl+Thumbprint"}})"));
    const vshot::ShortcutPreferences typo = vshot::loadShortcutPreferences();
    expect(typo.matches(vshot::ShortcutAction::Copy,
                        pressed(Qt::ControlModifier, Qt::Key_S)),
           "an unreadable binding falls back to the default rather than to nothing");

    // A value of the wrong type is the same story.
    writeConfig(QStringLiteral(R"({"shortcuts": {"copy": 7}})"));
    const vshot::ShortcutPreferences wrong = vshot::loadShortcutPreferences();
    expect(wrong.matches(vshot::ShortcutAction::Copy,
                         pressed(Qt::ControlModifier, Qt::Key_S)),
           "a binding that is not a string falls back to the default");

    // An action this build has never heard of is simply not read; it must not
    // disturb the ones beside it.
    writeConfig(QStringLiteral(R"({"shortcuts": {"future-action": "Ctrl+S", "copy": "Ctrl+K"}})"));
    const vshot::ShortcutPreferences future = vshot::loadShortcutPreferences();
    expect(future.matches(vshot::ShortcutAction::Copy,
                          pressed(Qt::ControlModifier, Qt::Key_K)),
           "an unknown action's key does not shadow a known one's");
}

void checkClearingABindingIsNotTheSameAsDefaulting()
{
    std::printf("--- clearing a binding leaves the action with no key ---------------\n");
    // The one pair of states the file has to keep apart: an absent key means
    // "the default stands", an empty one means "the user wants no key".
    writeConfig(QStringLiteral(R"({"shortcuts": {"copy": ""}})"));
    const vshot::ShortcutPreferences cleared = vshot::loadShortcutPreferences();
    expect(!cleared.hasKeys(vshot::ShortcutAction::Copy) && !cleared.matches(
               vshot::ShortcutAction::Copy,
               QKeySequence(static_cast<int>(Qt::ControlModifier | Qt::Key_S))),
           "an empty binding is no key at all, not the default back again");
    expect(cleared.matches(vshot::ShortcutAction::Undo,
                           QKeySequence(static_cast<int>(Qt::ControlModifier | Qt::Key_Z))),
           "clearing one action leaves every other one alone");

    // And it survives a round trip: the empty string has to be written, because
    // omitting the key would read back as the default.
    const QJsonObject root = afterSave([&] { vshot::saveShortcutPreferences(cleared); });
    expect(containsAt(root, "shortcuts/copy") && textAt(root, "shortcuts/copy").isEmpty(),
           "a cleared binding is written as an empty string, not omitted");
    const vshot::ShortcutPreferences again = vshot::loadShortcutPreferences();
    expect(!again.hasKeys(vshot::ShortcutAction::Copy), "it reads back as still cleared");
}

void checkTheShortcutsSaveKeepsTheRestOfTheFile()
{
    std::printf("--- saving the shortcuts keeps everything else ---------------------\n");
    writeConfig(QStringLiteral(R"({
        "editor": {"tool": "arrow", "color": "#00ff00"},
        "cli": {"png-compression": "high"},
        "shortcuts": {"copy": "Ctrl+K", "an-action-of-the-future": "Ctrl+J"},
        "some-future-section": {"keep": true}
    })"));
    vshot::ShortcutPreferences keys = vshot::loadShortcutPreferences();
    keys.setText(vshot::ShortcutAction::Undo, QStringLiteral("Ctrl+Backspace"));
    const QJsonObject root = afterSave([&] { vshot::saveShortcutPreferences(keys); });

    expect(textAt(root, "editor/tool") == QStringLiteral("arrow"),
           "the editor's style is still there", textAt(root, "editor/tool"));
    expect(textAt(root, "cli/png-compression") == QStringLiteral("high"),
           "the CLI defaults are still there");
    expect(containsAt(root, "some-future-section/keep"), "an unknown section is kept whole");
    expect(textAt(root, "shortcuts/copy") == QStringLiteral("Ctrl+K"),
           "a binding the save did not touch is still there", textAt(root, "shortcuts/copy"));
    expect(textAt(root, "shortcuts/undo") == QStringLiteral("Ctrl+Backspace"),
           "the rebound key was written", textAt(root, "shortcuts/undo"));
    expect(textAt(root, "shortcuts/an-action-of-the-future") == QStringLiteral("Ctrl+J"),
           "an action this build does not know is left in the file");

    // A binding put back to its default disappears from the file again: writing
    // twenty-two lines the user never asked for is not what a save is for.
    keys.setText(vshot::ShortcutAction::Undo, QStringLiteral("Ctrl+Z"));
    const QJsonObject back = afterSave([&] { vshot::saveShortcutPreferences(keys); });
    expect(!containsAt(root, "shortcuts/select-all"), "a default binding is not written");
    expect(!containsAt(back, "shortcuts/undo"),
           "a binding put back to its default is removed from the file");
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
    checkTheRetiredSelectToolStillMeansNothingArmed();
    checkTheHdrFormatRoundTripsAndIsCleared();
    checkTheToneMapRoundTripsAndIsCleared();
    checkTheHdrAreaTestRoundTripsAndKeepsItsZero();
    checkColorsUseTheCssSpelling();
    checkTheLegacyTextSizeIsMigrated();
    checkEditorSaveKeepsTheCliSection();
    checkSettingsSaveKeepsWhatItDoesNotOwn();
    checkClearingOneSessionLeavesTheOther();
    checkClearingAValueRemovesIt();
    checkTheLookNumbersKeepTheLargestValueTheyAreOffered();
    checkRoundTripOfEveryField();
    checkTheShortcutDefaultsAreReachable();
    checkAnUnrelatedKeyIsNotABinding();
    checkAReboundKeyIsTheOneHeard();
    checkClearingABindingIsNotTheSameAsDefaulting();
    checkTheShortcutsSaveKeepsTheRestOfTheFile();

    std::printf("--- result ---------------------------------------------------------\n");
    std::printf("%s (%d failure(s))\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}

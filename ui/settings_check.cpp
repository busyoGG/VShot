// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// Offline check for the settings window: that every field it shows is the
// field it saves, and that the file it writes is the file the CLI reads.
//
// The window is built exactly as the real one is and its widgets are driven
// through the same signals a user would produce -- combo boxes are switched,
// spin boxes are set, the Save button is clicked. What is read back is the
// config file on disk, not the dialog's own state, so a widget wired to the
// wrong field shows up here rather than in the user's next capture.
//
// This is why the check is worth having: the window has a field per setting
// across two sections, every one of them a plain widget connected to a struct
// member, and a mix-up between two neighbouring rows is invisible until someone
// notices their stroke width changing their text size.
//
// Built only with `-DVSHOT_BUILD_CHECKS=ON`. Needs Qt Widgets and the offscreen
// platform plugin; no compositor, because the dialog is never shown.

#include "config.hpp"
#include "i18n.hpp"
#include "settings_window.hpp"

#include <QApplication>
#include <QAbstractButton>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <cstdio>
#include <memory>

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
        std::printf("FAIL  could not write the probe config\n");
        ++failures;
        return;
    }
    file.write(text.toUtf8());
}

/// Finds a widget by the object name the window gives it, and fails loudly
/// rather than returning null: a renamed widget must break this check instead
/// of silently skipping it.
template <typename T> T *find(QDialog *dialog, const char *name)
{
    T *widget = dialog->findChild<T *>(QString::fromLatin1(name));
    if (widget == nullptr) {
        std::printf("FAIL  the settings window has no `%s` widget\n", name);
        ++failures;
    }
    return widget;
}

/// Selects a combo entry by its stored value, the way picking from the list
/// does, and reports whether the entry was there to pick.
bool choose(QComboBox *box, const QString &value)
{
    const int index = box->findData(value);
    if (index < 0) {
        return false;
    }
    box->setCurrentIndex(index);
    return true;
}

/// The colour a swatch button is showing, read off its own text -- the hex it
/// displays.  A colour button is a QPushButton with no public colour getter, so
/// the text is the honest thing to read: it is what the user sees, and it is
/// what a button wired to the wrong field would show the wrong value of.
QColor colorOf(QDialog *dialog, const char *name)
{
    QPushButton *button = find<QPushButton>(dialog, name);
    return button == nullptr ? QColor() : vshot::parseColorText(button->text());
}

void checkEveryFieldReachesTheFile()
{
    std::printf("--- every field the window shows reaches the file ---------------\n");
    writeConfig(QStringLiteral("{}"));
    std::unique_ptr<QDialog> dialog(vshot::createSettingsDialog());
    if (!dialog) {
        std::printf("FAIL  the settings dialog could not be built\n");
        ++failures;
        return;
    }

    // Every editor field set to a value that is not the default, so a field
    // wired to the wrong member (or to nothing) is caught by the read-back.
    expect(choose(find<QComboBox>(dialog.get(), "tool"), QStringLiteral("mosaic")),
           "the tool list offers mosaic");
    find<QSpinBox>(dialog.get(), "width")->setValue(23);
    expect(choose(find<QComboBox>(dialog.get(), "dash"), QStringLiteral("dotted")),
           "the line-style list offers dotted");
    find<QSpinBox>(dialog.get(), "arrowSize")->setValue(6);
    expect(choose(find<QComboBox>(dialog.get(), "arrowStyle"), QStringLiteral("filled")),
           "the arrow-head list offers filled");
    // The text size is a font height in pixels now, so the check uses a value
    // from that range -- and one that is *not* a whole glyph multiple, so a
    // round trip through the config cannot quietly snap it to one.
    find<QSpinBox>(dialog.get(), "textSize")->setValue(31);
    expect(choose(find<QComboBox>(dialog.get(), "mosaicShape"), QStringLiteral("brush")),
           "the mosaic-shape list offers brush");
    find<QSpinBox>(dialog.get(), "mosaicStrength")->setValue(3);

    expect(choose(find<QComboBox>(dialog.get(), "pngCompression"), QStringLiteral("balanced")),
           "the compression list offers balanced");
    find<QLineEdit>(dialog.get(), "monitor")->setText(QStringLiteral("  HDMI-A-1  "));
    find<QSpinBox>(dialog.get(), "pinDensity")->setValue(3);
    find<QSpinBox>(dialog.get(), "longNotches")->setValue(7);
    find<QSpinBox>(dialog.get(), "longMaxHeight")->setValue(12345);
    find<QSpinBox>(dialog.get(), "longMaxFrames")->setValue(678);
    find<QSpinBox>(dialog.get(), "longTimeout")->setValue(99);
    find<QSpinBox>(dialog.get(), "longIgnoreTop")->setValue(42);
    expect(choose(find<QComboBox>(dialog.get(), "longInject"), QStringLiteral("uinput")),
           "the scroll-backend list offers uinput");
    // The notification switch is the one row whose field is not optional, so
    // it is turned *off* here: on is what a mis-wired switch would leave it at.
    find<QAbstractButton>(dialog.get(), "ocrNotify")->setChecked(false);

    expect(choose(find<QComboBox>(dialog.get(), "recordEncoder"), QStringLiteral("hevc")),
           "the encoder list offers hevc");
    expect(choose(find<QComboBox>(dialog.get(), "recordEncoderBackend"), QStringLiteral("nvenc")),
           "the encoder-backend list offers nvenc");
    find<QSpinBox>(dialog.get(), "recordFps")->setValue(144);
    find<QLineEdit>(dialog.get(), "recordFollow")->setText(QStringLiteral("game, chat"));
    find<QAbstractButton>(dialog.get(), "recordPortal")->setChecked(true);
    // The recording notification switch is turned off, as the OCR one is: on is
    // what a mis-wired switch would leave it at.
    find<QAbstractButton>(dialog.get(), "recordNotify")->setChecked(false);
    // The microphone row is filled from the running session, so which devices
    // it offers is not this check's business -- but the first two entries are
    // the answers that always exist, and index 1 is the session's default
    // input, which is the one that exercises the empty string the file spells
    // it with.
    QComboBox *microphone = find<QComboBox>(dialog.get(), "recordMic");
    if (microphone != nullptr) {
        expect(microphone->count() >= 2, "the microphone row offers both built-in answers",
               QString::number(microphone->count()));
        microphone->setCurrentIndex(1);
    }

    // The replay card, every row set away from its default so a crossed wire is
    // caught: the two cards carry same-class widgets, which is exactly the shape
    // that gets two rows wired to one member.
    find<QSpinBox>(dialog.get(), "replayWindow")->setValue(45);
    find<QSpinBox>(dialog.get(), "replayGop")->setValue(3);
    expect(choose(find<QComboBox>(dialog.get(), "replayEncoder"), QStringLiteral("av1")),
           "the replay encoder list offers av1");
    expect(choose(find<QComboBox>(dialog.get(), "replayEncoderBackend"), QStringLiteral("vaapi")),
           "the replay encoder-backend list offers vaapi");
    find<QSpinBox>(dialog.get(), "replayFps")->setValue(72);
    find<QLineEdit>(dialog.get(), "replayFollow")->setText(QStringLiteral("game"));
    find<QAbstractButton>(dialog.get(), "replayPortal")->setChecked(true);
    find<QLineEdit>(dialog.get(), "replaySaveDir")->setText(QStringLiteral("  /tmp/clips  "));
    find<QAbstractButton>(dialog.get(), "replayNotify")->setChecked(false);
    QComboBox *replayMicrophone = find<QComboBox>(dialog.get(), "replayMic");
    if (replayMicrophone != nullptr) {
        expect(replayMicrophone->count() >= 2,
               "the replay microphone row offers both built-in answers",
               QString::number(replayMicrophone->count()));
        replayMicrophone->setCurrentIndex(0);
    }

    // The pins' look.  The radius and the border width are set to values that
    // are neither the default nor each other, so a pair wired to the same
    // member cannot pass.
    find<QSpinBox>(dialog.get(), "pinRadius")->setValue(17);
    find<QAbstractButton>(dialog.get(), "pinShadow")->setChecked(false);
    find<QSpinBox>(dialog.get(), "pinBorderWidth")->setValue(5);
    find<QSpinBox>(dialog.get(), "pinShadowSize")->setValue(23);
    find<QSpinBox>(dialog.get(), "pinShadowOffset")->setValue(-6);
    find<QSpinBox>(dialog.get(), "pinShadowOpacity")->setValue(200);
    find<QSpinBox>(dialog.get(), "dialogRadius")->setValue(19);
    find<QSpinBox>(dialog.get(), "dialogBorderWidth")->setValue(3);
    find<QSpinBox>(dialog.get(), "dialogShadowSize")->setValue(11);
    find<QSpinBox>(dialog.get(), "dialogShadowOffset")->setValue(4);
    find<QSpinBox>(dialog.get(), "dialogShadowOpacity")->setValue(90);

    QPushButton *save = find<QPushButton>(dialog.get(), "saveButton");
    if (save != nullptr) {
        save->click();
    }

    const vshot::Config saved = vshot::loadConfig();
    expect(saved.editor.tool == QStringLiteral("mosaic"), "the tool reached the file", saved.editor.tool);
    expect(saved.editor.width == 23, "the width reached the file", QString::number(saved.editor.width));
    expect(saved.editor.dash == QStringLiteral("dotted"), "the line style reached the file");
    expect(saved.editor.arrowSize == 6, "the arrow head size reached the file");
    expect(saved.editor.arrowStyle == QStringLiteral("filled"), "the arrow head style reached the file");
    expect(saved.editor.textSize == 31, "the text size reached the file");
    expect(saved.editor.mosaicShape == QStringLiteral("brush"), "the mosaic shape reached the file");
    expect(saved.editor.mosaicStrength == 3, "the mosaic strength reached the file");
    expect(saved.cli.pngCompression == QStringLiteral("balanced"),
           "the compression default reached the file", saved.cli.pngCompression);
    // Whitespace around a hand-typed monitor name is trimmed rather than saved.
    expect(saved.cli.monitor == QStringLiteral("HDMI-A-1"),
           "the monitor default reached the file", saved.cli.monitor);
    expect(saved.cli.pinDensity == 3, "the pin density reached the file");
    expect(saved.cli.longNotches == 7, "the scroll notches reached the file");
    expect(saved.cli.longMaxHeight == 12345, "the scroll height limit reached the file");
    expect(saved.cli.longMaxFrames == 678, "the scroll frame limit reached the file");
    expect(saved.cli.longTimeout == 99, "the scroll timeout reached the file");
    expect(saved.cli.longIgnoreTop == 42, "the scroll ignore-top reached the file");
    expect(saved.cli.longInject == QStringLiteral("uinput"), "the scroll backend reached the file");
    expect(!saved.cli.ocrNotify, "the notification switch reached the file");
    expect(saved.cli.recordEncoder == QStringLiteral("hevc"),
           "the encoder default reached the file", saved.cli.recordEncoder);
    expect(saved.cli.recordEncoderBackend == QStringLiteral("nvenc"),
           "the encoder-backend default reached the file", saved.cli.recordEncoderBackend);
    expect(saved.cli.recordFps == 144, "the frame rate default reached the file",
           QString::number(saved.cli.recordFps));
    expect(saved.cli.recordFollow == QStringList({QStringLiteral("game"), QStringLiteral("chat")}),
           "the follow list reached the file", saved.cli.recordFollow.join(QLatin1Char(',')));
    expect(saved.cli.recordPortal, "the portal switch reached the file");
    expect(!saved.cli.recordNotify, "the recording notification switch reached the file");
    expect(saved.cli.recordMicEnabled && saved.cli.recordMic.isEmpty(),
           "the session's default input reached the file as an empty name",
           saved.cli.recordMic);
    expect(saved.cli.replayWindow == 45, "the replay history window reached the file",
           QString::number(saved.cli.replayWindow));
    expect(saved.cli.replayGop == 3, "the replay key-frame distance reached the file",
           QString::number(saved.cli.replayGop));
    expect(saved.cli.replayEncoder == QStringLiteral("av1"),
           "the replay encoder reached the file", saved.cli.replayEncoder);
    expect(saved.cli.replayEncoderBackend == QStringLiteral("vaapi"),
           "the replay encoder-backend reached the file", saved.cli.replayEncoderBackend);
    expect(saved.cli.replayFps == 72, "the replay frame rate reached the file",
           QString::number(saved.cli.replayFps));
    expect(saved.cli.replayFollow == QStringList({QStringLiteral("game")}),
           "the replay follow list reached the file", saved.cli.replayFollow.join(QLatin1Char(',')));
    expect(saved.cli.replayPortal, "the replay portal switch reached the file");
    expect(!saved.cli.replayMicEnabled,
           "the replay microphone row's silence reached the file", saved.cli.replayMic);
    // Whitespace around a hand-typed path is trimmed rather than saved.
    expect(saved.cli.replaySaveDir == QStringLiteral("/tmp/clips"),
           "the replay save directory reached the file", saved.cli.replaySaveDir);
    expect(!saved.cli.replayNotify, "the replay notification switch reached the file");
    expect(saved.pin.radius == 17, "the pin radius reached the file",
           QString::number(saved.pin.radius));
    expect(!saved.pin.shadow.enabled, "the pin shadow switch reached the file");
    expect(saved.pin.shadow.size == 23 && saved.pin.shadow.offset == -6 &&
               saved.pin.shadow.opacity == 200,
           "the pin shadow's numbers reached the file",
           QStringLiteral("%1/%2/%3")
               .arg(saved.pin.shadow.size)
               .arg(saved.pin.shadow.offset)
               .arg(saved.pin.shadow.opacity));
    expect(saved.dialog.shadow.enabled && saved.dialog.shadow.size == 11 &&
               saved.dialog.shadow.offset == 4 && saved.dialog.shadow.opacity == 90,
           "the dialog shadow's numbers reached the file",
           QStringLiteral("%1/%2/%3/%4")
               .arg(saved.dialog.shadow.enabled ? 1 : 0)
               .arg(saved.dialog.shadow.size)
               .arg(saved.dialog.shadow.offset)
               .arg(saved.dialog.shadow.opacity));
    expect(saved.pin.borderWidth == 5, "the pin border width reached the file",
           QString::number(saved.pin.borderWidth));

    // The neighbouring rows must not have been crossed: this is what tells a
    // correct wiring apart from one that writes every value somewhere.
    expect(saved.editor.width != saved.editor.textSize &&
               saved.editor.arrowSize != saved.editor.mosaicStrength,
           "the numeric rows did not write each other's values");
}

void checkTheWindowOpensOnTheStoredValues()
{
    std::printf("--- the window opens on what the file says ----------------------\n");
    // The size is written under the pixel key, which is what this build
    // produces; the legacy `textSize` (a glyph multiple) is checked separately.
    writeConfig(QStringLiteral(R"({
        "editor": {"tool": "ellipse", "width": 12, "dash": "dashed", "arrowStyle": "filled",
                   "mosaicShape": "ellipse", "mosaicStrength": 1, "arrowSize": 2, "textPixels": 28},
        "cli": {"png-compression": "fastest", "monitor": "DP-3",
                "long": {"notches": 3, "inject": "portal", "timeout": 45},
                "pin": {"density": 2}, "ocr": {"notify": false},
                "record": {"encoder": "av1", "encoder-backend": "nvenc", "fps": 30, "portal": true,
                           "follow": ["game", "chat"], "notify": false,
                           "mic": "alsa_input.pci-0000_2f_00.4.analog-stereo"},
                "replay": {"window": 12, "gop": 2, "encoder": "hevc",
                           "encoder-backend": "vaapi", "fps": 24, "portal": true,
                           "follow": ["game"], "save-dir": "/tmp/clips", "notify": false}},
        "pin": {"radius": 9, "shadow": false, "shadowSize": 21, "shadowOffset": -7,
                "shadowOpacity": 200, "borderWidth": 6,
                "borderColor": "#112233", "activeBorderColor": "#445566"},
        "dialog": {"radius": 7, "shadowSize": 9, "shadowOffset": 5, "shadowOpacity": 77}
    })"));
    std::unique_ptr<QDialog> dialog(vshot::createSettingsDialog());
    if (!dialog) {
        return;
    }
    expect(find<QComboBox>(dialog.get(), "tool")->currentData().toString() == QStringLiteral("ellipse"),
           "the tool box shows the stored tool");
    expect(find<QSpinBox>(dialog.get(), "width")->value() == 12, "the width box shows the stored width");
    expect(find<QComboBox>(dialog.get(), "dash")->currentData().toString() == QStringLiteral("dashed"),
           "the line-style box shows the stored style");
    expect(find<QSpinBox>(dialog.get(), "arrowSize")->value() == 2, "the arrow size box is right");
    expect(find<QComboBox>(dialog.get(), "arrowStyle")->currentData().toString() == QStringLiteral("filled"),
           "the arrow style box is right");
    expect(find<QSpinBox>(dialog.get(), "textSize")->value() == 28, "the text size box is right");
    expect(find<QComboBox>(dialog.get(), "mosaicShape")->currentData().toString() == QStringLiteral("ellipse"),
           "the mosaic shape box is right");
    expect(find<QSpinBox>(dialog.get(), "mosaicStrength")->value() == 1, "the mosaic strength box is right");
    expect(find<QComboBox>(dialog.get(), "pngCompression")->currentData().toString() == QStringLiteral("fastest"),
           "the compression box shows the stored default");
    expect(find<QLineEdit>(dialog.get(), "monitor")->text() == QStringLiteral("DP-3"),
           "the monitor field shows the stored default");
    expect(find<QSpinBox>(dialog.get(), "longNotches")->value() == 3, "the notches box is right");
    expect(find<QComboBox>(dialog.get(), "longInject")->currentData().toString() == QStringLiteral("portal"),
           "the scroll-backend box is right");
    expect(find<QSpinBox>(dialog.get(), "longTimeout")->value() == 45, "the timeout box is right");
    expect(find<QSpinBox>(dialog.get(), "pinDensity")->value() == 2, "the pin density box is right");
    expect(!find<QAbstractButton>(dialog.get(), "ocrNotify")->isChecked(),
           "the notification switch shows the stored off state");
    expect(find<QComboBox>(dialog.get(), "recordEncoder")->currentData().toString() ==
               QStringLiteral("av1"),
           "the encoder box shows the stored codec");
    expect(find<QComboBox>(dialog.get(), "recordEncoderBackend")->currentData().toString() ==
               QStringLiteral("nvenc"),
           "the encoder-backend box shows the stored backend");
    expect(find<QSpinBox>(dialog.get(), "recordFps")->value() == 30,
           "the frame rate box shows the stored rate");
    expect(find<QLineEdit>(dialog.get(), "recordFollow")->text() ==
               QStringLiteral("game, chat"),
           "the follow box shows the stored windows",
           find<QLineEdit>(dialog.get(), "recordFollow")->text());
    expect(find<QAbstractButton>(dialog.get(), "recordPortal")->isChecked(),
           "the portal switch shows the stored on state");
    expect(!find<QAbstractButton>(dialog.get(), "recordNotify")->isChecked(),
           "the recording notification switch shows the stored off state");
    // A device the session is not offering right now -- this one is not running
    // on the machine the check runs on either -- has to stay selectable, or
    // opening and saving the window would quietly drop it from the file.
    expect(find<QComboBox>(dialog.get(), "recordMic")->currentData().toString() ==
               QStringLiteral("alsa_input.pci-0000_2f_00.4.analog-stereo"),
           "the microphone box shows the stored device",
           find<QComboBox>(dialog.get(), "recordMic")->currentData().toString());
    // The replay card reads its own `cli.replay` section, and every row has to
    // show the value its own key carries -- the recording rows above must not
    // have leaked into any of them.
    expect(find<QSpinBox>(dialog.get(), "replayWindow")->value() == 12,
           "the replay history box shows the stored window");
    expect(find<QSpinBox>(dialog.get(), "replayGop")->value() == 2,
           "the replay gop box shows the stored distance");
    expect(find<QComboBox>(dialog.get(), "replayEncoder")->currentData().toString() ==
               QStringLiteral("hevc"),
           "the replay encoder box shows the stored codec");
    expect(find<QComboBox>(dialog.get(), "replayEncoderBackend")->currentData().toString() ==
               QStringLiteral("vaapi"),
           "the replay encoder-backend box shows the stored backend");
    expect(find<QSpinBox>(dialog.get(), "replayFps")->value() == 24,
           "the replay frame rate box shows the stored rate");
    expect(find<QLineEdit>(dialog.get(), "replayFollow")->text() == QStringLiteral("game"),
           "the replay follow box shows the stored window",
           find<QLineEdit>(dialog.get(), "replayFollow")->text());
    expect(find<QAbstractButton>(dialog.get(), "replayPortal")->isChecked(),
           "the replay portal switch shows the stored on state");
    expect(find<QLineEdit>(dialog.get(), "replaySaveDir")->text() == QStringLiteral("/tmp/clips"),
           "the replay save directory shows the stored path",
           find<QLineEdit>(dialog.get(), "replaySaveDir")->text());
    expect(!find<QAbstractButton>(dialog.get(), "replayNotify")->isChecked(),
           "the replay notification switch shows the stored off state");
    expect(find<QSpinBox>(dialog.get(), "pinRadius")->value() == 9, "the pin radius box is right");
    expect(!find<QAbstractButton>(dialog.get(), "pinShadow")->isChecked(),
           "the shadow switch shows the stored off state");
    expect(find<QSpinBox>(dialog.get(), "pinShadowSize")->value() == 21 &&
               find<QSpinBox>(dialog.get(), "pinShadowOffset")->value() == -7 &&
               find<QSpinBox>(dialog.get(), "pinShadowOpacity")->value() == 200,
           "the pin shadow boxes show the stored numbers",
           QStringLiteral("%1/%2/%3")
               .arg(find<QSpinBox>(dialog.get(), "pinShadowSize")->value())
               .arg(find<QSpinBox>(dialog.get(), "pinShadowOffset")->value())
               .arg(find<QSpinBox>(dialog.get(), "pinShadowOpacity")->value()));
    expect(find<QSpinBox>(dialog.get(), "dialogRadius")->value() == 7,
           "the dialog radius box is right");
    expect(find<QSpinBox>(dialog.get(), "dialogShadowSize")->value() == 9 &&
               find<QSpinBox>(dialog.get(), "dialogShadowOffset")->value() == 5 &&
               find<QSpinBox>(dialog.get(), "dialogShadowOpacity")->value() == 77,
           "the dialog shadow boxes show the stored numbers",
           QStringLiteral("%1/%2/%3")
               .arg(find<QSpinBox>(dialog.get(), "dialogShadowSize")->value())
               .arg(find<QSpinBox>(dialog.get(), "dialogShadowOffset")->value())
               .arg(find<QSpinBox>(dialog.get(), "dialogShadowOpacity")->value()));
    expect(find<QSpinBox>(dialog.get(), "pinBorderWidth")->value() == 6,
           "the pin border width box is right");
    // The two colours are shown on their own buttons, and each has to be the
    // one its own row carries: they are the same widget class in neighbouring
    // rows, which is exactly the mix-up this check exists to catch.
    expect(colorOf(dialog.get(), "pinBorderColorButton") == QColor(0x11, 0x22, 0x33),
           "the border colour button shows the stored border colour",
           colorOf(dialog.get(), "pinBorderColorButton").name());
    expect(colorOf(dialog.get(), "pinActiveColorButton") == QColor(0x44, 0x55, 0x66),
           "the active colour button shows the stored active colour",
           colorOf(dialog.get(), "pinActiveColorButton").name());

    // A field the file *does* mention is shown as itself -- including the ones
    // whose leading entry is the built-in default, where a box wired to the
    // wrong index would sit on that entry and look plausible.
    expect(find<QSpinBox>(dialog.get(), "pinDensity")->value() == 2,
           "the pin density the file sets is shown as itself",
           QString::number(find<QSpinBox>(dialog.get(), "pinDensity")->value()));
    expect(find<QComboBox>(dialog.get(), "pngCompression")->currentText() ==
               QStringLiteral("fastest"),
           "the compression the file sets is shown as itself",
           find<QComboBox>(dialog.get(), "pngCompression")->currentText());
    expect(find<QComboBox>(dialog.get(), "recordEncoder")->currentText() ==
               QStringLiteral("av1"),
           "the encoder the file sets is shown as itself",
           find<QComboBox>(dialog.get(), "recordEncoder")->currentText());
    // And one the file says nothing about opens on the built-in default rather
    // than on a zero or the word "default": what the window shows is then what
    // the CLI will actually use.  This file does not mention `--max-height`, and
    // 30000 is the number `src/cli.rs` falls back to for it.
    QSpinBox *maxHeight = find<QSpinBox>(dialog.get(), "longMaxHeight");
    expect(maxHeight->value() == 30000, "an unset height limit opens on the built-in default",
           QString::number(maxHeight->value()));
    expect(maxHeight->specialValueText().isEmpty(),
           "an ordinary spin box has no special text left over",
           maxHeight->specialValueText());
}

void checkClearingOneColorLeavesTheOther()
{
    std::printf("--- clearing one pin colour leaves the other ---------------------\n");
    writeConfig(QStringLiteral(R"({
        "pin": {"borderColor": "#112233", "activeBorderColor": "#445566"}
    })"));
    std::unique_ptr<QDialog> dialog(vshot::createSettingsDialog());
    if (!dialog) {
        return;
    }
    // Clearing the idle colour is what "Automatic" does on that row, and the
    // active colour -- a different field with its own button -- has to survive
    // it.  Two buttons of the same class in neighbouring rows is precisely the
    // shape that gets wired to the same member by accident.
    find<QPushButton>(dialog.get(), "pinBorderColorClear")->click();
    find<QPushButton>(dialog.get(), "saveButton")->click();

    const vshot::Config saved = vshot::loadConfig();
    expect(!saved.pin.borderColor.isValid(),
           "the cleared colour is stored as automatic", saved.pin.borderColor.name());
    expect(saved.pin.activeBorderColor == QColor(0x44, 0x55, 0x66),
           "the other colour is untouched", saved.pin.activeBorderColor.name());

    // And the reverse, from a fresh window: clearing the active colour leaves
    // the idle one alone.
    writeConfig(QStringLiteral(R"({
        "pin": {"borderColor": "#112233", "activeBorderColor": "#445566"}
    })"));
    std::unique_ptr<QDialog> second(vshot::createSettingsDialog());
    if (!second) {
        return;
    }
    find<QPushButton>(second.get(), "pinActiveColorClear")->click();
    find<QPushButton>(second.get(), "saveButton")->click();
    const vshot::Config reread = vshot::loadConfig();
    expect(reread.pin.borderColor == QColor(0x11, 0x22, 0x33),
           "clearing the active colour leaves the idle one", reread.pin.borderColor.name());
    expect(!reread.pin.activeBorderColor.isValid(),
           "and the active colour is the one that went automatic",
           reread.pin.activeBorderColor.name());
}

void checkCancelChangesNothing()
{
    std::printf("--- cancel leaves the file alone ---------------------------------\n");
    const QString original = QStringLiteral(R"({
        "editor": {"tool": "pen", "width": 5},
        "cli": {"monitor": "DP-2", "future-key": 1}
    })");
    writeConfig(original);
    std::unique_ptr<QDialog> dialog(vshot::createSettingsDialog());
    if (!dialog) {
        return;
    }
    find<QSpinBox>(dialog.get(), "width")->setValue(40);
    choose(find<QComboBox>(dialog.get(), "tool"), QStringLiteral("text"));
    QPushButton *cancel = dialog->findChild<QPushButton *>(QStringLiteral("cancelButton"));
    if (cancel == nullptr) {
        // The cancel button is a plain reject button; drive the dialog instead.
        dialog->reject();
    } else {
        cancel->click();
    }

    QFile file(configPath());
    if (!file.open(QIODevice::ReadOnly)) {
        std::printf("FAIL  the config file could not be read back after cancel\n");
        ++failures;
        return;
    }
    const QString after = QString::fromUtf8(file.readAll());
    QJsonParseError error{};
    const QJsonObject root = QJsonDocument::fromJson(after.toUtf8(), &error).object();
    expect(error.error == QJsonParseError::NoError, "the file is still valid JSON after cancel");
    expect(root.value(QStringLiteral("editor")).toObject().value(QStringLiteral("tool")).toString() ==
               QStringLiteral("pen"),
           "the tool is unchanged after cancel");
    expect(root.value(QStringLiteral("editor")).toObject().value(QStringLiteral("width")).toInt() == 5,
           "the width is unchanged after cancel");
    expect(root.value(QStringLiteral("cli")).toObject().value(QStringLiteral("future-key")).toInt() == 1,
           "an unknown key survives a cancel");
}

void checkTheOcrEngineSurvivesASave()
{
    std::printf("--- saving the notification switch keeps the OCR engine ------------\n");
    // The `ocr` section is mostly hand-written: the engine and its external
    // command are not in this window, but the window's save is what rewrites
    // the section around them.  A save that dropped `engine` would move a user
    // who set up a GPU engine back onto the CPU without saying so.
    writeConfig(QStringLiteral(R"({
        "cli": {"ocr": {"engine": "external",
                        "external": {"command": ["my-ocr", "--stdin"], "stdin": true,
                                     "timeout": 12}}}
    })"));
    std::unique_ptr<QDialog> dialog(vshot::createSettingsDialog());
    if (!dialog) {
        return;
    }
    expect(find<QAbstractButton>(dialog.get(), "ocrNotify")->isChecked(),
           "a file that says nothing about notifications opens with them on");
    find<QAbstractButton>(dialog.get(), "ocrNotify")->setChecked(false);
    find<QPushButton>(dialog.get(), "saveButton")->click();

    QFile file(configPath());
    if (!file.open(QIODevice::ReadOnly)) {
        std::printf("FAIL  the config file could not be read back\n");
        ++failures;
        return;
    }
    QJsonParseError error{};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll(), &error).object();
    expect(error.error == QJsonParseError::NoError, "the saved file is valid JSON");
    const QJsonObject ocr = root.value(QStringLiteral("cli"))
                                .toObject()
                                .value(QStringLiteral("ocr"))
                                .toObject();
    expect(ocr.value(QStringLiteral("notify")).isBool() &&
               !ocr.value(QStringLiteral("notify")).toBool(),
           "the switch was written as off");
    expect(ocr.value(QStringLiteral("engine")).toString() == QStringLiteral("external"),
           "the OCR engine survives a save", ocr.value(QStringLiteral("engine")).toString());
    const QJsonObject external = ocr.value(QStringLiteral("external")).toObject();
    expect(external.value(QStringLiteral("command")).toArray().size() == 2 &&
               external.value(QStringLiteral("stdin")).toBool() &&
               external.value(QStringLiteral("timeout")).toInt() == 12,
           "the external command survives a save",
           QStringLiteral("%1 args, stdin=%2, timeout=%3")
               .arg(external.value(QStringLiteral("command")).toArray().size())
               .arg(external.value(QStringLiteral("stdin")).toBool() ? 1 : 0)
               .arg(external.value(QStringLiteral("timeout")).toInt()));
}

/// The page a widget is on, as an index into the sidebar, or -1 when it is not
/// on any of them.
///
/// Walks the parent chain rather than looking at the stacked widget directly:
/// every row is nested a few widgets deep (row, card, column, page), and the
/// nesting is an implementation detail this check should not have to know.
int pageOf(QWidget *widget, QStackedWidget *pages)
{
    for (QWidget *at = widget; at != nullptr; at = at->parentWidget()) {
        const int index = pages->indexOf(at);
        if (index >= 0) {
            return index;
        }
    }
    return -1;
}

/// That the sidebar and the pages agree, and that every setting sits on the
/// page its own name promises.
///
/// The window's row index *is* the page index -- `currentRowChanged` switches
/// on it -- so a page added without a sidebar item, or added in a different
/// order, silently shows one page while the sidebar highlights another.  The
/// split into one page per feature is exactly the change that can do that, and
/// nothing about the window looks wrong when it happens: the settings are all
/// still there, just not where the sidebar says they are.  So both halves are
/// pinned: the counts match, and a known widget from each feature is found on
/// the page that feature names.
void checkEverySettingIsOnThePageTheSidebarNames()
{
    std::printf("--- the sidebar and the pages agree --------------------------------\n");
    writeConfig(QStringLiteral("{}"));
    std::unique_ptr<QDialog> dialog(vshot::createSettingsDialog());
    if (!dialog) {
        std::printf("FAIL  the settings dialog could not be built\n");
        ++failures;
        return;
    }

    QListWidget *sidebar = find<QListWidget>(dialog.get(), "sidebar");
    QStackedWidget *pages = find<QStackedWidget>(dialog.get(), "pages");
    if (sidebar == nullptr || pages == nullptr) {
        return;
    }

    expect(sidebar->count() == pages->count(),
           "there is one sidebar entry per page",
           QStringLiteral("%1 entries, %2 pages")
               .arg(sidebar->count())
               .arg(pages->count()));

    // The settings that moved in this split, plus one that stayed put, each
    // named by the sidebar entry it now belongs under.  A widget wired to a
    // page it does not belong on -- the recording rows left behind on the
    // output page, say -- is what this catches.
    const struct {
        const char *widget;
        const char *section;
    } expected[] = {
        {"pngCompression", "Output"},
        {"monitor", "Output"},
        {"longNotches", "Scrolling capture"},
        {"longInject", "Scrolling capture"},
        {"ocrNotify", "Text recognition"},
        {"recordEncoder", "Recording"},
        {"recordMic", "Recording"},
        {"replayWindow", "Recording"},
        {"replaySaveDir", "Recording"},
        {"pinDensity", "Pin appearance"},
        {"pinRadius", "Pin appearance"},
        {"dialogRadius", "File dialogs"},
        {"width", "Annotation editor"},
    };
    for (const auto &row : expected) {
        QWidget *widget = dialog->findChild<QWidget *>(QString::fromLatin1(row.widget));
        if (widget == nullptr) {
            expect(false, "the setting named in this check exists", row.widget);
            continue;
        }
        // Through `uiTr`, because the sidebar carries the translated section
        // name: this runs under whatever locale the machine has, and the
        // English source text is only what the table is keyed by.
        const QString wanted = vshot::uiTr(row.section);
        const int index = pageOf(widget, pages);
        const QString section =
            (index >= 0 && index < sidebar->count()) ? sidebar->item(index)->text() : QString();
        expect(section == wanted, "the setting is on the page the sidebar names",
               QStringLiteral("%1 -> %2 (wanted %3)")
                   .arg(QString::fromLatin1(row.widget), section, wanted));
    }

    // Every page ends with a stretch, which is what keeps a short page's cards
    // and rows at their natural height instead of spreading them down the
    // viewport.  Without it the cards are spaced out and the window looks
    // broken; with it, `addCard`'s insert-before-the-stretch is what has to
    // keep holding, so the invariant is asserted here rather than left to the
    // eye.
    for (int index = 0; index < pages->count(); ++index) {
        QScrollArea *scroll = qobject_cast<QScrollArea *>(pages->widget(index));
        if (scroll == nullptr) {
            expect(false, "every page is a scroll area", QString::number(index));
            continue;
        }
        QWidget *page = scroll->widget();
        auto *layout = page == nullptr ? nullptr : qobject_cast<QVBoxLayout *>(page->layout());
        if (layout == nullptr) {
            expect(false, "every page has a box layout", QString::number(index));
            continue;
        }
        const int last = layout->count() - 1;
        QLayoutItem *item = last >= 0 ? layout->itemAt(last) : nullptr;
        expect(item != nullptr && item->spacerItem() != nullptr,
               "the page ends with a stretch, so its cards keep their own height",
               sidebar->item(index)->text());
    }
}

/// The numbers the window opens on have to be the numbers the CLI will use.
///
/// The two sides cannot see each other -- the window is C++, the defaults live
/// in the Rust command line -- so every one of them is written down twice, which
/// is the arrangement that drifts.  Someone raises the recording frame rate in
/// `src/record/mod.rs` and the window goes on advertising the old one, which is
/// worse than showing nothing at all: it looks authoritative.  So the values
/// are read back out of the Rust source here, and a change there that the window
/// has not followed fails this check.
void checkTheBuiltInDefaultsAreTheClis()
{
    std::printf("--- the defaults the window shows are the CLI's -------------------\n");
    writeConfig(QStringLiteral("{}"));
    std::unique_ptr<QDialog> dialog(vshot::createSettingsDialog());
    if (!dialog) {
        std::printf("FAIL  the settings dialog could not be built\n");
        ++failures;
        return;
    }

    const QString sourceDir = QString::fromUtf8(VSHOT_SOURCE_DIR);
    auto readSource = [&](const QString &relative) -> QString {
        QFile file(sourceDir + QLatin1Char('/') + relative);
        if (!file.open(QIODevice::ReadOnly)) {
            expect(false, "the source file this check reads is in the tree", relative);
            return QString();
        }
        return QString::fromUtf8(file.readAll());
    };

    // The ones the Rust side gives a name to: `pub const DEFAULT_FPS: u32 = 60;`.
    const struct {
        const char *widget;
        const char *file;
        const char *marker;
    } named[] = {
        {"recordFps", "src/record/mod.rs", "DEFAULT_FPS: u32 = "},
        {"replayFps", "src/record/replay.rs", "DEFAULT_FPS: u32 = "},
        {"replayWindow", "src/record/replay.rs", "DEFAULT_WINDOW: u64 = "},
        {"replayGop", "src/record/replay.rs", "DEFAULT_GOP: u64 = "},
    };
    for (const auto &row : named) {
        const QString source = readSource(QString::fromLatin1(row.file));
        const QString marker = QString::fromLatin1(row.marker);
        const int at = source.indexOf(marker);
        int rust = -1;
        if (at >= 0) {
            QString digits;
            for (int i = at + marker.size(); i < source.size(); ++i) {
                const QChar c = source.at(i);
                if (c.isDigit()) {
                    digits.append(c);
                } else if (c != QLatin1Char('_')) {
                    break;
                }
            }
            rust = digits.toInt();
        }
        QSpinBox *box = dialog->findChild<QSpinBox *>(QString::fromLatin1(row.widget));
        expect(rust >= 0 && box != nullptr && box->value() == rust,
               "the number the window opens on is the CLI's",
               QStringLiteral("%1 shows %2, %3 says %4")
                   .arg(QString::fromLatin1(row.widget))
                   .arg(box == nullptr ? -1 : box->value())
                   .arg(QString::fromLatin1(row.file))
                   .arg(rust));
    }

    // The scrolling-capture ones have no constant of their own: they are inline
    // fallbacks in `src/cli.rs`, of the form
    // `max_height.or(defaults.max_height).unwrap_or(30_000)`.
    const QString cli = readSource(QStringLiteral("src/cli.rs"));
    const struct {
        const char *widget;
        const char *field;
    } inlineDefaults[] = {
        {"longNotches", "notches"},
        {"longMaxHeight", "max_height"},
        {"longMaxFrames", "max_frames"},
        {"longTimeout", "timeout"},
        {"longIgnoreTop", "ignore_top"},
    };
    for (const auto &row : inlineDefaults) {
        const QString field = QString::fromLatin1(row.field);
        const QRegularExpression re(QStringLiteral("%1\\.or\\(defaults\\.%1\\)\\.unwrap_or\\(([0-9_]+)\\)")
                                        .arg(field));
        const QRegularExpressionMatch match = re.match(cli);
        int rust = -1;
        if (match.hasMatch()) {
            rust = match.captured(1).replace(QLatin1Char('_'), QString()).toInt();
        }
        QSpinBox *box = dialog->findChild<QSpinBox *>(QString::fromLatin1(row.widget));
        expect(rust >= 0 && box != nullptr && box->value() == rust,
               "the number the window opens on is the CLI's",
               QStringLiteral("%1 shows %2, cli.rs says %3")
                   .arg(QString::fromLatin1(row.widget))
                   .arg(box == nullptr ? -1 : box->value())
                   .arg(rust));
    }

    // The arrows have to be usable.  They are painted in a strip the spin box's
    // own line edit covers, and a click there used to land on the line edit and
    // do nothing at all -- the arrows were decoration.
    QSpinBox *fps = dialog->findChild<QSpinBox *>(QStringLiteral("recordFps"));
    QLineEdit *fpsEdit = fps == nullptr ? nullptr : fps->findChild<QLineEdit *>();
    if (fps != nullptr && fpsEdit != nullptr) {
        const int before = fps->value();
        // The middle of the upper arrow, in the spin box's coordinates: the
        // strip starts 26px from the right edge and is 16px wide.
        const QPoint onUp(fps->width() - 26 + 8, fps->height() / 4);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(fpsEdit->mapFrom(fps, onUp)),
                          QPointF(fps->mapToGlobal(onUp)), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(fpsEdit, &press);
        expect(fps->value() == before + 1, "a click on the up arrow steps the box up",
               QStringLiteral("%1 -> %2").arg(before).arg(fps->value()));
        const QPoint onDown(fps->width() - 26 + 8, fps->height() * 3 / 4);
        QMouseEvent pressDown(QEvent::MouseButtonPress, QPointF(fpsEdit->mapFrom(fps, onDown)),
                              QPointF(fps->mapToGlobal(onDown)), Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
        QApplication::sendEvent(fpsEdit, &pressDown);
        expect(fps->value() == before, "a click on the down arrow steps the box back down",
               QStringLiteral("%1").arg(fps->value()));
    }

    // A default left alone must stay out of the file.  The file is for what the
    // user chose; a number in it would freeze today's default and stop a later
    // version's better one from ever reaching them.
    find<QPushButton>(dialog.get(), "saveButton")->click();
    QFile file(configPath());
    if (!file.open(QIODevice::ReadOnly)) {
        expect(false, "the config file can be read back after a save");
        return;
    }
    const QString saved = QString::fromUtf8(file.readAll());
    expect(!saved.contains(QStringLiteral("30000")) && !saved.contains(QStringLiteral("6000")),
           "defaults nobody touched are not written into the file", saved.trimmed());

    // The leading entry of each combo box names the built-in default rather than
    // saying the word "default", so a user can read which level or codec they
    // get without guessing.
    QComboBox *compression = find<QComboBox>(dialog.get(), "pngCompression");
    expect(compression->currentIndex() == 0 &&
               compression->itemText(0).contains(QStringLiteral("fast")),
           "the compression box names its built-in default", compression->itemText(0));
    QComboBox *encoder = find<QComboBox>(dialog.get(), "recordEncoder");
    expect(encoder->currentIndex() == 0 && encoder->itemText(0).contains(QStringLiteral("h264")),
           "the encoder box names its built-in default", encoder->itemText(0));
    // The pin density is the one box whose zero is an answer of its own rather
    // than "unset", so it is the one that keeps a special text.
    QSpinBox *density = find<QSpinBox>(dialog.get(), "pinDensity");
    expect(density->value() == 0 && !density->specialValueText().isEmpty(),
           "the pin density opens on zero and says what that means",
           density->specialValueText());
}

void checkTheDesktopEntryAndIconAgree()
{
    std::printf("--- the launcher entry, the icon and the window agree --------------\n");
    // Wayland gives a window no icon of its own: the compositor takes the
    // application id the client declares, looks for `<id>.desktop` and draws
    // whatever `Icon=` names.  Every link in that chain fails silently -- a
    // renamed desktop file, an Icon= pointing at a file that is not installed,
    // or an app id that no longer matches -- and the window just comes up with
    // a generic placeholder.  So the links are checked here.
    const QString sourceDir = QString::fromUtf8(VSHOT_SOURCE_DIR);
    const QString desktopPath = sourceDir + QStringLiteral("/vshot-settings.desktop");
    QFile desktop(desktopPath);
    if (!desktop.open(QIODevice::ReadOnly)) {
        expect(false, "the settings desktop file is in the source tree", desktopPath);
        return;
    }
    const QString contents = QString::fromUtf8(desktop.readAll());

    // The id the window declares has to be the file's own name, or the
    // compositor never finds it -- and it has to be declared only when that
    // file is installed, because Qt also registers it with the host portal and
    // an id with no desktop file behind it makes the portal log a failure on
    // every start.  Both halves are read out of the source, since neither is
    // observable without a compositor and an installed package.
    QFile mainSource(sourceDir + QStringLiteral("/ui/main.cpp"));
    QString declared;
    QString guard;
    if (mainSource.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(mainSource.readAll());
        const QString marker = QStringLiteral("setDesktopFileName(QStringLiteral(\"");
        const int at = text.indexOf(marker);
        if (at >= 0) {
            const int start = at + marker.size();
            declared = text.mid(start, text.indexOf(QLatin1Char('"'), start) - start);
        }
        // The condition the call sits behind, e.g.
        //   if (!QStandardPaths::locate(...).isEmpty()) {
        const QString guardMarker = QStringLiteral("QStandardPaths::locate");
        if (text.contains(guardMarker)) {
            guard = guardMarker;
        }
    }
    expect(declared == QStringLiteral("vshot-settings"),
           "the window declares the desktop file name the entry is installed as", declared);
    expect(QFileInfo(desktopPath).fileName() == declared + QStringLiteral(".desktop"),
           "the desktop file is named after that id",
           QFileInfo(desktopPath).fileName());
    expect(!guard.isEmpty(),
           "that id is declared only when the desktop file is installed", guard);

    // `Icon=` has to name an icon that exists, and the package has to install
    // it under that name.
    const QStringList lines = contents.split(QLatin1Char('\n'));
    QString iconName;
    QString exec;
    for (const QString &line : lines) {
        if (line.startsWith(QStringLiteral("Icon="))) {
            iconName = line.mid(5).trimmed();
        } else if (line.startsWith(QStringLiteral("Exec="))) {
            exec = line.mid(5).trimmed();
        }
    }
    expect(!iconName.isEmpty(), "the entry names an icon", iconName);
    expect(!exec.isEmpty(), "the entry names a command", exec);
    expect(exec.endsWith(QStringLiteral("vshot settings")),
           "the entry opens the settings window", exec);

    const QString iconPath =
        sourceDir + QStringLiteral("/icons/") + iconName + QStringLiteral(".svg");
    expect(QFileInfo::exists(iconPath), "the icon it names is in the source tree", iconPath);

    QFile pkgbuild(sourceDir + QStringLiteral("/PKGBUILD"));
    if (pkgbuild.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(pkgbuild.readAll());
        expect(text.contains(QStringLiteral("vshot-settings.desktop")),
               "the package installs the launcher entry");
        expect(text.contains(QStringLiteral("icons/") + iconName + QStringLiteral(".svg")),
               "the package installs the icon");
    } else {
        expect(false, "the PKGBUILD is in the source tree");
    }
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        std::printf("FAIL  no temporary directory to run in\n");
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", scratch.path().toUtf8());
    std::printf("config: %s\n", qPrintable(configPath()));

    checkEveryFieldReachesTheFile();
    checkTheWindowOpensOnTheStoredValues();
    checkClearingOneColorLeavesTheOther();
    checkCancelChangesNothing();
    checkTheOcrEngineSurvivesASave();
    checkEverySettingIsOnThePageTheSidebarNames();
    checkTheBuiltInDefaultsAreTheClis();
    checkTheDesktopEntryAndIconAgree();

    std::printf("--- result ---------------------------------------------------------\n");
    std::printf("%s (%d failure(s))\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}

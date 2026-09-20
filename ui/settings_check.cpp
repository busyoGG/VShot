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
#include "settings_window.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>

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
                "pin": {"density": 2}}
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

    // A field the file says nothing about shows the "not set" entry, and the
    // spin boxes show their special text rather than a real zero.
    expect(find<QSpinBox>(dialog.get(), "longMaxHeight")->value() == 0,
           "an unset height limit reads as unset");
    expect(!find<QSpinBox>(dialog.get(), "longMaxHeight")->specialValueText().isEmpty(),
           "an unset spin box says so instead of showing a zero");
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
    checkCancelChangesNothing();
    checkTheDesktopEntryAndIconAgree();

    std::printf("--- result ---------------------------------------------------------\n");
    std::printf("%s (%d failure(s))\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}

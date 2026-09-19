#pragma once

#include <QColor>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace vshot {

/// The editor's remembered style, as the user left it.
///
/// Every field here has the same default the editor hard-codes, so a missing
/// or unreadable config file leaves the tool behaving exactly as it did before
/// this existed.  The values are the *starting* style for a new session; the
/// style the user picks during a session is what gets written back.
struct EditorPreferences {
    /// Tool the toolbar opens with: select | rectangle | ellipse | arrow | pen
    /// | text | mosaic.
    QString tool = QStringLiteral("select");
    QColor color{255, 64, 64, 255};
    QString font;
    std::uint32_t width = 2;
    std::uint32_t textSize = 2;
    QString dash = QStringLiteral("solid");
    std::uint32_t arrowSize = 1;
    QString arrowStyle = QStringLiteral("open");
    QString mosaicShape = QStringLiteral("rect");
    std::uint32_t mosaicStrength = 2;
};

/// The command-line defaults the `cli` section supplies.
///
/// Unlike the editor style, every field here is genuinely optional: a zero or
/// an empty string means "the file says nothing", and the CLI keeps its own
/// built-in default for that flag.  That is what lets the settings window tell
/// "the user wants `fast`" apart from "the user never touched this".
struct CliPreferences {
    QString pngCompression; ///< `none` | `fastest` | `fast` | `balanced` | `high`
    QString monitor;        ///< an output name, or `current`
    std::uint32_t longNotches = 0;
    std::uint32_t longMaxHeight = 0;
    std::uint32_t longMaxFrames = 0;
    std::uint64_t longTimeout = 0;
    std::uint32_t longIgnoreTop = 0;
    QString longInject;     ///< `auto` | `wlr` | `portal` | `uinput`
    std::uint32_t pinDensity = 0;
};

/// Both sections of the shared config file.
struct Config {
    EditorPreferences editor;
    CliPreferences cli;
};

/// The absolute path of the config file: `$XDG_CONFIG_HOME/vshot/config.json`,
/// falling back to `$HOME/.config/vshot/config.json`.  Empty when neither
/// variable is set, in which case there is nowhere to remember anything.
QString configFilePath();

/// The `#rrggbb` or `#rrggbbaa` spelling of a color, and what a hand-written
/// value in the file is parsed as.  It is the CSS form rather than Qt's own
/// `#aarrggbb`, because the file is meant to be written by hand.
QString colorText(const QColor &color);
/// Parses the spellings [`colorText`] writes; an invalid color on anything else.
QColor parseColorText(const QString &text);

/// Reads both sections.  A missing, unreadable, or malformed file yields the
/// built-in defaults rather than an error: a config file is a convenience, and
/// losing it must never cost the user a screenshot.  Values are read field by
/// field, so one bad entry costs only itself.
Config loadConfig();

/// Writes both sections back, creating the directory if needed.
///
/// The file is *merged*, not replaced: keys this build does not know about —
/// a newer vshot's, or one the user added by hand — survive untouched.  That
/// is what lets the settings window save one section without wiping the other.
/// Returns false when there was nowhere to write or the write failed; the
/// capture path ignores that on purpose, because not being able to remember a
/// color is not worth interrupting a screenshot over, while the settings window
/// shows it, because there the write *is* what the user asked for.
bool saveConfig(const Config &config);

/// The editor's half of [`loadConfig`].
EditorPreferences loadEditorPreferences();

/// Writes the editor's half, leaving everything else in the file alone.
/// Returns false on the same terms as [`saveConfig`].
bool saveEditorPreferences(const EditorPreferences &preferences);

/// The accepted values for each enumerated field, in the order the settings
/// window should offer them.  The loaders use the same lists, so a value the
/// window offers is always one the file will accept.
const QStringList &toolNames();
const QStringList &dashNames();
const QStringList &arrowStyleNames();
const QStringList &mosaicShapeNames();
const QStringList &compressionNames();
const QStringList &injectNames();

} // namespace vshot

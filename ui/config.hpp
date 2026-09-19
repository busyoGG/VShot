#pragma once

#include <QColor>
#include <QString>

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

/// The absolute path of the config file: `$XDG_CONFIG_HOME/vshot/config.json`,
/// falling back to `$HOME/.config/vshot/config.json`.  Empty when neither
/// variable is set, in which case there is nowhere to remember anything.
QString configFilePath();

/// Reads the remembered editor style.  A missing, unreadable, or malformed
/// file yields the built-in defaults rather than an error: a config file is a
/// convenience, and losing it must never cost the user a screenshot.
EditorPreferences loadEditorPreferences();

/// Writes the editor style back, creating the directory if needed.  Failures
/// are silent for the same reason: not being able to remember a color is not
/// worth interrupting a capture over.
void saveEditorPreferences(const EditorPreferences &preferences);

} // namespace vshot

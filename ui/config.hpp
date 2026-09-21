#pragma once

#include "shadow.hpp"

#include <QColor>
#include <QPalette>
#include <QSize>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace vshot {

/// The style a session starts from.
///
/// Every field here has the same default the editor hard-codes, so a missing
/// or unreadable config file leaves the tool behaving exactly as it did before
/// this existed.  These are *reset* values, not a record of the last session:
/// nothing a capture does is written back, and the file changes only when the
/// user means it -- a save in the settings window, or a hand edit.
struct EditorPreferences {
    /// Tool the toolbar opens with: select | rectangle | ellipse | arrow | pen
    /// | text | mosaic.
    QString tool = QStringLiteral("select");
    QColor color{255, 64, 64, 255};
    QString font;
    std::uint32_t width = 2;
    /// Font height of a text label, **in pixels**: the same number the editor's
    /// size box shows, so 14 means a 14-pixel label.  The legacy integer scale
    /// the JSON protocol carries is derived from it in `ui/text_size.hpp`.
    std::uint32_t textSize = 14;
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

/// The look of the file dialogs.
///
/// These live in the config file rather than in the dialog's code because the
/// dialog is a layer surface: a compositor draws no decoration on one, so the
/// rim and the shadow this describes are the only things separating the dialog
/// from whatever is behind it, and how heavy they should be is the user's call
/// rather than ours.
struct DialogPreferences {
    /// Corner radius, in logical pixels.  Zero is a plain rectangle.
    std::uint32_t radius = 12;
    /// Stroke width of the rim, in logical pixels.  Zero draws no rim at all.
    std::uint32_t borderWidth = 1;
    /// The rim's colour.  The default is invalid, meaning "derive one from the
    /// palette" -- which is what follows a light or dark colour scheme without
    /// the user having to spell out a colour for each.
    QColor borderColor;
    /// The shadow the dialog casts, in the same terms a pin's is described in:
    /// one set of numbers for both, so tuning one does not mean learning a
    /// second vocabulary for the other.
    ShadowStyle shadow;
};

/// The ceiling on a pin's corner radius and on its border width, in logical
/// pixels.  They live here rather than in `config.cpp` because the settings
/// window has to offer the same range the loader accepts: a value the file
/// takes but the window cannot show would come back clamped the next time that
/// page was saved.
constexpr int kMaxPinRadius = 512;
constexpr int kMaxPinBorderWidth = 8;
/// The same ceilings for the file dialog's frame.  They live beside the pin's
/// for the same reason: the settings window has to offer the range the loader
/// accepts, and a hand-written value past what the window can show would come
/// back clamped the next time that page was saved.
constexpr int kMaxDialogRadius = 48;
constexpr int kMaxDialogBorderWidth = 8;
/// The same ceilings for a shadow's blur reach, its drop, and its alpha.  They
/// are shared by the pin and the dialog, which is the point: one pair of
/// numbers describes both, and the settings window offers one range for each.
constexpr int kMaxShadowSize = 64;
constexpr int kMaxShadowOffset = 32;
constexpr int kMaxShadowOpacity = 255;

/// The look of a pinned image.
///
/// A pin has no decoration from anyone else either -- it is a layer surface
/// with nothing but the image in it -- so its corners, the shadow that lifts it
/// off what is behind it and the stroke that says where it ends are all ours to
/// draw, and how much of each is a matter of what the user has on screen behind
/// them.
///
/// This is a different thing from `cli.pin`, which is the *density* a capture
/// is pinned at; that one is a command-line default, this one is how the result
/// looks.
struct PinPreferences {
    /// Corner radius in logical pixels.  Zero -- the default -- draws square
    /// corners, which is what a screenshot wants: it is a picture of a window,
    /// not a card.
    std::uint32_t radius = 0;
    /// The soft shadow behind every pin, on by default: a screenshot pinned
    /// over a window of its own colour is otherwise impossible to place.
    ShadowStyle shadow;
    /// Stroke width of the rim, in logical pixels.  Zero draws no rim at all.
    std::uint32_t borderWidth = 2;
    /// The rim's colour on every pin that is not the one the keyboard would act
    /// on.  The default is invalid, meaning the built-in light grey.
    QColor borderColor;
    /// The rim's colour on the pin the keyboard would act on -- the one the
    /// user last clicked on this output.  The default is invalid, meaning the
    /// built-in black.
    QColor activeBorderColor;
};

/// Every section of the shared config file.
struct Config {
    EditorPreferences editor;
    CliPreferences cli;
    DialogPreferences dialog;
    PinPreferences pin;
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

/// The file dialogs' half of [`loadConfig`].
DialogPreferences loadDialogPreferences();

/// The pins' half of [`loadConfig`].
PinPreferences loadPinPreferences();

/// The rim colour a pin uses in one of its two states: the user's own when the
/// file carries one, and otherwise the built-in.  Read through one function so
/// "which colour is this pin" cannot be answered two ways in two places.
///
/// The corner radius has no such reader: it is clamped against the painted
/// image's size, which only the surface knows -- see `paintRadius` in
/// `pin_surface.cpp`.
QColor resolvePinBorderColor(const PinPreferences &preferences, bool active);

/// The radius a dialog should use, never past what its own size can carry.  A
/// corner wider than half the shorter side would turn the rim inside out, which
/// is what a hand-written 48 would do to a dialog on a short screen.
int resolveDialogRadius(const DialogPreferences &preferences, const QSize &size);

/// The rim's colour.  The preferences' own colour when they carry one, and
/// otherwise a stroke derived from the dialog's own colours: a third of the way
/// from `surface` to `text`, so it is darker on a light scheme and lighter on a
/// dark one without a second constant to keep in step.
///
/// Both colours are passed in rather than read from a palette because the
/// dialog's stylesheet destroys the surface colour on the way in -- see
/// FramedFileDialog, which captures it before that happens.
QColor resolveDialogBorderColor(const DialogPreferences &preferences, const QColor &surface,
                                const QColor &text);

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

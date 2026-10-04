// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

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
    /// Tool the toolbar opens with: rectangle | ellipse | arrow | pen | text |
    /// mosaic.
    ///
    /// Empty is its own value here, and the one a fresh session has: nothing
    /// armed, which is the state that adjusts the marks and the selection
    /// without drawing anything.  The toolbar has a Select button for it, but
    /// the file does not name it: a config written before that tool went away
    /// carries "select", and it is read as this same empty string.
    QString tool;
    /// How a drag on a mark that is already selected is started: `precise`
    /// (the default) needs the press on the mark itself, `loose` moves it from
    /// anywhere on screen.
    ///
    /// A mark that is hard to hit -- a hairline pen stroke, a run of small text
    /// -- is what the second mode is for: once it is selected, the whole screen
    /// is its handle.  The cost is that a press elsewhere no longer reaches the
    /// capture selection, so a click (a press that does not move) drops the
    /// mark instead, and the next drag starts from nothing selected.
    QString selectMode = QStringLiteral("precise");
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
    /// How the HDR half of a capture is written: `avif` or `hdr`.  Empty means
    /// the file says nothing and the built-in default (AVIF) stands.
    QString hdrFormat;
    /// How the SDR copy of an HDR capture is mapped down: `auto`, `fixed` or
    /// `normalize`.  Empty means the file says nothing and the built-in default
    /// (auto) stands.
    QString toneMap;
    /// Where SDR white lands for the modes that take a level, as a fraction of
    /// the range.  Zero means the file says nothing; the map's own default is
    /// what stands, which is why a written value is never zero.
    double toneMapWhite = 0.0;
    /// Whether HDR content is judged by the share of the capture brighter than
    /// SDR white rather than by any single pixel (`cli.hdr-area-test`).  On when
    /// the file says nothing, which is what the built-in default is, so only
    /// "off" is ever written.
    bool hdrAreaTest = true;
    // Which way the picker reads a window's elements when the accessibility
    // tree cannot answer for it.  `lines` finds the dividers the interface
    // draws, `components` finds the areas of one colour; the two fail on
    // different interfaces, so it is a choice rather than a replacement.
    QString elementFallback = QStringLiteral("lines");
    /// The share that share has to reach (`cli.hdr-area-ratio`), in [0, 1].
    ///
    /// Negative means the file says nothing and the built-in default stands.
    /// Unlike the tone-map white level, **zero is a value here**: the switch on
    /// with a ratio of zero is the state that treats every capture of an HDR
    /// output as HDR content, so it has to be writable and has to read back as
    /// itself rather than as an absent key.
    double hdrAreaRatio = -1.0;
    QString monitor;        ///< an output name, or `current`
    std::uint32_t longNotches = 0;
    std::uint32_t longMaxHeight = 0;
    std::uint32_t longMaxFrames = 0;
    std::uint64_t longTimeout = 0;
    std::uint32_t longIgnoreTop = 0;
    QString longInject;     ///< `auto` | `wlr` | `portal` | `uinput`
    std::uint32_t pinDensity = 0;
    /// Which codec `record --encoder` falls back to: `h264` (the built-in
    /// default), `hevc` or `av1`.  Empty means the file says nothing.
    QString recordEncoder;
    /// Which hardware encoder `record --encoder-backend` falls back to: `auto`
    /// (the built-in default), `vaapi` or `nvenc`.  Empty means the file says
    /// nothing.
    QString recordEncoderBackend;
    /// The frame rate `record --fps` falls back to, 1-240; zero means the file
    /// says nothing.  A rate outside that range is read as "nothing" rather
    /// than clamped, because that is what vshot itself does with one.
    std::uint32_t recordFps = 0;
    /// The target bitrate `record --bitrate` falls back to, in Mbit/s; zero
    /// means the file says nothing and the recording derives one from the
    /// frame's own size.  There is no ceiling on it beyond the encoder's own:
    /// a bitrate is the user's to choose.
    std::uint32_t recordBitrate = 0;
    /// The encoder level `record --quality` falls back to, and whether the file
    /// names one at all.
    ///
    /// Two fields rather than the usual zero-means-nothing, because zero *is* a
    /// level: it is the most expensive one either scale has.  The scale is the
    /// codec's own and the number is stored as written -- 0-51 for h264 and
    /// hevc, 0-255 for av1 -- since converting between them would be inventing
    /// a meaning the encoder never agreed to.
    bool recordQualitySet = false;
    std::uint32_t recordQuality = 0;
    /// Whether a recording goes through the desktop portal without `--portal`.
    /// Off when the file says nothing, which is also what an absent key means.
    bool recordPortal = false;
    /// Whether a recording takes a microphone at all (`cli.record.mic`), and
    /// which one.  Silence is the absent key, so the pair is not one field:
    /// `false` writes no key at all, while `true` with an empty name writes the
    /// empty string, which is what "the session's default input" means to
    /// `vshot record --mic`.
    bool recordMicEnabled = false;
    QString recordMic;
    /// The windows a `record window` follows without `--follow`: the config's
    /// `cli.record.follow`, in the order written.  An empty list is the absent
    /// key -- nothing to follow -- so it writes nothing back.
    QStringList recordFollow;
    /// Whether a finished recording raises a desktop notification
    /// (`cli.record.notify`).  On when the file says nothing, the same rule as
    /// `ocrNotify`, and only `false` is ever written.
    bool recordNotify = true;

    /// The replay-side twins of the block above.  A replay is configured by its
    /// own `cli.replay` section, so every field here reads and writes a
    /// `replay.*` key rather than sharing the recording's: the two sessions
    /// have different sensible defaults (a replay is left running for hours at
    /// 30 fps; a recording runs for minutes at 60), and one section holding
    /// both would have to guess which was meant.
    ///
    /// How many seconds of history the ring keeps (`cli.replay.window`); zero
    /// means the file says nothing.
    std::uint32_t replayWindow = 0;
    /// The key-frame distance in seconds (`cli.replay.gop`, 1-10); zero means
    /// the file says nothing.
    std::uint32_t replayGop = 0;
    /// The replay codec (`cli.replay.encoder`); empty means the file says
    /// nothing.
    QString replayEncoder;
    /// The replay hardware backend (`cli.replay.encoder-backend`); empty means
    /// the file says nothing.
    QString replayEncoderBackend;
    /// The replay frame rate (`cli.replay.fps`); zero means the file says
    /// nothing.
    std::uint32_t replayFps = 0;
    /// The replay target bitrate (`cli.replay.bitrate`), on the recording
    /// side's terms.  A ring is held in memory, so this is what decides what a
    /// long `--window` costs.
    std::uint32_t replayBitrate = 0;
    /// The replay encoder level (`cli.replay.quality`), on the recording side's
    /// two-field terms.
    bool replayQualitySet = false;
    std::uint32_t replayQuality = 0;
    /// Whether a replay uses the portal (`cli.replay.portal`); off when the file
    /// says nothing.
    bool replayPortal = false;
    /// The replay microphone, on the same two-state terms as the recording's.
    bool replayMicEnabled = false;
    QString replayMic;
    /// The windows a `replay start window` follows without `--follow`.
    QStringList replayFollow;
    /// Where a triggered save lands when it names no path (`cli.replay.save-dir`,
    /// strftime-expanded); empty means the videos directory.
    QString replaySaveDir;
    /// Whether a triggered save raises a notification (`cli.replay.notify`); on
    /// when the file says nothing, and only `false` is written.
    bool replayNotify = true;
    /// Whether a finished text recognition raises a desktop notification
    /// (`cli.ocr.notify`).  Notifications are on when the file says nothing, so
    /// the switch shows the state the engine will actually run in, and the
    /// written key is the exception among these: only `false` is stored, since
    /// an absent key already means on.
    bool ocrNotify = true;
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

/// How far a frame's numbers may go: a pin's corner radius, a pin's rim, the
/// file dialog's corner radius and its rim, all in logical pixels.
///
/// There is no ceiling worth having on any of the four.  A radius is clamped to
/// half the shape where it is drawn -- a corner cannot be rounder than the
/// surface it is on -- and a rim is a pen width, so neither costs anything as it
/// grows and neither stops meaning something at some size.  They used to be
/// capped separately, at 512 and 8 for the pin and 48 and 8 for the dialog,
/// which was us deciding how round the user's corners were allowed to be.
///
/// The number is not the whole of an `int` on purpose: the repaint region a rim
/// is added to is a `QRect`, and a `QRect` grown by a billion on every side is
/// arithmetic that overflows rather than a rectangle.  A million logical pixels
/// is a thousand screens, so nothing a user can want is refused, and the sums
/// stay in range.
///
/// It lives here rather than in `config.cpp` because the settings window has to
/// offer the same range the loader accepts: a value the file takes but the
/// window cannot show would come back clamped the next time that page was saved.
constexpr int kMaxFrameValue = 1'000'000;

/// The ceilings on a shadow's blur reach and on its drop, in logical pixels.
/// They are shared by the pin and the dialog, which is the point: one pair of
/// numbers describes both, and the settings window offers one range for each.
///
/// These two are the only look values that do have a ceiling, because they are
/// the only ones that cost anything as they grow: the blur is built over the
/// shape grown by the reach on every side, so the mask is a square of that size,
/// and the dialog's own surface is grown by the reach too -- a hundred
/// megabytes of backing store for a haze nobody can see past.  Half a thousand
/// logical pixels of reach is already further than a 4K screen is tall, so what
/// is left out is a shadow that would be drawn mostly off screen.
constexpr int kMaxShadowSize = 512;
/// The highest encoder level any of the three codecs takes: H.264 and HEVC
/// stop at 51, AV1 at 255, and this is the wider of the two.  A level above
/// the chosen codec's own range is refused by that encoder with its own
/// message, which is the only place that knows the range.
constexpr int kMaxQuality = 255;
/// The bitrate ceiling the settings window and the settings *file* keep, in
/// Mbit/s.  It is not a policy: the command line has no ceiling at all, and
/// this one only exists so that a hand-edited number cannot overflow the spin
/// box that shows it.  It sits far above anything an encoder takes -- 1 Tbit/s.
constexpr int kMaxBitrate = 1'000'000;
/// The bitrate a recording aims for when the caller named none, in Mbit/s, and
/// the number the settings window opens its box on.
///
/// It is `avcodec::DEFAULT_BITRATE` on the Rust side, which is what actually
/// encodes the file; the two are separate languages and cannot share the
/// number, so each side pins it and each side has a check that says so.
constexpr int kDefaultBitrate = 45;
constexpr int kMaxShadowOffset = 512;
constexpr int kMaxShadowOpacity = 255;

/// The ceilings on a mark's own style, in the units the toolbar's controls show
/// them in.  They live here rather than in `config.cpp` because the session
/// reader has to accept exactly the marks the editor writes back: a value the
/// file takes but the toolbar cannot show would come back clamped the next time
/// that mark was touched.
constexpr int kMaxWidth = 64;
constexpr int kMaxArrowSize = 8;
constexpr int kMaxMosaicStrength = 3;

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
/// The two ways the Select tool may pick up an annotation: `precise` (the
/// press has to land on the mark) and `loose` (once selected, the mark follows
/// a drag that starts anywhere).  Offered by the settings window in this order,
/// and read back by the loader from the same list.
const QStringList &selectModeNames();
const QStringList &dashNames();
const QStringList &arrowStyleNames();
const QStringList &mosaicShapeNames();
const QStringList &compressionNames();
/// The HDR half's format: `avif`, `hdr`.
const QStringList &hdrFormatNames();
/// How the SDR half is mapped down from the HDR one: `auto`, `fixed`,
/// `normalize`.  The names are the ones `--tone-map` accepts.
const QStringList &toneMapNames();

/// The `--element-fallback` values, in the order the settings window offers
/// them: `lines` first, because it is the default.
const QStringList &elementFallbackNames();
/// The span a tone-map white level may take, as a fraction of the range: the
/// same numbers `model::hdr::ToneMapOptions` clamps to, repeated here because
/// the settings window has to bound its number box by them.
constexpr double kMinToneMapWhite = 0.5;
constexpr double kMaxToneMapWhite = 0.95;
/// What the white level is when the file says nothing: the map's own default.
constexpr double kDefaultToneMapWhite = 0.8;
/// What the HDR area test's ratio is when the file says nothing: the same floor
/// `model::hdr::HdrDecision::default` weighs a capture against, repeated here
/// because the settings window has to open its number box on it.
constexpr double kDefaultHdrAreaRatio = 0.0005;
const QStringList &injectNames();
const QStringList &encoderNames();
/// The hardware backends `record --encoder-backend` and its replay twin accept:
/// `auto`, `vaapi`, `nvenc`.  The settings window offers exactly these.
const QStringList &encoderBackendNames();

} // namespace vshot

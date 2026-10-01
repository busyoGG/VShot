// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#pragma once

#include "session_protocol.hpp"
#include "shortcuts.hpp"
#include "text_layer.hpp"
#include "translation_layer.hpp"

#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QHash>
#include <QImage>
#include <QJsonDocument>
#include <QObject>
#include <QPointF>
#include <QRect>
#include <QRegion>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

class QPainter;
class QScreen;
class QSlider;
class QSpinBox;
class QLabel;
class QWindow;
class QLocalSocket;
class QSocketNotifier;
class QTimer;

namespace vshot {

// Rasterizes one annotation and remembers the result.  Defined in the Qt
// helper; `Annotation` only holds its cache.  Forward-declared so the cache
// stays a `std::shared_ptr` and a copy of an annotation (an undo snapshot, a
// drag ghost) stays cheap.
class AnnotationRaster;

struct Point {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

inline bool operator==(const Point &first, const Point &second)
{
    return first.x == second.x && first.y == second.y;
}

// The looks a numbered badge can be drawn with.  The number tool is one tool;
// these four are the styles the editor offers for it.
enum class NumberStyle {
    // A filled disc with the count knocked out of it -- the ①②③ look.
    FilledCircle,
    // A hollow ring whose line is the current stroke width.
    Ring,
    // A rounded square filled like the disc.
    Square,
    // No background at all: the glyphs alone, given a thin contrasting halo so
    // they stay readable over a busy frame.
    Plain,
};

struct Annotation {
    enum class Kind {
        Shape,
        Stroke,
        Text,
        // A pasted image. `pixels` holds the source at its own resolution and
        // `rect` is where it lands on the canvas: the two differ because a
        // pasted image is scaled to fit inside the selection and the user can
        // then resize it with the handles. The pixels travel to the renderer as
        // a raw RGBA8888 file, the same way a text label's bitmap does.
        Image,
        // A translated capture: one placed line per recognized line, drawn over
        // the text it replaces. `translation` holds the geometry, the text and
        // the colours, all settled when the translation came back, and `rect`
        // is the union of the filled boxes -- what the hit test and the drag
        // clamp read. It travels to the renderer as an image bitmap, so the
        // Rust side needs no knowledge of it at all.
        Translation,
    };

    Kind kind = Kind::Stroke;
    QString tool;
    LogicalRect rect;
    QVector<Point> points;
    Point origin;
    QString text;
    // Whether a bezier path was closed back onto its first anchor (`tool ==
    // "bezier"` only).  A closed path is filled as well as stroked, so this is
    // content rather than decoration: it changes the pixels and travels to the
    // renderer as its own field.
    bool closed = false;
    // Font height in logical pixels, exactly as the size box shows it.  The
    // legacy integer `scale` the JSON protocol carries is derived from this
    // only when the result is written out (`textPixelsToScale`).
    std::uint32_t textPixels = 14;
    QColor color{255, 64, 64, 255};
    std::uint32_t width = 1;
    // Line style: "solid" | "dashed" | "dotted".
    QString dash = QStringLiteral("solid");
    // Arrow head size multiplier.
    std::uint32_t size = 1;
    // Arrow head style: "open" | "filled".
    QString arrowStyle = QStringLiteral("open");
    // Mosaic area shape: "rect" | "ellipse".
    QString mask = QStringLiteral("rect");
    // Mosaic strength level 1..3 (block size / smear radius factor).
    std::uint32_t strength = 2;
    // Text font family; empty resolves to the application default font.
    QString font;
    // A numbered badge (`Kind::Text` with `tool == "number"`): which of the four
    // looks it is drawn with, and the count it carries.  `number` is the value
    // the digit string is written from -- the model keeps one source of truth
    // for it so that two badges at the same place with different counts are
    // plainly different marks, which is what the undo comparison needs.
    NumberStyle numberStyle = NumberStyle::FilledCircle;
    int number = 0;
    // Diameter of a numbered badge, in logical pixels.  The badge's size used to
    // be derived from the stroke width, so the width slider silently resized a
    // placed badge; it is a value of its own now, and `width` means nothing to a
    // badge.
    std::uint32_t numberSize = 18;
    // A wave's shape, in logical pixels: how far a crest leaves the line its two
    // points describe, and how long one full period is.  Zero means "derive it
    // from the stroke width" -- which is what a wave that was never tuned keeps,
    // so the default look is unchanged and the wire may leave both out.
    std::uint32_t amplitude = 0;
    std::uint32_t wavelength = 0;
    // How a bezier path is painted: "stroke" outlines it, "fill" fills it, and
    // "both" does both.  "both" fills only a path that was actually closed, so
    // it is exactly what the editor did before this field existed.
    QString fill = QStringLiteral("both");
    // Output scale the label was drawn on; the text bitmap is rasterized at
    // this device ratio.
    std::uint32_t deviceRatio = 1;
    // The pasted image itself, at its own resolution (`Kind::Image` only).
    QImage pixels;
    // The placed translation (`Kind::Translation` only): one entry per
    // recognized line, holding the rectangle it replaces, the fill and the text
    // drawn in it.  Everything the paint needs is here, so the preview and the
    // committed bitmap are drawn from the same numbers.
    QVector<TranslatedLine> translation;
    // The last rasterized form of this annotation, kept so a repaint can blit
    // it instead of drawing the mark again.  A mosaic preview averages the
    // source image block by block, so redrawing every mark on every pointer
    // move is what made a busy capture stutter; the raster rebuilds itself
    // only when something the mark draws changes.  Shared (so copies are
    // cheap) and deliberately ignored by `annotationEquals`: it is derived
    // state, not content.
    mutable std::shared_ptr<AnnotationRaster> raster;

    // How many times `raster` has been built, or -1 when it has not been built
    // yet.  Lets the offline check tell a repaint that reused the cache from
    // one that rasterized the mark again, without seeing the raster's type.
    int rasterRebuilds() const;

    // The device-pixel ratio `raster` was built at, or 0 while it has not been
    // built.  Lets the offline check prove a high-DPI capture rasterizes at the
    // screen's resolution instead of blurring.
    qreal rasterDeviceRatio() const;
};

// A numbered badge's diameter range, in logical pixels: floored at 18 so the
// count stays legible and capped at 96 so it does not paint a billboard.  The
// editor's own size control and the session reader both clamp to these, so a
// badge that arrives from the daemon lands in the range the control can show.
constexpr int kNumberMinDiameter = 18;
constexpr int kNumberMaxDiameter = 96;

// The diameter a badge's stored size means, clamped into that range.
int numberDiameter(std::uint32_t size);

// The four looks, read from the tag the wire carries.
NumberStyle numberStyleForName(const QString &value);

// Lays a badge's box out around `center` for the diameter it carries.  The box
// is not decoration: the hit test, the drag clamp, the raster cache and the
// bitmap the renderer is handed are all sized from it, so it is re-derived
// wherever the diameter changes.
void layoutNumberBox(Annotation &annotation, Point center);

inline bool annotationEquals(const Annotation &first, const Annotation &second)
{
    if (first.kind != second.kind || first.tool != second.tool || first.dash != second.dash ||
        first.size != second.size || first.arrowStyle != second.arrowStyle ||
        first.mask != second.mask || first.strength != second.strength ||
        first.textPixels != second.textPixels || first.color != second.color ||
        first.width != second.width || first.font != second.font ||
        first.deviceRatio != second.deviceRatio ||
        // A numbered badge's count and style are content, not decoration: leave
        // either out and two badges that differ only in their number compare
        // equal, which silently collapses an undo step.
        first.numberStyle != second.numberStyle || first.number != second.number ||
        // A badge's diameter is the badge: two badges of different sizes are
        // plainly different marks.
        first.numberSize != second.numberSize ||
        // A wave's tuned shape is content too -- dragging the amplitude slider
        // must be an undoable change, not a repaint the comparison eats.
        first.amplitude != second.amplitude || first.wavelength != second.wavelength ||
        // And so is a pen path's paint mode: the same outline filled and merely
        // stroked are different pictures.
        first.fill != second.fill ||
        // Likewise for a bezier path's closure: an open curve and the closed
        // one that fills it are different marks, and leaving this out would let
        // an undo step that only closes a path look like no change at all.
        first.closed != second.closed ||
        // A translation's placed lines are its whole content: two of them that
        // differ in a background, a font or a box are different pictures.
        first.translation != second.translation) {
        return false;
    }
    if (first.rect.x != second.rect.x || first.rect.y != second.rect.y ||
        first.rect.width != second.rect.width || first.rect.height != second.rect.height) {
        return false;
    }
    if (first.points != second.points || first.origin.x != second.origin.x ||
        first.origin.y != second.origin.y || first.text != second.text) {
        return false;
    }
    // Comparing pixel buffers would copy megabytes per undo snapshot; the cache
    // key identifies the same image without touching it.
    if (first.pixels.cacheKey() != second.pixels.cacheKey()) {
        return false;
    }
    return true;
}

/// The style one tool draws with.
///
/// The editor used to keep a single colour and width for every tool, so moving
/// the rectangle's width slider also moved the pen's.  Each tool owns its
/// values now: the style row shows and edits the values of the tool it is
/// pointed at -- the selected annotation's tool while one is selected, and the
/// armed tool otherwise, which is what `styleTargetTool` answers.
///
/// `numberSize`, `amplitude` and `wavelength` mean something to one tool each
/// and are kept here anyway, so that "the style of a tool" stays one object
/// rather than a mix of shared and per-tool state.
struct ToolStyle {
    QColor color{255, 64, 64, 255};
    /// Stroke width in logical pixels: a shape's outline, a segment's
    /// thickness, the mosaic brush's radius base.
    std::uint32_t width = 2;
    /// Diameter of a numbered badge, in logical pixels.
    std::uint32_t numberSize = 18;
    /// A wave's crest offset and period, in logical pixels.
    std::uint32_t amplitude = 4;
    std::uint32_t wavelength = 18;
};

enum class Tool {
    // There is no select tool.  Choosing a mark and adjusting the selection are
    // not modes the user picks: a mark is picked by clicking it with any tool
    // armed, and the selection is adjusted through its own border and handles,
    // which are live whatever tool is up.  What used to be the Select tool is
    // now the state a session is in when no drawing tool has been chosen.
    Rectangle,
    Ellipse,
    Arrow,
    // The two straight, two-point tools: a plain segment and the same segment
    // turned into a sine wave.  Both are drawn from exactly the two points the
    // drag made, like the arrow, so neither accumulates points as the pointer
    // wanders.
    Line,
    Wave,
    // The pen: clicks lay down anchors and the drag after each one pulls its
    // outgoing handle out, so every segment is a cubic and the handle is
    // symmetric by construction.  It is the one tool here whose gesture is not
    // a single drag -- a path spans as many presses as it has anchors, and ends
    // either closed back onto its first anchor or double-clicked open.
    Bezier,
    Pen,
    Text,
    // A numbered badge: one click places one badge and the count advances.  It
    // travels as a text annotation with a bitmap, so the renderer needs no
    // knowledge of it at all.
    Number,
    Mosaic,
    // The eyedropper: a click reads the pixel under it and hands the colour to
    // the tool the picker was armed from, which is also the tool the pick hands
    // the session back to.  It paints nothing of its own, so it is the one tool
    // with no style row.
    Picker,
};

class CaptureOverlay;

/// What the toolbar's text button has to say: the recognition is running, the
/// text mode is up and waiting for a gesture, the copy landed, or the last
/// attempt failed.  One callback carries all of them so the button never has to
/// guess which of its own clicks it is answering.
enum class TextOutcome {
    /// The recognition run is in flight; the button stays on this label until
    /// the outcome that follows replaces it.
    Busy,
    /// Nothing to report: the mode is up and the button goes back to its own
    /// label, waiting for the copy the user is about to ask for.
    Idle,
    Copied,
    Failed,
};

class OverlayController final : public QObject {
public:
    explicit OverlayController(Session session, QObject *parent = nullptr);
    ~OverlayController();

    OverlayController(const OverlayController &) = delete;
    OverlayController &operator=(const OverlayController &) = delete;

    int outputCount() const;
    const Session &session() const;
    CaptureOverlay *addOverlay(int outputIndex, QScreen *screen, QString *error);
    // The overlay-local rect the last interactive step asked to be repainted,
    // or a null rect when that step asked for the whole surface, which happens
    // on the first step of a gesture.  Read by the offline check, which compares
    // two full renders around a step and proves the pixels that changed all fall
    // inside it; it runs a single output, so the last overlay the step reached
    // is the one that matters.
    QRect lastInteractiveUpdate() const;

    /// The spelling of one action's binding, for a tooltip: "Ctrl+S", "Tab",
    /// and so on.  A button that does something the keyboard also does has to
    /// say which key that is, and the key is the user's to change.
    QString shortcutText(ShortcutAction action) const;
    /// `label` with the binding of `action` in parentheses after it, which is
    /// what a toolbar button's tooltip reads.  `label` is already translated,
    /// so the key -- which is not a word in any language -- is appended in the
    /// form every other program spells it in.
    QString shortcutHint(ShortcutAction action, const QString &label) const;

    void paint(CaptureOverlay *overlay, QPainter *painter);
    void press(CaptureOverlay *overlay, const QPointF &local, Qt::MouseButton button,
               Qt::KeyboardModifiers modifiers);
    void move(CaptureOverlay *overlay, const QPointF &local, Qt::MouseButtons buttons,
              Qt::KeyboardModifiers modifiers);
    void release(CaptureOverlay *overlay, const QPointF &local, Qt::MouseButton button,
                 Qt::KeyboardModifiers modifiers);
    void doubleClick(CaptureOverlay *overlay, const QPointF &local, Qt::MouseButton button);
    void key(CaptureOverlay *overlay, int key, Qt::KeyboardModifiers modifiers);

    // Arms `tool` for the next press, or disarms everything when it is empty.
    void chooseTool(std::optional<Tool> tool);
    // What a tool button does: arms the tool, or disarms it when that is the
    // one already armed.
    void toggleTool(Tool tool);
    void setCurrentColor(const QColor &color);
    void setCurrentFont(const QString &family);
    void setWidth(std::uint32_t width);
    void setDash(const QString &dash);
    void setArrowSize(std::uint32_t size);
    void setArrowStyle(const QString &style);
    void setTextSize(std::uint32_t size);
    void setMosaicShape(const QString &shape);
    void setMosaicStrength(std::uint32_t strength);
    /// Diameter of the next numbered badge, in logical pixels.  A badge's size
    /// is a value of its own now; the width control no longer reaches it.
    void setNumberSize(std::uint32_t size);
    /// The wave's crest offset and period, in logical pixels.
    void setWaveAmplitude(std::uint32_t amplitude);
    void setWaveWavelength(std::uint32_t wavelength);
    /// How the next pen path is painted: "stroke" | "fill" | "both".
    void setFill(const QString &fill);
    /// The style of one tool, by its wire name (see `toolName`).  Every tool has
    /// one from construction, so a read never has to invent a value.
    ToolStyle &toolStyle(const QString &tool);
    const ToolStyle &toolStyle(const QString &tool) const;
    /// The tool the eyedropper hands its colour to: the one that was armed when
    /// the picker was chosen, which is also the tool a pick leaves the session
    /// on.  While the picker is up the style row shows this tool's values, so
    /// what a pick would change is on screen before the click.
    Tool pickerTarget() const { return pickerReturnTool_; }
    /// The colour the last pick took, invalid before the first one.  The pick
    /// writes the target tool's `ToolStyle`; this is the same value kept where
    /// a caller can read it without knowing which tool that was.
    QColor pickedColor() const { return pickedColor_; }
    /// The tool that is armed, or nothing while none is.  The eyedropper is
    /// what makes this worth reading: it is the one tool whose press changes
    /// the armed tool by itself, handing the session back to the tool the pick
    /// was for.  An unarmed session is not a failure -- it is the state a
    /// capture opens in, and the one a mark is picked up in -- so the empty
    /// answer is a real one and is returned as such.
    std::optional<Tool> currentTool() const { return tool_; }
    // The badge style the next numbered mark is placed with, and any number
    // already selected.
    void setNumberStyle(NumberStyle style);
    NumberStyle numberStyle() const { return numberStyle_; }
    // Pastes an image into the selection: it lands centred at its natural size,
    // shrunk to fit if it is larger than the canvas, and is left selected so
    // the handles can resize it. `source` names the file it came from, empty
    // for clipboard pixels, and is only used to report where it came from.
    // Returns false when there is nothing to paste onto or the image is empty.
    bool pasteImage(const QImage &image, const QString &source = QString());
    // The same, reading whatever the clipboard holds: image data, or a local
    // file path or URI list that points at an image. `error` is filled with
    // why nothing was pasted, for the caller to report.
    bool pasteFromClipboard(QString *error);
    // Pastes an image chosen from disk. The dialog runs in a process of its
    // own -- this one draws a layer surface, which cannot parent a popup -- so
    // the file arrives back here asynchronously and the paste happens then.
    // `error` is filled when the dialog cannot even be started.
    bool pasteFromFile(QString *error);
    // Whether a paste would have anything to work with, so the toolbar can
    // disable its button rather than offering a no-op.
    bool canPaste() const;
    /// Runs recognition over the selection and enters the text mode.  Returns
    /// false and fills `error` when there is nothing to select.
    bool beginTextSelection(QString *error);
    /// The recognition came back: parse it and either enter the text mode or,
    /// when the engine reported no positions, hand the whole text over the way
    /// this used to.  Separate from `beginTextSelection` so a check can drive
    /// the mode without a recognition run.
    bool enterTextSelection(const QByteArray &document, QString *error);
    void leaveTextMode();
    bool textMode() const { return textMode_; }
    /// Runs OCR then translation over the selection and adds the result as one
    /// annotation, in place.  Returns false and fills `error` when there is
    /// nothing to read; reports its progress through
    /// `setTranslateResultCallback`, the way the text button does.
    bool translateSelection(QString *error);
    /// Whether the session is the standalone `translate` overlay: a region-only
    /// frame, the translation drawn over the frozen scene as soon as that frame
    /// is finished, and an Enter that accepts, writing the composited PNG.
    bool translateMode() const { return translateMode_; }
    /// The text the last translation produced, empty before one has run.
    QString translatedText() const { return translatedText_; }
    /// The text the current range would copy, empty when nothing is selected.
    QString selectedText() const;
    /// Told what the text button should show.  Called with `Busy` before the
    /// recognition run starts -- the wait for the engine is long enough that
    /// the button has to say so -- and once more with the outcome.
    void setTextResultCallback(std::function<void(TextOutcome, const QString &)> callback);
    /// The same for the translate button: `Busy` while the two subprocesses
    /// run, then the outcome.
    void setTranslateResultCallback(std::function<void(TextOutcome, const QString &)> callback);
    /// Replaces the clipboard write the text paths use.  It exists so a check
    /// can verify what would be copied without a clipboard; the default writes
    /// through `wl-copy`.
    void setClipboardWriter(std::function<bool(const QString &)> writer);
    void notifyPanelDragged();
    void undo();
    void redo();
    void confirm();
    void cancel();
    // Finishes the session asking for the image to be pinned on the screen
    // instead of saved.  The image is composed on the CLI side, so the request
    // travels back with the result; see `resultDocument`.
    void pin();

    bool isFinished() const;
    bool isCancelled() const;
    // Whether the user finished with the Pin button rather than OK.
    bool isPinResult() const { return pinResult_; }
    bool isPinEdit() const { return pinEdit_; }
    // Pin-edit mode: the whole session bounds is the editable canvas; the
    // selection is fixed and the toolbar shows immediately. Call before the
    // overlay is shown.
    void setPinEditMode(bool enabled) { pinEdit_ = enabled; }
    // Pin-edit mode: the editor drives the real pin window over the daemon
    // socket rather than drawing a second copy of the image. Call before the
    // overlay is shown.
    void setPinTarget(std::uint64_t pinId, const QString &socketPath);
    // Pin-edit mode: where the editor may write a mark's pixels -- a pasted
    // image is its pixels and travels as a path to a file rather than inline.
    // Empty leaves it nowhere to put them, and the marks that need one are left
    // out of the document rather than named by paths that will not resolve.
    // Call before the overlay is shown.
    void setMarkAssetDirectory(const QString &directory) { markAssetDirectory_ = directory; }
    // Enters editing state over the fixed canvas (shows the toolbar).
    void beginPinEdit();
    // The same editor, opened on the text rather than on the marks: it does
    // what `beginPinEdit` does and then runs recognition over the whole pin,
    // entering the text-selection mode where the recognition succeeds. A
    // failure leaves the editor in the ordinary pin-editing state, reporting
    // through the same callback the toolbar's `Text+` button uses.
    void beginPinEditText();
    // Region sessions that arrive with a selection (window picking resolved
    // one) start in editing state with the toolbar up.  Call after the overlay
    // is shown; sessions without a selection are left alone.
    void beginPresetEdit();
    // Window-pick only: let the picker ask the CLI for a fresh candidate list
    // over the session pipes.  Picking runs on a live desktop, so the list it
    // started with goes stale as soon as the user switches workspace or a
    // window moves; the pointer asks again as it travels.  Call after the
    // overlay is shown, before the event loop runs.
    void enableCandidateRefresh();
    // Asks for that fresh list, at most every `kCandidateRefreshIntervalMs` and
    // never with a request already in flight.  A no-op unless the refresh was
    // enabled; the CLI may answer with nothing, which keeps the current list.
    void requestCandidateRefresh();
    // Let the keyboard's cursor walk move the real pointer through the CLI.
    // The walk itself is the editor's; the pointer the compositor draws is the
    // CLI's to move, because only it holds an injection backend.  Call once,
    // from the helper's startup, for a session that has a CLI behind it.
    void enablePointerWarp();
    // The editor has drawn everything it is going to draw and asks to be let
    // go.  It writes the session's result and rendered pixels, asks the CLI to
    // put them where they belong, and keeps its surface up -- showing the same
    // picture -- until the CLI answers, so the caller's own copy of it is on
    // the screen before this one goes and the user never sees the two blink
    // past each other.
    //
    // Always ends the session -- on the CLI's answer, on a lost connection, or
    // on a backstop -- so the process cannot outlive the request.  Called from
    // `terminal` for a session that rendered a capture; a helper run by hand
    // has no CLI to ask and quits outright.
    void beginHandoff();
    // Whether this session rendered a capture of its own that the caller has
    // not been told about yet; see `beginHandoff`.
    bool rendersCapture() const;
    // Whether `beginHandoff` has already written the session's result and
    // rendered pixels.  The caller then has nothing left to write: the pixels
    // travel once, and a second `resultDocument` would send them again.
    bool resultSent() const { return resultSent_; }
    bool hasValidSelection() const;
    // Whether the scrolling-capture action would do anything: the session
    // offers it, the selection is big enough, and it sits inside a single
    // output -- a scroll container never spans two monitors. The toolbar asks
    // this to decide whether to offer the button, and the action asks again
    // before it commits.
    bool canRequestLongCapture() const;
    // Takes the scrolling-capture action: the session ends the way a
    // confirmation does, but the CLI reads the answer as "scroll this region
    // and stitch it" rather than "keep this frame". A no-op when
    // `canRequestLongCapture` is false.
    void requestLongCapture();
    bool longRequested() const { return longRequested_; }
    const std::optional<LogicalRect> &selection() const;
    const QVector<Annotation> &annotations() const;
    // How many freehand segments the live preview has baked since the stroke
    // started.  Lets the offline check prove each segment is drawn once rather
    // than recomputed on every paint.
    int liveStrokeBakes() const;
    QJsonDocument resultDocument(QString *error = nullptr) const;
    // The marks painted onto the session's own pixels, at the pixel size the
    // result should be.  Qt is the only renderer: the CLI takes this image
    // instead of rasterizing the marks a second time, which is what used to
    // leave a committed mark half a pixel away from the preview.
    //
    // `pixels` is the capture the marks were placed on, already cropped to the
    // selection, in the output's device pixels; `density` is its device pixels
    // per logical pixel, which is what the marks were measured against.
    // Draws the committed marks.  `canvas` is what they are painted onto and
    // `source` is what a sampling mark reads: the two are the same image for
    // the flattened result, and differ for the marks alone, which is painted
    // onto transparency but must still pixelate the capture underneath it.
    //
    // `origin` is the global logical rect the crop was taken from and `density`
    // the capture's device pixels per logical pixel, which is what the marks
    // were measured against; together they are what maps a mark's own
    // coordinates onto the crop.
    QImage compositeAnnotations(const QImage &canvas, const QImage &source,
                                const LogicalRect &origin, double density,
                                QString *error = nullptr) const;
    // The same thing for the session's own selection, as the two images the CLI
    // needs.  `composite` is the capture with the marks drawn on it, which is
    // the SDR result verbatim; `marks` is the marks alone on transparency, which
    // is what composites onto the HDR half -- the flattened image would replace
    // it rather than mark it, since it is opaque everywhere.
    //
    // False when there is nothing to render, which is not an error -- a
    // cancelled session, or a route that never had a selection.
    bool produceComposite(QImage *composite, QImage *marks, QString *error = nullptr) const;
    // The marks as data, in the shape a session can hand straight back, so a
    // daemon that keeps them can reopen this image for editing rather than only
    // showing the flattened result.  The coordinates are relative to the
    // selection, which is the canvas a re-edit hands back; an empty array when
    // nothing is framed.  The pixels a label was rasterized into are left out:
    // they are derived from the mark, not part of it, and a daemon holding only
    // the marks can rebuild them.
    //
    // A pasted image and a placed translation are carried by their pixels and
    // have no other form, so those are written out beside the session and named
    // by path -- see `writeMarkAssets`.  A path rather than inline data because
    // a pasted screenshot is megabytes, and this document travels in a
    // newline-delimited JSON message to the daemon.
    QJsonArray marksDocument() const;
    // Writes the pixels of every mark that has any into `directory`, and returns
    // the document naming them.  Separate from `marksDocument` because the
    // document is also wanted where there is nowhere to write -- a check, a
    // caller that only wants the geometry -- and there the marks without a
    // carrier are simply left out, which is what this did for all of them
    // before.  The bytes written are the mark's *original* pixels, not the copy
    // scaled to fit the canvas: the scale is a placement, and a mark resized
    // later has to go back to the source rather than to a copy of a copy.
    QJsonArray writeMarkAssets(const QString &directory) const;

    // The inverse: the marks a session carried, placed on `canvas` so they can
    // be edited again.  A mark that cannot be rebuilt -- an unknown tool, a
    // missing field, a rectangle off the canvas -- fails the whole session
    // rather than being skipped, because an editor that opened with some of the
    // user's marks silently missing is worse than one that says so.
    bool parseMarks(const QJsonArray &marks, const LogicalRect &canvas,
                    std::uint32_t deviceRatio, QString *error);

    void setTerminalCallback(std::function<void()> callback);

    // The one pass behind `marksDocument` and `writeMarkAssets`: an asset is
    // written for each mark that has pixels, but only when there is a directory
    // to write into.  An empty `directory` is "document only", and a mark with no
    // other form is then left out of it.
    QJsonArray marksDocumentInto(const QString &directory) const;
    // One mark's pixels into `directory`, named from `*counter`, or an empty
    // string when there is nowhere to put them or nothing to write.
    QString writeMarkAsset(const QString &directory, const Annotation &annotation,
                           int *counter) const;

    // Whether the magnifier in front of the user is the colour picker's, and
    // whether any magnifier is up at all.  The two are a distinction the offline
    // checks have to make and cannot read off the pixels: the picker's loupe and
    // the drag loupe are the same widget drawn the same way, and the difference
    // is only whether the pill under it says a colour or a coordinate.
    bool colorPickerVisible() const;
    bool magnifierVisible() const;
    // The name of the tool a press would draw with, or an empty string when
    // nothing is armed.  The same answer the style row reads, exposed because
    // whether a session opens armed is a decision the offline checks have to
    // make and cannot see in the pixels: an unarmed press re-frames and an
    // armed one inks, and which happened is only visible in the marks.
    QString armedToolName() const { return styleTargetTool(); }

private:
    class FloatingToolbar;
    class InlineTextEdit;
    struct Gesture;

    Session session_;
    QVector<CaptureOverlay *> overlays_;
    FloatingToolbar *toolbar_ = nullptr;
    InlineTextEdit *textEdit_ = nullptr;
    std::optional<LogicalRect> selection_;
    QVector<Annotation> annotations_;
    // Whether the session offered the scrolling-capture action, and whether
    // the user took it.  The action ends the session like a confirmation, so
    // the answer travels back beside the selection.
    bool longAllowed_ = false;
    bool longRequested_ = false;
    // Window picking: the session's candidate windows are what the pointer may
    // snap to, so the first click replaces the free-hand drag that region
    // capture starts with.
    bool pickMode_ = false;
    /// The text-selection mode: the recognized characters of the selection are
    /// drawn where they were and the pointer selects a range of them.  It is a
    /// mode rather than a tool because the selection it works on is the one the
    /// capture already has, and because Escape has to leave it before it means
    /// "cancel the capture" -- the same shape `pickMode_` has.
    bool textMode_ = false;
    std::optional<TextLayer> textLayer_;
    int textAnchor_ = -1;
    int textFocus_ = -1;
    bool textDragging_ = false;
    /// When the last text-mode double click landed, so a second one in quick
    /// succession -- Qt's third press, reported as another double click -- can
    /// widen the word it took to the whole line.
    QElapsedTimer textClickClock_;
    /// Told when the recognition starts, when the text mode starts, and when a
    /// copy finishes, so the toolbar can say so on the button the user pressed.
    /// The copy can be triggered by a key, which the controller sees and the
    /// toolbar does not.
    std::function<void(TextOutcome outcome, const QString &error)> textResultCallback_;
    /// Writes the text the mode copies.  The default is `wl-copy`; a check
    /// replaces it so the copy can be verified without a clipboard.
    std::function<bool(const QString &)> clipboardWriter_;
    /// The standalone `translate` overlay: a region-only frame, a translation
    /// drawn over the frozen scene, then an accept that writes the PNG.  It is
    /// a mode of its own rather than the editor because it never shows a
    /// toolbar and its Enter key means two different things in turn.
    bool translateMode_ = false;
    /// Whether a translation is up over the framing, and what it holds.
    bool translated_ = false;
    QVector<TranslatedLine> translatedLines_;
    QString translatedText_;
    /// Where the accepted translation was written, for the result document.
    QString resultImagePath_;
    /// The absolute path the session named for it, from `result_path`.
    QString resultPath_;
    /// Told when a translation starts and how it ended, so the button the user
    /// pressed can say so.
    std::function<void(TextOutcome outcome, const QString &error)> translateResultCallback_;
    QVector<WindowCandidate> candidates_;
    int hoveredCandidate_ = -1;
    // Live candidate refresh: the picker's stdin carries fresh lists from the
    // CLI, and one request may be in flight at a time.
    QSocketNotifier *candidateReader_ = nullptr;
    QTimer *candidateTimer_ = nullptr;
    QByteArray candidateReplies_;
    QElapsedTimer candidateClock_;
    bool candidateRefreshEnabled_ = false;
    bool candidateRefreshPending_ = false;
    /// Whether the CLI on the other end of this pipe moves the real pointer.
    /// The keyboard walks a cursor of its own, and the pointer the compositor
    /// draws is not it -- so the walk asks for a warp, and this says whether
    /// there is anyone to ask.  Read from the session, which knows the output
    /// geometry the request has to be expressed in; false for a helper driven
    /// by hand or by a check, which is what keeps those from writing into a
    /// pipe nobody is reading.
    bool pointerWarpEnabled_ = false;
    QVector<QVector<Annotation>> undoStack_;
    QVector<QVector<Annotation>> redoStack_;
    std::optional<Annotation> cancelledText_;
    int editingTextIndex_ = -1;
    bool textEditSnapshot_ = false;
    int toolbarOutput_ = -1;
    int textOutput_ = -1;
    std::uint32_t textDeviceRatio_ = 1;
    Point textOrigin_;
    QString textEditFont_;
    // Font height the open inline editor is drawing at, kept in step with the
    // size box so changing the size while a label is being typed resizes it
    // live instead of leaving the editor at the old height.
    std::uint32_t textEditPixels_ = 0;
    Point pointer_;
    int pointerOutput_ = -1;
    // The tool the next press will draw with, or nothing while none is armed.
    // An unarmed session is the state the old Select tool used to be: a press
    // adjusts the selection and the marks rather than drawing a new one, and
    // that is where a region capture starts -- its first step is framing, not
    // inking.
    std::optional<Tool> tool_;
    // The eyedropper's own state: the tool a pick will hand its colour to --
    // the one that was armed when the picker was chosen -- and the colour the
    // last pick took.  The colour itself is written into that tool's
    // `ToolStyle`, which is what a painter reads; this copy is for readouts.
    //
    // The target is the tool that was armed, and with no tool armed there is
    // none to hand a colour to: a pick from that state goes to the pen, whose
    // colour is the session's own ink -- the same fallback the old Select tool
    // had, which is what an unarmed session is now.
    Tool pickerReturnTool_ = Tool::Pen;
    QColor pickedColor_;

    // The right button held: the magnifier is up for as long as it is, and the
    // pixel it shows can be copied (C) or taken as the current colour (A).
    //
    // Only the right button puts the colour picker in the magnifier: it is the
    // button the user presses *because* they are aiming at a pixel, while the
    // loupe a drag brings up is a coordinate readout for placing a mark, and a
    // colour pill under it would be answering a question nobody asked.
    bool magnifierHeld_ = false;
    // Where the pointer was on the last motion, for the magnifier the right
    // button holds: its own rect has to be invalidated on the way out as well
    // as the one it is moving to, or a picker dragged across the screen leaves
    // a trail of loupes behind it.
    Point lastMagnifierPointer_{};
    // The magnifier shown for a moment after the cursor was moved with the
    // keyboard, with the timer that ends it.  A keypress is a deliberate act,
    // and the pointer has not moved for it, so the user has nothing to aim
    // with unless the magnifier says where the cursor went.
    bool magnifierTyped_ = false;
    QTimer *magnifierTimer_ = nullptr;
    // Where the keyboard last put the cursor, in global logical pixels, or
    // nothing while the pointer is the one in charge.  The arrow keys and WASD
    // move it; any real pointer motion takes it over again.
    std::optional<Point> keyboardCursor_;
    /// When the last pointer-warp request went out, for the throttle in
    /// `requestPointerWarp`.  Invalid until the first one.
    QElapsedTimer pointerWarpClock_;
    /// The newest position the throttle held back, sent when its timer fires.
    /// Only the newest: the request carries an absolute position, so an older
    /// one has nowhere to land that the newer one does not overwrite.
    std::optional<Point> pointerWarpPending_;
    /// The position the last warp asked for, and the clock that says how long
    /// ago: together they identify the motion the compositor reports back, so a
    /// step of a keyboard walk is not read as the user moving the mouse.  See
    /// `isPointerWarpEcho`.
    std::optional<Point> pointerWarpTarget_;
    /// Parentless and deleted with the controller's other timers, like the
    /// magnifier flash's.
    QTimer *pointerWarpTimer_ = nullptr;
    // The mark the pick-up modifier is holding the pointer over, or -1.  See
    // `markUnderPointer`.
    int markHovered_ = -1;
    // The modifiers the last pointer event carried.  A move handler has them in
    // hand; the painter, which runs from `paintEvent` with nothing but the
    // widget, does not, and the hover frame it draws depends on whether the
    // pick-up modifier is held.  There is no query for the live state that does
    // not go through the window system, and the overlay is a layer surface with
    // no focus of its own.
    int lastModifiers_ = 0;
    QString currentFont_;
    // Per-tool colour and numeric parameters, keyed by `toolName`.  A painter
    // path reads the tool it is drawing with; the style row reads and writes
    // the selected annotation's tool, or the armed one.
    QHash<QString, ToolStyle> toolStyles_;
    // Font height for the next label, in logical pixels -- the same number the
    // size box shows.
    std::uint32_t textSize_ = 14;
    QString currentDash_ = QStringLiteral("solid");
    // How the next pen path is painted: "stroke" | "fill" | "both".  Like the
    // dash and the arrow head, this is a choice rather than a number, so it
    // stays shared instead of travelling per tool.
    QString currentFill_ = QStringLiteral("both");
    std::uint32_t arrowSize_ = 1;
    QString currentArrowStyle_ = QStringLiteral("open");
    QString mosaicShape_ = QStringLiteral("rect");
    std::uint32_t mosaicStrength_ = 2;
    NumberStyle numberStyle_ = NumberStyle::FilledCircle;
    bool panelPinned_ = false;
    // Automatic toolbar placement anchor: while the selection stays put, the
    // side of the selection the command bar was placed on stays fixed, so a
    // style-row toggle only grows the panel the other way.
    QRect toolbarAnchorSelection_;
    QPoint toolbarAnchor_;
    bool toolbarAnchorBelow_ = false;
    bool toolbarAnchorValid_ = false;
    // Selected annotation adjustment (move/resize under the Select tool).
    int selectedAnnotation_ = -1;
    /// The mark list as it was before the run of keyboard nudges in progress,
    /// so the whole run is one undo step.  Cleared by anything else that edits
    /// or reselects.
    std::optional<QVector<Annotation>> nudgeBase_;
    /// Ctrl+A: every mark is picked up at once.  The style row and the delete
    /// key then reach all of them, and any other selection clears it.
    bool allSelected_ = false;
    /// `editor.selectMode == "loose"`: a press that is not on a handle or
    /// another mark is held back until it moves, and then moves the selected
    /// mark from wherever it started.  The point is the one the button went
    /// down at, which is the anchor the move is measured from.
    bool looseSelect_ = false;
    std::optional<Point> looseDrag_;
    Annotation dragAnnotation_;
    QVector<Annotation> dragSnapshot_;
    bool dragMoved_ = false;
    bool styleAdjustmentActive_ = false;
    bool styleAdjustmentChanged_ = false;
    QVector<Annotation> styleAdjustmentSnapshot_;
    Gesture *gesture_ = nullptr;
    // Freehand segments the live preview has baked for the current stroke.
    int liveStrokeBakes_ = 0;
    // The session-space rect the last interactive step invalidated, and whether
    // there is one.  A step has to erase what the step before painted, and the
    // only record of that is this rect; `updateAll` clears it, a full repaint
    // being its own eraser.  See `updateTouch`.
    LogicalRect lastTouch_{};
    bool hasLastTouch_ = false;
    // The overlay-local rect that step handed to the widget, for the offline
    // checks; see `lastInteractiveUpdate`.
    QRect lastTouchLocal_;
    bool editing_ = false;
    bool pinEdit_ = false;
    // Where a mark's pixels are written when the session gives somewhere to put
    // them.  Empty means the document carries no mark that has no other form.
    QString markAssetDirectory_;
    // Set by the Pin button and reported in the result document.
    bool pinResult_ = false;
    /// `region-only`: a finished drag ends the session with the rectangle
    /// instead of opening the editor.  Scrolling capture asks for this, since
    /// the pixels it will annotate do not exist until the stitch is done.
    bool selectOnly_ = false;
    /// The keyboard bindings, read out of the config file once at start-up.
    /// Every key the editor acts on is looked up here rather than compared
    /// against a hard-coded `Qt::Key_`, so a binding the user has changed is
    /// the one the editor honours.
    ShortcutPreferences shortcuts_;
    bool finished_ = false;
    bool cancelled_ = false;
    std::function<void()> terminalCallback_;
    mutable int textBitmapIndex_ = 0;
    // The same counter for pasted images' pixel files, in the same directory.
    mutable int imageBitmapIndex_ = 0;
    // Region editor base layer: the session image with the dim veil already
    // composited, at device resolution.  It only depends on the output, its
    // pixel buffer, the surface and the display ratio, so a repaint (a pointer
    // move, a selection drag) blits it rather than drawing the frame and the
    // veil again.  `baseCompositeKey_` says when it has to be rebuilt.
    QImage baseComposite_;
    QByteArray baseCompositeKey_;
    // The magnifier's own view of the picture: the frozen frame with a band
    // round it and the committed marks painted in, so the loupe reads what the
    // user has actually made rather than the bare capture.  Kept apart from
    // `baseComposite_`, which is the frame plus the veil and deliberately has
    // no marks on it.  Rebuilt when the marks change, when the frame changes
    // and when the pin's image is moved under them -- `loupeCompositeKey_`
    // says which.
    QImage loupeComposite_;
    QByteArray loupeCompositeKey_;
    int loupeCompositeOutput_ = -1;
    // Live pin window the editor drives in pin-edit mode.  One connection
    // serves the whole drag: opening a socket costs a connect, a server accept
    // and a fresh object on both sides, and paying that per motion event was
    // most of the drag's latency.  The protocol is newline-delimited, so a move
    // is just a line on the connection the first one opened.
    std::uint64_t pinId_ = 0;
    QString pinSocketPath_;
    // Whether the pin being edited is still the daemon's live one.  True at the
    // open, where the editor has just brought it to the front, and set from
    // `notePinActive` afterwards.  It gates the image's frame and nothing else:
    // the marks, the input region and the drag all carry on either way, because
    // the edit itself has not ended.
    bool pinActive_ = true;
    QLocalSocket *pinSocket_ = nullptr;
    QByteArray pinReplyBuffer_;
    // Positions waiting for a free slot and the number already written but not
    // yet answered.  A few may be in flight at once -- the position is absolute
    // and the newest wins, so a small queue only keeps the daemon busy instead
    // of letting it idle between replies -- but bounded, so a stalled daemon
    // cannot grow it without end.
    std::optional<Point> pendingPinOrigin_;
    int pinMovesInFlight_ = 0;
    // A raise asked for while the socket was down, sent as soon as it is up.
    // The editor connects before the first drag now, so this is only ever the
    // race between a press and the connect, but a dropped connection puts it
    // back in play.
    bool pinRaisePending_ = false;
    // A pin-edit session's handoff: the wait between asking the CLI to be let
    // go and being told the pin's own frame is on the screen.  The editor has
    // stopped being useful but keeps drawing until then, so the pin's frame and
    // the editor's copy of it overlap and the marks do not blink out between
    // the two.  `handoffReader_` watches the CLI's answer on stdin;
    // `handoffTimer_` is the backstop for a CLI that never answers at all,
    // which is a hang rather than a gap.
    QSocketNotifier *handoffReader_ = nullptr;
    QTimer *handoffTimer_ = nullptr;
    QElapsedTimer handoffClock_;
    bool resultSent_ = false;
    // Set from VSHOT_PIN_DEBUG: traces the drag's round trip to stderr.
    bool pinDebug_ = false;
    QElapsedTimer pinMoveClock_;
    // Pin-edit: the coalescing timer behind `applyPinEditInputMask`.  The mask
    // is a round trip to the compositor, and a drag would otherwise issue one
    // per motion event.
    QTimer *inputMaskTimer_ = nullptr;
    // The position the marks are anchored to in pin-edit mode: the last
    // confirmed reply from the daemon, not the optimistic cursor position.  The
    // FP16 helper surface shows the image at this same position, so clipping
    // marks to it keeps them in sync with the image rather than ahead of it.
    std::optional<LogicalRect> marksOrigin_;
    // Pin-edit mode: whether a point is on the pinned image itself, or on the
    // band its own border occupies just outside it. The border is drawn by the
    // daemon, centred on the image's edge, so half of it stands outside the
    // image; both bands move the pin, but only the image takes ink. `inside`
    // reports the image and `border` the band around it, and both are false
    // everywhere else on the canvas.
    bool insidePinImage(Point point) const;
    bool onPinBorder(Point point) const;
    // The band the pin's border occupies: the image grown by half the border's
    // width. Zero-sized when the session named no border.
    LogicalRect pinBorderBand() const;
    // Pin-edit only: cuts the surface's input region down to the chrome the
    // editor actually owns -- the pinned image, its border, the toolbar and
    // anything open over them.  The surface covers the whole output so the
    // toolbar has somewhere to sit, and without this every click on the rest of
    // the screen would land on the editor instead of on the desktop behind it.
    // A no-op outside pin-edit mode, where the surface is the capture itself
    // and owning the whole output is the point.
    void applyPinEditInputMask();
    void scheduleInputMask();

    Point globalPoint(CaptureOverlay *overlay, const QPointF &local) const;
    Point unclampedGlobalPoint(CaptureOverlay *overlay, const QPointF &local) const;
    // Index of the output whose geometry holds the middle of `rect`, for
    // placing the toolbar next to a selection nobody dragged.
    int outputContaining(const LogicalRect &rect) const;
    const LogicalRect &annotationLimits() const;
    LogicalRect selectionLimits() const;
    void translateAnnotations(std::int32_t dx, std::int32_t dy);
    void applySelectionMove(LogicalRect origin, Point anchor, Point current);
    void requestPinMove(Point globalTopLeft);
    // Asks the daemon to put the pin being edited back on top of the stack.  A
    // click that would normally raise a pin cannot reach its surface while an
    // edit is open -- the editor's layer surface covers the output -- so a press
    // on the image asks on the user's behalf.
    void requestPinRaise();
    void flushPinMove();
    void openPinSocket();
    void dropPinSocket();
    void readPinReplies();
    void applyPinReply(QByteArray line);
    void applyPinRect(const LogicalRect &rect);
    // The daemon's answer to "which pin is the live one", which decides whether
    // the frame is drawn around the image being annotated.  The frame is Qt
    // chrome and every other pin is painted by a Wayland surface one layer
    // below, and the compositor orders a layer's surfaces by map time with no
    // restack -- so a frame left up for a pin the user has moved on from would
    // be drawn on top of every other pin on the screen.
    void notePinActive(std::uint64_t pinId);
    // Repaints exactly the overlays' part of `region` (global logical pixels),
    // without touching the "last touch" bookkeeping a gesture's steps share.
    void invalidateLogicalRegion(const LogicalRect &region);
    Point clampPoint(Point point) const;
    int candidateIndexAt(Point point) const;
    QString candidatePillText() const;
    QString selectionPillText() const;
    bool applyCandidateHover(Point point, CaptureOverlay *overlay);
    // Replaces the candidate list with a fresh one and points the hover at
    // whatever the (unmoved) pointer is over now.
    void applyCandidates(QVector<WindowCandidate> candidates);
    void readCandidateReplies();
    LogicalRect selectionBetween(Point first, Point second) const;
    LogicalRect moveSelection(LogicalRect origin, Point anchor, Point current) const;
    // Alt (`preserveAspect`) keeps the box's own width-to-height ratio while a
    // handle drags it: the corner the handle is not on stays put and the other
    // follows the pointer along the box's diagonal.
    LogicalRect resizeSelection(LogicalRect origin, int handle, Point current,
                                bool preserveAspect = false) const;
    int hitHandle(Point point) const;
    // What a press on the bare canvas does: resize the selection by its handle,
    // or start a new one.  The body of the selection is not a target -- dragging
    // it is `beginSelectionMove`, which only the middle button reaches.  Shared
    // with the loose drag's click path, which has to reach the same selection
    // logic once it has let go of the mark.
    void beginSelectionGesture(Point point, bool preserveAspect = false);
    // Middle-drag: the selection is dragged whole, from wherever the press
    // landed inside it.  The pin editor reaches it through the same call: a
    // pin's image is moved, not resized, so its whole area is this target.
    void beginSelectionMove(Point point);
    void startSelection(Point point);
    void updateSelection(Point point);
    void finishSelection(Point point);
    void beginDrawing(Point point);
    void updateDrawing(Point point);
    void finishDrawing(Point point);
    // The pen path: a press adds an anchor, the drag after it bends the segment
    // arriving at that anchor, and the path is only finished by closing it or
    // double-clicking.  It therefore outlives the release that ends a normal
    // drag, which is why it has its own three steps rather than reusing
    // `beginDrawing`/`finishDrawing`.
    void beginBezier(Point point);
    // Extends the path in progress: `dragging` pulls the last anchor's outgoing
    // handle to `point`, otherwise `point` is only where the rubber band reaches.
    void updateBezier(Point point, bool dragging);
    // Commits the path in progress, closed or open.
    void finishBezier(bool closed);
    void beginText(CaptureOverlay *overlay, Point point);
    // Places one numbered badge at `point` and advances the count.  The whole
    // tool is a press: there is no drag to preview.
    void placeNumber(Point point);
    // Reads the pixel under `point` and hands its colour to the tool the
    // picker was armed from, keeping that tool's opacity.  False when there is
    // no pixel to read -- the pointer is off the frozen image.  The whole tool
    // is a press, like the number badge.
    bool pickColorAt(CaptureOverlay *overlay, Point point);
    void startTextEditor(CaptureOverlay *overlay, int index, Point origin);
    void finishText(bool accept);
    int annotationHitAt(Point point) const;
    int annotationHandleAt(Point point) const;
    // The mark the pick-up modifier has put under the pointer, or -1.  This is
    // what the *hover* frame is drawn around: holding the modifier is the user
    // asking "what is under here?", and a mark that answers the press with a
    // drag has to say so before the press -- the pointer's shape alone is not
    // enough, because the shape says a drag is possible, not *which* mark.
    // Remembered rather than recomputed so the frame can be erased again: the
    // pointer's last position is not the same as the mark's rect.
    int markUnderPointer() const;
    // Repaints the hover frame where it has just moved, and nothing where it has
    // not: the mark the pointer is over changes on a motion, and the modifier
    // that decides whether there *is* one changes on a key.  Both call this.
    void refreshMarkHover();
    // One JSON request to the CLI, written to the pipe the session arrived on.
    // False when it could not be written whole.
    bool writeCliRequest(const QByteArray &request);
    // Asks the CLI to put the real pointer at `point`, in global logical
    // pixels.  See the definition for why this is not something the editor can
    // do itself.
    void requestPointerWarp(Point point);
    // Whether a motion event at `point` is the compositor reporting the warp
    // VShot asked for rather than the user moving the mouse.  Such a motion
    // must not take the keyboard cursor back or end the magnifier flash, or a
    // walk's own echo would blink the loupe on every step.
    bool isPointerWarpEcho(Point point) const;
    // Which way the selected mark's own border can be stretched at `point`, in
    // the same handle numbering as `annotationHandleAt` but with the whole edge
    // answering rather than only the eight handles.  0 where the pointer is not
    // on the border at all.  Used for the pointer's shape.
    int annotationBorderAt(Point point) const;
    // The same question about a named mark rather than the selected one, which
    // is what a *press* needs: it lands on a mark before that mark is selected,
    // so a rim has to be recognisable on a mark that has no handles on screen.
    // That is what makes a mark draggable by its border with a tool armed --
    // there are no handles to aim at until it is picked up, and the border is
    // the one part of it that cannot be read as "start a stroke here".
    int annotationBorderOf(int index, Point point) const;
    void beginAnnotationDrag(Point point, bool resize, bool preserveAspect = false);
    void updateAnnotationDrag(Point point);
    void finishAnnotationDrag(CaptureOverlay *overlay, Point point);
    Annotation translatedAnnotation(const Annotation &original, int dx, int dy) const;
    Annotation scaledAnnotation(const Annotation &original, const LogicalRect &bounds) const;
    void selectAnnotation(int index);
    void deleteSelectedAnnotation();
    void applyStyleToSelected(const std::function<void(Annotation &)> &mutate);
    void beginStyleAdjustment();
    void endStyleAdjustment();
    QString styleTargetTool() const;
    int sceneScale() const;
    void showToolbar();
    void hideToolbar();
    void updateToolbarGeometry();
    int outputIndexForSelection() const;
    void settlePanelAtGlobal(QPoint topLeft);
    void repaintEverything();
    void updateAll();
    // Invalidates only the part of the surface an interactive step changed.  A
    // repaint of a 4K overlay costs about 1.7 ms per full-frame blit and the
    // marks sit on top of two of them, so a step that touches a few hundred
    // pixels must not ask for the whole surface; `touched` is in session
    // coordinates and every overlay gets its own share of it.  The region the
    // step before invalidated is included as well -- that is what erases where
    // a dragged mark used to be -- and an empty rect falls back to `updateAll`.
    void updateTouch(const LogicalRect &touched);
    // The rects one interactive step can have changed, in session coordinates.
    LogicalRect selectionTouch() const;
    LogicalRect annotationTouch() const;
    LogicalRect drawingTouch(int pointsBefore) const;
    // The rects a step of the pen path can have changed, in session coordinates:
    // the path so far plus the rubber band from its last anchor to the pointer.
    LogicalRect bezierTouch() const;
    // The mark the pen path in progress would commit, as it stands.  The
    // preview and its bounds both read the shape from here.
    Annotation previewAnnotation() const;
    // The magnifier the editor draws around the pointer while a gesture drags
    // something, in session coordinates.
    LogicalRect pointerTouch() const;
    // The same, around a point that is not the current pointer: the magnifier
    // the right button holds follows the pointer, so a move has to name the
    // rect the loupe has just left as well as the one it is about to cover.
    LogicalRect pointerTouchAt(Point point) const;
    // True for the tools whose preview builds up through the incremental raster
    // rather than being redrawn whole from the anchor every step.
    bool drawsGrowingStroke() const;
    void terminal(bool cancelled);
    void removeTextEditor();
    // The text-selection mode's own steps: turning two indices into the range
    // the pointer described, widening one to a word or a line, copying what the
    // range holds, and the pointer shape the mode shows while it is idle.
    void copyTextSelection();
    void textSelectAll();
    void textSelectWord(int index);
    void textSelectLine(int index);
    void selectTextRange(int anchor, int focus);
    void updateTextModeCursor();
    // The one place the mode's text reaches the clipboard: the injected writer
    // when there is one, `wl-copy` otherwise.
    bool writeClipboard(const QString &text);
    // The translation path, shared by the editor's button and the standalone
    // overlay: read the selection, run it through the CLI's two steps, and
    // place the result.  `computeTranslation` does the subprocesses and the
    // parse; the two callers differ only in what they keep afterwards.
    bool computeTranslation(QVector<TranslatedLine> *lines, QString *text, QString *error);
    bool runTranslationPipeline(const QImage &pixels, QByteArray *document,
                                QString *error) const;
    bool runTranslateStage(QString *error);
    bool acceptTranslation(QString *error);
    void mutateAnnotations(QVector<Annotation> next);
    void drawLoupe(CaptureOverlay *overlay, QPainter *painter);
    // The capture with the marks already on it, as the magnifier reads it: the
    // frozen frame, a band of `kLoupeCompositeMargin` device pixels round it,
    // and every committed mark painted in.  Returns null where there is
    // nothing to read.
    //
    // It is a separate image from the frame on purpose.  The frame is the
    // capture's own pixels and is what the marks are drawn *over* -- the result
    // is composited from it, the mosaic samples it, and the pin's HDR half is
    // built from it -- so painting marks into it would put them in the saved
    // picture twice.  Rebuilt only when the marks, the frame or the frame's
    // place have changed; see `loupeCompositeKey`.
    const QImage *loupeFrame(const OutputSession &output);
    // Everything the composite depends on.  A change to any of it is what
    // makes the next magnifier paint rebuild rather than reuse the pixels.
    QByteArray loupeCompositeKey(const OutputSession &output) const;
    // Throws the composite away: the marks are about to change, or the frame
    // has moved, so what is cached no longer describes the screen.
    void invalidateLoupeComposite();
    // The colour readout that hangs off the loupe: the pixel's code, on a
    // ground of that pixel's own colour.  Drawn as its own pill rather than
    // folded into the loupe's, because the two say different things -- one
    // where the cursor is, one what is under it -- and a hint line under a
    // swatch is unreadable on a swatch that happens to match it.
    void drawColorPill(CaptureOverlay *overlay, QPainter *painter, const QPointF &anchor,
                       const QColor &color);
    // Puts the magnifier up for its moment after a keyboard move, restarting
    // the countdown if it was already up.
    void flashMagnifier();
    void endMagnifierFlash();
    // The image pixel under the cursor, as the magnifier reads it, or an
    // invalid colour where there is no image under it.  `pixelIndex` receives
    // the source pixel's coordinates, which is what the loupe's own pill shows.
    QColor pixelUnderCursor(int *pixelIndexX, int *pixelIndexY) const;
    // The frozen frame of an output, or null where it has none to read.
    const QImage *outputFrame(const OutputSession &output) const;
    // C copies the colour code, A takes it as the current tool's colour.  Both
    // are bound while the magnifier is up and swallowed otherwise.
    void copyColorUnderCursor();
    void adoptColorUnderCursor();
    // Walks the cursor by `dx`/`dy` logical pixels with the keyboard, which is
    // how a start point is picked without the mouse: the magnifier comes up so
    // the user can see where it landed.
    void moveCursorBy(int dx, int dy);
    // Walks the *stroke in progress* by `dx`/`dy` logical pixels, for the tools
    // whose press picks a start and whose release picks an end: the button is
    // held for the whole of it, so the keyboard is the only way to place the far
    // end exactly.  The anchor the press set does not move.
    void walkLiveGesture(int dx, int dy);
    // The cursor the keyboard moves: where the last keyboard step left it, or
    // where the pointer was if the keyboard has not been used yet.
    Point cursorPoint() const;
    // Moves the selected mark by `dx`/`dy` logical pixels.  A run of these is
    // one undo step, so holding a key down does not bury the user's last edit.
    void nudgeSelectedAnnotation(int dx, int dy);
    // Moves the cursor to the next or previous mark, so the keyboard alone can
    // walk the marks and the arrows can then nudge the one it stopped on.
    void cycleAnnotationFocus(int step);
    // Whether `modifiers` puts the editor in its selection state: what is on
    // the screen is the target, and the armed tool waits.
    //
    // Shift is the modifier that answers to it: it is the state the removed
    // Select tool left behind, and holding it must not change which tool is
    // armed, so letting it go puts the user back exactly where they were.
    bool pickingMarks(int modifiers) const;
    // Ctrl+A: every mark is picked up at once, for a style change or a delete
    // that reaches all of them.
    void selectAllAnnotations();
    // Ctrl+S: the capture with its marks on it goes to the clipboard as an
    // image, through the same composite the Copy button would have made.
    void copyToClipboard();
    // Draws the in-progress freehand stroke from a raster that only grows by the
    // points appended since the last paint.
    void paintLiveStroke(QPainter *painter, const OutputSession &output, const QSize &size,
                         int outputIndex);
    bool annotationBounds(const Annotation &annotation, LogicalRect *bounds) const;
    bool canDrawAt(Point point) const;
};

class CaptureOverlay final : public QWidget {
public:
    CaptureOverlay(int outputIndex, OverlayController *controller, QScreen *screen);
    ~CaptureOverlay() override;

    int outputIndex() const;
    const OutputSession &output() const;
    QPointF localFromGlobal(Point point) const;
    // Restricts which parts of this surface take pointer input.  Wayland has no
    // empty-region request -- Qt sends nothing for an empty mask, which the
    // compositor reads as "the whole surface is interactive" -- so an empty
    // region is turned into one parked outside the surface, which is the same
    // click-through the pin daemon's own surfaces use.
    void setInputMask(const QRegion &mask);
    bool showLayerSurface();
    // Floating layer surface carved to a specific global logical rect
    // (top-left anchored + margins): used by the pin editor.
    bool showLayerSurfaceAt(int globalX, int globalY, int width, int height);

    // Called when the controller that drove this overlay is destroyed.  The
    // overlay outlives it -- the caller owns the widget and deletes it -- and a
    // window left painting through a dead controller is a crash waiting for the
    // next turn of the event loop.
    void detachController();

    // Whether the magnifier in front of the user is the colour picker's, and
    // whether any magnifier is up at all.  Public because the two are a
    // distinction the offline checks have to make and cannot see from the
    // pixels alone: the picker's loupe and the drag loupe are the same widget
    // drawn the same way, and the only difference is whether the pill under it
    // says a colour or a coordinate.
    bool colorPickerVisible() const;
    bool magnifierVisible() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    // Takes the keyboard while the pointer is over the surface and gives it back
    // when the pointer leaves.  The surface asks for exclusive interactivity --
    // it is a full-output editor and every key belongs to it while the user is
    // working in it -- but exclusive interactivity is not scoped to focus: the
    // compositor routes every key to a mapped surface that holds it, so an
    // editor left holding it swallows the keyboard of whatever the user switches
    // to.  Following the pointer is what scopes it: on the surface the user is
    // typing into the editor, off it they are typing into their own window.
    //
    // The same pair `PinSurface` uses, and for the same reason.
    void wantKeyboard();
    void offerKeyboardBack();

    int outputIndex_;
    OverlayController *controller_;
    QScreen *screen_;
    QWindow *layerWindow_ = nullptr;
    // Whether this surface is the one currently holding the keyboard, so a
    // repeated enter or leave does not re-send an interactivity the compositor
    // already has.  It starts true because both layer surfaces are created with
    // exclusive interactivity and `setActivateOnShow`: the surface has the
    // keyboard from the moment it is shown, and a flag that started false would
    // make the first leave a no-op -- the one leave that most needs to give it
    // back.
    bool keyboardWanted_ = true;
    // The input region last handed to the window, so an unchanged one is not
    // re-sent to the compositor on every frame of a pin drag.
    QRegion inputMask_;
};

} // namespace vshot

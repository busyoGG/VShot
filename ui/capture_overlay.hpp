// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#pragma once

#include "session_protocol.hpp"

#include <QColor>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>
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

struct Point {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

inline bool operator==(const Point &first, const Point &second)
{
    return first.x == second.x && first.y == second.y;
}

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
    };

    Kind kind = Kind::Stroke;
    QString tool;
    LogicalRect rect;
    QVector<Point> points;
    Point origin;
    QString text;
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
    // Output scale the label was drawn on; the text bitmap is rasterized at
    // this device ratio.
    std::uint32_t deviceRatio = 1;
    // The pasted image itself, at its own resolution (`Kind::Image` only).
    QImage pixels;
};

inline bool annotationEquals(const Annotation &first, const Annotation &second)
{
    if (first.kind != second.kind || first.tool != second.tool || first.dash != second.dash ||
        first.size != second.size || first.arrowStyle != second.arrowStyle ||
        first.mask != second.mask || first.strength != second.strength ||
        first.textPixels != second.textPixels || first.color != second.color ||
        first.width != second.width || first.font != second.font ||
        first.deviceRatio != second.deviceRatio) {
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

enum class Tool {
    Select,
    Rectangle,
    Ellipse,
    Arrow,
    Pen,
    Text,
    Mosaic,
};

class CaptureOverlay;

class OverlayController final {
public:
    explicit OverlayController(Session session);
    ~OverlayController();

    OverlayController(const OverlayController &) = delete;
    OverlayController &operator=(const OverlayController &) = delete;

    int outputCount() const;
    const Session &session() const;
    CaptureOverlay *addOverlay(int outputIndex, QScreen *screen, QString *error);

    void paint(CaptureOverlay *overlay, QPainter *painter);
    void press(CaptureOverlay *overlay, const QPointF &local, Qt::MouseButton button,
               Qt::KeyboardModifiers modifiers);
    void move(CaptureOverlay *overlay, const QPointF &local, Qt::MouseButtons buttons,
              Qt::KeyboardModifiers modifiers);
    void release(CaptureOverlay *overlay, const QPointF &local, Qt::MouseButton button,
                 Qt::KeyboardModifiers modifiers);
    void doubleClick(CaptureOverlay *overlay, const QPointF &local, Qt::MouseButton button);
    void key(CaptureOverlay *overlay, int key, Qt::KeyboardModifiers modifiers);

    void chooseTool(Tool tool);
    void setCurrentColor(const QColor &color);
    void setCurrentFont(const QString &family);
    void setWidth(std::uint32_t width);
    void setDash(const QString &dash);
    void setArrowSize(std::uint32_t size);
    void setArrowStyle(const QString &style);
    void setTextSize(std::uint32_t size);
    void setMosaicShape(const QString &shape);
    void setMosaicStrength(std::uint32_t strength);
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
    // Reads the text in the selection and puts it on the clipboard. The
    // recognition itself runs in a `vshot ocr --input` child, because the
    // engine is on the Rust side and this process draws a layer surface that
    // cannot be blocked on a model load. `error` is filled when there is
    // nothing to read, the child cannot be started, or it fails.
    bool copySelectionText(QString *error);
    void notifyPanelDragged();
    void undo();
    void redo();
    void confirm();
    void cancel();

    bool isFinished() const;
    bool isCancelled() const;
    bool isPinEdit() const { return pinEdit_; }
    // Pin-edit mode: the whole session bounds is the editable canvas; the
    // selection is fixed and the toolbar shows immediately. Call before the
    // overlay is shown.
    void setPinEditMode(bool enabled) { pinEdit_ = enabled; }
    // Pin-edit mode: the editor drives the real pin window over the daemon
    // socket rather than drawing a second copy of the image. Call before the
    // overlay is shown.
    void setPinTarget(std::uint64_t pinId, const QString &socketPath);
    // Enters editing state over the fixed canvas (shows the toolbar).
    void beginPinEdit();
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
    bool hasValidSelection() const;
    const std::optional<LogicalRect> &selection() const;
    const QVector<Annotation> &annotations() const;
    QJsonDocument resultDocument(const QString &bitmapDirectory = QString(), QString *error = nullptr) const;

    void setTerminalCallback(std::function<void()> callback);

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
    // Window picking: the session's candidate windows are what the pointer may
    // snap to, so the first click replaces the free-hand drag that region
    // capture starts with.
    bool pickMode_ = false;
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
    Tool tool_ = Tool::Select;
    QColor currentColor_{255, 64, 64, 255};
    QString currentFont_;
    std::uint32_t currentWidth_ = 2;
    // Font height for the next label, in logical pixels -- the same number the
    // size box shows.
    std::uint32_t textSize_ = 14;
    QString currentDash_ = QStringLiteral("solid");
    std::uint32_t arrowSize_ = 1;
    QString currentArrowStyle_ = QStringLiteral("open");
    QString mosaicShape_ = QStringLiteral("rect");
    std::uint32_t mosaicStrength_ = 2;
    bool panelPinned_ = false;
    // Automatic toolbar placement anchor: while the selection stays put, the
    // panel keeps the edge adjacent to the selection fixed so style-row
    // toggles never shift the command bar.
    QRect toolbarAnchorSelection_;
    QPoint toolbarAnchor_;
    int toolbarAnchorHeight_ = 0;
    bool toolbarAnchorBelow_ = false;
    bool toolbarAnchorValid_ = false;
    // Selected annotation adjustment (move/resize under the Select tool).
    int selectedAnnotation_ = -1;
    Annotation dragAnnotation_;
    QVector<Annotation> dragSnapshot_;
    bool dragMoved_ = false;
    bool styleAdjustmentActive_ = false;
    bool styleAdjustmentChanged_ = false;
    QVector<Annotation> styleAdjustmentSnapshot_;
    Gesture *gesture_ = nullptr;
    bool editing_ = false;
    bool pinEdit_ = false;
    /// `region-only`: a finished drag ends the session with the rectangle
    /// instead of opening the editor.  Scrolling capture asks for this, since
    /// the pixels it will annotate do not exist until the stitch is done.
    bool selectOnly_ = false;
    bool finished_ = false;
    bool cancelled_ = false;
    std::function<void()> terminalCallback_;
    mutable int textBitmapIndex_ = 0;
    // The same counter for pasted images' pixel files, in the same directory.
    mutable int imageBitmapIndex_ = 0;
    // Live pin window the editor drives in pin-edit mode. The daemon answers
    // exactly one request per connection and then closes, so each move gets a
    // fresh socket instead of a reconnected one.
    std::uint64_t pinId_ = 0;
    QString pinSocketPath_;
    QLocalSocket *pinSocket_ = nullptr;
    QByteArray pinReplyBuffer_;
    // Moves are coalesced: while one request is in flight the newest position
    // waits here, since a drag produces far more motion than the daemon needs.
    std::optional<Point> pendingPinOrigin_;

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
    void flushPinMove();
    void applyPinReply(QByteArray line);
    void consumePinReply(QLocalSocket *socket);
    void applyPinRect(const LogicalRect &rect);
    Point clampPoint(Point point) const;
    int candidateIndexAt(Point point) const;
    QString candidatePillText() const;
    bool applyCandidateHover(Point point, CaptureOverlay *overlay);
    // Replaces the candidate list with a fresh one and points the hover at
    // whatever the (unmoved) pointer is over now.
    void applyCandidates(QVector<WindowCandidate> candidates);
    void readCandidateReplies();
    LogicalRect selectionBetween(Point first, Point second) const;
    LogicalRect moveSelection(LogicalRect origin, Point anchor, Point current) const;
    LogicalRect resizeSelection(LogicalRect origin, int handle, Point current) const;
    int hitHandle(Point point) const;
    void startSelection(Point point);
    void updateSelection(Point point);
    void finishSelection(Point point);
    void beginDrawing(Point point);
    void updateDrawing(Point point);
    void finishDrawing(Point point);
    void beginText(CaptureOverlay *overlay, Point point);
    void startTextEditor(CaptureOverlay *overlay, int index, Point origin);
    void finishText(bool accept);
    int annotationHitAt(Point point) const;
    int annotationHandleAt(Point point) const;
    void beginAnnotationDrag(Point point, bool resize);
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
    void updateAll();
    void terminal(bool cancelled);
    void removeTextEditor();
    void mutateAnnotations(QVector<Annotation> next);
    void drawLoupe(CaptureOverlay *overlay, QPainter *painter);
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
    bool showLayerSurface();
    // Floating layer surface carved to a specific global logical rect
    // (top-left anchored + margins): used by the pin editor.
    bool showLayerSurfaceAt(int globalX, int globalY, int width, int height);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    int outputIndex_;
    OverlayController *controller_;
    QScreen *screen_;
    QWindow *layerWindow_ = nullptr;
};

} // namespace vshot

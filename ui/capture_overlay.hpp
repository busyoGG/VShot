#pragma once

#include "session_protocol.hpp"

#include <QColor>
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
    };

    Kind kind = Kind::Stroke;
    QString tool;
    LogicalRect rect;
    QVector<Point> points;
    Point origin;
    QString text;
    std::uint32_t scale = 2;
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
};

inline bool annotationEquals(const Annotation &first, const Annotation &second)
{
    if (first.kind != second.kind || first.tool != second.tool || first.dash != second.dash ||
        first.size != second.size || first.arrowStyle != second.arrowStyle ||
        first.mask != second.mask || first.strength != second.strength ||
        first.scale != second.scale || first.color != second.color ||
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
    // Enters editing state over the fixed canvas (shows the toolbar).
    void beginPinEdit();
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
    Point pointer_;
    int pointerOutput_ = -1;
    Tool tool_ = Tool::Select;
    QColor currentColor_{255, 64, 64, 255};
    QString currentFont_;
    std::uint32_t currentWidth_ = 2;
    std::uint32_t textSize_ = 2;
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
    bool finished_ = false;
    bool cancelled_ = false;
    std::function<void()> terminalCallback_;
    mutable int textBitmapIndex_ = 0;

    Point globalPoint(CaptureOverlay *overlay, const QPointF &local) const;
    Point clampPoint(Point point) const;
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

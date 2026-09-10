#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QWidget>

#include <functional>

class QScreen;

namespace vshot {

// One pinned image: content painted inside a full-output transparent
// layer-shell surface. Dragging moves it, the wheel scales it, a double-click
// closes just this pin, and Space (after a click focuses it) opens the
// annotation editor; all bulk operations live in the pin server.
//
// The surface is deliberately NOT sized to the image: layer margins are the
// only way to position a layer surface, and every margin change costs a full
// asynchronous configure round trip, which makes dragging trail the cursor.
// Keeping the surface fixed and repainting the image at an offset makes
// drags pure client-side repaints; the input mask follows the image rect so
// the rest of the output stays interactive.
class PinWindow final : public QWidget {
public:
    PinWindow(const QImage &image, QScreen *screen);

    // Invoked when the user double-clicks the surface, before it closes.
    void setCloseCallback(std::function<void()> callback)
    {
        closeRequested_ = std::move(callback);
    }

    // Invoked when the user presses Space while this pin has keyboard focus.
    void setEditCallback(std::function<void()> callback)
    {
        editRequested_ = std::move(callback);
    }

    // True while this pin owns keyboard focus (an edit session is running).
    bool hasKeyboardFocus() const { return hasFocus_; }

    // The image's display rect in output-local logical coordinates.
    QRect displayRect() const { return paintedRect_; }
    QScreen *screen() const { return screen_; }

    // Maps the widget onto its layer-shell surface. Returns false when
    // LayerShellQt is unavailable.
    bool showLayerSurface();

    // Bulk visibility, driven by the server's show/hide/toggle commands.
    void setPinnedVisible(bool visible);
    bool isPinnedVisible() const { return visible_; }

    // Positions the image by its top-left, in output-local logical pixels.
    void placeAt(QPoint topLeft);
    // Centers the image on its own output, nudged by a small cascade offset
    // so repeated pins stay distinguishable.
    void placeCentered(QPoint cascadeOffset);

    const QImage &sourceImage() const { return source_; }
    void setLabel(const QString &label) { label_ = label; }
    const QString &label() const { return label_; }

    // Swaps the pin's pixels (pin-edit result) while keeping its on-screen
    // position and apparent size.
    void setSourceImage(const QImage &image);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    static constexpr double kMinScale = 0.1;
    static constexpr double kMaxScale = 8.0;
    // Keep this many logical pixels on screen so a pin can never be lost.
    static constexpr int kGrabMargin = 32;
    // How long the zoom factor badge stays visible after the last wheel step.
    static constexpr int kZoomBadgeMs = 900;

    QSize displaySize() const;
    QPoint clampMargin(QPoint candidate) const;
    // Moves the input mask to the image rect and repaints the covered area.
    void applyGeometry();

    QImage source_;
    QScreen *screen_;
    std::function<void()> closeRequested_;
    std::function<void()> editRequested_;
    bool hasFocus_ = false;

    // Image top-left in output-local logical pixels.
    QPoint margin_{0, 0};
    QRect paintedRect_;
    // Device pixels per logical pixel of `source_` (clamped 1..4): 1 for
    // plain pins and captures, higher for cards rendered at HiDPI density.
    qreal imageRatio_ = 1.0;
    double scale_ = 1.0;
    QString label_;
    QString zoomLabel_;
    class QTimer *zoomTimer_ = nullptr;
    bool visible_ = true;
    bool surfaceReady_ = false;

    bool dragging_ = false;
    QPoint pressGlobal_;
    QPoint startMargin_;
};

} // namespace vshot

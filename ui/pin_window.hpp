#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QWidget>

#include <functional>

class QScreen;

namespace vshot {

// One rendering surface of one pinned image, covering one whole output.
//
// A layer-shell surface belongs to exactly one output, so a pin that spans two
// outputs is rendered by two of these, each painting the overlapping part.
// The pin's real state — image, scale, global position — lives in the daemon
// (see pin_server.cpp): every surface is a view onto that shared state, which
// is what makes dragging a pin across outputs work without handing the gesture
// over to another surface mid-drag.
//
// The surface is deliberately NOT sized to the image: layer margins are the
// only way to position a layer surface, and every margin change costs a full
// asynchronous configure round trip, which makes dragging trail the cursor.
// Keeping the surface fixed and repainting the image at an offset makes drags
// pure client-side repaints; the input mask follows the image rect so the rest
// of the output stays interactive.
class PinWindow final : public QWidget {
public:
    // `density` is the image's device pixels per logical pixel (1..4). The
    // pin's natural zoom is one device pixel per device pixel, i.e. the image
    // is shown one logical pixel per logical pixel on an output of the same
    // density, and scaled down proportionally on a denser or coarser one.
    PinWindow(const QImage &image, int density, QScreen *screen);

    // Invoked when the user double-clicks the image, before it closes.
    void setCloseCallback(std::function<void()> callback)
    {
        closeRequested_ = std::move(callback);
    }

    // Invoked when the user presses Space while this surface has keyboard focus.
    void setEditCallback(std::function<void()> callback)
    {
        editRequested_ = std::move(callback);
    }

    // Drags and zooms are delegated to the daemon, which owns the shared
    // geometry and has to move every surface of the pin, not just this one.
    void setDragCallback(std::function<void(QPoint)> callback)
    {
        dragMoved_ = std::move(callback);
    }
    void setZoomCallback(std::function<void(double, QPoint)> callback)
    {
        zoomRequested_ = std::move(callback);
    }

    // True while this surface owns keyboard focus (an edit session is running).
    bool hasKeyboardFocus() const { return hasFocus_; }

    // The output this surface is mapped onto.
    QScreen *screen() const { return screen_; }

    // Maps the widget onto its layer-shell surface. Returns false when
    // LayerShellQt is unavailable.
    bool showLayerSurface();

    // Bulk visibility, driven by the server's show/hide/toggle commands.
    void setPinnedVisible(bool visible);
    bool isPinnedVisible() const { return visible_; }

    // Global logical top-left of the image; this surface paints the part that
    // overlaps its own output.
    void setGlobalOrigin(QPoint topLeft);
    QPoint globalOrigin() const { return globalOrigin_; }
    // The image's rect in global logical pixels.
    QRect globalDisplayRect() const;

    // Logical pixels per source pixel, so `displaySize()` is the on-screen
    // size. The daemon owns it; 1 / density is the natural size.
    void setScale(double scale);
    double scale() const { return scale_; }
    // Device pixels per logical pixel of the pinned image, fixed at
    // construction. The zoom badge reports its factor relative to this
    // density, so a 4K capture on a 4K output reads as 100%.
    int density() const { return density_; }

    // Repaints the transient badge showing the current zoom factor.
    void showZoomBadge();

    const QImage &sourceImage() const { return source_; }
    void setLabel(const QString &label) { label_ = label; }
    const QString &label() const { return label_; }

    // Swaps the pin's pixels (pin-edit result); the daemon re-applies the
    // scale so the on-screen size and position stay put.
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
    // How long the zoom factor badge stays visible after the last wheel step.
    static constexpr int kZoomBadgeMs = 900;

    QSize displaySize() const;
    // The image's rect in this surface's output-local coordinates.
    QRect localDisplayRect() const;
    // The image's rect in this surface's device pixels.
    QSize deviceTargetSize() const;
    // The image at this surface's device resolution, cached: painting the
    // source straight into the rect makes Qt resample it per frame with a 2x2
    // filter, which softens a 4K capture pinned on a 1080p output. Scaling
    // once here uses Qt's area filter instead.
    const QImage &renderSource();
    // Moves the input mask to the image rect and repaints the covered area.
    void applyGeometry();

    QImage source_;
    QScreen *screen_;
    std::function<void()> closeRequested_;
    std::function<void()> editRequested_;
    std::function<void(QPoint)> dragMoved_;
    std::function<void(double, QPoint)> zoomRequested_;
    bool hasFocus_ = false;

    // Image top-left in global logical pixels.
    QPoint globalOrigin_{0, 0};
    // Image rect in output-local logical pixels (derived from globalOrigin_).
    QRect paintedRect_;
    double scale_ = 1.0;
    int density_ = 1;
    // Downscaled copy of source_ at this surface's device resolution, keyed by
    // the source image and the target size.
    QImage render_;
    qint64 renderKey_ = 0;
    QSize renderTarget_;
    QString label_;
    QString zoomLabel_;
    class QTimer *zoomTimer_ = nullptr;
    bool visible_ = true;
    bool surfaceReady_ = false;

    bool dragging_ = false;
    QPoint pressGlobal_;
    QPoint pressOrigin_;
};

} // namespace vshot

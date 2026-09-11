#include "pin_window.hpp"

#include <LayerShellQt/Window>

#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRegion>
#include <QScreen>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>

namespace vshot {

namespace {

// The focused outline's 2px pen is centered on the rect edge and paints up
// to ~2px outside paintedRect_; every repaint region must include that
// bleed, or stale border pixels survive at the previous position.
constexpr int kOutlineBleedPx = 3;

QRect expandOutline(const QRect &rect)
{
    return rect.adjusted(-kOutlineBleedPx, -kOutlineBleedPx, kOutlineBleedPx, kOutlineBleedPx);
}

// Wayland has no "no input here" request: an unset input region means the
// whole surface is interactive, and Qt sends no request at all for an empty
// mask, which is exactly that default. A region parked outside the surface is
// the portable way to say "click straight through": the compositor
// intersects it with the surface and nothing is ever hit. Without this, a pin
// image sitting on another output leaves this output's surface eating every
// click on that screen.
const QRegion &clickThroughInputRegion()
{
    static const QRegion region(QRect(-8, -8, 1, 1));
    return region;
}

} // namespace

PinWindow::PinWindow(const QImage &image, int density, QScreen *screen)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint)
    , source_(image)
    , screen_(screen)
    , density_(std::clamp(density, 1, 4))
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setCursor(Qt::SizeAllCursor);
    // Natural zoom: one source pixel per logical pixel of an output with the
    // same density as the image, so a 4K capture takes the same room on screen
    // it did where it was taken. The daemon sets the scale right after
    // construction; this only avoids a wrong first frame.
    scale_ = 1.0 / density_;
    zoomTimer_ = new QTimer(this);
    zoomTimer_->setSingleShot(true);
    connect(zoomTimer_, &QTimer::timeout, this, [this] {
        zoomLabel_.clear();
        update(paintedRect_);
    });
    if (screen_ != nullptr) {
        // One surface per output, covering the whole output; the image is
        // painted at an offset inside it. The compositor confirms this via
        // the layer configure, this only avoids a wrong-size first frame.
        resize(screen_->geometry().size());
    }
}

bool PinWindow::showLayerSurface()
{
    winId();
    QWindow *window = windowHandle();
    if (window == nullptr) {
        return false;
    }
    auto *layer = LayerShellQt::Window::get(window);
    if (layer == nullptr) {
        return false;
    }
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    // Anchor all four edges with zero margins: the surface always covers its
    // whole output, so moving the image never involves the compositor.
    LayerShellQt::Window::Anchors anchors(LayerShellQt::Window::AnchorTop);
    anchors |= LayerShellQt::Window::AnchorBottom;
    anchors |= LayerShellQt::Window::AnchorLeft;
    anchors |= LayerShellQt::Window::AnchorRight;
    layer->setExclusiveZone(-1);
    // On-demand keyboard focus: a pin only takes the keyboard after a click,
    // and releases it as soon as the user clicks elsewhere. This is what
    // makes the Space edit shortcut work without a global grab.
    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
    layer->setScope(QStringLiteral("vshot-pin"));
    layer->setDesiredSize(QSize(0, 0)); // follow the anchored edges
    layer->setScreen(screen_);
    surfaceReady_ = true;
    applyGeometry();
    show();
    applyGeometry(); // re-issue now that the platform window exists
    return true;
}

void PinWindow::setPinnedVisible(bool visible)
{
    visible_ = visible;
    setVisible(visible);
}

void PinWindow::setSourceImage(const QImage &image)
{
    if (image.isNull()) {
        return;
    }
    source_ = image;
    // Pixels changed even when the rect stays identical: applyGeometry skips
    // the repaint in that case, so force one here (widened for the outline).
    applyGeometry();
    update(expandOutline(paintedRect_));
}

void PinWindow::setGlobalOrigin(QPoint topLeft)
{
    globalOrigin_ = topLeft;
    applyGeometry();
}

void PinWindow::setScale(double scale)
{
    if (scale == scale_) {
        return;
    }
    scale_ = scale;
    applyGeometry();
}

QSize PinWindow::deviceTargetSize() const
{
    const qreal ratio = std::max<qreal>(1.0, devicePixelRatioF());
    return QSize(std::max(1, qRound(paintedRect_.width() * ratio)),
                 std::max(1, qRound(paintedRect_.height() * ratio)));
}

const QImage &PinWindow::renderSource()
{
    if (source_.isNull()) {
        return source_;
    }
    const QSize target = deviceTargetSize();
    if (target == source_.size()) {
        // Already one device pixel per device pixel: a plain 1:1 blit.
        return source_;
    }
    if (!render_.isNull() && renderKey_ == source_.cacheKey() && renderTarget_ == target) {
        return render_;
    }
    render_ = source_.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (render_.isNull()) {
        // Out of memory: fall back to letting the painter resample.
        return source_;
    }
    renderKey_ = source_.cacheKey();
    renderTarget_ = target;
    return render_;
}

QSize PinWindow::displaySize() const
{
    return QSize(std::max(1, qRound(source_.width() * scale_)),
                 std::max(1, qRound(source_.height() * scale_)));
}

QRect PinWindow::localDisplayRect() const
{
    const QPoint origin = screen_ != nullptr ? screen_->geometry().topLeft() : QPoint(0, 0);
    return QRect(globalOrigin_ - origin, displaySize());
}

QRect PinWindow::globalDisplayRect() const
{
    return QRect(globalOrigin_, displaySize());
}

void PinWindow::applyGeometry()
{
    const QRect nextRect = localDisplayRect();
    const QRect previous = paintedRect_;
    paintedRect_ = nextRect;
    // The input mask always follows the image so the rest of the output
    // keeps receiving clicks. It must be set on the QWindow, not the widget:
    // QWidget::setMask also tells Qt to stop repainting outside the mask,
    // which on Wayland (input region only, pixels still composited) leaves
    // stale pixels behind as ghosting when the image moves. An image that is
    // entirely on another output gets the click-through region above.
    if (QWindow *window = windowHandle()) {
        QRegion mask = QRegion(nextRect.intersected(QRect(QPoint(0, 0), size())));
        if (mask.isEmpty()) {
            if (dragging_) {
                // A drag that carries the image onto another output empties
                // this surface's region while this is still the surface
                // holding the pointer grab. Widen it for the rest of the
                // gesture: an empty input region is exactly the state that
                // can drop the grab, and while a button is held no other
                // client can receive input anyway. The release recomputes
                // the region.
                mask = QRegion(rect());
            } else {
                mask = clickThroughInputRegion();
            }
        }
        window->setMask(mask);
    }
    if (!surfaceReady_ || nextRect == previous) {
        return;
    }
    // Repaint the old image area (clearing it) plus the new one, widened by
    // the outline bleed: the focused pen is 2px wide and centered on the
    // rect edge, so it paints up to ~2px outside paintedRect_. Without the
    // margin, dragging a focused pin leaves a border-shaped ghost at the
    // old rect's right/bottom edges.
    update(previous.isNull() ? expandOutline(nextRect)
                             : expandOutline(previous.united(nextRect)));
}

void PinWindow::showZoomBadge()
{
    // The factor is relative to the image's native density, so a 4K capture
    // pinned at its natural size on a 4K output reads as 100%.
    zoomLabel_ = QString::number(qRound(scale_ * density_ * 100));
    zoomTimer_->start(kZoomBadgeMs);
    update(paintedRect_);
}

void PinWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    // The painter is clipped to the dirty region requested by update(),
    // which always includes the area the image just left. Force-clear that
    // area to transparent first: relying on Qt's implicit background clear
    // leaves stale pixels behind as ghosting during drags.
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    if (!paintedRect_.intersects(rect())) {
        // Every visible pixel of the image lives on another output.
        return;
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // renderSource() is already at this surface's device resolution when the
    // source is denser or coarser than the output, so this is a 1:1 blit then.
    painter.drawImage(paintedRect_, renderSource());
    // A thin outline keeps the pinned image distinguishable from identical
    // content behind it; a focused pin (Space = edit) gets a bright one.
    painter.setPen(hasFocus_ ? QPen(QColor(255, 255, 255, 200), 2.0)
                             : QPen(QColor(0, 0, 0, 120), 1.0));
    painter.drawRect(QRectF(paintedRect_.x() + 0.5, paintedRect_.y() + 0.5,
                            paintedRect_.width() - 1.0, paintedRect_.height() - 1.0));
    if (!zoomLabel_.isEmpty()) {
        // Transient zoom badge pinned to the image's bottom-right corner that
        // is still on this output. The font size is fixed: the badge reports
        // the factor, it must not grow with the image itself.
        QFont font = painter.font();
        font.setPixelSize(16);
        font.setBold(true);
        painter.setFont(font);
        const QFontMetrics metrics(font);
        const QString text = QStringLiteral("%1%").arg(zoomLabel_);
        const QRect textRect = metrics.boundingRect(text);
        const int pad = metrics.height() / 3;
        QRect badge = textRect.adjusted(-pad, -pad / 2, pad, pad / 2);
        const QRect corner = paintedRect_.intersected(rect());
        badge.moveBottomRight(corner.bottomRight() - QPoint(pad, pad));
        // A pin may hang partially off-screen; keep the badge readable.
        badge = badge.intersected(rect().adjusted(0, 0, -1, -1));
        if (badge.width() > 0 && badge.height() > 0) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 160));
            painter.drawRoundedRect(badge, 6, 6);
            painter.setPen(Qt::white);
            painter.drawText(badge, Qt::AlignCenter, text);
        }
    }
}

void PinWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        dragging_ = true;
        pressGlobal_ = event->globalPosition().toPoint();
        pressOrigin_ = globalOrigin_;
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PinWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (dragging_) {
        // Global deltas are frame-independent, so they map straight onto the
        // shared global position. The daemon applies the move to every
        // surface of this pin, so dragging across outputs keeps working: the
        // pointer stays grabbed by the surface the drag started on.
        if (dragMoved_) {
            dragMoved_(pressOrigin_ + event->globalPosition().toPoint() - pressGlobal_);
        }
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void PinWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        // The drag widened the input region past the image; put it back now
        // that the pointer is free again (full-surface repaint clears it).
        applyGeometry();
        update();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void PinWindow::wheelEvent(QWheelEvent *event)
{
    const QPoint steps = event->angleDelta();
    if (steps.y() == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    // 10% per notch, multiplicative so zooming feels even at any scale, and
    // keep the point under the cursor stationary while zooming. The daemon
    // anchors the zoom and rescales every surface at once.
    const double factor = steps.y() > 0 ? 1.1 : 1.0 / 1.1;
    if (zoomRequested_) {
        zoomRequested_(factor, event->globalPosition().toPoint());
    }
    showZoomBadge();
    event->accept();
}

void PinWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        event->accept();
        if (editRequested_) {
            editRequested_();
        }
        return;
    }
    QWidget::keyPressEvent(event);
}

void PinWindow::focusInEvent(QFocusEvent *event)
{
    hasFocus_ = true;
    // The outline style depends on focus; repaint the ring explicitly so the
    // change never waits for an unrelated repaint to piggyback on.
    update(expandOutline(paintedRect_));
    QWidget::focusInEvent(event);
}

void PinWindow::focusOutEvent(QFocusEvent *event)
{
    hasFocus_ = false;
    update(expandOutline(paintedRect_));
    QWidget::focusOutEvent(event);
}

void PinWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        event->accept();
        if (closeRequested_) {
            closeRequested_();
        }
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace vshot

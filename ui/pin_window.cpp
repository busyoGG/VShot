#include "pin_window.hpp"

#include <LayerShellQt/Window>

#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRegion>
#include <QTimer>
#include <QWheelEvent>

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

} // namespace

PinWindow::PinWindow(const QImage &image, QScreen *screen)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint)
    , source_(image)
    , screen_(screen)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setCursor(Qt::SizeAllCursor);
    // Text cards are rasterized at the output's pixel density; the natural
    // zoom for every image is one device pixel per logical pixel.
    imageRatio_ = std::clamp(source_.devicePixelRatio(), 1.0, 4.0);
    scale_ = 1.0 / imageRatio_;
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
    // Keep what the user sees stable: preserve the on-screen size even though
    // the pixel content changed (the edited image is 1:1 with the display).
    const QSize display = displaySize();
    source_ = image;
    if (image.width() > 0) {
        scale_ = std::clamp(static_cast<double>(display.width()) / image.width(), kMinScale,
                            kMaxScale);
    }
    // Pixels changed even when the rect stays identical: applyGeometry skips
    // the repaint in that case, so force one here (widened for the outline).
    applyGeometry();
    update(expandOutline(paintedRect_));
}

void PinWindow::placeAt(QPoint topLeft)
{
    margin_ = clampMargin(topLeft);
    applyGeometry();
}

void PinWindow::placeCentered(QPoint cascadeOffset)
{
    if (screen_ == nullptr) {
        placeAt(cascadeOffset);
        return;
    }
    const QRect bounds = screen_->geometry();
    const QSize size = displaySize();
    const QPoint center((bounds.width() - size.width()) / 2,
                        (bounds.height() - size.height()) / 2);
    placeAt(center + cascadeOffset);
}

QSize PinWindow::displaySize() const
{
    return QSize(std::max(1, qRound(source_.width() * scale_)),
                 std::max(1, qRound(source_.height() * scale_)));
}

QPoint PinWindow::clampMargin(QPoint candidate) const
{
    const QSize size = displaySize();
    if (screen_ == nullptr) {
        return candidate;
    }
    const QSize outputSize = screen_->geometry().size();
    // Keep at least kGrabMargin logical pixels of the image reachable so a
    // pin can never be dragged off-screen for good.
    const int minX = std::min(0, outputSize.width() - kGrabMargin - size.width());
    const int minY = std::min(0, outputSize.height() - kGrabMargin - size.height());
    const int maxX = std::max(minX, outputSize.width() - kGrabMargin);
    const int maxY = std::max(minY, outputSize.height() - kGrabMargin);
    return QPoint(std::clamp(candidate.x(), minX, maxX),
                  std::clamp(candidate.y(), minY, maxY));
}

void PinWindow::applyGeometry()
{
    const QRect nextRect(margin_, displaySize());
    const QRect previous = paintedRect_;
    paintedRect_ = nextRect;
    // The input mask always follows the image so the rest of the output
    // keeps receiving clicks. It must be set on the QWindow, not the widget:
    // QWidget::setMask also tells Qt to stop repainting outside the mask,
    // which on Wayland (input region only, pixels still composited) leaves
    // stale pixels behind as ghosting when the image moves.
    if (QWindow *window = windowHandle()) {
        window->setMask(QRegion(nextRect));
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
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(paintedRect_, source_);
    // A thin outline keeps the pinned image distinguishable from identical
    // content behind it; a focused pin (Space = edit) gets a bright one.
    painter.setPen(hasFocus_ ? QPen(QColor(255, 255, 255, 200), 2.0)
                             : QPen(QColor(0, 0, 0, 120), 1.0));
    painter.drawRect(QRectF(paintedRect_.x() + 0.5, paintedRect_.y() + 0.5,
                            paintedRect_.width() - 1.0, paintedRect_.height() - 1.0));
    if (!zoomLabel_.isEmpty()) {
        // Transient zoom badge pinned to the image's bottom-right corner.
        // The font size is fixed: the badge reports the factor, it must not
        // grow with the image itself.
        QFont font = painter.font();
        font.setPixelSize(16);
        font.setBold(true);
        painter.setFont(font);
        const QFontMetrics metrics(font);
        const QString text = QStringLiteral("%1%").arg(zoomLabel_);
        const QRect textRect = metrics.boundingRect(text);
        const int pad = metrics.height() / 3;
        QRect badge = textRect.adjusted(-pad, -pad / 2, pad, pad / 2);
        badge.moveBottomRight(paintedRect_.bottomRight() - QPoint(pad, pad));
        // A pin may hang partially off-screen; keep the badge readable.
        badge = badge.intersected(rect().adjusted(0, 0, -1, -1));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 160));
        painter.drawRoundedRect(badge, 6, 6);
        painter.setPen(Qt::white);
        painter.drawText(badge, Qt::AlignCenter, text);
    }
}

void PinWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        dragging_ = true;
        pressGlobal_ = event->globalPosition().toPoint();
        startMargin_ = margin_;
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PinWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (dragging_) {
        // Global deltas are frame-independent, so they map straight onto the
        // output-local image offset.
        const QPoint delta = event->globalPosition().toPoint() - pressGlobal_;
        margin_ = clampMargin(startMargin_ + delta);
        applyGeometry();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void PinWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
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
    // keep the point under the cursor stationary while zooming.
    const double factor = steps.y() > 0 ? 1.1 : 1.0 / 1.1;
    const double next = std::clamp(scale_ * factor, kMinScale, kMaxScale);
    if (next != scale_) {
        const QPoint cursor = event->position().toPoint();
        const double ratio = next / scale_;
        const QPoint anchor = cursor - margin_;
        margin_ += anchor - QPoint(qRound(anchor.x() * ratio), qRound(anchor.y() * ratio));
        scale_ = next;
        margin_ = clampMargin(margin_);
        applyGeometry();
    }
    // Show the resulting factor even when clamped at the limits, so the
    // wheel always gives feedback. The factor is relative to the image's
    // native density, so a HiDPI card at natural size reads as 100%.
    zoomLabel_ = QString::number(qRound(scale_ * imageRatio_ * 100));
    zoomTimer_->start(kZoomBadgeMs);
    update(paintedRect_);
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
        close();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace vshot

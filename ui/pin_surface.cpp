#include "pin_surface.hpp"

#include <LayerShellQt/Window>

#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRegion>
#include <QScreen>
#include <QSet>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>

namespace vshot {

namespace {

// The focused outline's 2px pen is centered on the rect edge and paints up
// to ~2px outside the image rect; every repaint region must include that
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
// intersects it with the surface and nothing is ever hit. Without this, a
// surface whose pins all sit on another output would eat every click on this
// screen.
const QRegion &clickThroughInputRegion()
{
    static const QRegion region(QRect(-8, -8, 1, 1));
    return region;
}

// The on-screen size of an image at a given zoom.
QSize paintedSize(const QImage &image, double scale)
{
    return QSize(std::max(1, qRound(image.width() * scale)),
                 std::max(1, qRound(image.height() * scale)));
}

} // namespace

PinSurface::PinSurface(QScreen *screen)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint)
    , screen_(screen)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setCursor(Qt::SizeAllCursor);
    zoomTimer_ = new QTimer(this);
    zoomTimer_->setSingleShot(true);
    connect(zoomTimer_, &QTimer::timeout, this, [this] {
        const QRect gone = badgeRect_;
        badgeId_ = 0;
        zoomLabel_.clear();
        badgeRect_ = QRect();
        if (!gone.isNull()) {
            update(gone.adjusted(-1, -1, 1, 1));
        }
    });
    if (screen_ != nullptr) {
        // One surface per output, covering the whole output; the images are
        // painted at an offset inside it. The compositor confirms this via the
        // layer configure, this only avoids a wrong-size first frame.
        resize(screen_->geometry().size());
    }
}

bool PinSurface::showLayerSurface()
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
    // whole output, so moving an image never involves the compositor.
    LayerShellQt::Window::Anchors anchors(LayerShellQt::Window::AnchorTop);
    anchors |= LayerShellQt::Window::AnchorBottom;
    anchors |= LayerShellQt::Window::AnchorLeft;
    anchors |= LayerShellQt::Window::AnchorRight;
    layer->setExclusiveZone(-1);
    // On-demand keyboard focus: this output only takes the keyboard after one
    // of its pins is clicked, and releases it as soon as the user clicks
    // elsewhere. This is what makes the Space edit shortcut work without a
    // global grab.
    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
    layer->setScope(QStringLiteral("vshot-pin"));
    layer->setDesiredSize(QSize(0, 0)); // follow the anchored edges
    layer->setScreen(screen_);
    surfaceReady_ = true;
    applyMask();
    show();
    applyMask(); // re-issue now that the platform window exists
    return true;
}

void PinSurface::setPinnedVisible(bool visible)
{
    visible_ = visible;
    setVisible(visible);
}

QRect PinSurface::localRect(const Item &item) const
{
    const QPoint origin = screen_ != nullptr ? screen_->geometry().topLeft() : QPoint(0, 0);
    return QRect(item.origin - origin, paintedSize(item.image, item.scale));
}

const PinSurface::Entry *PinSurface::entryFor(quint64 id) const
{
    for (const Entry &entry : entries_) {
        if (entry.item.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

void PinSurface::setPins(const QVector<Item> &pins)
{
    // Whatever stops being painted has to be repainted away, so both the rect
    // an image leaves and the one it arrives at are collected here.
    QHash<quint64, Entry> previous;
    previous.reserve(entries_.size());
    for (const Entry &entry : entries_) {
        previous.insert(entry.item.id, entry);
    }

    QRect dirty;
    QSet<quint64> ids;
    QVector<Entry> next;
    next.reserve(pins.size());
    for (const Item &item : pins) {
        Entry entry;
        entry.item = item;
        ids.insert(item.id);
        const QRect after = localRect(item);
        if (const auto found = previous.constFind(item.id); found != previous.constEnd()) {
            const Entry &before = *found;
            // Keep the scaled copy: setPins runs on every motion event of a
            // drag, and re-scaling a 4K image per frame is what would make it
            // stutter.
            entry.render = before.render;
            entry.renderKey = before.renderKey;
            entry.renderTarget = before.renderTarget;
            const QRect was = localRect(before.item);
            if (was != after) {
                dirty |= expandOutline(was);
            }
            // The pixels can be replaced without the rect moving at all (pin
            // edit writes an annotated image back in place), so the rect alone
            // does not say whether there is anything to repaint.
            if (before.item.image.cacheKey() != item.image.cacheKey() || was != after) {
                dirty |= expandOutline(after);
            }
        } else {
            dirty |= expandOutline(after);
        }
        next.append(entry);
    }
    for (const Entry &entry : entries_) {
        if (!ids.contains(entry.item.id)) {
            dirty |= expandOutline(localRect(entry.item));
        }
    }
    // A reorder changes what covers what without moving or recoloring anything,
    // so it has to be looked for explicitly: bringing a pin to the front only
    // shows up where it overlaps the pins that were in front of it.
    bool reordered = entries_.size() != next.size();
    for (qsizetype index = 0; !reordered && index < entries_.size(); ++index) {
        reordered = entries_.at(index).item.id != next.at(index).item.id;
    }
    if (reordered) {
        for (const Entry &entry : next) {
            dirty |= expandOutline(localRect(entry.item));
        }
    }

    entries_ = next;
    // A pin that is gone can be neither picked, dragged nor announced.
    if (!ids.contains(pickedId_)) {
        pickedId_ = 0;
    }
    if (!ids.contains(draggingId_)) {
        draggingId_ = 0;
    }
    if (!ids.contains(badgeId_)) {
        badgeId_ = 0;
        zoomLabel_.clear();
        badgeRect_ = QRect();
    }
    applyMask();
    if (surfaceReady_ && !dirty.isNull()) {
        update(dirty);
    }
}

const QImage &PinSurface::renderSource(Entry &entry)
{
    if (entry.item.image.isNull()) {
        return entry.item.image;
    }
    const QSize target = deviceTargetSize(entry);
    if (target == entry.item.image.size()) {
        // Already one device pixel per device pixel: a plain 1:1 blit.
        return entry.item.image;
    }
    if (!entry.render.isNull() && entry.renderKey == entry.item.image.cacheKey() &&
        entry.renderTarget == target) {
        return entry.render;
    }
    entry.render = entry.item.image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (entry.render.isNull()) {
        // Out of memory: fall back to letting the painter resample.
        return entry.item.image;
    }
    entry.renderKey = entry.item.image.cacheKey();
    entry.renderTarget = target;
    return entry.render;
}

QSize PinSurface::deviceTargetSize(const Entry &entry) const
{
    const qreal ratio = std::max<qreal>(1.0, devicePixelRatioF());
    const QSize size = paintedSize(entry.item.image, entry.item.scale);
    return QSize(std::max(1, qRound(size.width() * ratio)),
                 std::max(1, qRound(size.height() * ratio)));
}

quint64 PinSurface::pinAt(const QPoint &local) const
{
    // Front to back: the last entry of the stack is painted last, so it is the
    // one the user sees on top where they overlap.
    for (auto entry = entries_.crbegin(); entry != entries_.crend(); ++entry) {
        if (localRect(entry->item).contains(local)) {
            return entry->item.id;
        }
    }
    return 0;
}

void PinSurface::applyMask()
{
    // The input mask always follows the images so the rest of the output keeps
    // receiving clicks. It must be set on the QWindow, not the widget:
    // QWidget::setMask also tells Qt to stop repainting outside the mask, which
    // on Wayland (input region only, pixels still composited) leaves stale
    // pixels behind as ghosting when an image moves.
    QWindow *window = windowHandle();
    if (window == nullptr) {
        return;
    }
    const QRect bounds(QPoint(0, 0), size());
    QRegion mask;
    for (const Entry &entry : entries_) {
        const QRect rect = localRect(entry.item).intersected(bounds);
        if (!rect.isEmpty()) {
            mask += rect;
        }
    }
    if (mask.isEmpty()) {
        if (draggingId_ != 0) {
            // A drag that carries the last image onto another output empties
            // this surface's region while this is still the surface holding the
            // pointer grab. Widen it for the rest of the gesture: an empty
            // input region is exactly the state that can drop the grab, and
            // while a button is held no other client can receive input anyway.
            // The release recomputes the region.
            mask = QRegion(rect());
        } else {
            mask = clickThroughInputRegion();
        }
    }
    window->setMask(mask);
}

QRect PinSurface::pickedOutline() const
{
    if (const Entry *entry = entryFor(pickedId_)) {
        return expandOutline(localRect(entry->item));
    }
    return QRect();
}

void PinSurface::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    // The painter is clipped to the dirty region requested by update(), which
    // always includes the area an image just left. Force-clear that area to
    // transparent first: relying on Qt's implicit background clear leaves stale
    // pixels behind as ghosting during drags.
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // Back to front, so the images later in the stack land on top of the ones
    // before them.
    for (Entry &entry : entries_) {
        const QRect target = localRect(entry.item);
        if (!target.intersects(rect())) {
            // Every visible pixel of this image lives on another output.
            continue;
        }
        // renderSource() is already at this surface's device resolution when
        // the source is denser or coarser than the output, so this is a 1:1
        // blit then.
        painter.drawImage(target, renderSource(entry));
        // A thin outline keeps a pinned image distinguishable from identical
        // content behind it; the pin the user picked gets a bright one while
        // this output holds the keyboard.
        const bool focused = hasFocus_ && entry.item.id == pickedId_;
        painter.setPen(focused ? QPen(QColor(255, 255, 255, 200), 2.0)
                              : QPen(QColor(0, 0, 0, 120), 1.0));
        painter.drawRect(QRectF(target.x() + 0.5, target.y() + 0.5,
                                target.width() - 1.0, target.height() - 1.0));
    }
    badgeRect_ = QRect();
    if (zoomLabel_.isEmpty()) {
        return;
    }
    const Entry *badge = entryFor(badgeId_);
    if (badge == nullptr) {
        return;
    }
    // Transient zoom badge pinned to the image's bottom-right corner that is
    // still on this output. The font size is fixed: the badge reports the
    // factor, it must not grow with the image itself.
    QFont font = painter.font();
    font.setPixelSize(16);
    font.setBold(true);
    painter.setFont(font);
    const QFontMetrics metrics(font);
    const QString text = QStringLiteral("%1%").arg(zoomLabel_);
    const QRect textRect = metrics.boundingRect(text);
    const int pad = metrics.height() / 3;
    QRect badgeBox = textRect.adjusted(-pad, -pad / 2, pad, pad / 2);
    const QRect corner = localRect(badge->item).intersected(rect());
    badgeBox.moveBottomRight(corner.bottomRight() - QPoint(pad, pad));
    // A pin may hang partially off-screen; keep the badge readable.
    badgeBox = badgeBox.intersected(rect().adjusted(0, 0, -1, -1));
    if (badgeBox.width() <= 0 || badgeBox.height() <= 0) {
        return;
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 160));
    painter.drawRoundedRect(badgeBox, 6, 6);
    painter.setPen(Qt::white);
    painter.drawText(badgeBox, Qt::AlignCenter, text);
    badgeRect_ = badgeBox;
}

void PinSurface::showZoomBadge(quint64 id)
{
    const Entry *entry = entryFor(id);
    if (entry == nullptr) {
        return;
    }
    // The factor is relative to the image's native density, so a 4K capture
    // pinned at its natural size on a 4K output reads as 100%.
    zoomLabel_ = QString::number(qRound(entry->item.scale * entry->item.density * 100));
    badgeId_ = id;
    zoomTimer_->start(kZoomBadgeMs);
    update(localRect(entry->item));
}

void PinSurface::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const quint64 id = pinAt(event->position().toPoint());
    if (id == 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QRect wasPicked = pickedOutline();
    pickedId_ = id;
    draggingId_ = id;
    setFocus(Qt::MouseFocusReason);
    pressGlobal_ = event->globalPosition().toPoint();
    // Read the origin before telling the daemon: bringing the pin to the front
    // hands this surface a fresh stack, so the entry the value came from is
    // gone by the time the callback returns.
    if (const Entry *entry = entryFor(id)) {
        pressOrigin_ = entry->item.origin;
    }
    if (picked_) {
        picked_(id);
    }
    // The bright outline moved to the pin that was just picked; the repaint the
    // reorder caused covers the new one, this covers the old one.
    const QRect nowPicked = pickedOutline();
    if (!wasPicked.isNull() && wasPicked != nowPicked) {
        update(wasPicked);
    }
    event->accept();
}

void PinSurface::mouseMoveEvent(QMouseEvent *event)
{
    if (draggingId_ != 0) {
        // Global deltas are frame-independent, so they map straight onto the
        // shared global position. The daemon applies the move to every surface
        // of this pin, so dragging across outputs keeps working: the pointer
        // stays grabbed by the surface the drag started on.
        if (dragMoved_) {
            dragMoved_(draggingId_,
                       pressOrigin_ + event->globalPosition().toPoint() - pressGlobal_);
        }
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void PinSurface::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && draggingId_ != 0) {
        draggingId_ = 0;
        // The drag widened the input region past the images; put it back now
        // that the pointer is free again (full-surface repaint clears it).
        applyMask();
        update();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void PinSurface::wheelEvent(QWheelEvent *event)
{
    const QPoint steps = event->angleDelta();
    if (steps.y() == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    const quint64 id = pinAt(event->position().toPoint());
    if (id == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    // 10% per notch, multiplicative so zooming feels even at any scale. The
    // daemon keeps the image center fixed and rescales every surface at once,
    // so the badge below reports the factor it has just applied.
    if (zoomRequested_) {
        zoomRequested_(id, steps.y() > 0 ? 1.1 : 1.0 / 1.1);
    }
    showZoomBadge(id);
    event->accept();
}

void PinSurface::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        event->accept();
        const quint64 id = pickedId_;
        if (id != 0 && editRequested_) {
            // Starting an edit brings the pin to the front, which repaints this
            // stack and can clear this callback while it runs; invoke a copy.
            const std::function<void(quint64)> edit = editRequested_;
            edit(id);
        }
        return;
    }
    QWidget::keyPressEvent(event);
}

void PinSurface::focusInEvent(QFocusEvent *event)
{
    hasFocus_ = true;
    // The outline style depends on focus; repaint it explicitly so the change
    // never waits for an unrelated repaint to piggyback on.
    const QRect outline = pickedOutline();
    if (!outline.isNull()) {
        update(outline);
    }
    QWidget::focusInEvent(event);
}

void PinSurface::focusOutEvent(QFocusEvent *event)
{
    hasFocus_ = false;
    const QRect outline = pickedOutline();
    if (!outline.isNull()) {
        update(outline);
    }
    QWidget::focusOutEvent(event);
}

void PinSurface::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const quint64 id = pinAt(event->position().toPoint());
        if (id != 0) {
            event->accept();
            if (closeRequested_) {
                // Closing can take the surfaces down (the last pin ends the
                // daemon's stack), which clears this callback while it runs;
                // invoke a copy.
                const std::function<void(quint64)> close = closeRequested_;
                close(id);
            }
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace vshot
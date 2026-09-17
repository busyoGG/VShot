#include "pin_surface.hpp"

#include "i18n.hpp"

#include <LayerShellQt/Window>

#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QRegion>
#include <QScreen>
#include <QSet>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>

namespace vshot {

namespace {

// Whether the surfaces trace their focus life. Which of the outline's two
// colours a pin gets is decided by events only the compositor can produce --
// the keyboard being handed to this surface and taken away again -- and none
// of that is visible from the outside. With `VSHOT_PIN_FOCUS_DEBUG=1` in the
// daemon's environment, "the compositor never told us" and "we never repainted"
// stop looking the same.
bool focusTrace()
{
    static const bool on = qEnvironmentVariableIsSet("VSHOT_PIN_FOCUS_DEBUG");
    return on;
}

// The outline's stroke is centered on the image edge and reaches 1px outside
// it; every repaint region must include that bleed (plus a pixel of slack),
// or stale border pixels survive at the previous position.
constexpr int kOutlineBleedPx = 3;

// The pin outline, in both of its states: a solid, fully opaque stroke of one
// colour. Idle pins are light grey — enough to tell an image apart from a
// background of its own colour, quiet enough to ignore — and the pin the
// keyboard would act on is black, which is unmistakable on light content
// without needing the second, contrasting ring the two-tone edge used to
// carry. Both states share the width below, so a focus change is a pure
// recolour: the border never shifts or changes weight under the pointer.
constexpr double kOutlineWidthPx = 2.0;
const QColor kIdleOutlineColor(192, 192, 192);
const QColor kActiveOutlineColor(0, 0, 0);

// The right-click menu of a color card, in logical pixels. The rows reuse the
// card's own two fonts (label and value), so the menu reads as the same card
// with one more thing to click.
constexpr int kMenuRowPaddingY = 5;
constexpr int kMenuPaddingX = 10;
constexpr qreal kMenuLabelGap = 14.0;  // label column to the value column
constexpr qreal kMenuRadius = 6.0;
constexpr int kMenuHeadingPaddingY = 4;
constexpr int kMenuCursorGap = 4;  // menu offset from the pointer

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
        badgeText_.clear();
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
    layer_ = layer;
    keyboardWanted_ = true;
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
        badgeText_.clear();
        badgeRect_ = QRect();
    }
    // A menu is about a pin; the pin going away takes the menu with it.
    if (!ids.contains(menuId_)) {
        closeMenu();
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
    // The right-click menu is not part of any pin, but it has to take clicks
    // like one: without this the compositor would hand its clicks to whatever
    // is behind, and the menu could never be used.
    if (menuId_ != 0 && !menuRect_.isEmpty()) {
        const QRect rect = menuRect_.intersected(bounds);
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

// One line per event that can change the outline's colour, for
// `VSHOT_PIN_FOCUS_DEBUG`. `what` names the event; the state every line carries
// is what the next paint will read.
void PinSurface::traceFocus(const QString &what) const
{
    const QWindow *window = windowHandle();
    qWarning("pin focus: %s: %s (picked %llu, keyboard %s, window active %s)",
             qPrintable(screen_ != nullptr ? screen_->name() : QStringLiteral("-")), qPrintable(what),
             static_cast<unsigned long long>(pickedId_), hasFocus_ ? "yes" : "no",
             window != nullptr && window->isActive() ? "yes" : "no");
}

QRect PinSurface::pickedOutline() const
{
    if (const Entry *entry = entryFor(pickedId_)) {
        return expandOutline(localRect(entry->item));
    }
    return QRect();
}

bool PinSurface::event(QEvent *event)
{
    // The window's activation is a separate fact from the widget's focus, and
    // it is the one a layer surface loses when the compositor takes the
    // keyboard away, so both are traced.
    if (focusTrace()) {
        if (event->type() == QEvent::WindowActivate) {
            traceFocus(QStringLiteral("window activate"));
        } else if (event->type() == QEvent::WindowDeactivate) {
            traceFocus(QStringLiteral("window deactivate"));
        }
    }
    return QWidget::event(event);
}

// Which pin a key press would act on is the one under the pointer: that is what
// the black edge claims, and the pointer is the only signal about it that every
// compositor sends. Clicking a pin still hands this surface the keyboard (and
// raises the pin); the pointer decides which edge is black.
void PinSurface::movePickTo(quint64 id)
{
    if (id == pickedId_) {
        return;
    }
    const QRect was = pickedOutline();
    pickedId_ = id;
    const QRect now = pickedOutline();
    if (focusTrace()) {
        traceFocus(QStringLiteral("picked %1 (pointer)").arg(id));
    }
    if (!was.isNull()) {
        update(was);
    }
    if (!now.isNull() && now != was) {
        update(now);
    }
}

void PinSurface::offerKeyboardBack()
{
    if (layer_ == nullptr || !keyboardWanted_) {
        return;
    }
    keyboardWanted_ = false;
    layer_->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
    // The interactivity change only reaches the compositor with the next
    // commit; requestUpdate schedules exactly that, so the keyboard leaves
    // now rather than at some later repaint.
    if (QWindow *window = windowHandle()) {
        window->requestUpdate();
    }
    if (focusTrace()) {
        traceFocus(QStringLiteral("keyboard offered back (interactivity none)"));
    }
}

void PinSurface::wantKeyboard()
{
    if (layer_ == nullptr || keyboardWanted_) {
        return;
    }
    keyboardWanted_ = true;
    layer_->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityOnDemand);
    if (QWindow *window = windowHandle()) {
        window->requestUpdate();
    }
    if (focusTrace()) {
        traceFocus(QStringLiteral("keyboard wanted again (interactivity on-demand)"));
    }
}

void PinSurface::enterEvent(QEnterEvent *event)
{
    if (focusTrace()) {
        traceFocus(QStringLiteral("pointer entered"));
    }
    // Back from the empty desktop: the surface has to want the keyboard again,
    // or the click that follows could not take it (the offerKeyboardBack()
    // commit below switched it off while the pointer was away).
    wantKeyboard();
    // A menu and a drag own the surface while they are open: the pointer moving
    // inside them must not change what they act on.
    if (menuId_ == 0 && draggingId_ == 0) {
        movePickTo(pinAt(event->position().toPoint()));
    }
    QWidget::enterEvent(event);
}

void PinSurface::leaveEvent(QEvent *event)
{
    if (focusTrace()) {
        traceFocus(QStringLiteral("pointer left"));
    }
    if (menuId_ == 0 && draggingId_ == 0) {
        // The pointer left every pin on this output, so no pin is the one a key
        // press would act on any more. Riding on the pointer rather than on the
        // keyboard is deliberate: a compositor is not obliged to tell a layer
        // surface that it has stopped being focused, and the ones in the field
        // that stay quiet left the edge black long after the user had clicked a
        // window -- while the pointer had already gone. This one always comes.
        movePickTo(0);
        // The same signal is also the only reliable moment to hand the
        // keyboard back. Waiting for the compositor to move it on the click
        // into a window does not work: the click focuses the window's own
        // surface but the compositors in use leave the layer surface holding
        // the keyboard, so typing went nowhere until another pin was clicked.
        // Offering it back here means the click that follows lands on a
        // surface that no longer wants it, and the window gets it.
        offerKeyboardBack();
    }
    QWidget::leaveEvent(event);
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
        // content behind it, and its colour says whether the pin is the one a
        // key press would act on: black while this output holds the keyboard
        // and that pin was the last one clicked, light grey otherwise.
        const bool focused = hasFocus_ && entry.item.id == pickedId_;
        // Whole-pixel rect edges with an even pen width put both stroke lines
        // on whole device pixels, so the edge stays crisp instead of fading
        // over two rows. Centred that way the stroke sits one pixel outside
        // the image on each side — which also keeps it from degenerating on a
        // one-pixel pin, where an inward stroke would have no room at all. A
        // pin flush against the edge of its output simply loses that outer
        // pixel row, the same as it did before.
        const QRectF edge(target.x(), target.y(), target.width(), target.height());
        painter.setPen(QPen(focused ? kActiveOutlineColor : kIdleOutlineColor, kOutlineWidthPx));
        painter.drawRect(edge);
    }
    // Above every pin: the menu belongs to one of them but must never end up
    // under another.
    if (menuId_ != 0) {
        paintMenu(painter);
    }
    badgeRect_ = QRect();
    if (badgeText_.isEmpty()) {
        return;
    }
    const Entry *badge = entryFor(badgeId_);
    if (badge == nullptr) {
        return;
    }
    // Transient badge pinned to the image's bottom-right corner that is still
    // on this output. The font size is fixed: the badge reports what just
    // happened to the pin, it must not grow with the image itself.
    QFont font = painter.font();
    font.setPixelSize(16);
    font.setBold(true);
    painter.setFont(font);
    const QFontMetrics metrics(font);
    const QString text = badgeText_;
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
    showBadge(id, QStringLiteral("%1%").arg(qRound(entry->item.scale * entry->item.density * 100)));
}

void PinSurface::showBadge(quint64 id, const QString &text)
{
    const Entry *entry = entryFor(id);
    if (entry == nullptr) {
        return;
    }
    badgeText_ = text;
    badgeId_ = id;
    zoomTimer_->start(kBadgeMs);
    // The badge is painted inside the pin's rect, so repainting that rect is
    // what puts it up and what takes the previous one down.
    update(localRect(entry->item));
}

void PinSurface::openMenu(quint64 id, const QPoint &anchor)
{
    const Entry *entry = entryFor(id);
    if (entry == nullptr || entry->item.colorRows.isEmpty()) {
        return;
    }
    const QRect was = menuRect_;
    menuId_ = id;
    menuRows_ = entry->item.colorRows;
    menuHover_ = -1;
    menuRect_ = menuRectFor(anchor);
    // The keyboard is this surface's only while it has focus, and Esc is part
    // of using a menu: ask for it here rather than requiring a click first.
    setFocus(Qt::MouseFocusReason);
    applyMask();
    // Repaint both the old menu area (there may be none) and the new one; the
    // pins under either are covered by these two rects alone.
    if (!was.isNull() && was != menuRect_) {
        update(was.adjusted(-1, -1, 1, 1));
    }
    update(menuRect_.adjusted(-1, -1, 1, 1));
}

void PinSurface::closeMenu()
{
    if (menuId_ == 0) {
        return;
    }
    const QRect gone = menuRect_;
    menuId_ = 0;
    menuRows_.clear();
    menuHover_ = -1;
    menuRect_ = QRect();
    applyMask();
    if (!gone.isNull()) {
        update(gone.adjusted(-1, -1, 1, 1));
    }
}

QRect PinSurface::menuRectFor(const QPoint &anchor) const
{
    const QFontMetrics labelMetrics(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
    const QFontMetrics valueMetrics(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    const QFontMetrics headingMetrics(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
    qreal labelWidth = 0.0;
    qreal valueWidth = 0.0;
    for (const ColorRow &row : menuRows_) {
        labelWidth = std::max<qreal>(labelWidth, labelMetrics.horizontalAdvance(row.label));
        valueWidth = std::max<qreal>(valueWidth, valueMetrics.horizontalAdvance(row.value));
    }
    const int rowHeight = std::max(labelMetrics.height(), valueMetrics.height())
        + 2 * kMenuRowPaddingY;
    const int headingHeight = headingMetrics.height() + 2 * kMenuHeadingPaddingY;
    const qreal headingWidth = headingMetrics.horizontalAdvance(uiTr("Copy"));
    // Wide enough for the widest row and for the heading, whichever is wider.
    const qreal contentWidth =
        std::max(std::max(labelWidth + kMenuLabelGap + valueWidth, headingWidth), 1.0);
    const int width = qCeil(contentWidth) + 2 * kMenuPaddingX;
    const int height = headingHeight + menuRows_.size() * rowHeight;

    // Down and right of the pointer, the way a menu opens; flipped when that
    // would run off the output, and clamped so a pin at the very edge still
    // gets a usable menu (the pointer may sit anywhere inside it then).
    QRect box(anchor + QPoint(kMenuCursorGap, kMenuCursorGap), QSize(width, height));
    if (box.right() > rect().right()) {
        box.moveLeft(anchor.x() - kMenuCursorGap - width);
    }
    if (box.bottom() > rect().bottom()) {
        box.moveTop(anchor.y() - kMenuCursorGap - height);
    }
    box = box.intersected(rect());
    return box.width() > 0 && box.height() > 0 ? box : QRect();
}

int PinSurface::menuRowAt(const QPoint &local) const
{
    if (menuId_ == 0 || menuRect_.isEmpty()) {
        return -1;
    }
    const QFontMetrics labelMetrics(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
    const QFontMetrics valueMetrics(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    const QFontMetrics headingMetrics(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
    const int rowHeight = std::max(labelMetrics.height(), valueMetrics.height())
        + 2 * kMenuRowPaddingY;
    const int headingHeight = headingMetrics.height() + 2 * kMenuHeadingPaddingY;
    const int offset = local.y() - menuRect_.top() - headingHeight;
    if (offset < 0) {
        return -1;
    }
    const int row = offset / rowHeight;
    return row >= 0 && row < menuRows_.size() ? row : -1;
}

void PinSurface::paintMenu(QPainter &painter)
{
    if (menuId_ == 0 || menuRect_.isEmpty()) {
        return;
    }
    const QPalette palette = this->palette();
    const QFont labelFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    const QFont valueFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const QFontMetrics labelMetrics(labelFont);
    const QFontMetrics valueMetrics(valueFont);
    const QFontMetrics headingMetrics(labelFont);
    const int rowHeight = std::max(labelMetrics.height(), valueMetrics.height())
        + 2 * kMenuRowPaddingY;
    const int headingHeight = headingMetrics.height() + 2 * kMenuHeadingPaddingY;

    qreal labelWidth = 0.0;
    for (const ColorRow &row : menuRows_) {
        labelWidth = std::max<qreal>(labelWidth, labelMetrics.horizontalAdvance(row.label));
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0, 0, 0, 90), 1.0));
    painter.setBrush(palette.color(QPalette::Base));
    painter.drawRoundedRect(QRectF(menuRect_).adjusted(0.5, 0.5, -0.5, -0.5), kMenuRadius,
                            kMenuRadius);

    // The heading says what clicking a row does; the rows themselves stay the
    // card's own label-and-value pairs, so the value that gets copied is the
    // one the card prints.
    QColor dim = palette.color(QPalette::Text);
    dim.setAlpha(160);
    painter.setFont(labelFont);
    painter.setPen(dim);
    painter.drawText(QRect(menuRect_.left() + kMenuPaddingX, menuRect_.top() + kMenuHeadingPaddingY,
                           menuRect_.width() - 2 * kMenuPaddingX, headingMetrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, uiTr("Copy"));
    painter.setPen(QPen(QColor(0, 0, 0, 40), 1.0));
    const int separator = menuRect_.top() + headingHeight;
    painter.drawLine(menuRect_.left() + 1, separator, menuRect_.right() - 1, separator);

    const qreal labelX = menuRect_.left() + kMenuPaddingX;
    const qreal valueX = labelX + labelWidth + kMenuLabelGap;
    for (int index = 0; index < menuRows_.size(); ++index) {
        const QRect row(menuRect_.left(), separator + index * rowHeight, menuRect_.width(),
                        rowHeight);
        const bool hovered = index == menuHover_;
        if (hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(palette.color(QPalette::Highlight));
            painter.drawRect(row);
        }
        const QColor labelColor = hovered ? palette.color(QPalette::HighlightedText) : dim;
        const QColor valueColor = hovered ? palette.color(QPalette::HighlightedText)
                                         : palette.color(QPalette::Text);
        const QRect textBox(row.left(), row.top(), row.width(), row.height());
        painter.setFont(labelFont);
        painter.setPen(labelColor);
        painter.drawText(QRectF(labelX, textBox.top(), labelWidth, textBox.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, menuRows_.at(index).label);
        painter.setFont(valueFont);
        painter.setPen(valueColor);
        painter.drawText(QRectF(valueX, textBox.top(),
                                menuRect_.right() - kMenuPaddingX - valueX + 1.0,
                                textBox.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, menuRows_.at(index).value);
    }
    painter.restore();
}

void PinSurface::copyRow(int row)
{
    if (row < 0 || row >= menuRows_.size()) {
        return;
    }
    const ColorRow chosen = menuRows_.at(row);
    const quint64 id = menuId_;
    bool copied = false;
    if (copyRequested_) {
        // The callback runs the clipboard write; it can also take the whole
        // stack down (nothing in it does today), so nothing after it may touch
        // the menu state without re-reading it.
        const std::function<bool(quint64, const QString &)> copy = copyRequested_;
        copied = copy(id, chosen.value);
    }
    showBadge(id, copied ? uiTr("Copied") + QLatin1Char(' ') + chosen.label
                         : uiTr("Copy failed"));
}

void PinSurface::mousePressEvent(QMouseEvent *event)
{
    const QPoint local = event->position().toPoint();
    // Any press that the menu does not keep for itself may start a gesture on
    // the pins, and a gesture may legitimately begin with a double-click.
    swallowNextDoubleClick_ = false;
    // An open menu owns the next click, whichever button it is: a row copies
    // its format, anything else merely dismisses the menu -- a click that
    // closes a menu must not also start acting on the pins underneath.
    if (menuId_ != 0) {
        if (event->button() == Qt::LeftButton && menuRowAt(local) >= 0) {
            const int row = menuRowAt(local);
            // Order matters: the badge lands on the card, and closing the menu
            // must not repaint over it.
            copyRow(row);
            closeMenu();
            // This press was the menu's, and it may be the first half of a
            // double-click: the menu is gone by the time the second half
            // arrives, and that one would otherwise reach the pin the menu was
            // covering.
            swallowNextDoubleClick_ = true;
            event->accept();
            return;
        }
        closeMenu();
        swallowNextDoubleClick_ = true;
        event->accept();
        return;
    }
    // The right button is the card's: on a pinned color it opens the list of
    // formats, each of which the menu can put back on the clipboard. The
    // right button never picks or drags, so this cannot be confused with a
    // left-click gesture.
    if (event->button() == Qt::RightButton) {
        const quint64 id = pinAt(local);
        const Entry *entry = entryFor(id);
        if (entry != nullptr && !entry->item.colorRows.isEmpty()) {
            openMenu(id, local);
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const quint64 id = pinAt(local);
    if (id == 0) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QRect wasPicked = pickedOutline();
    pickedId_ = id;
    draggingId_ = id;
    // In case the pointer entered and clicked within one commit's time, the
    // wantKeyboard() from enterEvent may not have reached the compositor yet;
    // ask again so a second click is not needed to take the keyboard.
    wantKeyboard();
    setFocus(Qt::MouseFocusReason);
    if (focusTrace()) {
        // `setFocus` above may already have logged "keyboard in"; this line is
        // the pick itself, which is what the black outline follows.
        traceFocus(QStringLiteral("picked %1").arg(id));
    }
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
    if (menuId_ != 0) {
        // Hovering a row is the whole interaction model the menu needs: the
        // highlighted row is the one a click or Enter would copy.
        const int row = menuRowAt(event->position().toPoint());
        if (row != menuHover_) {
            menuHover_ = row;
            update(menuRect_.adjusted(-1, -1, 1, 1));
        }
        event->accept();
        return;
    }
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
    if (menuId_ != 0) {
        // Zooming the card out from under its own menu would leave the menu
        // pointing at nothing; the wheel waits until the menu is closed.
        event->accept();
        return;
    }
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
    // While the menu is up it takes the three keys a menu is expected to take;
    // everything else falls through, so this surface's other shortcuts are not
    // shadowed by it.
    if (menuId_ != 0 && !menuRows_.isEmpty()) {
        const int count = menuRows_.size();
        switch (event->key()) {
        case Qt::Key_Escape:
            event->accept();
            closeMenu();
            return;
        case Qt::Key_Down:
        case Qt::Key_Up: {
            const int step = event->key() == Qt::Key_Down ? 1 : -1;
            // With nothing highlighted yet, Down enters the list at the top
            // and Up at the bottom.
            const int from = menuHover_ < 0 ? (step > 0 ? -1 : 0) : menuHover_;
            menuHover_ = (from + step + count) % count;
            update(menuRect_.adjusted(-1, -1, 1, 1));
            event->accept();
            return;
        }
        case Qt::Key_Return:
        case Qt::Key_Enter:
            event->accept();
            if (menuHover_ >= 0) {
                copyRow(menuHover_);
            }
            closeMenu();
            return;
        default:
            break;
        }
    }
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
    if (focusTrace()) {
        traceFocus(QStringLiteral("keyboard in"));
    }
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
    if (focusTrace()) {
        traceFocus(QStringLiteral("keyboard out"));
    }
    const QRect outline = pickedOutline();
    if (!outline.isNull()) {
        update(outline);
    }
    QWidget::focusOutEvent(event);
}

void PinSurface::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (swallowNextDoubleClick_) {
        // The press before this one went to the menu, so this is the tail of a
        // double-click on a menu row -- not a double-click on a pin.
        swallowNextDoubleClick_ = false;
        event->accept();
        return;
    }
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
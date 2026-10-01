// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#include "pin_surface.hpp"

#include "i18n.hpp"

#include <LayerShellQt/Window>

#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
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
// or stale border pixels survive at the previous position.  Wider strokes and
// the shadow reach further, so this is the floor the per-surface margin is
// computed from rather than the value itself -- see `bleed`.
constexpr int kMinBleedPx = 3;

// The shadow a pin casts is described by `ShadowStyle` (in `ui/shadow.hpp`),
// which the config layer fills in: it is on by default while the corners are
// square, because a screenshot pinned over a window of its own colour is
// otherwise impossible to place -- there is no edge to see.

// The pin's rim: the values themselves live in `Style` (in the header) and are
// resolved by the config layer, so a surface is handed a complete style and
// never invents one of its own.  Both states share one width, so a focus change
// is a pure recolour -- the border never shifts or changes weight under the
// pointer.

// The right-click menu of a pin, in logical pixels. The copy rows reuse the
// card's own two fonts (label and value), so the menu reads as the same card
// with one more thing to click.
constexpr int kMenuRowPaddingY = 5;
constexpr int kMenuPaddingX = 10;
constexpr qreal kMenuLabelGap = 14.0;  // label column to the value column
constexpr qreal kMenuRadius = 6.0;
constexpr int kMenuHeadingPaddingY = 4;
constexpr int kMenuCursorGap = 4;  // menu offset from the pointer

// The action rows every pin's menu ends with, in paint order, after the
// color-card rows: `Copy image` first and `Close` last, with the row that
// throws the pin away at the bottom of a list where every other row is
// reversible. Naming the count lets the layout, the painting and the
// hit-testing agree while a row is added instead of each hard-coding "exactly
// one action row".
enum ActionRow {
    kCopyImageAction = 0,
    kSaveAction = 1,
    kEditAction = 2,
    kResetZoomAction = 3,
    kRecognizeAction = 4,
    kCloseAction = 5,
    kActionRowCount = 6,
};

// The label of action row `index`, in the order the rows are painted.
QString actionRowLabel(int index)
{
    switch (index) {
    case kCopyImageAction:
        return uiTr("Copy image");
    case kSaveAction:
        return uiTr("Save as…");
    case kEditAction:
        return uiTr("Edit");
    case kResetZoomAction:
        return uiTr("Reset zoom");
    case kRecognizeAction:
        return uiTr("Recognize text…");
    case kCloseAction:
        return uiTr("Close");
    default:
        return QString();
    }
}

QRect expandOutline(const QRect &rect, int bleed)
{
    return rect.adjusted(-bleed, -bleed, bleed, bleed);
}

// The shadow under one pin lives in `ui/shadow.cpp`; the falloff it is built
// from is shared with the file dialog's frame, which casts one too.


// The label both the transient badge and the HDR marker wear: a short tag in a
// translucent box, large enough to read over any image and no larger.  The font
// is a constant rather than the pin's: a tag says what the pin *is*, and it has
// to stay legible on a pin zoomed down to a thumbnail.
constexpr int kLabelPixelSize = 16;
constexpr qreal kLabelRadius = 6.0;
const QColor kLabelBox(0, 0, 0, 160);

// The HDR tag's two inks.  A pinned HDR capture and the SDR half the daemon
// keeps beside it are the same picture to the eye -- what differs is light no
// photograph of a screen reproduces -- so the tag is the only thing that says
// which of the two this output is showing: white while the pixels are the
// helper's HDR ones, muted grey while this surface is showing the SDR copy
// itself.  Either way the pin *is* an HDR capture, which is why the tag is up
// at all.
const QColor kHdrTagShown(255, 255, 255);
const QColor kHdrTagFallback(192, 192, 192);

/// The corner radius a pin can actually carry: never past half the shorter side
/// of the painted image, where a corner would stop being a corner and start
/// being a lozenge.  The same rule the dialog's rim follows, against the
/// image's own size -- which changes with every zoom step, so this is asked at
/// paint time rather than stored.
int paintRadius(std::uint32_t radius, const QSize &size)
{
    const int most = std::max(0, std::min(size.width(), size.height()) / 2);
    return std::min(static_cast<int>(radius), most);
}

// The box a label needs: the text's own bounds grown by the same padding on
// every side, so every tag of the same font comes out the same height whatever
// it says.
QRect labelBox(const QFontMetrics &metrics, const QString &text)
{
    const int pad = metrics.height() / 3;
    return metrics.boundingRect(text).adjusted(-pad, -pad / 2, pad, pad / 2);
}

// The font every corner tag is drawn with -- the `HDR` marker and the badge
// that reports a zoom or a copy.  Fixed, so a tag does not grow with the image
// it is drawn over.
QFont tagFont()
{
    QFont font;
    font.setPixelSize(kLabelPixelSize);
    font.setBold(true);
    return font;
}

// The marker's text: the pin under the pointer is one whose light comes from a
// shape of its own.
const QString kHdrTag = QStringLiteral("HDR");

// Where a tag of `text` lands when it is anchored at `corner`: just inside the
// pin's top-left for the marker, just inside its bottom-right for the badge.
// One definition, because the painter and the repaint region both ask it -- on
// a pin too small to hold the tag, the tag reaches past the pin, and a repaint
// region computed without it leaves the tag's outer pixels at the old
// position every time the pin is zoomed or dragged.
QRect tagBox(const QString &text, const QPoint &corner, bool atBottomRight, const QRect &bounds)
{
    const QFontMetrics metrics(tagFont());
    const int pad = metrics.height() / 3;
    QRect box = labelBox(metrics, text);
    if (atBottomRight) {
        box.moveBottomRight(corner - QPoint(pad, pad));
    } else {
        box.moveTopLeft(corner + QPoint(pad, pad));
    }
    return box.intersected(bounds.adjusted(0, 0, -1, -1));
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

void PinSurface::setStyle(const Style &style)
{
    style_ = style;
    // Every pin is drawn differently now, and a wider stroke or a shadow
    // reaches further than the last one did -- so the whole surface is
    // repainted rather than the pins' own rects, which would leave the old
    // paint's outer edge behind.
    if (surfaceReady_) {
        update();
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

void PinSurface::setHdrPixels(bool on)
{
    if (hdrPixels_ == on) {
        return;
    }
    hdrPixels_ = on;
    // Every item marked `hdr` is painted by the other side when this turns on,
    // and by this surface when it turns off, so the whole output has to be
    // repainted either way.
    if (surfaceReady_) {
        update();
    }
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
            // The shadow is carried over for the same reason and with the same
            // care: a drag changes only the origin, so the built image is still
            // valid -- and rebuilding it per motion event would cost more than
            // the scaling this line sits beside.
            entry.shadow = before.shadow;
            entry.shadowKey = before.shadowKey;
            const QRect was = localRect(before.item);
            if (was != after) {
                dirty |= dirtyRect(was);
            }
            // The pixels can be replaced without the rect moving at all (pin
            // edit writes an annotated image back in place), and a pin can also
            // change hands between this surface and the HDR helper without
            // moving or changing its pixels, so the rect alone does not say
            // whether there is anything to repaint.
            if (before.item.image.cacheKey() != item.image.cacheKey()
                || before.item.hdr != item.hdr || was != after) {
                dirty |= dirtyRect(after);
            }
        } else {
            dirty |= dirtyRect(after);
        }
        next.append(entry);
    }
    for (const Entry &entry : entries_) {
        if (!ids.contains(entry.item.id)) {
            dirty |= dirtyRect(localRect(entry.item));
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
            dirty |= dirtyRect(localRect(entry.item));
        }
    }
    // The corner tags are drawn above every pin and are anchored to one pin's
    // own corner, so on a pin smaller than a tag the tag reaches past the pin --
    // outside the rect the loops above cover.  Both the stack on its way out and
    // the one arriving are asked, because a zoom or a drag carries the tag with
    // its pin; a region computed from the pins alone would leave the tag's outer
    // pixels at the old position.
    for (const Entry &entry : entries_) {
        dirty |= tagBoxes(entry.item.id, localRect(entry.item));
    }
    for (const Entry &entry : next) {
        dirty |= tagBoxes(entry.item.id, localRect(entry.item));
    }

    entries_ = next;
    // A pin that is gone can be neither picked, dragged nor announced.
    if (!ids.contains(pickedId_)) {
        pickedId_ = 0;
    }
    if (!ids.contains(hoverId_)) {
        hoverId_ = 0;
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
    repaintRegion_ = dirty;
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
        return dirtyRect(localRect(entry->item));
    }
    return QRect();
}

// How far a pin's paint reaches past its own rect: the stroke is centred on the
// edge, and the shadow's blur reaches a spread in every direction.  Every
// repaint region is grown by this, or stale pixels survive at the previous
// position -- which for a shadow means a dark smear trailing a drag.
int PinSurface::bleed() const
{
    const int stroke = static_cast<int>(style_.borderWidth) / 2 + 1;
    return std::max(kMinBleedPx, std::max(stroke, style_.shadow.band()));
}

QRect PinSurface::dirtyRect(const QRect &pin) const
{
    return expandOutline(pin, bleed());
}

QRect PinSurface::tagBoxes(quint64 id, const QRect &target) const
{
    const QRect visible = target.intersected(rect());
    if (visible.isEmpty()) {
        return QRect();
    }
    QRect boxes;
    if (id == hoverId_) {
        boxes |= tagBox(kHdrTag, visible.topLeft(), false, rect());
    }
    if (id == badgeId_ && !badgeText_.isEmpty()) {
        boxes |= tagBox(badgeText_, visible.bottomRight(), true, rect());
    }
    return boxes;
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
    // The compositor's own frame callback, which Qt turns into an
    // `UpdateRequest` on the window once the buffer it was armed for has been
    // presented.  That is the only moment a client can know something it drew
    // is on the screen rather than merely queued, and it is what the daemon
    // waits on before it lets a handoff finish.  Installed once, on the first
    // event with a window behind it.
    if (event->type() == QEvent::Show && !frameWatch_) {
        if (QWindow *window = windowHandle()) {
            frameWatch_ = true;
            window->installEventFilter(this);
        }
    }
    return QWidget::event(event);
}

bool PinSurface::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::UpdateRequest && painted_) {
        painted_();
    }
    return QWidget::eventFilter(watched, event);
}

// Asks Qt for the compositor's next frame callback, so `setPaintedCallback`
// fires once the frame being composed now has been presented.
//
// Called after the stack has been handed over: the repaint that carries the
// change is committed either way, and this arms the callback that says the
// compositor took it.  A surface with nothing to draw may never be given one,
// which is why the caller waits with a deadline rather than for ever.
void PinSurface::requestPainted()
{
    if (QWindow *window = windowHandle()) {
        window->requestUpdate();
    }
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
    reportActive();
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

// Tells the daemon which pin's rim is the live one.  The black edge is a pin
// this surface holds the keyboard for and the pointer is over; the HDR half of
// that pin is drawn by another process, so the answer has to travel.
void PinSurface::reportActive()
{
    const quint64 active = hasFocus_ ? pickedId_ : 0;
    if (active == activeId_) {
        return;
    }
    activeId_ = active;
    if (activeReported_) {
        activeReported_(active);
    }
}

void PinSurface::trackHover(const QPoint &local)
{
    // Only an HDR capture carries the marker: over any other pin the pointer is
    // simply over a pin, and the surface has nothing to say about it.  It is the
    // capture that is marked, not the picture being shown -- the tag is what says
    // which of the two an HDR pin is currently drawn from.
    const quint64 under = pinAt(local);
    const Entry *entry = entryFor(under);
    moveHoverTo(entry != nullptr && entry->item.capturedHdr ? under : 0);
}

void PinSurface::moveHoverTo(quint64 id)
{
    if (id == hoverId_) {
        return;
    }
    const QRect was = hoverMarker_;
    hoverId_ = id;
    // Arriving repaints the pin the tag lands on, which is what puts it up --
    // it is anchored to that pin's own corner, so its box is not known until it
    // is painted.  Leaving repaints the box it was last painted in, because the
    // pin it was on may be gone by now.
    if (!was.isNull()) {
        update(was.adjusted(-1, -1, 1, 1));
    }
    if (const Entry *entry = entryFor(id)) {
        // Only the tag is about to appear: the tag belongs to the pin, and the
        // pin itself has already been painted.  The box can reach past a small
        // pin, which the pin's own rect would not have covered.
        update(tagBoxes(id, localRect(entry->item)));
    }
}

void PinSurface::paintHdrMarker(QPainter &painter, const QRect &target, bool shownAsHdr)
{
    QFont font = tagFont();
    // Anchored inside the part of the pin this output actually shows: a pin
    // hanging off the edge of the screen keeps its tag in the frame instead of
    // pushing it out with the corner it is anchored to.
    const QRect visible = target.intersected(rect());
    if (visible.isEmpty()) {
        return;
    }
    const QRect box = tagBox(kHdrTag, visible.topLeft(), false, rect());
    if (box.width() <= 0 || box.height() <= 0) {
        return;
    }
    const QColor ink = shownAsHdr ? kHdrTagShown : kHdrTagFallback;
    painter.save();
    painter.setFont(font);
    // Smoothing on, unlike the rim: the tag is the one thing here with corners
    // of its own, and a rounded box drawn without it reads as a staircase.
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(kLabelBox);
    painter.drawRoundedRect(box, kLabelRadius, kLabelRadius);
    // The stroke is what makes it a label rather than a smudge on a dark image:
    // the box alone is only a translucency, and a translucent tag over a dark
    // picture reads as part of the picture.
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ink, 1));
    painter.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), kLabelRadius,
                            kLabelRadius);
    painter.setPen(ink);
    painter.drawText(box, Qt::AlignCenter, kHdrTag);
    painter.restore();
    hoverMarker_ = box;
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
        const QPoint local = event->position().toPoint();
        movePickTo(pinAt(local));
        trackHover(local);
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
        // press would act on any more -- and the HDR marker, which is about the
        // pin under the pointer, has nothing left to be about.  Riding on the
        // pointer rather than on the keyboard is deliberate: a compositor is
        // not obliged to tell a layer surface that it has stopped being
        // focused, and the ones in the field that stay quiet left the edge
        // black long after the user had clicked a window -- while the pointer
        // had already gone. This one always comes.
        movePickTo(0);
        moveHoverTo(0);
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

void PinSurface::paintShadow(QPainter &painter, Entry &entry, const QRect &target, int radius)
{
    const qreal ratio = std::max<qreal>(1.0, devicePixelRatioF());
    // The key covers everything the built image depends on, so a drag (which
    // changes only the origin) re-uses it while a zoom step or a config change
    // rebuilds it.  The offset is part of that: it is baked into the silhouette
    // the blur is built from, so two offsets are two different images -- leaving
    // it out of the key meant a pin kept the shadow of the offset it was first
    // drawn with, and the file dialog's own key has carried it all along.
    const QString key = QStringLiteral("%1x%2/%3/%4/%5/%6/%7")
                            .arg(target.width())
                            .arg(target.height())
                            .arg(radius)
                            .arg(style_.shadow.size)
                            .arg(style_.shadow.offset)
                            .arg(style_.shadow.opacity)
                            .arg(qRound(ratio * 100));
    if (entry.shadow.isNull() || entry.shadowKey != key) {
        entry.shadow = renderShadow(target.size(), radius, style_.shadow, ratio).image;
        entry.shadowKey = key;
    }
    if (entry.shadow.isNull()) {
        return;
    }
    // Drawn at the pin's own top-left less the room the blur needs, so the
    // silhouette inside the image lands exactly on the pin -- with the offset
    // already baked into the silhouette, which is what makes the shadow hang
    // below the image.  The image carries its device ratio, which is what maps
    // its device pixels onto that logical rect on a scaled output.
    QImage shadow = entry.shadow;
    shadow.setDevicePixelRatio(ratio);
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target.topLeft() - QPoint(style_.shadow.size, style_.shadow.size), shadow);
    painter.restore();
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
        const bool focused = hasFocus_ && entry.item.id == pickedId_;
        // The corners, clamped to what this pin's size can carry: a radius past
        // half the shorter side turns the image into a lozenge.
        const int radius = paintRadius(style_.radius, target.size());
        // A pin the helper draws has its picture, its shadow and its rim all on
        // the helper's surface, which sits below this one: only a surface
        // carrying the output's *own* colour description is passed through
        // untouched, and that is a surface this Qt window can never be.  The
        // three travel together -- one surface, one commit -- or the picture
        // would trail its own edge the moment the pin was dragged.  What is
        // left here is the chrome: the badges, the menus and the `HDR` tag.
        const bool hdr = entry.item.hdr && hdrPixels_;
        if (hdr) {
            continue;
        }
        // The shadow goes down first and only under this pin -- the pins behind
        // it have already been painted, and a shadow drawn over them would read
        // as a smudge rather than as depth.  A shadow of no size, or one turned
        // off, paints nothing at all.
        if (style_.shadow.enabled && style_.shadow.size > 0 && style_.shadow.opacity > 0) {
            paintShadow(painter, entry, target, radius);
        }
        // renderSource() is already at this surface's device resolution when
        // the source is denser or coarser than the output, so this is a 1:1
        // blit then.
        if (radius > 0) {
            // Clipping to the rounded silhouette is what actually rounds the
            // image: drawImage has no radius of its own, and painting a
            // rounded rect of the right colour over the corners would show
            // through wherever the image is translucent.
            QPainterPath clip;
            clip.addRoundedRect(QRectF(target), radius, radius);
            painter.save();
            painter.setClipPath(clip, Qt::IntersectClip);
            painter.drawImage(target, renderSource(entry));
            painter.restore();
        } else {
            painter.drawImage(target, renderSource(entry));
        }
        if (style_.borderWidth == 0) {
            // No rim wanted: the pin is the image and nothing else.
            continue;
        }
        // A thin outline keeps a pinned image distinguishable from identical
        // content behind it, and its colour says whether the pin is the one a
        // key press would act on.  A stroke wider than one pixel is centred on
        // the edge, which also keeps it from degenerating on a one-pixel pin,
        // where an inward stroke would have no room at all.  A pin flush
        // against the edge of its output simply loses the outer half.
        const QRectF edge(target.x(), target.y(), target.width(), target.height());
        const QColor color =
            focused ? style_.activeBorderColor : style_.borderColor;
        // Antialiasing off for a square pin with a thin stroke: on, Qt would
        // fade the stroke over two rows at every edge and the pin would read as
        // blurry rather than as outlined.
        const bool rounded = radius > 0;
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, rounded);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(color, style_.borderWidth));
        if (rounded) {
            // Centred on the edge, exactly as the square case is: the outer
            // half of the stroke is meant to be seen, which is what keeps a
            // rounded pin the same size as a square one of the same image.
            painter.drawRoundedRect(edge, radius, radius);
        } else {
            painter.drawRect(edge);
        }
        painter.restore();
    }
    // Above every pin, because each of these belongs to one pin but must never
    // end up under another.  For the marker that is the whole reason it is not
    // painted with the pin it belongs to: the pin under the pointer can have a
    // pin in front of it covering the corner the tag is anchored to.
    hoverMarker_ = QRect();
    if (hoverId_ != 0) {
        if (const Entry *entry = entryFor(hoverId_)) {
            paintHdrMarker(painter, localRect(entry->item), entry->item.hdr && hdrPixels_);
        }
    }
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
    QFont font = tagFont();
    painter.setFont(font);
    const QString text = badgeText_;
    const QRect corner = localRect(badge->item).intersected(rect());
    const QRect badgeBox = tagBox(text, corner.bottomRight(), true, rect());
    if (badgeBox.width() <= 0 || badgeBox.height() <= 0) {
        return;
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(kLabelBox);
    painter.drawRoundedRect(badgeBox, kLabelRadius, kLabelRadius);
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
    // The badge is painted inside the pin's rect and just outside its corner on
    // a pin too small to hold it, so both are asked for.
    const QRect target = localRect(entry->item);
    update(dirtyRect(target) | tagBoxes(id, target));
}

void PinSurface::showMessage(quint64 id, const QString &text)
{
    // The same badge a copy confirmation uses: from the user's side a save and
    // a copy report through the same corner of the same image.
    showBadge(id, text);
}

void PinSurface::openMenu(quint64 id, const QPoint &anchor)
{
    const Entry *entry = entryFor(id);
    if (entry == nullptr) {
        return;
    }
    const QRect was = menuRect_;
    menuId_ = id;
    menuRows_ = entry->item.colorRows;
    menuHover_ = -1;
    menuRect_ = menuRectFor(anchor);
    if (menuRect_.isEmpty()) {
        // Nothing fits on this output: leaving menuId_ set would put the
        // surface into a state no click could leave.
        closeMenu();
        return;
    }
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

PinSurface::MenuLayout PinSurface::menuLayout() const
{
    const QFontMetrics labelMetrics(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
    const QFontMetrics valueMetrics(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    const QFontMetrics headingMetrics(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
    MenuLayout layout;
    layout.rowHeight =
        std::max(labelMetrics.height(), valueMetrics.height()) + 2 * kMenuRowPaddingY;
    layout.copyRows = menuRows_.size();
    layout.totalRows = layout.copyRows + kActionRowCount;
    // The heading belongs to the copy rows; a menu with none of them is
    // action rows only and needs no heading over them.
    layout.headingHeight =
        layout.copyRows > 0 ? headingMetrics.height() + 2 * kMenuHeadingPaddingY : 0;
    layout.rowsTop = layout.headingHeight;
    layout.actionTop = layout.rowsTop + layout.copyRows * layout.rowHeight;

    qreal valueWidth = 0.0;
    for (const ColorRow &row : menuRows_) {
        layout.labelWidth = std::max<qreal>(layout.labelWidth,
                                            labelMetrics.horizontalAdvance(row.label));
        valueWidth = std::max<qreal>(valueWidth, valueMetrics.horizontalAdvance(row.value));
    }
    const qreal headingWidth = headingMetrics.horizontalAdvance(uiTr("Copy"));
    qreal actionWidth = 0.0;
    for (int index = 0; index < kActionRowCount; ++index) {
        actionWidth = std::max<qreal>(actionWidth,
                                      labelMetrics.horizontalAdvance(actionRowLabel(index)));
    }
    // Wide enough for the widest copy row and for either single-line row
    // (heading, action), whichever is widest.
    const qreal contentWidth =
        std::max(std::max(layout.labelWidth + kMenuLabelGap + valueWidth, headingWidth),
                 actionWidth);
    layout.boxSize = QSize(qCeil(contentWidth) + 2 * kMenuPaddingX,
                           layout.headingHeight + layout.totalRows * layout.rowHeight);
    return layout;
}

QRect PinSurface::menuRectFor(const QPoint &anchor) const
{
    const MenuLayout layout = menuLayout();
    // Down and right of the pointer, the way a menu opens; flipped when that
    // would run off the output, and clamped so a pin at the very edge still
    // gets a usable menu (the pointer may sit anywhere inside it then).
    QRect box(anchor + QPoint(kMenuCursorGap, kMenuCursorGap), layout.boxSize);
    if (box.right() > rect().right()) {
        box.moveLeft(anchor.x() - kMenuCursorGap - layout.boxSize.width());
    }
    if (box.bottom() > rect().bottom()) {
        box.moveTop(anchor.y() - kMenuCursorGap - layout.boxSize.height());
    }
    box = box.intersected(rect());
    return box.width() > 0 && box.height() > 0 ? box : QRect();
}

int PinSurface::menuRowAt(const QPoint &local) const
{
    if (menuId_ == 0 || menuRect_.isEmpty()) {
        return -1;
    }
    // Both axes: the input region this surface holds also carries the pins, so a
    // pointer far to the side of the menu still reaches here -- and a row that
    // only looked at the vertical offset lit up under it.
    if (local.x() < menuRect_.left() || local.x() > menuRect_.right()) {
        return -1;
    }
    const MenuLayout layout = menuLayout();
    if (layout.rowHeight <= 0) {
        return -1;
    }
    const int offset = local.y() - menuRect_.top() - layout.rowsTop;
    if (offset < 0) {
        return -1;
    }
    const int row = offset / layout.rowHeight;
    return row >= 0 && row < layout.totalRows ? row : -1;
}

void PinSurface::paintMenu(QPainter &painter)
{
    if (menuId_ == 0 || menuRect_.isEmpty()) {
        return;
    }
    const MenuLayout layout = menuLayout();
    const QPalette palette = this->palette();
    const QFont labelFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    const QFont valueFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const QFontMetrics headingMetrics(labelFont);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0, 0, 0, 90), 1.0));
    painter.setBrush(palette.color(QPalette::Base));
    painter.drawRoundedRect(QRectF(menuRect_).adjusted(0.5, 0.5, -0.5, -0.5), kMenuRadius,
                            kMenuRadius);

    QColor dim = palette.color(QPalette::Text);
    dim.setAlpha(160);
    if (layout.headingHeight > 0) {
        // The heading says what clicking a row does; the rows themselves stay
        // the card's own label-and-value pairs, so the value that gets copied
        // is the one the card prints.
        painter.setFont(labelFont);
        painter.setPen(dim);
        painter.drawText(QRect(menuRect_.left() + kMenuPaddingX,
                               menuRect_.top() + kMenuHeadingPaddingY,
                               menuRect_.width() - 2 * kMenuPaddingX,
                               headingMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, uiTr("Copy"));
        painter.setPen(QPen(QColor(0, 0, 0, 40), 1.0));
        const int separator = menuRect_.top() + layout.headingHeight;
        painter.drawLine(menuRect_.left() + 1, separator, menuRect_.right() - 1, separator);
    }

    const qreal labelX = menuRect_.left() + kMenuPaddingX;
    const qreal valueX = labelX + layout.labelWidth + kMenuLabelGap;
    // A row is a full-width rectangle, so the first and last of them would square
    // off the rounded box they sit in -- and the highlight, which is painted over
    // the fill, would show its corners outside it.  Clip the rows to the box's
    // own outline, the one the fill above was drawn with.
    QPainterPath clip;
    clip.addRoundedRect(QRectF(menuRect_).adjusted(0.5, 0.5, -0.5, -0.5), kMenuRadius, kMenuRadius);
    painter.setClipPath(clip, Qt::IntersectClip);
    for (int index = 0; index < layout.copyRows; ++index) {
        const QRect row(menuRect_.left(), menuRect_.top() + layout.rowsTop + index * layout.rowHeight,
                        menuRect_.width(), layout.rowHeight);
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
        painter.drawText(QRectF(labelX, textBox.top(), layout.labelWidth, textBox.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, menuRows_.at(index).label);
        painter.setFont(valueFont);
        painter.setPen(valueColor);
        painter.drawText(QRectF(valueX, textBox.top(),
                                menuRect_.right() - kMenuPaddingX - valueX + 1.0,
                                textBox.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, menuRows_.at(index).value);
    }

    // The action rows: no label/value pair, and they sit under a separator so
    // they do not read as formats to copy. The separator is drawn once, above
    // the first of them.
    for (int index = 0; index < kActionRowCount; ++index) {
        const QRect action(menuRect_.left(),
                           menuRect_.top() + layout.actionTop + index * layout.rowHeight,
                           menuRect_.width(), layout.rowHeight);
        // A separator above the first of the action rows, so they do not read
        // as formats to copy, and a second above `Close`: the one row that
        // throws the pin away sits apart from the rows that only do something
        // to it. The first is drawn only when there are copy rows above it --
        // at the very top of the box it would be a stray line.
        if ((index == 0 && layout.copyRows > 0) || index == kCloseAction) {
            painter.setPen(QPen(QColor(0, 0, 0, 40), 1.0));
            painter.drawLine(menuRect_.left() + 1, action.top(), menuRect_.right() - 1,
                             action.top());
        }
        const bool hovered = menuHover_ == layout.copyRows + index;
        if (hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(palette.color(QPalette::Highlight));
            painter.drawRect(action);
        }
        painter.setFont(labelFont);
        painter.setPen(hovered ? palette.color(QPalette::HighlightedText)
                               : palette.color(QPalette::Text));
        painter.drawText(action.adjusted(kMenuPaddingX, 0, -kMenuPaddingX, 0),
                         Qt::AlignLeft | Qt::AlignVCenter, actionRowLabel(index));
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

void PinSurface::activateRow(int row)
{
    const int copyRows = menuRows_.size();
    if (row < 0) {
        return;
    }
    if (row < copyRows) {
        copyRow(row);
        return;
    }
    const int action = row - copyRows;
    const quint64 id = menuId_;
    if (action == kCopyImageAction && copyImageRequested_) {
        const std::function<bool(quint64)> copyImage = copyImageRequested_;
        // The daemon owns the pixels and the clipboard; its answer decides
        // whether the badge says the copy landed, exactly as a color row's
        // does.
        const bool copied = copyImage(id);
        showBadge(id, copied ? uiTr("Copied image") : uiTr("Copy failed"));
    } else if (action == kSaveAction && saveRequested_) {
        const std::function<void(quint64)> save = saveRequested_;
        // The daemon runs the dialog and writes the file; the badge that says
        // how it went arrives over showMessage() once that is known.
        save(id);
    } else if (action == kEditAction && editRequested_) {
        const std::function<void(quint64)> edit = editRequested_;
        // The daemon spawns the editor, which opens on the pin's own marks.
        edit(id);
    } else if (action == kResetZoomAction && resetZoomRequested_) {
        const std::function<void(quint64)> resetZoom = resetZoomRequested_;
        // The daemon rescales the pin; the badge it shows is the zoom one, so
        // the new factor is reported the way the wheel reports its own.
        resetZoom(id);
        showZoomBadge(id);
    } else if (action == kRecognizeAction && recognizeRequested_) {
        const std::function<void(quint64)> recognize = recognizeRequested_;
        // The daemon spawns the editor, which opens on the pin's text; unlike a
        // save, nothing about it comes back through this surface.
        recognize(id);
    } else if (action == kCloseAction && closeRequested_) {
        const std::function<void(quint64)> close = closeRequested_;
        // Closing takes the pin out of this surface's stack, which clears the
        // callback while it runs; invoke a copy.
        close(id);
    }
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
            activateRow(row);
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
    // formats, each of which the menu can put back on the clipboard, plus the
    // one action every pin has. The right button never picks or drags, so this
    // cannot be confused with a left-click gesture.
    if (event->button() == Qt::RightButton) {
        const quint64 id = pinAt(local);
        const Entry *entry = entryFor(id);
        if (entry != nullptr) {
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
    // The pointer inside the surface and over a pin: this is the only place the
    // marker can follow it from one pin to the next, because the input region
    // is the whole stack and crossing it produces no enter or leave.
    trackHover(event->position().toPoint());
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
    if (menuId_ != 0) {
        const int count = menuRows_.size() + kActionRowCount;
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
                activateRow(menuHover_);
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
    reportActive();
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
    reportActive();
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
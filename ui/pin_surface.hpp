// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#pragma once

#include "color_card.hpp"
#include "shadow.hpp"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

#include <cstdint>

class QScreen;

namespace LayerShellQt {
class Window;
}

namespace vshot {

// One rendering surface of the pin stack, covering one whole output.
//
// A layer-shell surface belongs to exactly one output, so the whole stack of
// pinned images is painted by one of these per output, each drawing the part of
// every pin that overlaps it. Keeping the stack in a single surface is what
// makes the stacking order ours: a compositor holds the surfaces of a layer in
// map order and the protocol has no request to restack them, so with one
// surface per pin, bringing a pin to the front would mean remapping it.
//
// The surface is deliberately NOT sized to the images: layer margins are the
// only way to position a layer surface, and every margin change costs a full
// asynchronous configure round trip, which makes dragging trail the cursor.
// Keeping the surface fixed and repainting the images at an offset makes drags
// pure client-side repaints; the input mask follows the images' rects so the
// rest of the output stays interactive.
class PinSurface final : public QWidget {
public:
    // One pinned image as this surface paints it, in global logical pixels.
    // The daemon owns the real state; this is the copy the surface needs.
    struct Item {
        quint64 id = 0;
        QImage image;
        // Device pixels per logical pixel of `image`. The zoom badge reports
        // its factor relative to this density, so a 4K capture pinned at its
        // natural size on a 4K output reads as 100%.
        int density = 1;
        // Logical pixels per source pixel, so the painted size is the source
        // size times this. The daemon owns it; 1 / density is the natural size.
        double scale = 1.0;
        // Global logical top-left of the image.
        QPoint origin;
        // Whether this pin's picture, its shadow and its rim are drawn by the
        // HDR surface helper rather than by this surface.  The image is still
        // held -- it is what the size, the mask and every edit are worked out
        // from -- but it is not painted, so the helper's copy shows through.
        //
        // True for *every* pin once the helper has this output, not only for
        // HDR ones: the helper is a single surface sitting below this one, and a
        // layer's surfaces are stacked in map order with no way to restack them,
        // so anything this surface painted would land above every pin the helper
        // draws -- including pins that are in front of it.
        bool hdr = false;
        // Whether this pin is an HDR capture, whatever is showing it.  The
        // helper's pixels and this pin's own image look the same to the eye, so
        // the `HDR` tag is the only thing on screen that tells them apart, and
        // it belongs to the capture rather than to the surface that happens to
        // be drawing it: it stays up, in a muted ink, on an output the helper
        // could not take.
        bool capturedHdr = false;
        // The formats a pinned color card shows, empty for every other pin.
        // A right-click on the card turns them into a copy menu, and the menu
        // hands back the very string the card prints for that format.
        QVector<ColorRow> colorRows;
    };

    explicit PinSurface(QScreen *screen);

    // How a pin is drawn: its corners, the shadow behind it and the stroke
    // around it.  Every value here is already resolved -- the config layer
    // decides what an absent setting means -- because a surface has no business
    // reading a preferences file, and two surfaces on two outputs have to agree
    // on what they are drawing.
    //
    // The radius is a wish rather than a promise: a corner wider than half the
    // image would turn it into a lozenge, so it is clamped against the size the
    // pin actually has, which changes with every zoom step.
    struct Style {
        std::uint32_t radius = 0;
        /// The shadow behind every pin.  A whole `ShadowStyle` rather than a
        /// flag, because its size, its offset and its darkness are the user's
        /// to tune; `enabled` is the master switch.
        ShadowStyle shadow;
        std::uint32_t borderWidth = 2;
        /// Stroke of a pin the keyboard would not act on.
        QColor borderColor{192, 192, 192};
        /// Stroke of the pin the keyboard would act on.
        QColor activeBorderColor{0, 0, 0};
    };

    /// Replaces the look of every pin this surface paints.  Called before the
    /// first pin arrives and again when the daemon reloads its config.
    void setStyle(const Style &style);

    // Maps the widget onto its layer-shell surface. Returns false when
    // LayerShellQt is unavailable.
    bool showLayerSurface();

    // Whether the surface helper draws this output's pins: true when it has the
    // output's own colour description on its copy of it.  An item marked `hdr`
    // is only left unpainted when this is on -- on any other output this
    // surface's own image is the only copy there is.
    void setHdrPixels(bool on);

    // Replaces the whole stack, back to front: the last entry is painted last,
    // i.e. it is the frontmost. Entries keep their scaled copy across calls
    // when their pixels and on-screen size are unchanged, so dragging a pin
    // (one origin change per motion event) never re-scales an image.
    void setPins(const QVector<Item> &pins);

    // Bulk visibility, driven by the server's show/hide/toggle commands.
    void setPinnedVisible(bool visible);
    bool isPinnedVisible() const { return visible_; }

    // The output this surface is mapped onto.
    QScreen *screen() const { return screen_; }

    // Input goes to the whole stack, so every callback names the pin it is
    // about and the daemon looks that pin up by id.
    //
    // `picked` fires when the left button goes down on a pin, before the drag
    // that may follow it: the daemon brings that pin to the front, which for it
    // is a reorder and a repaint, not a remap.  The surface's own keyboard
    // focus and picked pin are set by then.
    void setPickCallback(std::function<void(quint64)> callback)
    {
        picked_ = std::move(callback);
    }
    // Fires with the pin whose rim should show as the live one, or 0 when none
    // should.  Only the surface knows both halves of that -- whether it still
    // holds the keyboard and which pin the pointer last picked -- and the HDR
    // half of a pin is drawn by another process, so it has to be told.
    void setActiveCallback(std::function<void(quint64)> callback)
    {
        activeReported_ = std::move(callback);
    }
    // Fires once per frame this surface actually puts on the screen, after the
    // compositor has taken the buffer.  This is the only moment a client can
    // know that something it drew is visible rather than merely queued, which
    // is what a handoff that must not show a gap has to wait for -- the pin
    // editor leaving while the pin it was drawing appears.  A no-op until the
    // surface has a window to watch.
    void setPaintedCallback(std::function<void()> callback)
    {
        painted_ = std::move(callback);
    }
    // Asks for the compositor's next frame callback, so `setPaintedCallback`
    // fires once the frame being composed now has been presented.
    void requestPainted();
    void setDragCallback(std::function<void(quint64, QPoint)> callback)
    {
        dragMoved_ = std::move(callback);
    }
    void setZoomCallback(std::function<void(quint64, double)> callback)
    {
        zoomRequested_ = std::move(callback);
    }
    // Invoked when the user double-clicks a pin, before it is closed.
    void setCloseCallback(std::function<void(quint64)> callback)
    {
        closeRequested_ = std::move(callback);
    }
    // Invoked when the user picks `Reset zoom` out of a pin's right-click menu:
    // the pin goes back to the size it arrived at, one image pixel per logical
    // pixel of its own density. The daemon owns the scale, so it is the one
    // that has to be told.
    void setResetZoomCallback(std::function<void(quint64)> callback)
    {
        resetZoomRequested_ = std::move(callback);
    }
    // Invoked when the user picks `Edit` or `Recognize text…` out of a pin's
    // right-click menu, and when the user presses Space while this surface has
    // keyboard focus. The editor runs in a helper process the daemon spawns, so
    // nothing about its outcome comes back through this surface.
    void setEditCallback(std::function<void(quint64)> callback)
    {
        editRequested_ = std::move(callback);
    }
    // Invoked when the user picks a format out of the right-click menu of a
    // pinned color card, with the exact text the card shows for that format.
    // `false` means the copy did not reach the clipboard, which the surface
    // reports in the badge instead of claiming success.
    void setCopyCallback(std::function<bool(quint64, const QString &)> callback)
    {
        copyRequested_ = std::move(callback);
    }
    // Invoked when the user picks `Copy image` out of a pin's right-click menu.
    // The pixels belong to the daemon, so it is the one that encodes them and
    // hands them to `wl-copy`; `false` means the copy did not reach the
    // clipboard, which the surface reports in the badge instead of claiming
    // success.
    void setCopyImageCallback(std::function<bool(quint64)> callback)
    {
        copyImageRequested_ = std::move(callback);
    }
    // Invoked when the user picks `Save as…` out of a pin's right-click menu.
    // The write itself belongs to the daemon: it owns the image, and the file
    // dialog it opens is a plain toplevel, which this layer surface cannot be.
    void setSaveCallback(std::function<void(quint64)> callback)
    {
        saveRequested_ = std::move(callback);
    }
    // Invoked when the user picks `Recognize text…` out of a pin's right-click
    // menu. The editor runs in a helper process the daemon spawns, so nothing
    // about its outcome comes back through this surface.
    void setRecognizeCallback(std::function<void(quint64)> callback)
    {
        recognizeRequested_ = std::move(callback);
    }
    // Puts a transient message on a pin's corner from outside the surface. A
    // save runs in a helper process, so its outcome is known long after the
    // menu that started it has closed.
    void showMessage(quint64 id, const QString &text);
    // The region the last setPins() asked Qt to repaint, in this surface's own
    // coordinates.  The offline check has no compositor to deliver the update,
    // so this is what it compares a paint it produces itself against: a ghost is
    // exactly a change outside the region the surface claims it repainted.
    QRect repaintRegion() const { return repaintRegion_; }

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    // How long a badge — the zoom factor after a wheel step, the confirmation
    // after a menu pick — stays visible.
    static constexpr int kBadgeMs = 900;

    // One stack entry: the daemon's item plus the scaled copy this surface keeps
    // of it, at its own device resolution.
    struct Entry {
        Item item;
        QImage render;
        qint64 renderKey = 0; // source cache key the copy was made from
        QSize renderTarget;   // device-pixel size the copy was made for
        // The shadow under this pin, at this surface's device resolution, and
        // the key it was built for.  A 4K pin's shadow is worth keeping across
        // the frames of a drag, and a zoom step is the only thing that changes
        // the key.
        QImage shadow;
        QString shadowKey;
    };

    // The image's rect in output-local logical pixels.
    QRect localRect(const Item &item) const;

    // How the open menu's rows are laid out, in logical pixels relative to the
    // menu's own top-left. Painting and hit-testing both read this, so a row
    // can never be drawn where it cannot be clicked.
    //
    // The rows are the copy rows a color card offers, followed by the action
    // rows every pin has: `Save as…` and `Recognize text…`. The heading exists
    // only when there are copy rows to head -- an image pin's menu is those
    // action rows alone.
    struct MenuLayout {
        int headingHeight = 0;
        int rowHeight = 0;
        // How many rows copy a format, and how many rows there are in total.
        int copyRows = 0;
        int totalRows = 0;
        // The y of the first copy row and of the first action row.
        int rowsTop = 0;
        int actionTop = 0;
        qreal labelWidth = 0.0;
        QSize boxSize;
    };
    MenuLayout menuLayout() const;
    // The image's rect in this surface's device pixels.
    QSize deviceTargetSize(const Entry &entry) const;
    // The image at this surface's device resolution, cached: painting the
    // source straight into the rect makes Qt resample it per frame with a 2x2
    // filter, which softens a 4K capture pinned on a 1080p output. Scaling
    // once here uses Qt's area filter instead.
    const QImage &renderSource(Entry &entry);
    // Paints the shadow under one pin, building it on first use and re-using it
    // while the pin's size and radius are unchanged.
    void paintShadow(QPainter &painter, Entry &entry, const QRect &target, int radius);
    // How far this surface's paint reaches past a pin's own rect, which is what
    // every repaint region has to be grown by.  Depends on the style, so it is
    // asked for rather than being a constant.
    int bleed() const;
    // A pin's rect grown by [`bleed`], i.e. the region that has to be repainted
    // when the pin moves, changes or goes away.
    QRect dirtyRect(const QRect &pin) const;
    // The region the corner tags reach for pin `id` drawn at `target`: the
    // `HDR` marker while the pointer is on it, and the badge while it is
    // reporting something.  Both are a fixed size anchored to a corner of the
    // pin, so on a pin smaller than the tag they reach past the pin's own
    // rect -- which [`dirtyRect`] does not cover.  Empty when the pin carries
    // neither tag.
    QRect tagBoxes(quint64 id, const QRect &target) const;
    // The id of the frontmost pin whose rect covers `local`, 0 for none.
    quint64 pinAt(const QPoint &local) const;
    // The entry of a pin, null when this output does not show it.
    const Entry *entryFor(quint64 id) const;
    // Where the picked pin's outline is, for the repaints focus changes need.
    QRect pickedOutline() const;
    // Points the input mask at the images' rects.
    void applyMask();
    // Moves which pin is picked, repainting the strokes that change colour.
    void movePickTo(quint64 id);
    // Tells the daemon which pin's rim is the live one, when that changed.
    void reportActive();
    // Moves the HDR marker to the pin the pointer is over, repainting the one
    // it leaves and the one it lands on.
    void moveHoverTo(quint64 id);
    // Follows the pointer inside this surface: the marker is about the pin
    // under it, so this is asked for whenever the pointer moves while no drag
    // or menu owns the surface.
    void trackHover(const QPoint &local);
    // Paints the `HDR` tag a pinned HDR image carries while the pointer is over
    // it, and remembers where, so taking it down repaints only its own box.
    void paintHdrMarker(QPainter &painter, const QRect &target, bool shownAsHdr);
    // Hands the keyboard back to the compositor: interactivity goes to None
    // and a commit makes the change reach the compositor at once. Needed
    // because the compositors in the field never tell a layer surface that a
    // click on a window took the keyboard away -- the click focuses the
    // window's own surface, but the layer surface keeps the keyboard until it
    // gives it up itself. The pointer leaving every pin is the moment to do
    // that: by the time the user clicks a window, the keyboard is already
    // back with the compositor, and the click moves it to that window.
    void offerKeyboardBack();
    // Asks for the keyboard again (OnDemand) once the pointer returns to a
    // pin, so the compositor grants it on the next click.
    void wantKeyboard();
    // One line per focus event, for `VSHOT_PIN_FOCUS_DEBUG`.
    void traceFocus(const QString &what) const;
    // Repaints the transient badge showing the current zoom factor.
    void showZoomBadge(quint64 id);
    // Puts `text` into the badge on `id`'s bottom-right corner and starts its
    // timer. Both badges (the zoom factor and a copy confirmation) share it.
    void showBadge(quint64 id, const QString &text);

    // The right-click menu of a pinned color card. It is painted into this
    // surface instead of being a QMenu: a menu is a popup window, a popup needs
    // an xdg_surface parent, and this surface is a layer-shell surface, which
    // cannot be one. So the box, the hover highlight and the keyboard handling
    // are all drawn and hit-tested here.
    void openMenu(quint64 id, const QPoint &anchor);
    void closeMenu();
    // Where the menu is painted, anchored at the right-click point and clamped
    // into the surface so a pin at the edge of its output keeps it usable.
    QRect menuRectFor(const QPoint &anchor) const;
    // The menu row under a point, -1 for none.
    int menuRowAt(const QPoint &local) const;
    // Paints the open menu; does nothing while it is closed.
    void paintMenu(QPainter &painter);
    // Copies one row's value through the daemon and reports it in the badge.
    void copyRow(int row);
    // Runs the row's action: the copy rows copy, the action rows save and
    // recognize.
    void activateRow(int row);

    QVector<Entry> entries_;
    Style style_;
    QScreen *screen_;
    std::function<void(quint64)> picked_;
    std::function<void(quint64)> activeReported_;
    // Called on every frame this surface swaps in; see `setPaintedCallback`.
    std::function<void()> painted_;
    // Whether the frame callback has been hooked up, so it is connected once.
    bool frameWatch_ = false;
    std::function<void(quint64, QPoint)> dragMoved_;
    std::function<void(quint64, double)> zoomRequested_;
    std::function<void(quint64)> closeRequested_;
    std::function<void(quint64)> editRequested_;
    std::function<void(quint64)> resetZoomRequested_;
    std::function<bool(quint64, const QString &)> copyRequested_;
    std::function<bool(quint64)> copyImageRequested_;
    std::function<void(quint64)> saveRequested_;
    std::function<void(quint64)> recognizeRequested_;

    // The pin the user last clicked on this output: the one the zoom badge and
    // the Space edit shortcut belong to, and the only one drawn as focused.
    quint64 pickedId_ = 0;
    // The pin this surface last told the daemon was the live one, so an
    // unchanged answer is not reported again.
    quint64 activeId_ = 0;
    // The pin being dragged, 0 while no left button is down.
    quint64 draggingId_ = 0;
    QPoint pressGlobal_;
    QPoint pressOrigin_;
    // The pin the badge reports on, and its text.
    quint64 badgeId_ = 0;
    QString badgeText_;
    // Where the badge was painted last, so clearing it does not repaint the
    // whole output.
    QRect badgeRect_;
    // The pin the pointer is over, when that pin is drawn by the helper: the
    // marker this surface paints belongs to it alone.  0 while the pointer is
    // over no pin, off this output altogether, or on an output the helper
    // could not take.
    quint64 hoverId_ = 0;
    // Where that marker was painted last, so clearing it repaints its own box
    // rather than the whole pin it was on.
    QRect hoverMarker_;
    // What the last setPins() asked to have repainted; see `repaintRegion()`.
    QRect repaintRegion_;
    // The open right-click menu: the pin it belongs to (0 while closed), the
    // copy rows it offers, where it is painted, and the row the pointer is
    // over. Row indices run over `menuRows_` first and then the action rows,
    // which is why `menuHover_` may reach `menuRows_.size()` plus the action
    // count.
    quint64 menuId_ = 0;
    QVector<ColorRow> menuRows_;
    QRect menuRect_;
    int menuHover_ = -1;
    // Set when a press was spent on the menu, so the second half of a
    // double-click is not read as a double-click on the pin underneath it (and
    // does not close that pin).
    bool swallowNextDoubleClick_ = false;
    class QTimer *zoomTimer_ = nullptr;
    bool hasFocus_ = false;
    bool visible_ = true;
    bool surfaceReady_ = false;
    // Whether the surface helper draws this output's pins; see
    // `setHdrPixels`.
    bool hdrPixels_ = false;
    // This surface's layer-shell window, kept for the keyboard hand-back; null
    // until showLayerSurface() succeeded.
    LayerShellQt::Window *layer_ = nullptr;
    // What interactivity was last committed: true = OnDemand, false = None.
    bool keyboardWanted_ = true;
};

} // namespace vshot

#pragma once

#include "color_card.hpp"

#include <QHash>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

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
        // The formats a pinned color card shows, empty for every other pin.
        // A right-click on the card turns them into a copy menu, and the menu
        // hands back the very string the card prints for that format.
        QVector<ColorRow> colorRows;
    };

    explicit PinSurface(QScreen *screen);

    // Maps the widget onto its layer-shell surface. Returns false when
    // LayerShellQt is unavailable.
    bool showLayerSurface();

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
    // Invoked when the user presses Space while this surface has keyboard focus.
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

protected:
    bool event(QEvent *event) override;
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
    };

    // The image's rect in output-local logical pixels.
    QRect localRect(const Item &item) const;
    // The image's rect in this surface's device pixels.
    QSize deviceTargetSize(const Entry &entry) const;
    // The image at this surface's device resolution, cached: painting the
    // source straight into the rect makes Qt resample it per frame with a 2x2
    // filter, which softens a 4K capture pinned on a 1080p output. Scaling
    // once here uses Qt's area filter instead.
    const QImage &renderSource(Entry &entry);
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

    QVector<Entry> entries_;
    QScreen *screen_;
    std::function<void(quint64)> picked_;
    std::function<void(quint64, QPoint)> dragMoved_;
    std::function<void(quint64, double)> zoomRequested_;
    std::function<void(quint64)> closeRequested_;
    std::function<void(quint64)> editRequested_;
    std::function<bool(quint64, const QString &)> copyRequested_;

    // The pin the user last clicked on this output: the one the zoom badge and
    // the Space edit shortcut belong to, and the only one drawn as focused.
    quint64 pickedId_ = 0;
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
    // The open right-click menu: the pin it belongs to (0 while closed), its
    // rows, where it is painted, and the row the pointer is over.
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
    // This surface's layer-shell window, kept for the keyboard hand-back; null
    // until showLayerSurface() succeeded.
    LayerShellQt::Window *layer_ = nullptr;
    // What interactivity was last committed: true = OnDemand, false = None.
    bool keyboardWanted_ = true;
};

} // namespace vshot

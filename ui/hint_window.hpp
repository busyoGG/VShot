#pragma once

#include "session_protocol.hpp"

#include <QByteArray>
#include <QMargins>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>

class QSocketNotifier;

namespace vshot {

/// The small overlay a scrolling capture keeps on screen.
///
/// It reports how far the stitch has come and turns Enter/Esc (or a click) into
/// the answer the CLI is waiting for.  It is deliberately *not* a full-screen
/// surface: the desktop stays live while the capture runs, so this only ever
/// covers the corner it sits in — and it has to sit outside the region being
/// captured, or it would be stitched into the very image it reports on.
///
/// When no corner is free (a selection that covers every monitor) it shrinks
/// to a sliver: invisible, but still holding the keyboard so Esc and Enter keep
/// working.
class HintWindow : public QWidget {
public:
    explicit HintWindow(const Session &session, QWidget *parent = nullptr);

    /// Places and maps the surface.  Returns false when the compositor offers
    /// no layer-shell, in which case there is no overlay to be had.
    bool showLayerSurface();

    bool isCancelled() const { return cancelled_; }

    void setTerminalCallback(std::function<void()> callback)
    {
        terminalCallback_ = std::move(callback);
    }

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    /// Reads whatever the CLI has sent on stdin and folds it into the display.
    void readStatus();
    /// Applies one `{"height":..,"frames":..,"note":".."}` line.
    void applyStatus(const QByteArray &line);
    /// Picks the corner of some output that the captured region does not use.
    void placeOutsideRegion();
    /// Ends the session: `cancelled` tells the CLI to throw the stitch away.
    void finish(bool cancelled);

    QString progressText() const;

    Session session_;
    QByteArray pending_;
    /// Distance from the surface to the top-left of its output.
    QMargins margins_;
    QSocketNotifier *notifier_ = nullptr;
    std::function<void()> terminalCallback_;
    std::uint32_t height_ = 0;
    std::uint32_t frames_ = 0;
    QString note_;
    QString message_;
    bool degraded_ = false;
    bool finished_ = false;
    bool cancelled_ = false;
};

} // namespace vshot

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#include "hint_window.hpp"

#include "i18n.hpp"

#include <LayerShellQt/Window>

#include <QFontMetrics>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSocketNotifier>

#include <unistd.h>

#include <algorithm>
#include <cstdint>

namespace vshot {

namespace {

/// Size of the pill that carries the progress, and the distance it keeps from
/// the output edges.
constexpr int kHintWidth = 380;
constexpr int kHintHeight = 46;
constexpr int kHintMargin = 16;
/// What is left of the surface when there is nowhere it can go without landing
/// on the captured region: too small to see, still focused.
constexpr int kDegradedSize = 8;

/// A corner of one output, in global logical coordinates.
struct Placement {
    int x = 0;
    int y = 0;
};

bool overlaps(const LogicalRect &region, const Placement &placement, int width, int height)
{
    const std::int64_t right = static_cast<std::int64_t>(placement.x) + width;
    const std::int64_t bottom = static_cast<std::int64_t>(placement.y) + height;
    return !(right <= region.x || placement.x >= region.right() || bottom <= region.y ||
             placement.y >= region.bottom());
}

/// The four corners of `geometry`, top-right first: the least intrusive spot
/// for a pill that has to stay off the captured region.
QVector<Placement> corners(const LogicalRect &geometry)
{
    const int right = static_cast<int>(geometry.right()) - kHintWidth - kHintMargin;
    const int bottom = static_cast<int>(geometry.bottom()) - kHintHeight - kHintMargin;
    const int left = geometry.x + kHintMargin;
    const int top = geometry.y + kHintMargin;
    return {
        Placement{right, top},
        Placement{right, bottom},
        Placement{left, top},
        Placement{left, bottom},
    };
}

bool sameGeometry(const QRect &screen, const LogicalRect &output)
{
    return screen.x() == output.x && screen.y() == output.y &&
           screen.width() == static_cast<int>(output.width) &&
           screen.height() == static_cast<int>(output.height);
}

} // namespace

HintWindow::HintWindow(const Session &session, QWidget *parent)
    : QWidget(parent)
    , session_(session)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::StrongFocus);
    resize(kHintWidth, kHintHeight);
}

bool HintWindow::showLayerSurface()
{
    winId();
    QWindow *handle = windowHandle();
    if (handle == nullptr) {
        return false;
    }
    auto *layer = LayerShellQt::Window::get(handle);
    if (layer == nullptr) {
        return false;
    }
    placeOutsideRegion();

    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    // Anchor the top-left corner and carve the position out with margins, so
    // the surface lands exactly where it was computed regardless of where the
    // output's origin is.
    LayerShellQt::Window::Anchors anchors(LayerShellQt::Window::AnchorTop);
    anchors |= LayerShellQt::Window::AnchorLeft;
    layer->setAnchors(anchors);
    layer->setExclusiveZone(-1);
    // The keyboard is the whole point: Enter and Esc have to reach this window
    // while the page underneath keeps scrolling on its own.
    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
    layer->setActivateOnShow(true);
    layer->setScope(QStringLiteral("vshot-long-shot"));
    layer->setDesiredSize(QSize(width(), height()));
    layer->setScreen(screen());
    layer->setMargins(margins_);

    show();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);

    // The CLI writes one JSON object per status update; watching stdin is how
    // this window learns how far the stitch has come, and EOF is how it learns
    // the capture is over.
    notifier_ = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, [this] { readStatus(); });
    return true;
}

void HintWindow::placeOutsideRegion()
{
    const LogicalRect region = session_.bounds;
    const std::int32_t centerX = region.x + static_cast<std::int32_t>(region.width / 2);
    const std::int32_t centerY = region.y + static_cast<std::int32_t>(region.height / 2);

    // The output the region sits on comes first — the pill is closest to what
    // it describes — and every other monitor is a fallback for when that one
    // has no room left.
    LogicalRect regionOutput;
    bool haveRegionOutput = false;
    for (const OutputSession &output : session_.outputs) {
        if (centerX >= output.geometry.x && centerX < output.geometry.right() &&
            centerY >= output.geometry.y && centerY < output.geometry.bottom()) {
            regionOutput = output.geometry;
            haveRegionOutput = true;
            break;
        }
    }

    QVector<QScreen *> order;
    for (QScreen *candidate : QGuiApplication::screens()) {
        if (candidate == nullptr) {
            continue;
        }
        if (haveRegionOutput && sameGeometry(candidate->geometry(), regionOutput)) {
            order.prepend(candidate);
        } else {
            order.append(candidate);
        }
    }

    for (QScreen *candidate : order) {
        const QRect geometry = candidate->geometry();
        const LogicalRect logical{geometry.x(), geometry.y(),
                                  static_cast<std::uint32_t>(geometry.width()),
                                  static_cast<std::uint32_t>(geometry.height())};
        for (const Placement &corner : corners(logical)) {
            if (overlaps(region, corner, kHintWidth, kHintHeight)) {
                continue;
            }
            setScreen(candidate);
            margins_ = QMargins(corner.x - geometry.x(), corner.y - geometry.y(), 0, 0);
            resize(kHintWidth, kHintHeight);
            return;
        }
    }

    // Every monitor is covered by the selection: go invisible rather than paint
    // over the thing being captured.  The surface stays mapped, so the keyboard
    // still reaches us and the capture can still be stopped.
    degraded_ = true;
    resize(kDegradedSize, kDegradedSize);
    if (!order.isEmpty()) {
        setScreen(order.first());
        margins_ = QMargins(kHintMargin, kHintMargin, 0, 0);
    }
}

void HintWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    if (degraded_) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(16, 18, 24, 235));
    painter.drawRoundedRect(rect(), 12.0, 12.0);

    const QFontMetrics metrics(font());
    const QString text = metrics.elidedText(progressText(), Qt::ElideRight, width() - 28);
    painter.setPen(QColor(233, 236, 255));
    painter.drawText(rect().adjusted(14, 0, -14, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
}

QString HintWindow::progressText() const
{
    if (!message_.isEmpty()) {
        return message_;
    }
    const QString progress = QStringLiteral("%1 px · %2 %3")
                                 .arg(height_)
                                 .arg(frames_)
                                 .arg(uiTr("frames"));
    // Once the capture has stopped, the CLI's note says why; while it runs, the
    // two keys are the useful thing to show.
    const QString tail = note_.isEmpty() ? uiTr("Enter finish · Esc cancel") : uiTr(note_);
    return QStringLiteral("%1  —  %2").arg(progress, tail);
}

void HintWindow::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        finish(true);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        finish(false);
        return;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
}

void HintWindow::mousePressEvent(QMouseEvent *event)
{
    // Left keeps the stitch, right throws it away — the same two answers the
    // keys give, for a user who is already holding the mouse.
    finish(event->button() == Qt::RightButton);
}

void HintWindow::readStatus()
{
    char buffer[1024];
    const ssize_t count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
    if (count <= 0) {
        // The CLI is done: it closed the pipe and is waiting for this surface
        // to leave the screen.  Nothing is left to report.
        notifier_->setEnabled(false);
        finish(false);
        return;
    }
    pending_.append(buffer, static_cast<int>(count));
    int newline = pending_.indexOf('\n');
    while (newline >= 0) {
        const QByteArray line = pending_.left(newline);
        pending_.remove(0, newline + 1);
        applyStatus(line);
        newline = pending_.indexOf('\n');
    }
}

void HintWindow::applyStatus(const QByteArray &line)
{
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject()) {
        return;
    }
    const QJsonObject object = document.object();
    height_ = static_cast<std::uint32_t>(object.value(QStringLiteral("height")).toInt());
    frames_ = static_cast<std::uint32_t>(object.value(QStringLiteral("frames")).toInt());
    note_ = object.value(QStringLiteral("note")).toString();
    message_.clear();
    update();
}

void HintWindow::finish(bool cancelled)
{
    if (finished_) {
        return;
    }
    finished_ = true;
    cancelled_ = cancelled;
    if (notifier_ != nullptr) {
        notifier_->setEnabled(false);
    }
    hide();
    if (terminalCallback_) {
        terminalCallback_();
    }
}

} // namespace vshot

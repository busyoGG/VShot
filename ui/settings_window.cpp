// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// The settings window: an ordinary top-level dialog over the shared config
// file, so the style a capture session starts from and the command-line
// defaults the CLI falls back to can be looked at and changed in one place
// instead of in a JSON file by hand.
//
// This window is the only thing that writes those values: a capture session
// never does, so what is set here is exactly what every session opens with.
//
// It is deliberately *not* a layer surface like the capture overlay: there is
// no capture in progress, no frozen frame to cover, and a normal window is what
// a compositor can place, focus and stack without any help from vshot.
//
// The layout is a sidebar of sections beside a scrolling column of cards, which
// is what the settings windows people already use are shaped like (macOS's
// System Settings, GNOME's Settings, VS Code's).  Two things follow from that
// shape and are worth stating because they are decisions, not accidents:
//
//   * Rows are label-left, control-right, on one line, rather than the
//     label-above-control a `QFormLayout` gives.  Nineteen short settings fit
//     in half the vertical space that way, so the whole editor section is
//     visible at once instead of needing a scroll to reach the mosaic fields.
//   * The combo boxes and spin boxes paint their own chevrons.  The native
//     indicators are beveled triangles from a different decade; everything else
//     in this project draws its icons, so these do too.

#include "settings_window.hpp"

#include "config.hpp"
#include "shortcuts.hpp"
#include "text_size.hpp"
#include "i18n.hpp"

#include <QApplication>
#include <QAbstractButton>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QDoubleSpinBox>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTimer>
#include <QStackedWidget>
#include <QStyle>
#include <QStringList>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <unistd.h>

#include <algorithm>
#include <functional>

namespace vshot {
namespace {

// The window's palette.  The neutrals are the annotation toolbar's own, one
// step deeper so the cards can sit lighter than the page behind them; the
// accent is the #dde1ff the toolbar already highlights with, so the two
// surfaces read as one program.
constexpr const char *kWindowBg = "#1c2129";
constexpr const char *kCardBg = "#242b34";
constexpr const char *kCardBorder = "#2f3742";
constexpr const char *kControlBg = "#2a303a";
constexpr const char *kControlBorder = "#3a424e";
constexpr const char *kInk = "#e6e1e5";
constexpr const char *kInkDim = "#9aa3ae";
constexpr const char *kInkFaint = "#6f7680";
constexpr const char *kAccent = "#dde1ff";
constexpr const char *kAccentInk = "#00145c";

const QColor kChevron(0x9a, 0xa3, 0xae);

/// The height every form control is given.
///
/// It has to be forced rather than left to each widget: a `QSpinBox` asks the
/// style for room for its up/down buttons even with `NoButtons` set, so it
/// reports a hint 12px taller than the combo boxes and line edits beside it and
/// the rows come out visibly uneven.  The value is the stylesheet's `min-height`
/// of 32px plus its 1px border top and bottom; change one and change the other.
constexpr int kControlHeight = 34;

// The built-in defaults for the settings whose value is optional -- the ones a
// `cli` key can leave unsaid.  These are the numbers the window opens on, so
// that what a user reads is what the CLI will actually use; `spinValue` writes
// the sentinel back when one is left alone, so the file stays free of a value
// nobody chose and a later version's default can still reach this user.
//
// They are duplicated here because the Qt side cannot ask the Rust side for
// them.  `vshot-settings-check` reads them back out of `src/cli.rs` and
// `src/record/`, so a default changed over there fails that check instead of
// silently disagreeing with what the window shows.
constexpr int kDefaultLongNotches = 1;
constexpr int kDefaultLongMaxHeight = 30'000;
constexpr int kDefaultLongMaxFrames = 6'000;
constexpr int kDefaultLongTimeout = 120;
constexpr int kDefaultLongIgnoreTop = 0;
constexpr int kDefaultPinDensity = 0;  // not a density: "work it out from the screen"
constexpr int kDefaultRecordFps = 60;
constexpr int kDefaultReplayWindow = 30;
constexpr int kDefaultReplayGop = 1;
constexpr int kDefaultReplayFps = 30;

// The same idea for the ones a combo box carries: the name of the entry the
// CLI falls back to, which the box's leading entry shows so the default is
// readable rather than implied by the word "default".
constexpr const char *kDefaultPngCompression = "fast";
constexpr const char *kDefaultHdrFormat = "avif";
constexpr const char *kDefaultToneMap = "auto";
constexpr const char *kDefaultEncoder = "h264";
constexpr const char *kDefaultEncoderBackend = "auto";
constexpr const char *kDefaultLongInject = "auto";

/// The stylesheet.  Object names rather than widget classes carry the
/// structure, so a card's rows and a page's headings can be told apart without
/// subclassing everything.
QString dialogStyleSheet()
{
    return QStringLiteral(R"(
QDialog { background: %1; }
QWidget#page { background: transparent; }

QLabel#windowTitle { color: %2; font-size: 15px; font-weight: 600; }
QLabel#windowHint  { color: %3; font-size: 11px; }
QLabel#pageTitle   { color: %2; font-size: 17px; font-weight: 600; }
QLabel#pageHint    { color: %3; font-size: 12px; }
QLabel#rowLabel    { color: %2; font-size: 13px; }
QLabel#rowHint     { color: %3; font-size: 11px; }
QLabel#rowValue    { color: %3; font-size: 12px; font-weight: 600; }
QLabel#status      { color: %3; font-size: 11px; }
QLabel#status[error="true"] { color: #ffb4ab; }

QWidget#card { background: %4; border: 1px solid %5; border-radius: 12px; }
QFrame#rowDivider { background: %5; border: 0; }

QListWidget#sidebar { background: transparent; border: 0; outline: 0;
            font-size: 13px; padding: 0; }
QListWidget#sidebar::item { color: %6; border-radius: 8px;
            padding: 0 10px; margin: 1px 0; height: 34px; }
QListWidget#sidebar::item:hover { background: #262d37; color: %2; }
QListWidget#sidebar::item:selected { background: %7; color: %8; font-weight: 600; }

/* QDoubleSpinBox is a sibling of QSpinBox, not a subclass of it, so the class
   selector above does not reach it: a page that used one for a percentage got
   the platform's own box next to restyled ones.  It is listed here so the two
   look alike. */
QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox { color: %2; background: %9;
            border: 1px solid %10; border-radius: 8px;
            padding: 0 10px; min-height: 32px; font-size: 13px;
            selection-color: %8; selection-background-color: %7; }
QComboBox:hover, QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover { border-color: #4d5765; }
QComboBox:focus, QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: %7; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled {
            color: %11; background: #242a33; }

/* The indicator is painted by the widgets themselves (see ModernComboBox and
   ModernSpinBox); the native one is switched off here.  `width: 0` alone
   leaves a sliver on some styles, so the arrow is also told to draw nothing. */
QComboBox::drop-down { border: 0; background: transparent; width: 0; }
QComboBox::down-arrow { image: none; width: 0; height: 0; }
QComboBox QAbstractItemView { color: %2; background: #262d37;
            border: 1px solid %10; border-radius: 10px; padding: 4px;
            outline: 0; selection-color: %8; selection-background-color: %7; }
QComboBox QAbstractItemView::item { min-height: 28px; border-radius: 6px;
            padding: 0 8px; }

QPushButton { color: %2; background: #2f3742; border: 1px solid transparent;
            border-radius: 8px; padding: 0 16px; min-height: 32px;
            font-size: 13px; }
QPushButton:hover { background: #3a434f; }
QPushButton:pressed { background: #333b46; }
QPushButton:focus { border-color: %7; }
QPushButton#saveButton { background: %7; color: %8; font-weight: 600; }
QPushButton#saveButton:hover { background: #e9ecff; }
QPushButton#saveButton:pressed { background: #c7cdf2; }
QPushButton#swatch { text-align: left; padding-left: 8px; font-size: 12px;
            font-family: monospace; }

/* A modal box opened from this dialog inherits the sheet, and the QDialog rule
   above then forces its background dark.  Qt gives a box's own labels no object
   name, so they match nothing and keep the palette's WindowText -- which on a
   light system scheme is near-black, and near-black on the dark background is
   unreadable.  The labels are named here instead, and given the same ink the
   dialog's own text uses. */
QMessageBox { background: %1; }
QMessageBox QLabel, QMessageBox QLabel#qt_msgbox_label,
            QMessageBox QLabel#qt_msgboxex_icon_label { color: %2; background: transparent; }

QScrollArea { border: 0; background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px;
            border: 0; }
QScrollBar::handle:vertical { background: #3a434f; border-radius: 5px;
            min-height: 28px; border: 0; }
QScrollBar::handle:vertical:hover { background: #4d5765; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
)")
        .arg(QLatin1String(kWindowBg), QLatin1String(kInk), QLatin1String(kInkDim),
             QLatin1String(kCardBg), QLatin1String(kCardBorder), QLatin1String(kInkDim),
             QLatin1String(kAccent), QLatin1String(kAccentInk), QLatin1String(kControlBg),
             QLatin1String(kControlBorder), QLatin1String(kInkFaint));
}

/// Draws a 2px chevron centred in `box`, pointing down or up.
void paintChevron(QPainter &painter, const QRectF &box, const QColor &color, bool down)
{
    constexpr qreal halfWidth = 4.0;
    constexpr qreal halfHeight = 2.4;
    const QPointF center = box.center();
    QPainterPath path;
    if (down) {
        path.moveTo(center.x() - halfWidth, center.y() - halfHeight);
        path.lineTo(center.x(), center.y() + halfHeight);
        path.lineTo(center.x() + halfWidth, center.y() - halfHeight);
    } else {
        path.moveTo(center.x() - halfWidth, center.y() + halfHeight);
        path.lineTo(center.x(), center.y() - halfHeight);
        path.lineTo(center.x() + halfWidth, center.y() + halfHeight);
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
    painter.restore();
}

/// A combo box that paints its own chevron where the native indicator would be.
/// The stylesheet hides the indicator; this fills the space it left.
class ModernComboBox final : public QComboBox {
public:
    explicit ModernComboBox(QWidget *parent = nullptr)
        : QComboBox(parent)
    {
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QComboBox::paintEvent(event);
        QPainter painter(this);
        // The painter's coordinate system on a widget is already in logical
        // pixels -- Qt has applied the ratio -- so dividing by
        // devicePixelRatioF() here would halve every coordinate on a scale-2
        // output and leave the chevron jammed in the corner.
        const QRectF box(width() - 24.0, 0.0, 16.0, height());
        paintChevron(painter, box, kChevron, true);
    }
};

/// A spin box that paints its own up/down chevrons and routes clicks on them.
///
/// The native buttons are switched off rather than restyled: `NoButtons` is the
/// only way to be certain no style draws its own triangles, and it leaves the
/// arrows to be drawn here in the same idiom as the combo boxes'.  Keyboard
/// stepping and the wheel keep working because the base class still owns them.
class ModernSpinBox final : public QSpinBox {
public:
    explicit ModernSpinBox(QWidget *parent = nullptr)
        : QSpinBox(parent)
    {
        setButtonSymbols(QAbstractSpinBox::NoButtons);
        // The arrows are painted in a strip the line edit also covers: in a
        // 120px box the line edit is 108px wide and the arrows start at 94, so
        // a click on an arrow lands on the line edit and the spin box's own
        // `mousePressEvent` never sees it -- which is why the arrows did
        // nothing.  The filter is what puts them back in reach; the handlers
        // below still serve the pixels of the strip the line edit does not
        // cover, and a box that is ever laid out wide enough for the two to
        // stop overlapping keeps working either way.
        lineEdit()->installEventFilter(this);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QSpinBox::paintEvent(event);
        QPainter painter(this);
        paintChevron(painter, upBox(), kChevron, false);
        paintChevron(painter, downBox(), kChevron, true);
    }

    /// Whether `position`, in this widget's coordinates, is on one of the two
    /// arrows -- the strip the line edit overlaps.
    bool onArrow(const QPointF &position) const
    {
        return upBox().contains(position) || downBox().contains(position);
    }

    /// The arrows live on the line edit, so the clicks on them arrive here
    /// rather than at `mousePressEvent`.  `position()` on the event is in the
    /// *line edit's* coordinates and the arrows are laid out in the spin box's,
    /// so the point is mapped across before it is tested.
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == lineEdit()) {
            if (event->type() == QEvent::MouseButtonPress) {
                auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->button() == Qt::LeftButton) {
                    const QPointF position =
                        lineEdit()->mapTo(this, mouse->position().toPoint());
                    if (upBox().contains(position)) {
                        stepUp();
                        setFocus(Qt::MouseFocusReason);
                        return true;
                    }
                    if (downBox().contains(position)) {
                        stepDown();
                        setFocus(Qt::MouseFocusReason);
                        return true;
                    }
                }
            } else if (event->type() == QEvent::MouseMove) {
                // The arrows are hit targets, so say so under the pointer.  Not
                // consumed: the line edit still wants the move for its own
                // selection.
                auto *mouse = static_cast<QMouseEvent *>(event);
                const QPointF position =
                    lineEdit()->mapTo(this, mouse->position().toPoint());
                lineEdit()->setCursor(onArrow(position) ? Qt::PointingHandCursor
                                                        : Qt::IBeamCursor);
            }
        }
        return QSpinBox::eventFilter(watched, event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const QPointF position = event->position();
        if (event->button() == Qt::LeftButton && upBox().contains(position)) {
            stepUp();
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton && downBox().contains(position)) {
            stepDown();
            event->accept();
            return;
        }
        QSpinBox::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        // The arrows are hit targets, so say so under the pointer.
        const QPointF position = event->position();
        const bool onArrow =
            upBox().contains(position) || downBox().contains(position);
        setCursor(onArrow ? Qt::PointingHandCursor : Qt::IBeamCursor);
        QSpinBox::mouseMoveEvent(event);
    }

private:
    /// The two arrow hit boxes, stacked in the right-hand strip.  Logical
    /// pixels, like every other coordinate the painter and the event handlers
    /// see: Qt has already folded the output's scale into both.
    QRectF upBox() const
    {
        constexpr qreal strip = 26.0;
        constexpr qreal arrowWidth = 16.0;
        return QRectF(width() - strip, 0.0, arrowWidth, height() / 2.0);
    }

    QRectF downBox() const
    {
        QRectF box = upBox();
        box.moveTop(box.height());
        return box;
    }
};

/// [`ModernSpinBox`] for a value that is a real number rather than a count.
///
/// The tone-map white level is a fraction of the range, so it is stepped in
/// hundredths and shown as a percentage: the map's own arithmetic is in
/// fractions, but "80 %" is what a user can picture.
class ModernDoubleSpinBox final : public QDoubleSpinBox {
public:
    explicit ModernDoubleSpinBox(QWidget *parent = nullptr)
        : QDoubleSpinBox(parent)
    {
        setButtonSymbols(QAbstractSpinBox::NoButtons);
        setDecimals(2);
        setSingleStep(0.01);
        lineEdit()->installEventFilter(this);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QDoubleSpinBox::paintEvent(event);
        QPainter painter(this);
        paintChevron(painter, upBox(), kChevron, false);
        paintChevron(painter, downBox(), kChevron, true);
    }

    bool onArrow(const QPointF &position) const
    {
        return upBox().contains(position) || downBox().contains(position);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == lineEdit()) {
            if (event->type() == QEvent::MouseButtonPress) {
                auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->button() == Qt::LeftButton) {
                    const QPointF position =
                        lineEdit()->mapTo(this, mouse->position().toPoint());
                    if (upBox().contains(position)) {
                        stepUp();
                        setFocus(Qt::MouseFocusReason);
                        return true;
                    }
                    if (downBox().contains(position)) {
                        stepDown();
                        setFocus(Qt::MouseFocusReason);
                        return true;
                    }
                }
            } else if (event->type() == QEvent::MouseMove) {
                auto *mouse = static_cast<QMouseEvent *>(event);
                const QPointF position =
                    lineEdit()->mapTo(this, mouse->position().toPoint());
                lineEdit()->setCursor(onArrow(position) ? Qt::PointingHandCursor
                                                        : Qt::IBeamCursor);
            }
        }
        return QDoubleSpinBox::eventFilter(watched, event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const QPointF position = event->position();
        if (event->button() == Qt::LeftButton && upBox().contains(position)) {
            stepUp();
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton && downBox().contains(position)) {
            stepDown();
            event->accept();
            return;
        }
        QDoubleSpinBox::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPointF position = event->position();
        const bool onArrow = upBox().contains(position) || downBox().contains(position);
        setCursor(onArrow ? Qt::PointingHandCursor : Qt::IBeamCursor);
        QDoubleSpinBox::mouseMoveEvent(event);
    }

private:
    QRectF upBox() const
    {
        constexpr qreal strip = 26.0;
        constexpr qreal arrowWidth = 16.0;
        return QRectF(width() - strip, 0.0, arrowWidth, height() / 2.0);
    }

    QRectF downBox() const
    {
        QRectF box = upBox();
        box.moveTop(box.height());
        return box;
    }
};

/// A two-state switch: a pill that slides rather than a tick box.
///
/// Drawn here for the same reason the chevrons are -- the platform's check
/// indicator is a beveled box that belongs to a different decade than the rest
/// of this window -- and because a switch reads as "this is on" at a glance in
/// a row where the label is already carrying the meaning.
class ModernSwitch final : public QAbstractButton {
public:
    explicit ModernSwitch(QWidget *parent = nullptr)
        : QAbstractButton(parent)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFixedWidth(44);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal height = 22.0;
        const QRectF track(0.0, (this->height() - height) / 2.0, width(), height);
        const qreal radius = height / 2.0;
        painter.setPen(Qt::NoPen);
        painter.setBrush(isChecked() ? QColor(kAccent) : QColor(kControlBorder));
        painter.drawRoundedRect(track, radius, radius);
        // The knob: inset from the track, and at whichever end the state says.
        const qreal inset = 3.0;
        const qreal knob = height - 2 * inset;
        const qreal x = isChecked() ? track.right() - inset - knob : track.left() + inset;
        painter.setBrush(isChecked() ? QColor(kAccentInk) : QColor(kInkDim));
        painter.drawEllipse(QRectF(x, track.top() + inset, knob, knob));
    }
};

/// A combo box whose entries are the same names the config file accepts, with a
/// leading entry for "the file says nothing".
///
/// That leading entry *names* the built-in default instead of saying the word
/// "default".  The box is the only place a user can find out which codec or
/// compression level the CLI falls back to, and a row reading "built-in
/// default" left them to guess; `builtin` is that name.  What it means is still
/// "no key in the file" -- picking it writes nothing -- so a default left alone
/// keeps following the built-in one.
ModernComboBox *choiceBox(QWidget *parent, const QStringList &values, const QString &builtin)
{
    auto *box = new ModernComboBox(parent);
    box->addItem(uiTr("%1 (built-in default)").arg(builtin), QString());
    for (const QString &value : values) {
        box->addItem(value, value);
    }
    return box;
}

/// Selects `value` in a box built by [`choiceBox`], falling back to the empty
/// entry when the value is not one of the offered ones.
void selectChoice(QComboBox *box, const QString &value)
{
    const int index = box->findData(value);
    box->setCurrentIndex(index < 0 ? 0 : index);
}

/// A spin box whose value is optional.
///
/// Zero is the sentinel the config layer reads as "the file says nothing", and
/// a box showing that zero -- or the word "default" -- left the built-in
/// default invisible: nothing in the window said that `--fps` means 60, or that
/// an unset scroll timeout means two minutes.  So the box opens on the built-in
/// default itself, and the number on screen is the number the CLI will use.
/// Nothing is lost by it: `spinValue` writes the sentinel back while the value
/// is still the default, so a default left alone stays out of the file and
/// keeps following the built-in one.
///
/// `inferredText` is for the one box where zero is an answer of its own rather
/// than just "unset": a pin density of zero means "work it out from the
/// screen", and that is worth saying in words.
ModernSpinBox *optionalSpin(QWidget *parent, int builtin, int maximum, const QString &suffix,
                            const QString &inferredText = QString())
{
    auto *spin = new ModernSpinBox(parent);
    spin->setRange(0, maximum);
    if (!inferredText.isEmpty()) {
        spin->setSpecialValueText(inferredText);
    }
    spin->setValue(builtin);
    if (!suffix.isEmpty()) {
        spin->setSuffix(suffix);
    }
    return spin;
}

/// The value a box opens on: what the file remembers, or the built-in default
/// when the file says nothing (which is what its zero means).
template <typename T> int rememberedOr(T remembered, int builtin)
{
    return remembered == 0 ? builtin : static_cast<int>(remembered);
}

/// Reads a spin box back into the config: a value still sitting on its built-in
/// default is written as the sentinel, which is how the file says "nothing
/// here, use the built-in".  Writing the number instead would freeze today's
/// default into the file and stop a later version's better one from reaching
/// this user.
std::uint32_t spinValue(const QSpinBox *spin, int builtin)
{
    const int value = std::max(0, spin->value());
    return static_cast<std::uint32_t>(value == builtin ? 0 : value);
}

/// The two answers the microphone row always offers, and the data the combo
/// carries for them.  A real entry carries the PipeWire node name, which is
/// what the file stores -- so the markers have to be names no node can have.
/// The empty string cannot be one: `"mic": ""` is already how the file spells
/// "the session's default input".
const QString kNoMicrophone = QStringLiteral("\x01 none");
const QString kDefaultMicrophone = QStringLiteral("\x01 default");

/// The combo entry a stored microphone maps to: the "none" marker when the
/// config has no microphone, the "default input" marker for the empty name, and
/// the node name itself otherwise.
QString microphoneMarker(bool enabled, const QString &name)
{
    if (!enabled) {
        return kNoMicrophone;
    }
    return name.isEmpty() ? kDefaultMicrophone : name;
}

/// The `follow` line's text: the window names joined with commas.  One line
/// carries several names, which is what the file stores as an array, and a hand
/// edit reads the same as the `--follow` list on the command line.
QString followText(const QStringList &names)
{
    return names.join(QStringLiteral(", "));
}

/// Splits the follow line back into names, dropping blanks: a trailing comma or
/// an empty field is a typo, not a window named the empty string, and the CLI
/// drops the same blanks out of a remembered list.
QStringList parseFollowText(const QString &text)
{
    QStringList names;
    for (const QString &piece : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString name = piece.trimmed();
        if (!name.isEmpty()) {
            names.append(name);
        }
    }
    return names;
}

/// How long to wait for the input listing.  `vshot` gives up on PipeWire
/// itself after five seconds, so this is that plus room to start and exit.
constexpr int kMicrophoneListTimeoutMs = 8000;

/// Reads what `vshot record mics` printed into `(node name, label)` pairs.
///
/// The lines are `serial<TAB>name<TAB>description`.  The serial is not offered:
/// a name is stable across sessions and a serial is not, and the file has to
/// outlive the session.  Anything that does not look like a line is skipped, so
/// a warning on stdout does not become a device.
QList<QPair<QString, QString>> parseMicrophoneListing(const QString &listing)
{
    QList<QPair<QString, QString>> inputs;
    for (const QString &line : listing.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList fields = line.split(QLatin1Char('\t'));
        if (fields.size() < 2 || fields.at(1).isEmpty()) {
            continue;
        }
        const QString description = fields.size() > 2 ? fields.at(2) : QString();
        inputs.append({fields.at(1), description.isEmpty() ? fields.at(1) : description});
    }
    return inputs;
}

/// The path of the `vshot` binary to ask: this process *is* the helper, so its
/// own path names the program beside it.  A build tree puts it in the parent,
/// and anything else falls back to the name on `PATH`.
QString helperProgram()
{
    char buffer[4096];
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return QStringLiteral("vshot");
    }
    buffer[length] = '\0';
    const QString helper = QString::fromLocal8Bit(buffer);
    const QString program = QFileInfo(helper).absolutePath() + QStringLiteral("/vshot");
    if (QFileInfo::exists(program)) {
        return program;
    }
    const QString beside =
        QFileInfo(helper).absolutePath() + QStringLiteral("/../target/release/vshot");
    if (QFileInfo::exists(beside)) {
        return QDir::cleanPath(beside);
    }
    return QStringLiteral("vshot");
}

/// The audio inputs `vshot record mics` lists, as `(node name, label)` pairs:
/// the name is what `--mic` accepts and what the file keeps, the label is the
/// description a person reads.
///
/// The list has to come from the running session -- it is the only thing that
/// knows which inputs exist -- so this runs `vshot`, the same discovery the
/// capture overlay makes for the OCR engine.
///
/// It is asked for **once**, and in the background.  The command starts a
/// second process that has to bring PipeWire up before it can answer, which
/// takes about a second, and both the recording and the replay card carry a
/// microphone row -- so asking synchronously while building the pages left the
/// window blank for two seconds before it appeared, every time it was opened,
/// to fill in a list most users never look at.  The window now opens at once
/// with the two answers that always exist, and the devices drop into both boxes
/// when the answer lands; the Detect button re-asks.
///
/// Nothing here can fail the settings window.  A machine without PipeWire, a
/// session with no inputs, or a vshot that cannot be found all yield an empty
/// list, and the row then offers the two answers that always exist.
class MicrophoneProbe : public QObject {
public:
    explicit MicrophoneProbe(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    /// Fires when the listing arrives, from the event loop -- never from inside
    /// [`start`].  Both cards connect here rather than to a signal of their own,
    /// because the answer is the same for both.
    std::function<void()> onFinished;

    /// Asks for the listing, unless it is already on its way or already in.
    void start()
    {
        if (finished_ || process_ != nullptr) {
            return;
        }
        process_ = new QProcess(this);
        process_->setProgram(helperProgram());
        process_->setArguments({QStringLiteral("record"), QStringLiteral("mics")});
        process_->setStandardInputFile(QProcess::nullDevice());
        // Both are wired: a program that cannot be started reports it through
        // `errorOccurred` and never reaches `finished`, and a program that
        // starts and then hangs would otherwise leave the row waiting forever.
        connect(process_, &QProcess::finished, this, [this] { finish(true); });
        connect(process_, &QProcess::errorOccurred, this, [this] { finish(false); });
        process_->start();
        QTimer::singleShot(kMicrophoneListTimeoutMs, this, [this] { finish(false); });
    }

    /// Asks again from scratch, whatever the last answer was.
    ///
    /// This is what the Detect button does, and the reason it is not [`start`]:
    /// the point of pressing it is that something changed -- a microphone was
    /// plugged in, PipeWire was started -- so an answer already in is exactly
    /// the one the user is asking to replace.
    void restart()
    {
        if (process_ != nullptr) {
            return;
        }
        finished_ = false;
        inputs_.clear();
        start();
    }

    /// The inputs, empty until the answer lands.
    const QList<QPair<QString, QString>> &inputs() const { return inputs_; }
    /// Whether the answer has landed, whether or not it found anything.
    bool finished() const { return finished_; }

private:
    /// Reads the listing and hands it on.  `readable` is false when the answer
    /// is a failure rather than a listing -- a program that would not start, or
    /// one the timeout gave up on -- in which case the inputs stay empty.
    void finish(bool readable)
    {
        // The timeout, the process and the error can all arrive: the first one
        // here is the answer, and the rest are the same answer said again.
        if (finished_) {
            return;
        }
        finished_ = true;
        if (process_ != nullptr) {
            if (readable) {
                inputs_ = parseMicrophoneListing(
                    QString::fromUtf8(process_->readAllStandardOutput()));
            }
            if (process_->state() != QProcess::NotRunning) {
                process_->kill();
                process_->waitForFinished(100);
            }
            process_->deleteLater();
            process_ = nullptr;
        }
        if (onFinished) {
            onFinished();
        }
    }

    QProcess *process_ = nullptr;
    QList<QPair<QString, QString>> inputs_;
    bool finished_ = false;
};

/// A small square of a colour, drawn with the same rounding as the swatch
/// button it sits in.
QPixmap colorChip(const QColor &color)
{
    constexpr int size = 16;
    constexpr qreal ratio = 2.0;
    QPixmap pixmap(static_cast<int>(size * ratio), static_cast<int>(size * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(0.75, 0.75, size - 1.5, size - 1.5), 4.0, 4.0);
    // A checkerboard shows through a translucent colour, the way the pinned
    // color card does, so alpha is visible rather than silently dark.
    if (color.alpha() < 255) {
        painter.save();
        painter.setClipPath(path);
        painter.fillRect(pixmap.rect(), QColor(0x6f, 0x76, 0x80));
        painter.fillRect(QRectF(0, 0, size / 2.0, size / 2.0), QColor(0x9a, 0xa3, 0xae));
        painter.fillRect(QRectF(size / 2.0, size / 2.0, size / 2.0, size / 2.0),
                         QColor(0x9a, 0xa3, 0xae));
        painter.restore();
    }
    painter.fillPath(path, color);
    painter.setPen(QPen(QColor(0, 0, 0, 70), 1.0));
    painter.drawPath(path);
    return pixmap;
}

/// The colour, shown as a chip plus its hex text.  Clicking opens a picker; a
/// plain `QColorDialog` is enough here, unlike in the capture overlay, where an
/// extra top-level window would lose the keyboard grab.
class ColorButton final : public QPushButton {
public:
    explicit ColorButton(QWidget *parent, const QString &title = QString())
        : QPushButton(parent)
        , title_(title.isEmpty() ? uiTr("Annotation color") : title)
    {
        setObjectName(QStringLiteral("swatch"));
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumWidth(132);
        connect(this, &QPushButton::clicked, this, [this] {
            const QColor chosen = QColorDialog::getColor(
                color_, this, title_, QColorDialog::ShowAlphaChannel);
            if (chosen.isValid()) {
                setColor(chosen);
                if (onColorChanged) {
                    onColorChanged(chosen);
                }
            }
        });
    }

    void setColor(const QColor &color)
    {
        color_ = color;
        if (!color.isValid()) {
            setText(QString());
            setIcon(QIcon());
            return;
        }
        setText(vshot::colorText(color));
        setIcon(QIcon(colorChip(color)));
        setIconSize(QSize(16, 16));
        setToolTip(title_);
    }

    QColor color() const { return color_; }

    /// Emitted when the picker hands back a new colour, so a page that has to
    /// do something else with it (remember it in its own state, say) hears
    /// about it.  Not emitted by `setColor`, which is the caller's own doing.
    std::function<void(const QColor &)> onColorChanged;

private:
    QString title_;
    QColor color_{255, 64, 64, 255};
};

/// The button that captures a key: it shows the binding an action has, and
/// while it is armed it takes the next key press as the new one.
///
/// A line edit cannot do this. Typing "Ctrl+K" into one is a spelling the user
/// has to guess, and a wrong guess is silently stored as a binding that matches
/// nothing -- which reads as "this action has no key" rather than as a mistake.
/// Capturing the real key is both quicker and the only way the spelling is
/// guaranteed to be one `QKeySequence` understands.
///
/// Escape while armed puts the binding back to what it was, and Backspace or
/// Delete clears it, which is the one thing a capture widget has to offer that
/// a key press cannot express.
///
/// The button shows what the *action* answers to -- several keys joined by
/// commas -- but records one combination at a time: what it hands to
/// `onKeysChanged` is the single key the user pressed, and the binding it is
/// showing is the list the dialog around it keeps.
class KeyCaptureButton final : public QPushButton {
public:
    explicit KeyCaptureButton(QWidget *parent = nullptr)
        : QPushButton(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setMinimumWidth(150);
        setFocusPolicy(Qt::StrongFocus);
        connect(this, &QPushButton::clicked, this, [this] {
            if (onActivated) {
                onActivated();
                return;
            }
            setArmed(true);
        });
    }

    /// When set, a click calls this instead of arming the button.  The row on
    /// the settings page opens the editor with it -- the action's keys are a
    /// list there, and a list is not something a button can capture in place --
    /// while the editor's own record button leaves it unset and arms.
    std::function<void()> onActivated;

    /// The key the user just recorded, or an empty sequence when they cleared
    /// the binding.  Not a list: one press is one combination.
    QKeySequence keys() const { return keys_; }
    void setKeys(const QKeySequence &keys)
    {
        keys_ = keys;
        updateText();
    }

    /// Fires with the binding the user pressed, or an empty sequence when they
    /// cleared one.  Not fired by `setKeys`.
    std::function<void(const QKeySequence &)> onKeysChanged;

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (!armed_) {
            QPushButton::keyPressEvent(event);
            return;
        }
        event->accept();
        const int key = event->key();
        // The two keys a capture has to answer that are not keys to bind:
        // Escape means "never mind", and the delete keys mean "no key at all".
        // Neither can be bound from here either, which is the price of that --
        // both are still reachable by hand-editing the file.
        if (key == Qt::Key_Escape) {
            setArmed(false);
            return;
        }
        if (key == Qt::Key_Backspace || key == Qt::Key_Delete) {
            setArmed(false);
            take(QKeySequence());
            return;
        }
        // A modifier on its own is not a key press: the binding has to wait for
        // the key it belongs to, or holding Ctrl would immediately bind "Ctrl".
        // Tab is let through to the base class so the focus can still be moved
        // with the keyboard while a button is armed.
        if (key == Qt::Key_Tab || isModifier(key)) {
            QPushButton::keyPressEvent(event);
            return;
        }
        setArmed(false);
        // The modifiers the event carries, minus the ones that are part of the
        // key itself: `QKeySequence` wants them in its own bits, and an event
        // for Shift+Backtab already has Shift in its modifiers.
        take(QKeySequence(static_cast<int>(static_cast<int>(event->modifiers()) | key)));
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        // Losing the focus while armed is the same as pressing Escape: a button
        // left armed would swallow the next key wherever it landed, which is
        // not a thing a user can be expected to notice has happened.
        if (armed_) {
            setArmed(false);
        }
        QPushButton::focusOutEvent(event);
    }

private:
    static bool isModifier(int key)
    {
        switch (key) {
        case Qt::Key_Shift:
        case Qt::Key_Control:
        case Qt::Key_Alt:
        case Qt::Key_Meta:
        case Qt::Key_AltGr:
            return true;
        default:
            return false;
        }
    }

    void setArmed(bool armed)
    {
        armed_ = armed;
        updateText();
        if (armed) {
            setFocus(Qt::MouseFocusReason);
        }
    }

    void take(const QKeySequence &keys)
    {
        keys_ = keys;
        updateText();
        if (onKeysChanged) {
            onKeysChanged(keys);
        }
    }

    void updateText()
    {
        if (armed_) {
            setText(uiTr("Press the keys for this action"));
            return;
        }
        setText(keys_.isEmpty() ? uiTr("None")
                                : keys_.toString(QKeySequence::PortableText));
    }

    QKeySequence keys_;
    bool armed_ = false;
};

/// The dialog one key-binding row opens: record a key at the top, one row per
/// key the action already answers to below, each with the button that removes
/// it.
///
/// The row on the page can only ever show *that* an action has several keys;
/// this is where one of them can be taken away again, and where a new one can
/// be added without throwing away the ones already there.  It edits a copy and
/// hands the whole list back, so a dialog the user cancels leaves the action
/// exactly as it was.
///
/// The conflict rule is the reason this is a dialog rather than a row of
/// widgets: recording a key another action already owns has to *ask*, and the
/// answer decides whether the other action loses the key.  A key the action
/// being edited already owns is not a conflict -- it is the user pressing the
/// same key twice -- so it is taken as the duplicate it is and nothing is
/// added.
class ShortcutEditorDialog final : public QDialog {
public:
    /// `action` is the one being edited, `preferences` the state to edit.  The
    /// dialog reads and writes the preferences it is handed, so the page sees
    /// every change the moment it is made and a Cancel only has to put back the
    /// copy it took.  `ask` puts a conflict to the user and answers whether the
    /// combination moves; empty means the ordinary message box.
    ShortcutEditorDialog(ShortcutAction action, ShortcutPreferences *preferences,
                         QWidget *parent, std::function<bool(const QString &)> ask)
        : QDialog(parent)
        , action_(action)
        , preferences_(preferences)
        , saved_(*preferences)
        , ask_(std::move(ask))
    {
        setObjectName(QStringLiteral("shortcutEditor"));
        setWindowTitle(shortcutBinding(action).label);
        setModal(true);
        setMinimumWidth(380);
        // Its own copy of the sheet rather than the settings window's.  A
        // child would inherit that one anyway, but the conflict prompt this
        // dialog puts up is a `QMessageBox` -- and a box is only styled by
        // rules the sheet it inherits actually carries.  Carrying the sheet
        // here means the prompt is styled the same whether the editor was
        // opened from the window or built on its own.
        setStyleSheet(dialogStyleSheet());

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(10);

        auto *heading = new QLabel(shortcutBinding(action).label, this);
        heading->setObjectName(QStringLiteral("rowLabel"));
        layout->addWidget(heading);
        auto *hint = new QLabel(shortcutBinding(action).hint, this);
        hint->setObjectName(QStringLiteral("rowHint"));
        hint->setWordWrap(true);
        layout->addWidget(hint);

        record_ = new KeyCaptureButton(this);
        record_->setObjectName(QStringLiteral("shortcutRecord"));
        record_->setToolTip(uiTr("Click, then press the key"));
        record_->onKeysChanged = [this](const QKeySequence &keys) { record(keys); };
        layout->addWidget(record_);

        list_ = new QWidget(this);
        list_->setObjectName(QStringLiteral("shortcutList"));
        listLayout_ = new QVBoxLayout(list_);
        listLayout_->setContentsMargins(0, 0, 0, 0);
        listLayout_->setSpacing(6);
        layout->addWidget(list_);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        auto *close = buttons->addButton(uiTr("Done"), QDialogButtonBox::AcceptRole);
        close->setObjectName(QStringLiteral("shortcutDone"));
        // Cancel is the one that undoes: everything recorded so far is dropped
        // and the action goes back to the keys it opened with.
        connect(buttons, &QDialogButtonBox::rejected, this, [this] {
            *preferences_ = saved_;
            reject();
        });
        connect(close, &QPushButton::clicked, this, &QDialog::accept);
        layout->addWidget(buttons);

        refresh();
    }

    /// The dialog's own copy of the bindings, so a check can read back what a
    /// click left behind without a config file in the way.
    const ShortcutPreferences &preferences() const { return *preferences_; }

private:
    /// The ordinary conflict prompt: a modal message box whose default is No,
    /// so a stray Enter does not take a key off another action.
    bool askMove(const QString &question)
    {
        return QMessageBox::question(this, uiTr("Key already in use"), question,
                                     QMessageBox::Yes | QMessageBox::No,
                                     QMessageBox::No) == QMessageBox::Yes;
    }

    /// Takes one recorded key: dedupes it, asks about a conflict, and puts it
    /// on the list.
    void record(const QKeySequence &keys)
    {
        if (keys.isEmpty()) {
            // Backspace or Delete while armed: the whole binding goes, which is
            // the same answer the row on the page has always given.
            preferences_->setKeys(action_, QVector<QKeySequence>());
            refresh();
            return;
        }
        // Already this action's: the user pressed a key it already answers to.
        // Adding it again would leave two rows for one combination, each with
        // its own remove button, so it is taken as the no-op it is.
        if (preferences_->keysFor(action_).contains(keys)) {
            refresh();
            return;
        }
        const ShortcutAction owner = preferences_->ownerOtherThan(action_, keys);
        if (owner != ShortcutAction::kActionCount) {
            const QString question = uiTr("%1 is already bound to \"%2\". "
                                          "Move it to \"%3\"?")
                                         .arg(keys.toString(QKeySequence::PortableText),
                                              shortcutBinding(owner).label,
                                              shortcutBinding(action_).label);
            const bool move = ask_ ? ask_(question) : askMove(question);
            if (!move) {
                // Declined: nothing moves, and the key stays where it was.  The
                // button still has to stop showing it as the action's own.
                refresh();
                return;
            }
            preferences_->removeKey(owner, keys);
        }
        preferences_->addKey(action_, keys);
        refresh();
    }

    /// Rebuilds the list of keys and the record button from the preferences.
    void refresh()
    {
        while (QLayoutItem *item = listLayout_->takeAt(0)) {
            if (QWidget *widget = item->widget()) {
                // Reparented before it is dropped: `deleteLater` leaves the row
                // alive until the event loop comes back round, and a row that
                // is still a child of the list is a row `findChild` still hands
                // out -- which is a remove button that answers a click after
                // the key it belonged to is gone.
                widget->setParent(nullptr);
                widget->deleteLater();
            }
            delete item;
        }
        const QVector<QKeySequence> keys = preferences_->keysFor(action_);
        for (const QKeySequence &key : keys) {
            auto *row = new QWidget(list_);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(8);
            auto *label = new QLabel(key.toString(QKeySequence::PortableText), row);
            label->setObjectName(QStringLiteral("rowValue"));
            rowLayout->addWidget(label, 1);
            auto *remove = new QPushButton(uiTr("Remove"), row);
            remove->setObjectName(QStringLiteral("shortcutRemove"));
            remove->setCursor(Qt::PointingHandCursor);
            connect(remove, &QPushButton::clicked, this, [this, key] {
                preferences_->removeKey(action_, key);
                refresh();
            });
            rowLayout->addWidget(remove);
            listLayout_->addWidget(row);
        }
        if (keys.isEmpty()) {
            auto *empty = new QLabel(uiTr("None"), list_);
            empty->setObjectName(QStringLiteral("rowHint"));
            listLayout_->addWidget(empty);
        }
        // The record button keeps the *first* key as its own text: it is the
        // button the page's row shows, and a button that printed the whole list
        // would be wider than the row it sits in.
        record_->setKeys(keys.isEmpty() ? QKeySequence() : keys.constFirst());
    }

    ShortcutAction action_;
    ShortcutPreferences *preferences_;
    ShortcutPreferences saved_;
    std::function<bool(const QString &)> ask_;
    KeyCaptureButton *record_ = nullptr;
    QWidget *list_ = nullptr;
    QVBoxLayout *listLayout_ = nullptr;
};


/// One page of settings: a heading, a one-line explanation, and a stack of
/// cards.  Cards are added through [`addCard`], rows through [`addRow`].
///
/// The cards stack in one column rather than being spread into a grid.  The
/// rows put their label on the left and their control on the right, which needs
/// the full width to breathe: a hint under the label and a 200px combo box side
/// by side do not fit in half a page, and forcing them into two columns pushes
/// the page wider than its viewport instead of shorter.
QWidget *newPage(QScrollArea *scroll)
{
    auto *page = new QWidget(scroll);
    page->setObjectName(QStringLiteral("page"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(4, 2, 10, 14);
    layout->setSpacing(14);
    // The scroll area stretches the page to at least its viewport, and a page
    // shorter than that would otherwise have its spare height shared out
    // between the cards -- and, inside every card, between the rows -- which
    // spreads a three-card page's rows far apart.  The trailing stretch takes
    // all of the spare height instead, so a card stays as tall as its own rows
    // want.  Everything added to a page goes *before* this item: see the
    // `insertWidget` calls in [`addCard`] and [`addPageHeading`].
    layout->addStretch(1);
    scroll->setWidget(page);
    return page;
}

/// Adds a card to a page and returns the widget its rows go into.
QWidget *addCard(QWidget *page, const QString &heading)
{
    auto *column = new QWidget(page);
    auto *columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(8);

    if (!heading.isEmpty()) {
        auto *label = new QLabel(heading, column);
        label->setObjectName(QStringLiteral("rowHint"));
        columnLayout->addWidget(label);
    }

    auto *card = new QWidget(column);
    card->setObjectName(QStringLiteral("card"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(16, 4, 16, 4);
    cardLayout->setSpacing(0);
    columnLayout->addWidget(card);

    auto *pageLayout = qobject_cast<QVBoxLayout *>(page->layout());
    // Before the page's trailing stretch (see [`newPage`]), so the cards stack
    // from the top instead of being spread down the page.
    pageLayout->insertWidget(pageLayout->count() - 1, column);
    return card;
}

/// Adds one settings row: the label (and an optional explanation under it) on
/// the left, the control right-aligned on the right, a hairline above every row
/// but the first.
void addRow(QWidget *card, const QString &label, const QString &hint, QWidget *control,
            bool first)
{
    auto *cardLayout = qobject_cast<QVBoxLayout *>(card->layout());
    if (!first) {
        auto *divider = new QFrame(card);
        divider->setObjectName(QStringLiteral("rowDivider"));
        divider->setFixedHeight(1);
        cardLayout->addWidget(divider);
    }

    auto *row = new QWidget(card);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 6, 0, 6);
    rowLayout->setSpacing(16);

    auto *textColumn = new QVBoxLayout;
    textColumn->setContentsMargins(0, 0, 0, 0);
    textColumn->setSpacing(2);
    auto *name = new QLabel(label, row);
    name->setObjectName(QStringLiteral("rowLabel"));
    textColumn->addWidget(name);
    if (!hint.isEmpty()) {
        auto *sub = new QLabel(hint, row);
        sub->setObjectName(QStringLiteral("rowHint"));
        sub->setWordWrap(true);
        textColumn->addWidget(sub);
    }
    rowLayout->addLayout(textColumn, 1);

    control->setParent(row);
    // One height for every control in the window, so the spin boxes match the
    // combo boxes and line edits they sit beside (see kControlHeight).
    control->setFixedHeight(kControlHeight);
    rowLayout->addWidget(control, 0, Qt::AlignRight | Qt::AlignVCenter);
    cardLayout->addWidget(row);
}

/// Fills a microphone combo: silence and the session's own default input first,
/// then the inputs this session actually has, then -- when nothing is offering
/// it right now -- the name the file remembers.  A microphone that is merely
/// unplugged today must not be dropped from the file by opening and saving this
/// window.  Both the recording and the replay card carry one of these, so the
/// filling lives here once.
///
/// `inputs` is the probe's answer, which may be empty -- on the first fill it
/// has not landed yet, and on a machine with no PipeWire it never will.  An
/// empty list is not "no devices": it is "nothing to add", and the remembered
/// name is added back below either way, so a box filled before the answer
/// arrives still shows the device the file names.
///
/// `wanted` is the marker to keep selected: the box's current entry on a
/// re-detect, and the file's own value on the first fill.
void fillMicrophoneCombo(QComboBox *box, const QString &wanted,
                         const QList<QPair<QString, QString>> &inputs)
{
    box->clear();
    box->addItem(uiTr("Do not record audio"), kNoMicrophone);
    box->addItem(uiTr("The session's default input"), kDefaultMicrophone);
    for (const QPair<QString, QString> &input : inputs) {
        box->addItem(input.second, input.first);
        box->setItemData(box->count() - 1, input.first, Qt::ToolTipRole);
    }
    if (wanted != kNoMicrophone && wanted != kDefaultMicrophone && box->findData(wanted) < 0) {
        box->addItem(wanted, wanted);
        box->setItemData(box->count() - 1, wanted, Qt::ToolTipRole);
    }
    selectChoice(box, wanted);
}

/// A page heading and its explanation, above the first card.
void addPageHeading(QWidget *page, const QString &title, const QString &hint)
{
    auto *header = new QWidget(page);
    auto *layout = new QVBoxLayout(header);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    auto *titleLabel = new QLabel(title, header);
    titleLabel->setObjectName(QStringLiteral("pageTitle"));
    layout->addWidget(titleLabel);
    auto *hintLabel = new QLabel(hint, header);
    hintLabel->setObjectName(QStringLiteral("pageHint"));
    hintLabel->setWordWrap(true);
    layout->addWidget(hintLabel);
    auto *pageLayout = qobject_cast<QVBoxLayout *>(page->layout());
    // Before the page's trailing stretch, so the heading stays at the top.
    pageLayout->insertWidget(pageLayout->count() - 1, header);
}

/// The four rows that describe a shadow, on whichever card they are given.
///
/// A pin and a file dialog cast the same shadow -- the config file describes
/// both with one set of keys -- so the rows are built once and used twice
/// rather than written out twice and left to drift apart.  `prefix` is what
/// keeps their object names apart, which is what the offline check drives them
/// by; it also keeps the two sets of spin boxes from being wired to the same
/// member by a copy-paste.
struct ShadowControls {
    ModernSwitch *enabled = nullptr;
    ModernSpinBox *size = nullptr;
    ModernSpinBox *offset = nullptr;
    ModernSpinBox *opacity = nullptr;

    void build(QWidget *card, const ShadowStyle &shadow, const QString &prefix)
    {
        enabled = new ModernSwitch(card);
        enabled->setObjectName(prefix + QStringLiteral("Shadow"));
        enabled->setChecked(shadow.enabled);
        enabled->setToolTip(uiTr("Lifts it off whatever is behind it"));
        addRow(card, uiTr("Shadow"),
               uiTr("A soft shadow behind every one; turn it off for a hard edge"),
               enabled, true);

        size = new ModernSpinBox(card);
        size->setObjectName(prefix + QStringLiteral("ShadowSize"));
        size->setRange(0, static_cast<int>(kMaxShadowSize));
        size->setMinimumWidth(120);
        size->setValue(shadow.size);
        size->setSuffix(uiTr(" px"));
        addRow(card, uiTr("Shadow size"),
               uiTr("How far the blur reaches past the edge; 0 turns the blur off"),
               size, false);

        // The one control in this window that takes a negative number: the
        // offset is what says which way the light comes from, and a shadow
        // above the shape is a shadow cast by something below it.
        offset = new ModernSpinBox(card);
        offset->setObjectName(prefix + QStringLiteral("ShadowOffset"));
        offset->setRange(-static_cast<int>(kMaxShadowOffset),
                         static_cast<int>(kMaxShadowOffset));
        offset->setMinimumWidth(120);
        offset->setValue(shadow.offset);
        offset->setSuffix(uiTr(" px"));
        addRow(card, uiTr("Shadow offset"),
               uiTr("Drops the shadow below the edge; a negative value lifts it above"),
               offset, false);

        opacity = new ModernSpinBox(card);
        opacity->setObjectName(prefix + QStringLiteral("ShadowOpacity"));
        opacity->setRange(0, static_cast<int>(kMaxShadowOpacity));
        opacity->setMinimumWidth(120);
        opacity->setValue(shadow.opacity);
        addRow(card, uiTr("Shadow opacity"),
               uiTr("0-255; the blur spreads this rather than adding to it"),
               opacity, false);
    }

    ShadowStyle read() const
    {
        ShadowStyle shadow;
        shadow.enabled = enabled->isChecked();
        shadow.size = size->value();
        shadow.offset = offset->value();
        shadow.opacity = opacity->value();
        return shadow;
    }
};

/// The sidebar's section icons, drawn here rather than shipped as files, the
/// way the toolbar draws its own.
QIcon sectionIcon(int index, const QColor &color)
{
    constexpr qreal ratio = 2.0;
    constexpr int size = 18;
    QPixmap pixmap(static_cast<int>(size * ratio), static_cast<int>(size * ratio));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (index == 0) {
        // A pen: the annotation editor.
        painter.drawLine(QPointF(3.5, 14.5), QPointF(13.0, 5.0));
        painter.drawLine(QPointF(13.0, 5.0), QPointF(15.0, 7.0));
        painter.drawLine(QPointF(15.0, 7.0), QPointF(5.5, 16.5));
        painter.drawLine(QPointF(5.5, 16.5), QPointF(3.0, 17.0));
        painter.drawLine(QPointF(3.0, 17.0), QPointF(3.5, 14.5));
    } else if (index == 1) {
        // A framed picture with a corner turned down: the output, where a
        // screenshot is written.
        QPainterPath frame;
        frame.moveTo(3.0, 4.0);
        frame.lineTo(15.0, 4.0);
        frame.lineTo(15.0, 15.0);
        frame.lineTo(3.0, 15.0);
        frame.closeSubpath();
        painter.drawPath(frame);
        painter.drawLine(QPointF(3.0, 10.5), QPointF(7.5, 6.0));
        painter.drawLine(QPointF(7.5, 6.0), QPointF(11.0, 9.5));
    } else if (index == 2) {
        // A page with an arrow down it: the scrolling capture, which stitches
        // one long page out of many frames of a scroll.
        painter.drawLine(QPointF(4.0, 3.5), QPointF(14.0, 3.5));
        painter.drawLine(QPointF(4.0, 15.0), QPointF(14.0, 15.0));
        painter.drawLine(QPointF(9.0, 5.5), QPointF(9.0, 13.0));
        painter.drawLine(QPointF(9.0, 13.0), QPointF(6.5, 10.5));
        painter.drawLine(QPointF(9.0, 13.0), QPointF(11.5, 10.5));
    } else if (index == 3) {
        // A pair of brackets around two short lines: text recognition, which
        // reads the words out of the picture.
        painter.drawLine(QPointF(4.0, 3.5), QPointF(4.0, 6.5));
        painter.drawLine(QPointF(4.0, 3.5), QPointF(7.0, 3.5));
        painter.drawLine(QPointF(14.0, 3.5), QPointF(14.0, 6.5));
        painter.drawLine(QPointF(14.0, 3.5), QPointF(11.0, 3.5));
        painter.drawLine(QPointF(4.0, 14.5), QPointF(4.0, 11.5));
        painter.drawLine(QPointF(4.0, 14.5), QPointF(7.0, 14.5));
        painter.drawLine(QPointF(14.0, 14.5), QPointF(14.0, 11.5));
        painter.drawLine(QPointF(14.0, 14.5), QPointF(11.0, 14.5));
        painter.drawLine(QPointF(6.5, 7.5), QPointF(11.5, 7.5));
        painter.drawLine(QPointF(6.5, 10.5), QPointF(9.5, 10.5));
    } else if (index == 4) {
        // A lens with a dot at its centre: recording, one window or one
        // output, which is what this section's defaults are for.
        painter.drawEllipse(QPointF(9.0, 9.0), 5.0, 5.0);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(9.0, 9.0), 1.8, 1.8);
        painter.setBrush(Qt::NoBrush);
    } else if (index == 5) {
        // A window with a rounded top-left corner and a title strip: the
        // file dialog's own shape, which is what this section configures.
        QPainterPath frame;
        frame.moveTo(3.0, 8.0);
        frame.quadTo(3.0, 3.0, 8.0, 3.0);
        frame.lineTo(15.0, 3.0);
        frame.lineTo(15.0, 15.0);
        frame.lineTo(3.0, 15.0);
        frame.closeSubpath();
        painter.drawPath(frame);
        painter.drawLine(QPointF(3.0, 7.0), QPointF(15.0, 7.0));
    } else if (index == 6) {
        // A pushpin: the pin overlay, which is what this section configures.
        painter.drawLine(QPointF(9.0, 3.0), QPointF(15.0, 3.0));
        painter.drawLine(QPointF(12.0, 3.0), QPointF(12.0, 8.5));
        painter.drawLine(QPointF(12.0, 8.5), QPointF(15.0, 11.0));
        painter.drawLine(QPointF(15.0, 11.0), QPointF(9.0, 11.0));
        painter.drawLine(QPointF(9.0, 11.0), QPointF(9.0, 16.0));
    } else {
        // Three keys with a fourth pressed: the keyboard bindings.
        painter.drawLine(QPointF(3.0, 3.5), QPointF(15.0, 3.5));
        painter.drawLine(QPointF(3.0, 14.5), QPointF(15.0, 14.5));
        painter.drawLine(QPointF(3.0, 3.5), QPointF(3.0, 14.5));
        painter.drawLine(QPointF(15.0, 3.5), QPointF(15.0, 14.5));
        painter.drawLine(QPointF(6.5, 3.5), QPointF(6.5, 14.5));
        painter.drawLine(QPointF(11.5, 3.5), QPointF(11.5, 14.5));
        painter.setBrush(color);
        painter.drawRect(QRectF(4.0, 6.0, 2.0, 6.0));
        painter.setBrush(Qt::NoBrush);
    }
    return QIcon(pixmap);
}

/// The window itself.  It owns nothing but the widgets; the config is read once
/// when it opens and written once when Save is pressed, so a change made by
/// hand in the file while it is open is not silently reverted by a Cancel.
class SettingsDialog final : public QDialog {
public:
    /// `ownedShortcuts` is null for the ordinary window, which reads and writes
    /// the file; a caller that passes one gets the same pages over bindings of
    /// its own, which is what lets the offline check read the multi-key editor
    /// back without a config file standing in for it.
    explicit SettingsDialog(ShortcutPreferences *ownedShortcuts = nullptr)
        : config_(loadConfig())
        , shortcuts_(ownedShortcuts != nullptr ? *ownedShortcuts : loadShortcutPreferences())
        , shortcutsOwner_(ownedShortcuts)
    {
        setWindowTitle(uiTr("vshot settings"));
        setStyleSheet(dialogStyleSheet());
        // Sized so the taller of the two pages (the editor, at 768px of
        // content) opens without a scrollbar, while still fitting a 1080p
        // screen with room for a title bar.  The scroll area stays for the
        // smaller screens and for the longer translations.
        setMinimumSize(700, 520);
        resize(880, 890);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        root->addWidget(buildHeader());

        auto *body = new QWidget(this);
        auto *bodyLayout = new QHBoxLayout(body);
        bodyLayout->setContentsMargins(16, 0, 16, 0);
        bodyLayout->setSpacing(18);

        sidebar_ = new QListWidget(body);
        sidebar_->setObjectName(QStringLiteral("sidebar"));
        sidebar_->setFrameShape(QFrame::NoFrame);
        sidebar_->setFixedWidth(184);
        sidebar_->setFocusPolicy(Qt::NoFocus);
        sidebar_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        sidebar_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        // One page per feature rather than one page per kind of setting.  The
        // command-line defaults used to share a single page -- output, pin
        // density, scrolling capture, recognition and the two recording cards
        // -- which meant that finding a recording option meant scrolling past
        // three unrelated cards, and that the page's heading had to describe
        // all of them at once.  The section icons are indexed by row, so the
        // order here and in [`sectionIcon`] is the same order.
        const QStringList sections = {
            uiTr("Annotation editor"),
            uiTr("Output"),
            uiTr("Scrolling capture"),
            uiTr("Text recognition"),
            uiTr("Recording"),
            uiTr("File dialogs"),
            uiTr("Pin appearance"),
            uiTr("Keyboard"),
        };
        for (int row = 0; row < sections.size(); ++row) {
            sidebar_->addItem(new QListWidgetItem(sectionIcon(row, QColor(kInkDim)),
                                                  sections.at(row)));
        }
        bodyLayout->addWidget(sidebar_, 0);

        pages_ = new QStackedWidget(body);
        // Named so the offline check can walk the pages and say which settings
        // landed on which one; the sidebar's own item text is what it compares
        // them against.
        pages_->setObjectName(QStringLiteral("pages"));
        // Added in the sidebar's order: the row index is the page index, which
        // is what `currentRowChanged` below relies on.
        pages_->addWidget(buildEditorPage());
        pages_->addWidget(buildOutputPage());
        pages_->addWidget(buildScrollingPage());
        pages_->addWidget(buildRecognitionPage());
        pages_->addWidget(buildRecordingPage());
        pages_->addWidget(buildDialogPage());
        pages_->addWidget(buildPinPage());
        pages_->addWidget(buildKeyboardPage());
        bodyLayout->addWidget(pages_, 1);

        connect(sidebar_, &QListWidget::currentRowChanged, this, [this](int row) {
            if (row >= 0 && row < pages_->count()) {
                pages_->setCurrentIndex(row);
            }
        });
        sidebar_->setCurrentRow(0);

        root->addWidget(body, 1);
        root->addWidget(buildFooter());

        // Last, and queued rather than called: the pages are up and readable
        // without the input listing, and asking for it here would block the
        // constructor on a second process for about a second.  Queued so the
        // window is on screen before the probe starts, which is what makes the
        // two boxes fill in visibly rather than after a pause.
        QTimer::singleShot(0, this, [this] { askForMicrophones(); });
    }

private:
    QWidget *buildHeader()
    {
        auto *header = new QWidget(this);
        auto *layout = new QVBoxLayout(header);
        layout->setContentsMargins(22, 18, 22, 14);
        layout->setSpacing(3);

        auto *title = new QLabel(uiTr("Settings"), header);
        title->setObjectName(QStringLiteral("windowTitle"));
        layout->addWidget(title);

        const QString path = configFilePath();
        auto *hint = new QLabel(
            path.isEmpty()
                ? uiTr("There is no config directory, so nothing can be saved.")
                : uiTr("Saved to %1").arg(QDir::toNativeSeparators(path)),
            header);
        hint->setObjectName(QStringLiteral("windowHint"));
        hint->setWordWrap(true);
        layout->addWidget(hint);
        return header;
    }

    QWidget *buildFooter()
    {
        auto *footer = new QWidget(this);
        auto *layout = new QHBoxLayout(footer);
        layout->setContentsMargins(22, 12, 22, 18);
        layout->setSpacing(12);

        status_ = new QLabel(footer);
        status_->setObjectName(QStringLiteral("status"));
        status_->setWordWrap(true);
        layout->addWidget(status_, 1);

        auto *cancel = new QPushButton(uiTr("Cancel"), footer);
        cancel->setObjectName(QStringLiteral("cancelButton"));
        cancel->setCursor(Qt::PointingHandCursor);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        layout->addWidget(cancel, 0);

        auto *save = new QPushButton(uiTr("Save"), footer);
        save->setObjectName(QStringLiteral("saveButton"));
        save->setCursor(Qt::PointingHandCursor);
        save->setDefault(true);
        connect(save, &QPushButton::clicked, this, &SettingsDialog::save);
        layout->addWidget(save, 0);
        return footer;
    }

    QScrollArea *newScrollPage(QStackedWidget *stack)
    {
        auto *scroll = new QScrollArea(stack);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFrameShape(QFrame::NoFrame);
        // The viewport is a widget of its own, painted from the palette's Base
        // role, which in a dark palette is pure black -- and a stylesheet's
        // `background: transparent` on the scroll area does not reach it.  The
        // scrollbar's own track is drawn by the style from the same role, so
        // both are repainted from the palette here rather than from the sheet.
        QPalette palette = scroll->palette();
        palette.setColor(QPalette::Base, QColor(kWindowBg));
        palette.setColor(QPalette::Window, QColor(kWindowBg));
        palette.setColor(QPalette::Button, QColor(kWindowBg));
        scroll->setPalette(palette);
        scroll->viewport()->setPalette(palette);
        scroll->viewport()->setAutoFillBackground(true);
        // The scrollbars are children of the scroll area rather than of the
        // viewport, and the stylesheet leaves the 2px margin around the groove
        // unpainted; without their own palette that margin shows the palette's
        // Base, which is black.  Setting it here fills the whole strip.
        for (QScrollBar *bar : {scroll->verticalScrollBar(), scroll->horizontalScrollBar()}) {
            bar->setPalette(palette);
            bar->setAutoFillBackground(true);
        }
        return scroll;
    }

    QWidget *buildEditorPage()
    {
        QScrollArea *scroll = newScrollPage(pages_);
        QWidget *page = newPage(scroll);
        addPageHeading(page, uiTr("Annotation editor"),
                       uiTr("The style the toolbar opens with next time. Leaving the editor "
                            "writes this back, whether or not the capture went through."));

        // The armed tool and the colour it draws in -- the two things a user
        // reaches for before every capture -- in one card, under a heading of
        // their own so the card says what it holds. A card with no heading and
        // three unrelated rows in it is what this used to be.
        QWidget *shape = addCard(page, uiTr("Tool"));
        toolBox_ = new ModernComboBox(shape);
        toolBox_->setObjectName(QStringLiteral("tool"));
        toolBox_->setMinimumWidth(180);
        // Nothing armed is the first entry, and the value a config file written
        // before the Select tool went away carries: the two have to land on the
        // same state, so the empty value is spelled here as it is stored.
        toolBox_->addItem(uiTr("No tool"), QString());
        for (const QString &value : toolNames()) {
            toolBox_->addItem(value, value);
        }
        selectChoice(toolBox_, config_.editor.tool);
        addRow(shape, uiTr("Opening tool"),
               uiTr("The tool editing starts with; session changes are not saved here"),
               toolBox_, true);

        selectModeBox_ = new ModernComboBox(shape);
        selectModeBox_->setObjectName(QStringLiteral("selectMode"));
        selectModeBox_->setMinimumWidth(180);
        for (const QString &value : selectModeNames()) {
            selectModeBox_->addItem(value, value);
        }
        selectChoice(selectModeBox_, config_.editor.selectMode);
        addRow(shape, uiTr("Select tool drags"),
               uiTr("precise presses the mark itself; loose drags a selected mark from anywhere"),
               selectModeBox_, false);

        colorButton_ = new ColorButton(shape);
        colorButton_->setColor(config_.editor.color);
        addRow(shape, uiTr("Color"), uiTr("Hex, with alpha last when it is not opaque"),
               colorButton_, false);

        QWidget *stroke = addCard(page, uiTr("Stroke"));
        widthSpin_ = new ModernSpinBox(stroke);
        widthSpin_->setObjectName(QStringLiteral("width"));
        widthSpin_->setRange(1, 64);
        widthSpin_->setValue(static_cast<int>(config_.editor.width));
        widthSpin_->setMinimumWidth(120);
        addRow(stroke, uiTr("Width"), uiTr("1-64 logical pixels"), widthSpin_, true);

        dashBox_ = new ModernComboBox(stroke);
        dashBox_->setObjectName(QStringLiteral("dash"));
        dashBox_->setMinimumWidth(180);
        for (const QString &value : dashNames()) {
            dashBox_->addItem(value, value);
        }
        selectChoice(dashBox_, config_.editor.dash);
        addRow(stroke, uiTr("Line style"), QString(), dashBox_, false);

        QWidget *arrow = addCard(page, uiTr("Arrow"));
        arrowSizeSpin_ = new ModernSpinBox(arrow);
        arrowSizeSpin_->setObjectName(QStringLiteral("arrowSize"));
        arrowSizeSpin_->setRange(1, 8);
        arrowSizeSpin_->setValue(static_cast<int>(config_.editor.arrowSize));
        arrowSizeSpin_->setMinimumWidth(120);
        addRow(arrow, uiTr("Head size"), uiTr("1-8"), arrowSizeSpin_, true);

        arrowStyleBox_ = new ModernComboBox(arrow);
        arrowStyleBox_->setObjectName(QStringLiteral("arrowStyle"));
        arrowStyleBox_->setMinimumWidth(180);
        for (const QString &value : arrowStyleNames()) {
            arrowStyleBox_->addItem(value, value);
        }
        selectChoice(arrowStyleBox_, config_.editor.arrowStyle);
        addRow(arrow, uiTr("Head style"), QString(), arrowStyleBox_, false);

        QWidget *text = addCard(page, uiTr("Text"));
        textSizeSpin_ = new ModernSpinBox(text);
        textSizeSpin_->setObjectName(QStringLiteral("textSize"));
        textSizeSpin_->setRange(kMinTextPixels, kMaxTextPixels);
        textSizeSpin_->setValue(clampTextPixels(static_cast<int>(config_.editor.textSize)));
        textSizeSpin_->setMinimumWidth(120);
        addRow(text, uiTr("Size"), uiTr("Font height in pixels (7-448)"), textSizeSpin_, true);

        fontBox_ = new ModernComboBox(text);
        fontBox_->setObjectName(QStringLiteral("font"));
        fontBox_->setEditable(false);
        fontBox_->setMinimumWidth(220);
        fontBox_->addItem(uiTr("System default"), QString());
        for (const QString &family : QFontDatabase::families()) {
            fontBox_->addItem(family, family);
        }
        // A family the file names but this machine does not have is kept as an
        // entry of its own rather than silently reset: the config may well be
        // shared with a machine that does have it, and losing the name on a
        // save would be a surprise.
        if (!config_.editor.font.isEmpty() && fontBox_->findData(config_.editor.font) < 0) {
            fontBox_->addItem(config_.editor.font, config_.editor.font);
        }
        selectChoice(fontBox_, config_.editor.font);
        addRow(text, uiTr("Font"), QString(), fontBox_, false);

        QWidget *mosaic = addCard(page, uiTr("Mosaic"));
        mosaicShapeBox_ = new ModernComboBox(mosaic);
        mosaicShapeBox_->setObjectName(QStringLiteral("mosaicShape"));
        mosaicShapeBox_->setMinimumWidth(180);
        for (const QString &value : mosaicShapeNames()) {
            mosaicShapeBox_->addItem(value, value);
        }
        selectChoice(mosaicShapeBox_, config_.editor.mosaicShape);
        addRow(mosaic, uiTr("Shape"), QString(), mosaicShapeBox_, true);

        mosaicStrengthSpin_ = new ModernSpinBox(mosaic);
        mosaicStrengthSpin_->setObjectName(QStringLiteral("mosaicStrength"));
        mosaicStrengthSpin_->setRange(1, 3);
        mosaicStrengthSpin_->setValue(static_cast<int>(config_.editor.mosaicStrength));
        mosaicStrengthSpin_->setMinimumWidth(120);
        addRow(mosaic, uiTr("Strength"), uiTr("1-3"), mosaicStrengthSpin_, false);

        return scroll;
    }

    /// What the config remembers as the microphone, in the box's own terms.
    QString rememberedMicrophone() const
    {
        return microphoneMarker(config_.cli.recordMicEnabled, config_.cli.recordMic);
    }

    /// Fills the recording card's microphone row.  The config is read through
    /// [`rememberedMicrophone`]; rebuilding from the box rather than the config
    /// is what makes re-detecting midway through an edit keep the user's choice.
    ///
    /// The inputs come from the probe, which may not have answered yet: the row
    /// is filled at once with what is known, and filled again when the listing
    /// lands (see [`askForMicrophones`]).
    void fillMicrophoneBox()
    {
        const QString shown = recordMicBox_->currentData().toString();
        fillMicrophoneCombo(recordMicBox_, shown.isEmpty() ? rememberedMicrophone() : shown,
                            microphones_.inputs());
    }

    QString rememberedReplayMicrophone() const
    {
        return microphoneMarker(config_.cli.replayMicEnabled, config_.cli.replayMic);
    }

    /// Fills the replay card's microphone row, on the same terms as the
    /// recording's.
    void fillReplayMicrophoneBox()
    {
        const QString shown = replayMicBox_->currentData().toString();
        fillMicrophoneCombo(replayMicBox_,
                            shown.isEmpty() ? rememberedReplayMicrophone() : shown,
                            microphones_.inputs());
    }

    /// Asks the session for its inputs, and fills both rows again when the
    /// answer lands.  `again` is what the Detect button passes: the point of
    /// pressing it is that something changed, so an answer already in is the one
    /// the user is asking to replace.
    ///
    /// The first call is made after the window is up rather than while it is
    /// being built: the command takes about a second, and the rows are readable
    /// without it -- they open on the two answers that always exist and the name
    /// the file remembers -- so waiting for it before showing anything bought
    /// nothing.
    void askForMicrophones(bool again = false)
    {
        // Both rows are refilled from the box, so a choice made while the probe
        // was out is kept rather than reset to the file's value.
        microphones_.onFinished = [this] {
            fillMicrophoneBox();
            fillReplayMicrophoneBox();
        };
        if (again) {
            microphones_.restart();
        } else {
            microphones_.start();
        }
    }

    /// Whether the white-level box does anything for the mode now selected.
    ///
    /// `auto` and `fixed` both read a level; `normalize` works its own out from
    /// the capture's peak, so the box is greyed rather than left live and
    /// ignored.  The leading "built-in default" entry means `auto`, which does
    /// read one.
    void updateToneMapWhiteEnabled()
    {
        const QString mode = toneMapBox_->currentData().toString();
        toneMapWhiteSpin_->setEnabled(mode != QStringLiteral("normalize"));
    }

    /// Whether the area ratio does anything for the switch's position.
    ///
    /// With the area test off there is no share to weigh -- the one-pixel rule
    /// stands -- so the box is greyed rather than left live and ignored.
    void updateHdrAreaRatioEnabled()
    {
        hdrAreaRatioSpin_->setEnabled(hdrAreaSwitch_->isChecked());
    }

    QWidget *buildOutputPage()
    {
        QScrollArea *scroll = newScrollPage(pages_);
        QWidget *page = newPage(scroll);
        addPageHeading(page, uiTr("Output"),
                       uiTr("Where a screenshot is written. Used only where the command line "
                            "gives nothing: an argument, or an environment variable, always "
                            "wins over these."));

        // One card per subject, because a subject is what a user opens the page
        // for: how the PNG is written, how an HDR capture is split into two
        // files, and which output a capture takes when the command line names
        // none. Under one card they were told apart by a rule and a bold label
        // between rows that looked like every other row; a card apiece gives
        // each subject an edge of its own. They are ordered by how often each is
        // the reason someone opened this page.
        QWidget *output = addCard(page, uiTr("PNG"));
        compressionBox_ =
            choiceBox(output, compressionNames(), QString::fromLatin1(kDefaultPngCompression));
        compressionBox_->setObjectName(QStringLiteral("pngCompression"));
        compressionBox_->setMinimumWidth(200);
        selectChoice(compressionBox_, config_.cli.pngCompression);
        addRow(output, uiTr("PNG compression"),
               uiTr("All levels are lossless; slower ones buy a smaller file"),
               compressionBox_, true);

        output = addCard(page, uiTr("HDR"));
        hdrFormatBox_ =
            choiceBox(output, hdrFormatNames(), QString::fromLatin1(kDefaultHdrFormat));
        hdrFormatBox_->setObjectName(QStringLiteral("hdrFormat"));
        hdrFormatBox_->setMinimumWidth(200);
        selectChoice(hdrFormatBox_, config_.cli.hdrFormat);
        addRow(output, uiTr("HDR format"),
               uiTr("The second file of a capture that carries HDR content, written "
                    "beside the PNG with the same name. AVIF is ten-bit BT.2020 PQ and "
                    "says so in the file, so every reader shows it right, but it is "
                    "lossy; Radiance RGBE is the light exactly as captured, and is read "
                    "by few"),
               hdrFormatBox_, true);

        toneMapBox_ = choiceBox(output, toneMapNames(), QString::fromLatin1(kDefaultToneMap));
        toneMapBox_->setObjectName(QStringLiteral("toneMap"));
        toneMapBox_->setMinimumWidth(200);
        selectChoice(toneMapBox_, config_.cli.toneMap);
        addRow(output, uiTr("HDR to SDR"),
               uiTr("How the SDR half of an HDR capture is made from the HDR one. "
                    "Auto reads each capture: an SDR picture comes out exactly as it "
                    "was, and one with highlights makes room for them. Fixed always "
                    "maps SDR white to the level below, so a pixel's value does not "
                    "depend on what else is in the picture. Normalize scales the "
                    "capture so its brightest point becomes white"),
               toneMapBox_, false);

        toneMapWhiteSpin_ = new ModernDoubleSpinBox(output);
        toneMapWhiteSpin_->setObjectName(QStringLiteral("toneMapWhite"));
        // The map works in fractions and the box shows percentages, so the two
        // are converted on the way in and on the way out -- including the span,
        // which is why it is scaled here rather than taken as it comes.
        toneMapWhiteSpin_->setRange(kMinToneMapWhite * 100.0, kMaxToneMapWhite * 100.0);
        toneMapWhiteSpin_->setSuffix(uiTr(" %"));
        toneMapWhiteSpin_->setMinimumWidth(120);
        toneMapWhiteSpin_->setValue(
            (config_.cli.toneMapWhite > 0.0 ? config_.cli.toneMapWhite : kDefaultToneMapWhite) *
            100.0);
        addRow(output, uiTr("SDR white level"),
               uiTr("Where SDR white lands in the range, as a percentage. The rest is "
                    "spent on light above white, so a lower level keeps highlights more "
                    "apart and makes the picture dimmer. Used by Auto (only for a "
                    "capture that has highlights) and by Fixed"),
               toneMapWhiteSpin_, false);
        // A level is only read by two of the three modes, and Normalize works
        // its own out from the capture's peak: a box that did nothing would
        // read as a setting that was ignored.
        connect(toneMapBox_, &QComboBox::currentIndexChanged, this,
                [this] { updateToneMapWhiteEnabled(); });
        updateToneMapWhiteEnabled();

        hdrAreaSwitch_ = new ModernSwitch(output);
        hdrAreaSwitch_->setObjectName(QStringLiteral("hdrAreaTest"));
        hdrAreaSwitch_->setChecked(config_.cli.hdrAreaTest);
        hdrAreaSwitch_->setToolTip(
            uiTr("Off: one bright pixel is enough. On: the ratio below has to be met"));
        addRow(output, uiTr("Judge HDR by area"),
               uiTr("Whether a capture counts as HDR content by how much of it is brighter "
                    "than SDR white rather than by any single pixel. A ten-bit PQ screen "
                    "rounds ordinary SDR white a few thousandths over, so with this off a "
                    "handful of rounding pixels can pass a whole desktop off as HDR and dim "
                    "it. Only outputs the compositor describes as HDR are asked at all"),
               hdrAreaSwitch_, false);

        hdrAreaRatioSpin_ = new ModernDoubleSpinBox(output);
        hdrAreaRatioSpin_->setObjectName(QStringLiteral("hdrAreaRatio"));
        hdrAreaRatioSpin_->setDecimals(4);
        hdrAreaRatioSpin_->setSingleStep(0.0005);
        // The ratio is a share of the frame, shown as a percentage of it: the
        // box's own arithmetic is in fractions, but "0.05 %" is what a user can
        // picture.  Four decimals of a percent is the resolution the built-in
        // default needs.
        hdrAreaRatioSpin_->setRange(0.0, 100.0);
        hdrAreaRatioSpin_->setSuffix(uiTr(" %"));
        hdrAreaRatioSpin_->setMinimumWidth(120);
        hdrAreaRatioSpin_->setValue((config_.cli.hdrAreaRatio >= 0.0 ? config_.cli.hdrAreaRatio
                                                                     : kDefaultHdrAreaRatio) *
                                    100.0);
        addRow(output, uiTr("HDR area"),
               uiTr("How much of the capture has to be brighter than SDR white to count as "
                    "HDR content, as a percentage of it. Zero means every capture of an HDR "
                    "output is HDR content, with no test at all"),
               hdrAreaRatioSpin_, false);
        // A ratio is only read when the switch above is on, so a live box under
        // an off switch would read as a setting that was ignored.
        connect(hdrAreaSwitch_, &QAbstractButton::toggled, this,
                [this] { updateHdrAreaRatioEnabled(); });
        updateHdrAreaRatioEnabled();

        output = addCard(page, uiTr("Which output"));
        monitorEdit_ = new QLineEdit(output);
        monitorEdit_->setObjectName(QStringLiteral("monitor"));
        monitorEdit_->setMinimumWidth(220);
        monitorEdit_->setPlaceholderText(uiTr("follow the pointer"));
        monitorEdit_->setText(config_.cli.monitor);
        addRow(output, uiTr("Default monitor"),
               uiTr("Which output a capture takes when the command line names none. Leave it "
                    "empty to use whichever output the pointer is on -- `current` says the "
                    "same thing -- or write a name like `eDP-1` to pin one down"),
               monitorEdit_, true);

        return scroll;
    }

    QWidget *buildScrollingPage()
    {
        QScrollArea *scroll = newScrollPage(pages_);
        QWidget *page = newPage(scroll);
        addPageHeading(page, uiTr("Scrolling capture"),
                       uiTr("Defaults for the long scrolling capture. Used only where the "
                            "command line gives nothing: an argument, or an environment "
                            "variable, always wins over these."));

        // How the page is scrolled, then how the frames are stitched: the first
        // is what a user changes when a capture comes back wrong, the second
        // when one comes back short. A card each, so the two read as the
        // separate subjects they are.
        QWidget *scrolling = addCard(page, uiTr("Scrolling"));
        notchesSpin_ = optionalSpin(scrolling, kDefaultLongNotches, kMaxFrameValue, QString());
        notchesSpin_->setObjectName(QStringLiteral("longNotches"));
        notchesSpin_->setMinimumWidth(120);
        notchesSpin_->setValue(rememberedOr(config_.cli.longNotches, kDefaultLongNotches));
        addRow(scrolling, uiTr("Scroll notches"),
               uiTr("Wheel notches sent at a time"), notchesSpin_, true);

        maxHeightSpin_ = optionalSpin(scrolling, kDefaultLongMaxHeight, kMaxFrameValue, uiTr(" px"));
        maxHeightSpin_->setObjectName(QStringLiteral("longMaxHeight"));
        maxHeightSpin_->setMinimumWidth(120);
        maxHeightSpin_->setValue(rememberedOr(config_.cli.longMaxHeight, kDefaultLongMaxHeight));
        addRow(scrolling, uiTr("Max height"), QString(), maxHeightSpin_, false);

        maxFramesSpin_ = optionalSpin(scrolling, kDefaultLongMaxFrames, kMaxFrameValue, QString());
        maxFramesSpin_->setObjectName(QStringLiteral("longMaxFrames"));
        maxFramesSpin_->setMinimumWidth(120);
        maxFramesSpin_->setValue(rememberedOr(config_.cli.longMaxFrames, kDefaultLongMaxFrames));
        addRow(scrolling, uiTr("Max frames"), QString(), maxFramesSpin_, false);

        timeoutSpin_ = optionalSpin(scrolling, kDefaultLongTimeout, kMaxFrameValue, uiTr(" s"));
        timeoutSpin_->setObjectName(QStringLiteral("longTimeout"));
        timeoutSpin_->setMinimumWidth(120);
        timeoutSpin_->setValue(rememberedOr(config_.cli.longTimeout, kDefaultLongTimeout));
        addRow(scrolling, uiTr("Timeout"), QString(), timeoutSpin_, false);

        scrolling = addCard(page, uiTr("Stitching"));
        ignoreTopSpin_ = optionalSpin(scrolling, kDefaultLongIgnoreTop, kMaxFrameValue, uiTr(" px"));
        ignoreTopSpin_->setObjectName(QStringLiteral("longIgnoreTop"));
        ignoreTopSpin_->setMinimumWidth(120);
        ignoreTopSpin_->setValue(rememberedOr(config_.cli.longIgnoreTop, kDefaultLongIgnoreTop));
        addRow(scrolling, uiTr("Ignore top"),
               uiTr("Rows at the top of every frame left out of the match, for sticky "
                    "headers"),
               ignoreTopSpin_, false);

        injectBox_ = choiceBox(scrolling, injectNames(), QString::fromLatin1(kDefaultLongInject));
        injectBox_->setObjectName(QStringLiteral("longInject"));
        injectBox_->setMinimumWidth(200);
        selectChoice(injectBox_, config_.cli.longInject);
        addRow(scrolling, uiTr("Scroll backend"), QString(), injectBox_, false);

        return scroll;
    }

    QWidget *buildRecognitionPage()
    {
        QScrollArea *scroll = newScrollPage(pages_);
        QWidget *page = newPage(scroll);
        addPageHeading(page, uiTr("Text recognition"),
                       uiTr("What happens once the text is out. Used only where the command "
                            "line gives nothing: an argument, or an environment variable, "
                            "always wins over these."));

        // The OCR engine itself is not here -- it is a command and a timeout,
        // which the README documents for hand-editing -- but the notification
        // is: it is the part of the feature a user wants to change *after*
        // seeing it work, and that is what this window is for.
        QWidget *recognition = addCard(page, QString());
        ocrNotifySwitch_ = new ModernSwitch(recognition);
        ocrNotifySwitch_->setObjectName(QStringLiteral("ocrNotify"));
        ocrNotifySwitch_->setChecked(config_.cli.ocrNotify);
        ocrNotifySwitch_->setToolTip(uiTr("Shown with the result once recognition ends"));
        addRow(recognition, uiTr("Notify when the text is ready"),
               uiTr("A desktop notification with the text, or with why it failed; it "
                    "needs a notification daemon"),
               ocrNotifySwitch_, true);

        return scroll;
    }

    QWidget *buildRecordingPage()
    {
        QScrollArea *scroll = newScrollPage(pages_);
        QWidget *page = newPage(scroll);
        addPageHeading(page, uiTr("Recording"),
                       uiTr("Defaults for `record` and `replay`. Used only where the command "
                            "line gives nothing: an argument, or an environment variable, "
                            "always wins over these."));
        // Recording is the one card whose control asks the running session a
        // question -- which audio inputs it has -- so the answer is what the
        // row offers: a name has to be a PipeWire node's own, and nobody can
        // type one of those from memory.
        // Three subjects, in the order a user meets them: what the encoder does,
        // then what is recorded -- the source and the sound, which are the rows
        // a user changes between two recordings -- then the receipt at the end,
        // which is set once and never looked at again. A card each, so the
        // heading is the card's own rather than a label between rows.
        QWidget *recording = addCard(page, uiTr("Encoder"));
        recordEncoderBox_ =
            choiceBox(recording, encoderNames(), QString::fromLatin1(kDefaultEncoder));
        recordEncoderBox_->setObjectName(QStringLiteral("recordEncoder"));
        recordEncoderBox_->setMinimumWidth(200);
        selectChoice(recordEncoderBox_, config_.cli.recordEncoder);
        addRow(recording, uiTr("Encoder"),
               uiTr("All three encode on the GPU's media engine"),
               recordEncoderBox_, true);

        recordEncoderBackendBox_ = choiceBox(recording, encoderBackendNames(),
                                             QString::fromLatin1(kDefaultEncoderBackend));
        recordEncoderBackendBox_->setObjectName(QStringLiteral("recordEncoderBackend"));
        recordEncoderBackendBox_->setMinimumWidth(200);
        selectChoice(recordEncoderBackendBox_, config_.cli.recordEncoderBackend);
        addRow(recording, uiTr("Hardware encoder"),
               uiTr("auto tries VAAPI then Vulkan then NVENC; all three encode on the GPU "
                    "(VAAPI and Vulkan import the dma-buf, NVENC copies frames via the CPU)"),
               recordEncoderBackendBox_, false);

        recordFpsSpin_ = optionalSpin(recording, kDefaultRecordFps, kMaxFrameValue, uiTr(" fps"));
        recordFpsSpin_->setObjectName(QStringLiteral("recordFps"));
        recordFpsSpin_->setMinimumWidth(120);
        recordFpsSpin_->setValue(rememberedOr(config_.cli.recordFps, kDefaultRecordFps));
        addRow(recording, uiTr("Frame rate"), uiTr("1-240"), recordFpsSpin_, false);

        recording = addCard(page, uiTr("What is recorded"));
        recordFollowEdit_ = new QLineEdit(recording);
        recordFollowEdit_->setObjectName(QStringLiteral("recordFollow"));
        recordFollowEdit_->setMinimumWidth(240);
        recordFollowEdit_->setPlaceholderText(uiTr("no windows to follow"));
        recordFollowEdit_->setText(followText(config_.cli.recordFollow));
        addRow(recording, uiTr("Follow the focus"),
               uiTr("Window names, comma-separated (`record window` with no NAME); the "
                    "recording moves to whichever the focus lands on"),
               recordFollowEdit_, true);

        recordPortalSwitch_ = new ModernSwitch(recording);
        recordPortalSwitch_->setObjectName(QStringLiteral("recordPortal"));
        recordPortalSwitch_->setChecked(config_.cli.recordPortal);
        recordPortalSwitch_->setToolTip(
            uiTr("The compositor's own picker decides what is recorded"));
        addRow(recording, uiTr("Through the desktop portal"),
               uiTr("Needs xdg-desktop-portal and libpipewire; `record all` cannot use it"),
               recordPortalSwitch_, false);

        recordMicBox_ = new ModernComboBox(recording);
        recordMicBox_->setObjectName(QStringLiteral("recordMic"));
        recordMicBox_->setMinimumWidth(240);
        recordMicDetectButton_ = new QPushButton(uiTr("Detect"), recording);
        recordMicDetectButton_->setObjectName(QStringLiteral("recordMicDetect"));
        recordMicDetectButton_->setFixedHeight(kControlHeight);
        recordMicDetectButton_->setToolTip(
            uiTr("Ask the running session which inputs it has"));
        connect(recordMicDetectButton_, &QPushButton::clicked, this,
                [this] { askForMicrophones(true); });
        fillMicrophoneBox();

        auto *microphoneRow = new QWidget(recording);
        auto *microphoneLayout = new QHBoxLayout(microphoneRow);
        microphoneLayout->setContentsMargins(0, 0, 0, 0);
        microphoneLayout->setSpacing(8);
        recordMicBox_->setParent(microphoneRow);
        recordMicDetectButton_->setParent(microphoneRow);
        microphoneLayout->addWidget(recordMicBox_);
        microphoneLayout->addWidget(recordMicDetectButton_);
        addRow(recording, uiTr("Microphone"),
               uiTr("Recorded into the same MP4 as an AAC track; the name is what the "
                    "file keeps"),
               microphoneRow, false);

        recording = addCard(page, uiTr("Notification"));
        recordNotifySwitch_ = new ModernSwitch(recording);
        recordNotifySwitch_->setObjectName(QStringLiteral("recordNotify"));
        recordNotifySwitch_->setChecked(config_.cli.recordNotify);
        recordNotifySwitch_->setToolTip(uiTr("A receipt for a recording started from a keybinding"));
        addRow(recording, uiTr("Notify when the recording is written"),
               uiTr("A desktop notification naming the file; it needs a notification daemon"),
               recordNotifySwitch_, true);

        buildReplayCard(page);

        return scroll;
    }

    /// The replay card: the same shape as the recording one, over the
    /// `cli.replay` section.  It is its own card rather than rows shared with
    /// the recording because the two sessions have their own sensible defaults
    /// and their own keys -- a replay lives for hours at 30 fps, a recording for
    /// minutes at 60 -- and one card showing both would have to say which of two
    /// values each row meant.
    void buildReplayCard(QWidget *page)
    {
        // The same three subjects as the recording page, in the same order, so
        // the two pages can be read against each other. The ring's own two
        // numbers lead the first card here: they are the rows that decide
        // whether a replay is possible at all, and they have no counterpart on
        // the other page.
        QWidget *replay = addCard(page, uiTr("Ring"));
        // The one numeric default that keeps a ceiling, and it is not ours to
        // lift: the ring holds encoded packets in RAM, so the window is a memory
        // budget -- an hour of a 30 Mbps capture is already gigabytes -- and the
        // ceiling here is the same one `vshot replay --window` accepts.
        replayWindowSpin_ = optionalSpin(replay, kDefaultReplayWindow, 3600, uiTr(" s"));
        replayWindowSpin_->setObjectName(QStringLiteral("replayWindow"));
        replayWindowSpin_->setMinimumWidth(120);
        replayWindowSpin_->setValue(rememberedOr(config_.cli.replayWindow, kDefaultReplayWindow));
        addRow(replay, uiTr("History kept"),
               uiTr("Seconds of history the ring holds, 1-3600"), replayWindowSpin_, true);

        // Capped for the same reason the window is: the ring keeps a whole GOP
        // past the window, so the key-frame distance is memory too, and a GOP
        // longer than the window would make the window itself the smaller half
        // of the ring.
        replayGopSpin_ = optionalSpin(replay, kDefaultReplayGop, 10, uiTr(" s"));
        replayGopSpin_->setObjectName(QStringLiteral("replayGop"));
        replayGopSpin_->setMinimumWidth(120);
        replayGopSpin_->setValue(rememberedOr(config_.cli.replayGop, kDefaultReplayGop));
        addRow(replay, uiTr("Key-frame distance"),
               uiTr("1-10 seconds; smaller makes a save start closer to the moment you asked "
                    "for, at the cost of a bigger ring"),
               replayGopSpin_, false);

        replay = addCard(page, uiTr("Encoder"));
        replayEncoderBox_ =
            choiceBox(replay, encoderNames(), QString::fromLatin1(kDefaultEncoder));
        replayEncoderBox_->setObjectName(QStringLiteral("replayEncoder"));
        replayEncoderBox_->setMinimumWidth(200);
        selectChoice(replayEncoderBox_, config_.cli.replayEncoder);
        addRow(replay, uiTr("Encoder"),
               uiTr("All three encode on the GPU's media engine"),
               replayEncoderBox_, true);

        replayEncoderBackendBox_ = choiceBox(replay, encoderBackendNames(),
                                             QString::fromLatin1(kDefaultEncoderBackend));
        replayEncoderBackendBox_->setObjectName(QStringLiteral("replayEncoderBackend"));
        replayEncoderBackendBox_->setMinimumWidth(200);
        selectChoice(replayEncoderBackendBox_, config_.cli.replayEncoderBackend);
        addRow(replay, uiTr("Hardware encoder"),
               uiTr("auto tries VAAPI then Vulkan then NVENC; all three encode on the GPU "
                    "(VAAPI and Vulkan import the dma-buf, NVENC copies frames via the CPU)"),
               replayEncoderBackendBox_, false);

        replay = addCard(page, uiTr("What is recorded"));
        replayFpsSpin_ = optionalSpin(replay, kDefaultReplayFps, kMaxFrameValue, uiTr(" fps"));
        replayFpsSpin_->setObjectName(QStringLiteral("replayFps"));
        replayFpsSpin_->setMinimumWidth(120);
        replayFpsSpin_->setValue(rememberedOr(config_.cli.replayFps, kDefaultReplayFps));
        addRow(replay, uiTr("Frame rate"),
               uiTr("1-240; a rate below the recording's halves the encoder's work over a "
                    "long session"),
               replayFpsSpin_, true);

        replayFollowEdit_ = new QLineEdit(replay);
        replayFollowEdit_->setObjectName(QStringLiteral("replayFollow"));
        replayFollowEdit_->setMinimumWidth(240);
        replayFollowEdit_->setPlaceholderText(uiTr("no windows to follow"));
        replayFollowEdit_->setText(followText(config_.cli.replayFollow));
        addRow(replay, uiTr("Follow the focus"),
               uiTr("Window names, comma-separated (`replay start window` with no NAME); the "
                    "ring moves to whichever the focus lands on"),
               replayFollowEdit_, false);

        replayPortalSwitch_ = new ModernSwitch(replay);
        replayPortalSwitch_->setObjectName(QStringLiteral("replayPortal"));
        replayPortalSwitch_->setChecked(config_.cli.replayPortal);
        replayPortalSwitch_->setToolTip(
            uiTr("The compositor's own picker decides what is recorded"));
        addRow(replay, uiTr("Through the desktop portal"),
               uiTr("An experimental route for a compositor vshot cannot capture directly"),
               replayPortalSwitch_, false);

        replayMicBox_ = new ModernComboBox(replay);
        replayMicBox_->setObjectName(QStringLiteral("replayMic"));
        replayMicBox_->setMinimumWidth(240);
        replayMicDetectButton_ = new QPushButton(uiTr("Detect"), replay);
        replayMicDetectButton_->setObjectName(QStringLiteral("replayMicDetect"));
        replayMicDetectButton_->setFixedHeight(kControlHeight);
        replayMicDetectButton_->setToolTip(
            uiTr("Ask the running session which inputs it has"));
        connect(replayMicDetectButton_, &QPushButton::clicked, this,
                [this] { askForMicrophones(true); });
        fillReplayMicrophoneBox();

        auto *replayMicrophoneRow = new QWidget(replay);
        auto *replayMicrophoneLayout = new QHBoxLayout(replayMicrophoneRow);
        replayMicrophoneLayout->setContentsMargins(0, 0, 0, 0);
        replayMicrophoneLayout->setSpacing(8);
        replayMicBox_->setParent(replayMicrophoneRow);
        replayMicDetectButton_->setParent(replayMicrophoneRow);
        replayMicrophoneLayout->addWidget(replayMicBox_);
        replayMicrophoneLayout->addWidget(replayMicDetectButton_);
        addRow(replay, uiTr("Microphone"),
               uiTr("Kept in the ring beside the video, as an AAC track"),
               replayMicrophoneRow, false);

        replaySaveDirEdit_ = new QLineEdit(replay);
        replaySaveDirEdit_->setObjectName(QStringLiteral("replaySaveDir"));
        replaySaveDirEdit_->setMinimumWidth(240);
        replaySaveDirEdit_->setPlaceholderText(uiTr("the videos directory"));
        replaySaveDirEdit_->setText(config_.cli.replaySaveDir);
        addRow(replay, uiTr("Save directory"),
               uiTr("Where `replay save` lands when it names no path; strftime is expanded"),
               replaySaveDirEdit_, false);

        replay = addCard(page, uiTr("Notification"));
        replayNotifySwitch_ = new ModernSwitch(replay);
        replayNotifySwitch_->setObjectName(QStringLiteral("replayNotify"));
        replayNotifySwitch_->setChecked(config_.cli.replayNotify);
        replayNotifySwitch_->setToolTip(
            uiTr("A receipt for a save triggered from a keybinding"));
        addRow(replay, uiTr("Notify when a save is written"),
               uiTr("A desktop notification naming the file; it needs a notification daemon"),
               replayNotifySwitch_, true);
    }

    QWidget *buildDialogPage()
    {
        QScrollArea *scroll = newScrollPage(pages_);
        QWidget *page = newPage(scroll);
        addPageHeading(page, uiTr("File dialogs"),
                       uiTr("How the save and open windows are drawn. They are layer "
                            "surfaces, so the compositor draws them no decoration of its "
                            "own and the rim and shadow below are the only things "
                            "separating them from what is behind."));

        QWidget *shape = addCard(page, uiTr("Shape"));
        // Both spin boxes stop at 0 and say what that means, rather than using
        // the optional-spin shape the pin density row uses: there is no
        // "unset" here, only a size, and 0 is a legitimate one for each.
        //
        // The ceiling is the config layer's, not a round number picked here: a
        // value the file accepts has to be reachable from the window, or a
        // hand-written radius would come back clamped the next time this page
        // was saved.
        dialogRadiusSpin_ = new ModernSpinBox(shape);
        dialogRadiusSpin_->setObjectName(QStringLiteral("dialogRadius"));
        dialogRadiusSpin_->setRange(0, static_cast<int>(kMaxFrameValue));
        dialogRadiusSpin_->setMinimumWidth(120);
        dialogRadiusSpin_->setValue(static_cast<int>(config_.dialog.radius));
        dialogRadiusSpin_->setSuffix(uiTr(" px"));
        addRow(shape, uiTr("Corner radius"),
               uiTr("0 draws square corners; the painted corner stops at half the "
                    "shorter side of the window"),
               dialogRadiusSpin_, true);

        QWidget *frame = addCard(page, uiTr("Frame"));
        dialogBorderWidthSpin_ = new ModernSpinBox(frame);
        dialogBorderWidthSpin_->setObjectName(QStringLiteral("dialogBorderWidth"));
        dialogBorderWidthSpin_->setRange(0, static_cast<int>(kMaxFrameValue));
        dialogBorderWidthSpin_->setMinimumWidth(120);
        dialogBorderWidthSpin_->setValue(static_cast<int>(config_.dialog.borderWidth));
        dialogBorderWidthSpin_->setSuffix(uiTr(" px"));
        addRow(frame, uiTr("Border width"),
               uiTr("0 draws no border at all"), dialogBorderWidthSpin_, true);

        borderColorButton_ = new ColorButton(frame, uiTr("Border color"));
        borderColorButton_->setObjectName(QStringLiteral("dialogBorderColorButton"));
        borderColorButton_->onColorChanged = [this](const QColor &chosen) {
            borderColor_ = chosen;
        };
        setBorderColor(config_.dialog.borderColor);
        autoClear_ = automaticButton(frame);
        autoClear_->setObjectName(QStringLiteral("dialogBorderColorClear"));
        connect(autoClear_, &QPushButton::clicked, this, [this] {
            // An invalid colour is exactly how the config says "derive one",
            // so clearing the button is clearing the setting.
            setBorderColor(QColor());
        });
        addRow(frame, uiTr("Border color"),
               uiTr("Automatic derives one from the colour scheme"),
               colorRow(borderColorButton_, autoClear_), false);

        // The shadow shares its card with the rim because the two are one
        // thing to look at: the rim is where the dialog ends and the shadow is
        // what it casts, and a user tuning one is looking at the other.
        QWidget *shadow = addCard(page, uiTr("Shadow"));
        dialogShadow_.build(shadow, config_.dialog.shadow, QStringLiteral("dialog"));

        return scroll;
    }

    QWidget *buildPinPage()
    {
        QScrollArea *scroll = newScrollPage(pages_);
        QWidget *page = newPage(scroll);
        addPageHeading(page, uiTr("Pin appearance"),
                       uiTr("How a pinned image is drawn, and at what size. A pin is a layer "
                            "surface with nothing but the image in it, so its corners, the "
                            "shadow behind it and the line around it are all vshot's to "
                            "draw."));

        // The pin's own size, which is a `cli` default rather than a `pin` one:
        // it is the density a pin is read at, not how the overlay paints.  It
        // sits here anyway because this is the page a user opens when a pin
        // looks wrong, and a density that does not match the screen is one of
        // the two reasons for that.
        // Not "Size": the annotation editor's text card already owns that
        // string, and `uiTr` looks up by the English text, so a second "Size"
        // would render as the text tool's 字号.
        QWidget *density = addCard(page, uiTr("Pin size"));
        // Four is the renderer's own limit rather than a preference: a pin is
        // drawn by scaling the captured image, and the renderer takes a density
        // of 1 to 4 (see `pin.rs`).
        densitySpin_ = optionalSpin(density, kDefaultPinDensity, 4, QString(),
                                    uiTr("inferred"));
        densitySpin_->setObjectName(QStringLiteral("pinDensity"));
        densitySpin_->setMinimumWidth(120);
        densitySpin_->setValue(rememberedOr(config_.cli.pinDensity, kDefaultPinDensity));
        addRow(density, uiTr("Density"),
               uiTr("Device pixels per logical pixel, 1-4; `inferred` works it out from the "
                    "screen the pin is on"),
               densitySpin_, true);

        QWidget *shape = addCard(page, uiTr("Shape"));
        // The radius has no "unset" state, unlike the density row above: zero
        // is the default and means square corners, which is what a screenshot
        // wants. So this is a plain spin box that stops at zero and says what
        // that means, the way the dialog page's pair does.
        //
        // The ceiling is the config layer's, not a round number picked here: a
        // value the file accepts has to be reachable from the window, or a
        // hand-written radius would come back clamped the next time this page
        // was saved.
        pinRadiusSpin_ = new ModernSpinBox(shape);
        pinRadiusSpin_->setObjectName(QStringLiteral("pinRadius"));
        pinRadiusSpin_->setRange(0, static_cast<int>(vshot::kMaxFrameValue));
        pinRadiusSpin_->setMinimumWidth(120);
        pinRadiusSpin_->setValue(static_cast<int>(config_.pin.radius));
        pinRadiusSpin_->setSuffix(uiTr(" px"));
        addRow(shape, uiTr("Corner radius"),
               uiTr("0 draws square corners, which is what a screenshot usually wants; the "
                    "painted corner stops at half the shorter side of the image"),
               pinRadiusSpin_, true);

        // The shadow gets a card of its own rather than sitting in the shape
        // card: it is four rows, and it is the same four rows the file dialog
        // page has, so the two read alike.
        QWidget *shadow = addCard(page, uiTr("Shadow"));
        pinShadow_.build(shadow, config_.pin.shadow, QStringLiteral("pin"));

        QWidget *frame = addCard(page, uiTr("Border"));
        pinBorderWidthSpin_ = new ModernSpinBox(frame);
        pinBorderWidthSpin_->setObjectName(QStringLiteral("pinBorderWidth"));
        pinBorderWidthSpin_->setRange(0, static_cast<int>(vshot::kMaxFrameValue));
        pinBorderWidthSpin_->setMinimumWidth(120);
        pinBorderWidthSpin_->setValue(static_cast<int>(config_.pin.borderWidth));
        pinBorderWidthSpin_->setSuffix(uiTr(" px"));
        addRow(frame, uiTr("Border width"),
               uiTr("0 draws no border at all"), pinBorderWidthSpin_, true);

        // Two colours, because a pin has two states and the colour is how it
        // says which one it is in: the picked pin -- the one a key press would
        // reach -- against every other.
        pinBorderColorButton_ = new ColorButton(frame, uiTr("Border color"));
        pinBorderColorButton_->setObjectName(QStringLiteral("pinBorderColorButton"));
        pinBorderColorButton_->onColorChanged = [this](const QColor &chosen) {
            pinBorderColor_ = chosen;
        };
        setPinColor(false, config_.pin.borderColor);
        auto *clearBorder = automaticButton(frame);
        clearBorder->setObjectName(QStringLiteral("pinBorderColorClear"));
        connect(clearBorder, &QPushButton::clicked, this, [this] {
            setPinColor(false, QColor());
        });
        addRow(frame, uiTr("Border color"),
               uiTr("Automatic uses the built-in light grey"), colorRow(pinBorderColorButton_,
                                                                         clearBorder),
               false);

        pinActiveColorButton_ = new ColorButton(frame, uiTr("Active border color"));
        pinActiveColorButton_->setObjectName(QStringLiteral("pinActiveColorButton"));
        pinActiveColorButton_->onColorChanged = [this](const QColor &chosen) {
            pinActiveColorColor_ = chosen;
        };
        setPinColor(true, config_.pin.activeBorderColor);
        auto *clearActive = automaticButton(frame);
        clearActive->setObjectName(QStringLiteral("pinActiveColorClear"));
        connect(clearActive, &QPushButton::clicked, this, [this] {
            setPinColor(true, QColor());
        });
        addRow(frame, uiTr("Active border color"),
               uiTr("The pin the keyboard would act on; automatic uses black"),
               colorRow(pinActiveColorButton_, clearActive), false);

        return scroll;
    }

    /// What a row's button shows for `action`: every key it answers to, joined
    /// by commas, or the word for "none".  A row that printed only the first
    /// key would hide the rest of them behind a dialog the user has no reason
    /// to open.
    QString shortcutKeysText(ShortcutAction action) const
    {
        const QVector<QKeySequence> keys = shortcuts_.keysFor(action);
        if (keys.isEmpty()) {
            return uiTr("None");
        }
        QStringList parts;
        parts.reserve(keys.size());
        for (const QKeySequence &key : keys) {
            parts.append(key.toString(QKeySequence::PortableText));
        }
        return parts.join(QStringLiteral(", "));
    }

    /// The dialog one key-binding row opens, built over the dialog's own
    /// bindings.  It edits them in place, so a change is visible on the page the
    /// moment it is made and there is nothing to copy back; Cancel is the
    /// dialog's own job.
    ///
    /// Shown with `open` rather than `exec`: the editor is window-modal, which
    /// is what a child of a settings window wants, but it does not spin a
    /// nested event loop of its own.  A nested loop here would be a second
    /// place for the application to be re-entered from, and the page would have
    /// no way to redraw the row that lost a key while it ran.
    void openShortcutEditor(ShortcutAction action)
    {
        QDialog *dialog = createShortcutEditorDialog(action, &shortcuts_, this);
        connect(dialog, &QDialog::finished, this, [this, dialog] {
            refreshShortcutRows();
            // Off the parent before it is dropped: `deleteLater` leaves the
            // dialog alive until the event loop comes back round, and one that
            // is still a child is one `findChild` still hands out -- so the next
            // row the user clicks would be answered by the editor they just
            // closed.
            dialog->setParent(nullptr);
            dialog->deleteLater();
        });
        dialog->open();
    }

    /// Repaints every row's button from the bindings.  A dialog can move a key
    /// off another action, so the row that lost it has to be redrawn too -- not
    /// only the one that was opened.
    void refreshShortcutRows()
    {
        for (QAbstractButton *button : findChildren<QAbstractButton *>()) {
            const QString name = button->objectName();
            if (!name.startsWith(QStringLiteral("shortcut_"))) {
                continue;
            }
            const QString id = name.mid(QStringLiteral("shortcut_").size());
            for (int index = 0; index < static_cast<int>(ShortcutAction::kActionCount);
                 ++index) {
                const ShortcutAction action = static_cast<ShortcutAction>(index);
                if (shortcutBinding(action).id != id) {
                    continue;
                }
                if (auto *capture = dynamic_cast<KeyCaptureButton *>(button)) {
                    capture->setKeys(shortcutKeysText(action));
                }
                break;
            }
        }
    }

    QWidget *buildKeyboardPage()
    {
        QScrollArea *scroll = newScrollPage(pages_);
        QWidget *page = newPage(scroll);
        addPageHeading(page, uiTr("Keyboard"),
                       uiTr("Which key does what, while a capture is on the screen. Every "
                            "action here is also a toolbar button, so a key that is in the "
                            "way can be cleared instead of moved."));
        QWidget *card = addCard(page, QString());
        bool first = true;
        for (int index = 0; index < static_cast<int>(ShortcutAction::kActionCount);
             ++index) {
            const ShortcutAction action = static_cast<ShortcutAction>(index);
            const ShortcutBinding &binding = shortcutBinding(action);
            if (binding.label.isEmpty()) {
                continue;
            }
            // The held modifiers are listed but not editable: a `QKeySequence`
            // cannot say "Alt on its own" without also saying Alt+F4, so a row
            // that offered to rebind one would be offering a binding the editor
            // could never deliver.  A label with no button, rather than a button
            // the user has to discover does nothing.
            if (!binding.rebindable) {
                auto *text = new QLabel(card);
                text->setObjectName(QStringLiteral("rowValue"));
                text->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                text->setText(binding.defaultKeys);
                addRow(card, binding.label, binding.hint, text, first);
                first = false;
                continue;
            }
            auto *button = new KeyCaptureButton(card);
            button->setObjectName(QStringLiteral("shortcut_") + binding.id);
            button->setToolTip(uiTr("Click to see and change this action's keys"));
            button->setKeys(shortcutKeysText(action));
            button->onActivated = [this, action] { openShortcutEditor(action); };
            addRow(card, binding.label, binding.hint, button, first);
            first = false;
        }

        return scroll;
    }

    /// A colour button plus the button that gives it back to the built-in
    /// colour, laid out as one control for [`addRow`].
    QWidget *colorRow(ColorButton *button, QPushButton *clear)
    {
        auto *row = new QWidget(button->parentWidget());
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(8);
        button->setParent(row);
        clear->setParent(row);
        layout->addWidget(button);
        layout->addWidget(clear);
        return row;
    }

    QPushButton *automaticButton(QWidget *parent)
    {
        auto *button = new QPushButton(uiTr("Automatic"), parent);
        button->setObjectName(QStringLiteral("clearColor"));
        button->setFixedHeight(kControlHeight);
        button->setToolTip(uiTr("Use the built-in colour instead of one of its own"));
        return button;
    }

    /// Shows one of the pin's two rim colours, or the automatic state when
    /// `color` is invalid.  The pair share this so the two rows cannot be wired
    /// the same one by accident.
    void setPinColor(bool active, const QColor &color)
    {
        ColorButton *button = active ? pinActiveColorButton_ : pinBorderColorButton_;
        (active ? pinActiveColorColor_ : pinBorderColor_) = color;
        if (color.isValid()) {
            button->setColor(color);
        } else {
            button->setText(uiTr("Automatic"));
            button->setIcon(QIcon());
        }
        button->setEnabled(true);
    }

    /// Shows a rim colour, or the automatic state when `color` is invalid.
    void setBorderColor(const QColor &color)
    {
        borderColor_ = color;
        if (color.isValid()) {
            borderColorButton_->setColor(color);
            borderColorButton_->setEnabled(true);
        } else {
            // No colour chosen: the button still opens the picker, which is how
            // one gets chosen, but it carries no swatch to read.
            borderColorButton_->setText(uiTr("Automatic"));
            borderColorButton_->setIcon(QIcon());
            borderColorButton_->setEnabled(true);
        }
    }

    void save()
    {
        Config config = config_;
        EditorPreferences &editor = config.editor;
        editor.tool = toolBox_->currentData().toString();
        editor.selectMode = selectModeBox_->currentData().toString();
        editor.color = colorButton_->color();
        editor.width = static_cast<std::uint32_t>(std::max(1, widthSpin_->value()));
        editor.dash = dashBox_->currentData().toString();
        editor.arrowSize = static_cast<std::uint32_t>(std::max(1, arrowSizeSpin_->value()));
        editor.arrowStyle = arrowStyleBox_->currentData().toString();
        editor.textSize =
            static_cast<std::uint32_t>(clampTextPixels(textSizeSpin_->value()));
        editor.font = fontBox_->currentData().toString();
        editor.mosaicShape = mosaicShapeBox_->currentData().toString();
        editor.mosaicStrength =
            static_cast<std::uint32_t>(std::max(1, mosaicStrengthSpin_->value()));

        CliPreferences &cli = config.cli;
        cli.pngCompression = compressionBox_->currentData().toString();
        cli.hdrFormat = hdrFormatBox_->currentData().toString();
        cli.toneMap = toneMapBox_->currentData().toString();
        // A box still sitting on the built-in default writes nothing, exactly
        // like the spin boxes: the file then keeps following the map's own
        // default instead of freezing today's number into it.
        const double white = toneMapWhiteSpin_->value() / 100.0;
        cli.toneMapWhite = std::abs(white - kDefaultToneMapWhite) < 1e-6 ? 0.0 : white;
        cli.hdrAreaTest = hdrAreaSwitch_->isChecked();
        // A box still sitting on the built-in default writes nothing, as the
        // white level above does -- the file then follows the daemon's own
        // default instead of freezing today's number into it.  Zero is not the
        // default, so a user who asks for "always HDR" still writes a zero and
        // still reads it back: the sentinel is only for a box nobody touched.
        const double ratio = hdrAreaRatioSpin_->value() / 100.0;
        cli.hdrAreaRatio =
            std::abs(ratio - kDefaultHdrAreaRatio) < 1e-9 ? -1.0 : ratio;
        cli.monitor = monitorEdit_->text().trimmed();
        // Every spin box is read back with the built-in default it opened on: a
        // value still sitting there is written as the sentinel, which is what
        // keeps an untouched default out of the file and lets a later version's
        // default reach anyone who never chose one.
        cli.pinDensity = spinValue(densitySpin_, kDefaultPinDensity);
        cli.longNotches = spinValue(notchesSpin_, kDefaultLongNotches);
        cli.longMaxHeight = spinValue(maxHeightSpin_, kDefaultLongMaxHeight);
        cli.longMaxFrames = spinValue(maxFramesSpin_, kDefaultLongMaxFrames);
        cli.longTimeout = spinValue(timeoutSpin_, kDefaultLongTimeout);
        cli.longIgnoreTop = spinValue(ignoreTopSpin_, kDefaultLongIgnoreTop);
        cli.longInject = injectBox_->currentData().toString();
        cli.ocrNotify = ocrNotifySwitch_->isChecked();

        cli.recordEncoder = recordEncoderBox_->currentData().toString();
        cli.recordEncoderBackend = recordEncoderBackendBox_->currentData().toString();
        cli.recordFps = spinValue(recordFpsSpin_, kDefaultRecordFps);
        cli.recordPortal = recordPortalSwitch_->isChecked();
        cli.recordFollow = parseFollowText(recordFollowEdit_->text());
        cli.recordNotify = recordNotifySwitch_->isChecked();
        // Three answers, two stored states: no `mic` key at all is silence,
        // while the empty string is the session's default input.
        const QString microphone = recordMicBox_->currentData().toString();
        cli.recordMicEnabled = microphone != kNoMicrophone;
        cli.recordMic =
            (microphone == kNoMicrophone || microphone == kDefaultMicrophone) ? QString()
                                                                             : microphone;

        cli.replayWindow = spinValue(replayWindowSpin_, kDefaultReplayWindow);
        cli.replayGop = spinValue(replayGopSpin_, kDefaultReplayGop);
        cli.replayEncoder = replayEncoderBox_->currentData().toString();
        cli.replayEncoderBackend = replayEncoderBackendBox_->currentData().toString();
        cli.replayFps = spinValue(replayFpsSpin_, kDefaultReplayFps);
        cli.replayFollow = parseFollowText(replayFollowEdit_->text());
        cli.replayPortal = replayPortalSwitch_->isChecked();
        const QString replayMicrophone = replayMicBox_->currentData().toString();
        cli.replayMicEnabled = replayMicrophone != kNoMicrophone;
        cli.replayMic = (replayMicrophone == kNoMicrophone ||
                         replayMicrophone == kDefaultMicrophone)
                            ? QString()
                            : replayMicrophone;
        cli.replaySaveDir = replaySaveDirEdit_->text().trimmed();
        cli.replayNotify = replayNotifySwitch_->isChecked();

        DialogPreferences &dialog = config.dialog;
        dialog.radius =
            static_cast<std::uint32_t>(std::max(0, dialogRadiusSpin_->value()));
        dialog.borderWidth =
            static_cast<std::uint32_t>(std::max(0, dialogBorderWidthSpin_->value()));
        dialog.borderColor = borderColor_;
        dialog.shadow = dialogShadow_.read();

        PinPreferences &pin = config.pin;
        pin.radius = static_cast<std::uint32_t>(std::max(0, pinRadiusSpin_->value()));
        pin.shadow = pinShadow_.read();
        pin.borderWidth = static_cast<std::uint32_t>(std::max(0, pinBorderWidthSpin_->value()));
        pin.borderColor = pinBorderColor_;
        pin.activeBorderColor = pinActiveColorColor_;

        if (shortcutsOwner_ != nullptr) {
            // A caller that handed the dialog its own bindings reads them back
            // itself; there is no file in that arrangement to write, and the
            // status line has nothing to report about one.
            *shortcutsOwner_ = shortcuts_;
            status_->setText(uiTr("Saved"));
            return;
        }
        if (!saveConfig(config)) {
            status_->setProperty("error", true);
            status_->setText(uiTr("Could not write the config file."));
            status_->style()->unpolish(status_);
            status_->style()->polish(status_);
            return;
        }
        // The bindings are their own section, which `Config` does not carry:
        // the editor reads them out of the file on every start rather than out
        // of a capture process that has to be told.  Written after the config,
        // so a failure there leaves the file with the bindings it already had
        // instead of half of one save.
        if (!saveShortcutPreferences(shortcuts_)) {
            status_->setProperty("error", true);
            status_->setText(uiTr("Could not write the config file."));
            status_->style()->unpolish(status_);
            status_->style()->polish(status_);
            return;
        }
        config_ = config;
        status_->setProperty("error", false);
        // The window stays open on purpose: saving is not dismissing.  The
        // settings are meant to be tried and re-tried -- change a colour, save,
        // look at the result, come back and change it again -- and a window
        // that vanished on every save would have to be reopened for each one.
        // Cancel still closes it, and now means only "close".
        status_->setText(uiTr("Saved."));
        status_->style()->unpolish(status_);
        status_->style()->polish(status_);
    }

    Config config_;
    ShortcutPreferences shortcuts_;
    /// The caller's bindings when the dialog was handed some, and null for the
    /// ordinary window, which reads and writes the file.
    ShortcutPreferences *shortcutsOwner_ = nullptr;
    /// The session's audio inputs, asked for once and shared by both cards.
    MicrophoneProbe microphones_;
    QListWidget *sidebar_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QComboBox *toolBox_ = nullptr;
    QComboBox *selectModeBox_ = nullptr;
    ColorButton *colorButton_ = nullptr;
    QSpinBox *widthSpin_ = nullptr;
    QComboBox *dashBox_ = nullptr;
    QSpinBox *arrowSizeSpin_ = nullptr;
    QComboBox *arrowStyleBox_ = nullptr;
    QSpinBox *textSizeSpin_ = nullptr;
    QComboBox *fontBox_ = nullptr;
    QComboBox *mosaicShapeBox_ = nullptr;
    QSpinBox *mosaicStrengthSpin_ = nullptr;
    QComboBox *compressionBox_ = nullptr;
    QComboBox *hdrFormatBox_ = nullptr;
    QComboBox *toneMapBox_ = nullptr;
    QDoubleSpinBox *toneMapWhiteSpin_ = nullptr;
    ModernSwitch *hdrAreaSwitch_ = nullptr;
    QDoubleSpinBox *hdrAreaRatioSpin_ = nullptr;
    QLineEdit *monitorEdit_ = nullptr;
    QSpinBox *densitySpin_ = nullptr;
    QSpinBox *notchesSpin_ = nullptr;
    QSpinBox *maxHeightSpin_ = nullptr;
    QSpinBox *maxFramesSpin_ = nullptr;
    QSpinBox *timeoutSpin_ = nullptr;
    QSpinBox *ignoreTopSpin_ = nullptr;
    QComboBox *injectBox_ = nullptr;
    ModernSwitch *ocrNotifySwitch_ = nullptr;
    QComboBox *recordEncoderBox_ = nullptr;
    QComboBox *recordEncoderBackendBox_ = nullptr;
    ModernSpinBox *recordFpsSpin_ = nullptr;
    QLineEdit *recordFollowEdit_ = nullptr;
    ModernSwitch *recordPortalSwitch_ = nullptr;
    ModernComboBox *recordMicBox_ = nullptr;
    QPushButton *recordMicDetectButton_ = nullptr;
    ModernSwitch *recordNotifySwitch_ = nullptr;
    ModernSpinBox *replayWindowSpin_ = nullptr;
    ModernSpinBox *replayGopSpin_ = nullptr;
    QComboBox *replayEncoderBox_ = nullptr;
    QComboBox *replayEncoderBackendBox_ = nullptr;
    ModernSpinBox *replayFpsSpin_ = nullptr;
    QLineEdit *replayFollowEdit_ = nullptr;
    ModernSwitch *replayPortalSwitch_ = nullptr;
    ModernComboBox *replayMicBox_ = nullptr;
    QPushButton *replayMicDetectButton_ = nullptr;
    QLineEdit *replaySaveDirEdit_ = nullptr;
    ModernSwitch *replayNotifySwitch_ = nullptr;
    ModernSpinBox *dialogRadiusSpin_ = nullptr;
    ModernSpinBox *dialogBorderWidthSpin_ = nullptr;
    ShadowControls dialogShadow_;
    ColorButton *borderColorButton_ = nullptr;
    QPushButton *autoClear_ = nullptr;
    QColor borderColor_;
    ModernSpinBox *pinRadiusSpin_ = nullptr;
    ShadowControls pinShadow_;
    ModernSpinBox *pinBorderWidthSpin_ = nullptr;
    ColorButton *pinBorderColorButton_ = nullptr;
    ColorButton *pinActiveColorButton_ = nullptr;
    QColor pinBorderColor_;
    QColor pinActiveColorColor_;
    QLabel *status_ = nullptr;
};

} // namespace

QDialog *createShortcutEditorDialog(ShortcutAction action, ShortcutPreferences *preferences,
                                    QWidget *parent, std::function<bool(const QString &)> ask)
{
    return new ShortcutEditorDialog(action, preferences, parent, std::move(ask));
}

QDialog *createSettingsDialog()
{
    return new SettingsDialog();
}

QDialog *createSettingsDialogForShortcuts(ShortcutPreferences *shortcuts)
{
    return new SettingsDialog(shortcuts);
}

int runSettingsWindow()
{
    SettingsDialog dialog;
    dialog.show();
    return QApplication::exec();
}

} // namespace vshot

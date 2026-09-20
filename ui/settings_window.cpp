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
#include "text_size.hpp"
#include "i18n.hpp"

#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

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

QComboBox, QLineEdit, QSpinBox { color: %2; background: %9;
            border: 1px solid %10; border-radius: 8px;
            padding: 0 10px; min-height: 32px; font-size: 13px;
            selection-color: %8; selection-background-color: %7; }
QComboBox:hover, QLineEdit:hover, QSpinBox:hover { border-color: #4d5765; }
QComboBox:focus, QLineEdit:focus, QSpinBox:focus { border-color: %7; }
QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled {
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
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QSpinBox::paintEvent(event);
        QPainter painter(this);
        paintChevron(painter, upBox(), kChevron, false);
        paintChevron(painter, downBox(), kChevron, true);
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

/// A combo box whose entries are the same names the config file accepts, with
/// a leading entry for "the file says nothing".
ModernComboBox *choiceBox(QWidget *parent, const QStringList &values, const QString &emptyLabel)
{
    auto *box = new ModernComboBox(parent);
    box->addItem(emptyLabel, QString());
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

/// A spin box with an explicit "not set" value.  Zero is the sentinel the
/// config layer uses for "the file says nothing", so the special value text is
/// what makes an unset default visible instead of looking like a real zero.
ModernSpinBox *optionalSpin(QWidget *parent, int maximum, const QString &suffix)
{
    auto *spin = new ModernSpinBox(parent);
    spin->setRange(0, maximum);
    spin->setSpecialValueText(uiTr("default"));
    spin->setValue(0);
    if (!suffix.isEmpty()) {
        spin->setSuffix(suffix);
    }
    return spin;
}

/// Reads a spin box back into the config: 0 (the "default" entry) stays 0.
std::uint32_t spinValue(const QSpinBox *spin)
{
    return static_cast<std::uint32_t>(std::max(0, spin->value()));
}

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
    explicit ColorButton(QWidget *parent)
        : QPushButton(parent)
    {
        setObjectName(QStringLiteral("swatch"));
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumWidth(132);
        connect(this, &QPushButton::clicked, this, [this] {
            const QColor chosen = QColorDialog::getColor(
                color_, this, uiTr("Annotation color"), QColorDialog::ShowAlphaChannel);
            if (chosen.isValid()) {
                setColor(chosen);
            }
        });
    }

    void setColor(const QColor &color)
    {
        color_ = color;
        setText(vshot::colorText(color));
        setIcon(QIcon(colorChip(color)));
        setIconSize(QSize(16, 16));
        setToolTip(uiTr("Annotation color"));
    }

    QColor color() const { return color_; }

private:
    QColor color_{255, 64, 64, 255};
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
    pageLayout->addWidget(column);
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
    pageLayout->addWidget(header);
}

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
    } else {
        // A chevron prompt: the command line.
        painter.drawLine(QPointF(4.0, 5.5), QPointF(8.0, 9.0));
        painter.drawLine(QPointF(8.0, 9.0), QPointF(4.0, 12.5));
        painter.drawLine(QPointF(10.0, 13.0), QPointF(15.0, 13.0));
    }
    return QIcon(pixmap);
}

/// The window itself.  It owns nothing but the widgets; the config is read once
/// when it opens and written once when Save is pressed, so a change made by
/// hand in the file while it is open is not silently reverted by a Cancel.
class SettingsDialog final : public QDialog {
public:
    SettingsDialog()
        : config_(loadConfig())
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
        sidebar_->addItem(new QListWidgetItem(sectionIcon(0, QColor(kInkDim)),
                                              uiTr("Annotation editor")));
        sidebar_->addItem(new QListWidgetItem(sectionIcon(1, QColor(kInkDim)),
                                              uiTr("Command-line defaults")));
        bodyLayout->addWidget(sidebar_, 0);

        pages_ = new QStackedWidget(body);
        pages_->addWidget(buildEditorPage());
        pages_->addWidget(buildCliPage());
        bodyLayout->addWidget(pages_, 1);

        connect(sidebar_, &QListWidget::currentRowChanged, this, [this](int row) {
            if (row >= 0 && row < pages_->count()) {
                pages_->setCurrentIndex(row);
            }
        });
        sidebar_->setCurrentRow(0);

        root->addWidget(body, 1);
        root->addWidget(buildFooter());
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

        QWidget *shape = addCard(page, QString());
        toolBox_ = new ModernComboBox(shape);
        toolBox_->setObjectName(QStringLiteral("tool"));
        toolBox_->setMinimumWidth(180);
        for (const QString &value : toolNames()) {
            toolBox_->addItem(value, value);
        }
        selectChoice(toolBox_, config_.editor.tool);
        addRow(shape, uiTr("Opening tool"),
               uiTr("The tool editing starts with; session changes are not saved here"),
               toolBox_, true);

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

    QWidget *buildCliPage()
    {
        QScrollArea *scroll = newScrollPage(pages_);
        QWidget *page = newPage(scroll);
        addPageHeading(page, uiTr("Command-line defaults"),
                       uiTr("Used only for arguments the command line does not give. An "
                            "argument, or an environment variable, always wins over these."));

        QWidget *output = addCard(page, uiTr("Output"));
        compressionBox_ = choiceBox(output, compressionNames(), uiTr("built-in default"));
        compressionBox_->setObjectName(QStringLiteral("pngCompression"));
        compressionBox_->setMinimumWidth(200);
        selectChoice(compressionBox_, config_.cli.pngCompression);
        addRow(output, uiTr("PNG compression"),
               uiTr("All levels are lossless; slower ones buy a smaller file"),
               compressionBox_, true);

        monitorEdit_ = new QLineEdit(output);
        monitorEdit_->setObjectName(QStringLiteral("monitor"));
        monitorEdit_->setMinimumWidth(220);
        monitorEdit_->setPlaceholderText(uiTr("the pointer's output"));
        monitorEdit_->setText(config_.cli.monitor);
        addRow(output, uiTr("Default monitor"),
               uiTr("An output name, or `current` for the output under the pointer"),
               monitorEdit_, false);

        QWidget *pins = addCard(page, uiTr("Pins"));
        densitySpin_ = optionalSpin(pins, 4, QString());
        densitySpin_->setObjectName(QStringLiteral("pinDensity"));
        densitySpin_->setMinimumWidth(120);
        densitySpin_->setValue(static_cast<int>(config_.cli.pinDensity));
        addRow(pins, uiTr("Density"),
               uiTr("Device pixels per logical pixel, 1-4; inferred when unset"),
               densitySpin_, true);

        QWidget *scrolling = addCard(page, uiTr("Scrolling capture"));
        notchesSpin_ = optionalSpin(scrolling, 1000, QString());
        notchesSpin_->setObjectName(QStringLiteral("longNotches"));
        notchesSpin_->setMinimumWidth(120);
        notchesSpin_->setValue(static_cast<int>(config_.cli.longNotches));
        addRow(scrolling, uiTr("Scroll notches"),
               uiTr("Wheel notches sent at a time"), notchesSpin_, true);

        maxHeightSpin_ = optionalSpin(scrolling, 1'000'000, uiTr(" px"));
        maxHeightSpin_->setObjectName(QStringLiteral("longMaxHeight"));
        maxHeightSpin_->setMinimumWidth(120);
        maxHeightSpin_->setValue(static_cast<int>(config_.cli.longMaxHeight));
        addRow(scrolling, uiTr("Max height"), QString(), maxHeightSpin_, false);

        maxFramesSpin_ = optionalSpin(scrolling, 1'000'000, QString());
        maxFramesSpin_->setObjectName(QStringLiteral("longMaxFrames"));
        maxFramesSpin_->setMinimumWidth(120);
        maxFramesSpin_->setValue(static_cast<int>(config_.cli.longMaxFrames));
        addRow(scrolling, uiTr("Max frames"), QString(), maxFramesSpin_, false);

        timeoutSpin_ = optionalSpin(scrolling, 86'400, uiTr(" s"));
        timeoutSpin_->setObjectName(QStringLiteral("longTimeout"));
        timeoutSpin_->setMinimumWidth(120);
        timeoutSpin_->setValue(static_cast<int>(config_.cli.longTimeout));
        addRow(scrolling, uiTr("Timeout"), QString(), timeoutSpin_, false);

        ignoreTopSpin_ = optionalSpin(scrolling, 100'000, uiTr(" px"));
        ignoreTopSpin_->setObjectName(QStringLiteral("longIgnoreTop"));
        ignoreTopSpin_->setMinimumWidth(120);
        ignoreTopSpin_->setValue(static_cast<int>(config_.cli.longIgnoreTop));
        addRow(scrolling, uiTr("Ignore top"),
               uiTr("Rows at the top of every frame left out of the match, for sticky "
                    "headers"),
               ignoreTopSpin_, false);

        injectBox_ = choiceBox(scrolling, injectNames(), uiTr("built-in default (auto)"));
        injectBox_->setObjectName(QStringLiteral("longInject"));
        injectBox_->setMinimumWidth(200);
        selectChoice(injectBox_, config_.cli.longInject);
        addRow(scrolling, uiTr("Scroll backend"), QString(), injectBox_, false);

        return scroll;
    }

    void save()
    {
        Config config = config_;
        EditorPreferences &editor = config.editor;
        editor.tool = toolBox_->currentData().toString();
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
        cli.monitor = monitorEdit_->text().trimmed();
        cli.pinDensity = spinValue(densitySpin_);
        cli.longNotches = spinValue(notchesSpin_);
        cli.longMaxHeight = spinValue(maxHeightSpin_);
        cli.longMaxFrames = spinValue(maxFramesSpin_);
        cli.longTimeout = spinValue(timeoutSpin_);
        cli.longIgnoreTop = spinValue(ignoreTopSpin_);
        cli.longInject = injectBox_->currentData().toString();

        if (!saveConfig(config)) {
            status_->setProperty("error", true);
            status_->setText(uiTr("Could not write the config file."));
            status_->style()->unpolish(status_);
            status_->style()->polish(status_);
            return;
        }
        config_ = config;
        status_->setProperty("error", false);
        status_->setText(uiTr("Saved."));
        status_->style()->unpolish(status_);
        status_->style()->polish(status_);
        accept();
    }

    Config config_;
    QListWidget *sidebar_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QComboBox *toolBox_ = nullptr;
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
    QLineEdit *monitorEdit_ = nullptr;
    QSpinBox *densitySpin_ = nullptr;
    QSpinBox *notchesSpin_ = nullptr;
    QSpinBox *maxHeightSpin_ = nullptr;
    QSpinBox *maxFramesSpin_ = nullptr;
    QSpinBox *timeoutSpin_ = nullptr;
    QSpinBox *ignoreTopSpin_ = nullptr;
    QComboBox *injectBox_ = nullptr;
    QLabel *status_ = nullptr;
};

} // namespace

QDialog *createSettingsDialog()
{
    return new SettingsDialog();
}

int runSettingsWindow()
{
    SettingsDialog dialog;
    dialog.show();
    return QApplication::exec();
}

} // namespace vshot

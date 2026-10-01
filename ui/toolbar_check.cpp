// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// Offline check for where the editor's floating toolbar lands.
//
// The toolbar keeps its command bar (the tool row) pinned to the selection and
// grows the style row out of the way.  That is easy to get wrong at the edges:
// when the capture nearly fills the display there is no room beyond either side
// of the command bar, and a placement that only flips the whole panel up or
// down drags the buttons around as the style row shows and hides.  This check
// drives the real controller and reads the resulting widget geometry, so what
// is asserted is the placement the user actually meets.
//
// Needs QApplication and the offscreen platform plugin; no compositor and no
// layer shell.  `QT_QPA_PLATFORM=offscreen` supplies the one screen the overlay
// is parented to.
//
// Built only with `-DVSHOT_BUILD_CHECKS=ON`; see the README's verification
// section.

#include "capture_overlay.hpp"

#include <QApplication>
#include <QBoxLayout>
#include <QColor>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHelpEvent>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QPainter>
#include <QPoint>
#include <QPushButton>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QStyle>
#include <QStyleOptionButton>
#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void expect(bool condition, const char *what, const QString &detail = QString())
{
    if (condition) {
        std::printf("ok    %s\n", what);
        return;
    }
    ++failures;
    if (detail.isEmpty()) {
        std::printf("FAIL  %s\n", what);
    } else {
        std::printf("FAIL  %s -- %s\n", what, qPrintable(detail));
    }
}

// A one-output region session with a fixed output and an already-made
// selection, so the controller opens in editing state with the toolbar up.
//
// The output is 400x400 by default, which is small enough that the toolbar is
// clamped against its edge -- that is what the placement checks want.  A check
// that measures the toolbar's own width asks for a display-sized one instead.
vshot::Session sessionFor(const vshot::LogicalRect &selection,
                          const QSize &screen = QSize(400, 400))
{
    vshot::Session session;
    session.mode = QStringLiteral("region");
    session.bounds = vshot::LogicalRect{0, 0, static_cast<std::uint32_t>(screen.width()),
                                        static_cast<std::uint32_t>(screen.height())};
    vshot::OutputSession output;
    output.id = 1;
    output.name = QStringLiteral("CHECK-1");
    output.geometry = session.bounds;
    output.surface = output.geometry;
    output.scale = 1;
    output.pixelWidth = screen.width();
    output.pixelHeight = screen.height();
    session.outputs.push_back(output);
    session.selection = selection;
    return session;
}

struct ToolbarParts {
    QWidget *command = nullptr;
    QWidget *style = nullptr;
};

ToolbarParts toolbarParts(vshot::CaptureOverlay *overlay)
{
    ToolbarParts parts;
    parts.command = overlay->findChild<QWidget *>(QStringLiteral("toolbarCommandSurface"));
    parts.style = overlay->findChild<QWidget *>(QStringLiteral("toolbarStyleRow"));
    return parts;
}

// The card's own layout: the two rows of buttons in a column on the left, a
// stretch, the divider, and the grid of the four ends pinned to the right.
QBoxLayout *cardLayout(QWidget *command)
{
    return command != nullptr ? qobject_cast<QBoxLayout *>(command->layout()) : nullptr;
}

// One of the two rows of that column: 0 is the drawing tools, 1 is everything
// that acts on the capture.
QLayout *commandRow(QWidget *command, int index)
{
    QBoxLayout *card = cardLayout(command);
    if (card == nullptr || card->count() < 1) {
        return nullptr;
    }
    QLayout *column = card->itemAt(0)->layout();
    if (column == nullptr || column->count() <= index) {
        return nullptr;
    }
    return column->itemAt(index)->layout();
}

// The grid the four ends live in, which is the last thing in the card.
QGridLayout *endsGrid(QWidget *command)
{
    QBoxLayout *card = cardLayout(command);
    if (card == nullptr || card->count() < 1) {
        return nullptr;
    }
    return qobject_cast<QGridLayout *>(card->itemAt(card->count() - 1)->layout());
}

// Where a widget sits inside the card, as the card's own margins measure it.
QRect inCard(QWidget *command, QWidget *widget)
{
    const QPoint at = widget->mapTo(command, QPoint(0, 0));
    return QRect(at, widget->size());
}

int globalTop(const QWidget *widget)
{
    return widget->mapToGlobal(QPoint(0, 0)).y();
}

// The capture nearly fills the display: no room above or below the command bar
// for the style row.  The style row must double back over the selection rather
// than push the command bar, which is what made the buttons jump.
void checkCrampedCaptureKeepsTheButtonsStill()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(sessionFor(vshot::LogicalRect{0, 0, 400, 400}));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();
    const ToolbarParts parts = toolbarParts(overlay);
    expect(parts.command != nullptr && parts.style != nullptr,
           "the toolbar has a command bar and a style row");
    if (parts.command == nullptr || parts.style == nullptr) {
        return;
    }
    // The remembered tool in the user's config can open the session on another
    // tool, so arm nothing before asserting on what an unarmed session's style
    // row does.
    controller.chooseTool(std::nullopt);
    expect(!parts.style->isVisible(), "an unarmed session starts with no style row");
    const int before = globalTop(parts.command);

    controller.chooseTool(vshot::Tool::Rectangle);
    expect(parts.style->isVisible(), "the Rectangle tool raises the style row");
    const int after = globalTop(parts.command);
    const int styleTop = globalTop(parts.style);
    expect(std::abs(after - before) <= 1,
           "the command bar does not move when the style row appears",
           QStringLiteral("moved from %1 to %2").arg(before).arg(after));
    expect(styleTop < after,
           "the style row doubles back above the command bar",
           QStringLiteral("style row top %1, bar top %2").arg(styleTop).arg(after));
}

// Room above the selection: the whole panel stays above it and the style row
// grows further up, away from the selection.
//
// The command bar stands two rows tall, so the room the panel needs above a
// selection is about the panel's own height.  The selection is placed far
// enough down this 400 px output for that room to exist, and close enough to
// its bottom that the panel still cannot fit below instead -- a panel that
// dropped would otherwise pass this check without keeping any side at all.
void checkPanelAboveKeepsTheStyleRowAbove()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(sessionFor(vshot::LogicalRect{150, 200, 100, 100}));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();
    const ToolbarParts parts = toolbarParts(overlay);
    if (parts.command == nullptr || parts.style == nullptr) {
        expect(false, "the toolbar has a command bar and a style row");
        return;
    }
    const int before = globalTop(parts.command);
    expect(before + parts.command->height() <= 200,
           "the command bar sits above the selection",
           QStringLiteral("bar bottom %1, selection top 200")
               .arg(before + parts.command->height()));

    controller.chooseTool(vshot::Tool::Rectangle);
    const int after = globalTop(parts.command);
    expect(std::abs(after - before) <= 1,
           "the command bar stays put while the style row appears",
           QStringLiteral("moved from %1 to %2").arg(before).arg(after));
    expect(globalTop(parts.style) < after,
           "the style row grows above the command bar, away from the selection");
}

// Room below but not above: the panel drops below the selection and the style
// row grows further down, away from the selection, without moving the buttons.
void checkPanelBelowKeepsTheStyleRowBelow()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(sessionFor(vshot::LogicalRect{0, 0, 400, 120}));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();
    const ToolbarParts parts = toolbarParts(overlay);
    if (parts.command == nullptr || parts.style == nullptr) {
        expect(false, "the toolbar has a command bar and a style row");
        return;
    }
    const int before = globalTop(parts.command);
    expect(before >= 120, "the command bar drops below the selection",
           QStringLiteral("bar top %1, selection bottom 120").arg(before));

    controller.chooseTool(vshot::Tool::Rectangle);
    const int after = globalTop(parts.command);
    expect(std::abs(after - before) <= 1,
           "the command bar stays put while the style row appears",
           QStringLiteral("moved from %1 to %2").arg(before).arg(after));
    expect(globalTop(parts.style) > after,
           "the style row grows below the command bar, away from the selection");
}

// Hovering a button: the panel draws the tip itself.
//
// Qt's `QToolTip` is a popup window, and this process's layer-shell platform
// integration cannot host one: the compositor never gets a usable popup and Qt
// paints the text into the overlay's own full-output surface, which the user
// meets as a screen-sized block of panel colour.  This must stay a small child
// of the overlay, and no Qt tooltip window may appear.
void checkHoverShowsThePanelTooltip()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(sessionFor(vshot::LogicalRect{150, 150, 100, 100}));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();

    QAbstractButton *button = nullptr;
    const QList<QAbstractButton *> buttons = overlay->findChildren<QAbstractButton *>();
    for (QAbstractButton *candidate : buttons) {
        if (candidate->isVisible() && !candidate->toolTip().isEmpty()) {
            button = candidate;
            break;
        }
    }
    if (button == nullptr) {
        expect(false, "a visible toolbar button with a tooltip");
        return;
    }

    const QPoint local(button->width() / 2, button->height() / 2);
    const int windowsBefore = QApplication::topLevelWidgets().size();
    QHelpEvent hover(QEvent::ToolTip, local, button->mapToGlobal(local));
    QCoreApplication::sendEvent(button, &hover);
    const int windowsAfter = QApplication::topLevelWidgets().size();
    expect(windowsAfter == windowsBefore,
           "a hover raises no tooltip window of Qt's own",
           QStringLiteral("%1 window(s) appeared").arg(windowsAfter - windowsBefore));

    auto *tip = overlay->findChild<QLabel *>(QStringLiteral("vshotTooltip"));
    expect(tip != nullptr && tip->isVisible(), "the panel shows its own tooltip");
    if (tip == nullptr) {
        return;
    }
    expect(tip->text() == button->toolTip(), "the tip carries the hovered button's text");
    // A one-line card that fits the overlay: the failure this guards is a block
    // of panel colour covering the whole surface, not a wide line of text.
    expect(tip->height() < 60 && tip->width() < overlay->width(),
           "the tip is a small card, not a screen-sized block",
           QStringLiteral("tip %1x%2 over %3x%4")
               .arg(tip->width())
               .arg(tip->height())
               .arg(overlay->width())
               .arg(overlay->height()));
    const QPoint tipTopLeft = tip->mapTo(overlay, QPoint(0, 0));
    const QPoint buttonTopLeft = button->mapTo(overlay, QPoint(0, 0));
    expect(std::abs(tipTopLeft.y() - buttonTopLeft.y()) < 200,
           "the tip stays near the button it describes");
}

// The toolbar's Pin button: it finishes the session the way OK does and the
// result asks for the image on the screen instead of on disk.  The pin editor
// is already editing a pin, so it does not offer it again.
void checkPinButtonAsksForTheScreen()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(sessionFor(vshot::LogicalRect{100, 100, 120, 90}));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();

    auto *pin = overlay->findChild<QToolButton *>(QStringLiteral("pinButton"));
    expect(pin != nullptr && pin->isVisible(), "the capture editor offers a Pin button");
    if (pin == nullptr) {
        return;
    }
    // It is drawn as one of the tools -- the same square icon button -- rather
    // than as a text button beside OK, and it carries a pin glyph.
    expect(pin->property("toolButton").toBool(),
           "the Pin button wears the tool buttons' shape");
    expect(pin->toolButtonStyle() == Qt::ToolButtonTextUnderIcon && !pin->icon().isNull(),
           "the Pin button shows a pin icon over its label");
    controller.pin();
    expect(controller.isFinished() && !controller.isCancelled(),
           "the Pin button finishes the session like OK does");
    const QJsonObject result = controller.resultDocument().object();
    expect(result.value(QStringLiteral("status")).toString() == QStringLiteral("ok"),
           "the result is a kept capture");
    expect(result.value(QStringLiteral("pin")).toBool(),
           "the result asks for the image to be pinned");
}

void checkThePinEditorOffersNoPinButton()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(sessionFor(vshot::LogicalRect{0, 0, 200, 160}));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.setPinEditMode(true);
    controller.beginPinEdit();
    auto *pin = overlay->findChild<QToolButton *>(QStringLiteral("pinButton"));
    expect(pin != nullptr && !pin->isVisible(),
           "the pin editor does not offer Pin a second time");
}


// The command bar is two rows: the drawing tools, the actions that act on the
// capture, and the history pair beside them in the corner.  One row of all of
// them came to about a thousand logical pixels -- most of a 1080p output, and
// wider than the panels it has to sit beside -- and a bar that long can only be
// clamped against the screen edge, where it reads as a band across the capture
// rather than a panel on it.  So two rows are the fix.
//
// Which button goes in which row is what this checks.  The rows are filled in
// order and split where the wider of them comes out narrowest, so what has to
// hold is that no other split would be narrower: splitting by kind instead --
// the tools on the first row, the actions on the second -- leaves one row three
// times the length of its neighbour and makes the card as wide as the tools
// alone, which is what it was before.
void checkCommandBarIsTwoRows()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(sessionFor(vshot::LogicalRect{100, 100, 120, 120}));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    controller.beginPresetEdit();
    const ToolbarParts parts = toolbarParts(overlay);
    if (parts.command == nullptr) {
        expect(false, "the toolbar has a command bar");
        return;
    }
    QBoxLayout *card = cardLayout(parts.command);
    QLayout *column = card != nullptr && card->count() > 0 ? card->itemAt(0)->layout() : nullptr;
    expect(column != nullptr && column->count() == 2, "the command bar is two rows",
           QStringLiteral("%1 of them").arg(column != nullptr ? column->count() : -1));
    if (column == nullptr || column->count() != 2) {
        return;
    }
    QLayout *first = commandRow(parts.command, 0);
    QLayout *second = commandRow(parts.command, 1);
    if (first == nullptr || second == nullptr) {
        expect(false, "both rows are laid out as rows");
        return;
    }
    // The buttons of both rows in the order the rows carry them -- the first
    // row's left to right, then the second's.  That is the order they were built
    // in, which is the order the split is taken over, so any split of this list
    // is a split the toolbar could have made instead.
    QVector<QAbstractButton *> buttons;
    int inFirstRow = 0;
    for (QLayout *row : {first, second}) {
        for (int i = 0; i < row->count(); ++i) {
            if (auto *button = qobject_cast<QAbstractButton *>(row->itemAt(i)->widget())) {
                buttons.append(button);
                if (row == first) {
                    ++inFirstRow;
                }
            }
        }
    }
    expect(inFirstRow > 0 && inFirstRow < buttons.size(),
           "and both of them carry buttons",
           QStringLiteral("%1 and %2 of them")
               .arg(inFirstRow)
               .arg(buttons.size() - inFirstRow));
    // The drawing tools are the buttons that say which tool they select; the
    // rest are the actions.  Every one of them is in one of the two rows: the
    // history pair is in the grid in the corner and is not counted here.
    //
    // Eleven, not twelve: the Select tool is gone, so the ten drawing tools are
    // joined by the eyedropper alone.
    int tools = 0;
    for (QAbstractButton *button : buttons) {
        if (!button->property("tool").toString().isEmpty()) {
            ++tools;
        }
    }
    expect(tools == 11, "the drawing tools are among them",
           QStringLiteral("%1 of them").arg(tools));

    auto *paste = parts.command->findChild<QToolButton *>(QStringLiteral("pasteButton"));
    expect(paste != nullptr && (first->indexOf(paste) >= 0 || second->indexOf(paste) >= 0),
           "and so are the actions that act on the capture");
    // The split is the balanced one: every other place the list could have been
    // cut puts a longer row above the shorter one.
    const auto rowWidth = [&buttons, spacing = first->spacing()](int from, int to) {
        int width = 0;
        for (int i = from; i < to; ++i) {
            width += buttons.at(i)->sizeHint().width();
        }
        return to > from ? width + spacing * (to - from - 1) : 0;
    };
    const int count = buttons.size();
    const int wider = std::max(rowWidth(0, inFirstRow), rowWidth(inFirstRow, count));
    int best = -1;
    for (int candidate = 1; candidate < count; ++candidate) {
        const int other = std::max(rowWidth(0, candidate), rowWidth(candidate, count));
        best = best < 0 ? other : std::min(best, other);
    }
    expect(best == wider, "and it is cut where the two rows come out closest to level",
           QStringLiteral("rows %1 and %2, the best split %3")
               .arg(rowWidth(0, inFirstRow))
               .arg(rowWidth(inFirstRow, count))
               .arg(best));
    // Which is to say the two rows are about the same length, rather than one
    // of them carrying the whole bar while the other sits empty.
    const int widest = [&buttons] {
        int width = 0;
        for (QAbstractButton *button : buttons) {
            width = std::max(width, button->sizeHint().width());
        }
        return width;
    }();
    expect(std::abs(rowWidth(0, inFirstRow) - rowWidth(inFirstRow, count)) <= widest,
           "so neither row is a button wider than the other",
           QStringLiteral("%1 against %2")
               .arg(rowWidth(0, inFirstRow))
               .arg(rowWidth(inFirstRow, count)));
    // The two rows are one column, so the card is as wide as the wider of them
    // plus the ends -- not as wide as both rows laid end to end, which is what
    // one row of all of them would have been.
    const int bothRows = first->sizeHint().width() + second->sizeHint().width();
    expect(parts.command->sizeHint().width() < bothRows,
           "and is no wider than its widest row",
           QStringLiteral("bar %1, both rows %2")
               .arg(parts.command->sizeHint().width())
               .arg(bothRows));
    expect(column->sizeHint().width() == std::max(first->sizeHint().width(),
                                                  second->sizeHint().width()),
           "the column is as wide as its wider row",
           QStringLiteral("column %1, rows %2 and %3")
               .arg(column->sizeHint().width())
               .arg(first->sizeHint().width())
               .arg(second->sizeHint().width()));
}

// The four ends -- undo, redo, OK, Cancel -- are pinned to the right-hand edge
// of the card, two rows of two: the history pair on the first row, the two ends
// of the capture on the second, Cancel in the corner.
//
// They used to sit at the end of the second row, which put the two buttons the
// user reaches for most at the far end of the longest row, dragged them along
// whenever the style row changed the panel's width, and left the corner of the
// card empty.  Where they sit now is geometry rather than intent, so it is the
// geometry that is asserted.
void checkTheEndsArePinnedToTheRight()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    // A display-sized output: this check is about the card's own width, and on a
    // small one the panel is clamped and the card never reaches it.
    vshot::OverlayController controller(
        sessionFor(vshot::LogicalRect{200, 300, 800, 500}, QSize(1920, 1080)));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    controller.beginPresetEdit();
    overlay->show();
    // The tool the user's config remembers can open the session with a style row
    // up, which widens the card; this check measures what an unarmed session's
    // card comes to and then what the widest style row does to it, so arm
    // nothing first.
    controller.chooseTool(std::nullopt);
    const ToolbarParts parts = toolbarParts(overlay);
    QGridLayout *grid = endsGrid(parts.command);
    expect(grid != nullptr && grid->count() == 4, "the four ends are one grid",
           QStringLiteral("%1 of them").arg(grid != nullptr ? grid->count() : -1));
    if (parts.command == nullptr || grid == nullptr || grid->count() != 4) {
        return;
    }
    auto *divider = parts.command->findChild<QFrame *>(QStringLiteral("toolbarDivider"));
    auto *undo = parts.command->findChild<QToolButton *>(QStringLiteral("undoButton"));
    auto *redo = parts.command->findChild<QToolButton *>(QStringLiteral("redoButton"));
    auto *confirm = parts.command->findChild<QPushButton *>(QStringLiteral("confirmButton"));
    auto *cancel = parts.command->findChild<QPushButton *>(QStringLiteral("cancelButton"));
    if (undo == nullptr || redo == nullptr || confirm == nullptr || cancel == nullptr) {
        expect(false, "the four ends are on the command bar");
        return;
    }
    const auto cellOf = [grid](QWidget *widget, int *row, int *column) {
        const int index = grid->indexOf(widget);
        if (index < 0) {
            return false;
        }
        int rowSpan = 0;
        int columnSpan = 0;
        grid->getItemPosition(index, row, column, &rowSpan, &columnSpan);
        return true;
    };
    int row = -1;
    int column = -1;
    const bool placed = cellOf(undo, &row, &column);
    expect(placed && row == 0 && column == 0, "undo opens the grid",
           QStringLiteral("at %1,%2").arg(row).arg(column));
    expect(cellOf(redo, &row, &column) && row == 0 && column == 1,
           "with redo beside it", QStringLiteral("at %1,%2").arg(row).arg(column));
    expect(cellOf(confirm, &row, &column) && row == 1 && column == 0,
           "OK opens the row under them", QStringLiteral("at %1,%2").arg(row).arg(column));
    expect(cellOf(cancel, &row, &column) && row == 1 && column == 1,
           "and Cancel sits in the corner", QStringLiteral("at %1,%2").arg(row).arg(column));

    // Pinned right: the block's right edge is the card's own right margin, and
    // everything else on the card is to its left.
    const QRect cancelRect = inCard(parts.command, cancel);
    expect(parts.command->width() - cancelRect.right() - 1 == 2,
           "the ends are against the card's right-hand edge",
           QStringLiteral("card %1 wide, Cancel ends at %2")
               .arg(parts.command->width())
               .arg(cancelRect.right()));
    const QRect undoRect = inCard(parts.command, undo);
    const QRect redoRect = inCard(parts.command, redo);
    const QRect confirmRect = inCard(parts.command, confirm);
    expect(redoRect.y() == undoRect.y() && redoRect.x() > undoRect.x(),
           "redo is directly right of undo",
           QStringLiteral("undo at %1,%2, redo at %3,%4")
               .arg(undoRect.x())
               .arg(undoRect.y())
               .arg(redoRect.x())
               .arg(redoRect.y()));
    expect(confirmRect.y() > undoRect.y() && confirmRect.center().x() == undoRect.center().x(),
           "OK is centred under undo",
           QStringLiteral("undo centred on %1, OK on %2")
               .arg(undoRect.center().x())
               .arg(confirmRect.center().x()));
    const QRect cancelRectInBlock = inCard(parts.command, cancel);
    expect(cancelRectInBlock.center().x() == redoRect.center().x(),
           "Cancel is centred under redo",
           QStringLiteral("redo centred on %1, Cancel on %2")
               .arg(redoRect.center().x())
               .arg(cancelRectInBlock.center().x()));

    // The history pair is a tool button: as tall as the tools, the same label
    // under the same icon, the same hover and press painting.  Its width is the
    // ends' rather than the tools', because the two columns of the block are what
    // the two ends below it need.
    QWidget *firstTool = nullptr;
    QLayout *toolsRow = commandRow(parts.command, 0);
    for (int i = 0; toolsRow != nullptr && i < toolsRow->count() && firstTool == nullptr; ++i) {
        firstTool = toolsRow->itemAt(i)->widget();
    }
    auto *paste = parts.command->findChild<QToolButton *>(QStringLiteral("pasteButton"));
    if (firstTool == nullptr || paste == nullptr) {
        expect(false, "the two rows of buttons are there to measure against");
        return;
    }
    const QRect toolRect = inCard(parts.command, firstTool);
    const QRect pasteRect = inCard(parts.command, paste);
    expect(undoRect.height() == toolRect.height() && redoRect.height() == toolRect.height() &&
               undoRect.width() == redoRect.width(),
           "the history pair is as tall as the tools on the left",
           QStringLiteral("%1x%2 against the tools' %3x%4")
               .arg(undoRect.width())
               .arg(undoRect.height())
               .arg(toolRect.width())
               .arg(toolRect.height()));
    expect(undo->property("toolButton").toBool() &&
               undo->toolButtonStyle() == Qt::ToolButtonTextUnderIcon &&
               !undo->text().isEmpty(),
           "and is drawn like them, label and all", undo->text());

    // The two ends of the capture are a size of their own, and it is not the
    // height of the rows: a text button stretched to the row's own 41 px is a
    // slab with a small word in it, which is what the corner looked like before.
    expect(confirmRect.size() == cancelRectInBlock.size() &&
               confirmRect.width() == undoRect.width(),
           "the two ends of the capture are one size, and the width of the block",
           QStringLiteral("%1x%2 and %3x%4")
               .arg(confirmRect.width())
               .arg(confirmRect.height())
               .arg(cancelRectInBlock.width())
               .arg(cancelRectInBlock.height()));
    expect(confirmRect.height() < toolRect.height(),
           "and shorter than the rows beside them",
           QStringLiteral("%1 against a %2-tall row").arg(confirmRect.height()).arg(toolRect.height()));
    // The block is as wide as the wider of the two ends' own hints, which is
    // what the style sizes to their label plus the padding the corner is drawn
    // at.  Summing the label and a padding here instead is the same arithmetic
    // a second time, and the copy is the one that goes stale: it did, at the
    // style's generic ten pixels rather than the corner's seven, and in English
    // "Cancel" came out wider than its own button, which cut the first and last
    // letters off.  The hint cannot drift from the box it describes.
    const int widestHint = std::max(confirm->sizeHint().width(), cancel->sizeHint().width());
    expect(confirmRect.width() == widestHint,
           "the block is as wide as the wider of the two ends' own hints",
           QStringLiteral("block %1, widest hint %2").arg(confirmRect.width()).arg(widestHint));
    // And that is the point of measuring it there: the label of each fits inside
    // the box it is drawn in, in either language.  The room the style leaves for
    // the text is the one `SE_PushButtonContents` reports, padding and border
    // already taken out.
    const auto labelSlack = [](QPushButton *button) {
        QStyleOptionButton option;
        option.initFrom(button);
        option.text = button->text();
        const QRect label =
            button->style()->subElementRect(QStyle::SE_PushButtonContents, &option, button);
        return label.width() - QFontMetrics(button->font()).horizontalAdvance(button->text());
    };
    const int okSlack = labelSlack(confirm);
    const int cancelSlack = labelSlack(cancel);
    expect(okSlack >= 0 && cancelSlack >= 0,
           "and the label of each fits inside the box it is drawn in",
           QStringLiteral("OK has %1 px to spare, Cancel %2").arg(okSlack).arg(cancelSlack));
    // And the two buttons of a row stand apart by the same gap in both rows: the
    // ends of the capture at the style's own 2 px read as one box with a line
    // down it.
    const int endsGap = cancelRectInBlock.left() - confirmRect.right() - 1;
    const int historyGap = redoRect.left() - undoRect.left() - undoRect.width();
    expect(endsGap == historyGap && endsGap >= 4,
           "the two buttons of a row are the same distance apart in both rows",
           QStringLiteral("%1 between the ends, %2 between the history pair")
               .arg(endsGap)
               .arg(historyGap));
    expect(std::abs(undoRect.center().y() - toolRect.center().y()) <= 1 &&
               std::abs(confirmRect.center().y() - pasteRect.center().y()) <= 1,
           "each centred on the row it belongs to",
           QStringLiteral("undo %1 against %2, OK %3 against %4")
               .arg(undoRect.center().y())
               .arg(toolRect.center().y())
               .arg(confirmRect.center().y())
               .arg(pasteRect.center().y()));

    // The room in front of the block is the room the panel leaves behind it: a
    // block pressed up against the divider reads as though the line were cutting
    // into it, and the corner has the same breathing space on both sides.
    QWidget *panel = parts.command->parentWidget();
    const auto inPanel = [panel](QWidget *widget) {
        return QRect(widget->mapTo(panel, QPoint(0, 0)), widget->size());
    };
    const int before = inPanel(undo).left() - inPanel(divider).right() - 1;
    const int after = panel->width() - 1 - inPanel(cancel).right();
    expect(std::abs(before - after) <= 1,
           "the gap in front of the ends is the one the panel leaves behind them",
           QStringLiteral("%1 before, %2 after").arg(before).arg(after));
    int rightOfRows = 0;
    for (QLayout *row_ : {commandRow(parts.command, 0), commandRow(parts.command, 1)}) {
        for (int i = 0; row_ != nullptr && i < row_->count(); ++i) {
            QWidget *button = row_->itemAt(i)->widget();
            if (button != nullptr) {
                rightOfRows = std::max(rightOfRows, inCard(parts.command, button).right());
            }
        }
    }
    expect(rightOfRows < undoRect.left(),
           "and the two rows of buttons are to their left",
           QStringLiteral("rows end at %1, the ends start at %2")
               .arg(rightOfRows)
               .arg(undoRect.left()));

    // The divider stands between the two, one line for the height of both rows.
    if (divider == nullptr) {
        expect(false, "the card has a divider");
        return;
    }
    const QRect dividerRect = inCard(parts.command, divider);
    expect(dividerRect.left() > rightOfRows && dividerRect.right() < undoRect.left(),
           "the divider stands between the rows and the ends",
           QStringLiteral("rows end at %1, divider at %2..%3, ends start at %4")
               .arg(rightOfRows)
               .arg(dividerRect.left())
               .arg(dividerRect.right())
               .arg(undoRect.left()));
    expect(dividerRect.height() > dividerRect.width() * 10,
           "and is one line rather than one per row",
           QStringLiteral("%1x%2").arg(dividerRect.width()).arg(dividerRect.height()));

    // The same with a style row up.  In a language whose tool labels are short
    // the command bar is narrower than the style row, so raising it widens the
    // card past its own two rows; in a longer one the command bar already sets
    // the card's width and the style row brings nothing.  Either way the same
    // property has to hold: whatever the card gains goes to the stretch in front
    // of the divider, so the ends stay in the corner, the rows stay where they
    // were, and the ends move right by exactly what the card grew by.
    const int narrowCardWidth = parts.command->width();
    controller.chooseTool(vshot::Tool::Rectangle);
    const QRect wideCancel = inCard(parts.command, cancel);
    expect(parts.command->width() - wideCancel.right() - 1 == 2,
           "the ends stay in the corner when the style row raises the card",
           QStringLiteral("card %1 wide, Cancel ends at %2")
               .arg(parts.command->width())
               .arg(wideCancel.right()));
    expect(inCard(parts.command, undo).left() > rightOfRows,
           "with the two rows still to their left",
           QStringLiteral("rows end at %1, the ends start at %2")
               .arg(rightOfRows)
               .arg(inCard(parts.command, undo).left()));
    const int grew = parts.command->width() - narrowCardWidth;
    expect(wideCancel.left() - cancelRect.left() == grew,
           "and the widening went to the gap rather than to the rows",
           QStringLiteral("Cancel at %1 then %2, card %3 then %4")
               .arg(cancelRect.left())
               .arg(wideCancel.left())
               .arg(narrowCardWidth)
               .arg(parts.command->width()));
}

// Every button on the command bar is one size, and that size is one the label
// of the button actually fits in.
//
// The size is read from the style -- the widest label the row has to draw --
// rather than picked by hand, so the promise worth locking down is not a number:
// it is that the eleven tools share one box, that the actions which follow them
// are as tall as that box, that the history pair and the two ends of the capture
// are too, and that no button is narrower or shorter than its own size hint.
// Undo and redo were 32x28 in a 46-tall row, which is what a floating box in the
// middle of a row of buttons looks like; a hand-picked 48x46 for the tools is
// what put about a fifth of slack into the row's width for nothing.
void checkCommandBarButtonsShareOneSize()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(sessionFor(vshot::LogicalRect{100, 100, 120, 120}));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    controller.beginPresetEdit();
    overlay->show();
    auto *surface = overlay->findChild<QWidget *>(QStringLiteral("toolbarCommandSurface"));
    if (surface == nullptr) {
        expect(false, "the toolbar has a command surface");
        return;
    }
    QLayout *tools = commandRow(surface, 0);
    if (tools == nullptr) {
        expect(false, "the command bar is two rows to measure");
        return;
    }
    // The heights are the ones the buttons were built at rather than the ones the
    // layout would hand them: each button is fixed to the row's height, so a
    // shorter one cannot be evened out by the layout later.
    //
    // The two rows are what this measures.  The four ends pinned to their right
    // are a block of their own and a button's height rather than the row's --
    // see `checkTheEndsArePinnedToTheRight`.
    QVector<QAbstractButton *> buttons;
    for (int row = 0; row < 2; ++row) {
        QLayout *layout = commandRow(surface, row);
        for (int i = 0; layout != nullptr && i < layout->count(); ++i) {
            if (auto *button = qobject_cast<QAbstractButton *>(layout->itemAt(i)->widget())) {
                buttons.append(button);
            }
        }
    }
    expect(buttons.size() >= 14, "the two rows carry the tools and the actions",
           QStringLiteral("%1 of them").arg(buttons.size()));
    if (buttons.isEmpty()) {
        return;
    }
    int heightMismatches = 0;
    int squeezed = 0;
    QString worst;
    for (QAbstractButton *button : buttons) {
        if (button->height() != buttons.first()->height()) {
            ++heightMismatches;
            worst = QStringLiteral("%1 is %2 tall in a %3-tall bar")
                        .arg(button->objectName().isEmpty() ? button->text()
                                                            : button->objectName())
                        .arg(button->height())
                        .arg(buttons.first()->height());
        }
        if (button->sizeHint().width() > button->width() ||
            button->sizeHint().height() > button->height()) {
            ++squeezed;
        }
    }
    expect(heightMismatches == 0, "every button on the bar is the height of the bar", worst);
    expect(squeezed == 0, "no button is smaller than its own size hint",
           QStringLiteral("%1 of %2").arg(squeezed).arg(buttons.size()));

    // The tools are one column rather than one per row: the widest of their
    // labels sets the width of all eleven, so a tool that lands in the lower row
    // is the same size as one in the upper and the rows read across rather than
    // stepping.  And the width it comes to is exactly the widest hint -- not
    // more: a size measured before the buttons were polished is measured in the
    // application's font, which leaves the rows wider than any label in them
    // needs.
    int uniform = 0;
    int widest = 0;
    QString toolDetail;
    QSize toolSize;
    int toolCount = 0;
    for (QAbstractButton *button : surface->findChildren<QToolButton *>()) {
        if (button->property("tool").toString().isEmpty()) {
            continue;
        }
        ++toolCount;
        widest = std::max(widest, button->sizeHint().width());
        if (toolSize.isEmpty()) {
            toolSize = button->size();
            continue;
        }
        if (button->size() != toolSize) {
            ++uniform;
            toolDetail = QStringLiteral("%1x%2 next to %3x%4")
                             .arg(button->width())
                             .arg(button->height())
                             .arg(toolSize.width())
                             .arg(toolSize.height());
        }
    }
    expect(toolCount == 11, "the eleven tools are on the bar",
           QStringLiteral("%1 of them").arg(toolCount));
    expect(uniform == 0, "the drawing tools are all one size", toolDetail);
    expect(!toolSize.isEmpty() && toolSize.width() == widest,
           "and exactly as wide as the widest tool label",
           QStringLiteral("%1 against a %2-wide label").arg(toolSize.width()).arg(widest));
}

// What one rendered editor says about the size pill: the rect its own dark
// pixels occupy outside the selection, and the rect the floating toolbar was
// given.
struct PillPlacement {
    QRect pill;
    QRect toolbar;
    int overCapture = 0;
};

// The size pill hangs off the selection rather than over it, and it must not
// end up under the floating toolbar: the toolbar is a child widget, so it
// paints over whatever the overlay's own paint() put in that spot and a pill
// behind it is simply not seen.  Neither failure is visible in the arithmetic
// -- both are questions about where a rounded rectangle lands next to the
// toolbar and the screen edges -- so the check paints the real overlay and
// looks for the pill's own pixels.
//
// The frame is left unset: the editor then draws the chrome over nothing, and
// every opaque, nearly black pixel is either the pill or one of the selection's
// handles, which sit on the selection's edge and are excluded by the bands
// below.
PillPlacement pillPlacement(const vshot::LogicalRect &selection, const QSize &screenSize)
{
    PillPlacement placement;
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return placement;
    }
    vshot::OverlayController controller(sessionFor(selection, screenSize));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return placement;
    }
    overlay->show();
    controller.beginPresetEdit();

    QImage target(overlay->size(), QImage::Format_ARGB32_Premultiplied);
    target.fill(Qt::transparent);
    QPainter painter(&target);
    controller.paint(overlay, &painter);
    painter.end();

    const QRect capture(static_cast<int>(selection.x), static_cast<int>(selection.y),
                        static_cast<int>(selection.width), static_cast<int>(selection.height));
    const QRect clear(capture.adjusted(-12, -12, 12, 12));
    const QRect inside(capture.adjusted(12, 12, -12, -12));
    for (int y = 0; y < target.height(); ++y) {
        for (int x = 0; x < target.width(); ++x) {
            const QColor pixel = target.pixelColor(x, y);
            if (pixel.alpha() < 200 || pixel.red() > 60 || pixel.green() > 60 ||
                pixel.blue() > 60) {
                continue;
            }
            if (inside.contains(x, y)) {
                ++placement.overCapture;
            } else if (!clear.contains(x, y)) {
                placement.pill = placement.pill.isNull() ? QRect(x, y, 1, 1)
                                                         : placement.pill.united(QRect(x, y, 1, 1));
            }
        }
    }
    if (QWidget *bar = overlay->findChild<QWidget *>(QStringLiteral("toolbarCommandSurface"))) {
        QWidget *panel = bar->parentWidget();
        placement.toolbar = QRect(panel->mapTo(overlay, QPoint(0, 0)), panel->size());
    }
    return placement;
}

void checkTheSizePillStaysOutsideTheCapture()
{
    // Room on every side of the selection, so the pill has somewhere to go, and
    // then a selection that reaches both the top and the bottom of the output,
    // where the toolbar has to double back over the capture and the pill is
    // left with the side of the region.
    const vshot::LogicalRect roomy{300, 300, 600, 400};
    const vshot::LogicalRect fullHeight{300, 0, 600, 1200};
    const QSize output(1600, 1200);
    const struct {
        const char *what;
        vshot::LogicalRect selection;
    } cases[] = {
        {"a selection with room around it", roomy},
        {"a selection that spans the output's height", fullHeight},
    };
    for (const auto &item : cases) {
        const PillPlacement placement = pillPlacement(item.selection, output);
        const QString where = QStringLiteral("%1: pill=(%2,%3 %4x%5) toolbar=(%6,%7 %8x%9)")
                                  .arg(QLatin1String(item.what))
                                  .arg(placement.pill.x())
                                  .arg(placement.pill.y())
                                  .arg(placement.pill.width())
                                  .arg(placement.pill.height())
                                  .arg(placement.toolbar.x())
                                  .arg(placement.toolbar.y())
                                  .arg(placement.toolbar.width())
                                  .arg(placement.toolbar.height());
        expect(!placement.pill.isNull(),
               "the size pill is drawn outside the capture", where);
        expect(placement.overCapture == 0, "the size pill never covers the capture", where);
        expect(!placement.toolbar.intersects(placement.pill),
               "the size pill is not painted behind the floating toolbar", where);
    }
}

// The tools' icons come from one switch with no default case, so a tool that is
// added to the enum and not to that switch is a button with a hole in it: a
// blank square sitting in a row, which is what the eyedropper was for one build.
// The split already asserts how many buttons the two rows carry; what this adds
// is that each of the tools among them actually draws something.  It walks both
// rows, because which of them a tool lands in is the split's business.
void checkEveryToolDrawsItsOwnIcon()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        expect(false, "a screen to hang an overlay off");
        return;
    }
    vshot::OverlayController controller(sessionFor(vshot::LogicalRect{100, 100, 120, 120}));
    QString error;
    vshot::CaptureOverlay *overlay = controller.addOverlay(0, screen, &error);
    if (overlay == nullptr) {
        expect(false, "the controller accepts an overlay", error);
        return;
    }
    overlay->show();
    controller.beginPresetEdit();
    const ToolbarParts parts = toolbarParts(overlay);
    if (parts.command == nullptr) {
        expect(false, "the toolbar has a command bar");
        return;
    }
    int counted = 0;
    int blank = 0;
    QStringList names;
    for (QToolButton *button : parts.command->findChildren<QToolButton *>()) {
        const QString tool = button->property("tool").toString();
        if (tool.isEmpty()) {
            continue;
        }
        ++counted;
        names << tool;
        const QImage icon = button->icon().pixmap(20, 20).toImage();
        int inked = 0;
        for (int y = 0; y < icon.height() && inked == 0; ++y) {
            for (int x = 0; x < icon.width(); ++x) {
                if (qAlpha(icon.pixel(x, y)) > 8) {
                    inked = 1;
                    break;
                }
            }
        }
        if (inked == 0) {
            ++blank;
        }
    }
    expect(counted == 11, "the two rows carry the eleven tools",
           QStringLiteral("%1 of them").arg(counted));
    // And the eyedropper is one of them, on a row of its own choosing: a tool
    // that never made it out of the list would be counted here as a hole in the
    // eleven.
    expect(names.contains(QStringLiteral("picker")), "with the eyedropper among them");
    expect(blank == 0, "and every one of them draws an icon",
           QStringLiteral("%1 blank").arg(blank));
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    checkCrampedCaptureKeepsTheButtonsStill();
    checkPanelAboveKeepsTheStyleRowAbove();
    checkPanelBelowKeepsTheStyleRowBelow();
    checkHoverShowsThePanelTooltip();
    checkPinButtonAsksForTheScreen();
    checkThePinEditorOffersNoPinButton();
    checkCommandBarIsTwoRows();
    checkEveryToolDrawsItsOwnIcon();
    checkCommandBarButtonsShareOneSize();
    checkTheEndsArePinnedToTheRight();
    checkTheSizePillStaysOutsideTheCapture();

    if (failures != 0) {
        std::printf("\n%d toolbar checks failed\n", failures);
        return 1;
    }
    std::printf("\nall toolbar checks passed\n");
    return 0;
}

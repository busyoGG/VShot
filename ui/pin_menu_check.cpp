// Offline check for the pinned color card's right-click menu: the formats it
// offers, which row a click picks, and how it goes away.
//
// The menu is driven the way a pointer would drive it -- synthetic mouse and
// key events into a real PinSurface, whose output is rendered into a QImage
// and read back pixel by pixel. Nothing is read out of the surface's own
// state, so what is checked is what the user would see: that a right-click on
// a card opens a box, that the box is a list of rows, that the row under the
// pointer is the one highlighted, and that clicking a row hands the daemon the
// exact string the card prints for that format.
//
// The row bands are found through the hover highlight rather than by repeating
// the menu's layout arithmetic here: the highlighted band is what a click
// would copy, so measuring it in pixels is measuring the thing itself.
//
// Built only with `-DVSHOT_BUILD_CHECKS=ON`. Needs Qt Widgets and the
// offscreen platform plugin, but no compositor: the layer-shell call that
// would need one is never made.

#include "color_card.hpp"
#include "pin_surface.hpp"

#include <QApplication>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPair>
#include <QRegion>
#include <QScreen>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cstdio>

namespace {

int failures = 0;

void expect(bool ok, const char *what, const QString &detail = QString())
{
    if (ok) {
        std::printf("ok    %-52s %s\n", what, qPrintable(detail));
        return;
    }
    std::printf("FAIL  %-52s %s\n", what, qPrintable(detail));
    ++failures;
}

// Paints the whole surface into an image of its own device resolution, the way
// the compositor would show it. The canvas carries the surface's device ratio
// so QPainter's device transform does the logical-to-device mapping; scaling on
// top of it would double the ratio.
QImage paint(vshot::PinSurface &surface)
{
    const qreal ratio = surface.devicePixelRatioF();
    QImage canvas(surface.size() * ratio, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    canvas.setDevicePixelRatio(ratio);
    QPainter painter(&canvas);
    surface.render(&painter);
    painter.end();
    return canvas;
}

QPointF device(const QImage &image, const QPoint &logical)
{
    const qreal ratio = image.devicePixelRatio();
    return QPointF(logical.x() * ratio, logical.y() * ratio);
}

void sendClick(vshot::PinSurface &surface, const QPoint &local, Qt::MouseButton button)
{
    const QPoint global = surface.screen()->geometry().topLeft() + local;
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(global), button, button,
                      Qt::NoModifier);
    QApplication::sendEvent(&surface, &press);
}

void sendMove(vshot::PinSurface &surface, const QPoint &local)
{
    const QPoint global = surface.screen()->geometry().topLeft() + local;
    QMouseEvent move(QEvent::MouseMove, QPointF(local), QPointF(global), Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QApplication::sendEvent(&surface, &move);
}

void sendKey(vshot::PinSurface &surface, int key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(&surface, &press);
}

// A double-click as Qt delivers one: a press, a release, and then a
// double-click event in place of the second press.
void sendDoubleClick(vshot::PinSurface &surface, const QPoint &local)
{
    const QPoint global = surface.screen()->geometry().topLeft() + local;
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(global), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&surface, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), QPointF(global),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&surface, &release);
    QMouseEvent again(QEvent::MouseButtonDblClick, QPointF(local), QPointF(global), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&surface, &again);
}

// The bounding box of everything painted outside the pins, i.e. the menu and
// nothing else, in the same logical pixels the rest of the check speaks.
// Invalid when nothing is painted there.
QRect paintedOutside(const QImage &image, const QRegion &pins)
{
    const qreal ratio = image.devicePixelRatio();
    int left = image.width();
    int right = -1;
    int top = image.height();
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QPoint logical(qFloor(x / ratio), qFloor(y / ratio));
            if (pins.contains(logical)) {
                continue;
            }
            if (qAlpha(image.pixel(x, y)) == 0) {
                continue;
            }
            left = std::min(left, logical.x());
            right = std::max(right, logical.x());
            top = std::min(top, logical.y());
            bottom = std::max(bottom, logical.y());
        }
    }
    if (right < 0) {
        return QRect();
    }
    return QRect(QPoint(left, top), QPoint(right, bottom));
}

bool isColor(const QImage &image, const QPoint &logical, const QColor &wanted, int slack = 2)
{
    const QPointF at = device(image, logical);
    if (at.x() < 0 || at.y() < 0 || at.x() >= image.width() || at.y() >= image.height()) {
        return false;
    }
    const QColor got = image.pixelColor(qRound(at.x()), qRound(at.y()));
    return got.alpha() == 255 && qAbs(got.red() - wanted.red()) <= slack
        && qAbs(got.green() - wanted.green()) <= slack
        && qAbs(got.blue() - wanted.blue()) <= slack;
}

// The badge is a dark box with white text on it, drawn over the blank margin
// at a pin's bottom-right corner. Counting the dark pixels of that corner is
// what finds it -- sampling one point would as likely land on a glyph.
bool hasBadge(const QImage &image, const QRect &region)
{
    int dark = 0;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            const QPointF at = device(image, QPoint(x, y));
            if (at.x() < 0 || at.y() < 0 || at.x() >= image.width() || at.y() >= image.height()) {
                continue;
            }
            const QColor got = image.pixelColor(qRound(at.x()), qRound(at.y()));
            if (got.alpha() == 255 && got.red() + got.green() + got.blue() < 300) {
                ++dark;
            }
        }
    }
    return dark >= 40;
}

// How many pixels of a region two frames disagree about.
int differingPixels(const QImage &one, const QImage &other, const QRect &region)
{
    int count = 0;
    for (int y = region.top(); y <= region.bottom(); ++y) {
        for (int x = region.left(); x <= region.right(); ++x) {
            const QPointF at = device(one, QPoint(x, y));
            if (at.x() < 0 || at.y() < 0 || at.x() >= one.width() || at.y() >= one.height()) {
                continue;
            }
            if (one.pixelColor(qRound(at.x()), qRound(at.y()))
                != other.pixelColor(qRound(at.x()), qRound(at.y()))) {
                ++count;
            }
        }
    }
    return count;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        std::printf("FAIL  no screen\n");
        return 1;
    }
    const QPoint base = screen->geometry().topLeft();
    std::printf("screen %s: %dx%d, %.2g device pixels per logical pixel\n",
                qPrintable(screen->name()), screen->geometry().width(),
                screen->geometry().height(), screen->devicePixelRatio());

    const QString dumpPath = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    const QDir dump(dumpPath);

    vshot::PinSurface surface(screen);
    surface.resize(900, 700);
    surface.show();
    // The offscreen platform never activates a window by itself, and
    // activateWindow() only asks for it (a request offscreen drops), so the
    // harness has to say so -- the menu's keys go to the focused widget.
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    QApplication::setActiveWindow(&surface);
    QT_WARNING_POP

    const QColor color(255, 0, 0);
    const QVector<vshot::ColorRow> rows = vshot::colorCardRows(color);
    vshot::PinSurface::Item card;
    card.id = 1;
    card.image = vshot::renderColorCard(color, 1);
    card.density = 1;
    card.origin = base + QPoint(20, 20);
    card.colorRows = rows;
    // A pin with no formats: the same right-click must leave it alone.
    vshot::PinSurface::Item picture;
    picture.id = 2;
    picture.image = QImage(60, 60, QImage::Format_ARGB32_Premultiplied);
    picture.image.fill(Qt::black);
    picture.origin = base + QPoint(20, 600);

    QVector<QString> copied;
    QVector<quint64> closed;
    bool copySucceeds = true;
    surface.setCopyCallback([&copied, &copySucceeds](quint64, const QString &value) {
        copied.append(value);
        return copySucceeds;
    });
    // A close request is what a double-click on a pin means; recording them is
    // how the check tells "the menu kept the double-click" apart from "it
    // leaked through to the pin underneath".
    surface.setCloseCallback([&closed](quint64 id) { closed.append(id); });
    surface.setPins({card, picture});

    const QRect cardRect(QPoint(20, 20), card.image.size());
    const QRect pictureRect(QPoint(20, 600), picture.image.size());
    // Everything a pin paints lives inside these, its own outline included.
    // The menu is the only thing ever painted outside them, which is what makes
    // "anything out there" a usable measurement of it.
    const QRegion pinAreas = QRegion(cardRect.adjusted(-2, -2, 2, 2))
        | QRegion(pictureRect.adjusted(-2, -2, 2, 2));
    const QColor highlight = surface.palette().color(QPalette::Highlight);
    // Where a badge lands: the blank margin between the last row and the
    // border, in the bottom-right corner of the card. It is empty background
    // until a copy puts a badge over it.
    const QRect badgeArea(cardRect.right() - 130, cardRect.bottom() - 26, 130, 26);

    const auto leave = [&dump, &dumpPath](const QString &name, const QImage &image) {
        if (dumpPath.isEmpty()) {
            return;
        }
        image.save(dump.filePath(name));
    };

    // The right-click anchor: one pixel inside the card's bottom-right corner,
    // which opens the menu just clear of the card (down and right of it).
    const QPoint anchor = cardRect.bottomRight() - QPoint(1, 1);

    std::printf("--- what the right button does ------------------------------------\n");
    expect(!hasBadge(paint(surface), badgeArea), "the card shows no badge until something is copied");
    sendClick(surface, pictureRect.center(), Qt::RightButton);
    QImage frame = paint(surface);
    expect(paintedOutside(frame, pinAreas).isNull(), "a pin with no formats opens no menu");

    // A click that lands on no pin at all is the other half of the same rule.
    sendClick(surface, QPoint(700, 40), Qt::RightButton);
    frame = paint(surface);
    expect(paintedOutside(frame, pinAreas).isNull(), "empty canvas opens no menu");

    sendClick(surface, anchor, Qt::RightButton);
    frame = paint(surface);
    leave(QStringLiteral("pin-menu-open.png"), frame);
    const QRect menu = paintedOutside(frame, pinAreas);
    expect(!menu.isNull(), "right-clicking a color card paints a menu beside it",
           menu.isNull() ? QString() : QStringLiteral("%1x%2 at %3,%4")
                               .arg(menu.width())
                               .arg(menu.height())
                               .arg(menu.left())
                               .arg(menu.top()));
    if (menu.isNull()) {
        std::printf("--- result ---------------------------------------------------------\n");
        std::printf("FAILURES (%d)\n", failures + 1);
        return 1;
    }
    expect(menu.top() > cardRect.bottom() && menu.left() > cardRect.right(),
           "the menu opens clear of the card it belongs to");

    // Where the rows are, found by moving the pointer down the menu. The rows
    // are contiguous and all highlighted the same way, so a highlighted pixel
    // says little on its own; what does say something is that the screen
    // changes exactly when the pointer crosses into the next row. Counting
    // those changes counts the rows, and the frames themselves mark where each
    // one begins -- no layout arithmetic is repeated here.
    std::printf("--- rows, as the pointer finds them --------------------------------\n");
    QVector<int> rowTop;
    QImage walk = paint(surface);
    for (int y = menu.top() + 1; y <= menu.bottom() - 1; ++y) {
        sendMove(surface, QPoint(menu.left() + 5, y));
        const QImage now = paint(surface);
        if (differingPixels(walk, now, menu) > 0) {
            rowTop.append(y);
        }
        walk = now;
    }
    expect(rowTop.size() == rows.size(), "moving down the menu crosses one row per format",
           QStringLiteral("%1 changes for %2 rows").arg(rowTop.size()).arg(rows.size()));

    QVector<int> rowMid;
    QVector<int> rowSpan;
    for (int index = 0; index < rowTop.size(); ++index) {
        const int bottom = index + 1 < rowTop.size() ? rowTop.at(index + 1) - 1 : menu.bottom() - 1;
        rowMid.append((rowTop.at(index) + bottom) / 2);
        rowSpan.append(bottom - rowTop.at(index) + 1);
    }
    if (rowSpan.size() >= 2) {
        const int thinnest = *std::min_element(rowSpan.begin(), rowSpan.end());
        const int thickest = *std::max_element(rowSpan.begin(), rowSpan.end());
        expect(thickest - thinnest <= 1, "every row is the same height",
               QStringLiteral("%1 vs %2 logical px").arg(thinnest).arg(thickest));
    }

    // Hovering a row highlights that row and no other: the column through each
    // row's own blank left margin is the highlight's colour for the row under
    // the pointer and the card's background for every other row.
    for (int index = 0; index < rowMid.size(); ++index) {
        sendMove(surface, QPoint(menu.left() + 5, rowMid.at(index)));
        const QImage lit = paint(surface);
        bool only = isColor(lit, QPoint(menu.left() + 5, rowMid.at(index)), highlight);
        for (int other = 0; other < rowMid.size(); ++other) {
            if (other != index) {
                only = only && !isColor(lit, QPoint(menu.left() + 5, rowMid.at(other)), highlight);
            }
        }
        expect(only, "only the row under the pointer is highlighted",
               QStringLiteral("row %1 `%2`").arg(index).arg(rows.at(index).label));
    }

    std::printf("--- picking a row --------------------------------------------------\n");
    // Start from a closed menu: the walk above left it open, and a right-click
    // on an open menu is a dismissal, not an opening.
    sendKey(surface, Qt::Key_Escape);
    QImage afterPick;
    if (rowMid.size() == rows.size()) {
        for (int index = 0; index < rowMid.size(); ++index) {
            sendClick(surface, anchor, Qt::RightButton); // reopen: a pick closes it
            sendMove(surface, QPoint(menu.left() + 5, rowMid.at(index)));
            sendClick(surface, QPoint(menu.left() + 5, rowMid.at(index)), Qt::LeftButton);
            const bool right = !copied.isEmpty() && copied.last() == rows.at(index).value;
            expect(right, "clicking a row hands over the value the card prints",
                   QStringLiteral("%1 -> `%2`").arg(rows.at(index).label).arg(copied.isEmpty()
                                                                                   ? QString()
                                                                                   : copied.last()));
        }
        afterPick = paint(surface);
        leave(QStringLiteral("pin-menu-picked.png"), afterPick);
        expect(paintedOutside(afterPick, pinAreas).isNull(), "picking a row closes the menu");
        // The badge is the only feedback a copy gets.
        expect(hasBadge(afterPick, badgeArea), "the card shows a badge after the copy");
    }

    std::printf("--- going away -----------------------------------------------------\n");
    const int settled = copied.size();
    const int firstRow = rowMid.isEmpty() ? menu.center().y() : rowMid.first();
    sendClick(surface, anchor, Qt::RightButton);
    sendMove(surface, QPoint(menu.left() + 5, firstRow));
    sendClick(surface, QPoint(menu.center().x(), menu.bottom() + 10), Qt::LeftButton);
    frame = paint(surface);
    expect(copied.size() == settled && paintedOutside(frame, pinAreas).isNull(),
           "a click outside the menu only dismisses it");

    sendClick(surface, anchor, Qt::RightButton);
    sendKey(surface, Qt::Key_Escape);
    frame = paint(surface);
    expect(copied.size() == settled && paintedOutside(frame, pinAreas).isNull(),
           "Esc closes the menu without copying");

    // Enter copies the highlighted row, which is the keyboard half of the same
    // interaction.
    if (rowMid.size() == rows.size() && rows.size() >= 2) {
        sendClick(surface, anchor, Qt::RightButton);
        sendMove(surface, QPoint(menu.left() + 5, rowMid.at(1)));
        sendKey(surface, Qt::Key_Return);
        frame = paint(surface);
        expect(copied.size() == settled + 1 && copied.last() == rows.at(1).value
                   && paintedOutside(frame, pinAreas).isNull(),
               "Enter copies the highlighted row and closes the menu",
               QStringLiteral("`%1`").arg(rows.at(1).value));
    }

    // A copy that does not reach the clipboard still has to go through the
    // daemon and still has to say so: the badge is the only place the user can
    // learn about it. The wording differs from a successful copy's, which is
    // what comparing the two frames shows (the text itself is not readable
    // here, only the fact that the badge is not the same one).
    copySucceeds = false;
    const int beforeFailure = copied.size();
    sendClick(surface, anchor, Qt::RightButton);
    sendClick(surface, QPoint(menu.left() + 5, firstRow), Qt::LeftButton);
    const QImage afterFailure = paint(surface);
    leave(QStringLiteral("pin-menu-failed.png"), afterFailure);
    expect(copied.size() == beforeFailure + 1 && hasBadge(afterFailure, badgeArea),
           "a failed copy still reaches the daemon and reports through the badge");
    expect(differingPixels(afterPick, afterFailure, badgeArea) > 4,
           "the badge says something else when the copy failed");

    // A double-click on a row is a plausible way to use a menu, and its second
    // half arrives after the menu has closed -- right where a pin may be
    // sitting. It must copy once and leave that pin alone rather than read as
    // "double-click closes this pin".
    std::printf("--- a double-click on a row ----------------------------------------\n");
    if (rowMid.size() == rows.size() && rows.size() >= 3) {
        const QPoint spot(menu.left() + 5, rowMid.at(2));
        vshot::PinSurface::Item covered;
        covered.id = 3;
        covered.image = QImage(60, 60, QImage::Format_ARGB32_Premultiplied);
        covered.image.fill(QColor(0, 0, 255));
        covered.origin = base + QPoint(spot.x() - 30, spot.y() - 30);
        surface.setPins({card, picture, covered});
        const int beforeDouble = copied.size();
        sendClick(surface, anchor, Qt::RightButton);
        sendDoubleClick(surface, spot);
        const QImage afterDouble = paint(surface);
        leave(QStringLiteral("pin-menu-double-click.png"), afterDouble);
        expect(copied.size() == beforeDouble + 1 && copied.last() == rows.at(2).value,
               "a double-click on a row copies that row once",
               QStringLiteral("`%1`").arg(rows.at(2).value));
        expect(closed.isEmpty() && isColor(afterDouble, spot, QColor(0, 0, 255)),
               "the pin the menu was covering is not closed by it");
    }

    std::printf("--- result ---------------------------------------------------------\n");
    std::printf("%s (%d failure(s))\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#pragma once

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QVector>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace vshot {

struct LogicalRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    std::int64_t right() const;
    std::int64_t bottom() const;
    bool isEmpty() const;
};

struct OutputSession {
    std::uint32_t id = 0;
    QString name;
    LogicalRect geometry;
    // Global logical rect the overlay surface covers. Region capture leaves it
    // equal to `geometry` (the surface is exactly the frozen output); the pin
    // editor widens it to the whole output so the toolbar can float beside the
    // pinned image instead of on top of it.
    LogicalRect surface;
    // Device pixels per logical pixel of this output: a whole number for a real
    // output (1, 2, ...), but not for the pin editor's virtual one, where it is
    // the zoom the pinned image is shown at -- 160 pixels across a 176-logical-
    // pixel window is 0.909. Rounding that to a whole number is what made the
    // editor refuse a zoomed pin's session, so the ratio is carried as it is.
    double scale = 0.0;
    std::uint32_t pixelWidth = 0;
    std::uint32_t pixelHeight = 0;
    QString path;
    // Set when VShot is already showing this output's frozen frame on an HDR
    // backdrop surface below the overlay: the overlay then draws no image and
    // veils the backdrop, so what shows through the selection is the light the
    // screen showed.  `image` is still loaded, for the mosaic preview.
    bool backdrop = false;
    QImage image;
};

// One window the pointer may snap to in `window-pick` mode. The label is what
// the helper shows in the size pill; it is empty when the source (the pixel
// fallback, or a compositor that reports no titles) has none.
struct WindowCandidate {
    LogicalRect rect;
    QString label;
};

// The UI elements of one window, and the level the picker is on in them.
//
// A window is the first level of picking and its widgets are the second, so
// this is the whole of the second level: the tree the CLI sent, which node the
// highlight is on, and the two wheel gestures that move it.  It is separate
// from the controller because the gestures are the part worth pinning down --
// climbing out of an element has to reach the whole window, and descending has
// to return along the way it came rather than to some other child.
//
// The tree is flat, with each node naming its parent by index, and the CLI
// writes it pre-order so a parent always arrives before its children.
class ElementTree {
public:
    struct Node {
        LogicalRect rect;
        QString label;
        // Index of the parent, -1 at the top of the window's tree.
        int parent = -1;
    };

    // Reads the CLI's `elements` array.  A node with no usable rect is dropped
    // rather than kept: it cannot be pointed at, and a degenerate extent would
    // be drawn across the screen.
    void load(const QJsonArray &array);
    void clear();

    bool isEmpty() const { return nodes_.isEmpty(); }
    int size() const { return nodes_.size(); }

    // The deepest node containing `point`, -1 when the point is in the window
    // but in no element.  The last node containing the point wins, so one drawn
    // over a sibling later in the list is the one picked -- the same rule the
    // window level uses for overlapping windows.
    int indexAt(std::int32_t x, std::int32_t y) const;

    // The node the highlight is on, -1 for the whole window.
    int current() const { return current_; }
    const Node *node(int index) const
    {
        return index >= 0 && index < nodes_.size() ? &nodes_.at(index) : nullptr;
    }

    // Puts the highlight on `index` and forgets the descent: a pointer that
    // moved is aiming at something else, and retracing a path taken from a
    // different element would climb out of the one now under the cursor.
    void select(int index);

    // One wheel step.  `up` climbs towards the window -- repeatedly, until the
    // whole window is selected, which is the level picking started at.
    // `down` replays the descent, so it returns to the node the user left
    // rather than to some other child of it.  False when there was nowhere to
    // go, which is how the caller knows not to repaint.
    bool step(bool up);

private:
    QVector<Node> nodes_;
    int current_ = -1;
    // How the wheel got here, as node indices, deepest last.
    QVector<int> descent_;
};

// The `translate` session object: how a `vshot translate` call is to be made.
// Every field is optional, and an absent one means "use the CLI's own config
// default", which is exactly what an absent whole object means too.
struct TranslateOptions {
    QString from;
    QString to;
    QString provider;
};

struct Session {
    QString mode;
    LogicalRect bounds;
    QVector<OutputSession> outputs;
    // `window-pick` only: the pickable windows, ordered bottom to top, so the
    // last one containing the pointer is the one on top.
    QVector<WindowCandidate> candidates;
    // `region` only: a selection that is already made, so the session opens in
    // editing state instead of waiting for a drag. Window picking resolves a
    // window, then hands the frame it captured to an editing session this way.
    std::optional<LogicalRect> selection;
    // Pin-edit only: id of the pinned image inside the daemon and the daemon
    // socket to reach it. The editor moves the real pin window through this
    // socket instead of drawing a second copy of the image.
    std::uint64_t pinId = 0;
    QString pinSocket;
    // Pin-edit only: how wide the pin's own border is drawn, in logical pixels.
    // The stroke is centred on the image's edge, so it reaches half this far
    // outside the image; the editor counts that band as part of the pin, so a
    // drag that starts on the rim moves the pin instead of reading as a click
    // on the bare canvas beside it. Zero when the session says nothing, which
    // is also what a pin with no border wants.
    std::uint32_t pinBorderWidth = 0;
    // Pin-edit only: which part of the editor to open on, absent for the
    // ordinary annotation editor. `"text"` opens it in the text-selection mode.
    QString action;
    // `region` editing only: whether the toolbar offers the scrolling-capture
    // action. Window editing reuses the same editor on a frame that has
    // nothing to scroll, so the CLI leaves this false there and the action
    // never appears.
    bool longAllowed = false;
    // `translate` only, optional: how the translation is asked for. Absent
    // leaves every choice to the CLI's own config, exactly as an object with
    // no fields does.
    std::optional<TranslateOptions> translate;
    // `translate` only, optional: the absolute path the composited PNG is
    // written to once the user accepts.
    QString resultPath;
    // Pin-edit only, optional: the marks already on the pinned image, in the
    // shape the editor itself reports them, so a re-edit opens on them and they
    // stay editable. The CLI sends them on every edit after the first; a first
    // edit has none and the editor opens blank.
    //
    // Kept as the wire's own JSON rather than parsed here: an `Annotation`
    // carries Qt types a session header has no business naming, and the editor
    // is the only place that knows how to read one.
    QJsonArray annotations;
};

// The little vocabulary the wire's JSON is read with: every number in a session
// or a mark arrives as a `double`, so a reader that wants an integer has to say
// so and check the range, and a value the editor cannot reproduce exactly is
// refused rather than silently rounded.  They live in the header because two
// readers share them -- the session loader and the editor's own mark parser --
// and a mark the loader accepts but the editor cannot place would be a session
// that opens with marks missing.
inline bool jsonFail(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

inline bool jsonInteger(const QJsonValue &value, std::int64_t minimum, std::int64_t maximum,
                        std::int64_t *result)
{
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(minimum) || number > static_cast<double>(maximum)) {
        return false;
    }
    const auto converted = static_cast<std::int64_t>(number);
    if (converted < minimum || converted > maximum) {
        return false;
    }
    *result = converted;
    return true;
}

inline bool jsonSigned32(const QJsonObject &object, const char *key, std::int32_t *result)
{
    std::int64_t value = 0;
    if (!jsonInteger(object.value(QLatin1String(key)), std::numeric_limits<std::int32_t>::min(),
                     std::numeric_limits<std::int32_t>::max(), &value)) {
        return false;
    }
    *result = static_cast<std::int32_t>(value);
    return true;
}

inline bool jsonUnsigned32(const QJsonObject &object, const char *key, std::uint32_t *result,
                           bool requirePositive = false)
{
    std::int64_t value = 0;
    if (!jsonInteger(object.value(QLatin1String(key)), 0,
                     std::numeric_limits<std::uint32_t>::max(), &value) ||
        (requirePositive && value == 0)) {
        return false;
    }
    *result = static_cast<std::uint32_t>(value);
    return true;
}

inline bool jsonRect(const QJsonObject &object, LogicalRect *rect, const QString &label,
                     QString *error)
{
    if (!jsonSigned32(object, "x", &rect->x) || !jsonSigned32(object, "y", &rect->y) ||
        !jsonUnsigned32(object, "width", &rect->width, true) ||
        !jsonUnsigned32(object, "height", &rect->height, true)) {
        return jsonFail(error, label +
                                   QStringLiteral(" must contain integer x/y and positive width/height"));
    }
    if (rect->right() > std::numeric_limits<std::int32_t>::max() ||
        rect->bottom() > std::numeric_limits<std::int32_t>::max()) {
        return jsonFail(error, label + QStringLiteral(" edge overflows int32"));
    }
    return true;
}

bool loadSession(const QString &sessionPath, Session *session, QString *error);

// Parses one `{x,y,width,height[,label]}` candidate object. Shared by the
// session reader and the picker's live candidate refresh, which receives the
// same shape from the CLI while it is open.
bool parseWindowCandidate(const QJsonObject &object, const QString &label,
                          WindowCandidate *candidate, QString *error);

} // namespace vshot

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#include "session_protocol.hpp"

#include "pixel_fd.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <cmath>
#include <functional>
#include <limits>

namespace vshot {
namespace {

bool fail(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

// Device pixels per logical pixel. A real output's density is a whole number,
// but the pin editor's virtual output carries the zoom its image is shown at,
// which is not: the value is read as the number it is rather than rounded to a
// whole one, or a zoomed pin's session would be refused.
bool jsonPositiveNumber(const QJsonObject &object, const char *key, double *result)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || number <= 0.0) {
        return false;
    }
    *result = number;
    return true;
}

bool loadRawImage(OutputSession *output, QString *error)
{
    if (output->pixelWidth > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        output->pixelHeight > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return fail(error, QStringLiteral("output %1 pixel dimensions exceed QImage limits")
                              .arg(output->name));
    }

    const std::uint64_t pixels = static_cast<std::uint64_t>(output->pixelWidth) *
                                 static_cast<std::uint64_t>(output->pixelHeight);
    const std::uint64_t maxBytes = static_cast<std::uint64_t>(std::numeric_limits<qsizetype>::max());
    if (pixels == 0 || pixels > maxBytes / 4U) {
        return fail(error, QStringLiteral("output %1 raw dimensions are too large").arg(output->name));
    }
    const std::uint64_t expected = pixels * 4U;

    // The pixels come over the pixel channel when there is one -- a shared
    // mapping rather than a file -- and the frame is the size of a screen, so
    // that is the path every real session takes.  The file is the fallback for
    // a helper started by hand against a session someone wrote out.
    if (pixelChannelAvailable()) {
        PixelBuffer buffer;
        if (!receivePixelBuffer(kPixelKindSource, &buffer, error)) {
            return false;
        }
        if (!buffer.isValid() || buffer.width != output->pixelWidth ||
            buffer.height != output->pixelHeight) {
            return fail(error,
                        QStringLiteral("output %1 received %2x%3 pixels of an unusable format, "
                                       "expected %4x%5 RGBA8")
                            .arg(output->name)
                            .arg(buffer.width)
                            .arg(buffer.height)
                            .arg(output->pixelWidth)
                            .arg(output->pixelHeight));
        }
        output->image = buffer.toImage();
        if (output->image.isNull()) {
            return fail(error, QStringLiteral("cannot build an image for output %1").arg(output->name));
        }
        return true;
    }

    QFile file(output->path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error, QStringLiteral("cannot open raw file for output %1: %2")
                              .arg(output->name, file.errorString()));
    }
    if (static_cast<std::uint64_t>(file.size()) != expected) {
        return fail(error, QStringLiteral("raw file for output %1 has %2 bytes, expected %3")
                              .arg(output->name)
                              .arg(file.size())
                              .arg(expected));
    }
    const QByteArray bytes = file.readAll();
    if (static_cast<std::uint64_t>(bytes.size()) != expected) {
        return fail(error, QStringLiteral("could not read the complete raw file for output %1")
                              .arg(output->name));
    }

    const int width = static_cast<int>(output->pixelWidth);
    const int height = static_cast<int>(output->pixelHeight);
    const qsizetype stride = static_cast<qsizetype>(width) * 4;
    QImage view(reinterpret_cast<const uchar *>(bytes.constData()), width, height, stride,
                QImage::Format_RGBA8888);
    if (view.isNull()) {
        return fail(error, QStringLiteral("cannot create RGBA8 image for output %1").arg(output->name));
    }
    output->image = view.copy();
    if (output->image.isNull() || output->image.sizeInBytes() != static_cast<qsizetype>(expected)) {
        return fail(error, QStringLiteral("cannot copy RGBA8 image for output %1").arg(output->name));
    }
    return true;
}

} // namespace

bool parseWindowCandidate(const QJsonObject &object, const QString &label,
                          WindowCandidate *candidate, QString *error)
{
    if (!jsonRect(object, &candidate->rect, label, error)) {
        return false;
    }
    // Optional: the pixels/geometry may name what this window is.
    candidate->label = object.value(QStringLiteral("label")).toString();
    return true;
}

std::int64_t LogicalRect::right() const
{
    return static_cast<std::int64_t>(x) + static_cast<std::int64_t>(width);
}

std::int64_t LogicalRect::bottom() const
{
    return static_cast<std::int64_t>(y) + static_cast<std::int64_t>(height);
}

bool LogicalRect::isEmpty() const
{
    return width == 0 || height == 0;
}

bool loadSession(const QString &sessionPath, Session *session, QString *error)
{
    if (session == nullptr) {
        return fail(error, QStringLiteral("session destination is null"));
    }
    QFile file(sessionPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error, QStringLiteral("cannot open session JSON: %1").arg(file.errorString()));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(error, QStringLiteral("invalid session JSON: %1").arg(parseError.errorString()));
    }

    const QJsonObject root = document.object();
    std::int64_t version = 0;
    if (!jsonInteger(root.value(QStringLiteral("version")), 1, 1, &version)) {
        return fail(error, QStringLiteral("session version must be integer 1"));
    }

    Session parsed;
    parsed.mode = root.value(QStringLiteral("mode")).toString();
    if (parsed.mode != QStringLiteral("region") && parsed.mode != QStringLiteral("region-only") &&
        parsed.mode != QStringLiteral("pin-edit") && parsed.mode != QStringLiteral("window-pick") &&
        parsed.mode != QStringLiteral("long-shot") && parsed.mode != QStringLiteral("translate")) {
        return fail(error,
                    QStringLiteral("session mode must be `region`, `region-only`, `pin-edit`, "
                                   "`window-pick`, `long-shot` or `translate`"));
    }
    // A hint session is the small overlay a scrolling capture keeps on screen;
    // it draws no image at all, so its outputs carry geometry only.
    const bool hintSession = parsed.mode == QStringLiteral("long-shot");
    const QJsonValue boundsValue = root.value(QStringLiteral("bounds"));
    if (!boundsValue.isObject()) {
        return fail(error, QStringLiteral("session bounds must be an object"));
    }

    if (!jsonRect(boundsValue.toObject(), &parsed.bounds, QStringLiteral("session bounds"), error)) {
        return false;
    }
    const QJsonValue outputsValue = root.value(QStringLiteral("outputs"));
    if (!outputsValue.isArray() || outputsValue.toArray().isEmpty()) {
        return fail(error, QStringLiteral("session outputs must be a non-empty array"));
    }

    // Pin-edit sessions name the live pin and the daemon socket that owns it;
    // the editor drives the real pin window through it. Region sessions have
    // neither and must not carry them.
    if (parsed.mode == QStringLiteral("pin-edit")) {
        std::int64_t pinId = 0;
        if (!jsonInteger(root.value(QStringLiteral("id")), 0,
                         std::numeric_limits<std::int64_t>::max(), &pinId)) {
            return fail(error, QStringLiteral("pin-edit session `id` must be an unsigned integer"));
        }
        parsed.pinId = static_cast<std::uint64_t>(pinId);
        const QJsonValue socketValue = root.value(QStringLiteral("socket"));
        if (!socketValue.isString() || socketValue.toString().isEmpty()) {
            return fail(error, QStringLiteral("pin-edit session `socket` must be a non-empty string"));
        }
        parsed.pinSocket = socketValue.toString();
        if (!parsed.pinSocket.startsWith(QLatin1Char('/'))) {
            return fail(error, QStringLiteral("pin-edit session `socket` must be an absolute path"));
        }
        // Optional: how wide the pin's border is drawn. Absent means zero, which
        // is the right answer both for a pin with no border and for a session
        // from a helper that does not carry the field yet.
        const QJsonValue borderValue = root.value(QStringLiteral("border_width"));
        if (!borderValue.isUndefined() && !borderValue.isNull()) {
            std::int64_t border = 0;
            if (!jsonInteger(borderValue, 0, 64, &border)) {
                return fail(error, QStringLiteral("pin-edit session `border_width` must be an "
                                                  "integer between 0 and 64"));
            }
            parsed.pinBorderWidth = static_cast<std::uint32_t>(border);
        }
        // Optional: which part of the editor to open on. Absent for the
        // ordinary annotation editor; unknown values are left for the editor to
        // fall back on, so they do not fail the session here.
        const QJsonValue actionValue = root.value(QStringLiteral("action"));
        if (!actionValue.isUndefined() && !actionValue.isNull()) {
            if (!actionValue.isString()) {
                return fail(error,
                            QStringLiteral("pin-edit session `action` must be a string"));
            }
            parsed.action = actionValue.toString();
        }
        // Optional: the marks already on the pinned image, so a re-edit opens on
        // them. Carried as they arrived -- the editor is the only place that
        // knows how to read one -- but their shape is checked here, because a
        // session whose marks are not a list is one the editor cannot use.
        const QJsonValue marksValue = root.value(QStringLiteral("annotations"));
        if (!marksValue.isUndefined() && !marksValue.isNull()) {
            if (!marksValue.isArray()) {
                return fail(error,
                            QStringLiteral("pin-edit session `annotations` must be an array"));
            }
            parsed.annotations = marksValue.toArray();
        }
    }

    // Window picking needs something to pick: the candidates come from the
    // compositor's window list or the pixel fallback, and an empty list would
    // leave the user staring at a frozen screen with nothing to click.
    if (parsed.mode == QStringLiteral("window-pick")) {        const QJsonValue candidatesValue = root.value(QStringLiteral("candidates"));
        if (!candidatesValue.isArray() || candidatesValue.toArray().isEmpty()) {
            return fail(error, QStringLiteral("window-pick session needs a non-empty `candidates` array"));
        }
        const QJsonArray candidates = candidatesValue.toArray();
        parsed.candidates.reserve(candidates.size());
        for (int index = 0; index < candidates.size(); ++index) {
            const QJsonValue value = candidates.at(index);
            if (!value.isObject()) {
                return fail(error, QStringLiteral("candidate %1 must be an object").arg(index));
            }
            WindowCandidate candidate;
            if (!parseWindowCandidate(value.toObject(),
                                      QStringLiteral("candidate %1").arg(index), &candidate,
                                      error)) {
                return false;
            }
            parsed.candidates.push_back(std::move(candidate));
        }
    }

    // A region session may arrive with the selection already made — window
    // picking resolves a window, then hands the frame it captured to an
    // editing session this way, so the user edits the pixels that will be
    // saved. Pin-edit sessions select their whole canvas by construction, and
    // the picker itself never makes a selection, so neither carries one.
    if (parsed.mode == QStringLiteral("region") && root.contains(QStringLiteral("selection"))) {
        const QJsonValue selectionValue = root.value(QStringLiteral("selection"));
        if (!selectionValue.isObject()) {
            return fail(error, QStringLiteral("session selection must be an object"));
        }
        LogicalRect selection;
        if (!jsonRect(selectionValue.toObject(), &selection,
                      QStringLiteral("session selection"), error)) {
            return false;
        }
        if (selection.right() <= parsed.bounds.x || selection.x >= parsed.bounds.right() ||
            selection.bottom() <= parsed.bounds.y || selection.y >= parsed.bounds.bottom()) {
            return fail(error, QStringLiteral("session selection is outside the session bounds"));
        }
        parsed.selection = selection;
    }

    // Region editing only: whether the toolbar offers the scrolling-capture
    // action. Window editing reuses the same editor on a frame that has
    // nothing to scroll, so the CLI leaves the key out there.
    if (parsed.mode == QStringLiteral("region")) {
        const QJsonValue longValue = root.value(QStringLiteral("long_allowed"));
        if (!longValue.isUndefined() && !longValue.isNull()) {
            if (!longValue.isBool()) {
                return fail(error, QStringLiteral("session `long_allowed` must be a boolean"));
            }
            parsed.longAllowed = longValue.toBool();
        }
    }

    // A translate session may name how the translation is asked for and where
    // the composited PNG goes.  Both are optional: an absent key leaves the
    // choice to the CLI's own config, the same thing an empty object means.
    if (parsed.mode == QStringLiteral("translate")) {
        const QJsonValue translateValue = root.value(QStringLiteral("translate"));
        if (!translateValue.isUndefined() && !translateValue.isNull()) {
            if (!translateValue.isObject()) {
                return fail(error, QStringLiteral("session `translate` must be an object"));
            }
            const QJsonObject object = translateValue.toObject();
            TranslateOptions options;
            const auto readOption = [&error, &object](const char *key, QString *out) {
                const QJsonValue value = object.value(QLatin1String(key));
                if (value.isUndefined() || value.isNull()) {
                    return true;
                }
                if (!value.isString() || value.toString().isEmpty()) {
                    return fail(error, QStringLiteral("session `translate.%1` must be a "
                                                      "non-empty string")
                                           .arg(QLatin1String(key)));
                }
                *out = value.toString();
                return true;
            };
            if (!readOption("from", &options.from) || !readOption("to", &options.to) ||
                !readOption("provider", &options.provider)) {
                return false;
            }
            parsed.translate = options;
        }
        const QJsonValue resultValue = root.value(QStringLiteral("result_path"));
        if (!resultValue.isUndefined() && !resultValue.isNull()) {
            if (!resultValue.isString() || resultValue.toString().isEmpty()) {
                return fail(error,
                            QStringLiteral("session `result_path` must be a non-empty string"));
            }
            parsed.resultPath = resultValue.toString();
            if (!parsed.resultPath.startsWith(QLatin1Char('/'))) {
                return fail(error, QStringLiteral("session `result_path` must be an absolute path"));
            }
        }
    }

    const QJsonArray outputs = outputsValue.toArray();
    parsed.outputs.reserve(outputs.size());
    for (int index = 0; index < outputs.size(); ++index) {
        const QJsonValue value = outputs.at(index);
        if (!value.isObject()) {
            return fail(error, QStringLiteral("output %1 must be an object").arg(index));
        }
        const QJsonObject object = value.toObject();
        OutputSession output;
        if (!jsonUnsigned32(object, "id", &output.id, false)) {
            return fail(error, QStringLiteral("output %1 id must be an unsigned integer").arg(index));
        }
        if (!object.value(QStringLiteral("name")).isString() ||
            object.value(QStringLiteral("name")).toString().isEmpty()) {
            return fail(error, QStringLiteral("output %1 name must be a non-empty string").arg(index));
        }
        output.name = object.value(QStringLiteral("name")).toString();
        if (!jsonRect(object, &output.geometry, QStringLiteral("output %1 geometry").arg(index),
                      error)) {
            return false;
        }
        // Optional: the overlay surface rect when it is larger than the output
        // (the pin editor puts its toolbar on the surrounding canvas). Sessions
        // without it cover exactly their own output.
        output.surface = output.geometry;
        if (object.contains(QStringLiteral("surface"))) {
            const QJsonValue surfaceValue = object.value(QStringLiteral("surface"));
            if (!surfaceValue.isObject() ||
                !jsonRect(surfaceValue.toObject(), &output.surface,
                          QStringLiteral("output %1 surface").arg(index), error)) {
                return false;
            }
            if (output.surface.x > output.geometry.x || output.surface.y > output.geometry.y ||
                output.surface.right() < output.geometry.right() ||
                output.surface.bottom() < output.geometry.bottom()) {
                return fail(error, QStringLiteral("output %1 surface does not cover its geometry")
                                      .arg(index));
            }
        }
        // A hint overlay draws nothing, so it has no image to load and no
        // pixel dimensions to agree with: scale and path stay optional there.
        // Every other session draws the frozen frame it was handed.
        if (!hintSession) {
            if (!jsonPositiveNumber(object, "scale", &output.scale) ||
                !jsonUnsigned32(object, "pixel_width", &output.pixelWidth, true) ||
                !jsonUnsigned32(object, "pixel_height", &output.pixelHeight, true)) {
                return fail(error, QStringLiteral("output %1 scale/pixel dimensions are invalid").arg(index));
            }
            // A path is what a hand-written session uses to name its raw file;
            // a session the CLI built carries the pixels over the channel
            // instead, and then there is no file to name.
            const QJsonValue pathValue = object.value(QStringLiteral("path"));
            if (!pixelChannelAvailable()) {
                if (!pathValue.isString() || pathValue.toString().isEmpty()) {
                    return fail(error, QStringLiteral("output %1 path must be a non-empty string").arg(index));
                }
                output.path = pathValue.toString();
            } else if (pathValue.isString()) {
                output.path = pathValue.toString();
            }
            // Optional, and only ever set on a frozen-screen session: the frame
            // is already on screen underneath, shown by VShot's own HDR
            // backdrop surface, so this overlay leaves it out and veils the
            // backdrop instead. The image is still loaded, because mosaics are
            // previewed from its pixels.
            output.backdrop = object.value(QStringLiteral("backdrop")).toBool(false);

            // The dimensions have to be the rect times the scale, so a session
            // that describes a frame of the wrong size is refused rather than
            // drawn against the wrong pixels.  The product is exact for every
            // scale -- a whole number's is exact by construction, and the pin
            // editor's ratio is the quotient the pixel size and the rect were
            // formed from -- so the comparison is against a rounding epsilon
            // rather than a whole pixel, which a wrong frame could hide inside.
            constexpr double kDimensionEpsilon = 1e-6;
            const double expectedWidth =
                static_cast<double>(output.geometry.width) * output.scale;
            const double expectedHeight =
                static_cast<double>(output.geometry.height) * output.scale;
            if (std::abs(expectedWidth - static_cast<double>(output.pixelWidth)) >
                    kDimensionEpsilon ||
                std::abs(expectedHeight - static_cast<double>(output.pixelHeight)) >
                    kDimensionEpsilon) {
                return fail(error, QStringLiteral("output %1 pixel dimensions do not match logical size and scale")
                                      .arg(index));
            }
            if (!loadRawImage(&output, error)) {
                return false;
            }
        }
        parsed.outputs.push_back(std::move(output));
    }

    // Every drawn session shows one frame per output, so each output has to
    // meet the session bounds.  A hint overlay draws nothing: its `bounds` is
    // the region being captured, and the outputs are merely the places it may
    // live, so they do not have to touch that region at all.
    if (!hintSession) {
        for (const OutputSession &output : parsed.outputs) {
            if (output.geometry.right() <= parsed.bounds.x || output.geometry.x >= parsed.bounds.right() ||
                output.geometry.bottom() <= parsed.bounds.y || output.geometry.y >= parsed.bounds.bottom()) {
                return fail(error, QStringLiteral("output %1 does not intersect session bounds").arg(output.name));
            }
        }
    }
    *session = std::move(parsed);
    return true;
}

// --- ElementTree -----------------------------------------------------------

void ElementTree::clear()
{
    nodes_.clear();
    current_ = -1;
    descent_.clear();
}

void ElementTree::load(const QJsonArray &array)
{
    // Built as a real tree first, because collapsing a wrapper changes which
    // node a child hangs off and therefore the parent index every later node
    // carries -- which cannot be patched up in the flat form.
    struct Branch {
        LogicalRect rect;
        QString label;
        QVector<int> children;
    };
    QVector<Branch> branches;
    QVector<int> roots;
    branches.reserve(array.size());
    for (int index = 0; index < array.size(); ++index) {
        const QJsonObject node = array.at(index).toObject();
        const std::int32_t x = static_cast<std::int32_t>(node.value(QStringLiteral("x")).toDouble());
        const std::int32_t y = static_cast<std::int32_t>(node.value(QStringLiteral("y")).toDouble());
        const std::uint32_t width =
            static_cast<std::uint32_t>(node.value(QStringLiteral("width")).toDouble());
        const std::uint32_t height =
            static_cast<std::uint32_t>(node.value(QStringLiteral("height")).toDouble());
        // A node with no extent cannot be pointed at.  Real trees carry a
        // tenth of these -- zero-sized containers, and hidden pages whose size
        // is uninitialized memory -- and drawing one would put a box across the
        // screen, so they are dropped instead.
        if (width == 0 || height == 0) {
            continue;
        }
        Branch branch;
        branch.rect = LogicalRect{x, y, width, height};
        branch.label = node.value(QStringLiteral("label")).toString();
        branches.push_back(std::move(branch));
        const int parent = node.contains(QStringLiteral("parent"))
                               ? node.value(QStringLiteral("parent")).toInt(-1)
                               : -1;
        if (parent >= 0 && parent < branches.size() - 1) {
            branches[parent].children.push_back(branches.size() - 1);
        } else {
            roots.push_back(branches.size() - 1);
        }
    }

    // A wrapper is a node whose only visible child covers exactly the same
    // rectangle -- a filler, or a plain container a toolkit nests for layout.
    // Offering it as a level of its own makes the wheel stop on a box
    // identical to the one under it, which reads as a gesture that did
    // nothing, so the two are one node.  Repeated, because a toolkit nests
    // several in a row.
    auto wrapper = [](const Branch &branch, const QVector<Branch> &all) {
        if (branch.children.size() != 1) {
            return -1;
        }
        const Branch &child = all.at(branch.children.first());
        const bool same = child.rect.x == branch.rect.x && child.rect.y == branch.rect.y &&
                          child.rect.width == branch.rect.width &&
                          child.rect.height == branch.rect.height;
        return same ? branch.children.first() : -1;
    };

    QVector<Node> nodes;
    // Pre-order, so a parent is written before its children.
    std::function<void(int, int)> write = [&](int index, int parent) {
        int taken = index;
        // A named wrapper would lose its name to an anonymous child, so the
        // child inherits it: the name is what the user recognises the level by.
        for (int child = wrapper(branches.at(taken), branches); child >= 0;
             child = wrapper(branches.at(taken), branches)) {
            if (!branches.at(taken).label.isEmpty() && branches.at(child).label.isEmpty()) {
                branches[child].label = branches.at(taken).label;
            }
            taken = child;
        }
        const Branch &branch = branches.at(taken);
        Node node;
        node.rect = branch.rect;
        node.label = branch.label;
        node.parent = parent;
        nodes.push_back(node);
        const int here = nodes.size() - 1;
        for (int child : branch.children) {
            write(child, here);
        }
    };
    for (int root : roots) {
        write(root, -1);
    }

    nodes_ = std::move(nodes);
    current_ = -1;
    descent_.clear();
}

int ElementTree::indexAt(std::int32_t x, std::int32_t y) const
{
    int best = -1;
    for (int index = 0; index < nodes_.size(); ++index) {
        const LogicalRect &rect = nodes_.at(index).rect;
        if (x < rect.x || y < rect.y || x >= rect.right() || y >= rect.bottom()) {
            continue;
        }
        if (best < 0) {
            best = index;
            continue;
        }
        // The *smallest* hit, not the last one.  This is the opposite of the
        // rule the window level uses, and deliberately so: an element tree is
        // full of containers that cover the whole window -- the web-content
        // area, a layout panel -- and a browser's toolbar buttons sit declared
        // after them in pre-order, so "last one wins" picks a full-window box
        // instead of the button under the pointer.
        //
        // Equal areas keep the later one, which is what makes a sibling drawn
        // over another win.
        const LogicalRect &chosen = nodes_.at(best).rect;
        const std::int64_t mine = static_cast<std::int64_t>(rect.width) * rect.height;
        const std::int64_t theirs = static_cast<std::int64_t>(chosen.width) * chosen.height;
        if (mine < theirs || (mine == theirs && index > best)) {
            best = index;
        }
    }
    return best;
}

void ElementTree::select(int index)
{
    current_ = index;
    // Deliberately *not* the node's whole ancestry.  The descent is what the
    // wheel has travelled, and a highlight placed by a pointer move has not
    // travelled at all -- so there is nothing to retrace, and wheeling down
    // does nothing until the user has climbed.  Seeding it with the ancestry
    // would make a descent from a freshly pointed-at node walk all the way back
    // to it, which is no gesture the user made.
    descent_.clear();
}

bool ElementTree::step(bool up)
{
    if (nodes_.isEmpty()) {
        return false;
    }
    if (up) {
        // Remembered so wheeling back down returns to this node rather than to
        // some other child of the parent.
        if (current_ >= 0) {
            descent_.push_back(current_);
            current_ = nodes_.at(current_).parent;
        } else {
            return false;
        }
    } else {
        if (descent_.isEmpty()) {
            return false;
        }
        current_ = descent_.takeLast();
    }
    return true;
}

} // namespace vshot

#include "session_protocol.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

#include <cmath>
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

bool jsonInteger(const QJsonValue &value, std::int64_t minimum, std::int64_t maximum,
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

bool jsonSigned32(const QJsonObject &object, const char *key, std::int32_t *result)
{
    std::int64_t value = 0;
    if (!jsonInteger(object.value(QLatin1String(key)), std::numeric_limits<std::int32_t>::min(),
                     std::numeric_limits<std::int32_t>::max(), &value)) {
        return false;
    }
    *result = static_cast<std::int32_t>(value);
    return true;
}

bool jsonUnsigned32(const QJsonObject &object, const char *key, std::uint32_t *result,
                    bool requirePositive)
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

bool jsonRect(const QJsonObject &object, LogicalRect *rect, const QString &label, QString *error)
{
    if (!jsonSigned32(object, "x", &rect->x) || !jsonSigned32(object, "y", &rect->y) ||
        !jsonUnsigned32(object, "width", &rect->width, true) ||
        !jsonUnsigned32(object, "height", &rect->height, true)) {
        return fail(error, label + QStringLiteral(" must contain integer x/y and positive width/height"));
    }
    if (rect->right() > std::numeric_limits<std::int32_t>::max() ||
        rect->bottom() > std::numeric_limits<std::int32_t>::max()) {
        return fail(error, label + QStringLiteral(" edge overflows int32"));
    }
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
        parsed.mode != QStringLiteral("long-shot")) {
        return fail(error,
                    QStringLiteral("session mode must be `region`, `region-only`, `pin-edit`, "
                                   "`window-pick` or `long-shot`"));
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
            if (!jsonUnsigned32(object, "scale", &output.scale, true) ||
                !jsonUnsigned32(object, "pixel_width", &output.pixelWidth, true) ||
                !jsonUnsigned32(object, "pixel_height", &output.pixelHeight, true)) {
                return fail(error, QStringLiteral("output %1 scale/pixel dimensions are invalid").arg(index));
            }
            if (!object.value(QStringLiteral("path")).isString() ||
                object.value(QStringLiteral("path")).toString().isEmpty()) {
                return fail(error, QStringLiteral("output %1 path must be a non-empty string").arg(index));
            }
            output.path = object.value(QStringLiteral("path")).toString();

            const std::uint64_t expectedWidth = static_cast<std::uint64_t>(output.geometry.width) * output.scale;
            const std::uint64_t expectedHeight = static_cast<std::uint64_t>(output.geometry.height) * output.scale;
            if (expectedWidth != output.pixelWidth || expectedHeight != output.pixelHeight) {
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

} // namespace vshot

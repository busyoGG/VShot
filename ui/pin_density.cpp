// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#include "pin_density.hpp"

#include <QBuffer>
#include <QFile>
#include <QIODevice>
#include <QtEndian>

#include <array>
#include <cmath>

namespace vshot {
namespace {

// The eight bytes every PNG starts with.
constexpr std::array<char, 8> kPngSignature{'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};

// A well-formed header is a handful of chunks. A file that never reaches its
// pixels — malformed, or hostile — must not keep the walk going.
constexpr quint32 kMaxHeaderChunks = 64;

// Two densities are the same when their DPI agree to a twentieth: a writer
// that rounds 96 DPI to whole pixels per metre lands within that.
constexpr double kPiPerMetrePerDpi = 1.0 / 0.0254;
constexpr double kDensityTolerance = 0.05;

// Reads exactly `count` bytes into `buffer`, false when the device runs out.
bool readExact(QIODevice &device, QByteArray &buffer, qint64 count)
{
    if (count < 0) {
        return false;
    }
    buffer = device.read(count);
    return buffer.size() == count;
}

} // namespace

int densityFromPixelDensity(int pixelsPerMeterX, int pixelsPerMeterY, int unit)
{
    // Unit 0 is "aspect ratio only" and says nothing about density; a
    // non-square declaration is not a device density either.
    if (unit != 1 || pixelsPerMeterX <= 0 || pixelsPerMeterX != pixelsPerMeterY) {
        return 0;
    }
    const double ratio = static_cast<double>(pixelsPerMeterX) / kPiPerMetrePerDpi / 96.0;
    const int rounded = qRound(ratio);
    if (rounded < 1 || rounded > 4 || std::abs(ratio - rounded) > kDensityTolerance) {
        return 0;
    }
    return rounded;
}

int pngDeclaredDensity(QIODevice &device)
{
    // Chunks are skipped by seeking over them, and a PNG header is meant to be
    // read that way; a pipe would have to be read through instead.
    if (device.isSequential()) {
        return 0;
    }
    QByteArray chunk;
    constexpr qint64 kSignatureSize = static_cast<qint64>(kPngSignature.size());
    if (!readExact(device, chunk, kSignatureSize) ||
        chunk != QByteArray(kPngSignature.data(), kPngSignature.size())) {
        return 0;
    }
    for (quint32 seen = 0; seen < kMaxHeaderChunks; ++seen) {
        if (!readExact(device, chunk, 8)) {
            return 0; // the header ends before the pixels start
        }
        const quint32 length = qFromBigEndian<quint32>(chunk.constData());
        const QByteArray type = chunk.mid(4, 4);
        if (type == QByteArrayLiteral("IDAT") || type == QByteArrayLiteral("IEND")) {
            return 0; // the pixels begin here and nothing was declared before them
        }
        if (type == QByteArrayLiteral("pHYs")) {
            if (length < 9 || !readExact(device, chunk, 9)) {
                return 0;
            }
            const quint32 x = qFromBigEndian<quint32>(chunk.constData());
            const quint32 y = qFromBigEndian<quint32>(chunk.constData() + 4);
            const int unit = static_cast<unsigned char>(chunk.at(8));
            // A declaration this large cannot be a device density, and the
            // narrowing below would be undefined behaviour.
            constexpr quint32 kSanePixelsPerMetre = 1u << 20;
            if (x > kSanePixelsPerMetre || y > kSanePixelsPerMetre) {
                return 0;
            }
            return densityFromPixelDensity(static_cast<int>(x), static_cast<int>(y), unit);
        }
        // Skip this chunk's data and its CRC without reading either: a big
        // header chunk (an ICC profile, a long comment) costs a seek. A length
        // that does not fit in what is left is a malformed file, not a reason
        // to seek somewhere undefined.
        const qint64 skip = static_cast<qint64>(length) + 4;
        const qint64 available = device.size() - device.pos();
        if (skip > available || !device.seek(device.pos() + skip)) {
            return 0;
        }
    }
    return 0;
}

int pngDeclaredDensity(const QByteArray &png)
{
    // QBuffer does not copy the array it is handed, and the walk only reads.
    QBuffer buffer(const_cast<QByteArray *>(&png));
    if (!buffer.open(QIODevice::ReadOnly)) {
        return 0;
    }
    return pngDeclaredDensity(buffer);
}

int pngDeclaredDensityOfFile(const QString &path)
{
    if (path.isEmpty()) {
        return 0;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }
    return pngDeclaredDensity(file);
}

} // namespace vshot

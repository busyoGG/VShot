// Offline check for the density a PNG declares about itself. That declaration
// is read from the `pHYs` chunk, and the point of reading the chunk rather than
// the decoded value is that 96 DPI has to be seen as the 1x it is: it is what
// vshot writes for a capture from a scale-1 output, and a pin has to keep that
// size when it lands on a denser screen.
//
// This is what makes the check worth having: Qt reports 3780 dots per metre
// both for a PNG that declares 96 DPI and for one that declares nothing, so
// the two are only told apart in the bytes. Built only with
// `-DVSHOT_BUILD_CHECKS=ON`; it needs Qt Gui for the image codecs and nothing
// else -- no compositor, no layer shell, no platform plugin.

#include "pin_density.hpp"

#include <QBuffer>
#include <QByteArray>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QList>
#include <QtEndian>

#include <cstdio>

namespace {

int failures = 0;

void expectDensity(const char *what, int got, int want)
{
    if (got != want) {
        std::printf("FAIL  %-46s -> %d (wanted %d)\n", what, got, want);
        ++failures;
        return;
    }
    std::printf("ok    %-46s -> %d\n", what, got);
}

// PNG's CRC-32, over the chunk type and its data. It matters that this is the
// real thing: the last section hands these bytes to QImage, and libpng refuses
// a chunk whose CRC does not check out.
quint32 crc32(const QByteArray &bytes)
{
    quint32 crc = 0xffffffffu;
    for (const char raw : bytes) {
        crc ^= static_cast<quint8>(raw);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xffffffffu;
}

// A chunk header on its own: the length and the type, with no data behind it.
QByteArray chunkHeader(const char *type, quint32 length)
{
    QByteArray out;
    QDataStream stream(&out, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << length;
    out += QByteArray(type, 4);
    return out;
}

// One PNG chunk: length, type, data, CRC.
QByteArray chunk(const char *type, const QByteArray &data)
{
    const QByteArray kind(type, 4);
    QByteArray out;
    QDataStream size(&out, QIODevice::WriteOnly);
    size.setByteOrder(QDataStream::BigEndian);
    size << quint32(data.size());
    out += kind;
    out += data;
    QDataStream crc(&out, QIODevice::Append);
    crc.setByteOrder(QDataStream::BigEndian);
    crc << crc32(kind + data);
    return out;
}

// A syntax-checkable fake: the reader only walks chunk headers, so the pixel
// data of a PNG built here is deliberately meaningless.
QByteArray png(const QList<QByteArray> &chunks, const QByteArray &afterIdat = QByteArray())
{
    QByteArray out("\x89PNG\r\n\x1a\n", 8);
    out += chunk("IHDR", QByteArray(13, '\0'));
    for (const QByteArray &piece : chunks) {
        out += piece;
    }
    out += chunk("IDAT", QByteArray(4, '\0'));
    out += afterIdat;
    out += chunk("IEND", QByteArray());
    return out;
}

QByteArray pixelDensity(int x, int y, int unit)
{
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(x) << quint32(y) << quint8(unit);
    return chunk("pHYs", data);
}

// Rewrites a real PNG's chunks, dropping its `pHYs` or replacing it with the
// one given. Used to build the two files below out of PNGs Qt itself wrote.
QByteArray rewritePixelDensity(const QByteArray &source, const QByteArray &replacement)
{
    if (source.size() < 8) {
        return QByteArray();
    }
    QByteArray out = source.left(8);
    qint64 offset = 8;
    while (offset + 8 <= source.size()) {
        const QByteArray header = source.mid(offset, 8);
        const quint32 length = qFromBigEndian<quint32>(header.constData());
        const QByteArray type = header.mid(4, 4);
        const qint64 total = 8 + qint64(length) + 4;
        if (type != QByteArrayLiteral("pHYs")) {
            if (type == QByteArrayLiteral("IDAT") && !replacement.isEmpty()) {
                out += replacement;
            }
            out += source.mid(offset, total);
        }
        offset += total;
        if (type == QByteArrayLiteral("IEND")) {
            break;
        }
    }
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    // Optional: a directory to leave the fixture PNGs in, so a reader can
    // compare what Qt reports for each of them.
    const QString dump = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();

    std::printf("--- the chunk, field by field --------------------------------------\n");
    using vshot::densityFromPixelDensity;
    expectDensity("3780 dots per metre, metre unit: 96 DPI", densityFromPixelDensity(3780, 3780, 1), 1);
    expectDensity("7559: 192 DPI", densityFromPixelDensity(7559, 7559, 1), 2);
    expectDensity("11339: 288 DPI", densityFromPixelDensity(11339, 11339, 1), 3);
    expectDensity("15118: 384 DPI", densityFromPixelDensity(15118, 15118, 1), 4);
    expectDensity("a unit of 0 is aspect ratio only", densityFromPixelDensity(7559, 7559, 0), 0);
    expectDensity("non-square is not a device density", densityFromPixelDensity(7559, 3780, 1), 0);
    expectDensity("48 DPI is not a screen density", densityFromPixelDensity(1890, 1890, 1), 0);
    expectDensity("72 DPI is not a screen density", densityFromPixelDensity(2835, 2835, 1), 0);
    expectDensity("300 DPI is a print resolution", densityFromPixelDensity(11811, 11811, 1), 0);
    expectDensity("480 DPI is beyond any screen here", densityFromPixelDensity(18900, 18900, 1), 0);
    expectDensity("zero declares nothing", densityFromPixelDensity(0, 0, 1), 0);

    std::printf("--- walking a PNG, in memory and on disk ---------------------------\n");
    using vshot::pngDeclaredDensity;
    using vshot::pngDeclaredDensityOfFile;
    expectDensity("no pHYs chunk at all", pngDeclaredDensity(png({})), 0);
    expectDensity("pHYs 3780 (a 1x capture)", pngDeclaredDensity(png({pixelDensity(3780, 3780, 1)})), 1);
    expectDensity("pHYs 7559 (a 2x capture)", pngDeclaredDensity(png({pixelDensity(7559, 7559, 1)})), 2);
    expectDensity("pHYs declaring no unit", pngDeclaredDensity(png({pixelDensity(3780, 3780, 0)})), 0);
    expectDensity("pHYs 300 DPI", pngDeclaredDensity(png({pixelDensity(11811, 11811, 1)})), 0);
    expectDensity("pHYs after IDAT is not a declaration",
                  pngDeclaredDensity(png({}, pixelDensity(7559, 7559, 1))), 0);
    expectDensity("pHYs with a truncated payload", pngDeclaredDensity(png({chunk("pHYs", QByteArray(4, '\0'))})), 0);
    expectDensity("an IEND where the header should be", pngDeclaredDensity(png({chunk("IEND", QByteArray())})), 0);
    expectDensity("a length field longer than the file",
                  pngDeclaredDensity(png({chunkHeader("tEXt", 1u << 20)})), 0);
    expectDensity("200 KB of comment before pHYs",
                  pngDeclaredDensity(png({chunk("tEXt", QByteArray(200 * 1024, 'x')), pixelDensity(7559, 7559, 1)})), 2);
    expectDensity("nothing but a chunk cap full of chunks",
                  pngDeclaredDensity(png(QList<QByteArray>(80, chunk("tEXt", QByteArray(1, 'x'))))), 0);
    expectDensity("not a PNG at all", pngDeclaredDensity(QByteArray("GIF89a and then some", 20)), 0);
    expectDensity("empty bytes", pngDeclaredDensity(QByteArray()), 0);
    expectDensity("a path that does not exist", pngDeclaredDensityOfFile(QStringLiteral("/nonexistent/x.png")), 0);
    expectDensity("an empty path", pngDeclaredDensityOfFile(QString()), 0);

    const QString directory = QDir::tempPath();
    const auto writeProbe = [&directory](const QString &name, const QByteArray &bytes) {
        const QString path = directory + QLatin1Char('/') + name;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return QString();
        }
        file.write(bytes);
        file.close();
        return path;
    };
    const QString onePath = writeProbe(QStringLiteral("vshot-density-1x.png"),
                                       png({pixelDensity(3780, 3780, 1)}));
    const QString twoPath = writeProbe(QStringLiteral("vshot-density-2x.png"),
                                       png({pixelDensity(7559, 7559, 1)}));
    expectDensity("the same PNG read from disk, 1x", pngDeclaredDensityOfFile(onePath), 1);
    expectDensity("the same PNG read from disk, 2x", pngDeclaredDensityOfFile(twoPath), 2);

    std::printf("--- why the bytes have to be read at all ---------------------------\n");
    // Real, decodable PNGs: Qt's own writer always emits a pHYs, so the
    // undeclared one is built by dropping that chunk again.
    QImage image(64, 48, QImage::Format_RGBA8888);
    image.fill(Qt::darkGreen);
    QByteArray encoded;
    {
        QBuffer buffer(&encoded);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
    }
    const QByteArray declaringOne = rewritePixelDensity(encoded, pixelDensity(3780, 3780, 1));
    const QByteArray declaringTwo = rewritePixelDensity(encoded, pixelDensity(7559, 7559, 1));
    const QByteArray undeclared = rewritePixelDensity(encoded, QByteArray());
    if (!dump.isEmpty()) {
        const QDir directory(dump);
        const auto leave = [&directory](const QString &name, const QByteArray &bytes) {
            QFile file(directory.filePath(name));
            if (file.open(QIODevice::WriteOnly)) {
                file.write(bytes);
            }
        };
        leave(QStringLiteral("density-1x.png"), declaringOne);
        leave(QStringLiteral("density-2x.png"), declaringTwo);
        leave(QStringLiteral("density-undeclared.png"), undeclared);
    }
    // The decoded value is what the daemon used to go by, and it is not enough
    // to tell a 1x declaration from no declaration at all. What Qt reports for
    // the latter is even context-dependent -- 3780 dots per metre with a
    // QGuiApplication around, 3937 (100 DPI) without one -- while both mean
    // "not a density" to any rule that wants 2..4. What the chunk says is
    // exact.
    const auto reportDecoded = [](const char *what, const QByteArray &bytes) {
        const QImage decoded = QImage::fromData(bytes);
        const int dotsPerMeter = decoded.isNull() ? 0 : decoded.dotsPerMeterX();
        const double ratio = dotsPerMeter / (1.0 / 0.0254) / 96.0;
        std::printf("ok    %-46s -> %d dpm (%.3f x, not a density)\n", what, dotsPerMeter, ratio);
        if (decoded.isNull() || ratio > 1.5) {
            std::printf("FAIL  %-46s expected a decode in the 1x-or-nothing band\n", what);
            ++failures;
        }
    };
    reportDecoded("decoded: the 1x capture", declaringOne);
    reportDecoded("decoded: the undeclared PNG", undeclared);
    expectDensity("chunks of the 1x capture", pngDeclaredDensity(declaringOne), 1);
    expectDensity("chunks of the 2x capture", pngDeclaredDensity(declaringTwo), 2);
    expectDensity("chunks of the undeclared PNG", pngDeclaredDensity(undeclared), 0);
    expectDensity("the 2x capture still reads as 2x decoded",
                  QImage::fromData(declaringTwo).dotsPerMeterX(), 7559);

    std::printf("--- result ---------------------------------------------------------\n");
    std::printf("%s (%d failure(s))\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}

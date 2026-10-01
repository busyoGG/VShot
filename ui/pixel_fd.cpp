// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#include "pixel_fd.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

namespace vshot {
namespace {

// The header: magic, version, kind, format, width, height, and four reserved
// bytes.  The Rust side builds the same layout field by field.
constexpr char kMagic[4] = {'V', 'S', 'P', 'X'};
constexpr quint32 kVersion = 1;
constexpr int kHeaderBytes = 32;

// Reads a little-endian-agnostic field: both sides run on one machine, so the
// header is native-endian on purpose and the magic is what catches a message
// that is not one of ours.
quint32 fieldAt(const char *header, int offset)
{
    quint32 value = 0;
    std::memcpy(&value, header + offset, sizeof(value));
    return value;
}

void putField(char *header, int offset, quint32 value)
{
    std::memcpy(header + offset, &value, sizeof(value));
}

int channelFd()
{
    const QByteArray text = qgetenv(kPixelChannelEnv);
    if (text.isEmpty()) {
        return -1;
    }
    bool ok = false;
    const int fd = text.toInt(&ok);
    if (!ok || fd < 0) {
        return -1;
    }
    // The descriptor has to be *ours* and *open*, not merely a number that
    // parses.  A helper run by hand with the variable left over in the
    // environment -- which is how a check process is started from a shell that
    // ran the CLI -- has a stale number that names some other descriptor, or
    // none at all, and sending a frame down it would either fail at the moment
    // the session ends or, worse, reach an unrelated socket.
    return ::fcntl(fd, F_GETFD) >= 0 ? fd : -1;
}

QString errnoText(const char *what)
{
    return QStringLiteral("%1: %2").arg(QString::fromLatin1(what),
                                        QString::fromLocal8Bit(std::strerror(errno)));
}

// One mapped anonymous file, closed on the way out.
struct Mapping {
    void *address = MAP_FAILED;
    std::size_t length = 0;

    ~Mapping()
    {
        if (address != MAP_FAILED) {
            ::munmap(address, length);
        }
    }
    Mapping() = default;
    Mapping(const Mapping &) = delete;
    Mapping &operator=(const Mapping &) = delete;

    bool valid() const { return address != MAP_FAILED; }
};

// Reads one message: the header, and the descriptor that came with it.
bool receiveMessage(char *header, int *descriptor, QString *error)
{
    const int fd = channelFd();
    if (fd < 0) {
        *error = QStringLiteral("this process was given no pixel channel");
        return false;
    }
    struct iovec iov;
    iov.iov_base = header;
    iov.iov_len = kHeaderBytes;

    char control[CMSG_SPACE(sizeof(int))];
    std::memset(control, 0, sizeof(control));
    struct msghdr message;
    std::memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    const ssize_t read = ::recvmsg(fd, &message, 0);
    if (read < 0) {
        *error = errnoText("failed to read the pixel buffer");
        return false;
    }
    if (read == 0) {
        *error = QStringLiteral("the pixel channel closed before a buffer arrived");
        return false;
    }
    if (read != kHeaderBytes) {
        *error = QStringLiteral("the pixel channel sent %1 bytes where a %2-byte header was due")
                     .arg(read)
                     .arg(kHeaderBytes);
        return false;
    }
    if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
        *error = QStringLiteral("the pixel channel sent a buffer that is not VShot's");
        return false;
    }
    if (fieldAt(header, 4) != kVersion) {
        *error = QStringLiteral("the pixel channel speaks version %1 where %2 was expected")
                     .arg(fieldAt(header, 4))
                     .arg(kVersion);
        return false;
    }
    const struct cmsghdr *controlHeader = CMSG_FIRSTHDR(&message);
    if (controlHeader == nullptr || controlHeader->cmsg_level != SOL_SOCKET ||
        controlHeader->cmsg_type != SCM_RIGHTS) {
        *error = QStringLiteral("the pixel channel sent no descriptor");
        return false;
    }
    std::memcpy(descriptor, CMSG_DATA(controlHeader), sizeof(int));
    return true;
}

} // namespace

const char *const kPixelChannelEnv = "VSHOT_PIXEL_FD";

bool PixelBuffer::isValid() const
{
    if (width == 0 || height == 0 || format != kPixelFormatRgba8888) {
        return false;
    }
    const quint64 expected =
        static_cast<quint64>(width) * static_cast<quint64>(height) * 4ULL;
    return expected == static_cast<quint64>(bytes.size());
}

QImage PixelBuffer::toImage() const
{
    if (!isValid()) {
        return QImage();
    }
    const QImage view(reinterpret_cast<const uchar *>(bytes.constData()),
                      static_cast<int>(width), static_cast<int>(height),
                      static_cast<qsizetype>(width) * 4, QImage::Format_RGBA8888);
    return view.copy();
}

bool pixelChannelAvailable()
{
    return channelFd() >= 0;
}

bool receivePixelBuffer(quint32 expectedKind, PixelBuffer *buffer, QString *error)
{
    char header[kHeaderBytes];
    int descriptor = -1;
    if (!receiveMessage(header, &descriptor, error)) {
        return false;
    }
    // The descriptor is this process's now, whatever happens next.
    struct DescriptorGuard {
        int fd;
        ~DescriptorGuard() { ::close(fd); }
    } guard{descriptor};

    const quint32 kind = fieldAt(header, 8);
    const quint32 format = fieldAt(header, 12);
    const quint32 width = fieldAt(header, 16);
    const quint32 height = fieldAt(header, 20);
    if (kind != expectedKind) {
        *error = QStringLiteral("the pixel channel sent kind %1 where %2 was expected")
                     .arg(kind)
                     .arg(expectedKind);
        return false;
    }
    if (width == 0 || height == 0) {
        *error = QStringLiteral("the pixel channel sent an empty buffer");
        return false;
    }
    const quint64 expected = static_cast<quint64>(width) * static_cast<quint64>(height) * 4ULL;
    if (expected > static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
        *error = QStringLiteral("the pixel buffer is too large to hold");
        return false;
    }
    Mapping mapping;
    mapping.length = static_cast<std::size_t>(expected);
    mapping.address = ::mmap(nullptr, mapping.length, PROT_READ, MAP_PRIVATE, descriptor, 0);
    if (!mapping.valid()) {
        *error = errnoText("failed to map the pixel buffer");
        return false;
    }
    buffer->bytes = QByteArray(static_cast<const char *>(mapping.address),
                               static_cast<qsizetype>(expected));
    buffer->kind = kind;
    buffer->format = format;
    buffer->width = width;
    buffer->height = height;
    return true;
}

bool receivePixelImage(quint32 expectedKind, QImage *image, QString *error)
{
    PixelBuffer buffer;
    if (!receivePixelBuffer(expectedKind, &buffer, error)) {
        return false;
    }
    if (!buffer.isValid()) {
        *error = QStringLiteral("the pixel channel sent a buffer in a format this side cannot read");
        return false;
    }
    *image = buffer.toImage();
    if (image->isNull()) {
        *error = QStringLiteral("could not build an image from the pixel buffer");
        return false;
    }
    return true;
}

bool sendPixelImage(quint32 kind, const QImage &image, QString *error)
{
    const int fd = channelFd();
    if (fd < 0) {
        *error = QStringLiteral("this process was given no pixel channel");
        return false;
    }
    if (image.isNull()) {
        *error = QStringLiteral("there is no image to send");
        return false;
    }
    // One layout on the wire, whatever the painter worked in.
    const QImage rgba = image.format() == QImage::Format_RGBA8888
                            ? image
                            : image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull()) {
        *error = QStringLiteral("could not convert the rendered image to RGBA8");
        return false;
    }
    const quint32 width = static_cast<quint32>(rgba.width());
    const quint32 height = static_cast<quint32>(rgba.height());
    const quint64 length = static_cast<quint64>(width) * static_cast<quint64>(height) * 4ULL;

    const int buffer = ::memfd_create("vshot-pixels", 0);
    if (buffer < 0) {
        *error = errnoText("failed to create a pixel buffer");
        return false;
    }
    struct DescriptorGuard {
        int fd;
        ~DescriptorGuard() { ::close(fd); }
    } guard{buffer};
    if (::ftruncate(buffer, static_cast<off_t>(length)) != 0) {
        *error = errnoText("failed to size the pixel buffer");
        return false;
    }
    void *address = ::mmap(nullptr, static_cast<std::size_t>(length),
                           PROT_READ | PROT_WRITE, MAP_SHARED, buffer, 0);
    if (address == MAP_FAILED) {
        *error = errnoText("failed to fill the pixel buffer");
        return false;
    }
    // Row by row: the image's own stride need not be the tight one the wire
    // format uses.
    for (quint32 row = 0; row < height; ++row) {
        std::memcpy(static_cast<char *>(address) + static_cast<std::size_t>(row) * width * 4,
                    rgba.constScanLine(row), static_cast<std::size_t>(width) * 4);
    }
    Mapping mapping;
    mapping.address = address;
    mapping.length = static_cast<std::size_t>(length);

    char header[kHeaderBytes];
    std::memset(header, 0, sizeof(header));
    std::memcpy(header, kMagic, sizeof(kMagic));
    putField(header, 4, kVersion);
    putField(header, 8, kind);
    putField(header, 12, kPixelFormatRgba8888);
    putField(header, 16, width);
    putField(header, 20, height);

    struct iovec iov;
    iov.iov_base = header;
    iov.iov_len = kHeaderBytes;
    char control[CMSG_SPACE(sizeof(int))];
    std::memset(control, 0, sizeof(control));
    struct msghdr message;
    std::memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    struct cmsghdr *controlHeader = CMSG_FIRSTHDR(&message);
    controlHeader->cmsg_level = SOL_SOCKET;
    controlHeader->cmsg_type = SCM_RIGHTS;
    controlHeader->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(controlHeader), &buffer, sizeof(int));

    const ssize_t sent = ::sendmsg(fd, &message, 0);
    if (sent < 0) {
        *error = errnoText("failed to send the pixel buffer");
        return false;
    }
    return true;
}

} // namespace vshot

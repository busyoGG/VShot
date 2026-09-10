#pragma once

#include <QImage>
#include <QString>
#include <QVector>

#include <cstdint>

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
    std::uint32_t scale = 0;
    std::uint32_t pixelWidth = 0;
    std::uint32_t pixelHeight = 0;
    QString path;
    QImage image;
};

struct Session {
    QString mode;
    LogicalRect bounds;
    QVector<OutputSession> outputs;
    // Pin-edit only: id of the pinned image inside the daemon and the daemon
    // socket to reach it. The editor moves the real pin window through this
    // socket instead of drawing a second copy of the image.
    std::uint64_t pinId = 0;
    QString pinSocket;
};

bool loadSession(const QString &sessionPath, Session *session, QString *error);

} // namespace vshot

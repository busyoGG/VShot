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
};

bool loadSession(const QString &sessionPath, Session *session, QString *error);

} // namespace vshot

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#pragma once

#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <cstdint>
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
    std::uint32_t scale = 0;
    std::uint32_t pixelWidth = 0;
    std::uint32_t pixelHeight = 0;
    QString path;
    QImage image;
};

// One window the pointer may snap to in `window-pick` mode. The label is what
// the helper shows in the size pill; it is empty when the source (the pixel
// fallback, or a compositor that reports no titles) has none.
struct WindowCandidate {
    LogicalRect rect;
    QString label;
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
};

bool loadSession(const QString &sessionPath, Session *session, QString *error);

// Parses one `{x,y,width,height[,label]}` candidate object. Shared by the
// session reader and the picker's live candidate refresh, which receives the
// same shape from the CLI while it is open.
bool parseWindowCandidate(const QJsonObject &object, const QString &label,
                          WindowCandidate *candidate, QString *error);

} // namespace vshot

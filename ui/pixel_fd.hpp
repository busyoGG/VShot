// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

namespace vshot {

// One pixel buffer, in the layout both sides agree on: a 32-byte header naming
// the size and the format, and the pixels themselves in a `memfd` the header
// arrives with.  See `src/pixel_fd.rs`, which is the other half of this and
// where the layout is described in full.
//
// The buffer never touches the filesystem: the sender writes a sealed anonymous
// file and passes the descriptor with `SCM_RIGHTS`, and this side maps it.  A
// session's image arrives this way, and the rendered result goes back the same
// way, so neither side has a temporary file to name or to clean up.

// What the buffer holds.  A result is never mistaken for a source image.
constexpr quint32 kPixelKindSource = 0;
constexpr quint32 kPixelKindResult = 1;

// Eight-bit RGBA, straight alpha, one byte per channel.
constexpr quint32 kPixelFormatRgba8888 = 1;

// The environment variable the CLI sets to the descriptor it installed the
// channel on.  It is both the flag that a channel exists and the number to use:
// a helper started by hand has no channel, and neither does one whose fd 3
// happens to be some unrelated descriptor a shell left open.
extern const char *const kPixelChannelEnv;

// One buffer as it arrived, with the pixels still in the sender's mapping.
struct PixelBuffer {
    QByteArray bytes;
    quint32 kind = 0;
    quint32 format = 0;
    quint32 width = 0;
    quint32 height = 0;

    bool isValid() const;
    // The pixels as an image, copied out of the mapping so the buffer can go.
    QImage toImage() const;
};

// Whether the CLI installed a pixel channel for this process.
bool pixelChannelAvailable();

// Reads one buffer.  `expectedKind` is what the caller is waiting for; a buffer
// of another kind is refused rather than handed over misread.
bool receivePixelBuffer(quint32 expectedKind, PixelBuffer *buffer, QString *error);

// Reads one buffer and answers it as an image.
bool receivePixelImage(quint32 expectedKind, QImage *image, QString *error);

// Hands one image back as a buffer of `kind`.
bool sendPixelImage(quint32 kind, const QImage &image, QString *error);

} // namespace vshot

#pragma once

#include <QString>

#include <functional>

namespace vshot {

// Runs the resident pin daemon: listens on a local socket, owns every pinned
// image surface, and dispatches the add/toggle/show/hide/close/quit/list
// commands. Blocks until quit (event loop driven).
int runPinServer(const QString &socketPath);

} // namespace vshot

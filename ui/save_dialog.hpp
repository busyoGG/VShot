#pragma once

#include <QString>

namespace vshot {

// The save dialog the pin daemon needs, shown in a process of its own and
// reporting its answer on stdout as one JSON object.
//
// It cannot be shown from the process that wants it: the daemon is a
// layer-shell client, and a layer surface cannot be the parent of a popup --
// which is exactly what a file dialog is. A helper started with the layer-shell
// integration cleared gets an ordinary toplevel the compositor can place and
// focus. The pixels never travel: the caller picks a path here and does the
// writing itself.

// `{"ok":true,"path":"..."}`, or `{"ok":false}` when the user cancelled.
int runSaveDialog(const QString &suggestedPath);

} // namespace vshot

#pragma once

#include <QString>

namespace vshot {

// The two file dialogs vshot needs, each shown in a process of its own and each
// reporting its answer on stdout as one JSON object.
//
// They cannot be shown from the process that wants them. The pin daemon and the
// capture overlay are both layer-shell clients, and a layer surface cannot be
// the parent of a popup -- which is exactly what a file dialog is. A helper
// started with the layer-shell integration cleared gets an ordinary toplevel the
// compositor can place and focus. The pixels never travel: the caller picks a
// path here and does the writing or reading itself.

// `{"ok":true,"path":"..."}`, or `{"ok":false}` when the user cancelled.
int runSaveDialog(const QString &suggestedPath);

// The same shape, for picking an existing image to open.
int runOpenDialog(const QString &suggestedPath);

} // namespace vshot

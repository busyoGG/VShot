#pragma once

#include <QString>

namespace vshot {

// Runs one interactive pin-edit round in this helper process: loads the
// pin-edit session written by the daemon, shows the annotation editor in a
// window placed over the pin, and writes the result JSON to stdout when the
// user confirms (or `{"status":"cancelled"}` on Escape). Blocks until the
// editor closes. Returns a non-zero exit code on protocol errors.
int runPinEdit(const QString &sessionPath);

} // namespace vshot

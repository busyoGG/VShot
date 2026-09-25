// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#pragma once

#include <QString>

class QDialog;

namespace vshot {

// Shows the settings window: one plain top-level dialog for the two sections
// of the shared config file, the annotation editor's remembered style and the
// command-line defaults. Saving writes the file, so the next capture opens with
// what is chosen here and the next CLI run falls back to it.
//
// It is an ordinary window on purpose -- the capture overlay's layer surface
// cannot be one -- so it needs no compositor support beyond a window. Returns a
// non-zero exit code when the dialog could not be shown at all.
int runSettingsWindow();

// Builds the same dialog without showing it, for the offline check that drives
// its widgets: every field the window offers has to be the field it saves, and
// a check that reads the widgets back is the only way to tell a mis-wired one
// from a correct one. The caller owns the dialog.
QDialog *createSettingsDialog();

} // namespace vshot

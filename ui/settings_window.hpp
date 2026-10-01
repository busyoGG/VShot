// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#pragma once

#include "shortcuts.hpp"

#include <QString>

#include <functional>

class QDialog;
class QWidget;

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

// The same dialog, over bindings the caller owns rather than the ones on disk.
//
// The key-binding rows open a dialog of their own, and a check that drives that
// dialog has to be able to look at what it left behind -- a config file would
// do it, but it would also mean the check could pass by reading back a file
// nobody wrote. The dialog keeps this reference for its lifetime, so it has to
// outlive the returned dialog.
QDialog *createSettingsDialogForShortcuts(ShortcutPreferences *shortcuts);

/// Builds one action's key-binding editor over `preferences`, which it edits in
/// place and which has to outlive the dialog.
///
/// `parent` is null in the offline check, which builds the dialog without a
/// settings window around it.  `ask` is what the dialog uses to put a conflict
/// to the user; a check substitutes one so it can answer both ways without a
/// modal prompt on an offscreen platform.
QDialog *createShortcutEditorDialog(ShortcutAction action, ShortcutPreferences *preferences,
                                    QWidget *parent = nullptr,
                                    std::function<bool(const QString &)> ask = {});

} // namespace vshot

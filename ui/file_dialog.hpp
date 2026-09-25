// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

class QFileDialog;

namespace vshot {

// The two file dialogs vshot needs, each shown in a process of its own and each
// reporting its answer on stdout as one JSON object.
//
// They run in a process of their own because the caller cannot host them: the
// pin daemon and the capture overlay are layer-shell clients, and the dialog
// has to be one too.  Every surface vshot shows is a layer surface, and the
// compositor draws those above all ordinary windows -- so a dialog opened as a
// toplevel from inside a capture ends up under the frozen frame the user is
// working on: invisible, and unreachable by the pointer, which is what made
// the paste and the save look like dead buttons.  The protocol orders the
// surfaces of a layer by map time, so a dialog mapped after the overlay stacks
// above it.  The pixels never travel: the caller picks a path here and does the
// writing or reading itself.

// `{"ok":true,"path":"..."}`, or `{"ok":false}` when the user cancelled.
// `screenName` is the output the dialog should open on, as Qt names it; empty
// falls back to the primary one.
int runSaveDialog(const QString &suggestedPath, const QString &screenName = QString());

// The same shape, for picking an existing image to open.
int runOpenDialog(const QString &suggestedPath, const QString &screenName = QString());

// Builds the same dialog without showing it, for the offline check that drives
// its widgets.  The check has to be able to tell a dialog that was dressed
// (stylesheet applied, thumbnail grid set) from one that was not, and the only
// honest way to do that is on the widget tree this function returns rather than
// on a second dialog built to look like it.  The caller owns the dialog.
QFileDialog *createFileDialog(bool saving, const QString &suggestedPath);

// One place in the file dialogs' sidebar.
struct Place {
    QString path;  //< an absolute local directory
    QString title; //< what the file manager calls it, empty when it has no name
};

// Reads the places out of the given bookmark files rather than out of the ones
// this machine happens to have, so the check can drive the reader with fixtures
// of its own: the formats are the file managers', not ours, and getting one
// wrong is invisible until a user's sidebar comes up with a row that does
// nothing.
//
// KDE's file is XBEL; GTK's is one `file://` URI per line.  Which is which is
// decided by the `.xbel` suffix, which is what the two actually differ in.
QList<Place> readPlaces(const QStringList &bookmarkFiles);

// The files this machine's file managers record their bookmarks in, whether or
// not any of them exists: the KDE one, then GTK 3's and GTK 4's.
QStringList bookmarkFiles();

} // namespace vshot

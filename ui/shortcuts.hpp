// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#pragma once

#include <QKeySequence>
#include <QString>
#include <QVector>

namespace vshot {

/// The editor's keyboard actions, in the order the settings page lists them.
///
/// A shortcut is bound to an action rather than to a key, because the same
/// action is reachable from two places: the toolbar has a button for most of
/// these, and a button that does something the keyboard cannot reach is a
/// button whose label the user has to read every time.  The id is what the
/// file writes, so renaming a label or rebinding a key never orphans a value a
/// user has already saved.
enum class ShortcutAction {
    /// Ends the session and writes the result.
    Confirm,
    /// Throws the capture away.
    Cancel,
    /// Takes back the last change to the marks.
    Undo,
    /// Puts the last undone change back.
    Redo,
    /// Puts the composite on the clipboard.
    Copy,
    /// Copies the range the text-selection mode or the translate overlay has
    /// picked up.  A separate action from `Copy`, because both are live in the
    /// same session and neither is what the other copies: this one takes the
    /// text the user selected, that one the picture it was read off.
    CopyText,
    /// Puts the image on the clipboard, unbothered by the marks.
    Paste,
    /// Picks every mark up at once.
    SelectAll,
    /// Drops every mark.
    SelectNone,
    /// Walks the focus from one mark to the next.
    NextMark,
    /// Walks it back.
    PreviousMark,
    /// Deletes the mark the focus is on.
    Delete,
    /// Copies the colour under the cursor while the magnifier is up.
    CopyColor,
    /// Makes the colour under the cursor the armed tool's while the magnifier
    /// is up.
    AdoptColor,
    /// Shows the magnifier for two seconds without a drag.
    ShowMagnifier,
    /// Holds the aspect ratio while a resize is in progress.
    PreserveAspect,
    /// Takes a coarser step while walking the cursor with the keyboard.
    CoarseStep,
    /// Picks an existing mark up: while it is held a press on a mark's body
    /// moves it instead of drawing with the armed tool.  A modifier rather than
    /// a key, because the state has to be on *while* the press is made, and
    /// because entering it must not touch which tool is armed.
    SelectMark,
    /// Walks the cursor one pixel left.
    CursorLeft,
    /// Walks it one pixel right.
    CursorRight,
    /// Walks it one pixel up.
    CursorUp,
    /// Walks it one pixel down.
    CursorDown,
    kActionCount,
};

/// The binding of one action: what it is called, what it is bound to, and
/// whether the user may rebind it.
struct ShortcutBinding {
    /// The name the config file and the settings page use. Lower-case words
    /// joined by `-`, so a hand edit reads the way the rest of the file does.
    QString id;
    /// The label the settings page shows; the tooltip of the toolbar button
    /// that does the same thing carries it too.
    QString label;
    /// The key the action is bound to when the file says nothing.
    ///
    /// Two characters or one: a modifier letter (`C`, `S`, `A`, `M`) followed
    /// by a key name, or a bare key name. `C` is Ctrl, `S` Shift, `A` Alt and
    /// `M` Meta; the key name is what `QKeySequence::fromString` understands
    /// (`Tab`, `Del`, `Left`, ...), so a file the user wrote by hand and the
    /// string the settings page shows are the same spelling.
    QString defaultKeys;
    /// Whether the settings page offers this one for rebinding.
    ///
    /// The modifiers a drag reads -- aspect ratio and the coarse step -- are
    /// held *while* another key is already down, and a plain `QKeySequence`
    /// cannot say "Alt alone": it would fire on Alt+Escape too.  They are
    /// listed so the user can see what they are, but not edited, rather than
    /// being edited into something that cannot be delivered.
    bool rebindable = true;
    /// The hint under the settings row: what the action does, in one line.
    QString hint;
};

/// Every action, in `ShortcutAction` order. The table is the single place a
/// binding is spelled out, so the settings page, the file reader and the
/// editor's own key handling cannot disagree about which id means what.
const QVector<ShortcutBinding> &shortcutBindings();

/// One action's binding, or a null one for an out-of-range id.
const ShortcutBinding &shortcutBinding(ShortcutAction action);

/// The editor's keyboard bindings, read out of the config file.
///
/// The defaults in here are the ones the editor used to hard-code, so a file
/// that says nothing leaves every key where it always was. A binding the file
/// spells in a way `QKeySequence` does not understand falls back to the
/// default rather than to nothing: an unbound Undo is worse than a
/// misspelled one.
struct ShortcutPreferences {
    QVector<QKeySequence> keys;
    /// Whether the user cleared each action's binding. A flag rather than an
    /// empty `QKeySequence`, because "cleared" and "the file says nothing" have
    /// to be different answers: the second falls back to the default, the first
    /// is the user asking for no key at all.
    QVector<bool> unbound;
    /// The keys of each action, one entry per alternative, in the order the
    /// file and the settings page list them. Empty for an action with no key.
    ///
    /// A list rather than the alternatives of one `QKeySequence`, because the
    /// two are not the same thing: a sequence of several combinations is what
    /// `QKeySequence` calls a *chord* -- Ctrl+K then Ctrl+C, pressed in that
    /// order -- while these are alternatives, any one of which fires.  Qt reads
    /// and writes both with the same comma-separated spelling, so the file is
    /// unchanged; only the meaning of an entry with a comma in it is now the
    /// one the settings page has always described.
    QVector<QVector<QKeySequence>> bindings;

    ShortcutPreferences();

    /// Whether `action` is bound to anything. False when the user has cleared
    /// it: an action with no key is reachable from the toolbar alone, which is
    /// a thing the user may well want for one they keep hitting by accident.
    bool hasKeys(ShortcutAction action) const;
    /// Every key `action` answers to, in the order the file lists them.  Empty
    /// for an action the user cleared.
    QVector<QKeySequence> keysFor(ShortcutAction action) const;
    /// The spelling the file stores for `action`: the alternatives joined by
    /// ", ", which is the spelling the settings page shows and the one
    /// [`setText`] reads back.  Empty for an action with no key.
    QString textFor(ShortcutAction action) const;
    /// Replaces `action`'s binding with `keys`. An empty list is accepted: it
    /// is what the settings window writes when the user clears one.
    void setKeys(ShortcutAction action, const QVector<QKeySequence> &keys);
    /// Parses the spelling [`textFor`] writes. An unreadable value is rejected,
    /// which leaves the default standing.
    void setText(ShortcutAction action, const QString &text);
    /// Adds `key` to `action`'s binding, unless that action already answers to
    /// it -- a second entry for the same combination would be a duplicate row
    /// in the settings dialog and a second thing to remove for one key.
    /// Returns false when nothing was added because it was already there.
    bool addKey(ShortcutAction action, const QKeySequence &key);
    /// Takes `key` out of `action`'s binding, leaving the rest.  Clearing the
    /// last one is how the user says "this action has no key".  Returns false
    /// when `action` did not answer to it.
    bool removeKey(ShortcutAction action, const QKeySequence &key);
    /// Which action `key` is bound to, `kActionCount` when none is.  When more
    /// than one answers -- the table ships with two, `A` on both the cursor and
    /// the magnifier's adopt-colour -- the *later* action wins, which is the
    /// one the editor's own key handler would reach last.
    ShortcutAction owner(const QKeySequence &key) const;
    /// The same, for every action but `except`: what a conflict prompt is
    /// asked when the user sets a key on the action that already holds it.
    ShortcutAction ownerOtherThan(ShortcutAction except, const QKeySequence &key) const;
    /// Whether `event` is the binding of `action`.
    ///
    /// Compared through `QKeySequence::matches` rather than for equality,
    /// because a key event carries the modifiers that are down at the time and
    /// a `QKeySequence` of one key with no modifier must not match Ctrl+that
    /// key -- which is what an equality test on the raw integer would do.
    bool matches(ShortcutAction action, const QKeySequence &pressed) const;
    /// Whether `modifiers` includes the modifier `action` is bound to.
    ///
    /// The two held-modifier actions -- aspect ratio and the coarse cursor
    /// step -- are read from the state of the keyboard while another input is
    /// already under way, not from a key event of their own. They are looked up
    /// here so what the settings page prints as the key for one is the key the
    /// editor actually reads, even though the binding cannot be changed.
    bool held(ShortcutAction action, int modifiers) const;
    /// Whether `pressed` is the binding of `action` with `consumed` removed
    /// from the modifiers it carries.
    ///
    /// For the one modifier the editor reads as part of another action rather
    /// than as part of a key: Shift makes a cursor step ten pixels instead of
    /// one, so Shift+Left is still "left" -- and a match that demanded the
    /// binding spell Shift too would make the coarse step unreachable, since
    /// every cursor binding would need a Shift twin beside it.
    bool matches(ShortcutAction action, const QKeySequence &pressed, int consumed) const;
};

/// The editor's half of the config file's `shortcuts` section. Missing, absent
/// or malformed entries keep the built-in binding.
ShortcutPreferences loadShortcutPreferences();

/// Writes the editor's half, leaving everything else in the file alone.
/// Returns false on the same terms as `saveConfig`.
bool saveShortcutPreferences(const ShortcutPreferences &preferences);

} // namespace vshot

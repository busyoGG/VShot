// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

// The editor's keyboard bindings: which action is bound to which key, what the
// file calls each one, and how a pressed key is compared against a binding.
//
// The bindings used to be written into the key handler itself, one `if (key ==
// Qt::Key_S)` per action, which meant a user who wanted Ctrl+C for something
// else had nowhere to say so. Here they are data: a table of actions, a
// spelling the config file stores, and one comparison every caller shares.
//
// The comparison is the part worth being careful about. `QKeySequence::matches`
// takes the *stored* sequence as the subject, and a stored sequence with more
// than one alternative ("Return, Enter") matches nothing when a single-key
// sequence is passed to it -- so the pressed key has to be the subject and each
// stored alternative the object. Doing it the other way round silently makes
// every multi-key binding unreachable, which is the kind of bug that shows up
// as "Enter does nothing" and no test failing.

#include "shortcuts.hpp"

#include "config.hpp"
#include "i18n.hpp"

#include <QJsonObject>
#include <QKeyCombination>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace vshot {
namespace {

// The table. The order is `ShortcutAction`'s, and `shortcutBinding` indexes it
// by the enum value, so the two can only disagree if one of them is edited
// alone -- which the settings page would show as a row with no label.
//
// `defaultKeys` is the spelling `QKeySequence::fromString` reads, which for a
// modifier is the same word Qt prints: "Ctrl+S", "Shift+Tab", "Alt+Del". The
// bare letters (C, A, W, S, D) are spelled as themselves, so what the settings
// page shows is what a hand-written file has to say.
const QVector<ShortcutBinding> &bindings()
{
    static const QVector<ShortcutBinding> table = {
        {QStringLiteral("confirm"), uiTr("Confirm the capture"),
         QStringLiteral("Return, Enter"), true,
         uiTr("Accepts the capture and writes it out")},
        {QStringLiteral("cancel"), uiTr("Discard the capture"), QStringLiteral("Esc"), true,
         uiTr("Throws the capture away")},
        {QStringLiteral("undo"), uiTr("Undo"), QStringLiteral("Ctrl+Z"), true,
         uiTr("Takes back the last change to the marks")},
        {QStringLiteral("redo"), uiTr("Redo"), QStringLiteral("Ctrl+Y, Ctrl+Shift+Z"), true,
         uiTr("Puts the last undone change back")},
        {QStringLiteral("copy"), uiTr("Copy the result"), QStringLiteral("Ctrl+S"), true,
         uiTr("Puts the capture and its marks on the clipboard")},
        {QStringLiteral("copy-text"), uiTr("Copy the selected text"),
         QStringLiteral("Ctrl+C"), true,
         uiTr("Copies the text the text-selection mode or a translation picked")},
        {QStringLiteral("paste"), uiTr("Paste an image"), QStringLiteral("Ctrl+V"), true,
         uiTr("Pastes an image from the clipboard into the selection")},
        {QStringLiteral("select-all"), uiTr("Select every mark"), QStringLiteral("Ctrl+A"),
         true, uiTr("Picks up every mark at once")},
        {QStringLiteral("select-none"), uiTr("Select no mark"), QStringLiteral("Ctrl+D"), true,
         uiTr("Puts every mark down")},
        // Both spellings of a plain Tab: reachable bare, and under Ctrl so a
        // run of marks can be walked without letting go of it.
        {QStringLiteral("next-mark"), uiTr("Next mark"), QStringLiteral("Tab, Ctrl+Tab"), true,
         uiTr("Moves the focus to the next mark")},
        // Spelled the way the key actually arrives, which is not "Shift+Tab":
        // Qt delivers Shift+Tab as `Key_Backtab` *with* Shift still in the
        // modifiers, so "Shift+Tab" (Key_Tab plus Shift) matches nothing and a
        // bare "Backtab" (no Shift) is a different key from the one pressed.
        // The second spelling is Ctrl+Shift+Tab, which arrives the same way
        // with Ctrl added.
        {QStringLiteral("previous-mark"), uiTr("Previous mark"),
         QStringLiteral("Shift+Backtab, Ctrl+Shift+Backtab"), true,
         uiTr("Moves the focus to the previous mark")},
        {QStringLiteral("delete"), uiTr("Delete the mark"), QStringLiteral("Del, Backspace"),
         true, uiTr("Deletes the mark the focus is on")},
        {QStringLiteral("copy-color"), uiTr("Copy the colour under the cursor"),
         QStringLiteral("C"), true,
         uiTr("While the magnifier is up: copies that pixel's colour code")},
        // Not `A`, which the cursor walk below already spends on "left": the
        // picker's two keys are live only while the right button holds the
        // magnifier up, and that is exactly the state in which the mouse is
        // still being moved -- so the cursor keys are the ones the user needs
        // there, and a letter of their own is what keeps the two apart.
        {QStringLiteral("adopt-color"), uiTr("Use the colour under the cursor"),
         QStringLiteral("V"), true,
         uiTr("While the magnifier is up: makes that colour the current tool's")},
        {QStringLiteral("magnifier"), uiTr("Show the magnifier"), QStringLiteral("M"), true,
         uiTr("Shows the magnifier for two seconds, without dragging")},
        // Not rebindable: a modifier read while a drag is already under way.
        // `QKeySequence` has no spelling for "Alt on its own", so a stored
        // binding could not be matched against a modifier-only press without
        // also matching Alt+F4.
        {QStringLiteral("preserve-aspect"), uiTr("Keep the aspect ratio"),
         QStringLiteral("Alt"), false,
         uiTr("Hold while resizing: the shape keeps its proportions")},
        {QStringLiteral("coarse-step"), uiTr("Take a bigger step"), QStringLiteral("Shift"),
         false, uiTr("Hold while walking the cursor: ten pixels at a time")},
        {QStringLiteral("select-mark"), uiTr("Pick a mark up"), QStringLiteral("Shift"),
         false,
         uiTr("Hold to move or stretch a mark under the pointer, whatever tool is armed")},
        {QStringLiteral("cursor-left"), uiTr("Cursor left"), QStringLiteral("Left, A"), true,
         uiTr("Moves the cursor one pixel left")},
        {QStringLiteral("cursor-right"), uiTr("Cursor right"), QStringLiteral("Right, D"), true,
         uiTr("Moves the cursor one pixel right")},
        {QStringLiteral("cursor-up"), uiTr("Cursor up"), QStringLiteral("Up, W"), true,
         uiTr("Moves the cursor one pixel up")},
        {QStringLiteral("cursor-down"), uiTr("Cursor down"), QStringLiteral("Down, S"), true,
         uiTr("Moves the cursor one pixel down")},
    };
    return table;
}

QKeySequence parse(const QString &text)
{
    return QKeySequence::fromString(text, QKeySequence::PortableText);
}

/// Whether `parsed` is a real binding rather than a string `QKeySequence`
/// could not read.
///
/// An unreadable string does not come back empty -- it comes back as one
/// `Key_unknown`, with no modifier and no key -- so asking `isEmpty` is not
/// enough.  Without this, a typo in the file would be stored as a binding that
/// matches nothing, and the action would be unreachable from the keyboard
/// rather than falling back to the key it has always had.
bool isUnreadable(const QKeySequence &parsed)
{
    if (parsed.isEmpty() || parsed.count() == 0) {
        return true;
    }
    for (int index = 0; index < parsed.count(); ++index) {
        const int combined = parsed[index].toCombined();
        const int key = combined & ~static_cast<int>(Qt::KeyboardModifierMask);
        if (key == 0 || key == static_cast<int>(Qt::Key_unknown)) {
            return true;
        }
    }
    return false;
}

/// The alternatives one action's spelling names, in the order they are written.
///
/// Split on the comma by hand rather than left as one `QKeySequence`, because
/// Qt reads "Left, A" as a *chord*: two combinations pressed in sequence.  The
/// editor has always treated it as two ways to press one key -- the settings
/// page prints one row per alternative and the key handler asks about each in
/// turn -- so the split is what makes the model say what the rest of the editor
/// already did.  An empty or whitespace-only entry is dropped, which is how a
/// trailing comma is tolerated.
QVector<QKeySequence> parseAlternatives(const QString &text)
{
    QVector<QKeySequence> keys;
    for (const QString &part : text.split(QLatin1Char(','))) {
        const QString trimmed = part.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const QKeySequence parsed = parse(trimmed);
        if (isUnreadable(parsed)) {
            // One unreadable alternative does not throw away the readable ones
            // beside it: a file that spells one of two keys wrong keeps the one
            // it got right, and the page shows the misspelling as gone rather
            // than silently rebinding it to something else.
            continue;
        }
        keys.append(QKeySequence(parsed[0].toCombined()));
    }
    return keys;
}

/// The spelling [`ShortcutPreferences::textFor`] writes: the alternatives in
/// the order they are stored, joined the way the table writes them.
QString joinAlternatives(const QVector<QKeySequence> &keys)
{
    QStringList parts;
    parts.reserve(keys.size());
    for (const QKeySequence &key : keys) {
        parts.append(key.toString(QKeySequence::PortableText));
    }
    return parts.join(QStringLiteral(", "));
}

/// Whether `text` asks for no binding at all: an empty or whitespace-only
/// string, which is how the settings window says "this action has no key".
bool isUnbound(const QString &text)
{
    return text.trimmed().isEmpty();
}

/// The modifier bits `text` names, for the three actions read from the state of
/// the keyboard rather than from a key event.
///
/// Parsed here rather than through `QKeySequence`, because Qt will not parse a
/// modifier on its own: "Ctrl" comes back as a sequence holding `Key_unknown`
/// with the whole modifier *mask* set, which would match every key the user
/// pressed with any modifier down. The words are the ones `QKeySequence` prints
/// when it does parse a modifier, so what the settings page shows and what a
/// hand-written file says are the same spelling either way.
int modifierBits(const QString &text)
{
    int bits = 0;
    for (const QString &word : text.split(QLatin1Char('+'))) {
        const QString name = word.trimmed().toLower();
        if (name == QStringLiteral("ctrl") || name == QStringLiteral("control")) {
            bits |= static_cast<int>(Qt::ControlModifier);
        } else if (name == QStringLiteral("shift")) {
            bits |= static_cast<int>(Qt::ShiftModifier);
        } else if (name == QStringLiteral("alt")) {
            bits |= static_cast<int>(Qt::AltModifier);
        } else if (name == QStringLiteral("meta")) {
            bits |= static_cast<int>(Qt::MetaModifier);
        }
    }
    return bits;
}

} // namespace

const QVector<ShortcutBinding> &shortcutBindings()
{
    return bindings();
}

const ShortcutBinding &shortcutBinding(ShortcutAction action)
{
    const int index = static_cast<int>(action);
    if (index < 0 || index >= bindings().size()) {
        // One past the end, so an out-of-range id reads as a binding with no
        // key rather than reading the action before it.
        static const ShortcutBinding none{QString(), QString(), QString(), false, QString()};
        return none;
    }
    return bindings().at(index);
}

ShortcutPreferences::ShortcutPreferences()
{
    const QVector<ShortcutBinding> &table = shortcutBindings();
    bindings.reserve(table.size());
    unbound.reserve(table.size());
    for (const ShortcutBinding &binding : table) {
        this->bindings.append(parseAlternatives(binding.defaultKeys));
        unbound.append(false);
    }
}

bool ShortcutPreferences::hasKeys(ShortcutAction action) const
{
    const int index = static_cast<int>(action);
    if (index < 0 || index >= bindings.size()) {
        return false;
    }
    return !unbound.at(index) && !bindings.at(index).isEmpty();
}

QVector<QKeySequence> ShortcutPreferences::keysFor(ShortcutAction action) const
{
    const int index = static_cast<int>(action);
    if (index < 0 || index >= bindings.size() || unbound.at(index)) {
        return QVector<QKeySequence>();
    }
    // A file that spelled every alternative of an action wrong falls back to
    // the default rather than to nothing: an action the user cannot reach from
    // the keyboard without meaning it is worse than a misspelling they can see
    // in the settings page.
    if (bindings.at(index).isEmpty()) {
        return parseAlternatives(shortcutBinding(action).defaultKeys);
    }
    return bindings.at(index);
}

bool ShortcutPreferences::matches(ShortcutAction action, const QKeySequence &pressed) const
{
    if (pressed.isEmpty() || !hasKeys(action)) {
        return false;
    }
    // The pressed key is the subject: a stored entry with several alternatives
    // matches nothing when it is the one being asked, so asking each in turn is
    // what makes "Del, Backspace" reachable at all.
    for (const QKeySequence &alternative : bindings.at(static_cast<int>(action))) {
        if (pressed.matches(alternative) == QKeySequence::ExactMatch) {
            return true;
        }
    }
    return false;
}

bool ShortcutPreferences::held(ShortcutAction action, int modifiers) const
{
    const int index = static_cast<int>(action);
    if (index < 0 || index >= bindings.size() || unbound.at(index)) {
        return false;
    }
    // Parsed from the file's own spelling rather than from the parsed sequence,
    // because Qt turns a lone "Ctrl" into a sequence with the whole modifier
    // *mask* set, which would match any key pressed with any modifier down.
    const int bits = modifierBits(shortcutBinding(action).defaultKeys);
    return bits != 0 && (modifiers & bits) == bits;
}

bool ShortcutPreferences::matches(ShortcutAction action, const QKeySequence &pressed,
                                 int consumed) const
{
    if (pressed.isEmpty() || !hasKeys(action)) {
        return false;
    }
    // `consumed` is dropped from *both* sides: it is a modifier another action
    // has already read, so neither the binding nor the key still carries it.
    // Shift+Left therefore matches a "Left" binding once Shift is the coarse
    // step's -- which is what keeps a cursor binding from needing a Shift twin
    // beside every entry.
    const QKeySequence asked(pressed[0].toCombined() & ~consumed);
    if (asked.isEmpty() || isUnreadable(asked)) {
        return false;
    }
    for (const QKeySequence &stored : bindings.at(static_cast<int>(action))) {
        const QKeySequence alternative(stored[0].toCombined() & ~consumed);
        if (isUnreadable(alternative)) {
            continue;
        }
        if (asked.matches(alternative) == QKeySequence::ExactMatch) {
            return true;
        }
    }
    return false;
}

void ShortcutPreferences::setKeys(ShortcutAction action, const QVector<QKeySequence> &keys)
{
    const int index = static_cast<int>(action);
    if (index < 0 || index >= bindings.size()) {
        return;
    }
    bindings[index] = keys;
    // No alternatives is "the user cleared this", which is a state of its own
    // rather than "fall back to the default" -- otherwise clearing a binding in
    // the settings window would put the key straight back.
    unbound[index] = keys.isEmpty();
}

QString ShortcutPreferences::textFor(ShortcutAction action) const
{
    const int index = static_cast<int>(action);
    if (index < 0 || index >= bindings.size() || unbound.at(index)) {
        return QString();
    }
    return joinAlternatives(bindings.at(index));
}

void ShortcutPreferences::setText(ShortcutAction action, const QString &text)
{
    const int index = static_cast<int>(action);
    if (index < 0 || index >= bindings.size()) {
        return;
    }
    if (isUnbound(text)) {
        bindings[index].clear();
        unbound[index] = true;
        return;
    }
    const QVector<QKeySequence> parsed = parseAlternatives(text.trimmed());
    // Rejected rather than stored: `QKeySequence` hands back a `Key_unknown`
    // for a string it cannot read, and a binding of `Key_unknown` matches
    // nothing -- so storing it would leave the action unreachable from the
    // keyboard instead of on the key it has always had.
    if (parsed.isEmpty()) {
        return;
    }
    bindings[index] = parsed;
    unbound[index] = false;
}

bool ShortcutPreferences::addKey(ShortcutAction action, const QKeySequence &key)
{
    const int index = static_cast<int>(action);
    if (index < 0 || index >= bindings.size() || key.isEmpty()) {
        return false;
    }
    // Already there: the row the user just recorded is the row the dialog
    // already shows, and appending it again would leave two of them for one
    // combination, each with its own remove button.
    if (bindings.at(index).contains(key)) {
        return false;
    }
    bindings[index].append(key);
    unbound[index] = false;
    return true;
}

bool ShortcutPreferences::removeKey(ShortcutAction action, const QKeySequence &key)
{
    const int index = static_cast<int>(action);
    if (index < 0 || index >= bindings.size()) {
        return false;
    }
    QVector<QKeySequence> &keys = bindings[index];
    const auto found = std::find(keys.begin(), keys.end(), key);
    if (found == keys.end()) {
        return false;
    }
    keys.erase(found);
    // The last one gone is the user clearing the action, which is a state the
    // file has to be able to write.
    unbound[index] = keys.isEmpty();
    return true;
}

ShortcutAction ShortcutPreferences::owner(const QKeySequence &key) const
{
    return ownerOtherThan(ShortcutAction::kActionCount, key);
}

ShortcutAction ShortcutPreferences::ownerOtherThan(ShortcutAction except,
                                                   const QKeySequence &key) const
{
    if (key.isEmpty()) {
        return ShortcutAction::kActionCount;
    }
    // Backwards, so the *later* action is the one reported when two hold the
    // same key.  The table ships with one such pair -- `A` on both the cursor
    // and the magnifier's adopt-colour -- and the key handler reaches the later
    // one first, so naming that one is naming the binding the user would have
    // to give up for the other to be reachable again.
    for (int index = bindings.size() - 1; index >= 0; --index) {
        if (index == static_cast<int>(except) || unbound.at(index)) {
            continue;
        }
        if (bindings.at(index).contains(key)) {
            return static_cast<ShortcutAction>(index);
        }
    }
    return ShortcutAction::kActionCount;
}

} // namespace vshot

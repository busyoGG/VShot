// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

#pragma once

#include <QString>

namespace vshot {

// Resolves the active UI language: VSHOT_LANG wins when set ("zh*" selects
// Chinese, any other non-empty value selects English); otherwise the system
// locale decides. Call once before any widget is constructed.
void initUiLanguage();

// Translates a display-only UI string. English mode returns the source
// unchanged; Chinese mode looks the key up and falls back to the source, so
// an unmapped string can never render blank. Never wrap protocol values,
// JSON keys, object names or font families with this.
QString uiTr(const QString &english);
inline QString uiTr(const char *english)
{
    return uiTr(QString::fromUtf8(english));
}

} // namespace vshot

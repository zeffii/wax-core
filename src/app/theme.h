// The look of the window: colour themes (generated from the web app's themes.css, plus the native "slate"), thin lines, flat.
// The colours are plain variables the views read; setTheme() changes them all at once and restyles ImGui.
#pragma once

#include "imgui.h"

namespace wax::app {

// Sets ImGui's sizes for the display scale (once) and the colours of the current theme.
void applyTheme(float dpiScale);

int themeCount();
const char* themeName(int index);
int themeIndex();
// Chooses a theme by position or by name (an unknown name leaves the theme as it is); restyles ImGui.
void setTheme(int index);
bool setThemeByName(const char* name);

// The colours of the theme, for the views.
inline ImU32 kBg, kText, kPanel, kField, kLine, kSoft, kDimText, kAccent, kWarm, kHiFg;
inline ImU32 kBeatRow, kBarRow, kCursorCell;

inline ImU32 withAlpha(ImU32 c, int alpha) { return (c & 0x00FFFFFFu) | (static_cast<ImU32>(alpha < 0 ? 0 : alpha > 255 ? 255 : alpha) << 24); }
inline ImU32 textAlpha(int a) { return withAlpha(kText, a); }
inline ImU32 dimAlpha(int a) { return withAlpha(kDimText, a); }
inline ImU32 accentAlpha(int a) { return withAlpha(kAccent, a); }
inline ImU32 lineAlpha(int a) { return withAlpha(kLine, a); }

}  // namespace wax::app

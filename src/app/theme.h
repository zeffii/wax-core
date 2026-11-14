// The look of the window: dark slate, thin lines, flat. No purple and no pink.
#pragma once

#include "imgui.h"

namespace wax::app {

void applyTheme(float dpiScale);

// Colours the views use besides ImGui's own.
inline constexpr ImU32 kBeatRow = IM_COL32(255, 255, 255, 10);
inline constexpr ImU32 kBarRow = IM_COL32(255, 255, 255, 22);
inline constexpr ImU32 kPlayRow = IM_COL32(64, 180, 160, 70);
inline constexpr ImU32 kDimText = IM_COL32(120, 134, 146, 255);
inline constexpr ImU32 kAccent = IM_COL32(64, 190, 168, 255);
inline constexpr ImU32 kWarm = IM_COL32(226, 164, 74, 255);

}  // namespace wax::app

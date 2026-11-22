// The pattern editor's toolbar: switches for how the grid is shown and heard (all lanes side by side, tint patterns, hear notes, hide empty
// columns, solo-edit shows the song, follow the playing row). Each is a setting that is also listed in the browser's "Pattern view" group.
#pragma once

#include "app/toolbar.h"

namespace wax::app {

// Draws the toolbar centred along the top of [areaMin, areaMax] and returns the height it takes (the grid starts below it). `follow` is the
// Follow switch's state (kept by the caller; a click flips it and keeps the setting).
float drawPatternToolbar(ImDrawList* dl, ImVec2 areaMin, ImVec2 areaMax, float scale, bool& follow);

}  // namespace wax::app

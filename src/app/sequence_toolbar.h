// The sequence editor's toolbar: switches for what the pattern tiles show (names, minimaps and their parts), the song overview strip, the loop and
// following the playhead. Each is a setting; the pattern names and minimaps are also on the config screen.
#pragma once

#include <functional>

#include "app/toolbar.h"

namespace wax::app {

// Draws the toolbar centred along the top of [areaMin, areaMax] and returns the height it takes (the sheet starts below it). `follow` is the
// Follow switch's state (kept by the caller). The loop belongs to the song document, so the caller says whether it is on and flips it
// (`loopSet`: a loop exists at all; with none the switch says how to make one).
float drawSequenceToolbar(ImDrawList* dl, ImVec2 areaMin, ImVec2 areaMax, float scale, bool& follow, bool loopOn, bool loopSet, const std::function<void()>& toggleLoop);

}  // namespace wax::app

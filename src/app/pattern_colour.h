// The colour of a pattern, as the web draws it: one of 16 preset pastels, a custom "#rrggbb", or (automatic) one of six by the
// pattern's index. Stepping goes through the presets and back to automatic.
#pragma once

#include "imgui.h"

#include "project/project.h"

namespace wax::app {

// The pattern's colour at an alpha (0-255).
ImU32 patternColour(const Pattern& pattern, int alpha);

// The 16 presets, for a picker: how many there are, and one of them at an alpha.
int patternPresetCount();
ImU32 patternPresetColour(int preset, int alpha);

// A custom colour as the web stores it: "#rrggbb" from red, green, blue in 0-1.
Json customPatternColor(float red, float green, float blue);

// The colour after stepping `delta` (+1 / -1) through automatic, the presets, and back (null: automatic). A custom colour steps as automatic.
Json steppedPatternColor(const Pattern& pattern, int delta);

}  // namespace wax::app

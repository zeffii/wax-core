// The Song screen's pattern list: the patterns of the lane under the cursor, one line each (index, name, rows, how often it is placed).
// Click picks one (outlined); clicking a pattern's colour square opens the colour popup (presets, automatic, custom); double-click or the Place button puts it at the cursor row; New makes a blank one there.
#pragma once

#include <string>

#include "project/project.h"

namespace wax::app {

struct PatternListState {
  std::string picked;      // the outlined pattern's id
  char name[32] = {0};     // the rename box
  std::string nameFor;     // the pattern that text belongs to
  bool scrollToPicked = false;   // the keys moved the outline: the list scrolls so it is in view (a click needs no scroll, the pattern is under the mouse)
};

struct PatternListResult {
  std::string place;       // a pattern to place at the cursor row ("" none)
  bool create = false;     // New was pressed
  bool clone = false;      // Clone was pressed (the outlined pattern)
  std::string colourFor;   // the pattern whose colour was chosen in the swatch's popup ("" none)
  Json colour;             // its new colour: a preset number, "#rrggbb", or null for automatic
  std::string rename;      // a new name for the outlined pattern ("" none)
};

// `lane` may be null (no lane). The list picks the first pattern when `picked` is not one of the lane's.
PatternListResult drawPatternList(PatternListState& state, const Lane* lane, float height);

// Moves the outlined pattern by a step through the lane's list (Ctrl+Up / Ctrl+Down).
void stepPatternList(PatternListState& state, const Lane* lane, int step);

}  // namespace wax::app

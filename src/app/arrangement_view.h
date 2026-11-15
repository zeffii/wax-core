// The arrangement: one thin row per lane with its patterns drawn as bars along the song. Mouse only; it never takes the keyboard.
#pragma once

#include <string>

#include "project/project.h"

namespace wax::app {

struct ArrangementResult {
  std::string lane;   // the lane clicked ("" when none)
  int tick = -1;      // the song tick under the click
};

// `playTick` is -1 when stopped; `cursorTick` -1 hides the edit cursor.
ArrangementResult drawArrangement(const Project& project, const std::string& selectedLane, int playTick, int cursorTick);

// The height the view wants, so the caller can lay out around it.
float arrangementHeight(const Project& project);

}  // namespace wax::app

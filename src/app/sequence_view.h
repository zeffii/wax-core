// The sequence: the song zoomed out, as in the web app. Time runs down the rows (one row is a bar) and each lane is a column.
// A pattern is a block that starts with its index and name; a later pattern covers the ones under it. Mouse only here: it
// never takes the keyboard, which stays with the pattern editor.
#pragma once

#include <string>

#include "project/project.h"

namespace wax::app {

struct SequenceClick {
  std::string lane;   // the lane clicked ("" when none)
  int tick = -1;      // the tick at the start of the row clicked
};

// `playTick` is -1 when stopped; `cursorTick` -1 hides the edit cursor. With `follow` the view keeps the play row in sight.
SequenceClick drawSequenceView(const Project& project, const std::string& selectedLane, int playTick, int cursorTick, bool follow);

// The width the view wants for the song's lanes (the caller may give it less).
float sequenceWidth(const Project& project);

}  // namespace wax::app

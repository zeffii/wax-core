// A lane laid out along the song, one row per tick, shown as a table that follows the playhead.
#pragma once

#include <string>
#include <vector>

#include "project/project.h"

namespace wax::app {

// The rows of a lane as text cells, tick by tick: each placement's pattern copied in where it starts.
// A tick with no pattern under it has no cells.
std::vector<std::vector<std::string>> flattenLane(const Lane& lane, int songLength);

// The editing cursor to draw, and whether the view should scroll to keep it in sight.
struct CursorView {
  bool show = false;
  int tick = -1, cell = -1;
  bool scrollTo = false;
};

struct PatternViewResult {
  int clickedTick = -1;   // a row number was clicked: play from here
  int cellTick = -1, cell = -1;   // a cell was clicked
};

// `labels`: the column headings. `playTick`: the row to highlight (-1: none). `follow`: scroll to keep it in view.
PatternViewResult drawPatternView(const std::vector<std::vector<std::string>>& rows, const std::vector<std::string>& labels,
                                  int ticksPerBeat, int beatsPerBar, int playTick, bool follow, const CursorView& cursor = {});

}  // namespace wax::app

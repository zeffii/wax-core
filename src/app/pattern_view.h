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
// One lane's grid: its cells (rows[tick][column]), the column headings, and the tint of each row (the colour of the pattern that plays there).
struct PatternLane {
  std::string id;
  const std::vector<std::vector<std::string>>* rows = nullptr;
  const std::vector<std::string>* labels = nullptr;
  std::vector<unsigned int> tint;   // per tick, an ImU32 colour (0: none); empty: no tint
  // per tick: where patterns start and end, and which rows belong to a pattern other than the one under the cursor; empty: none
  static constexpr unsigned char kStart = 1, kDim = 4, kLocked = 8;   // kLocked: solo-edit keeps the row out of reach: greyed, hatched, no cells to click
  std::vector<unsigned char> marks;
  std::vector<unsigned int> edge;               // on a pattern's first row (kStart): the pattern's colour, for the bar drawn across its top
};

struct CursorView {
  int lane = 0;                          // which of the lanes shown holds the cursor

  int rowFirst = 0, rowEnd = -1;         // only the rows rowFirst..rowEnd-1 are drawn, numbered from rowFirst (solo-edit on its own); rowEnd -1: all
  bool hideEmpty = false;                // columns with nothing in them anywhere are left out (the cursor's column stays)
  bool show = false;
  int tick = -1, cell = -1;
  bool scrollTo = false;
  bool selected = false;                 // a block of cells is selected: ticks tick0..tick1, cells cell0..cell1
  int tick0 = 0, tick1 = 0, cell0 = 0, cell1 = 0;
  bool loop = false;                     // the song has a loop: ticks loopStart..loopEnd-1 (loopOn false: set, but switched off)
  bool loopOn = false;
  int loopStart = 0, loopEnd = 0;
  std::string info;                      // the line under the grid (what the column under the caret is); empty: no line
  float playFrac = 0;                    // how far through the playing row the song is (0..1): the playhead line glides through the row
};

struct PatternViewResult {
  int clickedTick = -1;   // a row number was clicked: play from here
  int cellTick = -1, cell = -1;   // a cell was clicked
  int lane = -1;                  // ...in this one of the lanes shown
};

// One lane (the solo view) or several side by side, the lane's name over its columns. `playTick`: the row to highlight (-1: none).
// `follow`: scroll to keep it in view.
PatternViewResult drawPatternView(const std::vector<PatternLane>& lanes, int ticksPerBeat, int beatsPerBar, int playTick, bool follow, const CursorView& cursor = {});

}  // namespace wax::app

// The sequence: the song zoomed out, as in the web app. Time runs down the rows (one row is a bar) and each lane is a column.
// A pattern is a block that starts with its index and name; a later pattern covers the ones under it. Mouse only here: it
// never takes the keyboard, which stays with the pattern editor.
#pragma once

#include <string>

#include <map>

#include "app/curve_view.h"
#include "app/minimap.h"
#include "project/project.h"

namespace wax::app {

struct SequenceClick {
  std::string lane;   // the lane clicked ("" when none)
  int tick = -1;      // the tick at the start of the row clicked
  int playFrom = -1;  // a click in the tick column: play from this tick
  CurveEditRequest curve;   // a change to a lane's curves asked for (see curve_view.h)
};

// A selected block: lanes first..last (by position) and the ticks from..to (to exclusive). `first` -1 when nothing is selected.
struct SequenceBlock {
  int first = -1, last = -1;
  int from = 0, to = 0;
};

// `playTick` is -1 when stopped; `cursorTick` -1 hides the edit cursor. With `follow` the view keeps the play row in sight.
// `curves` keeps the lane curves' view state; `targets` says what a lane's curve can move.
SequenceClick drawSequenceView(const Project& project, const std::string& selectedLane, int playTick, int cursorTick, int pickedTick, bool follow, CurveView& curves, const CurveView::Targets& targets, const SequenceBlock& block = {}, const std::map<std::string, MinimapLane>* minimap = nullptr);

// What the cursor is on, for the status line: the lane, the row, the tick and the pattern there ("lane 2/9 oberheim · row 9/96 · tick 144 ·
// 00 "pat00" used 8x in this lane").
std::string describeSongCursor(const Project& project, const std::string& laneId, int tick);

// The size of a row in ticks (the zoom the user set, a bar when that does not suit the song).
int sequenceRowTicks(const Song& song);

// The width the view wants for the song's lanes (the caller may give it less).
float sequenceWidth(const Project& project);

}  // namespace wax::app

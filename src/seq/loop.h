// The song's loop: song.loop = { start, end } in ticks, `end` not included, optionally `off`. Without one the whole song loops.
// Port of webtracker src/seq/loop.js.
#pragma once

#include <string>

#include "project/project.h"

namespace wax {

struct LoopRange {
  int start = 0, end = 0;
  bool on = false;   // a loop is set and not switched off
};

// The loop as the song holds it (on = false when none is set or it is off; the range is then the whole song).
LoopRange loopOf(const Song& song);

// What song.loop becomes when a point is set at `tick` (null: no loop). `which` is "start" or "end" (the first tick after the loop).
// Setting a point where it already is takes it away; setting a point switches the loop on again. `message` is for the status line.
Json loopWithPoint(const Song& song, const std::string& which, int tick, std::string& message);

// The loop switched off or on again (null when no loop is set).
Json loopToggled(const Song& song);

}  // namespace wax

// A lane made ready to play: its columns, and its patterns laid out along the song as pages of floats.
// Port of the playing part of webtracker src/seq/lane.js (parsePattern, flattenLane, buildPages, writeMask, prepareSong).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/params.h"
#include "project/project.h"
#include "seq/automation.h"
#include "seq/columns.h"

namespace wax {

// The sequence lives in fixed-size pages of ticks. Each page holds one float per tick and channel.
inline constexpr int kPageTicks = 1024;

struct PreparedLane {
  std::string id;
  std::string instrument;
  std::vector<Column> columns;
  int channels = 0;
  std::vector<std::vector<float>> pages;
  // For each tick: 0 plays, 1 muted by the lane's flips, 2 silenced as a whole; bit 4 marks a tick where the lane's
  // notes are let go.
  std::vector<std::uint8_t> mask;
  std::vector<AutoCurve> curves;   // the lane's drawn curves, baked
};

// Which row of which pattern is sounding at each tick of the song, by the same rules the sequencer flattens a lane with
// (patterns are overlays; the one that starts later wins; a pattern's length counts, and rows of blank text at the ends of a
// pattern are not rows). `pattern` is an index into Lane::patterns, -1 where nothing plays; `row` indexes Pattern::rows.
struct RowOwner {
  int pattern = -1;
  int row = 0;
};
std::vector<RowOwner> rowOwners(const Lane& lane, int songLength);

// One lane of the project made ready (the same as its entry in prepareSong). Used when a lane is edited while the song plays.
// Throws std::runtime_error with the lane and row when a pattern cannot be read.
PreparedLane prepareLane(const Project& project, const Lane& source, const ParamRegistry& registry);

// Instruments must already be compiled, so their parameter slots exist.
// Throws std::runtime_error with the lane and row when a pattern cannot be read.
std::vector<PreparedLane> prepareSong(const Project& project, const ParamRegistry& registry);

}  // namespace wax

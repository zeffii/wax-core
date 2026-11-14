// Lane curves: a drawn line over time that moves one parameter of a lane's machine (its volume, a filter cutoff, any
// parameter column). It is not pattern data: it belongs to the lane and runs across patterns.
//
// curve = { target: "node.param", on, pts: [{ t, v, pin }], kinds: ["c" | "l" | "g", ...], rest }
//   t: tick (may fall between rows), v: 0..1 along the parameter's own scale. kinds[i] is the segment from pts[i] to
//   pts[i + 1]: 'c' a bezier with vertical handles, 'l' a straight line, 'g' a gap (the curve is not there).
//   rest: where the parameter goes back to when a section ends; absent, it stays where the curve left it.
// Port of the playing side of webtracker src/seq/automation.js.
#pragma once

#include <string>
#include <vector>

#include "core/node.h"   // Json
#include "core/params.h"
#include "seq/columns.h"

namespace wax {

inline constexpr int kAutoRes = 8;   // baked samples per tick

struct AutoCurve {
  int slot = -1;
  double min = 0, max = 1;
  bool log = false;
  double taper = 1;
  bool amp = false;                  // the machine's Volume (its channel stage)
  bool hasRest = false;
  double rest = 0;
  std::vector<float> table;          // the value (0..1) at kAutoRes samples a tick; NaN where there is none
};

// Every working curve of a lane. `curves` is the lane's `curves` list as written.
std::vector<AutoCurve> buildAutomation(const Json& curves, const std::vector<Column>& columns, const std::string& instrument,
                                       const ParamRegistry& registry, int songLength);

}  // namespace wax

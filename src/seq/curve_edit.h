// Editing a lane's curves (automation): the anchors of a curve, snapping them to pattern edges, pins that follow those edges, sections
// separated by gaps. The playing side (what a curve does to a parameter) is seq/automation.h; this is the drawn line.
// Port of the editing half of webtracker src/seq/automation.js.
//
// A curve is { target: "node.param", on, pts: [{ t, v, pin }], kinds: ["c" | "l" | "g", ...], rest } in the lane's `curves` list.
//   pts[i].t is a tick, v is 0..1 along the parameter's own scale, pin is "start" or "end" when the anchor sits on that edge of a pattern
//   and follows it. kinds[i] is the segment from pts[i] to pts[i + 1]: c curve, l straight, g gap.
#pragma once

#include <string>
#include <vector>

#include "core/node.h"   // Json
#include "project/project.h"

namespace wax {

enum class CurvePin { None, Start, End };

struct CurvePoint {
  double t = 0, v = 0;
  CurvePin pin = CurvePin::None;
};

struct LaneCurve {
  std::vector<CurvePoint> pts;
  std::vector<char> kinds;   // one fewer than pts
  Json raw;                  // the curve as read: target, on, rest and whatever else it carries, which editing leaves alone

  char kindAt(std::size_t i) const { return i < kinds.size() ? kinds[i] : 'c'; }
  std::string target() const { return raw.is_object() ? raw.value("target", std::string()) : std::string(); }
};

LaneCurve readLaneCurve(const Json& json);
Json writeLaneCurve(const LaneCurve& curve);   // `raw` with its anchors and kinds replaced
LaneCurve newLaneCurve(const std::string& target, double t, double v);   // one anchor, switched on

// A placement as the curves see it: its length clipped to the pattern it plays.
struct CurveSpan {
  std::string pattern;
  int start = 0, length = 0;
};
std::vector<CurveSpan> curveSpans(const Lane& lane);

// A pattern edge of a lane.
struct CurveEdge {
  double t = 0;
  CurvePin edge = CurvePin::Start;
  std::string pattern;
};
std::vector<CurveEdge> edgesOf(const Lane& lane);

// The value (0..1) of the curve at a tick; NaN where the curve is not (before the first anchor, in a gap, after the last).
double curveValueAt(const LaneCurve& curve, double t);
// The middle of segment i (a tick and the value there) for the box shown on hover; false when there is no such segment.
bool segmentMiddle(const LaneCurve& curve, std::size_t i, double& t, double& v);

// Where an anchor dropped near tick `t` lands: pattern edges first (within `reach` ticks, the start winning where an end meets a start), then
// the nearest multiple of `rows` (any whole tick when rows is 0). `pin` is set when it landed on an edge.
struct CurveSnap {
  double t = 0;
  CurvePin pin = CurvePin::None;
};
CurveSnap snapTick(const Lane& lane, double t, bool edges, int rows, int reach, int length);

// Puts an anchor at (t, v) (an anchor already at t takes the value); returns its index. The segment it splits keeps its kind on both halves.
int addPoint(LaneCurve& curve, double t, double v, CurvePin pin = CurvePin::None);
bool removePoint(LaneCurve& curve, int i);
// Moves anchor i, keeping the order (it stops one tick short of its neighbours); dragging lets go of a pin.
void movePoint(LaneCurve& curve, int i, double t, double v);
bool toggleGap(LaneCurve& curve, int i);   // the segment after anchor i is joined or left as a gap

// After placements changed: pinned anchors follow the pattern edge they sat on (`before` and `after` are curveSpans of the lane). A pin whose
// pattern is gone lets go and the anchor stays. Anchors that land on one tick keep the later one. Returns how many pins were let go.
int followPins(LaneCurve& curve, const std::vector<CurveSpan>& before, const std::vector<CurveSpan>& after);
void clipCurve(LaneCurve& curve, int length);   // anchors past the song's end go

// The runs of joined anchors: [first, last] by index (a lone anchor is a run of one).
std::vector<std::pair<int, int>> curveSections(const LaneCurve& curve);
bool addSection(LaneCurve& curve, double t0, double t1, double v);   // pinned at both ends; false when anchors are already inside
bool removeSection(LaneCurve& curve, double t0, double t1);          // what was joined across it becomes a gap
// E on the last anchor: another at the same value at the end of the next pattern, the section carries on over it. Returns its index or -1.
int extendCurve(const Lane& lane, LaneCurve& curve, int i, int length);
// Shift+E on the last anchor: the next pattern is left alone (a gap) and a new section starts over the one after it. Returns the new last index or -1.
int skipCurve(const Lane& lane, LaneCurve& curve, int i, int length);

}  // namespace wax

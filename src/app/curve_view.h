// The lanes' curves (automation) over the Song screen's sequence: each lane shows one of its curves as a hairline down the lane, time down and
// the value across. Mouse only, like the sequence:
//   drag an anchor    moves it; it snaps to pattern edges (and pins there) and to rows; hold Alt to move it freely
//   double-click the line   a new anchor there
//   click the line    selects the segment; right-click gives its kind (curve, straight, gap)
//   right-click an anchor   delete, pin or unpin, the kind of the segment after it, extend over the next pattern, skip a pattern
//   right-click the lane    add a curve for a parameter, show another curve, switch it off, delete it
// The view changes nothing itself: each frame it may hand back one edit (a lane's curves list replaced), which the window applies.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "project/project.h"
#include "seq/automation.h"
#include "seq/curve_edit.h"

namespace wax::app {

// An edit the view asks for: the lane's whole `curves` list, a label for undo, and whether it carries on the one before (a drag).
struct CurveEditRequest {
  bool any = false;
  std::string lane;
  Json curves;
  std::string label;
  bool fold = false;
};

class CurveView {
public:
  struct LaneBox {
    const Lane* lane = nullptr;
    float left = 0, width = 0;   // the lane's column on screen
    bool selected = false;
  };
  using Targets = std::function<std::vector<CurveTarget>(const Lane&)>;

  // The layout of this frame: the lanes' columns, the screen y of tick 0, the height of a row (`cell` ticks), the ticks on show. Finds what the
  // pointer is over.
  void frame(const Project& project, std::vector<LaneBox> boxes, float top, float rowHeight, int cell, int firstTick, int lastTick, float scale);
  // True when the pointer is on a curve, an anchor, or one is being dragged: a click there is the curve's, not the sheet's.
  bool claimsPointer() const { return hover_.kind != Hit::None || drag_.active; }
  void draw(ImDrawList* draw, const Lane& lane, bool selected);
  // Mouse work (inside the window that shows the sequence). The edit asked for, if any.
  CurveEditRequest interact(const Targets& targets);

private:
  struct Hit {
    enum Kind { None, Anchor, Segment } kind = None;
    int lane = -1;      // index into boxes_
    int index = 0;      // the anchor, or the segment (from that anchor)
  };
  struct Drag {
    bool active = false, moved = false, edited = false;
    int lane = -1, point = 0;
    ImVec2 from;
  };
  struct MenuAt {
    int lane = -1;
    Hit::Kind kind = Hit::None;
    int index = 0;
    double tick = 0, value = 0;
  };

  const Project* project_ = nullptr;
  std::vector<LaneBox> boxes_;
  float top_ = 0, rowHeight_ = 16, scale_ = 1;
  int cell_ = 1, firstTick_ = 0, lastTick_ = 0;
  std::map<std::string, int> shown_;   // lane -> which of its curves it shows
  Hit hover_;
  Drag drag_;
  MenuAt menu_;
  std::string selectedLane_;   // the selected anchor or segment (drawn stronger)
  int selectedCurve_ = -1;
  Hit selected_;

  int shownIndex(const Lane& lane) const;
  const Json* shownCurve(const Lane& lane) const;
  float xOf(const LaneBox& box, double v) const;
  float yOf(double tick) const { return top_ + static_cast<float>(tick / cell_) * rowHeight_; }
  double tickOf(float y) const { return (y - top_) / rowHeight_ * cell_; }
  double valueOf(const LaneBox& box, float x) const;
  Hit hitTest(ImVec2 mouse) const;
  int laneUnder(ImVec2 mouse) const;
  CurveEditRequest replaced(int laneIndex, int curveIndex, const LaneCurve& curve, const std::string& label, bool fold = false) const;
  CurveEditRequest menu(const Targets& targets);
};

}  // namespace wax::app

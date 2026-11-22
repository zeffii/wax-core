// A toolbar: a small row of icon switches in a rounded plate, centred along the top edge of an area. Each switch is a yes/no setting; a click flips it,
// and hovering says what it is, whether it is on, and what it does. The graph window and the pattern editor each have one (graph_toolbar, pattern_toolbar).
#pragma once

#include <functional>
#include <vector>

#include "imgui.h"

namespace wax::app {

// Where a toolbar sits (screen pixels); the area under it leaves clicks there alone. Empty (max <= min) when the area is too narrow.
struct ToolbarBox {
  ImVec2 min{0, 0}, max{0, 0};
  bool contains(ImVec2 p) const { return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y; }
};

struct ToolbarSwitch {
  const char* name;
  const char* what;
  std::function<bool()> isOn;
  std::function<void()> flip;
  std::function<void(ImDrawList*, ImVec2 lo, float unit, ImU32 ink)> icon;   // strokes in a 16 by 16 box starting at lo, `unit` pixels per icon unit
};

constexpr float kToolbarTop = 4;      // web pixels from the area's top edge to the plate
float toolbarHeight(float scale);     // the plate's height

ToolbarBox toolbarBox(ImVec2 areaMin, ImVec2 areaMax, float scale, int switches);

// Draws the toolbar; `active`: the mouse may act on it (nothing else is over it).
void drawToolbar(ImDrawList* dl, const ToolbarBox& box, const std::vector<ToolbarSwitch>& switches, float scale, bool active);

}  // namespace wax::app

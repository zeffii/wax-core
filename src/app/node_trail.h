// The quick-node trail: a column of small square tiles at the right edge of the graph view, one for each of the nodes the
// user reaches for most. Drag a tile out into the graph to place the node (onto a wire to put it into that wire); a click
// places it at the middle of the view. The tile at the foot of the strip opens the bank: every node, a band per category, outlined in the
// category's colour; the same click and drag. The list is `kTrailNodes` in node_trail.cpp: change it there.
#pragma once

#include <set>
#include <string>

#include "imgui.h"

namespace wax::app {

// The width the trail takes at the right edge of the graph view, in web pixels (the caller multiplies by the UI scale).
constexpr float kTrailWidth = 40;

struct NodeTrailState {
  std::string pressed;          // the tile the mouse went down on
  ImVec2 press{0, 0};           // where
  bool dragging = false;        // the mouse has moved far enough from the press for it to be a drag
  bool bankOpen = false;        // the bank of all the nodes is showing
  float bankScroll = 0;         // how far it is scrolled, in pixels
  bool bankBarDrag = false;     // the bank's scrollbar thumb is being dragged
  ImVec2 bankMin{0, 0}, bankMax{0, 0};   // where it was drawn last frame (empty when closed), so the graph can leave clicks there alone
  bool over(ImVec2 point) const { return bankOpen && point.x >= bankMin.x && point.x <= bankMax.x && point.y >= bankMin.y && point.y <= bankMax.y; }
};

struct TrailEvent {
  enum class Kind { None, Click, Drop } kind = Kind::None;
  std::string type;             // the node type of the tile
  ImVec2 at{0, 0};              // Drop: where the mouse was let go (screen pixels)
};

// Draws the trail down the right edge of the rectangle [min, max] (screen pixels) and follows the mouse. While a tile is dragged out
// of the trail, `dragType` is its type and the tile follows the mouse (the caller lights the wire it is over). Returns what happened.
TrailEvent drawNodeTrail(NodeTrailState& state, ImDrawList* dl, ImVec2 min, ImVec2 max, float scale, std::string& dragType, const std::set<std::string>& available);

// True when the point is over the trail's strip of the rectangle [min, max].
bool overTrail(ImVec2 point, ImVec2 min, ImVec2 max, float scale);

}  // namespace wax::app

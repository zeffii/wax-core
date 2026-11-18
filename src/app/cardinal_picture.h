// The picture of the Cardinal synth (port of webtracker src/graph/cardinalview.js): four panels in one picture, redrawn as a parameter moves.
//   oscillators   a few cycles of osc 1 (blue), osc 2 (amber) and the two together, with sync, fm and ring applied: the node's own code run offline
//   filter        the gain of the chosen filter against frequency, the stretch the mod envelope sweeps the cutoff through shaded
//   envelopes     the amp envelope (amber) and the mod envelope (blue), held and then let go
//   lfos          one cycle of each LFO and what it moves
// Dragged: the filter's cutoff line along the axis, the edge of the shaded sweep (the mod envelope's cutoff amount), each LFO's height (its amount).
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "imgui.h"

#include "app/node_response.h"
#include "core/node.h"

namespace wax::app {

inline constexpr float kCardinalWidth = 360, kCardinalHeight = 362;

void drawCardinal(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 origin, float scale, ResponseHandles& handles);

// The parameter changes dragging a "@card" handle to `at` makes.
std::vector<std::pair<std::string, double>> dragCardinalHandle(const ResponseHandles::Handle& handle, const GraphNode& node, const NodeType& type, ImVec2 at);

}  // namespace wax::app

// Pictures beside a node in the graph view that are drawn from its parameters alone (as the web app's per-node views): the echo delays'
// bars against the song's ticks (canyon), the three modulated delays over four seconds (ensemble) and the early/late pulses with the
// mono comb they make (haas). Shown for the picked node, or for nodes told to keep their pictures (Alt+V), like the envelopes.
#pragma once

#include <string>

#include "imgui.h"

#include "core/node.h"
#include "project/project.h"

namespace wax::app {

// The song's timing, for pictures that lay time out in ticks.
struct PictureTiming { double tickMs = 0; int perBeat = 4; };

bool hasPicture(const std::string& nodeType);

void drawNodePicture(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 tileMin, ImVec2 tileMax, ImVec2 areaMin, ImVec2 areaMax, float scale, const PictureTiming& timing);

}  // namespace wax::app

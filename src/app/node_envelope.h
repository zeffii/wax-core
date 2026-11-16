// The envelope picture beside a node in the graph view, as in the web app: every attack/decay/sustain/release set a node type lists (a synth
// with a filter envelope shows two, overlaid), drawn big under the node when it is picked or its pictures are kept (Alt+V).
#pragma once

#include "imgui.h"

#include "core/node.h"
#include "project/project.h"

namespace wax::app {

// Whether the node type has envelopes to draw.
bool hasEnvelopes(const std::string& nodeType);

// Draws the pictures for `node` below its tile (above it when the tile is low in the area), inside the area `areaMin`..`areaMax`.
void drawNodeEnvelopes(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 tileMin, ImVec2 tileMax, ImVec2 areaMin, ImVec2 areaMax, float scale);

}  // namespace wax::app

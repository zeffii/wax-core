// The frequency-response picture of an equaliser node, beside it in the graph view (as in the web app): the sum of its bands as a filled
// curve, each band's own curve thin, a numbered dot per band and a small triangle for each cut. Drag a dot sideways for the band's
// frequency and up or down for its gain; drag a triangle sideways for the cut's frequency.
#pragma once

#include <string>
#include <vector>

#include "imgui.h"

#include "core/node.h"
#include "project/project.h"

namespace wax::app {

// What the mouse can take hold of in the picture last drawn.
struct ResponseHandles {
  struct Handle { std::string node, param; int band = 0; ImVec2 pos{0, 0}; };   // band 0: a cut (param is "lowcut" or "highcut")
  std::vector<Handle> list;
  std::string node;                    // the node the picture belongs to
  float px = 0, py = 0, pw = 1, ph = 1;   // the plot's rectangle
};

bool hasResponse(const std::string& nodeType);

// Draws the picture for `node` and fills `handles` for the next frame's mouse.
void drawNodeResponse(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 tileMin, ImVec2 tileMax, ImVec2 areaMin, ImVec2 areaMax, float scale, ResponseHandles& handles);

// The parameter changes that dragging `handle` to the screen point `at` makes: {name, value} pairs, within the parameters' ranges.
std::vector<std::pair<std::string, double>> dragResponse(const ResponseHandles& handles, const ResponseHandles::Handle& handle, const GraphNode& node, const NodeType& type, ImVec2 at);

}  // namespace wax::app

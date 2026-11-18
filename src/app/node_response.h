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
  struct Handle { std::string node, param; int band = 0; ImVec2 pos{0, 0}; float gx = 0, gy = 0, gw = 1, gh = 1; bool once = false; };   // once: a click does it (a step turned on or off), there is no drag   // g*: the picture's own rectangle, for pictures that are not the equaliser's   // band 0: a cut (param is "lowcut" or "highcut")
  std::vector<Handle> list;
  std::string node;                    // the node the picture belongs to
  float px = 0, py = 0, pw = 1, ph = 1;   // the plot's rectangle
  // the drag in progress: Shift slows it to a tenth, measured from where Shift was pressed
  ImVec2 grabAt{0, 0}, fineMouse{0, 0}, fineAt{0, 0};
  bool fine = false;
};

// Starts a drag at `mouse`.
void beginHandleDrag(ResponseHandles& handles, ImVec2 mouse);
// The point the drag is at now: the mouse, or with Shift held a tenth of the mouse's movement since Shift went down.
ImVec2 handleDragPoint(ResponseHandles& handles, ImVec2 mouse, bool shift);
// What a double-click on `handle` does: the parameters it edits, put back to their defaults (empty for a button).
std::vector<std::pair<std::string, double>> resetHandle(const ResponseHandles& handles, const ResponseHandles::Handle& handle, const GraphNode& node, const NodeType& type);

bool hasResponse(const std::string& nodeType);

// Draws the picture for `node` and fills `handles` for the next frame's mouse.
void drawNodeResponse(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 tileMin, ImVec2 tileMax, ImVec2 areaMin, ImVec2 areaMax, float scale, ResponseHandles& handles);

// The parameter changes that dragging `handle` to the screen point `at` makes: {name, value} pairs, within the parameters' ranges.
std::vector<std::pair<std::string, double>> dragResponse(const ResponseHandles& handles, const ResponseHandles::Handle& handle, const GraphNode& node, const NodeType& type, ImVec2 at);

}  // namespace wax::app

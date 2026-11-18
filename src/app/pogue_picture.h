// The Pogue node's picture, in five parts side by side as in the web app (src/graph/pogueview.js): the voices over the last few seconds
// (where each one reads the delay line, between the shortest and longest delay, with the detector, its sensitivity line and the triggers
// that fired), the filters, what saturation and bit reduction do, the feedback between the two sides, and the pattern's steps.
// What can be dragged: the two edges of the delay band, the onset line, the two filter cut-offs; a click on a step turns it on or off.
#pragma once

#include "imgui.h"

#include "app/node_pictures.h"

namespace wax::app {

inline constexpr PictureSize kPogueSize{1500, 194};

// Draws the five parts into the frame whose top left is `origin` (the parts are `scale` times their size in web pixels). The live part asks
// the node's probe for its reports and keeps the history it draws. Handles are added to `handles`.
void drawPogue(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 origin, float scale, const PictureTiming& timing, ResponseHandles& handles);

// The parameter changes dragging a "@pog..." handle to `at` makes.
std::vector<std::pair<std::string, double>> dragPogueHandle(const ResponseHandles::Handle& handle, const GraphNode& node, const NodeType& type, ImVec2 at);

}  // namespace wax::app

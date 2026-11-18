// The pictures of the web's "space" panels (src/graph/headview.js, ringview.js, notepanview.js, sympathyview.js, filterproview.js and
// specpaneditor.js), drawn in web pixels with the Canvas of picture_canvas.h:
//   headpan    the head from above with the sound to drag round it, and what each ear gets
//   ringmod    the sound's partials, the carrier and the sidebands (live: the pitch heard and the carrier)
//   notepan    the listener with the sounding notes round it, and the same laid out left to right (live)
//   sympathy   the strings by pitch, each lit by how much it rings (live), the left and right sums, chord and root buttons
//   filterpro  the sound in and out (live spectra) and the filter's own curve, a dot to drag for the cutoff and resonance
//   specpan    the pan of each band against frequency
// The live ones read the reports their nodes publish through the node's probe while the picture is drawn (the draw asks for them).
#pragma once

#include "imgui.h"

#include "app/node_pictures.h"

namespace wax::app {

// The size of the picture of a node type, in web pixels; zero for a type that is not one of these.
PictureSize spacePictureSize(const std::string& nodeType);

// Draws the picture whose top left is `origin`; handles (what is dragged or clicked) are added to `handles`.
void drawSpacePicture(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 origin, float scale, const PictureTiming& timing, ResponseHandles& handles);

// The parameter changes dragging a "@sp." handle to `at` makes.
std::vector<std::pair<std::string, double>> dragSpaceHandle(const ResponseHandles::Handle& handle, const GraphNode& node, const NodeType& type, ImVec2 at);

}  // namespace wax::app

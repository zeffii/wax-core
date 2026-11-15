// The graph view: the nodes of one instrument as small flat tiles joined by thin wires, as in the web app's Instruments tab. Click a
// tile to pick the node (the parameter panel follows); drag it to move it (one undo step per drag). Rounded tiles are sources, boxes
// are processors, the circle is the voice stage and the speaker is the output.
#pragma once

#include <string>

#include "edit/document.h"

namespace wax::app {

struct GraphViewState {
  std::string held;          // the tile being dragged
  float grabX = 0, grabY = 0;   // where on the tile it was taken
  std::string wiring;        // the node a new wire is being pulled from (its right-hand dot)
  std::string deleteNode;    // set by the right-click menu; the caller never needs to clear it
};

// `picked` is the node chosen (in/out). Pull from a tile's right-hand dot to another tile to wire them; click the dot on a wire to cut it;
// right-click a tile for delete. Wiring changes go through `doc` (the caller runs syncGraphs). Returns true when a node moved.
bool drawGraphView(GraphViewState& state, Document& doc, const std::string& instrument, std::string& picked, float height);

}  // namespace wax::app

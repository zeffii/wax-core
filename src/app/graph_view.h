// The graph view: the nodes of one instrument as small flat tiles joined by thin wires, as in the web app's Instruments tab. Click a
// tile to pick the node (the parameter panel follows); drag it to move it (one undo step per drag). Rounded tiles are sources, boxes
// are processors, the circle is the voice stage and the speaker is the output.
#pragma once

#include <string>

#include "imgui.h"

#include "app/insert_menu.h"
#include "edit/document.h"

namespace wax::app {

struct GraphViewState {
  std::string held;          // the tile being dragged
  float grabX = 0, grabY = 0;   // where on the tile it was taken
  std::string wiring;        // "node.port": a new wire is being pulled from this output
  std::string wiringIn;      // "node.port": a new wire is being pulled from this (free) input towards an output
  struct Rewire {            // a port with wires was pressed: dragging picks up one wire by that end
    bool active = false, started = false, movingTo = false;   // movingTo: the wire's input end moves (else its output end)
    std::string from, to;    // the wire
    ImVec2 press{0, 0};
  } rewire;
  std::string wireFrom, wireTo;   // the selected wire (Delete cuts it)
  std::string message;       // the last thing that went wrong (a refused wire), shown under the graph until the next click
  ImVec2 addAt{0, 0};        // where the add-node menu was opened (web pixels)
  InsertMenuState menu;      // the add-node menu (right-click on empty space)
  std::string deleteNode;    // set by the right-click menu; the caller never needs to clear it
};

// `picked` is the node chosen (in/out). As in the web app: drag from an output port to an input port (or the other way from a free input);
// drag a port that has wires to pick up a wire by that end (drop it on another port to move it, anywhere else to unplug it, Esc puts it
// back; Shift+drag starts a new wire instead); click a wire to select it and press Delete to cut it; right-click a tile for delete. Wiring changes go through `doc` (the caller runs syncGraphs). Returns true when a node moved.
bool drawGraphView(GraphViewState& state, Document& doc, const std::string& instrument, std::string& picked, float height);

}  // namespace wax::app

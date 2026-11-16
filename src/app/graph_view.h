// The graph view: the nodes of one instrument as small flat tiles joined by thin wires, as in the web app's Instruments tab. Click a
// tile to pick the node (the parameter panel follows); drag it to move it (one undo step per drag). Rounded tiles are sources, boxes
// are processors, the circle is the voice stage and the speaker is the output.
#pragma once

#include <set>
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
  // while a tile is dragged: Esc moves it alone (wires stay), Shift at the press means "take it out of its chain"; the wire it is over (it
  // goes into that wire when let go) and whether it is far enough out of its chain (its neighbours are then joined) are kept until release
  bool dragFree = false, dragForce = false, dragMoved = false, dropPull = false;
  std::string dropFrom, dropTo;
  std::string hoverKey;      // the port the mouse rests on (with its side), and which of its wires Tab has lit
  int pickWire = 0;
  std::set<std::string> group;   // nodes picked together (lasso, or Shift+click): they move, duplicate and go as one
  std::string lastPicked;    // the node picked before the last click (a Shift+click starts the group from it)
  bool lassoing = false;     // a lasso is being dragged on empty space
  ImVec2 lassoFrom{0, 0};    // where it started, relative to the graph's top left
  std::string wireFrom, wireTo;   // the selected wire (Delete cuts it)
  std::string message;       // the last thing that went wrong (a refused wire), shown under the graph until the next click
  ImVec2 addAt{0, 0};        // where the add-node menu was opened (web pixels)
  InsertMenuState menu;      // the add-node menu (right-click on empty space)
  InsertMenuState replaceMenu;   // the same menu, choosing the type a node becomes (right-click a tile > replace)
  InsertMenuState wireMenu;      // the same menu again, for a node to put into the wire that was right-clicked
  std::string menuWireFrom, menuWireTo;   // (that wire: output address, input address)
  std::string replacing;     // the node the replace menu is for
  std::string deleteNode;    // set by the right-click menu; the caller never needs to clear it
};

// `picked` is the node chosen (in/out). As in the web app: drag from an output port to an input port (or the other way from a free input);
// drag a port that has wires to pick up a wire by that end (drop it on another port to move it, anywhere else to unplug it, Esc puts it
// back; Shift+drag starts a new wire instead); click a wire to select it and press Delete to cut it; drag on empty space to lasso nodes (Shift+click adds one); a group moves together; Ctrl+D duplicates, Delete or Ctrl+X removes, Alt+R removes and joins the wires round; Tab and Alt+arrows pick the next node; drag a tile onto a wire to put it into that wire (a node far out of its chain, or Shift+drag, takes it out: its neighbours are joined; Esc during the drag moves it alone); right-click a tile for delete or replace (the node becomes another type: its wires, values and exposed columns follow). Wiring changes go through `doc` (the caller runs syncGraphs). Returns true when a node moved.
bool drawGraphView(GraphViewState& state, Document& doc, const std::string& instrument, std::string& picked, float height);

}  // namespace wax::app

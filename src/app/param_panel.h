// The parameter panel, as the web app's parameter view, here a window that floats over the graph: a title with the node's name, and one
// row per parameter: its name, then a bar with the value written inside it. Drag a bar to set the value (double-click resets, the wheel
// nudges), right-click for a menu to type a value, reset, show the parameter as a pattern column (and its width) or give it a port.
// Rows of a parameter shown as a column carry its label and the cell's hex; a dot marks a parameter with a port.
#pragma once

#include <map>
#include <string>

#include "edit/document.h"
#include "engine/engine.h"
#include "imgui.h"

namespace wax::app {

// One window of exposed sliders: where it is and what its rows needed in height.
struct ExposedWindow {
  bool placed = false;   // put at its first place once; after that it stays where the user left it
  float content = 0;     // the rows' height last frame
  int cursor = 0;        // the row the keys act on
  ImVec2 min{0, 0}, max{0, 0};   // as drawn last
};

struct ParamPanelState {
  std::string instrument;   // the instrument the node choice belongs to
  std::string node;         // the node picked ("" = the first one with parameters)
  std::string drawNode;     // set when the user asked to draw this node's envelopes (the caller opens the window and clears it)
  std::string gateNode;     // set when the user asked for the step levels of this node (gate, arpeggiator)
  std::string dxNode;       // set when the user asked for the DX loader of this node
  bool dragging = false;    // a drag is in progress: its steps are one undo
  bool placed = false;      // the window has been put at the graph's corner once; after that it stays where the user left it
  int paramCursor = 0;      // the row the keys act on
  float paramContent = 0;   // what the panel's rows needed in height last frame (it is as high as that)
  ExposedWindow dock;       // the exposed dock in the graph screen (min/max: as drawn last, zero size when none, for the graph to keep clear of)
  ExposedWindow follow;     // the floating window of the cursor lane's machine on the Song and Pattern screens
  bool followOpen = false;  // ... and whether it is shown
  std::map<std::string, ExposedWindow> pinned;   // a window kept open for each pinned machine (its id)
  bool open = true;         // the window is shown (the close button hides it; the Instrument screen's tab brings it back)
};

// Draws the window floating over the graph area (screen rectangle `areaMin`..`areaMax`: where it first appears, at the top right).
// Changes go through `doc` as SetParam edits (and SetExposed, SetExposedBits, SetPort); the caller then runs syncParams, syncLanes and
// syncGraphs. Returns true when a parameter changed.
bool drawParamPanel(ParamPanelState& state, Document& doc, const Engine& engine, const std::string& instrument, ImVec2 areaMin, ImVec2 areaMax);

// The exposed-parameter dock, at the left of the graph: a row for each column the machine's patterns have (named by the column's label), the
// same bars as the parameter panel. Shown only when something is exposed. Returns true when a parameter changed.
bool drawExposedDock(ParamPanelState& state, Document& doc, const Engine& engine, const std::string& instrument, ImVec2 areaMin, ImVec2 areaMax);

// The floating windows over the Song and Pattern screens: one for `followInstrument` (the cursor lane's machine; its X hides it, `followOpen`
// shows it) with a pin that keeps a window for that machine open, and a window for each pinned machine (its X, or unpin, puts it away).
// Returns true when a parameter changed.
bool drawFloatPanels(ParamPanelState& state, Document& doc, const Engine& engine, const std::string& followInstrument, ImVec2 areaMin, ImVec2 areaMax);

}  // namespace wax::app

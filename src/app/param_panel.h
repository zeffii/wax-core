// The parameter panel, as the web app's parameter view: a docked vertical list at the right, a title strip with the node's name, and
// one row per parameter: its name, then a bar with the value written inside it. A grip on the left edge sets the width.
// Drag a bar to set the value (double-click resets, the wheel nudges), right-click for a menu to type a value or reset.
#pragma once

#include <string>

#include "edit/document.h"
#include "engine/engine.h"

namespace wax::app {

struct ParamPanelState {
  std::string instrument;   // the instrument the node choice belongs to
  std::string node;         // the node picked ("" = the first one with parameters)
  std::string drawNode;     // set when the user asked to draw this node's envelopes (the caller opens the window and clears it)
  std::string dxNode;       // set when the user asked for the DX loader of this node
  bool dragging = false;    // a drag is in progress: its steps are one undo
  float width = 0;          // the dock's width in points (0: the usual 210, scaled)
};

// The room the dock takes (grip included), for the caller to leave at the right of the window.
float paramDockSpan(const ParamPanelState& state);

// Draws the dock in a column of this height (negative: what is left, as for BeginChild). Changes go through `doc` as SetParam edits;
// the caller then runs syncParams. Returns true when a parameter changed.
bool drawParamPanel(ParamPanelState& state, Document& doc, const Engine& engine, const std::string& instrument, float height);

}  // namespace wax::app

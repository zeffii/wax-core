// The parameter panel: the nodes of the selected lane's instrument as flat text, and one row of sliders for the node picked.
#pragma once

#include <string>

#include "edit/document.h"
#include "engine/engine.h"

namespace wax::app {

struct ParamPanelState {
  std::string instrument;   // the instrument the node choice belongs to
  std::string node;         // the node picked ("" = the first one with parameters)
  bool dragging = false;    // a slider drag is in progress: its steps are one undo
};

// Draws it in the space left. Changes go through `doc` as SetParam edits; the caller then runs syncParams.
// Returns true when a parameter changed.
bool drawParamPanel(ParamPanelState& state, Document& doc, const Engine& engine, const std::string& instrument);

}  // namespace wax::app

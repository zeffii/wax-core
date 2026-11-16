// The step-level window of a trance gate or an arpeggiator's step mask: one bar per step, its height the level the sound is let through at
// (0 closed, 1 open). Click or drag across the bars to draw; presets below. The levels are kept in the node's data.gate (64 of them, of
// which the first `steps` are shown and played); a drag is one undo step.
#pragma once

#include <string>

#include "edit/document.h"
#include "engine/engine.h"

namespace wax::app {

struct GateWindow {
  bool open = false;
  std::string instrument, node;
  bool dragging = false;   // a drag across the bars is in progress
};

// Whether the node type has step levels to edit.
bool hasGate(const std::string& nodeType);

void showGate(GateWindow& window, const std::string& instrument, const std::string& node);

// Draws the window when open. True when the levels changed (the caller then runs syncData).
bool drawGateWindow(GateWindow& window, Document& doc, const Engine& engine);

}  // namespace wax::app

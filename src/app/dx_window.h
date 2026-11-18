// The DX loader window: one voice of a dvii node's bank, edited on one screen (voice, algorithm, operator, scaling, LFO and pitch). Every
// change repacks the voice into the node's data.bank through SetBank, so it is heard on the next note and is one undo step per drag.
#pragma once

#include <string>

#include "edit/document.h"
#include "engine/engine.h"

namespace wax::app {

struct DxWindow {
  bool open = false;
  std::string instrument, node;
  int op = 0;           // the operator being edited (0..5)
  bool dragging = false;   // a slider drag is in progress: its steps are one undo
  int dragPoint = -1;      // the envelope point held (0..7: rate/level pairs), -1 none
  int dragPitchPoint = -1;
};

void showDx(DxWindow& window, const std::string& instrument, const std::string& node);

// Draws the window when open. True when the bank or a parameter changed (the caller then runs syncData and syncParams).
bool drawDxWindow(DxWindow& window, Document& doc, const Engine& engine);

}  // namespace wax::app

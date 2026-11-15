// The envelope window: a floating window holding the drawings of one node (tabs when it has several), each as the canvas and the tool
// row of envelope_editor.h. Edits go through the document as SetDrawing; a drag is one undo step.
#pragma once

#include <map>
#include <string>

#include "app/envelope_editor.h"
#include "edit/document.h"
#include "engine/engine.h"

namespace wax::app {

struct EnvelopeWindow {
  bool open = false;
  std::string instrument, node;            // what the window shows
  std::string drawing;                     // the tab chosen
  std::map<std::string, EnvelopeView> views;   // one per drawing: the chosen anchor, snap and time setting
  bool wantsKeys() const {
    if (!open) return false;
    for (const auto& [name, view] : views) if (view.wantsKeys()) return true;
    return false;
  }
};

// Shows the drawings of a node in the window.
void showEnvelopes(EnvelopeWindow& window, const std::string& instrument, const std::string& node);

// Draws the window when it is open. Returns true when a drawing changed (the caller then runs syncData).
bool drawEnvelopeWindow(EnvelopeWindow& window, Document& doc, const Engine& engine);

}  // namespace wax::app

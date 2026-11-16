// The wavetable node's window, four pictures side by side: the table (a stack of waves, the frame the position points at picked out; the
// bundled set until a table is cut from a sample), wave A and wave B (each 512 points cut from any sample of the project: pick the sample, then
// where, by typing a frame, clicking the strip that shows the sample, stepping, or snapping to the next rising zero crossing) and the blend of
// A and B at the current mix. The table and pair are kept on the node (data.table, data.pair, as base64 text; data.meta remembers where each
// was cut from), and a change is one undo step.
#pragma once

#include <string>

#include "edit/document.h"
#include "engine/engine.h"

namespace wax::app {

struct WavetableWindow {
  bool open = false;
  std::string instrument, node;
  std::string tableSample;     // the sample the next "cut table" uses
  int tableSize = 512;         // points per frame in the sample (0: the whole sample)
};

void showWavetable(WavetableWindow& window, const std::string& instrument, const std::string& node);

// Draws the window when open. True when the node's data changed (the caller then runs syncData).
bool drawWavetableWindow(WavetableWindow& window, Document& doc, const Engine& engine);

}  // namespace wax::app

// The window of the vector nodes, its parts side by side in one row: the diamond (the four sources A to D, the path the mix travels after a
// note starts, the time bar under it) for compass, vecpath and vecmix, and the wave sequences (one row of steps per lane, four in the compass
// synth, one in a waveseq node) for compass and waveseq. Port of webtracker src/graph/compassview.js, vectorpad.js and waveseqview.js; the
// compass amplitude and filter drawings stay in the envelope window. The path is kept on the node as data.path and the steps as data.lanes;
// a drag is one undo step.
#pragma once

#include <string>

#include "edit/document.h"
#include "engine/engine.h"

namespace wax::app {

struct VectorWindow {
  bool open = false;
  std::string instrument, node;
  int selectedPoint = 0;                 // the chosen point of the path
  int laneChosen = 0, stepChosen = 0;    // the chosen step
  // what the mouse holds: 0 nothing, 1 a path point, 2 the + (the shift), 3 a time mark, 4 a step's wave, 5 a step's length
  int grab = 0, grabIndex = 0;
  float grabFrom = 0, grabX0 = 0, grabWidth = 1;
  int grabWave = 0;
  bool dragging = false;                 // a drag is in progress (one undo step)
};

// Whether the node type has a window of this kind.
bool hasVectorWindow(const std::string& nodeType);

void showVector(VectorWindow& window, const std::string& instrument, const std::string& node);

// Draws the window when open. True when the node's data or parameters changed (the caller then runs syncData).
bool drawVectorWindow(VectorWindow& window, Document& doc, const Engine& engine);

}  // namespace wax::app

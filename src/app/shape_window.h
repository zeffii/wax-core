// The drawing window of an oscillator's wave (one cycle, 128 points) and a waveshaper's transfer curve (129 points from x = -1 to 1): drag across
// the pad to draw (the gaps between mouse events are filled in), or start from a preset. Every change replaces the node's data.wave or data.curve
// as a whole, so the sound follows the mouse; a drag is one undo step.
#pragma once

#include <string>

#include "edit/document.h"
#include "engine/engine.h"

namespace wax::app {

struct ShapeWindow {
  bool open = false;
  std::string instrument, node;
  bool dragging = false;
  int lastIndex = -1;      // the point last painted in this drag
  double lastValue = 0;
  int grabTap = -1, grabLane = 0;   // taps: the tap held and its lane (1 level and time, 2 pan)
  int op = 0;              // the FM synth's operator whose wave is drawn (0..3)
  bool symmetric = false;  // curve: the left half mirrors the right through the origin
};

// Whether the node type has a wave or curve to draw.
bool hasShape(const std::string& nodeType);

void showShape(ShapeWindow& window, const std::string& instrument, const std::string& node);

// Draws the window when open. True when the shape changed (the caller then runs syncData).
bool drawShapeWindow(ShapeWindow& window, Document& doc);

}  // namespace wax::app

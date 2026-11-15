// The envelope canvas: one drawing as a Bezier spline, edited the way the glyph editor edits a path. Square anchors, round handles on
// thin arms, the chosen anchor in pink, a ring on the first anchor, the area under the curve lightly filled. The first and last
// anchors stay at the ends of the time axis and the others never pass their neighbours. Port of the canvas of webtracker
// src/graph/bezieredit.js.
//
//   press empty space        add an anchor there            press an anchor        choose it and move it
//   drag a handle            bend the curve (Alt: break the handles apart)
//   double-click a curve     put an anchor on it, shape unchanged        double-click an anchor    sharp or smooth
//   right-click an anchor    remove it                      right-click a curve    a segment line or curve
//   Delete                   remove the chosen anchor       arrows   nudge it (Shift for 10)    Ctrl+Shift-drag  along one axis only
// The point types (sharp, smooth, equal, free), the sustain knot and the rest are the buttons of the tool row. Single letters are not
// used on the canvas: they are piano keys.
#pragma once

#include "imgui.h"
#include "nodes/bezier.h"

namespace wax::app {

struct EnvelopeView {
  int selected = -1;
  int snapAt = 0;           // 0: off, else the number of steps across the drawing
  bool logTime = false;     // the start of the drawing gets most of the room
  bool focused = false;     // the canvas has the keys (Delete and the arrows)
  // what the mouse is doing
  int hoverKind = 0, hoverIndex = -1;     // 1 anchor, 2 left handle, 3 right handle, 4 curve
  int dragKind = 0, dragIndex = -1;
  double fromX = 0, fromY = 0, fromMouseX = 0, fromMouseY = 0;
  bool dragStepped = false;               // the drag has already made its first step
  bool wantsKeys() const { return focused; }
};

struct EnvelopeSpec {
  const char* title = "";
  double totalMs = 0;       // how long the drawing takes; 0 when it is not in ms (the axis then reads in percent)
  bool knot = false;        // the sustain knot means something here
  bool firstHandle = false; // the first anchor always shows its handle
};

struct EnvelopeResult {
  bool changed = false;     // `spline` was changed this frame: write it back
  bool fold = false;        // as one undo step with the change before it (a drag in progress)
};

// Draws the canvas into the space of `size` and handles the mouse and keys. `spline` is the drawing as it is; when the result says
// changed, it holds the new one.
EnvelopeResult drawEnvelopeCanvas(EnvelopeView& view, const char* id, bezier::Spline& spline, const EnvelopeSpec& spec, ImVec2 size);

// The tool row: shapes, point types, auto, flip, reverse, sustain, snap, time. Changes `spline` and `view` in place.
EnvelopeResult drawEnvelopeTools(EnvelopeView& view, bezier::Spline& spline, const EnvelopeSpec& spec);

}  // namespace wax::app

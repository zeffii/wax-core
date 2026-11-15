// What the envelope editor does to a drawing, as plain functions on a spline (so each can be tested without a window). The way points
// move follows the glyph editor: the first and last anchors stay at the ends of the time axis, the others never pass their neighbours.
// Port of the editing half of webtracker src/graph/bezieredit.js.
#pragma once

#include "nodes/bezier.h"

namespace wax::bezier {

enum class Side { Left, Right };

inline constexpr double kMinGap = 0.004;   // how close an anchor may come to its neighbour in time

// The type of an anchor: "" is smooth.
const std::string& kindOf(const Anchor& anchor);

// Writes both of an anchor's handles out in full (the ends have one handle only; the other is zero).
void materialize(Spline& spline, std::size_t i);

// A new anchor at (x, y); returns its index. The end anchors keep the ends.
int addAnchor(Spline& spline, double x, double y);

// A new anchor on a curve where nearestOnSpline found it, the shape unchanged; returns its index.
int insertOn(Spline& spline, const Nearest& at);

// Removes an anchor that is not the first or last. False when it cannot be removed.
bool removeAnchor(Spline& spline, std::size_t i);

// Moves an anchor to (x, y): the ends move in level only, the others stay between their neighbours.
void moveAnchor(Spline& spline, std::size_t i, double x, double y);

// Puts the tip of one handle at (x, y); the other handle follows by the anchor's type. With `breakApart` the anchor becomes free.
void setHandle(Spline& spline, std::size_t i, Side side, double x, double y, bool breakApart);

// Makes an anchor "smooth", "equal", "free" or "sharp" ("" is smooth too).
void setKind(Spline& spline, std::size_t i, const std::string& kind);

// The sustain knot: one only, never on the last anchor; asking again for the same anchor takes it off.
void setKnot(Spline& spline, std::size_t i);

// A segment between two anchors: a straight line (no handles) or a curve with handles at a third and two thirds.
void toggleLine(Spline& spline, std::size_t i);

void autoAll(Spline& spline);    // every handle worked out again
void flip(Spline& spline);       // upside down
void reverse(Spline& spline);    // back to front

}  // namespace wax::bezier

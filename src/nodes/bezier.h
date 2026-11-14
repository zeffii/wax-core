// A one-shot envelope drawn as a Bezier spline, the part the nodes play (drum synth 2, hat 2, env 2). The drawing is a list
// of anchors with handles; it is played as a table of 257 values. Port of the playing half of webtracker
// src/audio/nodes/bezier.js (the editor's half, splitSegment and nearestOnSpline, is for the window and not needed here).
#pragma once

#include <array>
#include <optional>
#include <vector>

#include "core/node.h"

namespace wax::bezier {

inline constexpr int kEnvPoints = 256;

struct Anchor {
  double x = 0, y = 0;
  bool hasLeft = false, hasRight = false;   // a handle that is null is worked out to make the curve smooth
  double left[2] = {0, 0}, right[2] = {0, 0};
  bool sustain = false;                     // the sustain knot
};
using Spline = std::vector<Anchor>;
using Table = std::array<float, kEnvPoints + 1>;

enum class Shape { Fall, Line, Punch, Hump, Hold, Flat };
Spline shapeOf(Shape shape);

// Whatever was stored becomes a clean spline: sorted, from x = 0 to x = 1, levels 0 to 1, at least two anchors.
// `stored` is the JSON array of anchors (or nullptr); without a usable one the fallback is used.
Spline cleanSpline(const Json* stored, Shape fallback = Shape::Fall);
Spline cleanSpline(const Json* stored, const Spline& fallback);

// An anchor, for writing a default drawing by hand: handles may be null (worked out to make the curve smooth).
Anchor makeAnchor(double x, double y, const double* left, const double* right);

// Where the sustain knot is: its x, or nothing when the drawing has none.
struct Knot { int index; double x, y; };
std::optional<Knot> sustainKnot(const Spline& spline);

Table envelopeTable(const Spline& spline);

// The table read at `pos`, 0 to 1 (a position past the end holds the last value).
double readEnvelope(const Table& table, double pos);

// The drawing `name` of a node's data (data.env.<name>) as a spline, or the fallback.
Spline drawingOf(const Json* data, const char* name, Shape fallback = Shape::Fall);
Spline drawingOf(const Json* data, const char* name, const Spline& fallback);

}  // namespace wax::bezier

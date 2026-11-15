// The drawn envelopes of the node types that have them: which drawings a type holds, what they are called in the window, the
// parameter that sets how long a drawing takes, and the drawing a node has until one is made. The nodes and the envelope editor
// both read the defaults from here, so they cannot disagree.
#pragma once

#include <string>
#include <vector>

#include "nodes/bezier.h"

namespace wax {

struct DrawingInfo {
  std::string name;        // the key in the node's data (data.env.<name>)
  std::string title;       // shown above the drawing
  std::string timeParam;   // the parameter that is the drawing's length in ms; empty when the length is not in ms
  bool knot = false;       // whether a sustain knot means anything here
};

// The drawings of a node type, in the order they are shown; empty for a type without any.
const std::vector<DrawingInfo>& drawingsOf(const std::string& nodeType);

// The drawing a node plays until one is made. Falls back to the plain fall for a name it does not know.
bezier::Spline defaultDrawing(const std::string& nodeType, const std::string& name);

}  // namespace wax

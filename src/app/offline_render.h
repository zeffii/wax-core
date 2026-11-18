// The node's own code run offline, so a picture shows what is heard: a note into a node and what comes out, and the curve of one ADSR.
#pragma once

#include <vector>

#include "core/node.h"

namespace wax::app {

// Runs a node of `type` for `ms` milliseconds: a note on at the start and, when offAt >= 0, a note off at that time. The values are the
// node's parameters in the type's order. Returns the left output.
std::vector<float> renderOffline(const NodeType& type, std::vector<float> values, int note, double vel, double ms, double sr, double offAt, const Json* data = nullptr);

// One ADSR (times in ms), a value for each millisecond: held for `hold` ms, then let go; `total` values.
std::vector<float> adsrCurve(double a, double d, double s, double r, int hold, int total);

}  // namespace wax::app

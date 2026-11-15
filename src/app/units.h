// How a parameter's value is shown in real units: "200 ms", "1.2 kHz", "+7 st", "triangle". Port of webtracker src/graph/units.js
// (the tables are generated from it: units_table.inc). A unit is looked up by node type and parameter name, then by the name alone.
#pragma once

#include <string>
#include <vector>

#include "core/node.h"

namespace wax::app {

// The unit of a parameter ("" none): "ms", "Hz", "%", "st", "ct", "dB", "oct", "x", ":1", ...
std::string unitOf(const std::string& nodeType, const std::string& param);

// The names of a parameter's choices (empty when it has none).
const std::vector<std::string>& choicesOf(const std::string& nodeType, const std::string& param);

// The value as text in real units.
std::string displayValue(const std::string& nodeType, const ParamSpec& spec, double value);

// The value as typed (in the same units, e.g. "1.2k" for Hz or "250" for ms): parses what the user typed back to the parameter's value.
// False when it cannot be read.
bool parseValue(const std::string& nodeType, const ParamSpec& spec, const std::string& text, double& value);

}  // namespace wax::app

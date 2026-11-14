// The small drawn controls every panel is made of: thin lines, dots and round knobs, no boxes. One place for the look, so a
// panel written later comes out like the others. Colours come from theme.h.
#pragma once

#include <string>

#include "imgui.h"

namespace wax::app {

struct KnobResult {
  bool changed = false;   // the value moved this frame
  bool active = false;    // the mouse has hold of the knob (a drag in progress)
};

struct KnobSpec {
  double min = 0, max = 1, def = 0;
  bool isInt = false;
  bool log = false;       // the knob moves through the range by ratio instead of by amount (needs min > 0)
  float diameter = 34;    // in points; scaled with the window
};

// A round knob with its name above and its value below. Drag up and down; hold Shift for fine steps; the wheel nudges;
// double-click puts the starting value back; right-click types a value.
KnobResult knob(const char* id, const char* label, double& value, const KnobSpec& spec);

// A value as short text: "440", "0.25", "12.5", "3".
std::string valueText(double value, bool isInt);

// A thin line down the height of the current row, between groups of controls.
void groupDivider(float height);

}  // namespace wax::app

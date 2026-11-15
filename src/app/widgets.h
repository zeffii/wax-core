// The small drawn controls every panel is made of: thin lines, dots and sliders, no boxes. One place for the look, so a
// panel written later comes out like the others. Colours come from theme.h.
#pragma once

#include <string>

#include "imgui.h"

namespace wax::app {

struct SliderResult {
  bool changed = false;   // the value moved this frame
  bool active = false;    // the mouse has hold of the slider (a drag in progress)
};

struct SliderSpec {
  double min = 0, max = 1, def = 0;
  bool isInt = false;
  bool log = false;       // the slider moves through the range by ratio instead of by amount (needs min > 0)
  float width = 150;      // in points; scaled with the window
};

// A slider as in the web app: a hairline track, a faint fill up to the value, one thin accent bar at it. The name is above on
// the left and the value above on the right. Press anywhere on the track to jump there and drag to move; the wheel nudges;
// double-click puts the starting value back; right-click types a value.
SliderResult slider(const char* id, const char* label, double& value, const SliderSpec& spec);

// A value as short text: "440", "0.25", "12.5", "3".
std::string valueText(double value, bool isInt);

// A thin line down the height of the current row, between groups of controls.
void groupDivider(float height);

// A word that acts as a button: dim, lit when hovered or `on`, no box. True when clicked.
bool textButton(const char* label, bool on = false);

}  // namespace wax::app

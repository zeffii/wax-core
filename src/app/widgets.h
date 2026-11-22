// The small drawn controls every panel is made of: thin lines, dots and sliders, no boxes. One place for the look, so a
// panel written later comes out like the others. Colours come from theme.h.
#pragma once

#include <initializer_list>
#include <string>
#include <utility>

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
  float labelWidth = 0;   // above 0 the slider is one line: the name in this much width, the track, then the value; `width` is the whole line
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
// (`strongIdle`: the text colour when idle, like the transport icons, rather than the dim one.)
bool textButton(const char* label, bool on = false, bool strongIdle = false);

// A row of a menu: the words in the normal text colour (a dim row would look unavailable), lit when hovered, with the shortcut dim at the
// right edge of a menu `width` wide. `arrow` marks a row that opens a submenu. True when clicked.
bool menuRow(const char* label, const char* shortcut, float width, bool arrow = false);
// The width a menu needs for rows of these labels and shortcuts.
float menuWidth(std::initializer_list<std::pair<const char*, const char*>> rows);

// A tick box, drawn at kTickScale of the standard size (the transport icons are scaled down by the same amount).
inline constexpr float kTickScale = 0.75f;
bool checkbox(const char* label, bool* value);

// A spin box: the value in an inset box with a pair of small arrows stacked at its right edge, as in a classic number field. The caller
// formats the value. Returns +1 or -1 when an arrow is clicked (the mouse wheel over the box counts too), else 0.
int spinBox(const char* id, const char* text, float width);

// A translucent area (under a curve, say) filled without the faint lines along the seams of its triangles: ImGui cuts a concave outline into
// triangles and antialiases the edge of each, so where two share an edge a see-through colour shows a thin line. Here the fill is not
// antialiased (its triangles then meet exactly); the curve's own line drawn over the top edge keeps that edge smooth.
void addAreaFilled(ImDrawList* dl, const ImVec2* points, int count, ImU32 colour);

}  // namespace wax::app

// The graph's toolbar: a small row of icon switches, centred along the top edge of the graph window and inside it. Each switch is a setting
// that is also in Config (grid, snap, dashed wires, straight short wires, pull out, hide command parameters), so the toolbar only presents
// them: a click flips the setting and keeps it. Hovering a switch says what it does and whether it is on.
// (The web's seventh switch, compact nodes, waits for the classic node look: native has only the compact one.)
#pragma once

#include "app/toolbar.h"
#include "imgui.h"

namespace wax::app {

// Where it sits in the graph window [areaMin, areaMax] (screen pixels); the graph leaves clicks there alone.
ToolbarBox graphToolbarBox(ImVec2 areaMin, ImVec2 areaMax, float scale);

// Draws the toolbar and flips the switch that is clicked (`active`: the mouse may act on it, i.e. nothing else is over it).
void drawGraphToolbar(ImDrawList* dl, ImVec2 areaMin, ImVec2 areaMax, float scale, bool active);

}  // namespace wax::app

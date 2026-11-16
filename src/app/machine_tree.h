// The machine tree: a popup listing every kind of instrument that can be added, in groups that fold open and shut, drawn as a tree of
// thin lines (the web app's ui/machinetree.js).
//   Up / Down    move through the rows showing         Home / End   first / last row
//   Right        open a group, or step into it         Left         close a group, or step out to the one it is in
//   Enter        pick a machine (on a group: fold)     Esc          close         a letter  jumps to the next row starting with it
// Which groups are open is kept for the session, and the menu opens on the machine picked last.
#pragma once

#include <string>

#include "imgui.h"

namespace wax::app {

// Opens the popup at the mouse.
void openMachineTree();

// Draws the popup if it is open; returns the kind picked ("" while nothing is). Machines that play samples are greyed until the project has one.
std::string drawMachineTree(bool haveSamples);

}  // namespace wax::app

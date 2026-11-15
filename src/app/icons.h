// Icons: the web app's vector drawings (art/icons/*.svg, drawn white) rasterised at run time at the size they are shown, kept as textures
// per size (so a change of display scale makes the new size once), and drawn tinted: dim, lit when hovered or on. No frames.
#pragma once

#include <string>

#include "imgui.h"

struct SDL_Renderer;

namespace wax::app {

// Gives the icons the renderer their textures are made with. Call once after it is created, and iconsShutdown before it is destroyed.
void iconsInit(SDL_Renderer* renderer);
void iconsShutdown();

// An icon as a button: `name` is the file name without .svg ("arrange", "play"); `size` in points (scaled with the window).
// `tip` shows when hovered. True when clicked. A name with no file shows its text instead.
bool iconButton(const char* name, const char* tip, bool on = false, float size = 22);

// A drawing at its own proportions (not a square button): its top left at `min`, `width` points wide (scaled with the window), height
// `aspect` times the width, tinted. Used for the logo on the splash screen.
void drawIcon(const char* name, ImVec2 min, float width, float aspect, ImU32 tint);

}  // namespace wax::app

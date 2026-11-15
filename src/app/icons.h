// Icons: the web app's vector drawings (art/icons/*.svg, drawn white) rasterised at run time at the size they are shown, kept as textures
// per size (so a change of display scale makes the new size once), and drawn tinted: dim, lit when hovered or on. No frames.
#pragma once

#include <string>

struct SDL_Renderer;

namespace wax::app {

// Gives the icons the renderer their textures are made with. Call once after it is created, and iconsShutdown before it is destroyed.
void iconsInit(SDL_Renderer* renderer);
void iconsShutdown();

// An icon as a button: `name` is the file name without .svg ("arrange", "play"); `size` in points (scaled with the window).
// `tip` shows when hovered. True when clicked. A name with no file shows its text instead.
bool iconButton(const char* name, const char* tip, bool on = false, float size = 22);

}  // namespace wax::app

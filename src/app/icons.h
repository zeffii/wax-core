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
bool iconButton(const char* name, const char* tip, bool on = false, float size = 22, bool strongIdle = false);   // strongIdle: the normal text colour when not lit (the transport, as in the web app)

// A picture made of pixels (`rgba`, w x h, 4 bytes each), kept as a texture under `key` and replaced each time this is called with that key.
// With `rgba` null it only gives the texture already kept. Returns 0 when there is none. Used for the spectrogram.
ImTextureID pixelTexture(const std::string& key, const unsigned char* rgba, int w, int h);

// A drawing at its own proportions (not a square button): its top left at `min`, `width` points wide (scaled with the window), height
// `aspect` times the width, tinted. Drawn over everything (the splash screen's logo) unless `list` is given (the logo in the header).
void drawIcon(const char* name, ImVec2 min, float width, float aspect, ImU32 tint, ImDrawList* list = nullptr);

}  // namespace wax::app

#include "app/icons.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "app/theme.h"
#include "app/widgets.h"
#include "imgui.h"

// nanosvg is vendored as it is; its warnings are not ours to fix
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace wax::app {

namespace {

SDL_Renderer* gRenderer = nullptr;
std::map<std::pair<std::string, int>, SDL_Texture*> gTextures;   // (name, pixel size) -> texture (nullptr: no such icon)

#ifndef WAX_ICON_DIR
#define WAX_ICON_DIR "art/icons"
#endif

// Rasterises art/icons/<name>.svg to a square of `pixels`: white on transparent, so the tint colours it.
SDL_Texture* rasterise(const std::string& name, int pixels) {
  if (!gRenderer) return nullptr;
  NSVGimage* image = nsvgParseFromFile((std::string(WAX_ICON_DIR) + "/" + name + ".svg").c_str(), "px", 96.0f);
  if (!image) return nullptr;
  NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
  std::vector<unsigned char> rgba(static_cast<std::size_t>(pixels) * static_cast<std::size_t>(pixels) * 4, 0);
  const float scale = static_cast<float>(pixels) / std::max(image->width, image->height);
  nsvgRasterize(rasterizer, image, 0, 0, scale, rgba.data(), pixels, pixels, pixels * 4);
  nsvgDeleteRasterizer(rasterizer);
  nsvgDelete(image);
  SDL_Texture* texture = SDL_CreateTexture(gRenderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, pixels, pixels);
  if (!texture) return nullptr;
  SDL_UpdateTexture(texture, nullptr, rgba.data(), pixels * 4);
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
  return texture;
}

SDL_Texture* textureFor(const std::string& name, int pixels) {
  const auto key = std::make_pair(name, pixels);
  const auto found = gTextures.find(key);
  if (found != gTextures.end()) return found->second;
  SDL_Texture* texture = rasterise(name, pixels);
  gTextures[key] = texture;
  return texture;
}

}  // namespace

void iconsInit(SDL_Renderer* renderer) { gRenderer = renderer; }

void iconsShutdown() {
  for (auto& [key, texture] : gTextures) if (texture) SDL_DestroyTexture(texture);
  gTextures.clear();
  gRenderer = nullptr;
}

bool iconButton(const char* name, const char* tip, bool on, float size) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const float side = size * scale;
  ImGui::PushID(name);
  const bool clicked = ImGui::InvisibleButton("icon", ImVec2(side, side));
  ImGui::PopID();
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
  const ImU32 tint = on ? kAccent : hovered ? kText : kDimText;
  SDL_Texture* texture = textureFor(name, std::max(8, static_cast<int>(std::lround(side))));
  if (texture) {
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture)), min, max, ImVec2(0, 0), ImVec2(1, 1), tint);
  } else {
    const ImVec2 text = ImGui::CalcTextSize(name);   // no drawing for it: the name stands in
    ImGui::GetWindowDrawList()->AddText(ImVec2(min.x, min.y + (side - text.y) / 2), tint, name);
  }
  if (hovered && tip && *tip) ImGui::SetTooltip("%s", tip);
  return clicked;
}

void drawIcon(const char* name, ImVec2 min, float width, float aspect, ImU32 tint) {
  SDL_Texture* texture = textureFor(name, std::max(8, static_cast<int>(std::lround(width))));   // (square; the drawing fills its top `aspect`)
  if (!texture) return;
  ImGui::GetForegroundDrawList()->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture)), min, ImVec2(min.x + width, min.y + width * aspect),
                                           ImVec2(0, 0), ImVec2(1, aspect), tint);
}

}  // namespace wax::app

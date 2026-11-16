#include "app/fonts.h"

#include "imgui.h"
#ifdef IMGUI_ENABLE_FREETYPE
#include "misc/freetype/imgui_freetype.h"
#endif

namespace wax::app {
namespace {
#include "app/font_proggy_vector.inc"
}  // namespace

void loadFonts() {
  ImFontConfig config;
  config.OversampleH = config.OversampleV = 1;   // (the stb rasteriser's fallback: one sample per pixel, not blurred over three)
  config.PixelSnapH = true;
#ifdef IMGUI_ENABLE_FREETYPE
  config.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_ForceAutoHint;   // the auto-hinter aligns the strokes and the x-height to whole pixels
#endif
  ImGui::GetIO().Fonts->AddFontFromMemoryCompressedBase85TTF(proggyVector_compressed_data_base85, 15.0f, &config);
}

}  // namespace wax::app

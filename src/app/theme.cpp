#include "app/theme.h"

namespace wax::app {

void applyTheme(float dpiScale) {
  ImGuiStyle& style = ImGui::GetStyle();
  ImGui::StyleColorsDark(&style);
  style.WindowRounding = 0;
  style.FrameRounding = 3;
  style.GrabRounding = 3;
  style.TabRounding = 3;
  style.WindowBorderSize = 0;
  style.FrameBorderSize = 0;
  style.ItemSpacing = ImVec2(8, 5);
  style.CellPadding = ImVec2(6, 1);
  style.ScaleAllSizes(dpiScale);
  style.FontSizeBase = 15.0f;
  style.FontScaleDpi = dpiScale;

  const auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
  const ImVec4 background = rgb(22, 27, 32), panel = rgb(30, 37, 44), raised = rgb(42, 52, 61), hot = rgb(56, 70, 82);
  const ImVec4 text = rgb(214, 222, 228), accent = rgb(64, 190, 168);
  ImVec4* c = style.Colors;
  c[ImGuiCol_Text] = text;
  c[ImGuiCol_TextDisabled] = rgb(120, 134, 146);
  c[ImGuiCol_WindowBg] = background;
  c[ImGuiCol_ChildBg] = background;
  c[ImGuiCol_PopupBg] = panel;
  c[ImGuiCol_Border] = rgb(56, 70, 82, 0.6f);
  c[ImGuiCol_FrameBg] = panel;
  c[ImGuiCol_FrameBgHovered] = raised;
  c[ImGuiCol_FrameBgActive] = hot;
  c[ImGuiCol_Button] = raised;
  c[ImGuiCol_ButtonHovered] = hot;
  c[ImGuiCol_ButtonActive] = accent;
  c[ImGuiCol_Header] = raised;
  c[ImGuiCol_HeaderHovered] = hot;
  c[ImGuiCol_HeaderActive] = hot;
  c[ImGuiCol_Tab] = panel;
  c[ImGuiCol_TabHovered] = hot;
  c[ImGuiCol_TabSelected] = raised;
  c[ImGuiCol_TabSelectedOverline] = accent;
  c[ImGuiCol_TabDimmed] = panel;
  c[ImGuiCol_TabDimmedSelected] = raised;
  c[ImGuiCol_CheckMark] = accent;
  c[ImGuiCol_SliderGrab] = accent;
  c[ImGuiCol_TableHeaderBg] = panel;
  c[ImGuiCol_TableBorderLight] = rgb(42, 52, 61);
  c[ImGuiCol_TableBorderStrong] = rgb(56, 70, 82);
  c[ImGuiCol_ScrollbarBg] = background;
  c[ImGuiCol_ScrollbarGrab] = raised;
  c[ImGuiCol_Separator] = rgb(42, 52, 61);
}

}  // namespace wax::app

#include "app/theme.h"

#include <cstring>

namespace wax::app {

namespace {

struct Rgb { int r, g, b; };
struct ThemeDef {
  const char* name;
  bool light;
  Rgb bg, fg, panel, hi, hiFg, field, accent, cc, line;
};

#include "app/themes_table.inc"

int gTheme = 0;
float gDpi = 1;

ImU32 pack(Rgb c, int a = 255) { return IM_COL32(c.r, c.g, c.b, a); }
ImVec4 vec(Rgb c, float a = 1.0f) { return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, a); }
Rgb mix(Rgb a, Rgb b, float t) { return {static_cast<int>(a.r + (b.r - a.r) * t), static_cast<int>(a.g + (b.g - a.g) * t), static_cast<int>(a.b + (b.b - a.b) * t)}; }

void applyColours() {
  const ThemeDef& t = kThemes[gTheme];
  const Rgb dim = mix(t.fg, t.bg, 0.5f), raised = mix(t.bg, t.fg, 0.12f), hot = mix(t.bg, t.fg, 0.22f), soft = mix(t.fg, t.bg, 0.3f);
  kBg = pack(t.bg); kText = pack(t.fg); kPanel = pack(t.panel); kField = pack(t.field); kLine = pack(t.line);
  kSoft = pack(soft); kDimText = pack(dim); kAccent = pack(t.accent); kHiFg = pack(t.hiFg);
  kWarm = t.light ? IM_COL32(190, 110, 20, 255) : IM_COL32(226, 164, 74, 255);
  kBeatRow = withAlpha(kText, 10);
  kBarRow = withAlpha(kText, 22);
  kPlayRow = withAlpha(kAccent, 70);
  kCursorCell = withAlpha(pack(t.hi), 90);

  ImVec4* c = ImGui::GetStyle().Colors;
  c[ImGuiCol_Text] = vec(t.fg);
  c[ImGuiCol_TextDisabled] = vec(dim);
  c[ImGuiCol_WindowBg] = vec(t.bg);
  c[ImGuiCol_ChildBg] = vec(t.bg);
  c[ImGuiCol_PopupBg] = vec(t.panel);
  c[ImGuiCol_Border] = vec(t.line, 0.6f);
  c[ImGuiCol_FrameBg] = vec(t.field);
  c[ImGuiCol_FrameBgHovered] = vec(raised);
  c[ImGuiCol_FrameBgActive] = vec(hot);
  c[ImGuiCol_Button] = vec(raised);
  c[ImGuiCol_ButtonHovered] = vec(hot);
  c[ImGuiCol_ButtonActive] = vec(t.accent);
  c[ImGuiCol_Header] = vec(raised);
  c[ImGuiCol_HeaderHovered] = vec(hot);
  c[ImGuiCol_HeaderActive] = vec(hot);
  c[ImGuiCol_Tab] = vec(t.panel);
  c[ImGuiCol_TabHovered] = vec(hot);
  c[ImGuiCol_TabSelected] = vec(raised);
  c[ImGuiCol_TabSelectedOverline] = vec(t.accent);
  c[ImGuiCol_TabDimmed] = vec(t.panel);
  c[ImGuiCol_TabDimmedSelected] = vec(raised);
  c[ImGuiCol_CheckMark] = vec(t.accent);
  c[ImGuiCol_SliderGrab] = vec(t.accent);
  c[ImGuiCol_TableHeaderBg] = vec(t.panel);
  c[ImGuiCol_TableBorderLight] = vec(raised);
  c[ImGuiCol_TableBorderStrong] = vec(t.line);
  c[ImGuiCol_ScrollbarBg] = vec(t.bg);
  c[ImGuiCol_ScrollbarGrab] = vec(raised);
  c[ImGuiCol_Separator] = vec(raised);
  c[ImGuiCol_TitleBg] = vec(t.panel);
  c[ImGuiCol_TitleBgActive] = vec(mix(t.panel, t.accent, 0.35f));
}

}  // namespace

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
  gDpi = dpiScale;
  applyColours();
}

int themeCount() { return static_cast<int>(sizeof kThemes / sizeof kThemes[0]); }
const char* themeName(int index) { return kThemes[index].name; }
int themeIndex() { return gTheme; }

void setTheme(int index) {
  if (index < 0 || index >= themeCount()) return;
  gTheme = index;
  applyColours();
}

bool setThemeByName(const char* name) {
  for (int i = 0; i < themeCount(); i++) if (std::strcmp(kThemes[i].name, name) == 0) { setTheme(i); return true; }
  return false;
}

}  // namespace wax::app

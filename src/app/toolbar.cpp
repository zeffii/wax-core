#include "app/toolbar.h"

#include <cmath>

#include "app/theme.h"

namespace wax::app {

namespace {
constexpr float kButton = 22, kGap = 4, kPad = 4;   // web pixels
}

float toolbarHeight(float scale) { return (kButton + kPad * 2) * scale; }

ToolbarBox toolbarBox(ImVec2 areaMin, ImVec2 areaMax, float scale, int switches) {
  ToolbarBox box;
  const float w = (kPad * 2 + static_cast<float>(switches) * kButton + static_cast<float>(switches - 1) * kGap) * scale;
  if (areaMax.x - areaMin.x < w + 2 * kToolbarTop * scale) return box;   // no room: no toolbar
  box.min = ImVec2(std::round((areaMin.x + areaMax.x - w) / 2), areaMin.y + kToolbarTop * scale);
  box.max = ImVec2(box.min.x + w, box.min.y + toolbarHeight(scale));
  return box;
}

void drawToolbar(ImDrawList* dl, const ToolbarBox& box, const std::vector<ToolbarSwitch>& switches, float scale, bool active) {
  if (box.max.x <= box.min.x) return;
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  dl->AddRectFilled(box.min, box.max, IM_COL32(24, 32, 36, 215), 5 * scale);
  dl->AddRect(box.min, box.max, lineAlpha(160), 5 * scale);
  for (std::size_t i = 0; i < switches.size(); i++) {
    const ToolbarSwitch& sw = switches[i];
    const ImVec2 lo(box.min.x + (kPad + static_cast<float>(i) * (kButton + kGap)) * scale, box.min.y + kPad * scale), hi(lo.x + kButton * scale, lo.y + kButton * scale);
    const bool on = sw.isOn(), over = active && mouse.x >= lo.x && mouse.x <= hi.x && mouse.y >= lo.y && mouse.y <= hi.y;
    if (on) dl->AddRectFilled(lo, hi, accentAlpha(48), 3 * scale);
    if (over) dl->AddRect(lo, hi, accentAlpha(160), 3 * scale);
    const float unit = kButton * scale * 0.6f / 16;   // the icon fills about 60% of the button
    sw.icon(dl, ImVec2(lo.x + (kButton * scale - 16 * unit) / 2, lo.y + (kButton * scale - 16 * unit) / 2), unit, on ? kAccent : kDimText);
    if (over) {
      ImGui::SetTooltip("%s · %s · %s", sw.name, on ? "on" : "off", sw.what);
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) sw.flip();
    }
  }
}

}  // namespace wax::app

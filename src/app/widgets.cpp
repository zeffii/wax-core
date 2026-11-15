#include "app/widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app/theme.h"

namespace wax::app {
namespace {

double toUnit(double value, const SliderSpec& s) {
  if (s.max <= s.min) return 0;
  const double t = s.log && s.min > 0 && value > 0 ? std::log(value / s.min) / std::log(s.max / s.min) : (value - s.min) / (s.max - s.min);
  return std::clamp(t, 0.0, 1.0);
}

double fromUnit(double t, const SliderSpec& s) {
  t = std::clamp(t, 0.0, 1.0);
  const double v = s.log && s.min > 0 ? s.min * std::pow(s.max / s.min, t) : s.min + t * (s.max - s.min);
  return s.isInt ? std::round(v) : v;
}

}  // namespace

std::string valueText(double value, bool isInt) {
  char text[32];
  if (isInt) std::snprintf(text, sizeof text, "%d", static_cast<int>(std::lround(value)));
  else if (std::fabs(value) >= 1000) std::snprintf(text, sizeof text, "%.0f", value);
  else if (std::fabs(value) >= 100) std::snprintf(text, sizeof text, "%.1f", value);
  else if (std::fabs(value) >= 10) std::snprintf(text, sizeof text, "%.2f", value);
  else std::snprintf(text, sizeof text, "%.3f", value);
  return text;
}

SliderResult slider(const char* id, const char* label, double& value, const SliderSpec& spec) {
  SliderResult result;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const float width = spec.width * scale;
  const float lineHeight = ImGui::GetTextLineHeight();
  const float trackHeight = 16 * scale;

  ImGui::PushID(id);
  ImGui::BeginGroup();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();

  // the track: one invisible button under the name line
  const ImVec2 track(origin.x, origin.y + lineHeight + 2 * scale);
  ImGui::SetCursorScreenPos(track);
  ImGui::InvisibleButton("##slider", ImVec2(width, trackHeight));
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  result.active = active;

  if (active) {   // press jumps, drag moves: the value is where the pointer is
    const double wanted = fromUnit((ImGui::GetIO().MousePos.x - track.x) / width, spec);
    if (wanted != value) { value = wanted; result.changed = true; }
  }
  if (hovered && !active && ImGui::GetIO().MouseWheel != 0) {
    const double nudge = ImGui::GetIO().MouseWheel * (ImGui::GetIO().KeyShift ? 0.002 : 0.02);
    const double wanted = spec.isInt ? std::clamp(std::round(value) + (ImGui::GetIO().MouseWheel > 0 ? 1 : -1), spec.min, spec.max) : fromUnit(toUnit(value, spec) + nudge, spec);
    if (wanted != value) { value = wanted; result.changed = true; }
  }
  if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    if (value != spec.def) { value = spec.def; result.changed = true; }
  }
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##type");
  if (ImGui::BeginPopup("##type")) {
    static char buffer[32];
    if (ImGui::IsWindowAppearing()) { std::snprintf(buffer, sizeof buffer, "%s", valueText(value, spec.isInt).c_str()); ImGui::SetKeyboardFocusHere(); }
    ImGui::SetNextItemWidth(90 * scale);
    if (ImGui::InputText("##value", buffer, sizeof buffer, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll)) {
      const double typed = std::clamp(std::atof(buffer), spec.min, spec.max);
      const double wanted = spec.isInt ? std::round(typed) : typed;
      if (wanted != value) { value = wanted; result.changed = true; }
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  // the drawing: a hairline, a faint fill up to the value, one thin bar at it
  const double unit = toUnit(value, spec);
  const float mid = track.y + trackHeight * 0.5f;
  const float x = track.x + width * static_cast<float>(unit);
  const ImU32 bar = active || hovered ? kWarm : kAccent;
  draw->AddRectFilled(ImVec2(track.x, mid - 4 * scale), ImVec2(x, mid + 4 * scale), IM_COL32(64, 190, 168, 34));
  draw->AddLine(ImVec2(track.x, mid), ImVec2(track.x + width, mid), IM_COL32(70, 84, 98, 255), 1);
  draw->AddRectFilled(ImVec2(x - std::max(1.0f, scale), mid - 7 * scale), ImVec2(x + std::max(1.0f, scale), mid + 7 * scale), bar);

  // the name on the left, the value on the right
  draw->AddText(origin, kDimText, label);
  const std::string text = valueText(value, spec.isInt);
  draw->AddText(ImVec2(origin.x + width - ImGui::CalcTextSize(text.c_str()).x, origin.y), active ? kWarm : kDimText, text.c_str());

  ImGui::SetCursorScreenPos(ImVec2(origin.x, track.y + trackHeight));
  ImGui::Dummy(ImVec2(width, 1));
  ImGui::EndGroup();
  ImGui::PopID();
  return result;
}

void groupDivider(float height) {
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x + 6 * scale, at.y), ImVec2(at.x + 6 * scale, at.y + height), IM_COL32(60, 72, 84, 255), 1);
  ImGui::Dummy(ImVec2(12 * scale, height));
}

}  // namespace wax::app

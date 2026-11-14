#include "app/widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app/theme.h"

namespace wax::app {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kStart = kPi * 0.75f;          // where the knob's range begins: lower left
constexpr float kSweep = kPi * 1.5f;           // and how far round it goes

double toUnit(double value, const KnobSpec& s) {
  if (s.max <= s.min) return 0;
  const double t = s.log && s.min > 0 && value > 0 ? std::log(value / s.min) / std::log(s.max / s.min) : (value - s.min) / (s.max - s.min);
  return std::clamp(t, 0.0, 1.0);
}

double fromUnit(double t, const KnobSpec& s) {
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

KnobResult knob(const char* id, const char* label, double& value, const KnobSpec& spec) {
  KnobResult result;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const float size = spec.diameter * scale;
  const float width = std::max(size + 14 * scale, ImGui::CalcTextSize(label).x);

  ImGui::PushID(id);
  ImGui::BeginGroup();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();

  // the name, centred above
  const float labelWidth = ImGui::CalcTextSize(label).x;
  draw->AddText(ImVec2(origin.x + (width - labelWidth) * 0.5f, origin.y), kDimText, label);
  const float lineHeight = ImGui::GetTextLineHeight();

  // the knob itself: one invisible button the size of the circle
  const ImVec2 box(origin.x + (width - size) * 0.5f, origin.y + lineHeight + 2 * scale);
  ImGui::SetCursorScreenPos(box);
  ImGui::InvisibleButton("##knob", ImVec2(size, size));
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  result.active = active;

  double unit = toUnit(value, spec);
  static double dragUnit = 0;   // where the drag has taken the knob, before rounding (only one knob is dragged at a time)
  if (ImGui::IsItemActivated()) dragUnit = unit;
  if (active && ImGui::GetIO().MouseDelta.y != 0) {
    const double fine = ImGui::GetIO().KeyShift ? 0.1 : 1.0;
    dragUnit = std::clamp(dragUnit - ImGui::GetIO().MouseDelta.y * 0.005 * fine, 0.0, 1.0);
    const double wanted = fromUnit(dragUnit, spec);
    if (wanted != value) { value = wanted; result.changed = true; }
  }
  if (hovered && ImGui::GetIO().MouseWheel != 0) {
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

  // the drawing: a thin track, the part of it that is filled, and a dot where the value is
  unit = toUnit(value, spec);
  const ImVec2 centre(box.x + size * 0.5f, box.y + size * 0.5f);
  const float radius = size * 0.5f - 3 * scale;
  const float line = std::max(1.0f, 1.5f * scale);
  draw->PathArcTo(centre, radius, kStart, kStart + kSweep, 32);
  draw->PathStroke(IM_COL32(60, 72, 84, 255), 0, line);
  if (unit > 0) {
    draw->PathArcTo(centre, radius, kStart, kStart + kSweep * static_cast<float>(unit), 32);
    draw->PathStroke(active || hovered ? kWarm : kAccent, 0, line * 1.4f);
  }
  const float angle = kStart + kSweep * static_cast<float>(unit);
  draw->AddCircleFilled(ImVec2(centre.x + std::cos(angle) * radius * 0.62f, centre.y + std::sin(angle) * radius * 0.62f), 2.2f * scale,
                        active || hovered ? kWarm : IM_COL32(205, 214, 222, 255));

  // the value, centred below
  const std::string text = valueText(value, spec.isInt);
  const float textWidth = ImGui::CalcTextSize(text.c_str()).x;
  draw->AddText(ImVec2(origin.x + (width - textWidth) * 0.5f, box.y + size + 2 * scale), active ? kWarm : kDimText, text.c_str());

  ImGui::SetCursorScreenPos(ImVec2(origin.x, box.y + size + 2 * scale + lineHeight));
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

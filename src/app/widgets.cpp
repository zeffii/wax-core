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
  const bool oneLine = spec.labelWidth > 0;
  const float totalWidth = spec.width * scale;
  const float width = oneLine ? totalWidth - spec.labelWidth * scale - 26 * scale : totalWidth;   // the track's
  const float lineHeight = ImGui::GetTextLineHeight();
  const float trackHeight = oneLine ? lineHeight + 2 * scale : 16 * scale;
  const float labelWidth = spec.labelWidth * scale;

  ImGui::PushID(id);
  ImGui::BeginGroup();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();

  // the track: one invisible button under the name line
  const ImVec2 track(origin.x + (oneLine ? labelWidth : 0), oneLine ? origin.y : origin.y + lineHeight + 2 * scale);
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
  draw->AddRectFilled(ImVec2(track.x, mid - (oneLine ? 3 : 4) * scale), ImVec2(x, mid + (oneLine ? 3 : 4) * scale), accentAlpha(34));
  draw->AddLine(ImVec2(track.x, mid), ImVec2(track.x + width, mid), kLine, 1);
  draw->AddRectFilled(ImVec2(x - std::max(1.0f, scale), mid - (oneLine ? 5 : 7) * scale), ImVec2(x + std::max(1.0f, scale), mid + (oneLine ? 5 : 7) * scale), bar);

  // the name on the left, the value on the right
  draw->AddText(origin, kDimText, label);
  const std::string text = valueText(value, spec.isInt);
  draw->AddText(ImVec2(origin.x + totalWidth - ImGui::CalcTextSize(text.c_str()).x, origin.y), active ? kWarm : kDimText, text.c_str());

  ImGui::SetCursorScreenPos(ImVec2(origin.x, track.y + trackHeight));
  ImGui::Dummy(ImVec2(totalWidth, 1));
  ImGui::EndGroup();
  ImGui::PopID();
  return result;
}

void groupDivider(float height) {
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x + 6 * scale, at.y), ImVec2(at.x + 6 * scale, at.y + height), kLine, 1);
  ImGui::Dummy(ImVec2(12 * scale, height));
}

bool textButton(const char* label, bool on) {
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const ImVec2 size = ImGui::CalcTextSize(label);
  ImGui::PushID(label);
  ImGui::InvisibleButton("##text", size);
  ImGui::PopID();
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImGui::GetWindowDrawList()->AddText(at, on ? kAccent : hovered ? kText : kDimText, label);
  return ImGui::IsItemClicked();
}

bool menuRow(const char* label, const char* shortcut, float width, bool arrow) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float height = ImGui::GetTextLineHeight() + 2 * scale;
  ImGui::PushID(label);
  ImGui::InvisibleButton("##row", ImVec2(width, height));
  ImGui::PopID();
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(ImVec2(at.x, at.y + scale), hovered ? kAccent : kText, label);
  if (shortcut && *shortcut) dl->AddText(ImVec2(at.x + width - ImGui::CalcTextSize(shortcut).x, at.y + scale), kDimText, shortcut);
  if (arrow) dl->AddText(ImVec2(at.x + width - ImGui::CalcTextSize(">").x, at.y + scale), hovered ? kAccent : kText, ">");
  return ImGui::IsItemClicked();
}

float menuWidth(std::initializer_list<std::pair<const char*, const char*>> rows) {
  float widest = 0;
  for (const auto& [label, shortcut] : rows) widest = std::max(widest, ImGui::CalcTextSize(label).x + (shortcut && *shortcut ? 24 * ImGui::GetStyle().FontScaleDpi + ImGui::CalcTextSize(shortcut).x : 0.0f));
  return widest + 6 * ImGui::GetStyle().FontScaleDpi;
}

bool checkbox(const char* label, bool* value) {
  const ImGuiStyle& style = ImGui::GetStyle();
  const float full = ImGui::GetFrameHeight(), side = full * kTickScale, lift = (full - side) / 2;
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + lift);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, std::max(0.0f, (side - ImGui::GetFontSize()) / 2)));
  const bool changed = ImGui::Checkbox(label, value);
  ImGui::PopStyleVar();
  return changed;   // (the row is a little shorter than a full-size tick box's; moving the cursor on instead would need an item to grow the window)
}

int spinBox(const char* id, const char* text, float width) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const float h = ImGui::GetFrameHeight(), arrowW = 14 * scale;
  ImGui::PushID(id);
  const ImVec2 min = ImGui::GetCursorScreenPos(), max(min.x + width, min.y + h);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 split(max.x - arrowW, (min.y + max.y) / 2);
  int step = 0;
  ImGui::InvisibleButton("box", ImVec2(width - arrowW, h));
  if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0) step = ImGui::GetIO().MouseWheel > 0 ? 1 : -1;
  ImGui::SameLine(0, 0);
  ImGui::InvisibleButton("up", ImVec2(arrowW, h / 2));
  const bool upHot = ImGui::IsItemHovered(), upDown = ImGui::IsItemActive();
  if (ImGui::IsItemClicked()) step = 1;
  ImGui::SetCursorScreenPos(ImVec2(split.x, split.y));
  ImGui::InvisibleButton("down", ImVec2(arrowW, h / 2));
  const bool downHot = ImGui::IsItemHovered(), downDown = ImGui::IsItemActive();
  if (ImGui::IsItemClicked()) step = -1;
  const float r = 3 * scale;
  dl->AddRectFilled(min, max, kField, r);
  dl->AddRectFilled(ImVec2(split.x, min.y), ImVec2(max.x, split.y), upDown ? kSoft : upHot ? lineAlpha(120) : lineAlpha(60), r, ImDrawFlags_RoundCornersTopRight);
  dl->AddRectFilled(split, max, downDown ? kSoft : downHot ? lineAlpha(120) : lineAlpha(60), r, ImDrawFlags_RoundCornersBottomRight);
  const float cx = split.x + arrowW / 2, a = 2.6f * scale;
  const float uy = (min.y + split.y) / 2, dy = (split.y + max.y) / 2;
  dl->AddTriangleFilled(ImVec2(cx - a, uy + a * 0.6f), ImVec2(cx + a, uy + a * 0.6f), ImVec2(cx, uy - a * 0.8f), kText);
  dl->AddTriangleFilled(ImVec2(cx - a, dy - a * 0.6f), ImVec2(cx + a, dy - a * 0.6f), ImVec2(cx, dy + a * 0.8f), kText);
  const ImVec2 size = ImGui::CalcTextSize(text);
  dl->AddText(ImVec2(max.x - arrowW - 6 * scale - size.x, min.y + (h - size.y) / 2), kText, text);   // the value, right-aligned
  ImGui::PopID();
  ImGui::SetCursorScreenPos(ImVec2(max.x, min.y));
  ImGui::Dummy(ImVec2(0, h));
  return step;
}

void addAreaFilled(ImDrawList* dl, const ImVec2* points, int count, ImU32 colour) {
  const ImDrawListFlags flags = dl->Flags;
  dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
  dl->AddConcavePolyFilled(points, count, colour);
  dl->Flags = flags;
}

}  // namespace wax::app

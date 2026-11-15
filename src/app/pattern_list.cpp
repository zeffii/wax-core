#include "app/pattern_list.h"

#include <algorithm>
#include <cstdio>

#include "imgui.h"

#include "app/pattern_colour.h"
#include "app/theme.h"
#include "app/widgets.h"

namespace wax::app {

namespace {

const Pattern* find(const Lane* lane, const std::string& id) {
  if (lane) for (const Pattern& p : lane->patterns) if (p.id == id) return &p;
  return nullptr;
}

int placements(const Lane& lane, const std::string& id) {
  int n = 0;
  for (const Placement& p : lane.placements) n += p.pattern == id;
  return n;
}

}  // namespace

PatternListResult drawPatternList(PatternListState& state, const Lane* lane, float height) {
  PatternListResult result;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  // the grip: a thin bar at the list's left edge, dragged to change the width
  {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("grip", ImVec2(5 * scale, height));
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) state.width = std::clamp(state.width - ImGui::GetIO().MouseDelta.x / scale, 110.0f, 420.0f);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x + 2 * scale, at.y), ImVec2(at.x + 2 * scale, at.y + height), hot ? kAccent : lineAlpha(90), 1.0f);
    ImGui::SameLine(0, 4 * scale);
  }
  ImGui::BeginGroup();
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kText), "Patterns%s%s", lane ? "  " : "", lane ? lane->id.c_str() : "");
  if (!lane) { ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "no lane"); ImGui::EndGroup(); return result; }
  if (!find(lane, state.picked) && !lane->patterns.empty()) state.picked = lane->patterns.front().id;
  if (textButton("New")) result.create = true;
  ImGui::SameLine(0, 10 * scale);
  if (textButton("Place") && !state.picked.empty()) result.place = state.picked;
  ImGui::SameLine(0, 10 * scale);
  if (textButton("Clone") && !state.picked.empty()) result.clone = true;
  if (const Pattern* picked = find(lane, state.picked)) {   // the outlined pattern's name, editable
    if (state.nameFor != picked->id) { std::snprintf(state.name, sizeof state.name, "%s", picked->name.c_str()); state.nameFor = picked->id; }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "colour");
    ImGui::SameLine(0, 6 * scale);
    if (textButton("<")) result.colorStep = -1;
    ImGui::SameLine(0, 4 * scale);
    {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float side = ImGui::GetTextLineHeight();
      ImGui::Dummy(ImVec2(side * 2, side));
      ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + side * 2, at.y + side), patternColour(*picked, 200), 2.0f);
    }
    ImGui::SameLine(0, 4 * scale);
    if (textButton(">")) result.colorStep = 1;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##patname", state.name, sizeof state.name, ImGuiInputTextFlags_EnterReturnsTrue) && state.name[0] && picked->name != state.name) result.rename = state.name;
  }
  ImGui::BeginChild("patlist", ImVec2(0, 0), false, ImGuiWindowFlags_NoNav);
  for (const Pattern& p : lane->patterns) {
    char text[96];
    std::snprintf(text, sizeof text, "%02X %-10s %3d  x%d", p.index, p.name.c_str(), static_cast<int>(p.rows.size()), placements(*lane, p.id));
    const bool picked = p.id == state.picked;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(picked ? kAccent : kDimText));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable(text, false, ImGuiSelectableFlags_AllowDoubleClick)) {
      state.picked = p.id;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) result.place = p.id;
    }
    ImGui::PopStyleColor();
    if (picked) ImGui::GetWindowDrawList()->AddRect(at, ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + ImGui::GetTextLineHeight() + 2), accentAlpha(160), 2.0f);
  }
  ImGui::EndChild();
  ImGui::EndGroup();
  return result;
}

void stepPatternList(PatternListState& state, const Lane* lane, int step) {
  if (!lane || lane->patterns.empty()) return;
  int at = 0;
  for (std::size_t i = 0; i < lane->patterns.size(); i++) if (lane->patterns[i].id == state.picked) at = static_cast<int>(i);
  at = std::clamp(at + step, 0, static_cast<int>(lane->patterns.size()) - 1);
  state.picked = lane->patterns[static_cast<std::size_t>(at)].id;
}

}  // namespace wax::app

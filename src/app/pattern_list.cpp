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

// The popup under a pattern's colour square: automatic, the 16 presets, and a free picker. The choice goes into `result`.
void drawColourPopup(const Pattern& pattern, PatternListResult& result) {
  if (!ImGui::BeginPopup("##patcolour")) return;
  const float scale = ImGui::GetStyle().FontScaleDpi, side = ImGui::GetTextLineHeight() * 1.2f, gap = 4 * scale;
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "colour");
  ImGui::SameLine(0, 10 * scale);
  if (textButton("automatic")) { result.colourFor = pattern.id; result.colour = Json(); ImGui::CloseCurrentPopup(); }
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const int count = patternPresetCount(), perRow = 8;
  for (int i = 0; i < count; i++) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##preset", ImVec2(side, side))) { result.colourFor = pattern.id; result.colour = Json(i); ImGui::CloseCurrentPopup(); }
    ImGui::PopID();
    draw->AddRectFilled(at, ImVec2(at.x + side, at.y + side), patternPresetColour(i, 255));
    if (pattern.color.is_number_integer() && pattern.color.get<int>() == i) draw->AddRect(ImVec2(at.x - 1, at.y - 1), ImVec2(at.x + side + 1, at.y + side + 1), kAccent, 0, 0, 1.5f);   // the current one
    if (i % perRow != perRow - 1 && i != count - 1) ImGui::SameLine(0, gap);
  }
  ImGui::Spacing();
  const ImU32 now = patternColour(pattern, 255);
  float rgb[3] = {(now & 255) / 255.0f, ((now >> 8) & 255) / 255.0f, ((now >> 16) & 255) / 255.0f};
  if (ImGui::ColorPicker3("##custom", rgb, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha)) {
    result.colourFor = pattern.id;
    result.colour = customPatternColor(rgb[0], rgb[1], rgb[2]);
  }
  ImGui::EndPopup();
}

}  // namespace

PatternListResult drawPatternList(PatternListState& state, const Lane* lane, float /*height: the list now fills what the controls leave*/) {
  PatternListResult result;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::BeginGroup();
  if (!lane) { ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "no lane"); ImGui::EndGroup(); return result; }
  if (!find(lane, state.picked) && !lane->patterns.empty()) state.picked = lane->patterns.front().id;
  const float line = ImGui::GetTextLineHeightWithSpacing();
  const float controlsHeight = line * 2 + 8 * scale;   // New / Place / Clone, then the name: under the list, as in the web
  {   // the heading: the lane, how many patterns, and the length column
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "%s: %d pattern%s", lane->id.c_str(), static_cast<int>(lane->patterns.size()), lane->patterns.size() == 1 ? "" : "s");
    const float lenWidth = ImGui::CalcTextSize("len").x;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - lenWidth - 6 * scale);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "len");
    ImGui::PopFont();
  }
  ImGui::BeginChild("patlist", ImVec2(0, std::max(line * 2, ImGui::GetContentRegionAvail().y - controlsHeight)), false, ImGuiWindowFlags_NoNav);
  for (const Pattern& p : lane->patterns) {
    const bool picked = p.id == state.picked;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, side = ImGui::GetTextLineHeight() * 0.7f;
    ImGui::PushID(p.id.c_str());
    ImGui::SetNextItemAllowOverlap();   // the colour square sits on top of the row
    if (ImGui::Selectable("##row", picked, ImGuiSelectableFlags_AllowDoubleClick)) {
      state.picked = p.id;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) result.place = p.id;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float top = at.y + (ImGui::GetTextLineHeight() - side) * 0.5f;
    draw->AddRectFilled(ImVec2(at.x + 2 * scale, top), ImVec2(at.x + 2 * scale + side, top + side), patternColour(p, 220));   // the pattern's colour
    {   // a click on the square sets the colour by hand
      ImGui::SetCursorScreenPos(ImVec2(at.x, at.y));
      ImGui::SetNextItemAllowOverlap();
      if (ImGui::InvisibleButton("##colour", ImVec2(side + 4 * scale, ImGui::GetTextLineHeight()))) {
        state.picked = p.id;
        ImGui::OpenPopup("##patcolour");
      }
      ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + ImGui::GetTextLineHeightWithSpacing()));   // on to the next row
      ImGui::Dummy(ImVec2(0, 0));   // (a moved cursor needs an item after it)
    }
    drawColourPopup(p, result);
    ImGui::PopID();
    char text[64], length[16];
    std::snprintf(text, sizeof text, "%02X %s", p.index, p.name.c_str());
    std::snprintf(length, sizeof length, "%d", static_cast<int>(p.rows.size()));
    const ImU32 ink = picked ? kText : kDimText;
    draw->AddText(ImVec2(at.x + side + 8 * scale, at.y), ink, text);
    draw->AddText(ImVec2(at.x + width - ImGui::CalcTextSize(length).x - 6 * scale, at.y), ink, length);
  }
  ImGui::EndChild();
  if (textButton("New")) result.create = true;
  ImGui::SameLine(0, 10 * scale);
  if (textButton("Place") && !state.picked.empty()) result.place = state.picked;
  ImGui::SameLine(0, 10 * scale);
  if (textButton("Clone") && !state.picked.empty()) result.clone = true;
  if (const Pattern* picked = find(lane, state.picked)) {   // the outlined pattern's name, editable
    if (state.nameFor != picked->id) { std::snprintf(state.name, sizeof state.name, "%s", picked->name.c_str()); state.nameFor = picked->id; }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##patname", state.name, sizeof state.name, ImGuiInputTextFlags_EnterReturnsTrue) && state.name[0] && picked->name != state.name) result.rename = state.name;
  }
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

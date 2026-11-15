#include "app/arrangement_view.h"

#include <algorithm>

#include "app/theme.h"

namespace wax::app {

namespace {

constexpr float kLabelWidth = 110.0f;

std::vector<const Lane*> shownLanes(const Project& project) {
  std::vector<const Lane*> out;
  for (const Lane& lane : project.lanes) if (lane.id != kMaster) out.push_back(&lane);
  return out;
}

}  // namespace

float arrangementHeight(const Project& project) {
  const float row = ImGui::GetTextLineHeight() + 4;
  return std::min<float>(static_cast<float>(shownLanes(project).size()), 8.0f) * row + 4;
}

ArrangementResult drawArrangement(const Project& project, const std::string& selectedLane, int playTick, int cursorTick) {
  ArrangementResult result;
  const std::vector<const Lane*> lanes = shownLanes(project);
  const int length = std::max(1, project.song.length);
  const float row = ImGui::GetTextLineHeight() + 4;

  ImGui::BeginChild("arrangement", ImVec2(0, arrangementHeight(project)), false, ImGuiWindowFlags_NoNav);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float barsLeft = origin.x + kLabelWidth;
  const float barsWidth = std::max(40.0f, ImGui::GetContentRegionAvail().x - kLabelWidth - 6);
  const float perTick = barsWidth / static_cast<float>(length);

  for (std::size_t i = 0; i < lanes.size(); ++i) {
    const Lane& lane = *lanes[i];
    const float top = origin.y + static_cast<float>(i) * row;
    const bool selected = lane.id == selectedLane;
    // a hair line under the row, the name, then the bars
    draw->AddLine(ImVec2(origin.x, top + row - 1), ImVec2(barsLeft + barsWidth, top + row - 1), kBeatRow);
    draw->AddText(ImVec2(origin.x, top + 2), selected ? kAccent : kDimText, lane.id.c_str());
    for (const Placement& placement : lane.placements) {
      const float x0 = barsLeft + perTick * static_cast<float>(placement.start);
      const float x1 = barsLeft + perTick * static_cast<float>(placement.start + placement.length);
      const ImU32 fill = selected ? IM_COL32(64, 190, 168, 120) : IM_COL32(120, 134, 146, 90);
      draw->AddRectFilled(ImVec2(x0, top + 3), ImVec2(std::max(x1 - 1, x0 + 1), top + row - 3), fill);
    }
  }
  if (playTick >= 0) {
    const float x = barsLeft + perTick * static_cast<float>(playTick);
    draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + row * static_cast<float>(lanes.size())), kAccent);
  }
  if (cursorTick >= 0) {
    const float x = barsLeft + perTick * static_cast<float>(cursorTick);
    draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + row * static_cast<float>(lanes.size())), kWarm);
  }

  // one click target over the whole thing; the position says which lane and tick
  ImGui::InvisibleButton("##arr", ImVec2(kLabelWidth + barsWidth, row * static_cast<float>(lanes.size())));
  if (ImGui::IsItemClicked() && !lanes.empty()) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const int index = std::clamp(static_cast<int>((mouse.y - origin.y) / row), 0, static_cast<int>(lanes.size()) - 1);
    result.lane = lanes[static_cast<std::size_t>(index)]->id;
    result.tick = std::clamp(static_cast<int>((mouse.x - barsLeft) / perTick), 0, length - 1);
  }
  ImGui::EndChild();
  return result;
}

}  // namespace wax::app

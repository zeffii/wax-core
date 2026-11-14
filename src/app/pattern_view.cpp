#include "app/pattern_view.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "app/theme.h"
#include "seq/lane.h"
#include "imgui.h"

namespace wax::app {

std::vector<std::vector<std::string>> flattenLane(const Lane& lane, int songLength) {
  std::vector<std::vector<std::string>> rows(static_cast<std::size_t>(std::max(0, songLength)));
  const std::vector<RowOwner> owners = rowOwners(lane, songLength);
  for (std::size_t tick = 0; tick < owners.size(); tick++) {
    if (owners[tick].pattern < 0) continue;
    std::istringstream words(lane.patterns[static_cast<std::size_t>(owners[tick].pattern)].rows[static_cast<std::size_t>(owners[tick].row)]);
    for (std::string cell; words >> cell;) rows[tick].push_back(cell);
  }
  return rows;
}

PatternViewResult drawPatternView(const std::vector<std::vector<std::string>>& rows, const std::vector<std::string>& labels,
                                  int ticksPerBeat, int beatsPerBar, int playTick, bool follow, const CursorView& cursor) {
  PatternViewResult result;
  const int columns = static_cast<int>(labels.size());
  const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_BordersInnerV |
                                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
  if (!ImGui::BeginTable("pattern", columns + 1, flags)) return result;
  ImGui::TableSetupScrollFreeze(1, 1);
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
  for (const std::string& label : labels) ImGui::TableSetupColumn(label.c_str());
  ImGui::TableHeadersRow();

  const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
  if (follow && playTick >= 0) {
    const float visible = ImGui::GetWindowHeight() - rowHeight * 2;
    ImGui::SetScrollY(std::max(0.0f, playTick * rowHeight - visible * 0.4f));
  }

  const int beat = std::max(1, ticksPerBeat), bar = beat * std::max(1, beatsPerBar);
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(rows.size()));
  if (cursor.show && cursor.scrollTo && cursor.tick >= 0) clipper.IncludeItemByIndex(cursor.tick);
  while (clipper.Step()) {
    for (int tick = clipper.DisplayStart; tick < clipper.DisplayEnd; tick++) {
      ImGui::TableNextRow();
      const ImU32 tint = tick == playTick ? kPlayRow : tick % bar == 0 ? kBarRow : tick % beat == 0 ? kBeatRow : 0;
      if (tint) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, tint);
      ImGui::TableSetColumnIndex(0);
      char number[16];
      std::snprintf(number, sizeof number, "%03d", tick);
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tick % beat == 0 ? kWarm : kDimText));
      if (ImGui::Selectable(number, false, ImGuiSelectableFlags_None)) result.clickedTick = tick;
      ImGui::PopStyleColor();
      const std::vector<std::string>& cells = rows[static_cast<std::size_t>(tick)];
      for (int c = 0; c < columns; c++) {
        ImGui::TableSetColumnIndex(c + 1);
        const bool here = cursor.show && tick == cursor.tick && c == cursor.cell;
        if (here) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, kCursorCell);
          if (cursor.scrollTo) ImGui::SetScrollHereY(0.5f);
        }
        const bool filled = c < static_cast<int>(cells.size());
        ImGui::PushID(tick * 4096 + c);
        if (ImGui::Selectable(filled ? cells[static_cast<std::size_t>(c)].c_str() : "   ", false, ImGuiSelectableFlags_None)) { result.cellTick = tick; result.cell = c; }
        ImGui::PopID();
      }
    }
  }
  ImGui::EndTable();
  return result;
}

}  // namespace wax::app

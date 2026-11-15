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
    ImGui::SetScrollY(std::max(0.0f, (static_cast<float>(playTick) + cursor.playFrac) * rowHeight - visible * 0.4f));
  }

  const int beat = std::max(1, ticksPerBeat), bar = beat * std::max(1, beatsPerBar);
  ImGuiListClipper clipper;
  float playY = -1;   // screen y of the playing row's top, when it was drawn
  clipper.Begin(static_cast<int>(rows.size()));
  if (cursor.show && cursor.scrollTo && cursor.tick >= 0) clipper.IncludeItemByIndex(cursor.tick);
  while (clipper.Step()) {
    for (int tick = clipper.DisplayStart; tick < clipper.DisplayEnd; tick++) {
      ImGui::TableNextRow();
      const ImU32 tint = tick == playTick ? kPlayRow : tick % bar == 0 ? kBarRow : tick % beat == 0 ? kBeatRow : 0;
      if (tint) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, tint);
      ImGui::TableSetColumnIndex(0);
      if (tick == playTick) playY = ImGui::GetCursorScreenPos().y - 1;
      if (cursor.loop && tick >= cursor.loopStart && tick < cursor.loopEnd) {   // the loop: a bar down the row-number column, ends marked
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float rowH = ImGui::GetTextLineHeightWithSpacing();
        const ImU32 mark = cursor.loopOn ? kAccent : lineAlpha(140);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(at.x - 3, at.y - 1), ImVec2(at.x - 1, at.y + rowH - 1), mark);
        if (tick == cursor.loopStart) dl->AddRectFilled(ImVec2(at.x - 3, at.y - 1), ImVec2(at.x + 8, at.y + 1), mark);
        if (tick == cursor.loopEnd - 1) dl->AddRectFilled(ImVec2(at.x - 3, at.y + rowH - 3), ImVec2(at.x + 8, at.y + rowH - 1), mark);
      }
      char number[16];
      std::snprintf(number, sizeof number, "%03d", tick);
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tick % beat == 0 ? kWarm : kDimText));
      if (ImGui::Selectable(number, false, ImGuiSelectableFlags_None)) result.clickedTick = tick;
      ImGui::PopStyleColor();
      const std::vector<std::string>& cells = rows[static_cast<std::size_t>(tick)];
      for (int c = 0; c < columns; c++) {
        ImGui::TableSetColumnIndex(c + 1);
        const bool here = cursor.show && tick == cursor.tick && c == cursor.cell;
        if (cursor.show && cursor.selected && tick >= cursor.tick0 && tick <= cursor.tick1 && c >= cursor.cell0 && c <= cursor.cell1)
          ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, accentAlpha(55));
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
  if (playY >= 0) {   // the playhead: a thin line across the table, moving down through the row as the song plays
    const float y = playY + cursor.playFrac * rowHeight;
    const ImVec2 from = ImGui::GetWindowPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float headerBottom = from.y + rowHeight * 1.5f;   // keep the line out from under the column headings
    draw->PushClipRect(ImVec2(from.x, headerBottom), ImVec2(from.x + ImGui::GetWindowWidth(), from.y + ImGui::GetWindowHeight()), false);
    draw->AddLine(ImVec2(from.x, y), ImVec2(from.x + ImGui::GetWindowWidth(), y), kAccent, 1.5f);
    draw->PopClipRect();
  }
  ImGui::EndTable();
  return result;
}

}  // namespace wax::app

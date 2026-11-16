#include "app/pattern_view.h"
#include "app/config.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "app/theme.h"
#include "seq/lane.h"
#include "imgui.h"
#include "imgui_internal.h"

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

// A cell with nothing in it: only dots (and blanks).
static bool cellIsEmpty(const std::string& cell) {
  for (const char ch : cell) if (ch != '.' && ch != ' ') return false;
  return true;
}

// Whether column `c` of the lane has anything in it at any tick.
static bool columnUsed(const std::vector<std::vector<std::string>>& rows, int c) {
  for (const std::vector<std::string>& cells : rows)
    if (c < static_cast<int>(cells.size()) && !cellIsEmpty(cells[static_cast<std::size_t>(c)])) return true;
  return false;
}

static PatternViewResult drawPatternGrid(const std::vector<PatternLane>& lanes, int ticksPerBeat, int beatsPerBar, int playTick, bool follow, const CursorView& cursor) {
  PatternViewResult result;
  if (lanes.empty() || !lanes.front().rows || !lanes.front().labels) return result;
  const std::vector<std::vector<std::string>>& rows = *lanes.front().rows;   // (every lane has the song's rows)
  std::vector<int> firstColumn;   // where each lane's columns start (after the row number column)
  int columns = 0;
  for (const PatternLane& lane : lanes) { firstColumn.push_back(columns + 1); columns += static_cast<int>(lane.labels->size()); }
  const bool several = lanes.size() > 1;
  const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
  const ImGuiTableFlags tableFlags = cursor.hideEmpty ? flags | ImGuiTableFlags_Hideable : flags;   // (hiding columns needs the flag)
  const float infoHeight = ImGui::GetTextLineHeightWithSpacing();   // room for the line under the grid
  // The gap between two columns is one character: half of it as padding on each side.
  const float charWidth = ImGui::CalcTextSize("0").x;
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(charWidth / 2, ImGui::GetStyle().CellPadding.y));
  const bool began = ImGui::BeginTable("pattern", columns + 1, tableFlags, ImVec2(0, std::max(60.0f, ImGui::GetContentRegionAvail().y - infoHeight)));
  ImGui::PopStyleVar();
  if (!began) return result;
  // no highlight under the mouse: the cursor cell is the only marked cell, the pointer needs no shadow
  for (ImGuiCol colour : {ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive}) ImGui::PushStyleColor(colour, ImVec4(0, 0, 0, 0));
  ImGui::TableSetupScrollFreeze(1, several ? 2 : 1);
  // Every column has the width of its widest text (the heading or any cell of any row), worked out from all the rows: left to the table, a
  // column would be as wide as the rows on show, and widen or narrow as the grid scrolls.
  const auto letters = [](const std::string& text) { int n = 0; for (const unsigned char ch : text) if ((ch & 0xC0) != 0x80) n++; return n; };
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 3 * charWidth);
  for (const PatternLane& lane : lanes) {
    for (std::size_t c = 0; c < lane.labels->size(); c++) {
      int widest = letters((*lane.labels)[c]);
      for (const std::vector<std::string>& cells : *lane.rows) if (c < cells.size()) widest = std::max(widest, letters(cells[c]));
      ImGui::TableSetupColumn((*lane.labels)[c].c_str(), ImGuiTableColumnFlags_WidthFixed, static_cast<float>(widest) * charWidth);
    }
  }
  if (cursor.hideEmpty) {
    for (std::size_t li = 0; li < lanes.size(); li++)
      for (int c = 0; c < static_cast<int>(lanes[li].labels->size()); c++) {
        const bool keep = (static_cast<int>(li) == cursor.lane && c == cursor.cell) || columnUsed(*lanes[li].rows, c);
        ImGui::TableSetColumnEnabled(firstColumn[li] + c, keep);
      }
  }
  if (several) {   // the lanes' names over their columns. Drawn on top of the cells, not as their text: a name wider than its first column would stretch that column
    ImGui::TableNextRow();
    ImGuiTable* table = ImGui::GetCurrentTable();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float frozenEdge = ImGui::TableGetCellBgRect(table, 0).Max.x;   // (a name sliding left stops at the row numbers)
    for (std::size_t li = 0; li < lanes.size(); li++) {
      ImGui::TableSetColumnIndex(firstColumn[li]);
      const ImVec2 at = ImGui::GetCursorScreenPos();
      float right = at.x;   // the lane's last shown column ends where its name must stop
      for (int c = 0; c < static_cast<int>(lanes[li].labels->size()); c++)
        if (ImGui::TableGetColumnFlags(firstColumn[li] + c) & ImGuiTableColumnFlags_IsEnabled) right = std::max(right, ImGui::TableGetCellBgRect(table, firstColumn[li] + c).Max.x);
      ImDrawList* draw = ImGui::GetWindowDrawList();
      draw->PushClipRect(ImVec2(frozenEdge, at.y), ImVec2(std::min(right, window->InnerClipRect.Max.x), at.y + ImGui::GetTextLineHeight()), false);
      draw->AddText(at, static_cast<int>(li) == cursor.lane ? kAccent : kDimText, lanes[li].id.c_str());
      draw->PopClipRect();
      ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
    }
  }
  ImGui::TableHeadersRow();

  const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
  // How far apart the rows really are on screen (a table row is its text plus the cell padding, which is not what the line height says):
  // measured from the rows drawn last frame, because the follow scroll and the playhead move by whole rows and drift if the pitch is wrong.
  static float measuredPitch = 0;
  const float rowPitch = measuredPitch > 1 ? measuredPitch : rowHeight;
  if (follow && playTick >= 0) {
    const float visible = ImGui::GetWindowHeight() - rowHeight * 2;
    ImGui::SetScrollY(std::max(0.0f, (static_cast<float>(playTick) + cursor.playFrac) * rowPitch - visible * 0.4f));
  }

  const int beat = std::max(1, ticksPerBeat), bar = beat * std::max(1, beatsPerBar);
  ImGuiListClipper clipper;
  float playY = -1;   // screen y of the playing row's top, when it was drawn
  int firstTick = -1, lastTick = -1; float firstY = 0, lastY = 0;   // the first and last rows drawn, to measure the pitch
  clipper.Begin(static_cast<int>(rows.size()));
  if (cursor.show && cursor.scrollTo && cursor.tick >= 0) clipper.IncludeItemByIndex(cursor.tick);
  while (clipper.Step()) {
    for (int tick = clipper.DisplayStart; tick < clipper.DisplayEnd; tick++) {
      ImGui::TableNextRow();
      const ImU32 tint = tick % bar == 0 ? kBarRow : tick % beat == 0 ? kBeatRow : 0;
      if (tint) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, tint);
      ImGui::TableSetColumnIndex(0);
      if (tick == playTick) playY = ImGui::GetCursorScreenPos().y - 1;
      if (firstTick < 0) { firstTick = tick; firstY = ImGui::GetCursorScreenPos().y; }
      lastTick = tick; lastY = ImGui::GetCursorScreenPos().y;
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
      for (std::size_t li = 0; li < lanes.size(); li++) {
        const PatternLane& lane = lanes[li];
        const std::vector<std::string>& cells = (*lane.rows)[static_cast<std::size_t>(tick)];
        const int laneColumns = static_cast<int>(lane.labels->size());
        const ImU32 patternTint = static_cast<std::size_t>(tick) < lane.tint.size() ? lane.tint[static_cast<std::size_t>(tick)] : 0;
        const unsigned char marks = static_cast<std::size_t>(tick) < lane.marks.size() ? lane.marks[static_cast<std::size_t>(tick)] : 0;
        const bool cursorLane = static_cast<int>(li) == cursor.lane;
        bool laneStarted = false;
        for (int c = 0; c < laneColumns; c++) {
          if (!ImGui::TableSetColumnIndex(firstColumn[li] + c)) continue;   // (a column left out)
          if (!laneStarted) {   // a thin line between lanes, at the left of the lane's first column (none between the columns of a lane: only a meta machine's own spacer column marks a gap)
            laneStarted = true;
            if (li > 0) {
              const ImVec2 at = ImGui::GetCursorScreenPos();
              const ImGuiStyle& style = ImGui::GetStyle();
              ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x - charWidth / 2, at.y - style.CellPadding.y), ImVec2(at.x - charWidth / 2, at.y + rowHeight - style.CellPadding.y), lineAlpha(160));
            }
          }
          if (patternTint) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, patternTint);
          const bool here = cursor.show && cursorLane && tick == cursor.tick && c == cursor.cell;
          if (cursor.show && cursorLane && cursor.selected && tick >= cursor.tick0 && tick <= cursor.tick1 && c >= cursor.cell0 && c <= cursor.cell1)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, accentAlpha(55));
          if (here) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, kCursorCell);
            if (cursor.scrollTo) ImGui::SetScrollHereY(0.5f);
          }
          const bool filled = c < static_cast<int>(cells.size());
          if (marks & (PatternLane::kStart | PatternLane::kEnd)) {   // a thin line where a pattern starts (above its first row) and ends (under its last)
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float wide = ImGui::GetContentRegionAvail().x + charWidth, y0 = at.y - ImGui::GetStyle().CellPadding.y;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (marks & PatternLane::kStart) dl->AddLine(ImVec2(at.x - charWidth / 2, y0), ImVec2(at.x - charWidth / 2 + wide, y0), dimAlpha(110));
            if (marks & PatternLane::kEnd) dl->AddLine(ImVec2(at.x - charWidth / 2, y0 + rowHeight), ImVec2(at.x - charWidth / 2 + wide, y0 + rowHeight), dimAlpha(110));
          }
          const bool dimmed = (marks & PatternLane::kDim) != 0;
          if (dimmed) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(withAlpha(kText, 105)));   // the other patterns step back
          ImGui::PushID(tick * 4096 + firstColumn[li] + c);
          if (ImGui::Selectable(filled ? cells[static_cast<std::size_t>(c)].c_str() : "   ", false, ImGuiSelectableFlags_None)) { result.cellTick = tick; result.cell = c; result.lane = static_cast<int>(li); }
          ImGui::PopID();
          if (dimmed) ImGui::PopStyleColor();
        }
      }
    }
  }
  if (lastTick > firstTick) measuredPitch = (lastY - firstY) / static_cast<float>(lastTick - firstTick);
  if (playY >= 0) {   // the playhead: a thin red line across the table, moving down through the row as the song plays
    const float y = playY + cursor.playFrac * rowPitch;
    const ImVec2 from = ImGui::GetWindowPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float headerBottom = from.y + rowHeight * 1.5f;   // keep the line out from under the column headings
    draw->PushClipRect(ImVec2(from.x, headerBottom), ImVec2(from.x + ImGui::GetWindowWidth(), from.y + ImGui::GetWindowHeight()), false);
    draw->AddLine(ImVec2(from.x, y), ImVec2(from.x + ImGui::GetWindowWidth(), y), IM_COL32(230, 60, 60, 255), 1.0f);
    draw->PopClipRect();
  }
  ImGui::PopStyleColor(3);
  ImGui::EndTable();
  if (!cursor.info.empty()) {   // the line under the grid: a small face, so a long description fits
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "%s", cursor.info.c_str());
    ImGui::PopFont();
  }
  return result;
}

// Ctrl + mouse wheel over the pattern makes its text bigger or smaller: the "Text size" setting of the Pattern view (kept in the preferences).
PatternViewResult drawPatternView(const std::vector<PatternLane>& lanes, int ticksPerBeat, int beatsPerBar, int playTick, bool follow, const CursorView& cursor) {
  ImGuiIO& io = ImGui::GetIO();
  if (io.KeyCtrl && io.MouseWheel != 0 && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
    float& size = settings().patternText;
    size = std::min(2.0f, std::max(0.6f, size * (io.MouseWheel > 0 ? 1.1f : 1.0f / 1.1f)));
    saveSettings();
    io.MouseWheel = 0;   // (the grid does not scroll as well)
  }
  return drawPatternGrid(lanes, ticksPerBeat, beatsPerBar, playTick, follow, cursor);
}

}  // namespace wax::app

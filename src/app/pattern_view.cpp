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
  constexpr float kTickColumnChars = 5;
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(charWidth / 2, ImGui::GetStyle().CellPadding.y));
  // One table per set of lanes: ImGui keeps a column's width by its index, so one table id for every layout (a lane on its own in solo-edit,
  // then all lanes again) would hand the old widths to the wrong columns.
  std::string tableId = "pattern";
  for (const PatternLane& lane : lanes) tableId += "/" + lane.id;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + charWidth);   // (the grid starts a character's width clear of the browser's drag handle)
  const bool began = ImGui::BeginTable(tableId.c_str(), columns + 1, tableFlags, ImVec2(0, std::max(60.0f, ImGui::GetContentRegionAvail().y - infoHeight)));
  ImGui::PopStyleVar();
  if (!began) return result;
  // no highlight under the mouse: the cursor cell is the only marked cell, the pointer needs no shadow
  for (ImGuiCol colour : {ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive}) ImGui::PushStyleColor(colour, ImVec4(0, 0, 0, 0));
  ImGui::TableSetupScrollFreeze(1, several ? 2 : 1);
  // Every column has the width of its widest text (the heading or any cell of any row), worked out from all the rows: left to the table, a
  // column would be as wide as the rows on show, and widen or narrow as the grid scrolls.
  const auto letters = [](const std::string& text) { int n = 0; for (const unsigned char ch : text) if ((ch & 0xC0) != 0x80) n++; return n; };
  ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, kTickColumnChars * charWidth);   // (room for five characters; the numbers are right-aligned)
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
  const int firstRow = std::max(0, cursor.rowFirst), endRow = cursor.rowEnd < 0 ? static_cast<int>(rows.size()) : std::min(static_cast<int>(rows.size()), cursor.rowEnd);
  float followScroll = -1;   // where the follow scroll is heading, once the playhead has reached its resting place (otherwise -1: the playhead moves)
  if (follow && playTick >= firstRow && playTick < endRow) {
    const float visible = ImGui::GetWindowHeight() - rowHeight * 2;
    const float wanted = (static_cast<float>(playTick - firstRow) + cursor.playFrac) * rowPitch - visible * 0.4f;
    ImGui::SetScrollY(std::max(0.0f, wanted));
    if (wanted > 0 && wanted < ImGui::GetScrollMaxY()) followScroll = wanted;
  }

  const int beat = std::max(1, ticksPerBeat), bar = beat * std::max(1, beatsPerBar);
  ImGuiListClipper clipper;
  float playY = -1;   // screen y of the playing row's top, when it was drawn
  float loopTopY = -1, loopBottomY = -1;   // screen y of the loop's first row's top and last row's bottom, when drawn
  const ImU32 loopYellow = IM_COL32(240, 200, 60, 255);
  int firstTick = -1, lastTick = -1; float firstY = 0, lastY = 0;   // the first and last rows drawn, to measure the pitch
  clipper.Begin(std::max(0, endRow - firstRow));
  if (cursor.show && cursor.scrollTo && cursor.tick >= firstRow && cursor.tick < endRow) clipper.IncludeItemByIndex(cursor.tick - firstRow);
  while (clipper.Step()) {
    for (int tick = firstRow + clipper.DisplayStart; tick < firstRow + clipper.DisplayEnd; tick++) {
      const int shown = tick - firstRow;   // the number the row is shown with
      ImGui::TableNextRow();
      const ImU32 tint = shown % bar == 0 ? kBarRow : shown % beat == 0 ? kBeatRow : 0;
      if (tint) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, tint);
      ImGui::TableSetColumnIndex(0);
      if (tick == playTick) playY = ImGui::GetCursorScreenPos().y - 1;
      if (firstTick < 0) { firstTick = tick; firstY = ImGui::GetCursorScreenPos().y; }
      lastTick = tick; lastY = ImGui::GetCursorScreenPos().y;
      if (cursor.loop && tick >= cursor.loopStart && tick < cursor.loopEnd) {   // the loop: a bar down the row-number column, ends marked
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float rowH = rowPitch;   // (the rows' real distance, so the end lands on the next row's top and not short or past it)
        const ImU32 mark = cursor.loopOn ? loopYellow : withAlpha(loopYellow, 110);   // (switched off: the same yellow, faint)
        if (tick == cursor.loopStart) loopTopY = at.y - 1;
        if (tick == cursor.loopEnd - 1) loopBottomY = at.y + rowH - 1;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(at.x - 3, at.y - 1), ImVec2(at.x - 1, at.y + rowH - 1), mark);
        if (tick == cursor.loopStart) dl->AddRectFilled(ImVec2(at.x - 3, at.y - 1), ImVec2(at.x + 8, at.y + 1), mark);
        if (tick == cursor.loopEnd - 1) dl->AddRectFilled(ImVec2(at.x - 3, at.y + rowH - 3), ImVec2(at.x + 8, at.y + rowH - 1), mark);
      }
      char number[16];
      std::snprintf(number, sizeof number, "%03d", shown);
      ImGui::PushID(tick);
      if (ImGui::Selectable("##tick", false, ImGuiSelectableFlags_None)) result.clickedTick = tick;
      ImGui::PopID();
      ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetItemRectMax().x - ImGui::CalcTextSize(number).x - charWidth / 2, ImGui::GetItemRectMin().y), shown % beat == 0 ? kWarm : kDimText, number);   // (right-aligned)
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
          if (here && cursor.character >= 0) {   // the character the cursor is on is marked
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 from(at.x + charWidth * static_cast<float>(cursor.character), at.y - 1), to(at.x + charWidth * static_cast<float>(cursor.character + 1), at.y + ImGui::GetTextLineHeight() + 1);
            ImGui::GetWindowDrawList()->AddRectFilled(from, to, accentAlpha(70));   // a light tint, and a thin outline, one character wide
            ImGui::GetWindowDrawList()->AddRect(from, to, accentAlpha(230), 0.0f, 0, std::max(1.0f, ImGui::GetStyle().FontScaleDpi));
          }
          if (here) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, kCursorCell);
            if (cursor.scrollTo) ImGui::SetScrollHereY(0.5f);
          }
          const bool filled = c < static_cast<int>(cells.size());
          if (marks & PatternLane::kStart) {   // where a pattern starts: a bar across its top, in the pattern's colour
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float wide = ImGui::GetContentRegionAvail().x + charWidth, top = at.y - ImGui::GetStyle().CellPadding.y;
            const ImU32 barColour = static_cast<std::size_t>(tick) < lane.edge.size() && lane.edge[static_cast<std::size_t>(tick)] ? lane.edge[static_cast<std::size_t>(tick)] : dimAlpha(110);
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x - charWidth / 2, top), ImVec2(at.x - charWidth / 2 + wide, top + std::max(2.0f, 2.5f * ImGui::GetStyle().FontScaleDpi)), barColour);
          }
          const bool dimmed = (marks & PatternLane::kDim) != 0;
          if (dimmed) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(withAlpha(kText, 105)));   // the other patterns step back
          if (marks & PatternLane::kLocked) {   // solo-edit: out of reach: faint, hatched, nothing to click
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float w = ImGui::CalcTextSize("0").x * static_cast<float>(std::max<std::size_t>(3, filled ? cells[static_cast<std::size_t>(c)].size() : 3)), h = rowHeight - ImGui::GetStyle().ItemSpacing.y;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(at, ImVec2(at.x + w, at.y + h), true);
            for (float x = -h; x < w; x += 5.0f) dl->AddLine(ImVec2(at.x + x, at.y + h), ImVec2(at.x + x + h, at.y), textAlpha(28));
            dl->PopClipRect();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(textAlpha(55)));
            ImGui::TextUnformatted(filled ? cells[static_cast<std::size_t>(c)].c_str() : "   ");
            ImGui::PopStyleColor();
            if (dimmed) ImGui::PopStyleColor();
            continue;
          }
          ImGui::PushID(tick * 4096 + firstColumn[li] + c);   // (a cell is picked at the press, as the web does: rows that scroll under the mouse cannot lose the click)
          if (ImGui::Selectable(filled ? cells[static_cast<std::size_t>(c)].c_str() : "   ", false, ImGuiSelectableFlags_SelectOnClick)) { result.cellTick = tick; result.cell = c; result.lane = static_cast<int>(li); }
          if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { result.menuTick = tick; result.menuCell = c; result.menuLane = static_cast<int>(li); }
          ImGui::PopID();
          if (dimmed) ImGui::PopStyleColor();
        }
      }
    }
  }
  if (lastTick > firstTick) measuredPitch = (lastY - firstY) / static_cast<float>(lastTick - firstTick);
  {   // a press anywhere on the grid is reported at once: a cell is only picked on release, and while the rows scroll under the mouse the release may miss it
    const ImVec2 corner = ImGui::GetWindowPos(), extent = ImGui::GetWindowSize();
    result.pressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(ImVec2(corner.x, corner.y + rowHeight * 1.5f), ImVec2(corner.x + extent.x, corner.y + extent.y));
  }
  if (playY >= 0) {   // the playhead: a thin red line across the table, moving down through the row as the song plays
    // Where the pattern is moving past it the playhead stands still: it is held at its resting place, not worked out again from the rows
    // each frame (those are drawn where the scroll was a frame ago, and the clock's small jitter showed as the line trembling).
    const float y = playY + cursor.playFrac * rowPitch + (followScroll >= 0 ? ImGui::GetScrollY() - followScroll : 0.0f);
    const ImVec2 from = ImGui::GetWindowPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float headerBottom = from.y + rowHeight * 1.5f;   // keep the line out from under the column headings
    draw->PushClipRect(ImVec2(from.x, headerBottom), ImVec2(from.x + ImGui::GetWindowWidth(), from.y + ImGui::GetWindowHeight()), false);
    draw->AddLine(ImVec2(from.x, y), ImVec2(from.x + ImGui::GetWindowWidth(), y), IM_COL32(230, 60, 60, 255), 1.0f);
    draw->PopClipRect();
  }
  // (the table clips what is drawn inside it to part of its width, and the window to the other part: the lines are drawn in both places)
  if (loopTopY >= 0 || loopBottomY >= 0) {   // the loop's limits: a yellow line across the table at its start and at its end
    const ImVec2 from = ImGui::GetWindowPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float headerBottom = from.y + rowHeight * 1.5f;
    const ImU32 yellow = cursor.loopOn ? loopYellow : withAlpha(loopYellow, 110);
    draw->PushClipRect(ImVec2(from.x, headerBottom), ImVec2(from.x + ImGui::GetWindowWidth(), from.y + ImGui::GetWindowHeight()), false);
    for (const float y : {loopTopY, loopBottomY})
      if (y >= 0) draw->AddLine(ImVec2(from.x, y), ImVec2(from.x + ImGui::GetWindowWidth(), y), yellow, std::max(1.0f, ImGui::GetStyle().FontScaleDpi));
    draw->PopClipRect();
  }
  ImGui::PopStyleColor(3);
  ImGui::EndTable();
  if (loopTopY >= 0 || loopBottomY >= 0) {   // the loop's limits: a yellow line across the table at its start and at its end
    const ImVec2 from = ImGui::GetWindowPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float headerBottom = from.y + rowHeight * 1.5f;
    const ImU32 yellow = cursor.loopOn ? loopYellow : withAlpha(loopYellow, 110);
    draw->PushClipRect(ImVec2(from.x, headerBottom), ImVec2(from.x + ImGui::GetWindowWidth(), from.y + ImGui::GetWindowHeight()), false);
    for (const float y : {loopTopY, loopBottomY})
      if (y >= 0) draw->AddLine(ImVec2(from.x, y), ImVec2(from.x + ImGui::GetWindowWidth(), y), yellow, std::max(1.0f, ImGui::GetStyle().FontScaleDpi));
    draw->PopClipRect();
  }
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

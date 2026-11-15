#include "app/sequence_view.h"
#include "seq/loop.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "app/pattern_colour.h"
#include "app/config.h"
#include "app/theme.h"

namespace wax::app {

namespace {

constexpr float kGutter = 44.0f;       // the tick numbers at the left
constexpr float kLaneWidth = 92.0f;
constexpr float kMasterWidth = 46.0f;  // the master lane has no notes, so it gets half

float laneWidth(const Lane& lane, float scale) { return (lane.id == kMaster ? kMasterWidth : kLaneWidth) * scale; }

// The placement that is heard at a tick: the one that starts latest of those covering it.
const Placement* placementAt(const Lane& lane, int tick) {
  const Placement* best = nullptr;
  for (const Placement& p : lane.placements) {
    if (tick < p.start || tick >= p.start + p.length) continue;
    if (!best || p.start >= best->start) best = &p;
  }
  return best;
}

const Pattern* patternOf(const Lane& lane, const std::string& id) {
  for (const Pattern& pattern : lane.patterns) if (pattern.id == id) return &pattern;
  return nullptr;
}

// The row sizes on offer: the divisors of a bar that are at least a beat, biggest first (16, 8, 4 for 4/4 at 4 ticks a beat).
std::vector<int> rowSizes(const Song& song) {
  const int bar = std::max(1, song.beatsPerBar * song.ticksPerBeat), beat = std::min(bar, std::max(1, song.ticksPerBeat));
  std::vector<int> sizes;
  for (int d = bar; d >= beat; d--) if (bar % d == 0) sizes.push_back(d);
  return sizes;
}

// The row size in use: the one the user zoomed to (Ctrl+wheel), a bar when that size does not fit this song.
int rowTicks(const Song& song) {
  const std::vector<int> sizes = rowSizes(song);
  return std::find(sizes.begin(), sizes.end(), settings().arrangementRow) != sizes.end() ? settings().arrangementRow : sizes.front();
}

}  // namespace

std::string describeSongCursor(const Project& project, const std::string& laneId, int tick) {
  const int cell = rowTicks(project.song);
  const int rows = std::max(1, (project.song.length + cell - 1) / cell);
  const int rowStart = tick / cell * cell;
  const int laneIndex = static_cast<int>(std::find_if(project.lanes.begin(), project.lanes.end(), [&](const Lane& l) { return l.id == laneId; }) - project.lanes.begin());
  if (laneIndex >= static_cast<int>(project.lanes.size())) return std::string();
  const Lane& lane = project.lanes[static_cast<std::size_t>(laneIndex)];
  std::string text = "lane " + std::to_string(laneIndex + 1) + "/" + std::to_string(project.lanes.size()) + " " + lane.id +
                     " \xc2\xb7 row " + std::to_string(tick / cell + 1) + "/" + std::to_string(rows) + " \xc2\xb7 tick " + std::to_string(rowStart) + " \xc2\xb7 ";
  const Placement* shown = nullptr;   // the pattern that starts inside the row, else the one covering it
  for (const Placement& p : lane.placements) if (p.start >= rowStart && p.start < rowStart + cell && (!shown || p.start < shown->start)) shown = &p;
  if (!shown) shown = placementAt(lane, rowStart);
  const Pattern* pattern = shown ? patternOf(lane, shown->pattern) : nullptr;
  if (!pattern) return text + "no pattern";
  int used = 0;
  for (const Placement& p : lane.placements) used += p.pattern == pattern->id;
  char index[8];
  std::snprintf(index, sizeof index, "%02X", pattern->index);
  return text + index + " \"" + pattern->name + "\" used " + std::to_string(used) + "x in this lane";
}

float sequenceWidth(const Project& project) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  float width = kGutter * scale + 14 * scale;   // (room for the scroll bar)
  for (const Lane& lane : project.lanes) width += laneWidth(lane, scale);
  return width;
}

SequenceClick drawSequenceView(const Project& project, const std::string& selectedLane, int playTick, int cursorTick, bool follow) {
  SequenceClick result;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0) {   // Ctrl+wheel: the size of a row, a step a notch
    const std::vector<int> sizes = rowSizes(project.song);
    const int at = static_cast<int>(std::find(sizes.begin(), sizes.end(), rowTicks(project.song)) - sizes.begin());
    const int next = std::clamp(at + (ImGui::GetIO().MouseWheel > 0 ? 1 : -1), 0, static_cast<int>(sizes.size()) - 1);
    if (next != at) { settings().arrangementRow = sizes[static_cast<std::size_t>(next)]; saveSettings(); }
  }
  const int cell = rowTicks(project.song);
  const int rows = std::max(1, (project.song.length + cell - 1) / cell);
  const float rowHeight = ImGui::GetTextLineHeight() + 2 * scale;
  const float headHeight = rowHeight + 4 * scale;
  const float gutter = kGutter * scale;

  ImGui::BeginChild("sequence", ImVec2(0, 0), false, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_HorizontalScrollbar);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float scrollY = ImGui::GetScrollY();
  const float viewHeight = ImGui::GetWindowHeight();

  float totalWidth = gutter;
  for (const Lane& lane : project.lanes) totalWidth += laneWidth(lane, scale);

  if (follow && playTick >= 0) {   // keep the play row in the middle third
    const float y = headHeight + static_cast<float>(playTick / cell) * rowHeight;
    if (y < scrollY + headHeight || y > scrollY + viewHeight - 2 * rowHeight) ImGui::SetScrollY(std::max(0.0f, y - viewHeight * 0.4f));
  }

  // the click target covers the whole sheet
  ImGui::SetCursorScreenPos(origin);
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  ImGui::InvisibleButton("##sheet", ImVec2(totalWidth, headHeight + rowHeight * static_cast<float>(rows)));
  ImGui::PopItemFlag();
  if (ImGui::IsItemClicked()) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float y = mouse.y - origin.y - headHeight;
    float x = mouse.x - origin.x - gutter;
    if (y >= 0) {
      for (const Lane& lane : project.lanes) {
        const float w = laneWidth(lane, scale);
        if (x >= 0 && x < w) {
          result.lane = lane.id;
          result.tick = std::clamp(static_cast<int>(y / rowHeight), 0, rows - 1) * cell;
          break;
        }
        x -= w;
      }
    }
  }

  const int first = std::max(0, static_cast<int>((scrollY - headHeight) / rowHeight));
  const int last = std::min(rows - 1, static_cast<int>((scrollY + viewHeight) / rowHeight));
  const int bar16 = cell * 4;   // a brighter number every four bars

  // the loop: a bar beside the tick numbers over its rows, and a line across the sheet at each end
  const LoopRange loop = loopOf(project.song);
  if (loop.on) {
    const float top = origin.y + headHeight + static_cast<float>(loop.start / cell) * rowHeight;
    const float bottom = origin.y + headHeight + static_cast<float>((loop.end + cell - 1) / cell) * rowHeight;
    draw->AddRectFilled(ImVec2(origin.x, top), ImVec2(origin.x + 3 * scale, bottom), kWarm);
    draw->AddLine(ImVec2(origin.x, top), ImVec2(origin.x + totalWidth, top), withAlpha(kWarm, 110));
    draw->AddLine(ImVec2(origin.x, bottom), ImVec2(origin.x + totalWidth, bottom), withAlpha(kWarm, 110));
  }

  // the gutter: tick numbers, the bright ones every four bars
  for (int r = first; r <= last; ++r) {
    const float y = origin.y + headHeight + static_cast<float>(r) * rowHeight;
    const int t = r * cell;
    char number[16];
    std::snprintf(number, sizeof number, "%04d", t);
    if (playTick >= t && playTick < t + cell) draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + totalWidth, y + rowHeight), kPlayRow);
    draw->AddText(ImVec2(origin.x + 2 * scale, y + scale), t % bar16 == 0 ? kText : kDimText, number);
  }

  // the lanes
  float left = origin.x + gutter;
  for (const Lane& lane : project.lanes) {
    const float w = laneWidth(lane, scale);
    const bool selected = lane.id == selectedLane;
    const ImU32 blockFill = selected ? accentAlpha(70) : dimAlpha(50);
    const ImU32 blockEdge = selected ? kAccent : dimAlpha(140);
    draw->AddLine(ImVec2(left, origin.y + headHeight), ImVec2(left, origin.y + headHeight + rowHeight * static_cast<float>(rows)), lineAlpha(160));
    for (int r = first; r <= last; ++r) {
      const float y = origin.y + headHeight + static_cast<float>(r) * rowHeight;
      const int t0 = r * cell;
      // what the row shows: the pattern that starts inside it, else the one covering its start
      const Placement* shown = nullptr;
      for (const Placement& p : lane.placements) if (p.start >= t0 && p.start < t0 + cell && (!shown || p.start < shown->start)) shown = &p;
      const bool starts = shown != nullptr;
      if (!shown) shown = placementAt(lane, t0);
      if (!shown) continue;
      const Pattern* shownPattern = patternOf(lane, shown->pattern);
      const ImU32 fill = shownPattern ? patternColour(*shownPattern, selected ? 70 : 50) : blockFill;   // the pattern's own colour
      draw->AddRectFilled(ImVec2(left + 2 * scale, y), ImVec2(left + w - 2 * scale, y + rowHeight), fill);
      draw->AddLine(ImVec2(left + 2 * scale, y), ImVec2(left + 2 * scale, y + rowHeight), blockEdge);
      if (starts) {
        draw->AddLine(ImVec2(left + 2 * scale, y), ImVec2(left + w - 2 * scale, y), blockEdge);
        if (const Pattern* pattern = patternOf(lane, shown->pattern)) {
          char label[48];
          std::snprintf(label, sizeof label, "%02X %s", pattern->index, pattern->name.c_str());
          draw->PushClipRect(ImVec2(left, y), ImVec2(left + w - 2 * scale, y + rowHeight), true);
          draw->AddText(ImVec2(left + 5 * scale, y + scale), kText, label);
          draw->PopClipRect();
        }
      }
    }
    if (cursorTick >= 0 && selected) {
      const float y = origin.y + headHeight + static_cast<float>(cursorTick / cell) * rowHeight;
      draw->AddRectFilled(ImVec2(left, y), ImVec2(left + w, y + rowHeight), kCursorCell);
    }
    left += w;
  }

  // the head, drawn last and held at the top of the view
  const float headTop = origin.y + scrollY;
  draw->AddRectFilled(ImVec2(origin.x, headTop), ImVec2(origin.x + totalWidth, headTop + headHeight), ImGui::GetColorU32(ImGuiCol_ChildBg) | 0xFF000000);
  left = origin.x + gutter;
  for (const Lane& lane : project.lanes) {
    const float w = laneWidth(lane, scale);
    draw->PushClipRect(ImVec2(left, headTop), ImVec2(left + w, headTop + headHeight), true);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.85f, ImVec2(left + 4 * scale, headTop + 3 * scale), lane.id == selectedLane ? kAccent : kDimText, lane.id.c_str());   // a small face, so names fit the lane
    draw->PopClipRect();
    left += w;
  }
  draw->AddLine(ImVec2(origin.x, headTop + headHeight - 1), ImVec2(origin.x + totalWidth, headTop + headHeight - 1), kLine);

  ImGui::EndChild();
  return result;
}

}  // namespace wax::app

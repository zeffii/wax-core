#include "app/sequence_view.h"
#include "seq/loop.h"

#include <algorithm>
#include <cstdio>

#include "app/pattern_colour.h"
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

int rowTicks(const Song& song) { return std::max(1, song.beatsPerBar * song.ticksPerBeat); }

}  // namespace

float sequenceWidth(const Project& project) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  float width = kGutter * scale + 14 * scale;   // (room for the scroll bar)
  for (const Lane& lane : project.lanes) width += laneWidth(lane, scale);
  return width;
}

SequenceClick drawSequenceView(const Project& project, const std::string& selectedLane, int playTick, int cursorTick, bool follow) {
  SequenceClick result;
  const float scale = ImGui::GetStyle().FontScaleDpi;
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
    draw->AddText(ImVec2(left + 4 * scale, headTop + 2 * scale), lane.id == selectedLane ? kAccent : kDimText, lane.id.c_str());
    draw->PopClipRect();
    left += w;
  }
  draw->AddLine(ImVec2(origin.x, headTop + headHeight - 1), ImVec2(origin.x + totalWidth, headTop + headHeight - 1), kLine);

  ImGui::EndChild();
  return result;
}

}  // namespace wax::app

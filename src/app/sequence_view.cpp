#include "app/sequence_view.h"
#include "seq/loop.h"
#include "seq/automation.h"

#include <algorithm>
#include <cmath>
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

  // the overview strip at the right edge (when the whole song is not on show already): its place, so clicks there do not reach the sheet
  const float stripPad = 4 * scale;
  const int lanesShown = static_cast<int>(std::count_if(project.lanes.begin(), project.lanes.end(), [](const Lane& l) { return !l.placements.empty(); }));
  const float stripW = static_cast<float>(lanesShown) * 8 * scale + 8 * scale;
  const float bodyHeight = viewHeight - headHeight;
  const bool fitsOnScreen = bodyHeight >= rowHeight * static_cast<float>(rows);
  const bool hasStrip = !fitsOnScreen && lanesShown > 0 && bodyHeight > 40 * scale;
  const float scrollbarW = ImGui::GetScrollMaxY() > 0 ? ImGui::GetStyle().ScrollbarSize : 0;
  const ImVec2 windowPos = ImGui::GetWindowPos();
  const ImVec2 stripMin(windowPos.x + ImGui::GetWindowWidth() - scrollbarW - stripW - stripPad, windowPos.y + headHeight);
  const ImVec2 stripMax(stripMin.x + stripW, stripMin.y + std::max(0.0f, bodyHeight - stripPad));
  const ImVec2 mousePos = ImGui::GetIO().MousePos;
  const bool overStrip = hasStrip && mousePos.x >= stripMin.x && mousePos.x < stripMax.x && mousePos.y >= stripMin.y && mousePos.y < stripMax.y;

  // the click target covers the whole sheet
  ImGui::SetCursorScreenPos(origin);
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  ImGui::InvisibleButton("##sheet", ImVec2(totalWidth, headHeight + rowHeight * static_cast<float>(rows)));
  ImGui::PopItemFlag();
  if (ImGui::IsItemClicked() && !overStrip) {
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
    {   // the lane's shown curve: a hairline down the lane (time down, the value across), its anchors as dots
      const Json curves = lane.raw.is_object() ? lane.raw.value("curves", Json::array()) : Json::array();
      if (curves.is_array() && !curves.empty()) {
        const int pick = std::min(static_cast<int>(curves.size()) - 1, std::max(0, lane.raw.value("shown", 0)));
        const Json& curve = curves[static_cast<std::size_t>(pick)];
        const float pad = 5 * scale, span = std::max(1.0f, w - 2 * pad);
        const float top = origin.y + headHeight;
        const int ticks0 = first * cell, ticks1 = std::min(project.song.length, (last + 1) * cell);
        const int samples = std::max(2, (last - first + 1) * 4);
        const std::vector<double> values = sampleCurve(curve, ticks0, ticks1, samples);
        const ImU32 ink = withAlpha(selected ? kAccent : kText, selected ? 230 : 150);
        ImVec2 before;
        bool drawing = false;
        for (int i = 0; i < samples; i++) {
          const double v = values[static_cast<std::size_t>(i)];
          if (std::isnan(v)) { drawing = false; continue; }
          const double tick = ticks0 + static_cast<double>(ticks1 - ticks0) * i / (samples - 1);
          const ImVec2 at(left + pad + static_cast<float>(v) * span, top + static_cast<float>(tick / cell) * rowHeight);
          if (drawing) draw->AddLine(before, at, ink, 1.2f * scale);
          before = at;
          drawing = true;
        }
        for (const Json& point : curve.value("pts", Json::array())) {
          const double tick = point.value("t", 0.0);
          if (tick < ticks0 || tick > ticks1) continue;
          draw->AddCircleFilled(ImVec2(left + pad + static_cast<float>(point.value("v", 0.0)) * span, top + static_cast<float>(tick / cell) * rowHeight), 2.5f * scale, ink);
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

  if (hasStrip) {   // the overview: the whole song in a thin strip, patterns in their colours; click or drag to scroll there
    const int len = std::max(1, project.song.length);
    const float inner = 2 * scale, height = std::max(1.0f, stripMax.y - stripMin.y - 2 * inner);
    const auto yOf = [&](int tick) { return stripMin.y + inner + static_cast<float>(std::clamp(tick, 0, len)) / static_cast<float>(len) * height; };
    draw->PushClipRect(windowPos, ImVec2(windowPos.x + ImGui::GetWindowWidth(), windowPos.y + viewHeight), false);
    draw->AddRectFilled(stripMin, stripMax, lineAlpha(40));
    if (loop.start < loop.end && loop.on) draw->AddRectFilled(ImVec2(stripMin.x, yOf(loop.start)), ImVec2(stripMax.x, yOf(loop.end)), withAlpha(kWarm, 40));
    float x = stripMin.x + 4 * scale;
    for (const Lane& lane : project.lanes) {
      if (lane.placements.empty()) continue;
      for (const Placement& placement : lane.placements) {
        const Pattern* pattern = patternOf(lane, placement.pattern);
        if (!pattern) continue;
        const int end = placement.start + std::min(placement.length, static_cast<int>(pattern->rows.size()));
        draw->AddRectFilled(ImVec2(x, yOf(placement.start)), ImVec2(x + 6 * scale, std::max(yOf(placement.start) + 1.5f, yOf(end) - 1)), patternColour(*pattern, lane.id == selectedLane ? 230 : 150));
      }
      x += 8 * scale;
    }
    const int firstTick = static_cast<int>(scrollY / rowHeight) * cell, lastTick = static_cast<int>((scrollY + bodyHeight) / rowHeight) * cell;
    draw->AddRectFilled(ImVec2(stripMin.x, yOf(firstTick)), ImVec2(stripMax.x, std::max(yOf(firstTick) + 3, yOf(lastTick))), IM_COL32(255, 255, 255, 36));   // the part on show
    if (cursorTick >= 0) draw->AddRectFilled(ImVec2(stripMin.x, yOf(cursorTick)), ImVec2(stripMax.x, yOf(cursorTick) + 2), kAccent);
    if (playTick >= 0) draw->AddLine(ImVec2(stripMin.x, yOf(playTick)), ImVec2(stripMax.x, yOf(playTick)), kText);
    draw->PopClipRect();
    static bool dragging = false;
    if (overStrip && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) dragging = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging = false;
    if (dragging) {   // the box centres on the pointer
      const float share = std::clamp((mousePos.y - stripMin.y - inner) / height, 0.0f, 1.0f);
      ImGui::SetScrollY(std::max(0.0f, share * static_cast<float>(len) / static_cast<float>(cell) * rowHeight - bodyHeight / 2));
    }
  }

  ImGui::EndChild();
  return result;
}

}  // namespace wax::app

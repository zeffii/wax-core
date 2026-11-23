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

constexpr float kLoopLane = 6.0f;      // the loop's bar, at the very left
constexpr float kGutter = 44.0f + kLoopLane;   // the loop lane and the tick numbers
// The whole sequence editor starts one character's width in from the browser's drag handle beside it (about 8 px).
float leftMargin() { return ImGui::CalcTextSize("0").x; }
float gutterWidth(float scale) { return kGutter * scale; }
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

int sequenceRowTicks(const Song& song) { return rowTicks(song); }

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
  float width = leftMargin() + gutterWidth(scale) + 14 * scale;   // (room for the scroll bar)
  for (const Lane& lane : project.lanes) width += laneWidth(lane, scale);
  return width;
}

namespace {

// Where everything is, worked out once a frame inside the child window: the sizes in pixels, and which rows are on show.
struct Geometry {
  float scale = 1;
  int cell = 1, rows = 1;                  // a row is `cell` ticks; the song has `rows` of them
  float rowHeight = 0, headHeight = 0, gutter = 0, totalWidth = 0;
  ImVec2 origin;                           // the top left of the sheet (scrolled with it)
  float scrollY = 0, viewHeight = 0;
  int first = 0, last = 0;                 // the rows on show

  float rowTop(int row) const { return origin.y + headHeight + static_cast<float>(row) * rowHeight; }
  float bottom() const { return rowTop(rows); }
};

// The overview strip at the right edge, when the whole song is not on show already: where it is, so clicks there do not reach the sheet.
struct Strip {
  bool shown = false, over = false;
  ImVec2 min, max;
  float bodyHeight = 0;
};

Geometry measure(const Project& project, ImVec2 origin) {
  Geometry g;
  g.scale = ImGui::GetStyle().FontScaleDpi;
  g.cell = rowTicks(project.song);
  g.rows = std::max(1, (project.song.length + g.cell - 1) / g.cell);
  g.rowHeight = ImGui::GetTextLineHeight() + 2 * g.scale;
  g.headHeight = g.rowHeight + 4 * g.scale;
  g.gutter = gutterWidth(g.scale);
  g.origin = origin;
  g.scrollY = ImGui::GetScrollY();
  g.viewHeight = ImGui::GetWindowHeight();
  g.totalWidth = g.gutter;
  for (const Lane& lane : project.lanes) g.totalWidth += laneWidth(lane, g.scale);
  g.first = std::max(0, static_cast<int>((g.scrollY - g.headHeight) / g.rowHeight));
  g.last = std::min(g.rows - 1, static_cast<int>((g.scrollY + g.viewHeight) / g.rowHeight));
  return g;
}

// Ctrl+wheel: the size of a row, a step a notch.
void zoomRows(const Project& project) {
  if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) || !ImGui::GetIO().KeyCtrl || ImGui::GetIO().MouseWheel == 0) return;
  const std::vector<int> sizes = rowSizes(project.song);
  const int at = static_cast<int>(std::find(sizes.begin(), sizes.end(), rowTicks(project.song)) - sizes.begin());
  const int next = std::clamp(at + (ImGui::GetIO().MouseWheel > 0 ? 1 : -1), 0, static_cast<int>(sizes.size()) - 1);
  if (next != at) { settings().arrangementRow = sizes[static_cast<std::size_t>(next)]; saveSettings(); }
}

// Keeps the play row (with follow) and a cursor moved by the keys in sight.
void keepInSight(const Geometry& g, const SequenceState& state) {
  if (state.follow && state.playTick >= 0) {   // the play row in the middle third
    const float y = g.headHeight + static_cast<float>(state.playTick / g.cell) * g.rowHeight;
    if (y < g.scrollY + g.headHeight || y > g.scrollY + g.viewHeight - 2 * g.rowHeight) ImGui::SetScrollY(std::max(0.0f, y - g.viewHeight * 0.4f));
  }
  static int lastCursor = -1;   // (the view only follows a cursor that has moved, so the mouse wheel can scroll it away)
  if (state.cursorTick >= 0 && state.cursorTick != lastCursor) {
    const float y = g.headHeight + static_cast<float>(state.cursorTick / g.cell) * g.rowHeight;
    if (y < g.scrollY + g.headHeight) ImGui::SetScrollY(std::max(0.0f, y - g.headHeight));
    else if (y + g.rowHeight > g.scrollY + g.viewHeight) ImGui::SetScrollY(y + g.rowHeight - g.viewHeight);
  }
  lastCursor = state.cursorTick;
}

Strip placeStrip(const Geometry& g, const Project& project) {
  Strip strip;
  const float pad = 4 * g.scale;
  const int lanesShown = static_cast<int>(std::count_if(project.lanes.begin(), project.lanes.end(), [](const Lane& l) { return !l.placements.empty(); }));
  const float width = static_cast<float>(lanesShown) * 8 * g.scale + 8 * g.scale;
  strip.bodyHeight = g.viewHeight - g.headHeight;
  const bool fitsOnScreen = strip.bodyHeight >= g.rowHeight * static_cast<float>(g.rows);
  strip.shown = settings().seqOverview && !fitsOnScreen && lanesShown > 0 && strip.bodyHeight > 40 * g.scale;
  const float scrollbarW = ImGui::GetScrollMaxY() > 0 ? ImGui::GetStyle().ScrollbarSize : 0;
  const ImVec2 windowPos = ImGui::GetWindowPos();
  strip.min = ImVec2(windowPos.x + ImGui::GetWindowWidth() - scrollbarW - width - pad, windowPos.y + g.headHeight);
  strip.max = ImVec2(strip.min.x + width, strip.min.y + std::max(0.0f, strip.bodyHeight - pad));
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  strip.over = strip.shown && mouse.x >= strip.min.x && mouse.x < strip.max.x && mouse.y >= strip.min.y && mouse.y < strip.max.y;
  return strip;
}

// Tells the curves' view where each lane's column is, and what the pointer is over.
void frameCurves(CurveView& curves, const Project& project, const Geometry& g, const std::string& selectedLane) {
  std::vector<CurveView::LaneBox> boxes;
  float x = g.origin.x + g.gutter;
  for (const Lane& lane : project.lanes) {
    boxes.push_back({&lane, x, laneWidth(lane, g.scale), lane.id == selectedLane});
    x += laneWidth(lane, g.scale);
  }
  curves.frame(project, std::move(boxes), g.rowTop(0), g.rowHeight, g.cell, g.first * g.cell, (g.last + 1) * g.cell, g.scale);
}

// The lane and row under the mouse (false over the tick column, the gutter or past the last lane).
bool laneRowAt(const Project& project, const Geometry& g, const ImVec2& mouse, std::string& laneId, int& rowTick) {
  const float y = mouse.y - g.origin.y - g.headHeight;
  float x = mouse.x - g.origin.x - g.gutter;
  if (y < 0) return false;
  for (const Lane& lane : project.lanes) {
    const float w = laneWidth(lane, g.scale);
    if (x >= 0 && x < w) { laneId = lane.id; rowTick = std::clamp(static_cast<int>(y / g.rowHeight), 0, g.rows - 1) * g.cell; return true; }
    x -= w;
  }
  return false;
}

// What the click target (the whole sheet) was given this frame: a click, or a drag that grows a block.
void readMouse(const Project& project, const Geometry& g, const Strip& strip, const CurveView& curves, SequenceClick& result) {
  if (ImGui::IsItemClicked() && !strip.over && !curves.claimsPointer()) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float y = mouse.y - g.origin.y - g.headHeight;
    if (y >= 0 && mouse.x - g.origin.x - g.gutter < 0) result.playFrom = std::clamp(static_cast<int>(y / g.rowHeight), 0, g.rows - 1) * g.cell;   // the tick column: play from there
    else if (laneRowAt(project, g, mouse, result.lane, result.tick)) result.doubleClicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
  } else if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !strip.over && !curves.claimsPointer()) {   // dragging from a click: the block grows to the pointer
    laneRowAt(project, g, ImGui::GetIO().MousePos, result.dragLane, result.dragTick);
  }
}

// The loop: a thin bar in the lane left of the tick numbers over its rows, and a line across the sheet at each end.
void drawLoop(ImDrawList* draw, const Geometry& g, const Song& song) {
  const LoopRange loop = loopOf(song);
  if (!loop.on) return;
  const float top = g.rowTop(loop.start / g.cell), bottom = g.rowTop((loop.end + g.cell - 1) / g.cell);
  draw->AddRectFilled(ImVec2(g.origin.x + g.scale, top), ImVec2(g.origin.x + 4 * g.scale, bottom), kWarm);   // (its own narrow lane, left of the tick numbers)
  draw->AddLine(ImVec2(g.origin.x, top), ImVec2(g.origin.x + g.totalWidth, top), withAlpha(kWarm, 110));
  draw->AddLine(ImVec2(g.origin.x, bottom), ImVec2(g.origin.x + g.totalWidth, bottom), withAlpha(kWarm, 110));
}

// The gutter: tick numbers, the bright ones every four bars.
void drawTickNumbers(ImDrawList* draw, const Geometry& g) {
  const int bar16 = g.cell * 4;
  for (int r = g.first; r <= g.last; ++r) {
    char number[16];
    std::snprintf(number, sizeof number, "%04d", r * g.cell);
    draw->AddText(ImVec2(g.origin.x + (kLoopLane + 2) * g.scale, g.rowTop(r) + g.scale), r * g.cell % bar16 == 0 ? kText : kDimText, number);
  }
}

// The pictures of the patterns go in their own layer, so they cost a few draw commands and not one between every cell's fill and label.
enum Layer { kFills = 0, kPictures = 1, kTop = 2 };

// One row of one lane: the pattern's block (fill, edge, picture, and its index and name where it starts).
void drawCell(ImDrawList* draw, const Geometry& g, const Lane& lane, int row, float left, float width, ImU32 blockFill, ImU32 blockEdge, const SequencePictures& pictures, const MinimapOptions& pictureOptions) {
  const float y = g.rowTop(row);
  const int t0 = row * g.cell;
  // what the row shows: the pattern that starts inside it, else the one covering its start
  const Placement* shown = nullptr;
  for (const Placement& p : lane.placements) if (p.start >= t0 && p.start < t0 + g.cell && (!shown || p.start < shown->start)) shown = &p;
  const bool starts = shown != nullptr;
  if (!shown) shown = placementAt(lane, t0);
  if (!shown) return;
  const bool withPictures = pictures.lanes && pictures.cache && settings().minimapOn;
  const auto layer = [&](Layer l) { if (withPictures) draw->ChannelsSetCurrent(l); };
  const Pattern* shownPattern = patternOf(lane, shown->pattern);
  const ImU32 fill = shownPattern ? patternColour(*shownPattern, 50) : blockFill;   // the pattern's own colour
  const ImVec2 inner = ImVec2(left + 2 * g.scale, y);
  layer(kFills);
  draw->AddRectFilled(inner, ImVec2(left + width - 2 * g.scale, y + g.rowHeight), fill);
  draw->AddLine(inner, ImVec2(inner.x, y + g.rowHeight), blockEdge);
  layer(kTop);
  if (withPictures) {
    const auto source = pictures.lanes->find(lane.id);
    if (source != pictures.lanes->end()) {
      layer(kPictures);
      drawMinimap(draw, lane.id, source->second, *pictures.cache, t0, g.cell, inner, width - 4 * g.scale, g.rowHeight, pictureOptions);
      layer(kTop);
    }
  }
  if (!starts) return;
  draw->AddLine(inner, ImVec2(left + width - 2 * g.scale, y), blockEdge);
  const Pattern* pattern = patternOf(lane, shown->pattern);
  if (!pattern) return;
  char label[48];
  if (settings().seqNames) std::snprintf(label, sizeof label, "%02X %s", pattern->index, pattern->name.c_str());
  else std::snprintf(label, sizeof label, "%02X", pattern->index);   // (the hex number alone; the status bar still has the name)
  draw->PushClipRect(ImVec2(left, y), ImVec2(left + width - 2 * g.scale, y + g.rowHeight), true);
  draw->AddText(ImVec2(left + 5 * g.scale, y + g.scale), kText, label);
  draw->PopClipRect();
}

// The lane's marks over its cells: the selected block's tint, the clicked pattern's rectangle, the cursor cell.
void drawLaneMarks(ImDrawList* draw, const Geometry& g, const Lane& lane, int laneIndex, float left, float width, bool selected, const SequenceState& state) {
  const SequenceBlock& block = state.block;
  if (block.first >= 0 && laneIndex >= block.first && laneIndex <= block.last) {   // the selected block, tinted
    draw->AddRectFilled(ImVec2(left, g.rowTop(block.from / g.cell)), ImVec2(left + width, g.rowTop((block.to + g.cell - 1) / g.cell)), accentAlpha(55));
  }
  if (selected && state.pickedTick >= 0) {   // the pattern that was clicked: a rectangle round it
    if (const Placement* picked = placementAt(lane, state.pickedTick)) {
      const float top = g.rowTop(picked->start / g.cell), bottom = g.rowTop((picked->start + picked->length + g.cell - 1) / g.cell);
      draw->AddRect(ImVec2(left + 2 * g.scale, top + 1), ImVec2(left + width - 2 * g.scale, bottom - 1), kAccent, 0, 0, 1.5f * g.scale);
    }
  }
  if (state.cursorTick >= 0 && selected) {
    const float y = g.rowTop(state.cursorTick / g.cell);
    draw->AddRectFilled(ImVec2(left, y), ImVec2(left + width, y + g.rowHeight), kCursorCell);
  }
}

// The lanes, column by column.
void drawLanes(ImDrawList* draw, const Geometry& g, const Project& project, CurveView& curves, const SequenceState& state, const SequencePictures& pictures) {
  MinimapOptions pictureOptions;
  pictureOptions.lengths = settings().minimapLengths; pictureOptions.caps = settings().minimapCaps; pictureOptions.params = settings().minimapParams;
  const bool withPictures = pictures.lanes && pictures.cache && settings().minimapOn;
  if (withPictures) draw->ChannelsSplit(3);
  float left = g.origin.x + g.gutter;
  int laneIndex = -1;
  for (const Lane& lane : project.lanes) {
    laneIndex++;
    const float width = laneWidth(lane, g.scale);
    const bool selected = lane.id == state.selectedLane;
    if (withPictures) draw->ChannelsSetCurrent(kTop);
    draw->AddLine(ImVec2(left, g.rowTop(0)), ImVec2(left, g.bottom()), lineAlpha(160));
    for (int r = g.first; r <= g.last; ++r) drawCell(draw, g, lane, r, left, width, dimAlpha(50), dimAlpha(140), pictures, pictureOptions);   // (every lane alike: the picked lane is shown by its head, the picked pattern by its rectangle)
    if (withPictures) draw->ChannelsSetCurrent(kTop);
    curves.draw(draw, lane, selected);
    drawLaneMarks(draw, g, lane, laneIndex, left, width, selected, state);
    left += width;
  }
  if (withPictures) draw->ChannelsMerge();
}

// The playhead: a thin red line across the lanes, moving smoothly down through the rows as the song plays.
void drawPlayhead(ImDrawList* draw, const Geometry& g, const SequenceState& state) {
  if (state.playTick < 0) return;
  const float y = g.origin.y + g.headHeight + (static_cast<float>(state.playTick) + std::clamp(state.playFrac, 0.0f, 1.0f)) / static_cast<float>(g.cell) * g.rowHeight;
  draw->AddLine(ImVec2(g.origin.x, y), ImVec2(g.origin.x + g.totalWidth, y), IM_COL32(230, 60, 60, 255), 1.0f);
}

// The head, drawn last and held at the top of the view.
void drawHead(ImDrawList* draw, const Geometry& g, const Project& project, const std::string& selectedLane) {
  const float headTop = g.origin.y + g.scrollY;
  draw->AddRectFilled(ImVec2(g.origin.x, headTop), ImVec2(g.origin.x + g.totalWidth, headTop + g.headHeight), ImGui::GetColorU32(ImGuiCol_ChildBg) | 0xFF000000);
  float left = g.origin.x + g.gutter;
  for (const Lane& lane : project.lanes) {
    const float w = laneWidth(lane, g.scale);
    draw->PushClipRect(ImVec2(left, headTop), ImVec2(left + w, headTop + g.headHeight), true);
    if (lane.id == selectedLane) {   // the picked lane: its head is tinted and underlined
      draw->AddRectFilled(ImVec2(left, headTop), ImVec2(left + w, headTop + g.headHeight), accentAlpha(40));
      draw->AddRectFilled(ImVec2(left, headTop + g.headHeight - 2 * g.scale), ImVec2(left + w, headTop + g.headHeight), kAccent);
    }
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.85f, ImVec2(left + 4 * g.scale, headTop + 3 * g.scale), lane.id == selectedLane ? kAccent : kDimText, lane.id.c_str());   // a small face, so names fit the lane
    draw->PopClipRect();
    // a lane that does not sound steps back, whether it is muted or another lane is soloed (the same amount); the whole column, head included
    const int stepBack = lane.silenced || lane.soloMuted ? 80 : 0;
    if (stepBack > 0) draw->AddRectFilled(ImVec2(left, headTop), ImVec2(left + w, g.bottom()), withAlpha(ImGui::GetColorU32(ImGuiCol_ChildBg), stepBack));
    left += w;
  }
  draw->AddLine(ImVec2(g.origin.x, headTop + g.headHeight - 1), ImVec2(g.origin.x + g.totalWidth, headTop + g.headHeight - 1), kLine);
}

// The overview: the whole song in a thin strip, patterns in their colours; click or drag to scroll there.
void drawOverview(ImDrawList* draw, const Geometry& g, const Strip& strip, const Project& project, const SequenceState& state) {
  const LoopRange loop = loopOf(project.song);
  const int len = std::max(1, project.song.length);
  const float inner = 2 * g.scale, height = std::max(1.0f, strip.max.y - strip.min.y - 2 * inner);
  const auto yOf = [&](int tick) { return strip.min.y + inner + static_cast<float>(std::clamp(tick, 0, len)) / static_cast<float>(len) * height; };
  const ImVec2 windowPos = ImGui::GetWindowPos();
  draw->PushClipRect(windowPos, ImVec2(windowPos.x + ImGui::GetWindowWidth(), windowPos.y + g.viewHeight), false);
  draw->AddRectFilled(strip.min, strip.max, lineAlpha(40));
  if (loop.start < loop.end && loop.on) {   // the loop: a solid edge down the strip and a bright line at each end (no tint over its span)
    draw->AddRectFilled(ImVec2(strip.min.x, yOf(loop.start)), ImVec2(strip.min.x + 2 * g.scale, yOf(loop.end)), kWarm);
    draw->AddRectFilled(ImVec2(strip.min.x, yOf(loop.start) - g.scale), ImVec2(strip.max.x, yOf(loop.start) + g.scale), kWarm);
    draw->AddRectFilled(ImVec2(strip.min.x, yOf(loop.end) - g.scale), ImVec2(strip.max.x, yOf(loop.end) + g.scale), kWarm);
  }
  float x = strip.min.x + 4 * g.scale;
  for (const Lane& lane : project.lanes) {
    if (lane.placements.empty()) continue;
    for (const Placement& placement : lane.placements) {
      const Pattern* pattern = patternOf(lane, placement.pattern);
      if (!pattern) continue;
      const int end = placement.start + std::min(placement.length, static_cast<int>(pattern->rows.size()));
      draw->AddRectFilled(ImVec2(x, yOf(placement.start)), ImVec2(x + 6 * g.scale, std::max(yOf(placement.start) + 1.5f, yOf(end) - 1)), patternColour(*pattern, lane.id == state.selectedLane ? 230 : 150));
    }
    x += 8 * g.scale;
  }
  const int firstTick = static_cast<int>(g.scrollY / g.rowHeight) * g.cell, lastTick = static_cast<int>((g.scrollY + strip.bodyHeight) / g.rowHeight) * g.cell;
  draw->AddRectFilled(ImVec2(strip.min.x, yOf(firstTick)), ImVec2(strip.max.x, std::max(yOf(firstTick) + 3, yOf(lastTick))), IM_COL32(255, 255, 255, 36));   // the part on show
  if (state.cursorTick >= 0) draw->AddRectFilled(ImVec2(strip.min.x, yOf(state.cursorTick)), ImVec2(strip.max.x, yOf(state.cursorTick) + 2), kAccent);
  if (state.playTick >= 0) draw->AddLine(ImVec2(strip.min.x, yOf(state.playTick)), ImVec2(strip.max.x, yOf(state.playTick)), kText);
  draw->PopClipRect();
  static bool dragging = false;
  if (strip.over && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) dragging = true;
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging = false;
  if (dragging) {   // the box centres on the pointer
    const float share = std::clamp((ImGui::GetIO().MousePos.y - strip.min.y - inner) / height, 0.0f, 1.0f);
    ImGui::SetScrollY(std::max(0.0f, share * static_cast<float>(len) / static_cast<float>(g.cell) * g.rowHeight - strip.bodyHeight / 2));
  }
}

}  // namespace

SequenceClick drawSequenceView(const Project& project, const SequenceState& state, CurveView& curves, const CurveView::Targets& targets, const SequencePictures& pictures) {
  SequenceClick result;
  zoomRows(project);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + leftMargin());
  ImGui::BeginChild("sequence", ImVec2(0, 0), false, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_HorizontalScrollbar);
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const Geometry g = measure(project, ImGui::GetCursorScreenPos());
  keepInSight(g, state);
  const Strip strip = placeStrip(g, project);
  frameCurves(curves, project, g, state.selectedLane);

  // the click target covers the whole sheet
  ImGui::SetCursorScreenPos(g.origin);
  ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
  ImGui::InvisibleButton("##sheet", ImVec2(g.totalWidth, g.headHeight + g.rowHeight * static_cast<float>(g.rows)));
  ImGui::PopItemFlag();
  readMouse(project, g, strip, curves, result);

  drawLoop(draw, g, project.song);
  drawTickNumbers(draw, g);
  drawLanes(draw, g, project, curves, state, pictures);
  if (!strip.over) result.curve = curves.interact(targets);
  drawPlayhead(draw, g, state);
  drawHead(draw, g, project, state.selectedLane);
  if (strip.shown) drawOverview(draw, g, strip, project, state);
  ImGui::EndChild();
  return result;
}

}  // namespace wax::app

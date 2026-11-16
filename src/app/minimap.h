// Pattern minimaps for the Song screen: each cell shows a small picture of the ticks it stands for. Notes are ticks placed across the
// cell by pitch and as bright as their velocity; a note's length is a thin line down to the next note, note off (===, cyan cap) or note
// cut (^^^, magenta cap); the parameter columns are grey cells at the right edge, brighter the higher the value. Port of webtracker
// src/seq/minimap.js.
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "seq/columns.h"

namespace wax::app {

struct MinimapOptions {
  bool lengths = true;   // a thin line from each note to the next event
  bool caps = true;      // note off and note cut marks
  bool params = true;    // the parameter strip at the right edge
};

// What one lane offers the minimap: its columns, and its flattened rows (rows[tick][column]).
struct MinimapLane {
  const std::vector<Column>* columns = nullptr;
  const std::vector<std::vector<std::string>>* rows = nullptr;
};

// What a row holds, read once per frame.
struct MinimapEvent { int column = 0; char kind = 'n'; int pitch = 0; float velocity = 0.8f; };   // kind: n note, o off, c cut
struct MinimapRow { std::vector<MinimapEvent> notes; std::vector<float> params; };   // params: 0..1, -1 empty

// Row readings kept for the frame that is drawing (the look-back for notes still sounding reads the same rows again and again).
struct MinimapCache { std::map<std::string, std::unordered_map<int, MinimapRow>> rows; };

// Draws the picture of ticks t0..t0+ticks-1 of one lane into the cell at `topLeft` (the left four characters are left alone: the pattern's
// number is written there).
void drawMinimap(ImDrawList* draw, const std::string& laneId, const MinimapLane& lane, MinimapCache& cache, int t0, int ticks, ImVec2 topLeft, float width, float height, const MinimapOptions& options);

}  // namespace wax::app

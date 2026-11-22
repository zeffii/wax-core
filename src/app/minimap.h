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
  int revision = 0;   // changes whenever this lane's rows do: pictures painted for an older one are painted again
};

// What a row holds, read once per frame.
struct MinimapEvent { int column = 0; char kind = 'n'; int pitch = 0; float velocity = 0.8f; };   // kind: n note, o off, c cut
struct MinimapRow { std::vector<MinimapEvent> notes; std::vector<float> params; };   // params: 0..1, -1 empty

// One rectangle of a picture, relative to the cell's top left.
struct MinimapShape { float x, y, w, h; ImU32 colour; };

// What the pictures keep between frames. The pictures are painted once into textures, many cells (a "page" of rows of one lane) to a
// texture, and a frame only places them: a cell is one image, however many notes it holds. A page is painted again when what it shows or
// how it looks changes (its lane's rows, the cell's size, the theme, the switches). The readings of the rows are kept for painting.
// Held by the session, so a song that is opened starts with none.
struct MinimapCache {
  struct Stamp {
    int revision = -1, ticks = 0, width = 0, height = 0, options = 0, columns = 0;
    ImU32 ink = 0;
    bool operator==(const Stamp& other) const { return revision == other.revision && ticks == other.ticks && width == other.width && height == other.height && options == other.options && columns == other.columns && ink == other.ink; }
  };
  std::map<std::string, std::unordered_map<int, MinimapRow>> rows;
  std::map<std::string, int> revisions;     // lane id -> the revision the readings above were made for
  std::map<std::string, Stamp> pages;       // "lane#page" -> what the texture of that page shows
};

// Draws the picture of ticks t0..t0+ticks-1 of one lane into the cell at `topLeft` (the left four characters are left alone: the pattern's
// number is written there).
void drawMinimap(ImDrawList* draw, const std::string& laneId, const MinimapLane& lane, MinimapCache& cache, int t0, int ticks, ImVec2 topLeft, float width, float height, const MinimapOptions& options);

}  // namespace wax::app

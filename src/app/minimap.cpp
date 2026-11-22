#include "app/minimap.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "app/theme.h"

namespace wax::app {

namespace {

constexpr int kReservedChars = 4;                 // the left of a cell is the pattern's number
constexpr int kLowest = 24, kHighest = 96;        // the pitch range across a cell: C-1 to C-7, as MIDI numbers
const ImU32 kOffColour = IM_COL32(23, 184, 212, 255), kCutColour = IM_COL32(255, 63, 164, 255);

// Which columns are notes (with their velocity column, if the next one is) and which are parameters.
struct Plan {
  struct Note { int column; int velocity; };   // velocity: the column index, -1 none
  std::vector<Note> notes;
  std::vector<int> params;
};

Plan planOf(const std::vector<Column>& columns) {
  Plan plan;
  std::vector<bool> isVelocity(columns.size(), false);
  for (std::size_t i = 0; i < columns.size(); i++) {
    const Column& c = columns[i];
    if (c.kind != ColumnKind::Note && !c.rel) continue;   // (a meta machine's relative notes are notes too, 80 being their root)
    const bool hasVelocity = i + 1 < columns.size() && columns[i + 1].kind == ColumnKind::Vel;
    plan.notes.push_back({static_cast<int>(i), hasVelocity ? static_cast<int>(i) + 1 : -1});
    if (hasVelocity) isVelocity[i + 1] = true;
  }
  for (std::size_t i = 0; i < columns.size(); i++) {
    const Column& c = columns[i];
    if (c.kind == ColumnKind::Note || c.rel || c.kind == ColumnKind::Spacer || isVelocity[i]) continue;
    plan.params.push_back(static_cast<int>(i));
  }
  return plan;
}

bool isHex(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }); }
int hexValue(const std::string& s) { return static_cast<int>(std::strtol(s.c_str(), nullptr, 16)); }

// A note cell's pitch as a MIDI number (C-4 is 60), -1 when the text is not a note.
int noteOf(const std::string& text) {
  if (text.size() < 3) return -1;
  static const int semitone[] = {9, 11, 0, 2, 4, 5, 7};   // A B C D E F G
  const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
  if (letter < 'A' || letter > 'G') return -1;
  if ((text[1] != '-' && text[1] != '#') || !std::isdigit(static_cast<unsigned char>(text[2]))) return -1;
  return semitone[letter - 'A'] + (text[1] == '#' ? 1 : 0) + (text[2] - '0' + 1) * 12;
}

float velocityOf(const std::vector<std::string>& row, int column) {
  if (column < 0 || column >= static_cast<int>(row.size()) || !isHex(row[static_cast<std::size_t>(column)])) return 0.8f;
  return static_cast<float>(hexValue(row[static_cast<std::size_t>(column)])) / 255.0f;
}

MinimapRow read(const MinimapLane& lane, const Plan& plan, int tick) {
  MinimapRow info;
  const auto& rows = *lane.rows;
  if (tick < 0 || tick >= static_cast<int>(rows.size()) || rows[static_cast<std::size_t>(tick)].empty()) return info;
  const std::vector<std::string>& row = rows[static_cast<std::size_t>(tick)];
  const std::vector<Column>& columns = *lane.columns;
  for (const Plan::Note& n : plan.notes) {
    if (n.column >= static_cast<int>(row.size())) continue;
    const std::string& token = row[static_cast<std::size_t>(n.column)];
    const Column& column = columns[static_cast<std::size_t>(n.column)];
    if (column.rel) {   // a relative note: 00 is a note off, otherwise the root and the steps above or below it
      if (token == "00") info.notes.push_back({n.column, 'o', 0, 0});
      else if (token.size() == 2 && isHex(token)) info.notes.push_back({n.column, 'n', std::clamp(column.root + hexValue(token) - 0x80, 0, 127), velocityOf(row, n.velocity)});
    } else if (token == "===") info.notes.push_back({n.column, 'o', 0, 0});
    else if (token == "^^^") info.notes.push_back({n.column, 'c', 0, 0});
    else if (const int pitch = noteOf(token); pitch >= 0) info.notes.push_back({n.column, 'n', pitch, velocityOf(row, n.velocity)});
  }
  for (int i : plan.params) {
    if (i >= static_cast<int>(row.size())) { info.params.push_back(-1); continue; }
    const std::string& token = row[static_cast<std::size_t>(i)];
    const Token kind = columns[static_cast<std::size_t>(i)].token;
    const int max = kind == Token::Hex1 ? 15 : kind == Token::Hex2 ? 255 : kind == Token::Hex4 ? 65535 : 0;
    if (max) info.params.push_back(isHex(token) ? static_cast<float>(hexValue(token)) / static_cast<float>(max) : -1.0f);
    else if (token.size() >= 2 && token != "......" && std::isxdigit(static_cast<unsigned char>(token[0])) && std::isxdigit(static_cast<unsigned char>(token[1])))
      info.params.push_back(std::min(1.0f, static_cast<float>(hexValue(token.substr(2))) / 65535.0f + 0.15f));   // a command: present, shown by its value
    else info.params.push_back(-1);
  }
  return info;
}

const MinimapRow& cached(MinimapCache& cache, const std::string& laneId, const MinimapLane& lane, const Plan& plan, int tick) {
  auto& rows = cache.rows[laneId];
  const auto found = rows.find(tick);
  if (found != rows.end()) return found->second;
  return rows.emplace(tick, read(lane, plan, tick)).first->second;
}

}  // namespace

namespace {

// The rectangles of one cell's picture, relative to its top left.
std::vector<MinimapShape> shapesOf(const std::string& laneId, const MinimapLane& lane, MinimapCache& cache, int t0, int ticks, float width, float height, const MinimapOptions& options) {
  std::vector<MinimapShape> shapes;
  const float charWidth = ImGui::CalcTextSize("0").x;
  const float skip = std::min(width, kReservedChars * charWidth);
  if (width - skip < charWidth) return shapes;
  const float x0 = skip, w = width - skip, y0 = 0;
  const Plan plan = planOf(*lane.columns);
  const float step = height / static_cast<float>(ticks), pad = 3;
  const float paramWidth = options.params && !plan.params.empty() ? std::min(w * 0.38f, static_cast<float>(plan.params.size()) * 5 + 3) : 0;
  const float noteWidth = w - paramWidth - pad * 2;
  const auto xOf = [&](int pitch) { return x0 + pad + std::clamp(static_cast<float>(pitch - kLowest) / (kHighest - kLowest), 0.0f, 1.0f) * (noteWidth - 4); };
  const auto ink = [&](float a) { return textAlpha(static_cast<int>(255 * std::clamp(a, 0.0f, 1.0f))); };
  const auto fill = [&](float x, float y, float rw, float rh, ImU32 colour) { shapes.push_back({x, y, rw, rh, colour}); };

  struct Sounding { float x, from, velocity; };
  std::map<int, Sounding> live;   // the notes that are sounding as the cell begins: the last event in each note column before it
  if (options.lengths) {
    for (const Plan::Note& n : plan.notes) {
      for (int t = t0 - 1, left = 96; t >= 0 && left > 0; t--, left--) {
        const MinimapRow& before = cached(cache, laneId, lane, plan, t);
        const auto event = std::find_if(before.notes.begin(), before.notes.end(), [&](const MinimapEvent& e) { return e.column == n.column; });
        if (event == before.notes.end()) continue;
        if (event->kind == 'n') live[n.column] = {xOf(event->pitch), y0 - 1, event->velocity};
        break;
      }
    }
  }
  const auto line = [&](const Sounding& s, float to) { const float from = std::max(s.from, y0); if (to > from) fill(s.x - 0.5f, from, 1, to - from, ink(0.28f * (0.4f + s.velocity))); };
  for (int k = 0; k < ticks; k++) {
    const MinimapRow& info = cached(cache, laneId, lane, plan, t0 + k);
    const float y = y0 + static_cast<float>(k) * step;
    for (const MinimapEvent& e : info.notes) {
      const auto open = live.find(e.column);
      const bool sounding = open != live.end();
      const float soundingX = sounding ? open->second.x : 0;
      if (sounding && options.lengths) { line(open->second, y); live.erase(open); }
      if (e.kind == 'n') {
        const float x = xOf(e.pitch);
        fill(x - 2.5f, y, 5, std::max(1.4f, step * 0.8f), ink(0.3f + 0.7f * e.velocity));
        if (options.lengths) live[e.column] = {x, y, e.velocity};
      } else if (options.caps) {
        const float x = sounding ? soundingX : x0 + pad + noteWidth / 2;   // (over the line it ends, if there was one)
        fill(x - 3.5f, y, 7, std::max(1.5f, step * 0.7f), e.kind == 'c' ? kCutColour : kOffColour);
      }
    }
  }
  if (options.lengths) for (const auto& [column, s] : live) line(s, y0 + height);
  if (paramWidth > 0) {   // the parameters: a narrow column each at the right edge, one cell per tick that sets a value
    const float each = (paramWidth - 3) / static_cast<float>(plan.params.size()), cellWidth = std::clamp(each - 1, 0.6f, 4.0f), left = x0 + w - paramWidth + 2;
    for (int k = 0; k < ticks; k++) {
      const MinimapRow& info = cached(cache, laneId, lane, plan, t0 + k);
      const float y = y0 + static_cast<float>(k) * step;
      for (std::size_t c = 0; c < info.params.size(); c++)
        if (info.params[c] >= 0) fill(left + static_cast<float>(c) * std::min(cellWidth + 1, each), y, cellWidth, std::max(1.4f, step), ink(0.14f + 0.72f * info.params[c]));
    }
  }
  return shapes;
}

}  // namespace

void drawMinimap(ImDrawList* draw, const std::string& laneId, const MinimapLane& lane, MinimapCache& cache, int t0, int ticks, ImVec2 topLeft, float width, float height, const MinimapOptions& options) {
  if (!lane.columns || !lane.rows || ticks < 1) return;
  int& seen = cache.revisions[laneId];
  if (seen != lane.revision) { seen = lane.revision; cache.rows[laneId].clear(); cache.cells[laneId].clear(); }
  MinimapCache::Cell& cell = cache.cells[laneId][t0];
  const int bits = (options.lengths ? 1 : 0) | (options.caps ? 2 : 0) | (options.params ? 4 : 0), columns = static_cast<int>(lane.columns->size());
  if (cell.t0 != t0 || cell.ticks != ticks || cell.revision != lane.revision || cell.columns != columns || cell.width != width || cell.height != height || cell.options != bits || cell.ink != kText) {   // (anything that changes what it looks like: made again)
    cell.shapes = shapesOf(laneId, lane, cache, t0, ticks, width, height, options);
    cell.t0 = t0; cell.ticks = ticks; cell.revision = lane.revision; cell.columns = columns; cell.width = width; cell.height = height; cell.options = bits; cell.ink = kText;
  }
  for (const MinimapShape& r : cell.shapes) draw->AddRectFilled(ImVec2(topLeft.x + r.x, topLeft.y + r.y), ImVec2(topLeft.x + r.x + r.w, topLeft.y + r.y + r.h), r.colour);
}

}  // namespace wax::app

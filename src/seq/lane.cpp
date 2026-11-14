#include "seq/lane.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace wax {
namespace {

using Row = std::vector<std::string>;   // one cell text per column

struct ParsedPattern {
  std::vector<Row> rows;
};

std::string trim(const std::string& text) {
  const char* space = " \t\r\n";
  const std::size_t first = text.find_first_not_of(space);
  if (first == std::string::npos) return "";
  return text.substr(first, text.find_last_not_of(space) - first + 1);
}

// Pattern text: one row per line, cells separated by spaces, one cell per column. Missing cells at the end of a row
// are empty, so a pattern written before a column existed still loads.
ParsedPattern parsePattern(const Pattern& pattern, const std::vector<Column>& columns) {
  std::string text;
  for (std::size_t i = 0; i < pattern.rows.size(); i++) text += (i ? "\n" : "") + pattern.rows[i];
  text = trim(text);

  ParsedPattern parsed;
  std::istringstream lines(text);
  std::string line;
  int rowNumber = 0;
  do {
    std::getline(lines, line);
    Row cells;
    std::istringstream words(line);
    for (std::string cell; words >> cell;) cells.push_back(cell);
    if (cells.empty()) cells.push_back("");   // (as the page does: an empty line is one empty text, which is no cell)
    if (cells.size() > columns.size()) {
      throw std::runtime_error("pattern row " + std::to_string(rowNumber) + ": " + std::to_string(cells.size()) + " cells but only " + std::to_string(columns.size()) + " columns");
    }
    while (cells.size() < columns.size()) cells.push_back(tokenBlank(columns[cells.size()].token));
    for (std::size_t c = 0; c < cells.size(); c++) encodeCell(cells[c], columns[c].token);   // validates
    parsed.rows.push_back(std::move(cells));
    rowNumber++;
  } while (!lines.eof());
  return parsed;
}

// A lane never has two patterns starting on the same tick: the first one in the list stays, the others are dropped.
std::vector<Placement> uniqueStarts(const std::vector<Placement>& placements) {
  std::set<int> seen;
  std::vector<Placement> kept;
  for (const Placement& p : placements) if (seen.insert(p.start).second) kept.push_back(p);
  return kept;
}

// Placements become one row of cells per tick. Patterns are opaque overlays: a placement that starts later
// overwrites the ticks it covers, and the pattern underneath carries on after it ends.
std::vector<const Row*> flattenLane(const Lane& lane, const std::map<std::string, ParsedPattern>& patterns, const Row& blank, int length) {
  std::vector<const Row*> rows(static_cast<std::size_t>(std::max(0, length)), &blank);
  std::vector<Placement> placements = uniqueStarts(lane.placements);
  std::stable_sort(placements.begin(), placements.end(), [](const Placement& a, const Placement& b) { return a.start < b.start; });
  for (const Placement& pl : placements) {
    const auto pattern = patterns.find(pl.pattern);
    if (pattern == patterns.end()) throw std::runtime_error("lane \"" + lane.id + "\": unknown pattern \"" + pl.pattern + "\"");
    const int n = std::min(pl.length, static_cast<int>(pattern->second.rows.size()));
    for (int t = 0; t < n && pl.start + t < length; t++) {
      if (pl.start + t >= 0) rows[static_cast<std::size_t>(pl.start + t)] = &pattern->second.rows[static_cast<std::size_t>(t)];
    }
  }
  return rows;
}

// Rows of cell text -> pages of floats. A placement that crosses a page boundary needs no special case:
// the tick index picks the page.
std::vector<std::vector<float>> buildPages(const std::vector<const Row*>& rows, const std::vector<Column>& columns, int channels) {
  const std::size_t count = std::max<std::size_t>(1, (rows.size() + kPageTicks - 1) / kPageTicks);
  std::vector<std::vector<float>> pages(count, std::vector<float>(static_cast<std::size_t>(kPageTicks * channels), kEmpty));
  for (std::size_t tick = 0; tick < rows.size(); tick++) {
    std::vector<float>& page = pages[tick / kPageTicks];
    const std::size_t base = (tick % kPageTicks) * static_cast<std::size_t>(channels);
    for (std::size_t c = 0; c < columns.size(); c++) {
      const std::vector<float> values = encodeCell((*rows[tick])[c], columns[c].token);
      for (std::size_t i = 0; i < values.size(); i++) page[base + static_cast<std::size_t>(columns[c].ch) + i] = values[i];
    }
  }
  return pages;
}

// A lane's mutes are the ticks where it flips between playing and muted: the first flip mutes it, the next unmutes
// it, and so on. A lane can also be silenced as a whole, which wins over the flips.
std::vector<std::uint8_t> buildMask(const Lane& lane, int length) {
  std::vector<std::uint8_t> mask(static_cast<std::size_t>(std::max(1, length)), 0);
  if (lane.silenced) {
    std::fill(mask.begin(), mask.end(), std::uint8_t{2});
  } else {
    std::vector<int> flips;
    for (const int t : lane.mutes) if (t < length) flips.push_back(t);
    std::sort(flips.begin(), flips.end());
    for (std::size_t i = 0; i < flips.size(); i += 2) {
      const int from = std::max(0, flips[i]), to = i + 1 < flips.size() ? std::min(length, flips[i + 1]) : length;
      for (int t = from; t < to; t++) mask[static_cast<std::size_t>(t)] = 1;
    }
  }
  for (const int t : lane.offs) if (t >= 0 && t < length) mask[static_cast<std::size_t>(t)] |= 4;
  return mask;
}

}  // namespace

std::vector<PreparedLane> prepareSong(const Project& project, const ParamRegistry& registry) {
  std::vector<PreparedLane> prepared;
  for (const Lane& source : project.lanes) {
    const Instrument& instrument = *project.findInstrument(source.instrument);
    PreparedLane lane;
    lane.id = source.id;
    lane.instrument = source.instrument;
    lane.columns = buildColumns(instrument, project, registry);
    lane.channels = channelCount(lane.columns);

    std::map<std::string, ParsedPattern> patterns;
    try {
      for (const Pattern& p : source.patterns) patterns[p.id] = parsePattern(p, lane.columns);
    } catch (const std::runtime_error& e) {
      throw std::runtime_error("lane \"" + source.id + "\": " + e.what());
    }
    Row blank;
    for (const Column& c : lane.columns) blank.push_back(tokenBlank(c.token));
    const std::vector<const Row*> rows = flattenLane(source, patterns, blank, project.song.length);
    lane.pages = buildPages(rows, lane.columns, lane.channels);
    lane.mask = buildMask(source, project.song.length);
    lane.curves = buildAutomation(source.raw.value("curves", Json::array()), lane.columns, lane.instrument, registry, project.song.length);
    prepared.push_back(std::move(lane));
  }
  return prepared;
}

}  // namespace wax

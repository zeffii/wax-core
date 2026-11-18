#include <cmath>
#include "edit/cell_editor.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <cstdio>
#include <sstream>

#include "seq/cells.h"
#include "seq/lane.h"

namespace wax {
namespace {

const char* const kNoteNames[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};

std::string noteText(int midi) {
  char text[8];
  std::snprintf(text, sizeof text, "%s%d", kNoteNames[midi % 12], midi / 12 - 1);
  return text;
}

std::string hexText(int value, int digits) {
  static const char* const hex = "0123456789ABCDEF";
  std::string text(static_cast<std::size_t>(digits), '0');
  for (int i = digits - 1; i >= 0; i--) { text[static_cast<std::size_t>(i)] = hex[value & 15]; value >>= 4; }
  return text;
}

int cellWidth(Token token) {
  switch (token) {
    case Token::Hex1: return 1;
    case Token::Note: return 3;
    case Token::Hex2: return 2;
    case Token::Hex4: return 4;
    case Token::Cmd: return 6;
    case Token::Spacer: return 1;
  }
  return 1;
}

}  // namespace

std::string CellEditor::rowWithCell(const std::string& row, const std::vector<Column>& columns, int cell, const std::string& text) {
  std::vector<std::string> cells;
  std::istringstream words(row);
  for (std::string word; words >> word;) cells.push_back(word);
  while (cells.size() < columns.size()) cells.push_back(tokenBlank(columns[cells.size()].token));
  cells[static_cast<std::size_t>(cell)] = text;
  std::string out;
  for (std::size_t i = 0; i < cells.size(); i++) out += (i ? " " : "") + cells[i];
  return out;
}

void CellEditor::setLane(const std::string& laneId, const std::vector<Column>& columns) {
  if (laneId != lane_) { tick_ = 0; cell_ = 0; typed_ = 0; nugget_ = -1; }
  lane_ = laneId;
  columns_ = columns;
  cell_ = std::min(cell_, std::max(0, static_cast<int>(columns_.size()) - 1));
}

int CellEditor::firstTick() const { return solo() ? soloLo_ : 0; }
int CellEditor::lastTick() const { return solo() ? std::min(soloHi_, std::max(1, document_.project().song.length)) - 1 : std::max(1, document_.project().song.length) - 1; }

void CellEditor::moveTo(int tick, int cell) {
  selecting_ = false;
  tick_ = std::clamp(tick, firstTick(), std::max(firstTick(), lastTick()));
  const int before = cell_;
  cell_ = std::clamp(cell, 0, std::max(0, static_cast<int>(columns_.size()) - 1));
  if (cell_ != before) nugget_ = -1;   // another column ends nugget mode
  typed_ = 0;
}

void CellEditor::move(int rows, int cells) {
  int cell = cell_;
  const int count = static_cast<int>(columns_.size());
  // spacers are skipped over
  for (int step = cells > 0 ? 1 : -1, left = std::abs(cells); left > 0; left--) {
    int next = cell + step;
    while (next >= 0 && next < count && columns_[static_cast<std::size_t>(next)].kind == ColumnKind::Spacer) next += step;
    if (next < 0 || next >= count) break;
    cell = next;
  }
  int tick = tick_ + rows;
  if (wrapRows_) {
    const int length = std::max(1, lastTick() - firstTick() + 1);
    tick = firstTick() + (((tick - firstTick()) % length) + length) % length;
  }
  moveTo(tick, cell);
}

bool CellEditor::onNoteCell() const {
  return cell_ < static_cast<int>(columns_.size()) && columns_[static_cast<std::size_t>(cell_)].token == Token::Note;
}

bool CellEditor::onHexCell() const {
  if (cell_ >= static_cast<int>(columns_.size())) return false;
  const Token t = columns_[static_cast<std::size_t>(cell_)].token;
  return t == Token::Hex1 || t == Token::Hex2 || t == Token::Hex4 || t == Token::Cmd;
}

CellEditor::Place CellEditor::placeAt(int tick) const {
  Place found;
  for (const Lane& lane : document_.project().lanes) {
    if (lane.id != lane_) continue;
    const std::vector<RowOwner> owners = rowOwners(lane, document_.project().song.length);
    if (tick < 0 || tick >= static_cast<int>(owners.size())) return found;
    const RowOwner owner = owners[static_cast<std::size_t>(tick)];
    if (owner.pattern < 0) return found;
    found.lane = &lane;
    found.pattern = &lane.patterns[static_cast<std::size_t>(owner.pattern)];
    found.row = owner.row;
  }
  return found;
}

bool CellEditor::onPattern() const { return place().pattern != nullptr; }

bool CellEditor::placePatternHere(const std::string& patternId) {
  if (solo()) return false;
  const Project& project = document_.project();
  for (const Lane& lane : project.lanes) {
    if (lane.id != lane_) continue;
    for (const Pattern& pattern : lane.patterns) {
      if (pattern.id != patternId) continue;
      const int length = std::min(static_cast<int>(pattern.rows.size()), project.song.length - tick_);
      if (length < 1) return false;
      document_.apply(std::make_unique<PlacePattern>(lane_, patternId, tick_, length));
      return true;
    }
  }
  return false;
}

std::string CellEditor::blankRowText() const {
  std::string blank;
  for (std::size_t i = 0; i < columns_.size(); i++) blank += (i ? " " : "") + tokenBlank(columns_[i].token);
  return blank;
}

bool CellEditor::createPatternHere() {
  if (solo()) return false;
  const Project& project = document_.project();
  const Lane* lane = nullptr;
  for (const Lane& l : project.lanes) if (l.id == lane_) lane = &l;
  if (!lane || columns_.empty()) return false;
  const int start = tick_ / kNewPatternRows * kNewPatternRows;
  const int length = std::min(kNewPatternRows, project.song.length - start);
  if (length < 1) return false;
  for (const Placement& p : lane->placements) if (p.start == start) return false;   // one already starts there
  const std::vector<RowOwner> owners = rowOwners(*lane, project.song.length);
  if (owners[static_cast<std::size_t>(start)].pattern >= 0) return false;            // one plays there
  std::set<int> used;
  for (const Pattern& p : lane->patterns) used.insert(p.index);
  int index = 0;
  while (used.count(index)) index++;
  if (index > 255) return false;
  static int counter = 0;
  Pattern pattern;
  pattern.id = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) + "-" + std::to_string(counter++);
  pattern.index = index;
  pattern.name = "pat" + hexText(index, 2);
  std::string blank;
  for (std::size_t i = 0; i < columns_.size(); i++) blank += (i ? " " : "") + tokenBlank(columns_[i].token);
  pattern.rows.assign(static_cast<std::size_t>(kNewPatternRows), blank);
  Placement placement;
  placement.pattern = pattern.id;
  placement.start = start;
  placement.length = length;
  document_.apply(std::make_unique<AddPattern>(lane_, pattern, placement));
  return true;
}

bool CellEditor::setCell(const std::string& text, bool foldIntoPrevious) {
  Place at = place();
  if (cell_ >= static_cast<int>(columns_.size())) return false;
  bool created = false;
  if (!at.pattern) {
    if (!createPatternHere()) return false;
    created = true;
    at = place();
    if (!at.pattern) return false;
  }
  foldIntoPrevious = foldIntoPrevious || created;
  const std::string& row = at.pattern->rows[static_cast<std::size_t>(at.row)];
  const std::string updated = rowWithCell(row, columns_, cell_, text);
  document_.apply(std::make_unique<SetRow>(lane_, at.pattern->id, at.row, updated), foldIntoPrevious);
  return true;
}

void CellEditor::advance() {
  typed_ = 0;
  moveTo(tick_ + step_, cell_);
}

bool CellEditor::typeNote(int midi) {
  if (cell_ >= static_cast<int>(columns_.size()) || midi < 0 || midi > 127) return false;
  const Column& column = columns_[static_cast<std::size_t>(cell_)];
  if (column.kind != ColumnKind::Note) return false;
  std::string text;
  if (column.token == Token::Note) text = noteText(midi);
  else if (column.rel) text = hexText(std::clamp(0x80 + midi - column.root, 1, 255), 2);
  else return false;
  if (!setCell(text, false)) return false;
  advance();
  return true;
}

bool CellEditor::nuggetStep(int d) {
  if (cell_ >= static_cast<int>(columns_.size()) || columns_[static_cast<std::size_t>(cell_)].kind == ColumnKind::Spacer) { nugget_ = -1; return false; }
  const int width = cellWidth(columns_[static_cast<std::size_t>(cell_)].token);
  if (nugget_ < 0) {
    if (width < 2) return false;
    nugget_ = d > 0 ? 1 : width - 1;
    typed_ = 0;
    return true;
  }
  const int to = nugget_ + d;
  if (to >= 0 && to < width) { nugget_ = to; return true; }
  nugget_ = -1;
  return false;
}

namespace {

// The note a text like "C#4" is, or -1 when it is not one (E# and B# do not exist; the octave is one digit).
int noteOfText(const std::string& text) {
  if (text.size() != 3 || text[2] < '0' || text[2] > '9') return -1;
  for (int i = 0; i < 12; i++) if (text.compare(0, 2, kNoteNames[i]) == 0) return (text[2] - '0' + 1) * 12 + i;
  return -1;
}

}  // namespace

bool CellEditor::typeNuggetChar(char c) {
  if (nugget_ < 0 || cell_ >= static_cast<int>(columns_.size())) return false;
  const Column& column = columns_[static_cast<std::size_t>(cell_)];
  const int width = cellWidth(column.token);
  const Place at = place();
  if (!at.pattern || nugget_ >= width) return false;
  const std::vector<std::string> cells = cellsOf(at.pattern->rows[static_cast<std::size_t>(at.row)]);
  std::string text = cells[static_cast<std::size_t>(cell_)];
  if (column.token == Token::Note) {
    if (noteOfText(text) < 0) return false;   // only a filled note can be edited by character
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (nugget_ == 0 && upper >= 'A' && upper <= 'G') text[0] = upper;
    else if (nugget_ == 1 && (c == '-' || c == '#')) text[1] = c;
    else if (nugget_ == 2 && c >= '0' && c <= '9') text[2] = c;
    else return false;
    if (noteOfText(text) < 0 || noteOfText(text) > 127) return false;
  } else if (column.kind == ColumnKind::Spacer || column.token == Token::Spacer || !std::isxdigit(static_cast<unsigned char>(c))) {
    return false;
  } else {
    if (text == tokenBlank(column.token)) text.assign(static_cast<std::size_t>(width), '0');   // an empty cell becomes zeros with this digit in place
    text[static_cast<std::size_t>(nugget_)] = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  if (!setCell(text, false)) return false;
  if (nugget_ + 1 < width) nugget_++;
  else moveTo(tick_ + step_, cell_);   // the last character: down a row, on the same character, still in the mode
  return true;
}

bool CellEditor::typeNoteOff() {
  if (cell_ >= static_cast<int>(columns_.size()) || columns_[static_cast<std::size_t>(cell_)].kind != ColumnKind::Note) return false;
  const bool relative = columns_[static_cast<std::size_t>(cell_)].token != Token::Note;
  if (!setCell(relative ? "00" : "===", false)) return false;
  advance();
  return true;
}

bool CellEditor::typeNoteCut() {
  if (!onNoteCell()) return false;
  if (!setCell("^^^", false)) return false;
  advance();
  return true;
}

bool CellEditor::typeHexDigit(int digit) {
  if (!onHexCell() || digit < 0 || digit > 15) return false;
  const int width = cellWidth(columns_[static_cast<std::size_t>(cell_)].token);
  static const char* const hex = "0123456789ABCDEF";
  // the cell holds the digits typed so far, filled out with zeros; they all land in one undo step
  std::string text;
  if (typed_ > 0) {
    const Place at = place();
    if (!at.pattern) return false;
    std::vector<std::string> cells;
    std::istringstream words(at.pattern->rows[static_cast<std::size_t>(at.row)]);
    for (std::string word; words >> word;) cells.push_back(word);
    if (cell_ < static_cast<int>(cells.size())) text = cells[static_cast<std::size_t>(cell_)].substr(0, static_cast<std::size_t>(typed_));
  }
  text += hex[digit];
  const int now = static_cast<int>(text.size());
  text.resize(static_cast<std::size_t>(width), '0');
  if (!setCell(text, typed_ > 0)) return false;
  typed_ = now;
  if (typed_ >= width) advance();
  return true;
}

std::string CellEditor::liveCellText(const Column& column, double value) {
  const double span = column.max - column.min;
  double x = 0;
  if (column.isInt) x = value - column.min;
  else if (column.log && column.min > 0 && column.max > column.min && value > 0) x = std::log(value / column.min) / std::log(column.max / column.min) * column.denom;
  else if (span != 0) x = (value - column.min) / span * column.denom;
  return hexText(static_cast<int>(std::clamp(std::lround(x), 0L, static_cast<long>(column.denom))), cellWidth(column.token));
}

bool CellEditor::writeLive(const std::function<std::optional<double>(const Column&)>& live, bool whole) {
  bool wrote = false;
  const int first = whole ? 0 : cell_, last = whole ? static_cast<int>(columns_.size()) - 1 : cell_;
  for (int c = first; c <= last && c < static_cast<int>(columns_.size()); c++) {
    const Column& column = columns_[static_cast<std::size_t>(c)];
    if (column.kind != ColumnKind::Param) continue;
    const std::optional<double> value = live(column);
    if (!value) continue;
    const int was = cell_;
    cell_ = c;
    const bool done = setCell(liveCellText(column, *value), wrote);   // later cells fold into the first one's undo step
    cell_ = was;
    wrote = wrote || done;
  }
  if (wrote) advance();
  return wrote;
}

bool CellEditor::clearCell(bool doAdvance) {
  if (cell_ >= static_cast<int>(columns_.size()) || columns_[static_cast<std::size_t>(cell_)].kind == ColumnKind::Spacer) return false;
  if (!setCell(tokenBlank(columns_[static_cast<std::size_t>(cell_)].token), false)) return false;
  if (doAdvance) advance(); else typed_ = 0;
  return true;
}

bool CellEditor::insertRow() {
  const Place at = place();
  if (!at.pattern) return false;
  std::string blank;
  for (std::size_t i = 0; i < columns_.size(); i++) blank += (i ? " " : "") + tokenBlank(columns_[i].token);
  document_.apply(std::make_unique<InsertRow>(lane_, at.pattern->id, at.row, blank));
  typed_ = 0;
  return true;
}

bool CellEditor::deleteRow() {
  const Place at = place();
  if (!at.pattern) return false;
  std::string blank;
  for (std::size_t i = 0; i < columns_.size(); i++) blank += (i ? " " : "") + tokenBlank(columns_[i].token);
  document_.apply(std::make_unique<DeleteRow>(lane_, at.pattern->id, at.row, blank));
  typed_ = 0;
  return true;
}

}  // namespace wax

namespace wax {

std::vector<std::string> CellEditor::cellsOf(const std::string& row) const {
  std::vector<std::string> cells;
  std::istringstream words(row);
  for (std::string word; words >> word;) cells.push_back(word);
  while (cells.size() < columns_.size()) cells.push_back(tokenBlank(columns_[cells.size()].token));
  return cells;
}

CellEditor::Selection CellEditor::selection() const {
  Selection s;
  if (!selecting_) return s;
  s.active = true;
  s.tick0 = std::min(anchorTick_, tick_); s.tick1 = std::max(anchorTick_, tick_);
  s.cell0 = std::min(anchorCell_, cell_); s.cell1 = std::max(anchorCell_, cell_);
  return s;
}

void CellEditor::extend(int rows, int cells) {
  const bool was = selecting_;
  const int fromTick = tick_, fromCell = cell_;
  move(rows, cells);
  selecting_ = true;
  if (!was) { anchorTick_ = fromTick; anchorCell_ = fromCell; }
}

void CellEditor::selectAll() {
  anchorTick_ = firstTick(); anchorCell_ = 0;
  tick_ = lastTick();
  cell_ = std::max(0, static_cast<int>(columns_.size()) - 1);
  selecting_ = true;
}

std::string CellEditor::copy() {
  Selection s = selection();
  if (!s.active) { s.tick0 = s.tick1 = tick_; s.cell0 = s.cell1 = cell_; }
  clipboard_.clear();
  std::string text;
  for (int t = s.tick0; t <= s.tick1; t++) {
    const Place at = placeAt(t);
    std::vector<std::string> cells = cellsOf(at.pattern ? at.pattern->rows[static_cast<std::size_t>(at.row)] : std::string());
    std::vector<std::string> line;
    for (int c = s.cell0; c <= s.cell1 && c < static_cast<int>(cells.size()); c++) line.push_back(cells[static_cast<std::size_t>(c)]);
    for (std::size_t i = 0; i < line.size(); i++) text += (i ? "\t" : "") + line[i];
    text += "\n";
    clipboard_.push_back(std::move(line));
  }
  return text;
}

bool CellEditor::cut() {
  Selection s = selection();
  if (!s.active) { s.tick0 = s.tick1 = tick_; s.cell0 = s.cell1 = cell_; }
  copy();
  std::vector<std::unique_ptr<Edit>> parts;
  for (int t = s.tick0; t <= s.tick1; t++) {
    const Place at = placeAt(t);
    if (!at.pattern) continue;
    std::string row = at.pattern->rows[static_cast<std::size_t>(at.row)];
    for (int c = s.cell0; c <= s.cell1 && c < static_cast<int>(columns_.size()); c++) {
      if (columns_[static_cast<std::size_t>(c)].kind == ColumnKind::Spacer) continue;
      row = rowWithCell(row, columns_, c, tokenBlank(columns_[static_cast<std::size_t>(c)].token));
    }
    parts.push_back(std::make_unique<SetRow>(lane_, at.pattern->id, at.row, row));
  }
  selecting_ = false;
  if (parts.empty()) return false;
  document_.apply(std::make_unique<Batch>(lane_, "Cut", std::move(parts)));
  return true;
}

bool CellEditor::paste() {
  std::vector<std::unique_ptr<Edit>> parts;
  for (std::size_t i = 0; i < clipboard_.size(); i++) {
    if (solo() && tick_ + static_cast<int>(i) > lastTick()) break;   // (nothing outside the pattern is pasted into)
    const Place at = placeAt(tick_ + static_cast<int>(i));
    if (!at.pattern) continue;
    std::string row = at.pattern->rows[static_cast<std::size_t>(at.row)];
    for (std::size_t j = 0; j < clipboard_[i].size(); j++) {
      const int c = cell_ + static_cast<int>(j);
      if (c >= static_cast<int>(columns_.size())) break;
      if (columns_[static_cast<std::size_t>(c)].kind == ColumnKind::Spacer) continue;
      row = rowWithCell(row, columns_, c, clipboard_[i][j]);
    }
    parts.push_back(std::make_unique<SetRow>(lane_, at.pattern->id, at.row, row));
  }
  selecting_ = false;
  if (parts.empty()) return false;
  document_.apply(std::make_unique<Batch>(lane_, "Paste", std::move(parts)));
  return true;
}

bool CellEditor::paste(const std::string& text) {
  std::vector<std::vector<std::string>> rows;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    std::vector<std::string> cells;
    std::istringstream words(line);
    for (std::string cell; std::getline(words, cell, '\t');) cells.push_back(cell);
    if (!cells.empty()) rows.push_back(std::move(cells));
  }
  if (rows.empty()) return false;
  clipboard_ = std::move(rows);
  return paste();
}

}  // namespace wax

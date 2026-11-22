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
  if (laneId != lane_) { tick_ = 0; cell_ = 0; typed_ = 0; char_ = 0; }
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
  if (cell_ != before) char_ = 0;   // another column: its first character
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

bool CellEditor::moveChar(int d) {
  const int count = static_cast<int>(columns_.size());
  if (cell_ >= count) return false;
  const int to = char_ + d;
  if (to >= 0 && to < cellWidth(columns_[static_cast<std::size_t>(cell_)].token)) { char_ = to; typed_ = 0; return true; }
  int next = cell_ + d;
  while (next >= 0 && next < count && columns_[static_cast<std::size_t>(next)].kind == ColumnKind::Spacer) next += d;
  if (next < 0 || next >= count) return false;
  selecting_ = false;
  cell_ = next;
  typed_ = 0;
  char_ = d > 0 ? 0 : cellWidth(columns_[static_cast<std::size_t>(cell_)].token) - 1;
  return true;
}

void CellEditor::lastCharacter() {
  if (cell_ < static_cast<int>(columns_.size())) char_ = std::max(0, cellWidth(columns_[static_cast<std::size_t>(cell_)].token) - 1);
}

namespace {

// The note a text like "C#4" is, or -1 when it is not one (E# and B# do not exist; the octave is one digit).
int noteOfText(const std::string& text) {
  if (text.size() != 3 || text[2] < '0' || text[2] > '9') return -1;
  for (int i = 0; i < 12; i++) if (text.compare(0, 2, kNoteNames[i]) == 0) return (text[2] - '0' + 1) * 12 + i;
  return -1;
}

}  // namespace

bool CellEditor::typeCharacter(char c) {
  if (cell_ >= static_cast<int>(columns_.size())) return false;
  const Column& column = columns_[static_cast<std::size_t>(cell_)];
  const int width = cellWidth(column.token);
  if (char_ >= width || column.kind == ColumnKind::Spacer || column.token == Token::Spacer) return false;
  const Place at = place();
  std::string text = at.pattern ? cellsOf(at.pattern->rows[static_cast<std::size_t>(at.row)])[static_cast<std::size_t>(cell_)] : tokenBlank(column.token);
  if (column.token == Token::Note) {
    if (char_ == 0 || !at.pattern || noteOfText(text) < 0) return false;   // the first character takes whole notes; the others edit a note that is there
    if (char_ == 1 && (c == '-' || c == '#')) text[1] = c;
    else if (char_ == 2 && c >= '0' && c <= '9') text[2] = c;
    else return false;
    if (noteOfText(text) < 0 || noteOfText(text) > 127) return false;
  } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
    return false;
  } else {
    if (text == tokenBlank(column.token)) text.assign(static_cast<std::size_t>(width), '0');   // an empty cell becomes zeros with this digit in place
    text[static_cast<std::size_t>(char_)] = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  if (!setCell(text, column.token != Token::Note && typed_ > 0)) return false;   // (the digits of one cell are one undo step)
  typed_++;
  if (char_ + 1 < width) char_++;
  else moveTo(tick_ + step_, cell_);   // the last character: down a row, on the same character
  return true;
}

bool CellEditor::typeNoteOff() {
  if (cell_ >= static_cast<int>(columns_.size()) || columns_[static_cast<std::size_t>(cell_)].kind != ColumnKind::Note) return false;
  const bool relative = columns_[static_cast<std::size_t>(cell_)].token != Token::Note;
  if (!setCell(relative ? "00" : "===", false)) return false;
  advance();
  return true;
}

bool CellEditor::typeNoteOffRow() {
  // a note off in every absolute note column of this row (relative columns are left alone, as the web's Shift+1 does); one change, one undo step
  Place at = place();
  bool created = false;
  if (!at.pattern) {
    if (!createPatternHere()) return false;
    created = true;
    at = place();
    if (!at.pattern) return false;
  }
  std::string updated = at.pattern->rows[static_cast<std::size_t>(at.row)];
  int count = 0;
  for (int i = 0; i < static_cast<int>(columns_.size()); i++) {
    const Column& column = columns_[static_cast<std::size_t>(i)];
    if (column.kind != ColumnKind::Note || column.token != Token::Note) continue;
    updated = rowWithCell(updated, columns_, i, "===");
    count++;
  }
  if (count == 0) return false;
  document_.apply(std::make_unique<SetRow>(lane_, at.pattern->id, at.row, updated), created);
  noteOffColumns_ = count;
  char_ = 0;
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
  return typeCharacter("0123456789ABCDEF"[digit]);
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

bool CellEditor::shiftSelection(Shift how) {
  const Selection sel = selection();
  const int t0 = sel.active ? sel.tick0 : tick_, t1 = sel.active ? sel.tick1 : tick_, c0 = sel.active ? sel.cell0 : cell_, c1 = sel.active ? sel.cell1 : cell_;
  const Lane* lane = nullptr;
  for (const Lane& l : document_.project().lanes) if (l.id == lane_) lane = &l;
  if (!lane) return false;
  const std::vector<RowOwner> owners = rowOwners(*lane, document_.project().song.length);
  const int lo = firstTick(), hi = std::min(lastTick() + 1, static_cast<int>(owners.size()));
  const auto owned = [&](int t) { return t >= lo && t < hi && owners[static_cast<std::size_t>(t)].pattern >= 0; };
  std::vector<std::unique_ptr<Edit>> parts;
  for (int t = std::max(t0, lo); t <= t1 && t < hi;) {
    if (!owned(t)) { t++; continue; }
    // the selected rows a..b that lie in one pattern, and where that pattern ends (e, excluded)
    const int pattern = owners[static_cast<std::size_t>(t)].pattern, a = t;
    int b = a;
    while (b + 1 <= t1 && owned(b + 1) && owners[static_cast<std::size_t>(b + 1)].pattern == pattern) b++;
    int e = b + 1;
    while (owned(e) && owners[static_cast<std::size_t>(e)].pattern == pattern) e++;
    t = b + 1;
    const int h = b - a + 1;
    const bool rotate = how == Shift::RotateUp || how == Shift::RotateDown;
    if (rotate && h < 2) continue;
    const Pattern& source = lane->patterns[static_cast<std::size_t>(pattern)];
    const int span = rotate ? b + 1 : e;   // the rows that change: the selection itself when rotating, else down to the pattern's end
    std::vector<std::vector<std::string>> before;   // the cells of the rows a..e-1 as they are now
    for (int r = a; r < e; r++) before.push_back(cellsOf(source.rows[static_cast<std::size_t>(owners[static_cast<std::size_t>(r)].row)]));
    for (int r = a; r < span; r++) {
      std::string row = source.rows[static_cast<std::size_t>(owners[static_cast<std::size_t>(r)].row)];
      bool changed = false;
      for (int c = c0; c <= c1 && c < static_cast<int>(columns_.size()); c++) {
        if (columns_[static_cast<std::size_t>(c)].kind == ColumnKind::Spacer) continue;
        int from = r;
        std::string text;
        switch (how) {
          case Shift::PullUp: from = r + h; break;
          case Shift::PushDown: from = r - h; break;
          case Shift::RotateUp: from = a + (r - a + 1) % h; break;
          case Shift::RotateDown: from = a + (r - a + h - 1) % h; break;
        }
        text = from >= a && from < e ? before[static_cast<std::size_t>(from - a)][static_cast<std::size_t>(c)] : tokenBlank(columns_[static_cast<std::size_t>(c)].token);
        if (text == before[static_cast<std::size_t>(r - a)][static_cast<std::size_t>(c)]) continue;
        row = rowWithCell(row, columns_, c, text);
        changed = true;
      }
      if (changed) parts.push_back(std::make_unique<SetRow>(lane_, source.id, owners[static_cast<std::size_t>(r)].row, row));
    }
  }
  if (parts.empty()) return false;
  const char* name = how == Shift::PullUp ? "Pull up" : how == Shift::PushDown ? "Push down" : "Rotate";
  document_.apply(std::make_unique<Batch>(lane_, name, std::move(parts)));
  typed_ = 0;
  return true;
}

bool CellEditor::transpose(bool notes, int direction, bool big) {
  const Selection sel = selection();
  const int t0 = sel.active ? sel.tick0 : tick_, t1 = sel.active ? sel.tick1 : tick_, c0 = sel.active ? sel.cell0 : cell_, c1 = sel.active ? sel.cell1 : cell_;
  const Lane* lane = nullptr;
  for (const Lane& l : document_.project().lanes) if (l.id == lane_) lane = &l;
  if (!lane) return false;
  const std::vector<RowOwner> owners = rowOwners(*lane, document_.project().song.length);
  std::vector<std::unique_ptr<Edit>> parts;
  for (int t = std::max(t0, firstTick()); t <= t1 && t <= lastTick() && t < static_cast<int>(owners.size()); t++) {
    const RowOwner owner = owners[static_cast<std::size_t>(t)];
    if (owner.pattern < 0) continue;
    const Pattern& pattern = lane->patterns[static_cast<std::size_t>(owner.pattern)];
    const std::vector<std::string> cells = cellsOf(pattern.rows[static_cast<std::size_t>(owner.row)]);
    std::string row = pattern.rows[static_cast<std::size_t>(owner.row)];
    bool changed = false;
    for (int c = c0; c <= c1 && c < static_cast<int>(columns_.size()); c++) {
      const Token token = columns_[static_cast<std::size_t>(c)].token;
      const std::string& cell = cells[static_cast<std::size_t>(c)];
      if (columns_[static_cast<std::size_t>(c)].kind == ColumnKind::Spacer || cell == tokenBlank(token)) continue;
      std::string next;
      if (notes) {
        const int midi = token == Token::Note ? noteOfText(cell) : -1;
        const int to = midi + direction * (big ? 12 : 1);
        if (midi < 0 || to < 0 || to > 127) continue;
        next = noteText(to);
      } else if (token == Token::Hex1 || token == Token::Hex2 || token == Token::Hex4 || token == Token::Cmd) {
        const int head = token == Token::Cmd ? 2 : 0, digits = cellWidth(token) - head;
        const long value = std::strtol(cell.substr(static_cast<std::size_t>(head)).c_str(), nullptr, 16), top = (1L << (4 * digits)) - 1;
        const long step = big ? (token == Token::Hex1 ? 1 : token == Token::Hex2 ? 16 : 256) : 1;
        const long moved = std::clamp(value + direction * step, 0L, top);
        if (moved == value) continue;
        next = cell.substr(0, static_cast<std::size_t>(head)) + hexText(static_cast<int>(moved), digits);
      } else {
        continue;
      }
      row = rowWithCell(row, columns_, c, next);
      changed = true;
    }
    if (changed) parts.push_back(std::make_unique<SetRow>(lane_, pattern.id, owner.row, row));
  }
  if (parts.empty()) return false;
  document_.apply(std::make_unique<Batch>(lane_, notes ? "Transpose notes" : "Transpose values", std::move(parts)));
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

void CellEditor::selectTo(int tick, int cell) {
  const bool was = selecting_;
  const int fromTick = tick_, fromCell = cell_;
  moveTo(tick, cell);
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

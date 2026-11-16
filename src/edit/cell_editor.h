// The cursor of the pattern editor and the rules for typing into cells. No window code in here: the window turns key presses
// into these calls, and every change goes through the Document, so it can be undone.
//
// A cell's kind decides what can be typed: a note cell takes notes (and the note-off and note-cut marks), a hex cell takes hex
// digits that fill it from the left (the cell reads as filled with zeros until the last digit is typed). Typing is an edit of the
// pattern the cursor's tick belongs to, so every place that pattern is used changes with it.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "edit/document.h"
#include "seq/columns.h"

namespace wax {

class CellEditor {
public:
  explicit CellEditor(Document& document) : document_(document) {}

  // The lane the cursor is in, and its columns (Engine::laneColumns). Moves the cursor to the top of the lane when the lane changes.
  void setLane(const std::string& laneId, const std::vector<Column>& columns);

  const std::string& lane() const { return lane_; }
  int tick() const { return tick_; }
  int cell() const { return cell_; }
  int digitsTyped() const { return typed_; }

  // Solo-edit: the cursor, selections and pastes stay on the rows lo to hi-1 (one pattern), and no pattern can be made or placed. clearSolo() ends it.
  void setSolo(int lo, int hi) { soloLo_ = std::max(0, lo); soloHi_ = std::max(soloLo_ + 1, hi); moveTo(tick_, cell_); }
  void clearSolo() { soloLo_ = soloHi_ = -1; }
  bool solo() const { return soloHi_ > 0; }

  // Where the cursor may go. Moving ends a half-typed cell.
  void moveTo(int tick, int cell);
  std::string blankRowText() const;   // a row of the lane's columns with every cell empty
  bool patternHere() const { return placeAt(tick_).pattern != nullptr; }   // a pattern plays at the cursor's row
  void move(int rows, int cells);
  // Up and Down wrap round the first and last row of the song (off: the cursor stops there).
  void setWrapRows(bool on) { wrapRows_ = on; }

  // A selection: a block of cells from where it was started to the cursor. Moving without extending drops it.
  struct Selection { bool active = false; int tick0 = 0, tick1 = 0, cell0 = 0, cell1 = 0; };
  Selection selection() const;
  void extend(int rows, int cells);   // moves the cursor and keeps (or starts) the selection
  void clearSelection() { selecting_ = false; }
  void selectAll();                   // the whole lane
  // Copy: the cells of the selection (or the cursor's cell) as text, rows on lines and cells split by tabs; also kept for paste.
  std::string copy();
  bool cut();                         // copy, then blank the cells
  bool paste();                       // the kept cells, with their top left at the cursor (rows where no pattern plays are skipped)
  bool paste(const std::string& text);   // text as copy() makes it
  bool hasClipboard() const { return !clipboard_.empty(); }

  // Rows the cursor moves down after a cell is filled (0 stays where it is).
  void setStep(int rows) { step_ = rows < 0 ? 0 : rows; }
  int step() const { return step_; }

  // What the cursor's cell takes. Spacers take nothing.
  bool onNoteCell() const;
  bool onHexCell() const;
  bool onPattern() const;   // a pattern is under the cursor's tick
  // Typing where no pattern plays makes one: 16 rows, starting at the multiple of 16 at or just below the cursor, when nothing
  // plays or starts there. (Anything else: the typing does nothing.)
  static constexpr int kNewPatternRows = 16;

  // Typing. Each returns false when nothing was changed (no pattern here, or the cell does not take it).
  bool typeNote(int midi);          // a note cell (or a relative one, from its root)
  bool typeNoteOff();               // "==="  (a relative cell: 00)
  bool typeNoteCut();               // "^^^"
  bool typeHexDigit(int digit);     // 0..15
  // A parameter's live value written back as the cell that would give it (the inverse of how the sequencer reads a cell), then down a row.
  // `live` gives a column's current value (nullopt: none). The cursor's cell only, or (whole: Shift+Space) every parameter column of the row, as one undo step.
  bool writeLive(const std::function<std::optional<double>(const Column&)>& live, bool whole);
  // The hex cell text of a value for a parameter column.
  static std::string liveCellText(const Column& column, double value);
  bool clearCell(bool advance);
  bool insertRow();                 // a blank row at the cursor's row: the rows below move down, the last one falls off
  bool deleteRow();                 // the cursor's row goes: the rows below move up

  // The text a cell would hold with a row's cells set and padded with blanks; what typing writes into the pattern.
  static std::string rowWithCell(const std::string& row, const std::vector<Column>& columns, int cell, const std::string& text);

  // Puts the lane's pattern `patternId` at the cursor row (what plays under it is covered); false when there is no room or no such pattern.
  bool placePatternHere(const std::string& patternId);
  // A new blank pattern at the cursor row (rounded down to a multiple of the new-pattern length); false when one starts or plays there.
  bool createPatternHere();

private:
  Document& document_;
  std::string lane_;
  std::vector<Column> columns_;
  int tick_ = 0, cell_ = 0, typed_ = 0, step_ = 1;
  bool selecting_ = false, wrapRows_ = false;
  int soloLo_ = -1, soloHi_ = -1;   // the rows of the solo-edited pattern (hi is one past its last); -1: not solo-editing
  int firstTick() const;   // the first and last row the cursor may be on
  int lastTick() const;
  int anchorTick_ = 0, anchorCell_ = 0;
  std::vector<std::vector<std::string>> clipboard_;   // rows of cell texts

  struct Place {
    const Lane* lane = nullptr;
    const Pattern* pattern = nullptr;
    int row = 0;
  };
  Place place() const { return placeAt(tick_); }
  Place placeAt(int tick) const;
  std::vector<std::string> cellsOf(const std::string& row) const;   // a row's cell texts, padded with blanks to the columns
  bool setCell(const std::string& text, bool foldIntoPrevious);
  void advance();
};

}  // namespace wax

// The cursor of the pattern editor and the rules for typing into cells. No window code in here: the window turns key presses
// into these calls, and every change goes through the Document, so it can be undone.
//
// A cell's kind decides what can be typed: a note cell takes notes (and the note-off and note-cut marks), a hex cell takes hex
// digits that fill it from the left (the cell reads as filled with zeros until the last digit is typed). Typing is an edit of the
// pattern the cursor's tick belongs to, so every place that pattern is used changes with it.
#pragma once

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

  // Where the cursor may go. Moving ends a half-typed cell.
  void moveTo(int tick, int cell);
  void move(int rows, int cells);

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
  bool clearCell(bool advance);
  bool insertRow();                 // a blank row at the cursor's row: the rows below move down, the last one falls off
  bool deleteRow();                 // the cursor's row goes: the rows below move up

  // The text a cell would hold with a row's cells set and padded with blanks; what typing writes into the pattern.
  static std::string rowWithCell(const std::string& row, const std::vector<Column>& columns, int cell, const std::string& text);

private:
  Document& document_;
  std::string lane_;
  std::vector<Column> columns_;
  int tick_ = 0, cell_ = 0, typed_ = 0, step_ = 1;

  struct Place {
    const Lane* lane = nullptr;
    const Pattern* pattern = nullptr;
    int row = 0;
  };
  Place place() const;
  bool createPatternHere();
  bool setCell(const std::string& text, bool foldIntoPrevious);
  void advance();
};

}  // namespace wax

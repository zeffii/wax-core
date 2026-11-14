// The song as the window edits it. One Document owns the project that is being changed; every change is an Edit that
// can be applied and taken back, so undo and redo work for every kind of edit, now and for the ones added later.
// The audio side never reads the project directly: it is told which lanes changed (see takeChangedLanes).
#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "project/project.h"

namespace wax {

// One reversible change. apply() then revert() must leave the project exactly as it was.
class Edit {
public:
  virtual ~Edit() = default;
  virtual void apply(Project& project) = 0;
  virtual void revert(Project& project) = 0;
  virtual std::string label() const = 0;               // "Set cell", shown next to Undo in the window
  virtual std::string lane() const { return {}; }      // the lane whose sound this changes ("" when none)
  virtual std::string paramAddress() const { return {}; }   // "instrument.node.param" of the parameter this sets ("" when none)
  // Typing in one cell is many edits; the next one may fold into this one so a single undo takes back the whole entry.
  // Return true when `next` has been absorbed (and keeps what this edit needs to revert it).
  virtual bool absorb(const Edit& next) { (void)next; return false; }
};

// Replaces one row of a pattern (a line of text, as in the file).
class SetRow final : public Edit {
public:
  SetRow(std::string lane, std::string pattern, int row, std::string text);
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Edit row"; }
  std::string lane() const override { return lane_; }
  bool absorb(const Edit& next) override;
  const std::string& patternId() const { return pattern_; }
  int row() const { return row_; }
  const std::string& text() const { return text_; }

private:
  std::string lane_, pattern_;
  int row_;
  std::string text_, before_;
  bool applied_ = false;
};

// A node parameter set to a value (the value the instrument starts with, as saved in the song). While a knob is dragged
// each step folds into the one before, so a drag is a single undo.
class SetParam final : public Edit {
public:
  SetParam(std::string instrument, std::string node, std::string param, double value);
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Set " + param_; }
  std::string paramAddress() const override { return instrument_ + "." + node_ + "." + param_; }
  bool absorb(const Edit& next) override;

private:
  std::string instrument_, node_, param_;
  double value_;
  bool captured_ = false, had_ = false;
  Json before_;
};

// A new pattern and where it is placed. Typing into the first cell of a new pattern folds into this edit, so one undo takes
// the pattern and what was typed in it away together.
class AddPattern final : public Edit {
public:
  AddPattern(std::string lane, Pattern pattern, Placement placement);
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "New pattern"; }
  std::string lane() const override { return lane_; }
  bool absorb(const Edit& next) override;

private:
  std::string lane_;
  Pattern pattern_;
  Placement placement_;
};

// Puts blank text in a row's place and pushes the rows below it down one; the last row falls off the end (it comes back on undo).
class InsertRow final : public Edit {
public:
  InsertRow(std::string lane, std::string pattern, int row, std::string blank);
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Insert row"; }
  std::string lane() const override { return lane_; }

private:
  std::string lane_, pattern_;
  int row_;
  std::string blank_, dropped_;
};

// Removes a row and pulls the rows below it up one; the pattern keeps its length (blank text fills the end).
class DeleteRow final : public Edit {
public:
  DeleteRow(std::string lane, std::string pattern, int row, std::string blank);
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Delete row"; }
  std::string lane() const override { return lane_; }

private:
  std::string lane_, pattern_;
  int row_;
  std::string blank_, removed_;
};

// The text of one cell of a row replaced, the other cells and the spacing left as they are. Throws std::out_of_range for a cell the row does not have.
std::string withCell(const std::string& row, int cell, const std::string& text);

class Document {
public:
  explicit Document(Project project) : project_(std::move(project)) {}

  const Project& project() const { return project_; }

  // Applies a edit and remembers it for undo. A new edit ends the redo history. With `foldIntoPrevious`, the edit is
  // absorbed by the one before it when that one allows it (so a cell typed in several steps is one undo).
  void apply(std::unique_ptr<Edit> edit, bool foldIntoPrevious = false);

  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  std::string undoLabel() const { return undo_.empty() ? std::string() : undo_.back()->label(); }
  std::string redoLabel() const { return redo_.empty() ? std::string() : redo_.back()->label(); }
  void undo();
  void redo();

  // Counts every change, undo and redo included; the window redraws when it moves, the file is "unsaved" when it differs from savedAt.
  int revision() const { return revision_; }
  bool modified() const { return revision_ != savedRevision_; }
  void markSaved() { savedRevision_ = revision_; }

  // The lanes changed since the last call, for the engine to rebuild.
  std::set<std::string> takeChangedLanes();
  // The parameters changed since the last call, as "instrument.node.param" (undo and redo count).
  std::set<std::string> takeChangedParams();
  void markLaneChanged(const std::string& lane) { changed_.insert(lane); }   // to try again later

private:
  Project project_;
  std::vector<std::unique_ptr<Edit>> undo_, redo_;
  int revision_ = 0, savedRevision_ = 0;
  std::set<std::string> changed_, changedParams_;

  void touched(const Edit& edit);
};

}  // namespace wax

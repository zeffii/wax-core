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
  virtual std::string otherLane() const { return {}; }  // a second lane whose sound this changes (the old name of a renamed lane)
  virtual std::string lane() const { return {}; }      // the lane whose sound this changes ("" when none)
  virtual std::string paramAddress() const { return {}; }   // "instrument.node.param" of the parameter this sets ("" when none)
  virtual std::string graphInstrument() const { return {}; }   // the instrument whose wiring this changes ("" when none)
  virtual bool songLoop() const { return false; }              // true when this changes the song's loop
  virtual std::string dataNode() const { return {}; }      // "instrument\x1fnode" whose data (drawings) this changes ("" when none)
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

// A drawn envelope of a node, replaced as a whole (`points` is the array of anchors as the file holds it). While an anchor is dragged
// each step folds into the one before, so a drag is a single undo.
class SetDrawing final : public Edit {
public:
  SetDrawing(std::string instrument, std::string node, std::string name, Json points);
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Edit " + name_; }
  std::string dataNode() const override { return instrument_ + "\x1f" + node_; }
  bool absorb(const Edit& next) override;

private:
  std::string instrument_, node_, name_;
  Json points_;
  bool captured_ = false, hadData_ = false, hadEnv_ = false, hadDrawing_ = false;
  Json before_, beforeData_;
};

// A wire made from "node.port" to "node.port" (the same wire twice is one wire). Undo takes it away again.
class Connect final : public Edit {
public:
  Connect(std::string instrument, std::string from, std::string to) : instrument_(std::move(instrument)), from_(std::move(from)), to_(std::move(to)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Connect " + from_ + " to " + to_; }
  std::string graphInstrument() const override { return instrument_; }

private:
  std::string instrument_, from_, to_;
  bool added_ = false;
};

// A wire cut.
class Disconnect final : public Edit {
public:
  Disconnect(std::string instrument, std::string from, std::string to) : instrument_(std::move(instrument)), from_(std::move(from)), to_(std::move(to)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Cut " + from_ + " to " + to_; }
  std::string graphInstrument() const override { return instrument_; }

private:
  std::string instrument_, from_, to_;
  std::size_t at_ = 0;
  bool removed_ = false;
};

// The song's loop set to a value (null: none). The caller works it out with loopWithPoint / loopToggled.
class SetLoop final : public Edit {
public:
  SetLoop(Json loop, std::string what) : loop_(std::move(loop)), what_(std::move(what)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return what_; }
  bool songLoop() const override { return true; }

private:
  Json loop_, before_;
  std::string what_;
  bool captured_ = false;
};

// Several edits that are one step to undo (a paste, a cut). They must all change the same lane.
class Batch final : public Edit {
public:
  Batch(std::string lane, std::string label, std::vector<std::unique_ptr<Edit>> parts) : lane_(std::move(lane)), label_(std::move(label)), parts_(std::move(parts)) {}
  void apply(Project& project) override { for (auto& part : parts_) part->apply(project); }
  void revert(Project& project) override { for (std::size_t i = parts_.size(); i-- > 0;) parts_[i]->revert(project); }
  std::string label() const override { return label_; }
  std::string lane() const override { return lane_; }

private:
  std::string lane_, label_;
  std::vector<std::unique_ptr<Edit>> parts_;
};

// Beats in a bar, the top number of the time signature (the arrangement's shading and bars follow it). A drag folds into one undo step.
class SetBeatsPerBar final : public Edit {
public:
  explicit SetBeatsPerBar(int value) : value_(value) {}
  void apply(Project& project) override { if (!captured_) { before_ = project.song.beatsPerBar; captured_ = true; } project.song.beatsPerBar = value_; }
  void revert(Project& project) override { project.song.beatsPerBar = before_; }
  std::string label() const override { return "Set beats per bar"; }
  bool absorb(const Edit& next) override {
    const auto* other = dynamic_cast<const SetBeatsPerBar*>(&next);
    if (!other) return false;
    value_ = other->value_;
    return true;
  }

private:
  int value_, before_ = 4;
  bool captured_ = false;
};

// A new node of a type, placed at x, y with its parameters at their defaults. Undo takes it out again.
class AddNode final : public Edit {
public:
  AddNode(std::string instrument, std::string type, std::string id, double x, double y)
      : instrument_(std::move(instrument)), type_(std::move(type)), id_(std::move(id)), x_(x), y_(y) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Add " + id_; }
  std::string graphInstrument() const override { return instrument_; }

private:
  std::string instrument_, type_, id_;
  double x_, y_;
};

// A node taken out with its wires; undo puts them back where they were.
class DeleteNode final : public Edit {
public:
  DeleteNode(std::string instrument, std::string node) : instrument_(std::move(instrument)), node_(std::move(node)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Delete " + node_; }
  std::string graphInstrument() const override { return instrument_; }

private:
  std::string instrument_, node_;
  GraphNode saved_;
  std::size_t nodeAt_ = 0;
  std::vector<std::pair<std::size_t, Connection>> wires_;
};

// Moves a node's tile in the graph view. A drag folds into one undo step. (Positions do not touch the sound.)
class MoveNode final : public Edit {
public:
  MoveNode(std::string instrument, std::string node, double x, double y);
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Move " + node_; }
  bool absorb(const Edit& next) override;

private:
  std::string instrument_, node_;
  double x_, y_;
  double beforeX_ = 0, beforeY_ = 0;
  bool beforePlaced_ = false, captured_ = false;
};

// A DX bank, replaced as a whole (the 4096 bytes of 32 packed voices, as the node's data.bank holds them). While a control is dragged each
// step folds into the one before, so a drag is a single undo.
class SetBank final : public Edit {
public:
  SetBank(std::string instrument, std::string node, std::vector<int> bytes, std::string what = "voice");
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Edit " + what_; }
  std::string dataNode() const override { return instrument_ + "\x1f" + node_; }
  bool absorb(const Edit& next) override;

private:
  std::string instrument_, node_, what_;
  std::vector<int> bytes_;
  bool captured_ = false;
  Json beforeData_;
};

// Silences a lane as a whole (the M button of the mixer), or lets it play again.
class SetSilenced final : public Edit {
public:
  SetSilenced(std::string lane, bool silenced) : lane_(std::move(lane)), silenced_(silenced) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return silenced_ ? "Silence " + lane_ : "Unsilence " + lane_; }
  std::string lane() const override { return lane_; }

private:
  std::string lane_;
  bool silenced_, before_ = false;
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

// A new instrument (a fresh machine of some kind) with a blank lane of its own at the end of the lanes. Undo takes both away again.
class AddInstrument final : public Edit {
public:
  AddInstrument(std::string instrument, Json definition, std::string lane) : instrument_(std::move(instrument)), definition_(std::move(definition)), lane_(std::move(lane)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Add " + instrument_; }
  std::string lane() const override { return lane_; }
  std::string graphInstrument() const override { return instrument_; }

private:
  std::string instrument_;
  Json definition_;
  std::string lane_;
};

// The lane under another name (its instrument and patterns stay). The engine's lane of the old name is replaced by an empty one.
class RenameLane final : public Edit {
public:
  RenameLane(std::string from, std::string to) : from_(std::move(from)), to_(std::move(to)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Rename lane " + from_; }
  std::string lane() const override { return to_; }
  std::string otherLane() const override { return from_; }

private:
  std::string from_, to_;
};

// A lane one place earlier (direction -1) or later (+1) among the lanes. The master lane stays first.
class MoveLane final : public Edit {
public:
  MoveLane(std::string lane, int direction) : lane_(std::move(lane)), direction_(direction) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Move lane " + lane_; }

private:
  std::string lane_;
  int direction_;
  int from_ = -1, to_ = -1;
};

// A copy of a lane (same instrument, patterns and placements) right after it, under a new name.
class DuplicateLane final : public Edit {
public:
  DuplicateLane(std::string source, std::string copy) : source_(std::move(source)), copy_(std::move(copy)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Duplicate lane " + source_; }
  std::string lane() const override { return copy_; }

private:
  std::string source_, copy_;
};

// A lane taken away; with `withMachine` the instrument it played goes too. Undo puts them back where they were.
class RemoveLane final : public Edit {
public:
  RemoveLane(std::string lane, bool withMachine) : lane_(std::move(lane)), withMachine_(withMachine) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return std::string(withMachine_ ? "Remove lane and machine " : "Remove lane ") + lane_; }
  std::string lane() const override { return lane_; }

private:
  std::string lane_;
  bool withMachine_;
  Lane removed_;
  int laneAt_ = -1, instrumentAt_ = -1;
  Instrument instrument_;
};

// A pattern under another name.
class RenamePattern final : public Edit {
public:
  RenamePattern(std::string lane, std::string pattern, std::string name) : lane_(std::move(lane)), pattern_(std::move(pattern)), name_(std::move(name)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Rename pattern"; }
  std::string lane() const override { return lane_; }

private:
  std::string lane_, pattern_, name_, old_;
};

// A copy of a pattern (same rows) under a new id, the first free index and the name with "-copy" added. It is not placed.
class ClonePattern final : public Edit {
public:
  ClonePattern(std::string lane, std::string source, std::string copyId) : lane_(std::move(lane)), source_(std::move(source)), copy_(std::move(copyId)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Clone pattern"; }
  std::string lane() const override { return lane_; }

private:
  std::string lane_, source_, copy_;
};

// An existing pattern placed on a lane at a tick (a placement already starting there is replaced; undo puts it back).
class PlacePattern final : public Edit {
public:
  PlacePattern(std::string lane, std::string pattern, int start, int length) : lane_(std::move(lane)), pattern_(std::move(pattern)), start_(start), length_(length) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Place pattern"; }
  std::string lane() const override { return lane_; }

private:
  std::string lane_, pattern_;
  int start_, length_;
  std::vector<Placement> replaced_;
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
  // The nodes whose data changed, as "instrument\x1fnode" (undo and redo count). markDataChanged puts one back when it could not be sent.
  std::set<std::string> takeChangedData();
  void markDataChanged(const std::string& node) { changedData_.insert(node); }
  // The instruments whose wiring changed (undo and redo count).
  std::set<std::string> takeChangedGraphs();
  // True once after the song's loop changed (undo and redo count).
  bool takeLoopChanged() { const bool was = loopChanged_; loopChanged_ = false; return was; }
  void markLaneChanged(const std::string& lane) { changed_.insert(lane); }   // to try again later

private:
  Project project_;
  std::vector<std::unique_ptr<Edit>> undo_, redo_;
  int revision_ = 0, savedRevision_ = 0;
  std::set<std::string> changed_, changedParams_, changedData_, changedGraphs_;
  bool loopChanged_ = false;

  void touched(const Edit& edit);
};

}  // namespace wax

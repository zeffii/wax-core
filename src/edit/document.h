// The song as the window edits it. One Document owns the project that is being changed; every change is an Edit that
// can be applied and taken back, so undo and redo work for every kind of edit, now and for the ones added later.
// The audio side never reads the project directly: it is told which lanes changed (see takeChangedLanes).
#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "project/project.h"
#include "seq/curve_edit.h"

namespace wax {

// One reversible change. apply() then revert() must leave the project exactly as it was.
class Edit {
public:
  virtual ~Edit() = default;
  virtual void apply(Project& project) = 0;
  virtual void revert(Project& project) = 0;
  virtual std::string label() const = 0;               // "Set cell", shown next to Undo in the window
  virtual bool allLanes() const { return false; }   // every lane changes (inserting silence)
  virtual std::string otherLane() const { return {}; }  // a second lane whose sound this changes (the old name of a renamed lane)
  virtual std::string lane() const { return {}; }      // the lane whose sound this changes ("" when none)
  virtual std::string paramAddress() const { return {}; }   // "instrument.node.param" of the parameter this sets ("" when none)
  // Every parameter this sets (a recalled preset sets many); by default just paramAddress().
  virtual std::vector<std::string> paramAddresses() const { return paramAddress().empty() ? std::vector<std::string>() : std::vector<std::string>{paramAddress()}; }
  virtual std::string graphInstrument() const { return {}; }   // the instrument whose wiring this changes ("" when none)
  virtual std::string sampleId() const { return {}; }          // the sample of the project whose audio or settings this changes ("" when none)
  virtual bool songLoop() const { return false; }              // true when this changes the song's loop
  virtual std::string dataNode() const { return {}; }      // "instrument\x1fnode" whose data (drawings) this changes ("" when none)
  // Every node whose data this changes (a recalled preset can carry the drawings of several); by default just dataNode().
  virtual std::vector<std::string> dataNodes() const { return dataNode().empty() ? std::vector<std::string>() : std::vector<std::string>{dataNode()}; }
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

// How many copies of an instrument's graph play notes at once (1 to 16), and whether it sleeps when quiet.
class SetInstrumentSetup final : public Edit {
public:
  SetInstrumentSetup(std::string instrument, int voices, bool neverSleeps) : instrument_(std::move(instrument)), voices_(voices), neverSleeps_(neverSleeps) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Voices and sleep of " + instrument_; }
  std::string graphInstrument() const override { return instrument_; }

private:
  std::string instrument_;
  int voices_, oldVoices_ = 1;
  bool neverSleeps_, oldNeverSleeps_ = false;
};

// An instrument renamed: the lanes that play it follow. Refused when another machine listens to it (a sidechain) or plays it (a meta
// machine), as those references live in the file under the old name.
class RenameInstrument final : public Edit {
public:
  RenameInstrument(std::string from, std::string to) : from_(std::move(from)), to_(std::move(to)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Rename machine " + from_; }
  std::string graphInstrument() const override { return to_; }
  bool allLanes() const override { return true; }

private:
  void rename(Project& project, const std::string& from, const std::string& to);
  std::string from_, to_;
};

// A copy of an instrument (graph, parameters, samples) listed right after it, under a new name.
class DuplicateInstrument final : public Edit {
public:
  DuplicateInstrument(std::string source, std::string copy) : source_(std::move(source)), copy_(std::move(copy)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Duplicate machine " + source_; }
  std::string graphInstrument() const override { return copy_; }

private:
  std::string source_, copy_;
};

// An instrument taken away. Refused while a lane plays it, another machine listens to it or a meta machine plays it.
class RemoveInstrument final : public Edit {
public:
  explicit RemoveInstrument(std::string instrument) : instrument_(std::move(instrument)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Remove machine " + instrument_; }

private:
  std::string instrument_;
  int at_ = -1;
  Instrument removed_;
};

// A preset recalled: every parameter it holds is set on the instrument's nodes (parameters whose node is gone are skipped), and the
// drawings it carries (envelope curves, drawn waves) replace those of the nodes they belong to.
class RecallPreset final : public Edit {
public:
  RecallPreset(std::string instrument, int preset) : instrument_(std::move(instrument)), preset_(preset) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Recall preset"; }
  std::vector<std::string> paramAddresses() const override { return addresses_; }
  std::vector<std::string> dataNodes() const override { return dataNodes_; }

private:
  struct Before { std::string node, param; bool had; Json value; };
  struct BeforeData { std::string node; Json data; };   // a node's drawings before the preset gave it its own
  std::string instrument_;
  int preset_;
  std::vector<Before> before_;
  std::vector<BeforeData> beforeData_;
  std::vector<std::string> addresses_, dataNodes_;
};

// The list of presets an instrument keeps (see edit/presets.h) replaced as a whole: saving, overwriting, renaming and removing one are
// all this. The command column's table follows, so the lanes of the machine are rebuilt.
class SetPresets final : public Edit {
public:
  SetPresets(std::string instrument, Json presets, std::string label) : instrument_(std::move(instrument)), presets_(std::move(presets)), label_(std::move(label)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return label_; }
  bool allLanes() const override { return true; }

private:
  std::string instrument_;
  Json presets_, old_;
  std::string label_;
};

// How many notes a row of an instrument's lanes can hold (1 to 8: the "tracks"). The lanes that play it are rebuilt.
class SetNoteColumns final : public Edit {
public:
  SetNoteColumns(std::string instrument, int columns) : instrument_(std::move(instrument)), columns_(columns) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Tracks of " + instrument_; }
  bool allLanes() const override { return true; }

private:
  std::string instrument_;
  int columns_, old_ = 1;
};

// A parameter shown, or no longer shown, as a pattern column of its instrument (the web's "Expose as pattern column"). A new column gets
// a three-letter label, one hex digit for a toggle or a short list, two for the rest, and a log scale where the range is wide.
// The lanes that play the instrument are rebuilt.
class SetExposed final : public Edit {
public:
  SetExposed(std::string instrument, std::string node, std::string param, bool on) : instrument_(std::move(instrument)), node_(std::move(node)), param_(std::move(param)), on_(on) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return std::string(on_ ? "Expose " : "Hide ") + param_; }
  bool allLanes() const override { return true; }

private:
  std::string instrument_, node_, param_;
  bool on_;
  std::vector<Exposed> before_;
};

// The width of an exposed parameter's column: 4 bits is one hex digit, 8 two (the usual), 16 four.
class SetExposedBits final : public Edit {
public:
  SetExposedBits(std::string instrument, std::string node, std::string param, int bits) : instrument_(std::move(instrument)), node_(std::move(node)), param_(std::move(param)), bits_(bits) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Width of " + param_; }
  bool allLanes() const override { return true; }

private:
  std::string instrument_, node_, param_;
  int bits_, old_ = 8;
};

// A parameter given an input port (so a wire can drive it), or without one: taking it away cuts the wires into it.
class SetPort final : public Edit {
public:
  SetPort(std::string instrument, std::string node, std::string param, bool on) : instrument_(std::move(instrument)), node_(std::move(node)), param_(std::move(param)), on_(on) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return std::string(on_ ? "Add port " : "Remove port ") + param_; }
  std::string graphInstrument() const override { return instrument_; }

private:
  std::string instrument_, node_, param_;
  bool on_;
  std::vector<std::string> portsBefore_;
  std::vector<Connection> connectionsBefore_;
};

// Nodes told to keep their pictures beside them (Alt+V) or not to. Only the view changes: the sound's graph is left alone.
class SetKeepView final : public Edit {
public:
  SetKeepView(std::string instrument, std::vector<std::string> nodes, bool on) : instrument_(std::move(instrument)), nodes_(std::move(nodes)), on_(on) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return on_ ? "Keep pictures" : "Stop keeping pictures"; }

private:
  std::string instrument_;
  std::vector<std::string> nodes_;
  bool on_;
  std::vector<std::pair<std::string, bool>> before_;
};

// An instrument one place earlier (direction -1) or later (+1) in the list. The master stays first.
class MoveInstrument final : public Edit {
public:
  MoveInstrument(std::string instrument, int direction) : instrument_(std::move(instrument)), direction_(direction) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Move instrument " + instrument_; }

private:
  std::string instrument_;
  int direction_, from_ = -1, to_ = -1;
};

// A wire's end moved to another port (a dragged wire dropped on a port): `from -> to` becomes `newFrom -> newTo`, in the same place in the list.
// A loop is refused (as Connect does) and nothing changes.
class MoveWire final : public Edit {
public:
  MoveWire(std::string instrument, std::string from, std::string to, std::string newFrom, std::string newTo)
      : instrument_(std::move(instrument)), from_(std::move(from)), to_(std::move(to)), newFrom_(std::move(newFrom)), newTo_(std::move(newTo)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Move wire to " + newFrom_ + " -> " + newTo_; }
  std::string graphInstrument() const override { return instrument_; }

private:
  std::string instrument_, from_, to_, newFrom_, newTo_;
  std::size_t at_ = 0;
  bool replaced_ = false, duplicate_ = false;
};

// A node dropped onto a wire: `from -> to` goes, `from -> newIn` and `newOut -> to` come (the node's first input and first output). A loop
// is refused and nothing changes. Undo puts the wires back as they were.
class InsertOnWire final : public Edit {
public:
  InsertOnWire(std::string instrument, std::string from, std::string to, std::string newIn, std::string newOut)
      : instrument_(std::move(instrument)), from_(std::move(from)), to_(std::move(to)), in_(std::move(newIn)), out_(std::move(newOut)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Put " + in_.substr(0, in_.find('.')) + " into a wire"; }
  std::string graphInstrument() const override { return instrument_; }

private:
  std::string instrument_, from_, to_, in_, out_;
  std::vector<Connection> before_;
};

// A node taken out of its chain: whatever fed its first input now feeds whatever its first output fed (the node keeps no wires on those
// two ports). A loop is refused and nothing changes. Undo puts the wires back as they were.
class PullOut final : public Edit {
public:
  PullOut(std::string instrument, std::string in, std::string out) : instrument_(std::move(instrument)), in_(std::move(in)), out_(std::move(out)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Take " + in_.substr(0, in_.find('.')) + " out of its chain"; }
  std::string graphInstrument() const override { return instrument_; }

private:
  std::string instrument_, in_, out_;
  std::vector<Connection> before_;
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

// A node swapped for one of another type, in the same place. Its wires follow to the ports of the same name (else the port in the same
// position; parameter ports only by name), parameters with the same name and range keep their values, and a node that still has the id its
// type gave it ("filter2") gets a new one to match; exposed columns, commands and sample choices follow it, those the new type lacks go.
// `sampleId` is given to a new sample-playing node that has none. Undo puts everything back as it was.
class ReplaceNode final : public Edit {
public:
  ReplaceNode(std::string instrument, std::string node, std::string type, std::string sampleId = {})
      : instrument_(std::move(instrument)), node_(std::move(node)), type_(std::move(type)), sampleId_(std::move(sampleId)), newId_(node_) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Make " + node_ + " a " + type_; }
  std::string graphInstrument() const override { return instrument_; }
  const std::string& newId() const { return newId_; }   // the node's id after apply

private:
  std::string instrument_, node_, type_, sampleId_, newId_;
  Graph graph_;
  std::vector<Exposed> exposed_;
  std::vector<Command> commands_;
  Json raw_;
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

// Several nodes' tiles moved together (a group drag): each to its own x, y. A drag folds into one undo step.
class MoveNodes final : public Edit {
public:
  struct Place { std::string node; double x, y; };
  MoveNodes(std::string instrument, std::vector<Place> places) : instrument_(std::move(instrument)), places_(std::move(places)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Move " + std::to_string(places_.size()) + " nodes"; }
  bool absorb(const Edit& next) override;

private:
  struct Before { double x, y; bool placed; };
  std::string instrument_;
  std::vector<Place> places_;
  std::vector<Before> before_;
};

// Nodes copied with all their settings (parameters, ports, data, sample choices): `copies` pairs each node with the id its copy gets, and the
// copies sit `dx`, `dy` away. The wires between the copied nodes are copied too. Undo takes the copies away.
class DuplicateNodes final : public Edit {
public:
  DuplicateNodes(std::string instrument, std::vector<std::pair<std::string, std::string>> copies, double dx = 30, double dy = 30)
      : instrument_(std::move(instrument)), copies_(std::move(copies)), dx_(dx), dy_(dy) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return copies_.size() == 1 ? "Duplicate " + copies_.front().first : "Duplicate " + std::to_string(copies_.size()) + " nodes"; }
  std::string graphInstrument() const override { return instrument_; }

private:
  std::string instrument_;
  std::vector<std::pair<std::string, std::string>> copies_;
  double dx_, dy_;
  std::vector<Connection> before_;
  Json rawBefore_;
};

// The wires round a node that is about to go: whatever fed any of its `inputs` now feeds whatever its `output` fed (a mixer's inputs all go on,
// summed). The node's own wires are left for the DeleteNode that follows. A loop is refused and nothing changes.
class JoinAround final : public Edit {
public:
  JoinAround(std::string instrument, std::vector<std::string> inputs, std::string output) : instrument_(std::move(instrument)), inputs_(std::move(inputs)), output_(std::move(output)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Join the wires round " + output_.substr(0, output_.find('.')); }
  std::string graphInstrument() const override { return instrument_; }

private:
  std::string instrument_;
  std::vector<std::string> inputs_;
  std::string output_;
  std::vector<Connection> before_;
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

// One entry of a node's data (`data[key]`, such as a gate's step levels), replaced as a whole. While it is dragged each step folds into the one
// before, so a drag is a single undo.
class SetNodeData final : public Edit {
public:
  SetNodeData(std::string instrument, std::string node, std::string key, Json value, std::string what = "")
      : instrument_(std::move(instrument)), node_(std::move(node)), key_(std::move(key)), value_(std::move(value)), what_(what.empty() ? key_ : std::move(what)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Edit " + what_; }
  std::string dataNode() const override { return instrument_ + "\x1f" + node_; }
  bool absorb(const Edit& next) override;

private:
  std::string instrument_, node_, key_;
  Json value_;
  std::string what_;
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

// A sample added to the project's pool (`sample` as the file keeps it: see encodeSample). Undo takes it away again.
class AddSample final : public Edit {
public:
  AddSample(std::string id, Json sample) : id_(std::move(id)), sample_(std::move(sample)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Load sample " + id_; }
  std::string sampleId() const override { return id_; }

private:
  std::string id_;
  Json sample_;
};

// A sample of the project replaced by an edited version of itself (cut, reversed, a loop moved, a level changed: `sample` as the file keeps it).
// Undo puts the old one back. With `fold`, edits with the same label in a row (dragging a slider) fold into one.
class SetSample final : public Edit {
public:
  SetSample(std::string id, Json sample, std::string label, bool fold = false) : id_(std::move(id)), sample_(std::move(sample)), label_(std::move(label)), fold_(fold) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return label_; }
  std::string sampleId() const override { return id_; }
  bool absorb(const Edit& next) override;

private:
  std::string id_;
  Json sample_, old_;
  std::string label_;
  bool fold_;
};

// A sample taken out of the project. Refused (std::runtime_error from the constructor's check, see usersOfSample) while an instrument plays it.
// Undo puts it back in its place.
class RemoveSample final : public Edit {
public:
  explicit RemoveSample(std::string id) : id_(std::move(id)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Remove sample " + id_; }

private:
  std::string id_;
  Json before_;
};

// A sample given another id; the instruments that play it follow. The caller has checked the new id is free (see uniqueSampleId).
class RenameSample final : public Edit {
public:
  RenameSample(std::string from, std::string to) : from_(std::move(from)), to_(std::move(to)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Rename sample " + from_; }

private:
  std::string from_, to_;
  Json before_;
  std::vector<std::pair<std::string, Json>> instrumentSamples_;   // each instrument's `samples` before
};

// The instruments that play a sample, by id.
std::vector<std::string> instrumentsPlaying(const Project& project, const std::string& sampleId);

// An empty lane (at the end) that plays an instrument the song already has.
class AddLane final : public Edit {
public:
  AddLane(std::string instrument, std::string lane) : instrument_(std::move(instrument)), lane_(std::move(lane)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Add lane " + lane_; }
  std::string lane() const override { return lane_; }

private:
  std::string instrument_, lane_;
};

// A clone of a lane together with a copy of its machine: the copy gets its own instrument (listed after the original) and a lane right
// after the source that plays it, so the sound as it is now is kept while the original goes on changing. `patterns` says whether the
// patterns (with their placements, mutes and offs) come along, `curves` whether the automation does; without them the lane starts empty.
class CloneLaneWithMachine final : public Edit {
public:
  CloneLaneWithMachine(std::string source, std::string instrumentCopy, std::string laneCopy, bool patterns, bool curves)
      : source_(std::move(source)), instrumentCopy_(std::move(instrumentCopy)), laneCopy_(std::move(laneCopy)), patterns_(patterns), curves_(curves) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Clone lane " + source_ + " with its machine"; }
  std::string lane() const override { return laneCopy_; }
  std::string graphInstrument() const override { return instrumentCopy_; }

private:
  std::string source_, instrumentCopy_, laneCopy_;
  bool patterns_, curves_;
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

// `ticks` of silence at the row holding `tick`: in every lane the placements starting at or after the row, the mutes and the note-offs
// move later, the loop follows, and the song grows (a pattern crossing the row stays whole). Undo takes it all back.
class InsertSilence final : public Edit {
public:
  // With `cut`, a pattern running across the row is cut in two there (two new patterns, named "x.1" and "x.2") and its second half moves
  // down; without, it stays whole and only the patterns starting at or after the row move.
  InsertSilence(int tick, int ticks, bool cut = false) : tick_(tick), ticks_(ticks), cut_(cut) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Insert silence"; }
  bool allLanes() const override { return true; }
  bool songLoop() const override { return true; }

private:
  int tick_, ticks_;
  bool cut_ = false;
  int at_ = 0;
  Json loopBefore_;
  std::vector<Lane> lanesBefore_;   // (when cutting: the lanes as they were)
};

// The colour of a pattern (null: automatic, 0-15: a preset, "#rrggbb": custom).
class SetPatternColor final : public Edit {
public:
  SetPatternColor(std::string lane, std::string pattern, Json color) : lane_(std::move(lane)), pattern_(std::move(pattern)), color_(std::move(color)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Pattern colour"; }
  std::string lane() const override { return lane_; }

private:
  std::string lane_, pattern_;
  Json color_, old_;
};

// The song's length in ticks (a whole number of 16-tick rows). Shortening drops the placements that start past the end, trims one
// that runs over it, and drops the mutes and note-offs past it; the patterns stay in the lanes. Undo brings it all back.
class SetSongLength final : public Edit {
public:
  explicit SetSongLength(int ticks) : ticks_(ticks) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Song length"; }
  bool allLanes() const override { return true; }
  bool songLoop() const override { return true; }

private:
  int ticks_;
  int oldLength_ = 0;
  std::vector<Lane> oldLanes_;   // the lanes as they were (placements, mutes, offs)
  Json loopBefore_;
};

// The song's info text (notes, credits, greetings); empty takes it away.
class SetSongInfo final : public Edit {
public:
  explicit SetSongInfo(std::string text) : text_(std::move(text)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return "Song info"; }

private:
  std::string text_;
  Json old_;
};

// The project's name.
class SetSongName final : public Edit {
public:
  explicit SetSongName(std::string name) : name_(std::move(name)) {}
  void apply(Project& project) override { old_ = project.song.name; project.song.name = name_; }
  void revert(Project& project) override { project.song.name = old_; }
  std::string label() const override { return "Project name"; }

private:
  std::string name_, old_;
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

// A lane's `curves` list replaced as a whole (an anchor added, moved, removed; a segment's kind changed; a curve added or taken away).
// The lane is rebuilt in the engine, which bakes the curves again. Edits of one label in a row fold into one undo step (a drag).
class SetCurves final : public Edit {
public:
  SetCurves(std::string lane, Json curves, std::string label) : lane_(std::move(lane)), curves_(std::move(curves)), label_(std::move(label)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return label_; }
  std::string lane() const override { return lane_; }
  bool absorb(const Edit& next) override;
  const Json& curves() const { return curves_; }

private:
  std::string lane_;
  Json curves_, old_;
  std::string label_;
};

// An edit and the changes that came with it, undone and redone together: the pinned anchors of lane curves that followed a pattern when
// the edit moved it (Document::apply makes these).
class FollowedEdit final : public Edit {
public:
  FollowedEdit(std::unique_ptr<Edit> primary, std::vector<std::unique_ptr<Edit>> followers) : primary_(std::move(primary)), followers_(std::move(followers)) {}
  void apply(Project& project) override;
  void revert(Project& project) override;
  std::string label() const override { return primary_->label(); }
  bool absorb(const Edit& next) override { return primary_->absorb(next); }
  const Edit& primary() const { return *primary_; }
  const std::vector<std::unique_ptr<Edit>>& followers() const { return followers_; }

private:
  std::unique_ptr<Edit> primary_;
  std::vector<std::unique_ptr<Edit>> followers_;
};

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
  // The samples whose audio or settings changed, by id (undo and redo count). markSampleChanged puts one back when it could not be sent.
  std::set<std::string> takeChangedSamples();
  void markSampleChanged(const std::string& id) { changedSamples_.insert(id); }
  // True once after the song's loop changed (undo and redo count).
  bool takeLoopChanged() { const bool was = loopChanged_; loopChanged_ = false; return was; }
  void markLaneChanged(const std::string& lane) { changed_.insert(lane); }   // to try again later

private:
  Project project_;
  std::vector<std::unique_ptr<Edit>> undo_, redo_;
  int revision_ = 0, savedRevision_ = 0;
  std::set<std::string> changed_, changedParams_, changedData_, changedGraphs_, changedSamples_;
  bool loopChanged_ = false;

  void touched(const Edit& edit);
  // Lane curves with pinned anchors follow their patterns when an edit moves them.
  std::vector<std::unique_ptr<Edit>> pinFollowers(const std::map<std::string, std::vector<CurveSpan>>& before);
};

}  // namespace wax

// An instrument's pattern columns: what each cell of a row means to the sequencer.
// Port of buildColumns() in webtracker src/seq/lane.js and of src/seq/metacolumns.js, without the parts only the
// page uses (headers and long names).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/params.h"
#include "project/project.h"
#include "seq/cells.h"

namespace wax {

enum class ColumnKind { Note, Vel, Trig, Param, Cmd, Spacer };

// An entry of a command column's table, with its parameter looked up.
struct CommandTarget {
  int id = 0;
  int slot = -1;
  double min = 0;
  double max = 0;
  bool log = false;
  bool isInt = false;
  std::optional<double> fixed;
  int reset = 0;
  int setting = 0;   // 1 save, 2 load
};

// A parameter a reset command puts back, with the value it starts at.
struct GroupValue {
  int slot = 0;
  double value = 0;
};

// A preset, recalled by command 00: the slots and values to write.
struct PresetValues {
  int id = 0;
  std::vector<int> slots;
  std::vector<double> values;
};

struct Column {
  ColumnKind kind = ColumnKind::Note;
  Token token = Token::Note;
  int ch = 0;                   // the column's first channel in the lane's pages

  int nc = 0;                   // Note, Vel: which note column
  int tc = 0;                   // Trig: which trigger column
  int note = 0;                 // Trig: the note number a hit sends
  std::string label, longName;  // the header text before it is squeezed to the cell ("Cut"), and the name for the status line
  std::string target;           // Note, Trig of a meta machine: the instrument it plays (empty: the lane's own)
  bool rel = false;             // Note: a two-digit cell relative to `root`
  int root = 60;

  int slot = -1;                // Param: the parameter it writes
  std::string node, param;      // Param: which node and parameter (what a lane curve calls it)
  double min = 0;
  double max = 1;
  bool isInt = false;
  double denom = 255;           // Param: the largest number the cell can hold
  bool log = false;

  std::vector<CommandTarget> commands;                 // Cmd
  std::vector<PresetValues> presets;
  std::map<int, std::vector<GroupValue>> groups;       // Cmd: group bit -> the parameters of that group
};

// Instruments must already be compiled, so their parameter slots exist.
// Throws std::runtime_error when the instrument shows a parameter that its graph does not have.
std::vector<Column> buildColumns(const Instrument& instrument, const Project& project, const ParamRegistry& registry);

// The columns of a meta machine that show, as indices into its metaColumns (the cells of its lane's rows, in order).
std::vector<int> visibleMetaSpecs(const Instrument& meta, const Project& project);

int channelCount(const std::vector<Column>& columns);

}  // namespace wax

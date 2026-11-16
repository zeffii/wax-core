// A .wax file read into plain structs. This is the file as written: nothing is compiled or played here.
// The same checks as parseProject() in webtracker src/project/file.js.
//
// Order matters: instruments and nodes are kept in the order of the file, because parameter slots are handed out
// in that order (and the JS engine walks its objects in the order they were written).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/node.h"   // Json

namespace wax {

struct Song {
  std::string name = "untitled";
  double bpm = 120;
  int ticksPerBeat = 4;
  int length = 0;          // in ticks
  double swing = 0.5;
  int beatsPerBar = 4;
  Json raw;                // everything the song holds, for the parts not read into fields yet (loop, nodeseq, ...)
};

struct GraphNode {
  std::string id;
  std::string type;
  Json params;                       // { name: value } as written (empty object when the node has none)
  std::vector<std::string> ports;    // parameters that have been given an input port (named "@param" in connections)
  bool stereo = false;               // e.g. a sampler whose sample has two channels
  std::string alias;                 // a copy of a range node keeps the original's parameters under this id
  std::string source;                // sidechain, listen: the instrument it hears
  Json parts;                        // sidechain on a meta machine: which of its machines (null: all)
  double x = 0, y = 0;               // where the node's tile sits in the graph view (web pixels)
  bool keepView = false;             // its pictures (the envelopes) stay beside it when it is not picked (Alt+V)
  bool placed = false;               // the file gave a position (else the view lays the node out)
  Json data;                         // what a node carries besides parameters (a drawn wave, a chord list, ...), or null

  // The value the graph sets for a parameter, or `fallback` when it sets none.
  double param(const std::string& name, double fallback) const;
};

struct Connection {
  std::string from;        // "node.port"
  std::string to;
};

struct Graph {
  std::vector<GraphNode> nodes;
  std::vector<Connection> connections;
  int voices = 1;          // copies of the graph that play notes at once
  bool neverSleeps = false;
};

// A parameter shown as a pattern column.
struct Exposed {
  std::string node;
  std::string param;
  int bits = 8;            // 16: a four-digit column, 4: one digit, otherwise two
  bool log = false;        // the column runs across the range on a log scale
  std::string label, longName;   // the header ("Cut") and the name for the status line ("Filter cutoff"); empty: made up
};

// An entry of the command column's table (CCXXYY): command id -> a parameter.
struct Command {
  int id = 0;
  std::string node;
  std::string param;
  bool log = false;
  std::optional<double> fixed;   // writes this one value whatever the value cell says
  int reset = 0;                 // a group mask: the command puts those groups back to their starting values
  std::string setting;           // "save" or "load": the command stores or recalls a set of values
};

// A trigger column (the hi-hat's Cl and Op): each filled cell is a hit with this note number.
struct Trigger {
  int note = 0;
  std::string label, longName;   // the header ("Cl") and the name for the status line ("Closed hat"); empty: made up
};

// One column of a meta machine: a proxy for one thing of another instrument (see seq/columns.cpp).
struct MetaColumn {
  std::string id;
  std::string instrument;
  std::string role;        // note, rnote, vel, trig, spacer, param
  int nc = 0;
  int tc = 0;
  std::string node;
  std::string param;
  int bits = 0;            // 0: as wide as the instrument shows it
  int root = 60;           // rnote: the MIDI number of the root note
};

struct Instrument {
  std::string id;
  Graph graph;
  int noteColumns = 1;
  bool noVel = false;
  std::vector<Exposed> exposed;
  std::vector<Command> commands;
  std::vector<Trigger> triggers;
  bool isMeta = false;
  std::vector<MetaColumn> metaColumns;
  Json raw;                // presets, commandGroups, samples: read when the nodes that need them are ported
};

struct Pattern {
  std::string id;          // the key in the lane's patterns ("p01")
  std::string name;
  int index = 0;
  std::vector<std::string> rows;   // one line of text per row, as in the file
  Json color;              // null: automatic (by index); an integer 0-15: one of the preset pastels; a "#rrggbb" string: custom
};

struct Placement {
  std::string pattern;     // a Pattern::id
  int start = 0;           // in pages
  int length = 0;
};

struct Lane {
  std::string id;
  std::string instrument;
  std::vector<Pattern> patterns;
  std::vector<Placement> placements;
  std::vector<int> mutes;  // the ticks where the lane flips between playing and muted
  std::vector<int> offs;   // the ticks where the lane lets go of its notes
  bool silenced = false;
  Json raw;                // curves: read when automation is ported
};

struct Project {
  Song song;
  std::vector<Instrument> instruments;
  std::vector<Lane> lanes;
  Json samples;            // as written (still encoded); decoded when the sampler is ported
  Json file;               // the whole file as read: saving writes this back with the lanes as edited, so nothing else is lost

  const Instrument* findInstrument(const std::string& id) const;

  // Every node type the project's instruments use, sorted, each once.
  std::vector<std::string> nodeTypes() const;
};

// The master machine and its lane: every project gets them when it is loaded, first in the lists (see project/master.cpp).
inline const std::string kMaster = "master";

// Both throw std::runtime_error with a message meant for the user when the text is not a project this build can read.
// One instrument as the file holds it ({ graph, noteColumns, exposed, commands, ... }) under an id, and a blank lane for it.
Instrument instrumentFromJson(const std::string& id, const Json& data);
Lane blankLane(const std::string& laneId, const std::string& instrumentId);

Project parseProject(const std::string& text);
Project loadProject(const std::string& path);

}  // namespace wax

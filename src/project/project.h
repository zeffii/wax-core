// A .wax file read into plain structs. This is the file as written: nothing is compiled or played here.
// The same checks as parseProject() in webtracker src/project/file.js.
//
// Order matters: instruments and nodes are kept in the order of the file, because parameter slots are handed out
// in that order (and the JS engine walks its objects in the order they were written).
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace wax {

using Json = nlohmann::ordered_json;   // keeps the keys of an object in file order

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
  Json params;             // { name: value } as written (empty object when the node has none)
  Json raw;                // the whole node (data, init, ...), for nodes that carry more than parameters
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

struct Instrument {
  std::string id;
  Graph graph;
  int noteColumns = 1;
  Json raw;                // exposed, commands, triggers, meta, samples: read by the sequencer when it is ported
};

struct Pattern {
  std::string id;          // the key in the lane's patterns ("p01")
  std::string name;
  int index = 0;
  std::vector<std::string> rows;   // one line of text per row, as in the file
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
  Json raw;                // mutes, offs, silenced, curves
};

struct Project {
  Song song;
  std::vector<Instrument> instruments;
  std::vector<Lane> lanes;
  Json samples;            // as written (still encoded); decoded when the sampler is ported

  const Instrument* findInstrument(const std::string& id) const;

  // Every node type the project's instruments use, sorted, each once.
  std::vector<std::string> nodeTypes() const;
};

// Both throw std::runtime_error with a message meant for the user when the text is not a project this build can read.
Project parseProject(const std::string& text);
Project loadProject(const std::string& path);

}  // namespace wax

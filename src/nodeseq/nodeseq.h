// The Node Sequencer's engine: a graph of event and control wires that runs beside the pattern lanes. The Sequencer
// calls it on every tick and subtick; it has no audio of its own, it sends notes and parameter values out.
// Port of webtracker src/audio/nodeseq.js, without the parts only the page uses (activity lights, recording notes into
// patterns, editing a graph while it plays).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/node.h"
#include "core/random.h"
#include "nodeseq/seq_node.h"

namespace wax::nodeseq {

inline constexpr int kMaxSubs = 16, kMaxGroups = 8;

// What the names in a graph mean on this machine.
struct SetTarget {   // a `set` node's parameter
  int slot = -1;
  double min = 0;
  double max = 1;
  bool log = false;
  bool isInt = false;
};
struct Resolve {
  std::map<std::string, SetTarget> slots;       // by node id
  std::array<int, kMaxGroups> groupSlots;       // the slot a pattern writes to start or stop each group, or -1
  Resolve() { groupSlots.fill(-1); }
};

// How the engine reaches the rest: notes to instruments, the parameter block, and the dice.
struct Io {
  std::function<void(const std::string& instrument, const NoteEvent&)> note;
  float* params = nullptr;
  const Clock* clock = nullptr;
  Xorshift32* dice = nullptr;
};

// One node of a loaded graph.
struct Rec {
  std::string id;
  std::string type;
  std::map<std::string, double> p;      // parameters by name
  Json data;
  int group = 0;
  int columnKey = 0;                    // tells an instrument which node a note came from (see NoteEvent::column)

  struct Wire {
    Rec* to;
    std::string port;
  };
  struct Source {
    Rec* from;
    std::string out;
  };
  std::map<std::string, std::vector<Wire>> outs;             // event wires leaving each output
  std::vector<std::pair<std::string, Source>> srcs;          // what is wired into each control or pulled input, in the order it was wired
  std::map<std::string, double> cout;                        // the control outputs as they stand
  std::unique_ptr<SeqNode> inst;

  const Source* source(const std::string& in) const;
};

class NodeSeq {
public:
  NodeSeq(double sampleRate, Io io);
  ~NodeSeq();

  // graph: { sub, nodes: [{ id, type, group, params, data }], wires: [{ from: "node.port", to: "node.port" }], groups: [{ name, on }] }
  void load(const Json& graph, const Resolve& resolve);

  bool empty() const { return list_.empty(); }
  int subs() const { return subs_; }
  // The types in the loaded graph that this engine does not have, each once.
  const std::vector<std::string>& unknownTypes() const { return unknown_; }

  void start();
  void stop();
  // The first subtick of a tick: the tick itself, then the subtick.
  void onTick(int tick, int tpb, int barTicks, double tickSamples, double bpm);
  void onSub(int index);
  // Controls move with the audio: once for every stretch of samples the sequencer runs.
  void control(int span);

private:
  friend class Cx;

  void reset();
  void emit(Rec& rec, const std::string& out, double value, double vel, double len);
  void deliver(Rec& rec, const std::string& port, double value, double vel, double len);
  void after(Rec& rec, double subs, std::function<void()> fn, bool always);
  void runDue();
  void subEvent();
  void setGroupOn(int group, bool on);
  void pollGroups();
  std::vector<Rec*> controlOrder();

  double sampleRate_;
  Io io_;
  Info info_;
  int subs_ = 4;
  Json graph_;
  Resolve resolve_;
  std::vector<std::unique_ptr<Rec>> list_;   // in the order of the graph
  std::vector<Rec*> controlOrder_;           // the nodes with a control(), sources first
  std::vector<std::string> unknown_;
  std::array<bool, kMaxGroups> on_;
  std::array<std::uint32_t, kMaxGroups> seen_{};   // how often a pattern had written each group's slot

  struct Queued {
    long at;
    std::function<void()> fn;
    Rec* rec;
    bool always;
  };
  std::vector<Queued> queue_;   // what cx.after has waiting, soonest first
  int depth_ = 0;
  long abs_ = -1;
  bool live_ = false;
};

}  // namespace wax::nodeseq

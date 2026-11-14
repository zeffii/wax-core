// What a node of the Node Sequencer is. A node has event ports (a message at a moment: a value and a velocity,
// delivered on a tick or a subtick) and control ports (a number that is always there, 0 to 1, read every stretch of
// audio). Two more kinds of port are pulled: a chord and a set of note volumes; the node they are wired into asks for
// them when it needs them, and the node they come from answers in get().
// Port of the node interface in webtracker src/nodeseq/nodes.js (see there for the same notes on `cx`).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/node.h"   // Json

namespace wax::nodeseq {

class NodeSeq;
struct Rec;

// Where the song is.
struct Info {
  int tick = 0;
  int subIdx = 0;
  int subs = 4;          // subticks a tick
  int tpb = 4;
  int barTicks = 16;
  long abs = 0;          // every subtick since the start
  double tickSec = 0.125;
  double bpm = 120;
};

// What a chord or weights port hands over when it is pulled.
struct Pulled {
  std::vector<int> notes;        // a chord: its notes, lowest first
  std::vector<double> vels;      // a chord: the volume of each note, 0 to 1
  std::vector<double> weights;   // a weights port: volumes in percent, lowest note first
  bool isWeights = false;
};

// What a node gets to work with (the JS calls it cx).
class Cx {
public:
  Cx(NodeSeq& engine, Rec& rec) : engine_(engine), rec_(rec) {}

  const std::string& id() const;
  double p(const std::string& name) const;   // a parameter of the node, by name
  const Json& data() const;                  // the node's own data (a list of steps, a chord, a target)
  const Info& info() const;

  // Sends an event out of a port. vel -1: the sender has no velocity, the receiver picks its own; len, in ticks, is how
  // long a note should last (0: the receiver decides).
  void emit(const std::string& out, double value, double vel, double len = 0) const;
  bool has(const std::string& in) const;                       // is something wired into this pulled or control port?
  std::optional<Pulled> pull(const std::string& in) const;     // what the node wired into a chord or weights port offers now
  double read(const std::string& in) const;                    // the control wired into a port, or NaN when none is
  void ctl(const std::string& out, double x) const;            // sets a control output
  // Runs fn a number of subticks from now (0 is now). It is dropped if the node's group has stopped by then, unless `always`.
  void after(double subs, std::function<void()> fn, bool always = false) const;
  double rand() const;                                         // a random number in [0, 1)

  // the outside world
  void noteOn(int note, double vel, int from) const;           // on the instrument named in the node's data
  void noteOff(int note) const;
  void setParam(double x) const;                               // the parameter named in the node's data, 0 to 1 across its range
  void setGroup(int group, int mode) const;                    // mode 0 start, 1 stop, 2 flip

private:
  NodeSeq& engine_;
  Rec& rec_;
};

// The behaviour of one node. Every function is optional.
class SeqNode {
public:
  virtual ~SeqNode() = default;
  virtual void event(const std::string& /*port*/, double /*value*/, double /*vel*/, double /*len*/) {}
  virtual std::optional<Pulled> get(const std::string& /*out*/) { return std::nullopt; }   // answers a pull
  virtual void tick() {}                    // every tick of the song
  virtual void sub() {}                     // every subtick
  virtual bool hasControl() const { return false; }
  virtual void control(double /*dt*/) {}    // every stretch of audio, dt in seconds
  virtual void reset() {}                   // the graph starts again
  virtual void stop() {}                    // the transport stopped
};

enum class PortKind { Event, Control, Chord, Weights };

struct Port {
  std::string name;
  PortKind kind = PortKind::Event;
};

struct SeqNodeType {
  std::vector<Port> ins;
  std::vector<Port> outs;
  std::string dynInPrefix;                       // a Phrase node's chord ports: c1, c2 ... as many as are wired
  PortKind dynInKind = PortKind::Chord;
  std::map<std::string, double> defaults;        // the parameters a node of this type starts with
  std::function<std::unique_ptr<SeqNode>(Cx)> create;
};

// Every node type, by the name used in .wax files. Nothing for a type this engine does not have (the script node).
const SeqNodeType* findSeqType(const std::string& type);

// How the type files add their types (see nodeseq/types_*.cpp).
using TypeTable = std::map<std::string, SeqNodeType>;
void addTimeTypes(TypeTable& types);      // clock, every, euclid, steps, delay, section, modulo
void addValueTypes(TypeTable& types);     // Pseq, Pattern, Prand, Pwhite, chance, math, hold, drunk, latch
void addChordTypes(TypeTable& types);     // Pchord, Chord, Phrase, Voicing
void addOutputTypes(TypeTable& types);    // lfo, ad, scale, note, set, run, the route dots, monitor
void addLogicTypes(TypeTable& types);     // switch, pick, gate, counter, compare, flip, once, ratchet

// Helpers for writing a type in a few lines.
inline Port ev(const char* name) { return {name, PortKind::Event}; }
inline Port ct(const char* name) { return {name, PortKind::Control}; }

template <typename Node>
SeqNodeType makeType(std::vector<Port> ins, std::vector<Port> outs, std::map<std::string, double> defaults) {
  SeqNodeType type;
  type.ins = std::move(ins);
  type.outs = std::move(outs);
  type.defaults = std::move(defaults);
  type.create = [](Cx cx) { return std::make_unique<Node>(cx); };
  return type;
}

}  // namespace wax::nodeseq

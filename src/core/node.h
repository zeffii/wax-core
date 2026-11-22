// What a node is, to the engine. Each webtracker node (src/audio/nodes/*.js) becomes one NodeType here:
// the same ports, the same parameters in the same order, and a process() that is a line-by-line translation.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/params.h"
#include "core/probe.h"

namespace wax {

using Json = nlohmann::ordered_json;   // keeps the keys of an object in file order

struct ParamSpec {
  std::string name;
  double min = 0;
  double max = 1;
  double def = 0;
  bool isInt = false;
  std::string unit;   // empty means milliseconds where the name is a time (see engine/sleep.cpp)

  ParamSpec(std::string name_, double min_, double max_, double def_, bool isInt_ = false, std::string unit_ = {})
      : name(std::move(name_)), min(min_), max(max_), def(def_), isInt(isInt_), unit(std::move(unit_)) {}
};

// What a port carries. Ports are audio unless the node type lists them in `kinds`.
// Audio, Control and Param are one buffer; Stereo is a left/right pair that travels on one wire.
enum class PortKind { Audio, Control, Param, Stereo };

// A note event, as the sequencer sends it to an instrument and the runtime hands it to the nodes.
enum class NoteType { On, Off, AllOff, Cut };

struct NoteEvent {
  NoteType type = NoteType::On;
  int note = 0;          // MIDI number
  double vel = 1;        // 0..1
  int from = -1;         // On: the note this column was still playing (for a slide), or -1
  int column = -1;       // On: which note column of which lane it came from, or -1 (a trigger, a key)
  bool implied = false;  // Off: the release comes from a new note in the same column, not from a note off in the pattern
};

// What nodes can read about the song's clock. The sequencer keeps it up to date.
struct Clock {
  double tickSamples = 0;                           // samples in a tick at the tempo now
  bool playing = false;
  double songPos = 0;                               // in ticks and fractions of one
  int tickId = 0;                                   // counts the ticks that have fired
  std::array<std::uint32_t, kParamCapacity> stamps{};   // how often a pattern has written each parameter slot
};

// A sample as the sampler node is sent it (the 'sample' message of the JS nodes): the audio of one or two channels,
// the speed it was recorded at, and the levels, loops and slice markers that go with it. Positions are in frames.
struct SampleMessage {
  int slot = 0;                    // which of the node's samples this is
  std::vector<float> data, right;  // `right` stays empty for a mono sample
  double rate = 44100;
  double volume = 1, globalVolume = 1, pan = 0;
  std::vector<double> slices;      // marker frames, sorted
  bool loopOn = true;              // (a loop only counts when it fits inside the sample)
  double loopStart = 0, loopEnd = 0, loopXfade = 0;
  bool pingpong = false;           // the loop runs forwards, then backwards (XM and IT samples; only the XM and IT channels play it so)
  bool susPingpong = false;        // the sustain loop runs forwards, then backwards (IT samples)
  bool panOn = false;              // the sample sets the panning (IT samples; `pan` then counts)
  bool susOn = true;
  double susStart = 0, susEnd = 0, susXfade = 0;
};

// What the tap node sends out while it records: a take starts, blocks of its audio follow, and it ends (the page of the web
// app assembles the sample from them; see engine/takes.h).
struct TakeMessage {
  enum class Phase { Start, Chunk, End } phase = Phase::Start;
  int take = 0;                    // which take of this node
  int smp = 0, ch = 2, ticks = 0, copy = 0;   // Start: the pool slot (1 to 16), channels, length in ticks (0: until stopped), copy target
  double rate = 44100, fade = 0, fadeIn = 0;  // Start: the sample rate and the fade lengths in ms
  std::vector<float> l, r;         // Chunk: the audio (r is empty for one channel)
};

// The whole mix so far in this block, for the master graph's bus-in node.
struct Bus {
  const float* left = nullptr;
  const float* right = nullptr;
};

// The finished sound of another instrument in this block, for a sidechain node.
struct StereoView {
  const float* left = nullptr;
  const float* right = nullptr;
};

// What a node is told when it is made (the argument of create() in the JS nodes). It grows as nodes that need more are ported.
// The limits of the port a range node feeds, so the node can speak in real values.
struct RangeTarget {
  double min = 0;
  double max = 1;
  bool log = false;
  bool isInt = false;
};

struct NodeContext {
  double sampleRate = 44100;
  int maxBlock = 512;
  std::vector<bool> connected;           // one flag per input port
  bool stereo = false;                   // the node's own `stereo` flag in the graph
  const Json* data = nullptr;            // the node's `data` in the graph, when it has any
  std::shared_ptr<void>* shared = nullptr;   // one slot for each node of an instrument, shared by all its voice copies: a node that runs
                                             // once per voice can know what the others are doing (it makes its state there, once)
  std::optional<RangeTarget> target;     // a range node: the port it feeds
  std::optional<double> init;            // where a node feeding a parameter port should start (0..1)
  const Clock* clock = nullptr;
  const Bus* bus = nullptr;              // only in the master instrument; nullptr elsewhere
  std::function<void(const NoteEvent&)> deliver;   // plays a note on this node's own instrument (the arp does)
  // How many nodes of a kind this engine has made so far, counting this one (1, 2, 3 ...). The nodes that roll dice seed
  // their generator from it, so every node and every voice wanders its own way, the same in every render.
  std::function<int(const std::string& kind)> nextInstance;
  // The sound of the instrument this node listens to (its `source` in the graph), when that instrument has already run
  // in this block; nothing otherwise. Empty for a node with no source.
  std::function<std::optional<StereoView>()> tap;
  // Where the tap node sends what it records. Empty when nothing listens.
  std::function<void(const TakeMessage&)> emit;
  // Where a node with a live picture publishes it (shared with the window); null when the node has none.
  std::shared_ptr<SpectrumProbe> probe;
};

// One running copy of a node. All memory is taken in the constructor: process() never allocates.
class Node {
public:
  virtual ~Node() = default;

  // inputs and outputs hold one buffer of n samples per port, in port order; a stereo port takes two in a row
  // (left, then right). An unconnected input points at a buffer of silence.
  // The value of the node's parameter number i is params[slots[i]].
  virtual void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) = 0;

  // A note for this node's instrument (or for its voice, when the instrument has several).
  virtual void note(const NoteEvent&) {}

  // A sample for the node to play (sent when an instrument is loaded, and again if the instrument is rebuilt).
  virtual void sample(const std::shared_ptr<const SampleMessage>&) {}

  // The node's data (a drawn envelope and the like, as the file holds it) has changed while the song plays. Only the nodes whose
  // drawings can be edited in the window listen.
  virtual void data(const Json&) {}

  // True while the node is still sounding although its output is silent for now (a pulse train between pulses),
  // so its voice is not switched off.
  virtual bool busy() const { return false; }

  // Only for a node type marked noteFilter (the arp): it takes the instrument's notes first and plays what it makes of them.
  virtual void noteInput(const NoteEvent&, const float* /*params*/, const int* /*slots*/) {}
};

struct NodeType {
  std::string type;                   // the name used in .wax files
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  std::vector<ParamSpec> params;
  std::map<std::string, PortKind> kinds;        // ports that are not plain audio
  std::vector<std::string> stereoOptional;      // ports that are stereo when the node has `stereo: true` in the graph

  bool dual = false;         // a mono node that runs as a left/right pair when a stereo signal reaches it
  bool master = false;       // the output node: a graph has exactly one
  bool shared = false;       // never copied per voice: one copy runs for the whole instrument
  bool isEffect = false;     // effects get a hard "bypass" switch as their last parameter (added by the registry)
  // Names a parameter that, when it is 1, keeps a note that is struck again on the voice that still holds that note (one string, one voice).
  std::string restrikeParam;
  bool columnVoice = false;  // keeps a note column on one voice, so a note can slide into the next
  bool noteFilter = false;   // takes the instrument's notes first (see Node::noteInput)

  // How many samples late the node's sound comes out, from its settings by name. Empty: none.
  std::function<double(const std::map<std::string, double>& settings, double sampleRate)> latency;

  std::function<std::unique_ptr<Node>(const NodeContext&)> create;

  int paramIndex(const std::string& name) const {
    for (std::size_t i = 0; i < params.size(); i++) if (params[i].name == name) return static_cast<int>(i);
    return -1;
  }
};

}  // namespace wax

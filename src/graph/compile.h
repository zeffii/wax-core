// Graph description -> compiled graph: plain data the runtime can run. Port of webtracker src/graph/compile.js
// (with kinds.js, rangetarget.js and the per-node part of audio/latency.js).
//
// Buffer 0 is reserved as silence for unconnected inputs. A mono port is one buffer, a stereo port is a pair.
// A mono wire into a stereo input feeds both channels; a stereo wire into a mono input is averaged (a hidden fold
// node); several wires into one input are added (a hidden sum node).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/node.h"
#include "core/node_registry.h"
#include "core/params.h"
#include "project/project.h"

namespace wax {

// The buffer (or the pair of buffers) a port reads or writes.
struct BufferRef {
  int left = 0;
  int right = -1;   // -1: a mono port

  bool isStereo() const { return right >= 0; }
  bool isLive() const { return left != 0 || (isStereo() && right != 0); }   // anything but silence
};

// A wire into a parameter port: which input carries the value of which parameter, and how a 0 to 1 signal maps onto it.
struct ParamPort {
  int input = 0;
  int slot = 0;
  double min = 0;
  double max = 1;
  bool log = false;
  bool isInt = false;
};

// The limits of the port a range node feeds, so the node can speak in real values.
struct RangeTarget {
  double min = 0;
  double max = 1;
  bool log = false;
  bool isInt = false;
};

struct CompiledNode {
  std::string id;
  std::string type;
  const NodeType* def = nullptr;
  std::vector<int> slots;              // one parameter slot per parameter of the type
  std::vector<BufferRef> inputs;
  std::vector<BufferRef> outputs;
  std::vector<bool> connected;         // one flag per input
  std::vector<ParamPort> paramPorts;   // only the connected ones
  std::optional<double> init;
  bool dual = false;                   // runs as a left/right pair
  bool stereo = false;
  std::string source;
  Json parts;
  Json data;
  std::optional<RangeTarget> target;
};

struct CompiledGraph {
  std::string instrument;
  std::vector<CompiledNode> nodes;     // in the order they run
  int bufferCount = 1;
  int masterLeft = 0;                  // the buffers the output node writes
  int masterRight = 0;
  int voices = 1;
  bool sleeps = true;
  int amp = -1;                        // the slot of the hidden channel level after the output node
  int send = -1;                       // the slot of the output node's "master" switch
  int lead = -1;                       // the slot of the output node's "lead" switch
  bool wired = false;                  // something is connected to the output node
  int latency = 0;                     // how many samples late the sound comes out
};

struct CompileOptions {
  bool delayComp = false;
  double sampleRate = 48000;   // only used to work out latencies (webtracker's offline tools compile at 48000 too)
};

// Also gives every parameter of the graph its slot in the registry, wired or not.
// Throws std::runtime_error with a message for the user when the graph cannot be compiled.
CompiledGraph compileGraph(const Graph& graph, const std::string& instrument, const NodeRegistry& nodeTypes,
                           ParamRegistry& registry, const CompileOptions& options = {});

// Whether a wide positive range is logarithmic (sliders and pattern columns use the same rule).
inline bool isLogSpec(const ParamSpec& spec) { return spec.min > 0 && spec.max / spec.min >= 50; }

}  // namespace wax

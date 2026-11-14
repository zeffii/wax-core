// What a node is, to the engine. Each webtracker node (src/audio/nodes/*.js) becomes one NodeType here:
// the same ports, the same parameters in the same order, and a process() that is a line-by-line translation.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wax {

struct ParamSpec {
  std::string name;
  float min = 0;
  float max = 1;
  float def = 0;
  bool isInt = false;
};

// What a node is told when it is made (the argument of create() in the JS nodes). It grows as nodes that need more are ported.
struct NodeContext {
  double sampleRate = 44100;
  int maxBlock = 512;
  std::vector<bool> connected;   // one flag per input port, in the order of NodeType::inputs
  bool stereo = false;
};

// One running copy of a node. All memory is taken in the constructor: process() never allocates.
class Node {
public:
  virtual ~Node() = default;

  // inputs and outputs are one buffer of n samples per port. An unconnected input points at a buffer of silence.
  // The value of the node's parameter number i is params[slots[i]].
  virtual void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) = 0;
};

struct NodeType {
  std::string type;                   // the name used in .wax files
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  std::vector<ParamSpec> params;
  bool dual = false;                  // runs as a left/right pair when a stereo signal arrives
  bool isEffect = false;              // effects get a hard "bypass" switch as their last parameter (added by the registry)
  std::function<std::unique_ptr<Node>(const NodeContext&)> create;
};

}  // namespace wax

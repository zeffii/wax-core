// Marks where the voices of a polyphonic instrument are added together. Everything that feeds this node runs once per
// voice (an oscillator and its envelope, say); everything after it runs once, on the sum (a delay, a reverb, the out
// node). With one voice it simply passes the signal through. The runtime does the summing for several voices.
// Port of webtracker src/audio/nodes/voices.js.
#include <algorithm>

#include "nodes/all_nodes.h"

namespace wax {
namespace {

class Voices final : public Node {
public:
  void process(const float* const* inputs, float* const* outputs, int n, const float*, const int*) override {
    std::copy(inputs[0], inputs[0] + n, outputs[0]);
    std::copy(inputs[1], inputs[1] + n, outputs[1]);
  }
};

}  // namespace

void registerVoices(NodeRegistry& registry) {
  NodeType type;
  type.type = "voices";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.create = [](const NodeContext&) { return std::make_unique<Voices>(); };
  registry.add(std::move(type));
}

}  // namespace wax

// A route point: the signal goes in and comes out unchanged, a place to branch a wire. Port of webtracker src/audio/nodes/route.js.
#include <algorithm>

#include "nodes/all_nodes.h"

namespace wax {
namespace {

class Route final : public Node {
public:
  explicit Route(const NodeContext&) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float*, const int*) override {
    std::copy(inputs[0], inputs[0] + n, outputs[0]);
  }
};

}  // namespace

void registerRoute(NodeRegistry& registry) {
  NodeType type;
  type.type = "route";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.dual = true;   // runs as a left/right pair when a stereo signal arrives
  type.create = [](const NodeContext& context) { return std::make_unique<Route>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

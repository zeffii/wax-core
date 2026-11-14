// Bus in: the sound of the whole song so far, as a stereo signal. It exists in the Master graph: the sum of every
// machine arrives here, goes through whatever effects are wired after it, and into the Out node, then the master
// volume and the limiter. (In any other machine it is silent.)
// Port of webtracker src/audio/nodes/busin.js.
#include <algorithm>

#include "nodes/all_nodes.h"

namespace wax {
namespace {

class BusIn final : public Node {
public:
  explicit BusIn(const NodeContext& context) : bus_(context.bus) {}

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    if (!bus_ || !bus_->left) {
      std::fill(outLeft, outLeft + n, 0.0f);
      std::fill(outRight, outRight + n, 0.0f);
      return;
    }
    const float g = params[slots[0]];
    for (int i = 0; i < n; i++) {
      outLeft[i] = static_cast<float>(static_cast<double>(bus_->left[i]) * g);
      outRight[i] = static_cast<float>(static_cast<double>(bus_->right[i]) * g);
    }
  }

private:
  const Bus* bus_;
};

}  // namespace

void registerBusIn(NodeRegistry& registry) {
  NodeType type;
  type.type = "bus-in";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.shared = true;   // one copy for all voices
  type.params = {{"level", 0, 2, 1}};
  type.create = [](const NodeContext& context) { return std::make_unique<BusIn>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

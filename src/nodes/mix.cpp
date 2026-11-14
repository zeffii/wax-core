// Crossfade between a and b: out = a * (1 - mix) + b * mix. An unconnected input is silence.
// Port of webtracker src/audio/nodes/mix.js.
#include "nodes/all_nodes.h"

namespace wax {
namespace {

class Mix final : public Node {
public:
  explicit Mix(const NodeContext&) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* a = inputs[0];
    const float* b = inputs[1];
    float* out = outputs[0];
    const double m = params[slots[0]];
    if (m0_ < 0) m0_ = m;
    const double dm = (m - m0_) / n;   // ramp across the block
    for (int i = 0; i < n; i++) {
      m0_ += dm;
      out[i] = static_cast<float>(a[i] + (static_cast<double>(b[i]) - a[i]) * m0_);
    }
    m0_ = m;
  }

private:
  double m0_ = -1;
};

}  // namespace

void registerMix(NodeRegistry& registry) {
  NodeType type;
  type.type = "mix";
  type.inputs = {"a", "b"};
  type.outputs = {"out"};
  type.params = {{"mix", 0, 1, 0.5}};
  type.dual = true;   // runs as a left/right pair when a stereo signal arrives
  type.create = [](const NodeContext& context) { return std::make_unique<Mix>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

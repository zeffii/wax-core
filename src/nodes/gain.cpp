// Gain stage. With the cv input connected it is a VCA: out = in * gain * cv. Unconnected, cv counts as 1.
// Port of webtracker src/audio/nodes/gain.js.
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kGain = 0 };
enum Input { kIn = 0, kCv = 1 };

class Gain final : public Node {
public:
  explicit Gain(const NodeContext& context) : cvConnected_(context.connected.size() > kCv && context.connected[kCv]) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* in = inputs[kIn];
    const float* cv = inputs[kCv];
    float* out = outputs[0];
    const double target = params[slots[kGain]];
    if (gain_ < 0) gain_ = target;
    const double step = (target - gain_) / n;   // ramp across the block
    for (int i = 0; i < n; i++) {
      gain_ += step;
      out[i] = static_cast<float>(in[i] * gain_ * (cvConnected_ ? cv[i] : 1.0f));
    }
    gain_ = target;
  }

private:
  bool cvConnected_;
  double gain_ = -1;   // the gain the last block ended on (-1: no block yet). JS numbers are doubles, so this is one too.
};

}  // namespace

void registerGain(NodeRegistry& registry) {
  NodeType type;
  type.type = "gain";
  type.inputs = {"in", "cv"};
  type.outputs = {"out"};
  type.params = {{"gain", 0, 4, 1}};
  type.dual = true;
  type.create = [](const NodeContext& context) { return std::make_unique<Gain>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

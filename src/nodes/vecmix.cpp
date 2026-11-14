// Vector mix: four sounds mixed by one position on a diamond. Port of webtracker src/audio/nodes/vecmix.js.
#include <algorithm>

#include "nodes/all_nodes.h"
#include "nodes/vectorcore.h"

namespace wax {
namespace {

enum Param { kX = 0, kY, kLevel };

class VecMix final : public Node {
public:
  explicit VecMix(const NodeContext& context) {
    for (int k = 0; k < 6; k++) connected_[k] = static_cast<int>(context.connected.size()) > k && context.connected[static_cast<std::size_t>(k)];
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* out = outputs[0];
    const double level = params[slots[kLevel]];
    const double x = static_cast<double>(params[slots[kX]]) + (connected_[4] ? static_cast<double>(inputs[4][n - 1]) : 0);
    const double y = static_cast<double>(params[slots[kY]]) + (connected_[5] ? static_cast<double>(inputs[5][n - 1]) : 0);
    vec::diamondWeights(std::max(-1.0, std::min(1.0, x)), std::max(-1.0, std::min(1.0, y)), target_);
    if (fresh_) { for (int k = 0; k < 4; k++) weights_[k] = target_[k]; fresh_ = false; }
    for (int k = 0; k < 4; k++) step_[k] = static_cast<float>((static_cast<double>(target_[k]) - weights_[k]) / n);
    for (int i = 0; i < n; i++) {
      double sum = 0;
      for (int k = 0; k < 4; k++) {
        weights_[k] = static_cast<float>(static_cast<double>(weights_[k]) + step_[k]);
        if (connected_[k]) sum += static_cast<double>(inputs[k][i]) * weights_[k];
      }
      out[i] = static_cast<float>(sum * level);
    }
  }

private:
  bool connected_[6] = {};
  float target_[4] = {}, weights_[4] = {0.25f, 0.25f, 0.25f, 0.25f}, step_[4] = {};
  bool fresh_ = true;
};

}  // namespace

void registerVecMix(NodeRegistry& registry) {
  NodeType type;
  type.type = "vecmix";
  type.inputs = {"a", "b", "c", "d", "x", "y"};
  type.outputs = {"out"};
  type.kinds = {{"x", PortKind::Control}, {"y", PortKind::Control}};
  type.params = {{"x", -1, 1, 0}, {"y", -1, 1, 0}, {"level", 0, 2, 1}};
  type.create = [](const NodeContext& context) { return std::make_unique<VecMix>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

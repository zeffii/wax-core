// Add: control rate sum. Out = everything wired into `in` + `value`. Port of webtracker src/audio/nodes/add.js.
#include "nodes/all_nodes.h"

namespace wax {
namespace {

class Add final : public Node {
public:
  explicit Add(const NodeContext&) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const double v = params[slots[0]];
    for (int i = 0; i < n; i++) outputs[0][i] = static_cast<float>(inputs[0][i] + v);
  }
};

}  // namespace

void registerAdd(NodeRegistry& registry) {
  NodeType type;
  type.type = "add";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Control}, {"out", PortKind::Control}};
  type.params = {{"value", -1000, 1000, 0}};
  type.create = [](const NodeContext& context) { return std::make_unique<Add>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// DC offset remover, stereo: a one-pole high pass, y[n] = x[n] - x[n-1] + R * y[n-1], with R set from the cutoff.
// Port of webtracker src/audio/nodes/dcblock.js.
#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

class DcBlockNode final : public Node {
public:
  explicit DcBlockNode(const NodeContext& context) : sampleRate_(context.sampleRate) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const double R = js::exp(-2 * kPi * params[slots[0]] / sampleRate_);
    for (int i = 0; i < n; i++) {
      const double l = inputs[0][i], r = inputs[1][i];
      yl_ = l - xl_ + R * yl_; xl_ = l;
      yr_ = r - xr_ + R * yr_; xr_ = r;
      outputs[0][i] = static_cast<float>(yl_);
      outputs[1][i] = static_cast<float>(yr_);
    }
  }

private:
  double sampleRate_;
  double xl_ = 0, yl_ = 0, xr_ = 0, yr_ = 0;
};

}  // namespace

void registerDcBlock(NodeRegistry& registry) {
  NodeType type;
  type.type = "dcblock";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {{"freq", 1, 100, 10}};   // Hz, the cutoff of the high pass
  type.create = [](const NodeContext& context) { return std::make_unique<DcBlockNode>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

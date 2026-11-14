// Pan: mono in, stereo out, equal power; the cv input is added to the pan setting (an lfo gives an auto-pan), averaged over
// the block with the gains ramping across it. Port of webtracker src/audio/nodes/pan.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

class Pan final : public Node {
public:
  explicit Pan(const NodeContext& context) : cvOn_(context.connected.size() > 1 && context.connected[1]) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* input = inputs[0];
    double p = params[slots[0]];
    if (cvOn_) {
      double sum = 0;
      for (int i = 0; i < n; i++) sum += inputs[1][i];
      p += sum / n;
    }
    const double a = (std::min(std::max(p, -1.0), 1.0) + 1) * kPi / 4;
    const double gl = kSqrt2 * js::cos(a), gr = kSqrt2 * js::sin(a);
    if (gl0_ < 0) { gl0_ = gl; gr0_ = gr; }
    const double dl = (gl - gl0_) / n, dr = (gr - gr0_) / n;
    for (int i = 0; i < n; i++) {
      gl0_ += dl;
      gr0_ += dr;
      outputs[0][i] = static_cast<float>(input[i] * gl0_);
      outputs[1][i] = static_cast<float>(input[i] * gr0_);
    }
    gl0_ = gl;
    gr0_ = gr;
  }

private:
  bool cvOn_;
  double gl0_ = -1, gr0_ = 0;
};

}  // namespace

void registerPan(NodeRegistry& registry) {
  NodeType type;
  type.type = "pan";
  type.inputs = {"in", "cv"};
  type.outputs = {"out"};
  type.kinds = {{"cv", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {{"pan", -1, 1, 0}};
  type.create = [](const NodeContext& context) { return std::make_unique<Pan>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

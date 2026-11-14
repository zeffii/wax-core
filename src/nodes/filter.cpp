// Mono state-variable filter (TPT form). mode: 0 lowpass, 1 bandpass, 2 highpass. `mod` moves the cutoff in octaves,
// averaged over each block. Port of webtracker src/audio/nodes/filter.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kCutoff = 0, kRes, kMode };

class Filter final : public Node {
public:
  explicit Filter(const NodeContext& context)
      : sampleRate_(context.sampleRate), modOn_(context.connected.size() > 1 && context.connected[1]) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* input = inputs[0];
    float* out = outputs[0];
    double target = params[slots[kCutoff]];
    if (modOn_) {
      const float* mod = inputs[1];
      double sum = 0;
      for (int i = 0; i < n; i++) sum += mod[i];
      target *= js::pow(2, sum / n);
    }
    if (fc_ < 0) fc_ = target;
    fc_ += (target - fc_) * (1 - js::exp(-n / (sampleRate_ * 0.02)));   // slew
    const double g = js::tan(kPi * std::min(std::max(fc_, 5.0), sampleRate_ * 0.45) / sampleRate_);
    const double k = 2 - 1.95 * params[slots[kRes]];
    const int mode = jsRoundInt(params[slots[kMode]]);
    const double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
    for (int i = 0; i < n; i++) {
      const double x = input[i];
      const double v3 = x - ic2_;
      const double v1 = a1 * ic1_ + a2 * v3;
      const double v2 = ic2_ + a2 * ic1_ + a3 * v3;
      ic1_ = 2 * v1 - ic1_;
      ic2_ = 2 * v2 - ic2_;
      out[i] = static_cast<float>(mode == 0 ? v2 : mode == 1 ? v1 : x - k * v1 - v2);
    }
    if (std::abs(ic1_) < 1e-20) ic1_ = 0;   // no slow denormals while silent
    if (std::abs(ic2_) < 1e-20) ic2_ = 0;
  }

private:
  double sampleRate_;
  bool modOn_;
  double ic1_ = 0, ic2_ = 0, fc_ = -1;
};

}  // namespace

void registerFilter(NodeRegistry& registry) {
  NodeType type;
  type.type = "filter";
  type.inputs = {"in", "mod"};   // mod is in octaves, averaged over each block
  type.outputs = {"out"};
  type.dual = true;
  type.isEffect = true;
  type.kinds = {{"mod", PortKind::Control}};
  type.params = {
    {"cutoff", 20, 18000, 2000},
    {"resonance", 0, 0.95, 0.2},
    {"mode", 0, 2, 0, true},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Filter>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

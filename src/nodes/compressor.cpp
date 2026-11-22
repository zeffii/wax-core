// Feed-forward compressor with a soft knee, stereo-linked: the louder channel drives the detector and both channels
// get the same gain. The gain reduction is smoothed in dB with separate attack and release times. mix blends in the
// dry signal for parallel compression. Port of webtracker src/audio/nodes/compressor.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kThreshold = 0, kRatio, kAttack, kRelease, kKnee, kMakeup, kMix };

const double kDb = 20 / 2.302585092994046, kLin = 2.302585092994046 / 20;   // Math.LN10: dB per neper, and its inverse

class Compressor final : public Node {
public:
  explicit Compressor(const NodeContext& context) : sampleRate_(context.sampleRate) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const double T = params[slots[kThreshold]], R = std::max(1.0, static_cast<double>(params[slots[kRatio]]));
    const double W = params[slots[kKnee]];
    const double ca = coef(params[slots[kAttack]]), cr = coef(params[slots[kRelease]]);
    const double makeup = js::exp(params[slots[kMakeup]] * kLin), mix = params[slots[kMix]];
    for (int i = 0; i < n; i++) {
      const double l = inputs[0][i], r = inputs[1][i];
      const double level = kDb * js::log(std::max(std::max(std::abs(l), std::abs(r)), 1e-9));   // dBFS
      const double over = level - T;
      double target;   // wanted gain change in dB
      if (2 * over < -W) target = 0;
      else if (W > 0 && 2 * std::abs(over) <= W) target = (1 / R - 1) * js::pow(over + W / 2, 2) / (2 * W);
      else target = over / R - over;
      reduction_ = target < reduction_ ? target + (reduction_ - target) * ca : target + (reduction_ - target) * cr;
      const double g = js::exp(reduction_ * kLin) * makeup;
      outputs[0][i] = static_cast<float>(l + (l * g - l) * mix);
      outputs[1][i] = static_cast<float>(r + (r * g - r) * mix);
    }
    if (std::abs(reduction_) < 1e-9) reduction_ = 0;
  }

private:
  double coef(double ms) const { return js::exp(-1 / (ms * 0.001 * sampleRate_)); }

  double sampleRate_;
  double reduction_ = 0;   // gain reduction in dB, 0 or below
};

}  // namespace

void registerCompressor(NodeRegistry& registry) {
  NodeType type;
  type.type = "compressor";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.dryAtZero = "mix";
  type.params = {
    {"threshold", -60, 0, -18},   // dB
    {"ratio", 1, 20, 4},
    {"attack", 0.1, 200, 10},     // ms
    {"release", 5, 2000, 150},    // ms
    {"knee", 0, 24, 6},           // dB
    {"makeup", 0, 24, 0},         // dB
    {"mix", 0, 1, 1},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Compressor>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

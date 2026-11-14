// Distortion: the signal is driven into a clipping curve, then a one-pole lowpass (`tone`) takes the fizz off. mode 0 is
// soft (tanh), 1 hard clip, 2 tube (asymmetric, so it adds even harmonics) and 3 fuzz (a hard, squared-off curve). It
// runs at 1x, 2x or 4x (`oversample`) to cut the aliasing; the dry signal is delayed to match.
// out = (dry * (1 - mix) + wet * mix) * level. Port of webtracker src/audio/nodes/distortion.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"
#include "nodes/oversample.h"

namespace wax {
namespace {

enum Param { kDrive = 0, kTone, kMode, kMix, kLevel, kOversample };

constexpr int kFactors[3] = {1, 2, 4};

int clampMode(double v, int hi) { return std::min(hi, std::max(0, jsRoundInt(v))); }

double shapeOf(int mode, double x) {
  switch (mode) {
    case 0: return js::tanh(x);
    case 1: return x > 1 ? 1 : x < -1 ? -1 : x;
    case 2: return x >= 0 ? js::tanh(x) : js::tanh(x * 0.6) / 0.6 * 0.8;
    default: return js::tanh(x * 6) * (1 - 0.15 * js::exp(-std::abs(x) * 3));
  }
}

class Distortion final : public Node {
public:
  explicit Distortion(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        chains_{{Oversampler(1), Oversampler(1)}, {Oversampler(2), Oversampler(2)}, {Oversampler(4), Oversampler(4)}},
        delays_{FixedDelay(48), FixedDelay(48)},
        hpK_(1 - js::exp(-2 * kPi * 12 / context.sampleRate)) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const double drive = params[slots[kDrive]], mix = params[slots[kMix]], level = params[slots[kLevel]];
    const double toneK = 1 - js::exp(-2 * kPi * std::min(static_cast<double>(params[slots[kTone]]), sampleRate_ * 0.45) / sampleRate_);
    const int shape = clampMode(params[slots[kMode]], 3);
    const int mode = clampMode(params[slots[kOversample]], 2);
    for (int c = 0; c < 2; c++) {
      Oversampler& chain = chains_[mode][c];
      const float* x = inputs[c];
      float* out = outputs[c];
      const int wait = chain.latency();
      for (int i = 0; i < n; i++) {
        double wet = chain.run(x[i], [&](double v) { return shapeOf(shape, v * drive); });
        hp_[c] += (wet - hp_[c]) * hpK_;   // follows the offset
        wet -= hp_[c];
        lp_[c] += (wet - lp_[c]) * toneK;
        wet = lp_[c];
        const double dry = delays_[c].push(x[i], wait);
        out[i] = static_cast<float>((dry + (wet - dry) * mix) * level);
      }
    }
  }

private:
  double sampleRate_;
  Oversampler chains_[3][2];
  FixedDelay delays_[2];
  double lp_[2] = {0, 0}, hp_[2] = {0, 0};
  double hpK_;
};

}  // namespace

void registerDistortion(NodeRegistry& registry) {
  NodeType type;
  type.type = "distortion";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.latency = [](const std::map<std::string, double>& settings, double) {
    return static_cast<double>(oversampleLatency(kFactors[clampMode(settings.at("oversample"), 2)]));
  };
  type.params = {
    {"drive", 0.1, 40, 4},
    {"tone", 300, 20000, 8000},        // Hz, the lowpass after the clipper
    {"mode", 0, 3, 0, true},           // 0 soft, 1 hard, 2 tube, 3 fuzz
    {"mix", 0, 1, 1},
    {"level", 0, 2, 0.7},
    {"oversample", 0, 2, 1, true},     // 0: 1x, 1: 2x, 2: 4x
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Distortion>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

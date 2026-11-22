// Stereo phaser: a chain of first-order allpass stages whose corner frequency is swept by an lfo (`depth` octaves either
// side of `freq`). `feedback` feeds the last stage back to the first. The right channel's lfo is offset by `width`.
// out = dry + wet * mix. Port of webtracker src/audio/nodes/phaser.js.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kRate = 0, kDepth, kFreq, kStages, kFeedback, kWidth, kMix, kPhase, kGlide };

constexpr int kMaxStages = 12;

class Phaser final : public Node {
public:
  explicit Phaser(const NodeContext& context)
      : sampleRate_(context.sampleRate), clock_(context.clock), top_(context.sampleRate * 0.45) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    if (clock_) {   // a write to `phase` puts the sweep at that point in its cycle, then it runs on
      const std::uint32_t stamp = clock_->stamps[static_cast<std::size_t>(slots[kPhase])];
      if (stamp != seen_) {
        const double target = jsMod(params[slots[kPhase]], 1), d = phase_ + err_ - target, g = params[slots[kGlide]];
        err_ = g > 0 ? d - jsRound(d) : 0;
        phase_ = target;
        decay_ = js::exp(-3 / (sampleRate_ * g / 1000 + 1));
      }
      seen_ = stamp;
    }
    const double inc = params[slots[kRate]] / sampleRate_, depth = params[slots[kDepth]], centre = params[slots[kFreq]];
    const int stages = std::max(2, std::min(kMaxStages, jsRoundInt(params[slots[kStages]])));
    const double fb = params[slots[kFeedback]], offset = 0.25 * params[slots[kWidth]], mix = params[slots[kMix]];
    for (int i = 0; i < n; i++) {
      for (int c = 0; c < 2; c++) {
        const double lfo = js::sin(2 * kPi * (phase_ + err_ + (c ? offset : 0)));
        const double f = std::min(top_, std::max(20.0, centre * js::pow(2, depth * lfo)));
        const double t = js::tan(kPi * f / sampleRate_), a = (t - 1) / (t + 1);
        const double dry = inputs[c][i];
        double* state = z_[c];
        double x = dry + fb * last_[c];
        for (int s = 0; s < stages; s++) {
          const double y = a * x + state[s];
          state[s] = x - a * y;
          x = y;
        }
        last_[c] = std::abs(x) < 1e-20 ? 0 : x;
        outputs[c][i] = static_cast<float>(dry + x * mix);
      }
      err_ = std::abs(err_) < 1e-7 ? 0 : err_ * decay_;
      phase_ += inc;
      if (phase_ >= 1) phase_ -= 1;
    }
  }

private:
  double sampleRate_;
  const Clock* clock_;
  double top_;
  double z_[2][kMaxStages] = {};
  double last_[2] = {0, 0};
  double phase_ = 0, err_ = 0, decay_ = 0;
  std::uint32_t seen_ = 0;
};

}  // namespace

void registerPhaser(NodeRegistry& registry) {
  NodeType type;
  type.type = "phaser";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.isEffect = true;
  type.dryAtZero = "mix";
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.params = {
    {"rate", 0.02, 10, 0.4},          // Hz
    {"depth", 0, 4, 2},               // octaves each way
    {"freq", 100, 4000, 800},         // Hz, centre of the sweep
    {"stages", 2, kMaxStages, 6, true},
    {"feedback", -0.95, 0.95, 0.4},
    {"width", 0, 1, 1},
    {"mix", 0, 1, 0.5},
    {"phase", 0, 1, 0},
    {"glide", 0, 500, 20},            // ms
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Phaser>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

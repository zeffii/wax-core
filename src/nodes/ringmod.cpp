// Ring modulator with a carrier that can follow the pitch of what it hears (zero crossings of the lowpassed input), or a fixed
// one. Output = in x (1 - depth + depth x carrier), blended with the dry by mix. Port of webtracker src/audio/nodes/ringmod.js
// (without the panel's pitch reports).
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kFreq = 0, kTrack, kRatio, kOffset, kWave, kDepth, kMix };

double carrierAt(double phase, int wave) {
  switch (wave) {
    case 1: return phase < 0.5 ? 4 * phase - 1 : 3 - 4 * phase;
    case 2: return phase < 0.5 ? 1 : -1;
    case 3: return 2 * phase - 1;
    default: return js::sin(2 * kPi * phase);
  }
}

class RingMod final : public Node {
public:
  explicit RingMod(const NodeContext& context)
      : sampleRate_(context.sampleRate), lpc_(1 - js::exp(-2 * kPi * 1200 / context.sampleRate)) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const bool track = p(kTrack) >= 0.5;
    const double fixed = p(kFreq), ratio = p(kRatio), off = p(kOffset);
    const int wave = jsRoundInt(p(kWave));
    const double depth = p(kDepth), mix = p(kMix);
    constexpr double lo = 40, hi = 2000;
    for (int i = 0; i < n; i++) {
      const double inL = inputs[0][i], inR = inputs[1][i];
      const double x = (inL + inR) * 0.5;
      lp_ += (x - lp_) * lpc_;
      env_ += (std::abs(x) - env_) * 0.002;
      since_++;
      if (prev_ < 0 && lp_ >= 0) {
        const double hz = sampleRate_ / since_;
        if (hz > lo && hz < hi && env_ > 0.003) f0_ = f0_ > 0 && std::abs(hz / f0_ - 1) < 0.5 ? f0_ + (hz - f0_) * 0.35 : hz;
        since_ = 0;
      }
      prev_ = lp_;
      if (since_ > sampleRate_ * 0.05 && env_ < 0.002) f0_ = 0;
      const double want = track && f0_ > 0 ? f0_ * ratio + off : fixed + (track ? off : 0);
      fc_ += (std::max(0.1, want) - fc_) * 0.002;
      phase_ += fc_ / sampleRate_;
      if (phase_ >= 1) phase_ -= 1;
      const double g = 1 - depth + depth * carrierAt(phase_, wave);
      outputs[0][i] = static_cast<float>(inL + (inL * g - inL) * mix);
      outputs[1][i] = static_cast<float>(inR + (inR * g - inR) * mix);
    }
  }

private:
  double sampleRate_, lpc_;
  double phase_ = 0, f0_ = 0, fc_ = 220, lp_ = 0, prev_ = 0, since_ = 0, env_ = 0;
};

}  // namespace

void registerRingMod(NodeRegistry& registry) {
  NodeType type;
  type.type = "ringmod";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {
    {"freq", 0.1, 5000, 220},
    {"track", 0, 1, 1, true},
    {"ratio", 0.25, 8, 1},
    {"offset", -500, 500, 0},
    {"wave", 0, 3, 0, true},
    {"depth", 0, 1, 1},
    {"mix", 0, 1, 1},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<RingMod>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

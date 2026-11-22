// Bitcrusher, stereo: lowers the bit depth and the sample rate. `bits` may be fractional, `rate` is the rate in Hz the
// signal is held at (both channels share the clock), `jitter` randomises when the next sample is taken, `dither` adds a
// little noise before rounding, `mix` blends with the clean signal. Port of webtracker src/audio/nodes/crusher.js.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kBits = 0, kRate, kJitter, kDither, kMix };

class Crusher final : public Node {
public:
  explicit Crusher(const NodeContext& context) : sampleRate_(context.sampleRate) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inL = inputs[0];
    const float* inR = inputs[1];
    float* outL = outputs[0];
    float* outR = outputs[1];
    const double levels = js::pow(2, params[slots[kBits]] - 1.0);   // steps on each side of zero
    const double step = std::min(1.0, params[slots[kRate]] / sampleRate_), jitter = params[slots[kJitter]];
    const double dither = params[slots[kDither]] / levels, mix = params[slots[kMix]];
    auto quantise = [levels](double x) { return jsRound(std::max(-1.0, std::min(1.0, x)) * levels) / levels; };
    for (int i = 0; i < n; i++) {
      const double l = inL[i], r = inR[i];
      phase_ += step;
      if (phase_ >= 1) {
        phase_ -= 1 + (jitter != 0 ? (rand() - 0.5) * jitter * 1.9 : 0);   // the next sample comes sooner or later
        if (phase_ < 0) phase_ = 0;
        const double ditherL = dither != 0 ? (rand() + rand() - 1) * dither : 0;
        heldL_ = quantise(l + ditherL);
        const double ditherR = dither != 0 ? (rand() + rand() - 1) * dither : 0;
        heldR_ = quantise(r + ditherR);
      }
      outL[i] = static_cast<float>(l + (heldL_ - l) * mix);
      outR[i] = static_cast<float>(r + (heldR_ - r) * mix);
    }
  }

private:
  double rand() {
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    return seed_ / 4294967296.0;
  }

  double sampleRate_;
  double phase_ = 1, heldL_ = 0, heldR_ = 0;
  uint32_t seed_ = 0x1234567;
};

}  // namespace

void registerCrusher(NodeRegistry& registry) {
  NodeType type;
  type.type = "crusher";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.isEffect = true;
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.params = {
    {"bits", 1, 16, 8},
    {"rate", 100, 48000, 12000},
    {"jitter", 0, 1, 0},
    {"dither", 0, 1, 0},
    {"mix", 0, 1, 1},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Crusher>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

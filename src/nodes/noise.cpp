// Noise in every colour. `tilt` is the slope of the spectrum in dB per octave: -6 brown, -3 pink, 0 white, +3 blue,
// +6 violet; the values between blend neighbours. Silent until the first note unless `free` is on. A note restarts the
// noise from the same seed, so every hit is the same hit. Port of webtracker src/audio/nodes/noise.js.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kLevel = 0, kTilt, kSeed, kRestart, kFree };

// loudness of each colour against white, measured
constexpr double kGBrown = 0.03189, kGPink = 0.33605, kGBlue = 0.55922, kGViolet = 0.70775;

class Noise final : public Node {
public:
  explicit Noise(const NodeContext&) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { started_ = true; pending_ = true; }
    else if (event.type == NoteType::Cut) started_ = false;
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* out = outputs[0];
    const bool free = params[slots[kFree]] >= 0.5f;
    if (!started_ && !free) { std::fill(out, out + n, 0.0f); return; }
    if (pending_) {
      pending_ = false;
      if (params[slots[kRestart]] >= 0.5f) {
        state_ = static_cast<uint32_t>(static_cast<uint64_t>(jsRound(params[slots[kSeed]])) * 2654435761ull);
        if (state_ == 0) state_ = 1;
      }
    }
    const double level = params[slots[kLevel]];
    const double target = (std::min(6.0, std::max(-6.0, static_cast<double>(params[slots[kTilt]]))) + 6) / 3;   // 0 brown ... 4 violet
    if (idx_ < 0) idx_ = target;
    const double step = (target - idx_) / n;
    uint32_t s = state_;
    for (int i = 0; i < n; i++) {
      s ^= s << 13; s ^= s >> 17; s ^= s << 5;
      const double w = (s / 2147483648.0) - 1;
      idx_ += step;
      // pink: three one-pole filters summed; brown: a leaky integral; blue and violet: the difference of pink and of white
      p0_ = 0.99765 * p0_ + w * 0.0990460; p1_ = 0.963 * p1_ + w * 0.2965164; p2_ = 0.57 * p2_ + w * 1.0526913;
      const double pink = p0_ + p1_ + p2_ + w * 0.1848;
      brown_ = 0.9995 * brown_ + w;
      const double blue = pink - pinkPrev_, violet = w - whitePrev_;
      pinkPrev_ = pink; whitePrev_ = w;
      const int k = static_cast<int>(std::min(3.0, std::floor(idx_)));
      const double f = idx_ - k;
      double a, b;
      if (k == 0) { a = brown_ * kGBrown; b = pink * kGPink; }
      else if (k == 1) { a = pink * kGPink; b = w; }
      else if (k == 2) { a = w; b = blue * kGBlue; }
      else { a = blue * kGBlue; b = violet * kGViolet; }
      out[i] = static_cast<float>((a + (b - a) * f) * level);
    }
    state_ = s;
    for (const double v : {p0_, p1_, p2_, brown_}) {
      if (!std::isfinite(v)) { p0_ = p1_ = p2_ = brown_ = 0; break; }
    }
  }

private:
  uint32_t state_ = 0x9e3779b9u;
  bool started_ = false, pending_ = false;
  double p0_ = 0, p1_ = 0, p2_ = 0, brown_ = 0, pinkPrev_ = 0, whitePrev_ = 0, idx_ = -1;
};

}  // namespace

void registerNoise(NodeRegistry& registry) {
  NodeType type;
  type.type = "noise";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Audio}};
  type.params = {
    {"level", 0, 1, 1},
    {"tilt", -6, 6, 0},       // dB per octave: -6 brown, -3 pink, 0 white, 3 blue, 6 violet
    {"seed", 1, 9999, 1, true},
    {"restart", 0, 1, 1, true},
    {"free", 0, 1, 0, true},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Noise>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

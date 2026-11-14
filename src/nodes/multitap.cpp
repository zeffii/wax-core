// Stereo multi-tap delay. The input is summed to mono into one line; tap k is read at k * time, scaled by decay^(k-1),
// and panned (equal power) along a sweep from left to right across the taps, narrowed by `spread`. The last tap feeds
// back. Dry stays stereo. Port of webtracker src/audio/nodes/multitap.js.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kTime = 0, kTaps, kDecay, kSpread, kFeedback, kMix };

constexpr int kMaxTaps = 8;
constexpr double kMaxTimeMs = 1000;

class MultiTap final : public Node {
public:
  explicit MultiTap(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        size_(static_cast<int>(std::ceil(context.sampleRate * kMaxTaps * kMaxTimeMs / 1000)) + 4),
        buf_(static_cast<std::size_t>(size_), 0.0f),
        slew_(1 - js::exp(-1 / (context.sampleRate * 0.05))) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inL = inputs[0];
    const float* inR = inputs[1];
    float* outL = outputs[0];
    float* outR = outputs[1];
    const double target = std::min(std::max(params[slots[kTime]] * 0.001 * sampleRate_, 1.0), static_cast<double>(size_) / kMaxTaps - 2);
    const int taps = std::min(kMaxTaps, std::max(1, jsRoundInt(params[slots[kTaps]])));
    const double decay = params[slots[kDecay]], spread = params[slots[kSpread]], fb = params[slots[kFeedback]], mix = params[slots[kMix]];
    if (d_ < 0) d_ = target;
    for (int k = 0; k < taps; k++) {   // per tap level and pan, once per block
      const double p = taps == 1 ? 0 : spread * ((2.0 * k) / (taps - 1) - 1);
      const double a = (p + 1) * kPi / 4;
      gain_[k] = js::pow(decay, k);
      gl_[k] = gain_[k] * kSqrt2 * js::cos(a);
      gr_[k] = gain_[k] * kSqrt2 * js::sin(a);
    }
    for (int i = 0; i < n; i++) {
      d_ += (target - d_) * slew_;
      double wl = 0, wr = 0, last = 0;
      for (int k = 0; k < taps; k++) {
        double r = w_ - d_ * (k + 1);
        if (r < 0) r += size_;
        const int j = static_cast<int>(r), j1 = j + 1 == size_ ? 0 : j + 1;
        const double f = r - j;
        const double v = static_cast<double>(buf_[static_cast<std::size_t>(j)]) +
                         (static_cast<double>(buf_[static_cast<std::size_t>(j1)]) - buf_[static_cast<std::size_t>(j)]) * f;
        wl += v * gl_[k];
        wr += v * gr_[k];
        last = v;
      }
      const double l = inL[i], r = inR[i];
      outL[i] = static_cast<float>(l + wl * mix);
      outR[i] = static_cast<float>(r + wr * mix);
      const double v = (l + r) * 0.5 + last * fb;
      buf_[static_cast<std::size_t>(w_)] = static_cast<float>(std::abs(v) < 1e-20 ? 0 : v);   // no slow denormals in a long tail
      if (++w_ == size_) w_ = 0;
    }
  }

private:
  double sampleRate_;
  int size_;
  std::vector<float> buf_;
  double slew_;
  int w_ = 0;
  double d_ = -1;
  double gain_[kMaxTaps] = {}, gl_[kMaxTaps] = {}, gr_[kMaxTaps] = {};
};

}  // namespace

void registerMultiTap(NodeRegistry& registry) {
  NodeType type;
  type.type = "multitap";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.isEffect = true;
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.params = {
    {"time", 1, kMaxTimeMs, 180},       // ms between taps
    {"taps", 1, kMaxTaps, 4, true},
    {"decay", 0, 1, 0.7},
    {"spread", 0, 1, 0.8},
    {"feedback", 0, 0.9, 0.3},
    {"mix", 0, 1, 0.4},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<MultiTap>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

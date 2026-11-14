// Spectral panner: the sound is split into ten bands (a subtractive split, so with no panning the bands add back to the
// input) and each band gets its own place in the stereo field. A shape picks the pattern across the spectrum, `spread`
// how far bands move from where they are, `rotate` slides the pattern along the bands, and everything below `lowMono`
// Hz is made mono. A band moves by blending its own stereo image toward its mono sum placed at the pan position.
// Port of webtracker src/audio/nodes/specpan.js.
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kShape = 0, kSpread, kRotate, kLowMono, kMix, kB1, kRate = 15, kSweep = 16, kWave = 17, kPunch = 18, kPunchTime = 19 };

constexpr int kBands = 10, kCuts = kBands - 1;
constexpr double kFLow = 150, kFHigh = 10000;
constexpr double kSqrt1Half = 0.7071067811865476;

using Biquad = std::array<double, 5>;

Biquad lowpass(double f, double sr, bool high = false) {
  const double w = 2 * kPi * std::min(f, sr * 0.45) / sr, c = js::cos(w), al = js::sin(w) / (2 * kSqrt1Half), a0 = 1 + al;
  const double b0 = (high ? 1 + c : 1 - c) / 2 / a0, b1 = (high ? -(1 + c) : 1 - c) / a0;
  return {b0, b1, b0, -2 * c / a0, (1 - al) / a0};
}

// The pan position (-1 left to 1 right) of every band for a shape and a rotation; `custom` reads the band values.
void bandPans(int shape, double rotate, const double* custom, double* out) {
  for (int k = 0; k < kBands; k++) {
    const double t = static_cast<double>(k) / (kBands - 1);
    if (shape == 0) out[k] = js::cos(kPi * (k + kBands * rotate));
    else if (shape == 1) { const double f = (k + 0.5) / kBands + rotate; out[k] = 2 * (f - std::floor(f)) - 1; }
    else if (shape == 2) out[k] = js::sin(4 * kPi * (t + rotate));
    else {
      const double p = std::fmod(std::fmod(k + kBands * rotate, kBands) + kBands, kBands), a = std::floor(p), f = p - a;
      const int ai = static_cast<int>(a);
      out[k] = custom[ai] + (custom[(ai + 1) % kBands] - custom[ai]) * f;
    }
  }
}

double tick(const Biquad& c, double* z, int o, double x) {
  const double y = c[0] * x + z[o];
  z[o] = c[1] * x - c[3] * y + z[o + 1];
  z[o + 1] = c[2] * x - c[4] * y;
  return y;
}

class SpecPan final : public Node {
public:
  explicit SpecPan(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        low_(lowpass(100, context.sampleRate)),
        high_(lowpass(100, context.sampleRate, true)),
        fastK_(1 - js::exp(-1 / (0.001 * context.sampleRate))),
        slowUp_(1 - js::exp(-1 / (0.03 * context.sampleRate))),
        slowDown_(1 - js::exp(-1 / (0.08 * context.sampleRate))) {
    for (int i = 0; i < kCuts; i++) cuts_[static_cast<std::size_t>(i)] = lowpass(kFLow * js::pow(kFHigh / kFLow, static_cast<double>(i) / (kCuts - 1)), context.sampleRate);
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inL = inputs[0];
    const float* inR = inputs[1];
    float* outL = outputs[0];
    float* outR = outputs[1];
    auto g = [&](int i) -> double { return params[slots[i]]; };
    const int shape = jsRoundInt(g(kShape));
    const double spread = g(kSpread), mix = g(kMix);
    const double lf = g(kLowMono);
    if (lf != lowF_) { lowF_ = lf; low_ = lowpass(lf, sampleRate_); high_ = lowpass(lf, sampleRate_, true); }
    for (int k = 0; k < kBands; k++) custom_[k] = g(kB1 + k);
    double rot = g(kRotate);
    const double rate = g(kRate);
    if (rate > 0) {   // the internal motion, free-running
      const int wave = jsRoundInt(g(kWave));
      phase_ += rate * n / sampleRate_; phase_ -= std::floor(phase_);
      const double v = wave == 0 ? js::sin(2 * kPi * phase_)
                     : wave == 1 ? (phase_ < 0.25 ? 4 * phase_ : phase_ < 0.75 ? 2 - 4 * phase_ : 4 * phase_ - 4)
                     : wave == 2 ? 2 * phase_ - 1 : (phase_ < 0.5 ? 1 : -1);
      rot += g(kSweep) * v;
    }
    // punch: look for an attack in this block; the image is pulled to the centre at once and opens again over punchTime
    const double punch = g(kPunch);
    bool hit = false;
    if (punch > 0) {
      for (int i = 0; i < n; i++) {
        const double a = std::abs(static_cast<double>(inL[i]) + inR[i]) * 0.5;
        fast_ += (a - fast_) * fastK_; slow_ += (a - slow_) * (a > slow_ ? slowUp_ : slowDown_);
        if (!hot_ && fast_ > slow_ * 2 + 0.01) { hot_ = true; hit = true; }
        else if (hot_ && fast_ < slow_ * 1.2 + 0.005) hot_ = false;
      }
      centre_ = hit ? 1 : centre_ * js::exp(-n / (0.001 * g(kPunchTime) * sampleRate_));
    } else {
      centre_ = 0;
    }
    const double open = 1 - punch * centre_;   // 1: the full spread, 0: everything in the middle
    bandPans(shape, rot, custom_, pans_);
    for (int k = 0; k < kBands; k++) {
      const double p = std::max(-1.0, std::min(1.0, pans_[k])), w = std::min(1.0, std::abs(p) * spread * open), a = (p + 1) * kPi / 4;
      const double gl = kSqrt2 * js::cos(a), gr = kSqrt2 * js::sin(a);
      target_[k * 4] = 1 - w + 0.5 * w * gl; target_[k * 4 + 1] = 0.5 * w * gl;
      target_[k * 4 + 2] = 0.5 * w * gr; target_[k * 4 + 3] = 1 - w + 0.5 * w * gr;
    }
    // (an attack moves the image at once, not over a few blocks)
    if (!started_ || hit) {
      for (int i = 0; i < kBands * 4; i++) cur_[i] = target_[i];
      started_ = true;
    } else {
      for (int i = 0; i < kBands * 4; i++) cur_[i] += (target_[i] - cur_[i]) * 0.3;
    }
    // c_k are the gains of band k (LP_k - LP_k-1); folded onto LP_k: (c_k - c_k+1), the top band takes the input itself
    for (int k = 0; k <= kBands - 1; k++)
      for (int j = 0; j < 4; j++) coef_[k * 4 + j] = cur_[k * 4 + j] - (k + 1 < kBands ? cur_[(k + 1) * 4 + j] : 0);
    for (int i = 0; i < n; i++) {
      const double l = inL[i], r = inR[i];
      const double lo = tick(low_, zl_, kCuts * 4 + 2, tick(low_, zl_, kCuts * 4, l)), ro = tick(low_, zr_, kCuts * 4 + 2, tick(low_, zr_, kCuts * 4, r)), m = (lo + ro) * 0.5;
      const double xl = tick(high_, zl_, kCuts * 4 + 6, tick(high_, zl_, kCuts * 4 + 4, l)), xr = tick(high_, zr_, kCuts * 4 + 6, tick(high_, zr_, kCuts * 4 + 4, r));
      for (int k = 0; k < kCuts; k++) {
        const Biquad& cut = cuts_[static_cast<std::size_t>(k)];
        lpL_[k] = tick(cut, zl_, k * 4 + 2, tick(cut, zl_, k * 4, xl));
        lpR_[k] = tick(cut, zr_, k * 4 + 2, tick(cut, zr_, k * 4, xr));
      }
      lpL_[kCuts] = xl; lpR_[kCuts] = xr;
      double ol = m, orr = m;
      for (int k = 0; k < kBands; k++) {
        const double a = coef_[k * 4], b = coef_[k * 4 + 1], c = coef_[k * 4 + 2], d = coef_[k * 4 + 3];
        ol += a * lpL_[k] + b * lpR_[k];
        orr += c * lpL_[k] + d * lpR_[k];
      }
      outL[i] = static_cast<float>(l + (ol - l) * mix);
      outR[i] = static_cast<float>(r + (orr - r) * mix);
    }
    for (int i = 0; i < kStates; i++) {
      if (std::abs(zl_[i]) < 1e-20) zl_[i] = 0;
      if (std::abs(zr_[i]) < 1e-20) zr_[i] = 0;
    }
  }

private:
  static constexpr int kStates = kCuts * 4 + 8;

  double sampleRate_;
  std::array<Biquad, kCuts> cuts_{};
  Biquad low_, high_;
  double zl_[kStates] = {}, zr_[kStates] = {};   // two stages (24 dB/oct) for every crossover; the low-mono split is a Linkwitz-Riley pair
  double lowF_ = -1;
  double cur_[kBands * 4] = {}, target_[kBands * 4] = {}, pans_[kBands] = {}, custom_[kBands] = {};
  double lpL_[kBands] = {}, lpR_[kBands] = {}, coef_[(kBands + 1) * 4] = {};
  bool started_ = false;
  double phase_ = 0;
  // the attack detector for `punch`: a fast and a slow follower of the level
  double fastK_, slowUp_, slowDown_;
  double fast_ = 0, slow_ = 0, centre_ = 0;
  bool hot_ = false;
};

}  // namespace

void registerSpecPan(NodeRegistry& registry) {
  NodeType type;
  type.type = "specpan";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.isEffect = true;
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.params = {
    {"shape", 0, 3, 0, true},
    {"spread", 0, 1, 0.6},
    {"rotate", -1, 1, 0},
    {"lowMono", 20, 400, 100},
  };
  type.params.push_back({"mix", 0, 1, 1});
  for (int i = 0; i < kBands; i++) type.params.push_back({"b" + std::to_string(i + 1), -1, 1, 0});
  type.params.push_back({"rate", 0, 20, 0});         // Hz; 0 holds the image still
  type.params.push_back({"sweep", 0, 2, 0.25});      // how far the motion carries `rotate`
  type.params.push_back({"wave", 0, 3, 0, true});    // sine, triangle, saw, square
  type.params.push_back({"punch", 0, 1, 0});
  type.params.push_back({"punchTime", 5, 200, 40});  // ms
  type.create = [](const NodeContext& context) { return std::make_unique<SpecPan>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

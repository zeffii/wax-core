// Hall reverb: a feedback delay network of eight lines with a Householder mixing matrix (lossless, so the decay is set
// only by the per-line gains), a lowpass in every line for the damping, slow modulation of the read positions against
// metallic ringing, and a chain of four allpass diffusers plus a pre-delay on the way in. The input is summed to mono;
// the two outputs take different sign patterns of the lines, which makes the wet signal stereo.
// out = dry * (1 - mix) + wet * mix.
// Port of webtracker src/audio/nodes/hall.js.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kSize = 0, kDecay, kDamp, kPredelay, kDiffusion, kMod, kWidth, kMix };

constexpr int kLines = 8;
constexpr double kBaseMs[kLines] = {29.7, 37.1, 41.1, 43.7, 53.0, 61.3, 67.7, 73.9};
constexpr int kDiffusers44k[4] = {142, 107, 379, 277};
constexpr double kSignIn[kLines] = {1, -1, 1, -1, -1, 1, -1, 1};
constexpr double kSignLeft[kLines] = {1, 1, -1, -1, 1, 1, -1, -1}, kSignRight[kLines] = {1, -1, 1, -1, -1, 1, -1, 1};

class Hall final : public Node {
public:
  explicit Hall(const NodeContext& context) : sampleRate_(context.sampleRate), ms_(context.sampleRate / 1000) {
    maxDepth_ = 40 * sampleRate_ / 48000;
    size_ = static_cast<int>(std::ceil(kBaseMs[kLines - 1] * 2 * ms_ + maxDepth_)) + 8;
    lines_.assign(static_cast<std::size_t>(kLines * size_), 0.0f);   // the eight lines one after another
    for (int i = 0; i < kLines; i++) {
      phase_[i] = static_cast<double>(i) / kLines;
      rate_[i] = (0.31 + 0.037 * i) / sampleRate_;
    }
    preSize_ = static_cast<int>(std::ceil(0.2 * sampleRate_)) + 4;
    pre_.assign(static_cast<std::size_t>(preSize_), 0.0f);
    for (int q = 0; q < 4; q++) {
      diffusers_[q].length = std::max(2, jsRoundInt(kDiffusers44k[q] * sampleRate_ / 44100));
      diffusers_[q].buf.assign(static_cast<std::size_t>(diffusers_[q].length), 0.0f);
    }
    slew_ = 1 - std::exp(-1 / (sampleRate_ * 0.05));
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inLeft = inputs[0];
    const float* inRight = inputs[1];
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    const double sz = params[slots[kSize]], rt60 = std::min(20.0, static_cast<double>(params[slots[kDecay]]));
    const double openness = 1 - static_cast<double>(params[slots[kDamp]]);
    const double fc = std::max(800.0, 20000 * (openness * openness));
    const double lpCoef = 1 - std::exp(-2 * kPi * fc / sampleRate_);
    const double preTarget = std::min(params[slots[kPredelay]] * ms_, static_cast<double>(preSize_ - 3));
    const double g = 0.15 + 0.5 * params[slots[kDiffusion]];
    const double depth = params[slots[kMod]] * maxDepth_, width = params[slots[kWidth]], mix = params[slots[kMix]];
    for (int k = 0; k < kLines; k++) {
      len_[k] = kBaseMs[k] * sz * ms_;
      gain_[k] = std::pow(10.0, (-3 * len_[k]) / (sampleRate_ * rt60));   // 60 dB down after rt60 seconds
    }
    if (preCur_ < 0) preCur_ = preTarget;
    // The modulation moves a few times a second, so its sine is worked out at the start and end of the block and the
    // read position slides between them, instead of eight sines for every sample.
    const double inv = 1.0 / n;
    for (int k = 0; k < kLines; k++) {
      const double a0 = len_[k] + depth * (0.5 + 0.5 * std::sin(kTwoPi * phase_[k]));
      phase_[k] += rate_[k] * n;
      if (phase_[k] >= 1) phase_[k] -= 1;
      const double a1 = len_[k] + depth * (0.5 + 0.5 * std::sin(kTwoPi * phase_[k]));
      at_[k] = a0;
      step_[k] = (a1 - a0) * inv;
    }
    for (int i = 0; i < n; i++) {
      const float l = inLeft[i], r = inRight[i];
      preCur_ += (preTarget - preCur_) * slew_;
      pre_[static_cast<std::size_t>(pw_)] = static_cast<float>((static_cast<double>(l) + r) * 0.5);
      double rp = pw_ - preCur_;
      if (rp < 0) rp += preSize_;
      const int pk = static_cast<int>(rp);
      const double pf = rp - pk;
      const double p0 = pre_[static_cast<std::size_t>(pk)], p1 = pre_[static_cast<std::size_t>(pk + 1 == preSize_ ? 0 : pk + 1)];
      double x = p0 + (p1 - p0) * pf;
      if (++pw_ == preSize_) pw_ = 0;
      for (Diffuser& d : diffusers_) {   // Schroeder allpass: v = x - g * v[n-D]; y = g * v + v[n-D]
        const double delayed = d.buf[static_cast<std::size_t>(d.pos)], v = x - g * delayed;
        d.buf[static_cast<std::size_t>(d.pos)] = static_cast<float>(std::abs(v) < 1e-20 ? 0 : v);
        x = g * v + delayed;
        if (++d.pos == d.length) d.pos = 0;
      }
      double sum = 0, wl = 0, wr = 0;
      for (int k = 0; k < kLines; k++) {
        double rr = w_ - at_[k];
        if (rr < 0) rr += size_;
        const int o = k * size_, ki = static_cast<int>(rr);
        const double f = rr - ki;
        const int k1 = ki + 1 == size_ ? 0 : ki + 1;
        const double a0 = lines_[static_cast<std::size_t>(o + ki)];
        const double v = a0 + (lines_[static_cast<std::size_t>(o + k1)] - a0) * f;
        at_[k] += step_[k];
        const double dk = damp_[k] + lpCoef * (v - damp_[k]);
        damp_[k] = dk;
        const double yk = gain_[k] * dk;
        y_[k] = yk;
        sum += yk;
        wl += kSignLeft[k] * dk;
        wr += kSignRight[k] * dk;
      }
      const double mixTerm = (2.0 / kLines) * sum;
      for (int k = 0; k < kLines; k++) {
        const double v = x * 0.35 * kSignIn[k] + y_[k] - mixTerm;   // Householder: y - (2/N) * sum
        lines_[static_cast<std::size_t>(k * size_ + w_)] = static_cast<float>(std::abs(v) < 1e-20 ? 0 : v);
      }
      if (++w_ == size_) w_ = 0;
      wl *= 0.5;
      wr *= 0.5;
      const double mid = (wl + wr) * 0.5;
      wl = mid + (wl - mid) * width;
      wr = mid + (wr - mid) * width;
      outLeft[i] = static_cast<float>(l + (wl - l) * mix);
      outRight[i] = static_cast<float>(r + (wr - r) * mix);
    }
  }

private:
  struct Diffuser {
    std::vector<float> buf;
    int pos = 0;
    int length = 2;
  };

  double sampleRate_, ms_, maxDepth_ = 0, slew_ = 0;
  int size_ = 0;
  std::vector<float> lines_;
  double damp_[kLines] = {}, phase_[kLines], rate_[kLines];
  double gain_[kLines] = {}, len_[kLines] = {}, y_[kLines] = {}, at_[kLines] = {}, step_[kLines] = {};
  int w_ = 0;
  int preSize_ = 0;
  std::vector<float> pre_;
  int pw_ = 0;
  double preCur_ = -1;
  Diffuser diffusers_[4];
};

}  // namespace

void registerHall(NodeRegistry& registry) {
  NodeType type;
  type.type = "hall";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {
    {"size", 0.5, 2, 1},
    {"decay", 0.2, 20, 2.5},        // seconds to fall by 60 dB
    {"damp", 0, 1, 0.4},
    {"predelay", 0, 200, 20},       // ms
    {"diffusion", 0, 1, 0.7},
    {"mod", 0, 1, 0.3},
    {"width", 0, 1, 1},
    {"mix", 0, 1, 0.3},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Hall>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

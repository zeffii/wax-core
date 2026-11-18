// filterPro: one filter node with twelve voicings, per-sample cutoff modulation, input drive with bias, and a dry/wet
// mix. The SVF voicings are zero-delay (TPT) with a soft-clipped band state, so high resonance stays bounded and
// self-oscillates. The panel's picture of the spectrum (the node reports it while the panel is open) is left to the
// window. Port of webtracker src/audio/nodes/filterpro.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/filterpro_response.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kCut = 0, kRes, kMode, kMorph, kVowel, kDamp, kDrive, kBias, kMix, kLevel };

using filterpro::kFormants;
using filterpro::kFormantGain;

// smooth tanh-like, exact at +-3
double clip(double x) { return x > 3 ? 1 : x < -3 ? -1 : x * (27 + x * x) / (27 + 9 * x * x); }

// The copies of the node (one per voice) share this: only the newest one reports to the picture.
struct Reporter { const void* who = nullptr; };

class FilterPro final : public Node {
public:
  explicit FilterPro(const NodeContext& context)
      : probe_(context.probe), sr_(context.sampleRate),
        cs_(1 - js::exp(-4 / (context.sampleRate * 0.004))),
        modOn_(context.connected.size() > 1 && context.connected[1]),
        buf_(static_cast<std::size_t>(std::ceil(context.sampleRate / 15)) + 8, 0.0f) {
    if (context.shared) {
      if (!*context.shared) *context.shared = std::make_shared<Reporter>();
      reporter_ = std::static_pointer_cast<Reporter>(*context.shared);
      reporter_->who = this;
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* input = inputs[0];
    const float* mod = inputs[1];
    float* out = outputs[0];
    const double sr = sr_;
    const double base = params[slots[kCut]], res = std::min(std::max(static_cast<double>(params[slots[kRes]]), 0.0), 1.0);
    const int mode = jsRoundInt(params[slots[kMode]]);
    const double morph = params[slots[kMorph]], vowel = params[slots[kVowel]], damp = params[slots[kDamp]];
    const double drive = params[slots[kDrive]], bias = params[slots[kBias]], mix = params[slots[kMix]], level = params[slots[kLevel]];
    const double pre = 1 + drive * 9, post = 1 / (1 + drive * 2), b = bias * 0.8, bOff = clip(b);
    const bool driven = drive > 0.001 || bias != 0;
    const double k = 2 - 2.04 * res, kb = std::max(k, 0.02), kick = res > 0.97 ? 1e-5 : 0;
    const int vi = std::min(static_cast<int>(std::floor(vowel)), 3);
    const double vf = vowel - vi;
    double fa[3] = {0, 0, 0}, fb[3] = {0, 0, 0}, fc3[3] = {0, 0, 0};
    const double fk = 1 / (2 + res * 18);
    const int bufLen = static_cast<int>(buf_.size());
    for (int i = 0; i < n; i += 4) {
      const int m = std::min(4, n - i);
      double target = base;
      if (modOn_) target *= js::pow(2, std::max(-8.0, std::min(8.0, static_cast<double>(mod[i]))));
      if (fc_ < 0) fc_ = target;
      fc_ += (target - fc_) * cs_;
      const double f = std::min(std::max(fc_, 5.0), sr * 0.45);
      const double g = js::tan(kPi * f / sr);
      const double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
      double b1 = 0, b2 = 0, b3 = 0, G = 0, kl = 0, delay = 0, fbk = 0;
      if (mode == 7) {
        const double s = 1.4142;
        b1 = 1 / (1 + g * (g + s)); b2 = g * b1; b3 = g * b2;
      } else if (mode == 8) {
        const double g2 = js::tan(kPi * f / (2 * sr));
        G = g2 / (1 + g2); kl = res * 4.3;
      } else if (mode == 9) {
        const double scale = f / 1000;
        for (int q = 0; q < 3; q++) {
          const double fq = std::min((kFormants[vi][q] * (1 - vf) + kFormants[vi + 1][q] * vf) * scale, sr * 0.45);
          const double gq = js::tan(kPi * fq / sr), aq = 1 / (1 + gq * (gq + fk));
          fa[q] = aq; fb[q] = gq * aq; fc3[q] = gq * fb[q];
        }
      } else if (mode >= 10) {
        delay = std::min(std::max(sr / f, 2.0), static_cast<double>(bufLen - 4));
        fbk = (mode == 10 ? 1 : -1) * res * 0.97;
      }
      for (int j = i; j < i + m; j++) {
        const double dry = input[j];
        double x = driven ? (clip(pre * dry + b) - bOff) * post : dry;
        if (kick != 0 && mode <= 8) {   // lets a resonance at 1 start singing
          rng_ = static_cast<std::uint32_t>(static_cast<std::uint64_t>(rng_) * 1664525u + 1013904223u);
          x += (rng_ / 4294967296.0 - 0.5) * kick;
        }
        double y;
        if (mode <= 6) {
          step(0, x, a1, a2, a3);
          const double bp = kb * v1_, lp = v2_, hp = x - k * v1_ - lp;
          switch (mode) {
            case 0: y = lp; break;
            case 1: y = bp; break;
            case 2: y = hp; break;
            case 3: y = lp + hp; break;
            case 4: y = lp - hp; break;
            case 5: y = x - 2 * bp; break;
            default: {
              const double wl = std::max(0.0, 1 - 2 * morph), wn = 1 - std::abs(2 * morph - 1), wh = std::max(0.0, 2 * morph - 1);
              y = wl * lp + wn * (lp + hp) + wh * hp;
            }
          }
        } else if (mode == 7) {
          step(0, x, b1, b2, b3);
          step(2, v2_, a1, a2, a3);
          y = v2_;
        } else if (mode == 8) {
          double s = 0;
          for (int h = 0; h < 2; h++) {
            double u = clip(x - kl * z_[3]);
            for (int p = 0; p < 4; p++) { const double v = (u - z_[p]) * G; const double o = v + z_[p]; z_[p] = o + v; u = o; }
            s = u;
          }
          y = s * (1 + res * 0.7);
        } else if (mode == 9) {
          y = 0;
          for (int q = 0; q < 3; q++) {
            step(4 + 2 * q, x, fa[q], fb[q], fc3[q]);   // only the band is used, but the low-pass state needs its third term
            y += 4 * kFormantGain[q] * fk * v1_;
          }
        } else {
          double rp = wp_ - delay;
          if (rp < 0) rp += bufLen;
          const int i0 = static_cast<int>(std::floor(rp));
          const double fr = rp - i0;
          const int i1 = i0 + 1 >= bufLen ? 0 : i0 + 1;
          const double d = buf_[static_cast<std::size_t>(i0)] * (1 - fr) + buf_[static_cast<std::size_t>(i1)] * fr;
          damped_ += (d - damped_) * (1 - damp * 0.9);
          y = x + fbk * damped_;
          buf_[static_cast<std::size_t>(wp_)] = static_cast<float>(y);
          wp_ = wp_ + 1 >= bufLen ? 0 : wp_ + 1;
          y *= 1 - std::abs(fbk) * 0.5;
        }
        out[j] = static_cast<float>((dry + (y - dry) * mix) * level);
      }
    }
    for (int q = 0; q < 16; q++) if (std::abs(st_[q]) < 1e-20) st_[q] = 0;
    for (int q = 0; q < 4; q++) if (std::abs(z_[q]) < 1e-20) z_[q] = 0;
    if (std::abs(damped_) < 1e-20) damped_ = 0;
    if (probe_ && probe_->watched() && (!reporter_ || reporter_->who == this)) {   // the picture: the last frame of what went in and what came out
      for (int i = 0; i < n; i++) {
        heardIn_[static_cast<std::size_t>(heard_)] = input[i];
        heardOut_[static_cast<std::size_t>(heard_)] = out[i];
        heard_ = heard_ + 1 >= kFrame ? 0 : heard_ + 1;
      }
      sinceReport_ += n;
      if (sinceReport_ >= kReportEvery) {
        sinceReport_ = 0;
        for (int i = 0; i < kFrame; i++) {   // oldest first, ready for a window and an FFT
          frameIn_[static_cast<std::size_t>(i)] = heardIn_[static_cast<std::size_t>((heard_ + i) % kFrame)];
          frameOut_[static_cast<std::size_t>(i)] = heardOut_[static_cast<std::size_t>((heard_ + i) % kFrame)];
        }
        probe_->publish(frameIn_, frameOut_);
      }
    }
  }

private:
  // One TPT state-variable step on the state at offset o; leaves band in v1_ and low in v2_.
  void step(int o, double x, double a1, double a2, double a3) {
    const double v3 = x - st_[o + 1];
    double v1 = a1 * st_[o] + a2 * v3;
    v1 = 2 * clip(0.5 * v1);
    const double v2 = st_[o + 1] + a2 * st_[o] + a3 * v3;
    st_[o] = 2 * v1 - st_[o];
    st_[o + 1] = 2 * v2 - st_[o + 1];
    v1_ = v1; v2_ = v2;
  }

  static constexpr int kFrame = 2048, kReportEvery = 2400;   // a frame of sound, about every 50 ms
  std::shared_ptr<SpectrumProbe> probe_;
  std::shared_ptr<Reporter> reporter_;
  std::vector<float> heardIn_ = std::vector<float>(kFrame, 0.0f), heardOut_ = std::vector<float>(kFrame, 0.0f), frameIn_ = std::vector<float>(kFrame, 0.0f), frameOut_ = std::vector<float>(kFrame, 0.0f);
  int heard_ = 0, sinceReport_ = 0;
  double sr_, cs_;
  bool modOn_;
  double st_[16] = {};   // 0 main, 2 second stage, 4..9 three formants
  double z_[4] = {0, 0, 0, 0};
  std::vector<float> buf_;
  int wp_ = 0;
  double fc_ = -1, damped_ = 0;
  std::uint32_t rng_ = 22222;
  double v1_ = 0, v2_ = 0;
};

}  // namespace

void registerFilterPro(NodeRegistry& registry) {
  NodeType type;
  type.type = "filterpro";
  type.inputs = {"in", "mod"};   // mod is in octaves, applied sample by sample
  type.outputs = {"out"};
  type.dual = true;
  type.isEffect = true;
  type.kinds = {{"mod", PortKind::Control}};
  type.params = {
    {"cutoff", 20, 18000, 1500},
    {"resonance", 0, 1, 0.25},
    {"mode", 0, 11, 0, true},
    {"morph", 0, 1, 0},
    {"vowel", 0, 4, 0},
    {"damp", 0, 1, 0.3},
    {"drive", 0, 1, 0},
    {"bias", -1, 1, 0},
    {"mix", 0, 1, 1},
    {"level", 0, 2, 1},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<FilterPro>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Vocoder: the spectrum of the `voice` input is measured in a bank of bandpass filters, and each band's loudness is
// imposed on the same band of the `carrier` input (or of a built-in buzz when nothing is wired to carrier). Bands are
// log-spaced between low and high; analysis and synthesis are two-pole bandpasses, two in series each. `shift` moves the synthesis bands up or down against the analysis bands
// (formant shift), `sibilance` lets the voice's own highs through so consonants stay crisp.
// Port of webtracker src/audio/nodes/vocoder.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kBands = 0, kLow, kHigh, kQ, kAttack, kRelease, kShift, kSibilance, kTune, kNoise, kLevel };

constexpr int kMaxBands = 32;

// coefficients [a1, a2, a3, k] of a bandpass at f
void setBand(double* c, double f, double q, double sr) {
  const double g = js::tan(kPi * std::min(f, sr * 0.45) / sr), k = 1 / q, a1 = 1 / (1 + g * (g + k));
  c[0] = a1;
  c[1] = g * a1;
  c[2] = g * g * a1;
  c[3] = k;
}

// one bandpass step with unity peak gain; c holds the coefficients, s the two states
double bandpass(const double* c, double* s, double x) {
  const double v3 = x - s[1], v1 = c[0] * s[0] + c[1] * v3, v2 = s[1] + c[1] * s[0] + c[2] * v3;
  s[0] = 2 * v1 - s[0];
  s[1] = 2 * v2 - s[1];
  return v1 * c[3];
}

class Vocoder final : public Node {
public:
  explicit Vocoder(const NodeContext& context)
      : sr_(context.sampleRate), carrierOn_(context.connected.size() > 1 && context.connected[1]) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* voice = inputs[0];
    const float* carrier = inputs[1];
    float* out = outputs[0];
    const int bands = jsRoundInt(params[slots[kBands]]);
    const double low = params[slots[kLow]], high = std::max(static_cast<double>(params[slots[kHigh]]), low * 2);
    const double hz = params[slots[kTune]], noiseAmt = params[slots[kNoise]], dt = hz / sr_;
    const double q = params[slots[kQ]], shift = params[slots[kShift]], sib = params[slots[kSibilance]];
    if (!set_ || bands != bands_ || low != low_ || high != high_ || q != q_ || shift != shift_) {
      set_ = true;
      bands_ = bands; low_ = low; high_ = high; q_ = q; shift_ = shift;
      const double ratio = js::pow(high / low, 1.0 / std::max(bands - 1, 1)), sh = js::pow(2.0, shift / 12);
      // wider bands when there are fewer of them, so the spectrum stays covered
      const double qq = q * std::min(1.0, 16.0 / bands) * std::max(1.0, std::sqrt(bands / 16.0));
      for (int b = 0; b < bands && b < kMaxBands; b++) {
        const double f = low * js::pow(ratio, b);
        setBand(analysisCoef_ + b * 4, f, qq, sr_);
        setBand(synthCoef_ + b * 4, f * sh, qq, sr_);
      }
      std::fill(std::begin(analysisState_), std::end(analysisState_), 0.0);
      std::fill(std::begin(synthState_), std::end(synthState_), 0.0);
    }
    const int nb = std::min(bands_, kMaxBands);
    const double att = js::exp(-1 / (sr_ * params[slots[kAttack]] / 1000)), rel = js::exp(-1 / (sr_ * params[slots[kRelease]] / 1000));
    const double level = params[slots[kLevel]] * 10 * std::sqrt(16.0 / bands);
    const double gHp = js::tan(kPi * 5000 / sr_), hk = 1.4142, ha1 = 1 / (1 + gHp * (gHp + hk));
    for (int i = 0; i < n; i++) {
      const double x = voice[i];
      double c;
      if (carrierOn_) {
        c = carrier[i];
      } else {   // built-in carrier: a band-limited sawtooth plus noise
        phase_ += dt;
        if (phase_ >= 1) phase_ -= 1;
        double saw = 2 * phase_ - 1;
        if (phase_ < dt) {
          const double t = phase_ / dt;
          saw -= t + t - t * t - 1;
        } else if (phase_ > 1 - dt) {
          const double t = (phase_ - 1) / dt;
          saw -= t * t + t + t + 1;
        }
        rng_ = rng_ * 1664525u + 1013904223u;
        c = saw * 0.6 + noiseAmt * 0.6 * (rng_ / 2147483648.0 - 1);
      }
      double sum = 0;
      for (int b = 0; b < nb; b++) {
        const int o = b * 4;
        double a = bandpass(analysisCoef_ + o, analysisState_ + o, x);
        a = bandpass(analysisCoef_ + o, analysisState_ + o + 2, a);
        const double r = a < 0 ? -a : a, e = env_[b];
        env_[b] = r > e ? r + att * (e - r) : r + rel * (e - r);
        double s = bandpass(synthCoef_ + o, synthState_ + o, c);
        s = bandpass(synthCoef_ + o, synthState_ + o + 2, s);
        sum += s * env_[b];
      }
      // highs of the voice, straight through (a highpass at 5 kHz)
      const double v3 = x - hp_[1], v1 = ha1 * hp_[0] + gHp * ha1 * v3, v2 = hp_[1] + gHp * ha1 * hp_[0] + gHp * gHp * ha1 * v3;
      hp_[0] = 2 * v1 - hp_[0];
      hp_[1] = 2 * v2 - hp_[1];
      const double high5 = x - hk * v1 - v2;
      out[i] = static_cast<float>(sum * level + high5 * sib);
    }
  }

private:
  double sr_;
  bool carrierOn_;      // something is wired to carrier; otherwise the built-in buzz plays
  double phase_ = 0;
  uint32_t rng_ = 12345;
  // per band: analysis (two stages) and synthesis (two stages) states [ic1, ic2] and coefficients [a1, a2, a3, k]
  double analysisState_[kMaxBands * 4] = {}, synthState_[kMaxBands * 4] = {}, env_[kMaxBands] = {};
  double analysisCoef_[kMaxBands * 4] = {}, synthCoef_[kMaxBands * 4] = {};
  double hp_[2] = {0, 0};
  bool set_ = false;   // the bands have been laid out for the settings below
  int bands_ = 0;
  double low_ = 0, high_ = 0, q_ = 0, shift_ = 0;
};

}  // namespace

void registerVocoder(NodeRegistry& registry) {
  NodeType type;
  type.type = "vocoder";
  type.inputs = {"voice", "carrier"};
  type.outputs = {"out"};
  type.isEffect = true;
  type.params = {
    {"bands", 4, 32, 16, true},
    {"low", 40, 1000, 120},
    {"high", 2000, 18000, 8000},
    {"q", 1, 20, 5},
    {"attack", 0.5, 50, 3},
    {"release", 5, 500, 60},
    {"shift", -12, 12, 0},
    {"sibilance", 0, 1, 0.4},
    {"tune", 30, 800, 110},
    {"noise", 0, 1, 0.15},
    {"level", 0, 4, 1},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Vocoder>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

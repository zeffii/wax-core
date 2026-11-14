// The west-coast building blocks, shared by the wavefolder, pluck gate and west-coast voice nodes, so the voice sounds
// the same as the same parts patched by hand.
// Port of webtracker src/audio/westcore.js.
#pragma once

#include <algorithm>
#include <cmath>

#include "nodes/shared_dsp.h"

namespace wax {

inline constexpr double kFoldGain = 10;   // how many times louder the wave is pushed into the folder at fold = 1

// The wave folder: a wave pushed past the edge turns round and comes back, like a sine, once for each time it
// overshoots. `amount` 0..1 sets how hard the wave is pushed (1 to 11 times), `offset` moves it off centre first, so
// the folds come out uneven (that adds the even harmonics).
inline double foldWave(double v, double amount, double offset) {
  return std::sin((1 + amount * kFoldGain) * (v + offset) * (kPi / 2));
}

// Takes out the slow drift a folded, off-centre wave carries. About 4 Hz at 48 kHz.
class DcBlock {
public:
  double run(double v) {
    const double out = v - x_ + 0.9995 * y_;
    x_ = v;
    y_ = std::abs(out) < 1e-18 ? 0 : out;
    return y_;
  }
  void reset() { x_ = 0; y_ = 0; }

private:
  double x_ = 0, y_ = 0;
};

// The pluck gate: a vactrol (a lamp and a photocell) that opens quickly and lets go slowly, driving a low-pass filter
// and the gain together. A struck sound dies with its top end first. mode 0: both, 1: gain only, 2: filter only.
class PluckGate {
public:
  explicit PluckGate(double sampleRate) : sr_(sampleRate) {}

  // once per block. bright 0..1 is the highest cutoff (200 Hz to 25 kHz); `semis` is how far the cutoff is moved by the
  // note's pitch (already multiplied by tracking).
  void set(int mode, double bright, double res, double openMs, double closeMs, double semis) {
    mode_ = mode;
    k_ = 2 - 2 * std::min(0.95, std::max(0.0, res));
    fmax_ = std::min(sr_ * 0.45, 200 * std::pow(2.0, bright * 7) * std::pow(2.0, semis / 12));
    openCoef_ = 1 - std::exp(-1 / (sr_ * std::max(0.05, openMs) * 0.001));
    closeCoef_ = 1 - std::exp(-1 / (sr_ * std::max(1.0, closeMs) * 0.001));
    slowCoef_ = 1 - std::exp(-1 / (sr_ * std::max(1.0, closeMs) * 0.004));   // the photocell's slow part lets go four times slower
  }

  bool idle() const { return level_ < 1e-5; }

  void reset() { level_ = 0; fast_ = 0; slow_ = 0; s1_ = 0; s2_ = 0; }

  // one sample of sound in; `strike` 0..1 is how hard the lamp is lit
  double run(double x, double strike) {
    constexpr double fmin = 25;
    fast_ += (strike - fast_) * (strike > fast_ ? openCoef_ : closeCoef_);
    slow_ += (strike - slow_) * (strike > slow_ ? openCoef_ : slowCoef_);
    const double lvl = level_ = fast_ * 0.75 + slow_ * 0.25;
    if (mode_ == 1) return x * std::pow(lvl, 1.4);
    if ((count_++ & 7) == 0) {   // the filter's coefficients are worked out every 8 samples
      const double fc = fmin * std::pow(fmax_ / fmin, std::min(1.0, std::pow(lvl, 0.7)));
      const double g = std::tan(kPi * std::min(sr_ * 0.45, fc) / sr_);
      a1_ = 1 / (1 + g * (g + k_));
      a2_ = g * a1_;
      a3_ = g * a2_;
    }
    const double v3 = x - s2_, v1 = a1_ * s1_ + a2_ * v3, v2 = s2_ + a2_ * s1_ + a3_ * v3;
    s1_ = 2 * v1 - s1_;
    s2_ = 2 * v2 - s2_;
    if (std::abs(s1_) < 1e-18) s1_ = 0;
    if (std::abs(s2_) < 1e-18) s2_ = 0;
    return mode_ == 2 ? v2 : v2 * std::pow(lvl, 1.4);
  }

private:
  double sr_;
  double level_ = 0;                 // the vactrol, 0..1 (the fast part and the slow part together)
  double fast_ = 0, slow_ = 0;
  double s1_ = 0, s2_ = 0;           // the filter's state
  double a1_ = 0, a2_ = 0, a3_ = 0, k_ = 2;
  double fmax_ = 8000;
  int mode_ = 0;
  double openCoef_ = 0.1, closeCoef_ = 0.001, slowCoef_ = 0.001;
  unsigned count_ = 0;
};

}  // namespace wax

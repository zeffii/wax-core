// A two-pole low-pass filter that stays stable while its cutoff is swept quickly (a state variable filter in the trapezoidal
// form). set() once per block, or every few samples while the cutoff moves; next() per sample. Port of webtracker
// src/audio/nodes/svf.js.
#pragma once

#include <algorithm>

#include "core/js_math.h"
#include "nodes/shared_dsp.h"

namespace wax {

class LowPassSvf {
public:
  explicit LowPassSvf(double sampleRate) : sampleRate_(sampleRate) { set(20000, 0); }

  // cutoff in Hz; resonance 0 (none) to 1 (just short of ringing on its own)
  void set(double cutoff, double resonance) {
    const double g = js::tan(kPi * std::min(0.49, std::max(10.0, cutoff) / sampleRate_));
    const double k = 2 - 1.96 * std::min(1.0, std::max(0.0, resonance));
    a1_ = 1 / (1 + g * (g + k));
    a2_ = g * a1_;
    a3_ = g * a2_;
  }

  void reset() { s1_ = 0; s2_ = 0; }

  double next(double input) {
    const double v3 = input - s2_, v1 = a1_ * s1_ + a2_ * v3, v2 = s2_ + a2_ * s1_ + a3_ * v3;
    s1_ = 2 * v1 - s1_;
    s2_ = 2 * v2 - s2_;
    return v2;
  }

private:
  double sampleRate_;
  double s1_ = 0, s2_ = 0, a1_ = 0, a2_ = 0, a3_ = 0;
};

}  // namespace wax

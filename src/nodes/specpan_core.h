// The pan of each band of the spectral panner for a shape and a rotation, and where the bands are split: shared by the node and its picture
// (port of the exported helpers of webtracker src/audio/nodes/specpan.js).
#pragma once

#include <array>
#include <cmath>

#include "core/js_math.h"

namespace wax::specpan {

inline constexpr int kBands = 10, kCuts = kBands - 1;
inline constexpr double kFLow = 150, kFHigh = 10000;
inline constexpr double kPi = 3.141592653589793;

// The pan position (-1 left to 1 right) of every band for a shape (alternate, sweep, wave, custom) and a rotation; `custom` reads the band values.
inline void bandPans(int shape, double rotate, const double* custom, double* out) {
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

// The frequencies where the ten bands are split.
inline std::array<double, kCuts> crossovers() {
  std::array<double, kCuts> cuts{};
  for (int i = 0; i < kCuts; i++) cuts[static_cast<std::size_t>(i)] = kFLow * std::pow(kFHigh / kFLow, static_cast<double>(i) / (kCuts - 1));
  return cuts;
}

}  // namespace wax::specpan

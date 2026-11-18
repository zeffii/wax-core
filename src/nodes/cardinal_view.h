// The parts of Cardinal that its picture shares with the node (port of the exported helpers of webtracker src/audio/nodes/cardinal.js):
// the filter's gain at a frequency, and the shape of an LFO.
#pragma once

#include <cmath>
#include <functional>

#include "core/js_math.h"

namespace wax::cardinal {

inline constexpr const char* kWaves[] = {"triangle", "saw", "pulse", "sine", "noise"};
inline constexpr const char* kFilters[] = {"lowpass 24", "lowpass 12", "bandpass", "highpass", "notch"};
inline constexpr const char* kLfoWaves[] = {"sine", "triangle", "saw", "square", "random"};
inline constexpr const char* kLfoDests[] = {"pitch 1+2", "pitch 2", "fm", "pulse width", "filter", "level"};

// One LFO's value (-1..1) at phase p (0..1); `held` is the random wave's current value.
inline double lfoShape(int wave, double p, double held) {
  switch (wave) {
    case 0: return js::sin(2 * 3.14159265358979323846 * p);
    case 1: return p < 0.5 ? 4 * p - 1 : 3 - 4 * p;
    case 2: return 2 * p - 1;
    case 3: return p < 0.5 ? 1 : -1;
    default: return held;
  }
}

// The gain (1 is no change) of the filter type `type` with resonance `res` at `hz`, for a cutoff `cutoff`, from the analogue prototype each type is built on.
inline double filterResponse(int type, double cutoff, double res, double hz) {
  const double x = hz / cutoff;
  if (type == 0) {   // four poles with the resonance fed back around them
    double re = 1, im = 0;   // (1 + jx) to the fourth power
    for (int i = 0; i < 4; i++) { const double nr = re - im * x, ni = re * x + im; re = nr; im = ni; }
    return 1 / std::hypot(re + res * 4, im);
  }
  const double k = 2 - 1.95 * res, re = 1 - x * x, im = k * x, den = std::hypot(re, im);
  if (type == 1) return 1 / den;
  if (type == 2) return x / den;
  if (type == 3) return (x * x) / den;
  return std::fabs(re) / den;
}

}  // namespace wax::cardinal

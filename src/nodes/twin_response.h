// What the twin filter does to each frequency, worked out from its settings: the picture's side of the node (port of webtracker
// src/audio/nodes/twinresponse.js). Only the two filters and the way they are joined are modelled; drive, tilt, feedback and the
// modulation are not, so the picture is the filter at rest (cutoffs as set, lock and detune applied).
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <string>
#include <vector>

namespace wax::twin {

constexpr double kFLow = 20, kFHigh = 20000, kPi = 3.14159265358979323846;

// The node's settings by name; the picture reads them through this.
using Get = std::function<double(const char*)>;

inline double param(const Get& get, const char* base, int n) {
  const std::string name = std::string(base) + std::to_string(n);
  return get(name.c_str());
}

// The cutoffs the panel shows: filter 1 as set, filter 2 as set or (locked) at the ratio above filter 1, both with the detune.
inline void cutoffs(const Get& get, double& f1, double& f2) {
  const double detune = std::pow(2.0, get("detune") / 1200);
  f1 = std::max(get("cutoff1"), 5.0);
  f2 = std::lround(get("lock")) == 1 ? f1 * get("ratio") * detune : std::max(get("cutoff2"), 5.0) * detune;
}

// One filter's complex response at f, for filter n (1 or 2) with cutoff fc.
inline std::complex<double> filterResponse(double f, const Get& get, int n, double fc, double sampleRate = 48000) {
  const double res = std::min(std::max(param(get, "res", n), 0.0), 1.0), k = 2 - 2.04 * res, kb = std::max(k, 0.02);
  const double w = std::tan(kPi * std::min(f, sampleRate * 0.499) / sampleRate) / std::tan(kPi * std::min(std::max(fc, 5.0), sampleRate * 0.45) / sampleRate);
  const std::complex<double> d(1 - w * w, k * w), lp = 1.0 / d, hp = std::complex<double>(-w * w, 0) / d, bp = std::complex<double>(0, kb * w) / d;
  return lp * param(get, "low", n) + bp * param(get, "band", n) + hp * param(get, "high", n);
}

inline double toDb(double gain) { return 20 * std::log10(std::max(gain, 1e-6)); }

inline double filterDb(double f, const Get& get, int n, double sampleRate = 48000) {
  double f1, f2; cutoffs(get, f1, f2);
  return toDb(std::abs(filterResponse(f, get, n, n == 1 ? f1 : f2, sampleRate)));
}

// The pair: filter 2 hears a mix of filter 1's output and the input (the more parallel, the more input).
inline double responseDb(double f, const Get& get, double sampleRate = 48000) {
  double f1, f2; cutoffs(get, f1, f2);
  const double blend = std::min(std::max(get("blend"), 0.0), 1.0);
  const std::complex<double> h1 = filterResponse(f, get, 1, f1, sampleRate), h2 = filterResponse(f, get, 2, f2, sampleRate);
  const std::complex<double> into2 = 1.0 + (h1 - 1.0) * (1 - blend);
  return toDb(std::abs((h1 * blend + h2 * into2) / (1 + blend)));
}

struct Overtone { int n; double f; };
inline std::vector<Overtone> overtones(double f, int most = 10) {
  std::vector<Overtone> out;
  for (int n = 2; n <= most && n * f < kFHigh; n++) out.push_back({n, n * f});
  return out;
}

// Overtones of the two filters that land on the same frequency (within about a fifth of a semitone): a is the multiple of filter 1, b of filter 2.
struct Shared { double f; int a, b; };
inline std::vector<Shared> sharedOvertones(double f1, double f2, int most = 10) {
  std::vector<Shared> out;
  for (int a = 1; a <= most; a++) for (int b = 1; b <= most; b++) {
    if (a == 1 && b == 1) continue;
    const double x = a * f1, y = b * f2;
    if (x < kFHigh && std::fabs(std::log2(x / y)) < 0.017) out.push_back({(x + y) / 2, a, b});
  }
  return out;
}

}  // namespace wax::twin

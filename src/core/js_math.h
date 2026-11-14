// The few places where JS arithmetic and C++ arithmetic differ, so a translated line means the same thing.
#pragma once

#include <cmath>
#include <initializer_list>

#include "v8-ieee754/ieee754.h"

namespace wax {

// Math.round: halves go up (2.5 -> 3, -2.5 -> -2). std::round sends halves away from zero.
inline double jsRound(double x) { return std::floor(x + 0.5); }

inline int jsRoundInt(double x) { return static_cast<int>(jsRound(x)); }

// Math.hypot for two numbers, worked out the way the JS engine does (scaled by the larger, summed with compensation),
// so a decision that hangs on its last digit falls the same way.
inline double jsHypot(double a, double b) {
  a = std::abs(a);
  b = std::abs(b);
  const double largest = a > b ? a : b;
  if (largest == 0) return 0;
  double sum = 0, compensation = 0;
  for (const double value : {a, b}) {
    const double n = value / largest;
    const double summand = n * n - compensation;
    const double preliminary = sum + summand;
    compensation = (preliminary - sum) - summand;
    sum = preliminary;
  }
  return std::sqrt(sum) * largest;
}

// Math.sin and friends as the web app's engine computes them (fdlibm, vendored in third_party/v8-ieee754). The system
// maths library differs from it in the last digit now and then, and a node that decides on a value (mosh, the spectral
// ones) can fall the other way, so every node goes through these rather than std::sin and the rest.
namespace js {
inline double sin(double x) { return v8math::sin(x); }
inline double cos(double x) { return v8math::cos(x); }
inline double tan(double x) { return v8math::tan(x); }
inline double exp(double x) { return v8math::exp(x); }
inline double log(double x) { return v8math::log(x); }
inline double log10(double x) { return v8math::log10(x); }
inline double log2(double x) { return v8math::log2(x); }
inline double pow(double a, double b) { return v8math::pow(a, b); }
inline double atan2(double a, double b) { return v8math::atan2(a, b); }
inline double atan(double x) { return v8math::atan(x); }
inline double tanh(double x) { return v8math::tanh(x); }
inline double sinh(double x) { return v8math::sinh(x); }
inline double cosh(double x) { return v8math::cosh(x); }
inline double asin(double x) { return v8math::asin(x); }
inline double acos(double x) { return v8math::acos(x); }
inline double expm1(double x) { return v8math::expm1(x); }
inline double log1p(double x) { return v8math::log1p(x); }
inline double cbrt(double x) { return v8math::cbrt(x); }
}  // namespace js

inline double clamp(double x, double low, double high) { return x < low ? low : x > high ? high : x; }

}  // namespace wax

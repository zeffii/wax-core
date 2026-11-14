// The few places where JS arithmetic and C++ arithmetic differ, so a translated line means the same thing.
#pragma once

#include <cmath>
#include <initializer_list>

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

inline double clamp(double x, double low, double high) { return x < low ? low : x > high ? high : x; }

}  // namespace wax

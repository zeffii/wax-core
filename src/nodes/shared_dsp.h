// Small pieces of DSP that several nodes use, each written once in the JS too.
#pragma once

#include <cmath>

namespace wax {

inline constexpr double kPi = 3.141592653589793;
inline constexpr double kTwoPi = 2 * kPi;
inline constexpr double kSqrt2 = 1.4142135623730951;
inline constexpr double kLn2 = 0.6931471805599453;

// Rounds the corner of a saw or square wave's edge, so it aliases less. t: the phase (0..1), dt: the phase step.
inline double polyBlep(double t, double dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1; }
  if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
  return 0;
}

// A smooth tanh made of a ratio of polynomials: flat beyond 3.
inline double softClip(double x) { return x > 3 ? 1 : x < -3 ? -1 : x * (27 + x * x) / (27 + 9 * x * x); }

// JS's % on numbers: the sign follows the left side.
inline double jsMod(double a, double b) { return std::fmod(a, b); }

// Math.sign
inline double sign(double x) { return x > 0 ? 1 : x < 0 ? -1 : x; }

}  // namespace wax

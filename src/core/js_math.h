// The few places where JS arithmetic and C++ arithmetic differ, so a translated line means the same thing.
#pragma once

#include <cmath>

namespace wax {

// Math.round: halves go up (2.5 -> 3, -2.5 -> -2). std::round sends halves away from zero.
inline double jsRound(double x) { return std::floor(x + 0.5); }

inline int jsRoundInt(double x) { return static_cast<int>(jsRound(x)); }

inline double clamp(double x, double low, double high) { return x < low ? low : x > high ? high : x; }

}  // namespace wax

#include "engine/limiter.h"

#include <algorithm>
#include <cmath>

#include "core/js_math.h"

namespace wax {

constexpr double kLookMs = 1.5;

Limiter::Limiter(double sampleRate)
    : sampleRate_(sampleRate),
      look_(std::max(8, jsRoundInt(sampleRate * kLookMs / 1000))),
      delayedLeft_(static_cast<std::size_t>(look_), 0.0f),
      delayedRight_(static_cast<std::size_t>(look_), 0.0f),
      need_(static_cast<std::size_t>(look_), 1.0f),
      held_(static_cast<std::size_t>(look_), 1.0f),
      sum_(look_) {}

Limiter::Result Limiter::process(float* left, float* right, int n, double ceilingDb, double boostDb, double releaseMs) {
  const int L = look_;
  const double c = std::pow(10.0, std::min(0.0, ceilingDb) / 20), boost = std::pow(10.0, boostDb / 20);
  const double relA = 1 - std::exp(-1 / (sampleRate_ * std::max(1.0, releaseMs) / 1000));
  int pos = pos_;
  double sum = sum_, rel = release_, low = 1, top = 0;
  for (int i = 0; i < n; i++) {
    const double xl = left[i] * boost, xr = right[i] * boost, a = std::max(std::abs(xl), std::abs(xr));
    if (a > top) top = a;
    need_[static_cast<std::size_t>(pos)] = static_cast<float>(a > c ? c / a : 1);
    double m = 1;
    for (int k = 0; k < L; k++) if (need_[static_cast<std::size_t>(k)] < m) m = need_[static_cast<std::size_t>(k)];
    rel = std::min(m, rel + (1 - rel) * relA);
    sum += rel - held_[static_cast<std::size_t>(pos)];
    held_[static_cast<std::size_t>(pos)] = static_cast<float>(rel);
    delayedLeft_[static_cast<std::size_t>(pos)] = static_cast<float>(xl);
    delayedRight_[static_cast<std::size_t>(pos)] = static_cast<float>(xr);
    pos = pos + 1 == L ? 0 : pos + 1;   // the oldest sample is now where the next one goes
    const double g = sum / L;
    if (g < low) low = g;
    left[i] = static_cast<float>(std::max(-c, std::min(c, delayedLeft_[static_cast<std::size_t>(pos)] * g)));
    right[i] = static_cast<float>(std::max(-c, std::min(c, delayedRight_[static_cast<std::size_t>(pos)] * g)));
  }
  pos_ = pos;
  sum_ = sum;
  release_ = rel;
  if (sum_ < 0) sum_ = L;   // floating point drift can never leave it negative
  return {low < 1 ? -20 * std::log10(low) : 0, 20 * std::log10(std::max(1e-10, top))};
}

}  // namespace wax

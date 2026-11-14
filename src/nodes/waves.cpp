#include "nodes/waves.h"

#include <cmath>
#include <vector>

#include "core/js_math.h"

namespace wax {
namespace {

constexpr double kTau = 2 * 3.141592653589793;

// The stored sine preset: rounded to four decimals, as the page stores what is drawn.
WaveTable sine() {
  WaveTable table{};
  for (int i = 0; i < kWavePoints; i++) {
    table[static_cast<std::size_t>(i)] = static_cast<float>(jsRound(js::sin(kTau * (static_cast<double>(i) / kWavePoints)) * 10000) / 10000);
  }
  return table;
}

}  // namespace

WaveTable normalizeWave(const Json* points) {
  if (!points || !points->is_array() || points->size() < 2) return sine();
  std::vector<double> a;
  for (const Json& p : *points) {
    if (!p.is_number() || !std::isfinite(p.get<double>())) return sine();
    a.push_back(p.get<double>());
  }
  WaveTable table{};
  for (int i = 0; i < kWavePoints; i++) {
    const double x = (static_cast<double>(i) / kWavePoints) * static_cast<double>(a.size());
    const std::size_t k = static_cast<std::size_t>(std::floor(x));
    const double f = x - static_cast<double>(k);
    const double v = a[k % a.size()] * (1 - f) + a[(k + 1) % a.size()] * f;
    table[static_cast<std::size_t>(i)] = static_cast<float>(clamp(v, -1, 1));
  }
  return table;
}

}  // namespace wax

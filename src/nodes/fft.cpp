#include "nodes/fft.h"

#include <cmath>
#include <map>
#include <memory>
#include <mutex>

#include "core/js_math.h"

namespace wax {

FftPlan::FftPlan(int n) : n_(n), reversed_(static_cast<std::size_t>(n)), cos_(static_cast<std::size_t>(n / 2)), sin_(static_cast<std::size_t>(n / 2)) {
  constexpr double pi = 3.141592653589793;
  const int bits = jsRoundInt(js::log2(n));
  for (int i = 0; i < n; i++) {
    std::uint32_t r = 0;
    for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1u << (bits - 1 - b);
    reversed_[static_cast<std::size_t>(i)] = r;
  }
  for (int i = 0; i < n / 2; i++) {
    cos_[static_cast<std::size_t>(i)] = js::cos(2 * pi * i / n);
    sin_[static_cast<std::size_t>(i)] = -js::sin(2 * pi * i / n);
  }
}

void FftPlan::run(double* re, double* im, bool inverse) const {
  const int n = n_;
  for (int i = 0; i < n; i++) {
    const int j = static_cast<int>(reversed_[static_cast<std::size_t>(i)]);
    if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
  }
  for (int size = 2; size <= n; size <<= 1) {
    const int half = size >> 1, step = n / size;
    for (int i = 0; i < n; i += size) {
      for (int j = 0, k = 0; j < half; j++, k += step) {
        const double wr = cos_[static_cast<std::size_t>(k)], wi = inverse ? -sin_[static_cast<std::size_t>(k)] : sin_[static_cast<std::size_t>(k)];
        const int a = i + j, b = a + half;
        const double xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
      }
    }
  }
  if (inverse) {
    const double s = 1.0 / n;
    for (int i = 0; i < n; i++) { re[i] *= s; im[i] *= s; }
  }
}

const FftPlan& fftPlan(int n) {
  static std::mutex guard;   // (instruments will be rendered on several threads)
  static std::map<int, std::unique_ptr<FftPlan>> plans;
  const std::lock_guard<std::mutex> lock(guard);
  std::unique_ptr<FftPlan>& plan = plans[n];
  if (!plan) plan = std::make_unique<FftPlan>(n);
  return *plan;
}

}  // namespace wax

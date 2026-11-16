#include "project/spectrogram.h"

#include <algorithm>
#include <cmath>

namespace wax {

namespace {

constexpr int kSize = 1024;   // FFT length in frames
constexpr double kPi = 3.14159265358979323846;

}  // namespace

// In-place radix-2 FFT of re/im (length a power of two).
void fft(std::vector<double>& re, std::vector<double>& im) {
  const int n = static_cast<int>(re.size());
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { std::swap(re[static_cast<std::size_t>(i)], re[static_cast<std::size_t>(j)]); std::swap(im[static_cast<std::size_t>(i)], im[static_cast<std::size_t>(j)]); }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double ang = -2 * kPi / len, wr = std::cos(ang), wi = std::sin(ang);
    for (int i = 0; i < n; i += len) {
      double cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) {
        const std::size_t a = static_cast<std::size_t>(i + k), b = static_cast<std::size_t>(i + k + len / 2);
        const double tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr; im[a] += ti;
        const double nr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr; cr = nr;
      }
    }
  }
}

std::vector<std::uint8_t> computeSpectrogram(const std::vector<float>& left, const std::vector<float>& right, int from, int to, int cols, int rows) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(std::max(0, cols)) * static_cast<std::size_t>(std::max(0, rows)), 0);
  from = std::max(0, from);
  to = std::min(static_cast<int>(left.size()), to);
  const int length = to - from;   // the view is cut out of the sample first: its edges are silence to the windows
  if (length <= 0 || cols <= 0 || rows <= 0) return out;
  const bool stereo = !right.empty();
  const int half = kSize / 2;
  std::vector<double> window(kSize), re(kSize), im(kSize);
  for (int i = 0; i < kSize; i++) window[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2 * kPi * i / kSize);
  std::vector<float> db(out.size());
  const auto binOf = [&](int y) { return std::min(half - 1, static_cast<int>(std::floor(std::pow(static_cast<double>(rows - 1 - y) / rows, 2) * half))); };
  double top = -1e300;
  for (int x = 0; x < cols; x++) {
    const int centre = static_cast<int>(std::floor((x + 0.5) * length / cols));
    for (int i = 0; i < kSize; i++) {
      const int k = centre - half + i;
      double v = 0;
      if (k >= 0 && k < length) v = stereo ? (static_cast<double>(left[static_cast<std::size_t>(from + k)]) + right[static_cast<std::size_t>(from + k)]) * 0.5 : left[static_cast<std::size_t>(from + k)];
      re[static_cast<std::size_t>(i)] = v * window[static_cast<std::size_t>(i)];
      im[static_cast<std::size_t>(i)] = 0;
    }
    fft(re, im);
    for (int y = 0; y < rows; y++) {
      const std::size_t b = static_cast<std::size_t>(binOf(y));
      const double level = 20 * std::log10(std::hypot(re[b], im[b]) / half + 1e-9);
      db[static_cast<std::size_t>(x) * rows + static_cast<std::size_t>(y)] = static_cast<float>(level);
      if (static_cast<float>(level) > top) top = static_cast<float>(level);
    }
  }
  for (int x = 0; x < cols; x++)
    for (int y = 0; y < rows; y++)
      out[static_cast<std::size_t>(y) * cols + static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(std::lround(255 * std::min(1.0, std::max(0.0, (db[static_cast<std::size_t>(x) * rows + static_cast<std::size_t>(y)] - (top - 90)) / 90))));
  return out;
}

std::array<std::uint8_t, 4> spectrogramHeat(int v) {
  const double t = v / 255.0;
  const auto byte = [](double x) { return static_cast<std::uint8_t>(std::lround(std::min(255.0, std::max(0.0, x)))); };
  return {byte(255 * std::pow(std::min(1.0, t * 1.6), 1.5)), byte(255 * std::pow(std::max(0.0, t * 1.4 - 0.4), 1.2)), byte(255 * std::min(1.0, t * 2.2 + 0.1) * (t > 0.01 ? 1 : 0)), 255};
}

}  // namespace wax

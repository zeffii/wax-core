#include "project/onsets.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "project/spectrogram.h"

namespace wax {

namespace {
constexpr int kSize = 1024, kHop = 256;
constexpr double kPi = 3.14159265358979323846;
}  // namespace

std::vector<int> detectOnsets(const std::vector<float>& data, const std::vector<float>& right, double rate, const OnsetSettings& settings) {
  const int total = static_cast<int>(data.size());
  const int from = std::max(0, settings.from), to = std::min(total, settings.to < 0 ? total : settings.to);
  const int len = to - from;
  if (len < kSize) return {};
  std::vector<float> mono(static_cast<std::size_t>(len));
  for (int i = 0; i < len; i++) {
    const std::size_t at = static_cast<std::size_t>(from + i);
    mono[static_cast<std::size_t>(i)] = right.empty() ? data[at] : static_cast<float>((static_cast<double>(data[at]) + right[at]) / 2);
  }
  const int frames = (len - kSize) / kHop + 1;
  std::vector<float> window(kSize);
  for (int i = 0; i < kSize; i++) window[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * i / kSize));
  std::vector<double> re(kSize), im(kSize), prev(kSize / 2, 0.0), flux(static_cast<std::size_t>(frames), 0.0);
  for (int f = 0; f < frames; f++) {
    for (int i = 0; i < kSize; i++) { re[static_cast<std::size_t>(i)] = static_cast<double>(mono[static_cast<std::size_t>(f * kHop + i)]) * window[static_cast<std::size_t>(i)]; im[static_cast<std::size_t>(i)] = 0; }
    fft(re, im);
    double sum = 0;
    for (int k = 1; k < kSize / 2; k++) {
      const double m = std::log1p(50 * std::hypot(re[static_cast<std::size_t>(k)], im[static_cast<std::size_t>(k)]) / kSize * 4);
      if (f > 0 && m > prev[static_cast<std::size_t>(k)]) sum += m - prev[static_cast<std::size_t>(k)];
      prev[static_cast<std::size_t>(k)] = m;
    }
    flux[static_cast<std::size_t>(f)] = sum;
  }
  // a typical loud value to scale the threshold by (the 95th percentile, so one huge hit does not hide the rest)
  std::vector<double> sorted = flux;
  std::sort(sorted.begin(), sorted.end());
  double loud = sorted[static_cast<std::size_t>(std::floor(frames * 0.95))] ;
  if (!(loud != 0)) loud = sorted[static_cast<std::size_t>(frames - 1)];
  if (!(loud != 0)) loud = 1;
  if (loud < 1) return {};   // nothing but a steady tone or silence: flux this small is rounding noise
  const int half = std::max(2, static_cast<int>(std::lround(0.1 * rate / kHop)));   // the average is taken over about 100 ms either side
  const double delta = (1.05 - std::min(1.0, std::max(0.0, settings.sensitivity))) * 0.4 * loud;
  const int gap = std::max(1, static_cast<int>(std::lround(settings.minGapMs / 1000 * rate / kHop)));
  struct Peak { int f; double v; };
  std::vector<Peak> found;
  for (int f = 1; f < frames; f++) {
    double mean = 0;
    int n = 0;
    bool top = true;
    for (int k = std::max(0, f - half); k <= std::min(frames - 1, f + half); k++) { mean += flux[static_cast<std::size_t>(k)]; n++; }
    mean /= n;
    if (flux[static_cast<std::size_t>(f)] <= mean + delta) continue;
    for (int k = std::max(0, f - 3); k <= std::min(frames - 1, f + 3); k++) if (flux[static_cast<std::size_t>(k)] > flux[static_cast<std::size_t>(f)]) { top = false; break; }
    if (!top) continue;
    if (!found.empty() && f - found.back().f < gap) { if (flux[static_cast<std::size_t>(f)] > found.back().v) found.back() = {f, flux[static_cast<std::size_t>(f)]}; continue; }
    found.push_back({f, flux[static_cast<std::size_t>(f)]});
  }
  // The peak frame covers a window that the hit begins somewhere inside: from the first sample there that reaches a fifth of the loudest one
  // nearby, less a millisecond for the attack.
  const int back = static_cast<int>(std::lround(rate * 0.001));
  std::set<int> starts;
  for (const Peak& peak : found) {
    const int a = std::max(0, peak.f * kHop - kHop), b = std::min(len, peak.f * kHop + kSize + kHop);
    float maxLevel = 0;
    for (int i = a; i < b; i++) maxLevel = std::max(maxLevel, std::abs(mono[static_cast<std::size_t>(i)]));
    int at = a;
    for (int i = a; i < b; i++) if (std::abs(mono[static_cast<std::size_t>(i)]) >= maxLevel * 0.2) { at = i; break; }
    starts.insert(std::max(0, at - back));
  }
  std::vector<int> out;
  for (const int i : starts) out.push_back(from + i);
  return out;
}

}  // namespace wax

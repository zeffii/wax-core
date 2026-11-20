#include "nodes/padcore.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <list>
#include <mutex>
#include <tuple>

#include "core/js_math.h"
#include "nodes/fft.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

constexpr int kTableSizes[3] = {32768, 8192, 65536};   // quality 0 standard, 1 eco, 2 high
constexpr int kBandMax = 10;
constexpr double kRatioTop = 1.4143;   // the highest note of a band is this far above the pitch the table was made at
constexpr double kTableRms = 0.5;
constexpr std::uint32_t kSeeds[2] = {0x2545f491u, 0x9e3779b9u};   // left, right: the random phases
constexpr double kDbPerOctave = 0.015051499783199059;   // log10(2) / 20
constexpr std::size_t kCacheMax = 24;

std::uint32_t nextRandom(std::uint32_t r) { return r * 1664525u + 1013904223u; }   // (wraps at 32 bits, as the JS's imul and >>> 0)

// One channel's table.
std::vector<float> buildTable(double sampleRate, int size, int band, const SpreadSettings& p, int channel) {
  const int half = size / 2;
  const double pitch = spreadBandPitch(band), nyquist = sampleRate * 0.45;
  const double stretch = p.stretch * p.stretch * 0.004;
  std::vector<double> mag(static_cast<std::size_t>(half), 0.0);
  const double sqrtHalfPi = std::sqrt(kPi / 2);
  for (int h = 1; h <= p.harmonics; h++) {
    const double freq = pitch * h * std::sqrt(1 + stretch * h * h);
    if (freq * kRatioTop >= nyquist) break;
    const double level = js::pow(h, p.tilt * kDbPerOctave) * (h % 2 ? std::min(1.0, 1 - p.evenodd) : std::min(1.0, 1 + p.evenodd)) / h;
    const double bandwidth = (js::pow(2, p.bandwidth / 1200) - 1) * pitch * js::pow(h, p.spread);   // Hz
    const double width = std::max(bandwidth * size / (2 * sampleRate), 1.0);   // bins
    const double centre = freq * size / sampleRate;
    const double amp = level / std::sqrt(width * sqrtHalfPi);
    const int lo = static_cast<int>(std::max(1.0, std::floor(centre - 4 * width))), hi = static_cast<int>(std::min(static_cast<double>(half - 1), std::ceil(centre + 4 * width)));
    for (int k = lo; k <= hi; k++) { const double x = (k - centre) / width; mag[static_cast<std::size_t>(k)] += amp * js::exp(-x * x); }
  }
  std::vector<double> re(static_cast<std::size_t>(size), 0.0), im(static_cast<std::size_t>(size), 0.0);
  std::uint32_t rng = kSeeds[channel];
  for (int k = 1; k < half; k++) {
    rng = nextRandom(rng);
    const double phase = (rng / 4294967296.0) * 2 * kPi;
    const std::size_t at = static_cast<std::size_t>(k), mirror = static_cast<std::size_t>(size - k);
    re[at] = mag[at] * js::cos(phase); im[at] = mag[at] * js::sin(phase);
    re[mirror] = re[at]; im[mirror] = -im[at];
  }
  fftPlan(size).run(re.data(), im.data(), true);
  double sum = 0;
  for (int i = 0; i < size; i++) sum += re[static_cast<std::size_t>(i)] * re[static_cast<std::size_t>(i)];
  const double scale = sum > 1e-30 ? kTableRms / std::sqrt(sum / size) : 0;
  std::vector<float> table(static_cast<std::size_t>(size));
  for (int i = 0; i < size; i++) table[static_cast<std::size_t>(i)] = static_cast<float>(re[static_cast<std::size_t>(i)] * scale);
  return table;
}

struct CacheEntry {
  std::tuple<double, int, int, double, double, double, double, double, int> key;
  std::shared_ptr<const SpreadTables> tables;
};

std::mutex gMutex;
std::list<CacheEntry> gCache;   // most recently used first

}  // namespace

int spreadBandOf(double note) { return static_cast<int>(std::min(static_cast<double>(kBandMax), std::max(0.0, std::floor(note / 12)))); }
double spreadBandPitch(int band) { return 440 * js::pow(2, (band * 12 + 6 - 69) / 12.0); }

std::shared_ptr<const SpreadTables> spreadTables(double sampleRate, int quality, int band, const SpreadSettings& settings) {
  const int size = kTableSizes[std::min(2, std::max(0, quality))];
  const auto q = [](double x) { return jsRound(x * 1000) / 1000; };
  const SpreadSettings p{q(settings.bandwidth), q(settings.spread), q(settings.tilt), q(settings.evenodd), q(settings.stretch), settings.harmonics};
  const auto key = std::make_tuple(sampleRate, size, band, p.bandwidth, p.spread, p.tilt, p.evenodd, p.stretch, p.harmonics);
  std::lock_guard<std::mutex> lock(gMutex);   // (building under the lock keeps two voices from building the same table twice)
  for (auto it = gCache.begin(); it != gCache.end(); ++it) {
    if (it->key == key) { gCache.splice(gCache.begin(), gCache, it); return it->tables; }
  }
  auto made = std::make_shared<SpreadTables>();
  made->size = size;
  made->left = buildTable(sampleRate, size, band, p, 0);
  made->right = buildTable(sampleRate, size, band, p, 1);
  gCache.push_front({key, made});
  if (gCache.size() > kCacheMax) gCache.pop_back();
  return made;
}

void SpreadPlayer::restart() {
  rng_ = nextRandom(rng_); posL_ = rng_ / 4294967296.0;
  rng_ = nextRandom(rng_); posR_ = rng_ / 4294967296.0;
  rng_ = nextRandom(rng_); lfoL_ = (rng_ / 4294967296.0) * 2 * kPi;
  rng_ = nextRandom(rng_); lfoR_ = (rng_ / 4294967296.0) * 2 * kPi;
  fresh_ = true;
}

void SpreadPlayer::render(const SpreadTables& tables, int n, double speed, double motion, double width, float* outL, float* outR) {
  const int size = tables.size, mask = size - 1;
  const float* tl = tables.left.data();
  const float* tr = tables.right.data();
  if (fresh_) { posL_ *= size; posR_ *= size; fresh_ = false; }
  const double cents = motion * 12;
  const double stepL = speed * js::pow(2, (cents * js::sin(lfoL_)) / 1200), stepR = speed * js::pow(2, (cents * js::sin(lfoR_)) / 1200);
  lfoL_ += (2 * kPi * 0.11 * n) / sampleRate_; lfoR_ += (2 * kPi * 0.17 * n) / sampleRate_;
  if (lfoL_ > 2 * kPi) lfoL_ -= 2 * kPi;
  if (lfoR_ > 2 * kPi) lfoR_ -= 2 * kPi;
  const double mix = (1 - width) * kSqrt2 * 0.5;
  double pl = posL_, pr = posR_;
  for (int i = 0; i < n; i++) {
    const double fl0 = std::floor(pl), fr0 = std::floor(pr);
    const int il = static_cast<int>(fl0), ir = static_cast<int>(fr0);
    const double fl = pl - fl0, fr = pr - fr0;
    const double l0 = tl[(il - 1) & mask], l1 = tl[il & mask], l2 = tl[(il + 1) & mask], l3 = tl[(il + 2) & mask];
    const double r0 = tr[(ir - 1) & mask], r1 = tr[ir & mask], r2 = tr[(ir + 1) & mask], r3 = tr[(ir + 2) & mask];
    const double l = ((((3 * (l1 - l2) - l0 + l3) * 0.5) * fl + (2 * l2 + l0 - (5 * l1 + l3) * 0.5)) * fl + (l2 - l0) * 0.5) * fl + l1;
    const double r = ((((3 * (r1 - r2) - r0 + r3) * 0.5) * fr + (2 * r2 + r0 - (5 * r1 + r3) * 0.5)) * fr + (r2 - r0) * 0.5) * fr + r1;
    const double m = mix * (l + r);
    outL[i] = static_cast<float>(m + width * l); outR[i] = static_cast<float>(m + width * r);
    pl += stepL; if (pl >= size) pl -= size;
    pr += stepR; if (pr >= size) pr -= size;
  }
  posL_ = pl; posR_ = pr;
}

}  // namespace wax

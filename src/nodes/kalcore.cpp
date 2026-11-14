#include "nodes/kalcore.h"

#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/shared_dsp.h"

namespace wax::kal {
namespace {

// The multiple of the frequency that copy k of the overtone mode lands on: k = 0 is the source (x1), k = 1 is x2, ...
// and k = -1 is x1/2, ... `stretch` (0 to 1) moves the multiples off the whole numbers.
double overtoneRatio(int k, double stretch) {
  const double n = std::abs(k) + 1, b = stretch * stretch * 0.02, r = n * std::sqrt(1 + b * n * n);
  return k < 0 ? 1 / r : r;
}

// How loud copy k of the overtone mode is, 1 for the source.
double overtoneGain(int k, double decay) { return js::pow(std::abs(k) + 1, -2 * decay); }

// A windowed (periodic Hann) sinusoid's spectrum near its frequency: the weight at distance d bins from it, times n.
double lobe(double d) {
  const double x = std::fabs(d);
  if (x < 1e-6) return 0.5;
  const double den = 1 - x * x;
  if (std::fabs(den) < 1e-6) return 0.25;
  return js::sin(kPi * x) / (2 * kPi * x * den);
}

struct Spot { double out, gain; };

// Where a tone at octave position `oSrc` of the band lands in tile `k`, and how loud it is there (the fade at the seams, 0 to 1).
Spot tileOf(double oSrc, int k, const Band& band, const Settings& s) {
  if (s.mode == 2) {   // overtones: the tone's own frequency times a multiple; a short fade at the band's ends
    const double inBand = oSrc - band.b0, fade = s.edge / 12;
    double gain = overtoneGain(k, s.decay);
    if (fade > 0) {
      const double t = std::min(1.0, std::min(inBand, band.width - inBand) / fade);
      gain *= t * t * (3 - 2 * t);
    }
    return {oSrc + js::log2(overtoneRatio(k, s.stretch)), gain};
  }
  const double b0 = band.b0, width = band.width, start = b0 + s.rotate * width + k * width;
  const bool flipped = s.mode == 0 && (((k % 2) + 2) % 2) == 1;
  const double inBand = oSrc - b0;
  const double out = flipped ? start + width - inBand : start + inBand;
  // a fade where the ends do not match: at the band's both ends in repeat mode, and at the outer end of the last copy in mirror mode
  double gain = 1;
  const double fade = s.edge / 12;
  if (fade > 0) {
    double near = INFINITY;
    if (s.mode == 1) near = std::min(inBand, width - inBand);
    else if (s.copies > 0 && std::abs(k) == s.copies) {
      const double toHighEnd = width - inBand, toLowEnd = inBand;
      near = ((k > 0) != flipped) ? toHighEnd : toLowEnd;
    }
    const double t = std::min(1.0, near / fade);
    gain = t * t * (3 - 2 * t);
  }
  return {out, gain};
}

}  // namespace

Band bandOf(double low, double high) {
  const double lo = std::max(kLowMin, std::min(low, high / 1.26));
  const double hi = std::min(kHighMax, std::max(high, lo * 1.26));
  return {lo, hi, js::log2(lo), js::log2(hi / lo)};
}

Kaleidoscoper::Kaleidoscoper(int n, double sampleRate)
    : n_(n), sr_(sampleRate), hop_(n / 4), half_(n >> 1),
      prev_(static_cast<std::size_t>(half_ + 1), 0.0),
      acc_(static_cast<std::size_t>(kTiles * 2 + 1), std::vector<double>(static_cast<std::size_t>(half_ + 1), 0.0)),
      seen_(static_cast<std::size_t>(kTiles * 2 + 1), std::vector<int>(static_cast<std::size_t>(half_ + 1), -5)),
      mag_(static_cast<std::size_t>(half_ + 2), 0.0), ph_(static_cast<std::size_t>(half_ + 1), 0.0) {}

void Kaleidoscoper::run(const double* re, const double* im, double* reW, double* imW, const Settings& s) {
  const Band band = bandOf(s.low, s.high);
  const double perBin = sr_ / n_;
  const double step = kTwoPi * hop_ / n_;
  std::fill(reW, reW + half_ + 1, 0.0);
  std::fill(imW, imW + half_ + 1, 0.0);
  double top = 0;
  for (int k = 0; k <= half_; k++) {
    const std::size_t i = static_cast<std::size_t>(k);
    mag_[i] = jsHypot(re[k], im[k]);
    ph_[i] = js::atan2(im[k], re[k]);
    if (mag_[i] > top) top = mag_[i];
  }
  mag_[static_cast<std::size_t>(half_) + 1] = 0;
  const double floorMag = std::max(1e-7 * n_, top * 1e-4);
  frame_++;
  // only the tiles that can land between 20 Hz and the top of the range are looked at
  const double oLow = js::log2(20), oHigh = js::log2(sr_ * 0.48);
  const int kLow = static_cast<int>(std::max(static_cast<double>(-kTiles), std::floor((oLow - band.b0 - s.rotate * band.width) / band.width)));
  const int kHigh = static_cast<int>(std::min(static_cast<double>(kTiles), std::floor((oHigh - band.b0 - s.rotate * band.width) / band.width) + 1));
  for (int p = 1; p < half_; p++) {
    const std::size_t pi = static_cast<std::size_t>(p);
    if (mag_[pi] < floorMag || mag_[pi] <= mag_[pi - 1] || mag_[pi] < mag_[pi + 1]) continue;
    // the tone's true frequency, in bins, from the phase advance (the part beyond what a tone at the bin's own frequency would do)
    double dev = ph_[pi] - prev_[pi] - step * p;
    dev -= kTwoPi * jsRound(dev / kTwoPi);
    const double q = p + std::max(-1.0, std::min(1.0, dev / step)), d = p - q;
    const double f = q * perBin;
    if (f < band.lo || f > band.hi) continue;
    const double oSrc = js::log2(f), halfA = mag_[pi] / (n_ * lobe(d)), phi = ph_[pi] + kPi * d;   // half the amplitude, and the phase at the start of the frame
    int from = std::max(kLow, s.copies > 0 ? -s.copies : -kTiles), to = std::min(kHigh, s.copies > 0 ? s.copies : kTiles);
    if (s.mode == 2) {
      from = -static_cast<int>(std::min({static_cast<double>(s.under), std::floor(f / 20), static_cast<double>(kTiles)}));
      to = static_cast<int>(std::min({static_cast<double>(s.overtones), static_cast<double>(kTiles), std::floor(sr_ * 0.48 / f)}));
    }
    for (int k = from; k <= to; k++) {
      const Spot t = tileOf(oSrc, k, band, s);
      const double fOut = js::pow(2, t.out);
      if (fOut < 20 || fOut > sr_ * 0.48 || t.gain <= 0) continue;
      const double qOut = fOut / perBin;
      const std::size_t i = static_cast<std::size_t>(k + kTiles);
      // carry the phase on from the last frame if this tone was there; otherwise start it where the source is
      const double carried = seen_[i][pi] == frame_ - 1 ? std::fmod(acc_[i][pi] + step * qOut, kTwoPi) : phi;
      acc_[i][pi] = carried;
      seen_[i][pi] = frame_;
      const double amp = halfA * n_ * t.gain * s.level;
      const int bLow = std::max(1, static_cast<int>(std::floor(qOut)) - 1), bHigh = std::min(half_, static_cast<int>(std::floor(qOut)) + 2);
      for (int b = bLow; b <= bHigh; b++) {
        const double dd = b - qOut, w = amp * lobe(dd), a = carried - kPi * dd;
        reW[b] += w * js::cos(a);
        imW[b] += w * js::sin(a);
      }
    }
  }
  for (int k = 0; k <= half_; k++) prev_[static_cast<std::size_t>(k)] = ph_[static_cast<std::size_t>(k)];
}

}  // namespace wax::kal

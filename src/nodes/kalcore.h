// The kaleidoscope's geometry and frame step: a band of the spectrum is repeated up and down the range (mirrored, repeated, or
// as an overtone series). A tone moved to another place keeps its phase in step from frame to frame, so the effect works on the
// tones themselves (the peaks of each frame) and writes each one into every tile as a clean sinusoid.
// Port of the playing part of webtracker src/audio/kalcore.js (the panel's half, pictureOf, is for the window).
#pragma once

#include <cmath>
#include <algorithm>
#include <array>
#include <vector>

namespace wax::kal {

inline constexpr double kLowMin = 40, kHighMax = 16000;
inline constexpr int kTiles = 34;   // tiles either side of the source that are looked at

struct Band { double lo, hi, b0, width; };
Band bandOf(double low, double high);

// The multiple of a tone's frequency that copy k of the overtone mode lands on (k = 0: x1, k = 1: x2, k = -1: x1/2), and the copy's loudness.
double overtoneRatio(int k, double stretch);
double overtoneGain(int k, double decay);

struct Settings {
  double low, high;
  int mode;        // 0 mirror, 1 repeat, 2 overtone
  double rotate;
  int copies;
  double edge, level;
  double stretch = 0, decay = 0.5;
  int overtones = 8, under = 2;
};

class Kaleidoscoper {
public:
  Kaleidoscoper(int n, double sampleRate);

  // re, im: the frame's spectrum. Writes the arrayed sound's spectrum into reW and imW (bins 0 to n/2).
  void run(const double* re, const double* im, double* reW, double* imW, const Settings& settings);

private:
  int n_;
  double sr_;
  int hop_, half_;
  std::vector<double> prev_;                        // the phase of every bin in the last frame
  std::vector<std::vector<double>> acc_;            // per tile and source bin: the phase of the sinusoid made there
  std::vector<std::vector<int>> seen_;
  std::vector<double> mag_, ph_;
  int frame_ = 0;
};

// The spectrum of a frame as `out.size()` values from 0 to 1 along the panel's log axis (27.5 Hz to 14 kHz): the loudest bin under each
// point, in dB with a sine of amplitude 1 at the top and -80 dB at the bottom. re, im: bins 0 to n/2. Port of kalcore.js pictureOf.
inline void pictureOf(const double* re, const double* im, int n, double sampleRate, double peak, std::vector<float>& out) {
  constexpr double kFMin = 27.5, kFMax = 14080;
  const int half = n >> 1, points = static_cast<int>(out.size());
  const double perBin = sampleRate / n, lo = std::log2(kFMin), hi = std::log2(kFMax);
  const auto power = [&](int i) { return re[i] * re[i] + im[i] * im[i]; };
  for (int p = 0; p < points; p++) {
    const double a = std::pow(2, lo + (hi - lo) * p / points) / perBin, b = std::pow(2, lo + (hi - lo) * (p + 1) / points) / perBin;
    double m = 0;
    if (b - a < 1) {   // finer than a bin: blend the two bins the point sits between
      const double c = (a + b) / 2;
      const int i = std::max(1, std::min(half - 1, static_cast<int>(std::floor(c))));
      const double f = c - i, m0 = power(i), m1 = power(i + 1);
      m = m0 + (m1 - m0) * f;
    } else {
      for (int i = static_cast<int>(std::floor(a)); i <= std::min(half, static_cast<int>(std::floor(b))); i++) m = std::max(m, power(i));
    }
    out[static_cast<std::size_t>(p)] = m > 0 ? static_cast<float>(std::max(0.0, std::min(1.0, (10 * std::log10(m / (peak * peak)) + 80) / 80))) : 0.f;
  }
}

}  // namespace wax::kal

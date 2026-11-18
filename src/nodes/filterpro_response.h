// What FilterPro does to each frequency, worked out from its settings: the picture's side of the node (port of webtracker
// src/audio/nodes/filterproresponse.js). Only the filter itself is modelled: drive, bias and the cutoff input are not, so with those in
// use the picture is the filter at rest. The two-pole voicings are zero-delay state-variable filters, whose response is the analog
// 2nd-order one with the frequency axis warped by tan(pi f / sr).
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <vector>

namespace wax::filterpro {

inline constexpr double kFLow = 20, kFHigh = 20000, kPi = 3.14159265358979323846;
inline constexpr double kFormants[5][3] = {{800, 1150, 2900}, {350, 2000, 2800}, {270, 2140, 2950}, {450, 800, 2830}, {325, 700, 2700}};   // a e i o u
inline constexpr double kFormantGain[3] = {1, 0.5, 0.3};

using Get = std::function<double(const char*)>;   // the node's settings by name
using Complex = std::complex<double>;

// the formant centres in Hz for a vowel setting (0 to 4) and a cutoff (the vowels are written for 1 kHz)
inline std::vector<double> formantsAt(double vowel, double cutoff, double sampleRate = 48000) {
  const int vi = std::min(static_cast<int>(std::floor(vowel)), 3);
  const double vf = vowel - vi;
  std::vector<double> out;
  for (int q = 0; q < 3; q++) out.push_back(std::min((kFormants[vi][q] * (1 - vf) + kFormants[vi + 1][q] * vf) * (cutoff / 1000), sampleRate * 0.45));
  return out;
}

namespace detail {
inline Complex divide(Complex a, Complex b) { return std::norm(b) == 0 ? a / Complex(1e-30, 0) : a / b; }
// the warped frequency (1 at the cutoff)
inline double warp(double f, double fc, double sampleRate) { return std::tan(kPi * std::min(f, sampleRate * 0.499) / sampleRate) / std::tan(kPi * std::min(fc, sampleRate * 0.45) / sampleRate); }
}  // namespace detail

// The gain of the voicing at frequency f, as a plain number (1 is no change).
inline double responseGain(double f, const Get& get, double sampleRate = 48000) {
  using detail::divide;
  using detail::warp;
  const int mode = static_cast<int>(std::lround(get("mode")));
  const double res = std::min(std::max(get("resonance"), 0.0), 1.0), fc = std::max(get("cutoff"), 5.0);
  const double k = 2 - 2.04 * res, kb = std::max(k, 0.02);
  Complex h;
  if (mode <= 6) {
    const double w = warp(f, fc, sampleRate);
    const Complex d(1 - w * w, k * w), lp = divide(1, d), hp = divide(-w * w, d), bp = divide(Complex(0, kb * w), d);
    switch (mode) {
      case 0: h = lp; break;
      case 1: h = bp; break;
      case 2: h = hp; break;
      case 3: h = lp + hp; break;
      case 4: h = lp - hp; break;
      case 5: h = 1.0 - 2.0 * bp; break;
      default: {
        const double m = get("morph"), wl = std::max(0.0, 1 - 2 * m), wn = 1 - std::fabs(2 * m - 1), wh = std::max(0.0, 2 * m - 1);
        h = wl * lp + wn * (lp + hp) + wh * hp;
      }
    }
  } else if (mode == 7) {   // two sections, the first at a fixed damping of root two
    const double w = warp(f, fc, sampleRate);
    h = divide(1, Complex(1 - w * w, 1.4142 * w)) * divide(1, Complex(1 - w * w, k * w));
  } else if (mode == 8) {   // the ladder: four one-pole stages with the feedback of the resonance around them
    const double w = warp(f, fc, sampleRate);
    const Complex stage = divide(1, Complex(1, w)), four = stage * stage * stage * stage;
    h = divide(four, 1.0 + four * (res * 4.3)) * (1 + res * 0.7);
  } else if (mode == 9) {   // three formant bands in parallel
    const double fk = 1 / (2 + res * 18);
    h = 0;
    const std::vector<double> formants = formantsAt(get("vowel"), fc, sampleRate);
    for (std::size_t q = 0; q < 3; q++) {
      const double w = warp(f, formants[q], sampleRate);
      h += divide(Complex(0, w), Complex(1 - w * w, fk * w)) * (4 * kFormantGain[q] * fk);
    }
  } else {   // a comb: the output fed back through a delay of one period of the cutoff, with a damping filter in the loop
    const double fbk = (mode == 10 ? 1 : -1) * res * 0.97, w = 2 * kPi * f / sampleRate, T = sampleRate / std::min(std::max(fc, 20.0), 5000.0 * 4);
    const double damp = 1 - get("damp") * 0.9;
    const Complex z(std::cos(-w), std::sin(-w));
    const Complex hd = divide(damp, 1.0 + z * -(1 - damp));
    const Complex delayed(std::cos(-w * T), std::sin(-w * T));
    h = divide(1, 1.0 + hd * delayed * -fbk) * (1 - std::fabs(fbk) * 0.5);
  }
  const double mix = get("mix"), level = get("level");
  return std::abs(h * mix + (1 - mix)) * level;
}

inline double responseDb(double f, const Get& get, double sampleRate = 48000) { return 20 * std::log10(std::max(responseGain(f, get, sampleRate), 1e-5)); }

// A spectrum as `out.size()` values from 0 to 1 along a log axis from 20 Hz to 20 kHz: the loudest bin under each point, in dB with a sine of
// amplitude 1 at the top and -90 dB at the bottom. re and im are the bins 0 to n/2 of a Hann-windowed frame.
inline void spectrumPoints(const double* re, const double* im, int n, double sampleRate, std::vector<float>& out) {
  const int half = n >> 1, points = static_cast<int>(out.size());
  const double perBin = sampleRate / n, lo = std::log2(kFLow), hi = std::log2(kFHigh), peak = n / 4.0;
  for (int p = 0; p < points; p++) {
    const double a = std::pow(2.0, lo + (hi - lo) * p / points) / perBin, b = std::pow(2.0, lo + (hi - lo) * (p + 1) / points) / perBin;
    double m = 0;
    if (b - a < 1) {   // finer than a bin: blend the two bins the point sits between
      const double c = (a + b) / 2;
      const int i = std::max(1, std::min(half - 1, static_cast<int>(std::floor(c))));
      const double f = c - i, m0 = re[i] * re[i] + im[i] * im[i], m1 = re[i + 1] * re[i + 1] + im[i + 1] * im[i + 1];
      m = m0 + (m1 - m0) * f;
    } else {
      for (int i = static_cast<int>(std::floor(a)); i <= std::min(half, static_cast<int>(std::floor(b))); i++) m = std::max(m, re[i] * re[i] + im[i] * im[i]);
    }
    out[static_cast<std::size_t>(p)] = m > 0 ? static_cast<float>(std::max(0.0, std::min(1.0, (10 * std::log10(m / (peak * peak)) + 90) / 90))) : 0.0f;
  }
}

}  // namespace wax::filterpro

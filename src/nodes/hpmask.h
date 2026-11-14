// Tells the held part of a sound from its hits, bin by bin, frame by frame (used by the harmonic/percussive split and by the body
// blur). A median along time keeps the held lines of the spectrum picture, a median along frequency keeps the stripes of the hits,
// and the two decide how much of each bin is body. Port of webtracker src/audio/hpmask.js.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/js_math.h"

namespace wax {

inline constexpr int kMaskMaxFrames = 25, kMaskMaxBins = 25;

// An odd count, at least 3 and at most `max`, nearest to `v`.
inline int maskOdd(double v, int max) { return std::max(3, std::min(max, jsRoundInt(v) | 1)); }

struct MaskSettings {
  int frames = 3, bins = 3;
  double sharp = 1, lean = 1;
};

// The settings for a frame length `n`: how many frames a held tone must last, how many bins wide a hit must be.
inline MaskSettings maskSettings(double sampleRate, int n, double lengthMs, double widthHz, double sharp, double lean = 1) {
  MaskSettings s;
  s.frames = maskOdd((lengthMs * 0.001 * sampleRate) / (n / 4), kMaskMaxFrames);
  s.bins = maskOdd(widthHz / (sampleRate / n), kMaskMaxBins);
  s.sharp = sharp;
  s.lean = lean;
  return s;
}

class Masker {
public:
  explicit Masker(int n)
      : top_(n / 2 + 1), history_(static_cast<std::size_t>(kMaskMaxFrames * top_), 0.0f), mag_(top_, 0.0), along_(top_, 0.0), across_(top_, 0.0) {}

  // Keeps this frame's magnitudes.
  void listen(const double* re, const double* im) {
    for (int k = 0; k < top_; k++) mag_[k] = jsHypot(re[k], im[k]);
    float* slot = &history_[static_cast<std::size_t>((frames_ % kMaskMaxFrames) * top_)];
    for (int k = 0; k < top_; k++) slot[k] = static_cast<float>(mag_[k]);
    frames_++;
  }

  // How much of each bin of the latest frame is body (out[k], bins 0 to n/2).
  void body(const MaskSettings& p, double* out) {
    const int half = top_ - 1, have = std::min(frames_, p.frames);
    for (int k = 0; k <= half; k++) {
      for (int f = 0; f < have; f++) pick_[f] = history_[static_cast<std::size_t>(((frames_ - 1 - f + kMaskMaxFrames * 4) % kMaskMaxFrames) * top_ + k)];
      along_[k] = middle(have);
    }
    const int reach = p.bins >> 1;
    for (int k = 0; k <= half; k++) {
      const int lo = std::max(0, k - reach), hi = std::min(half, k + reach);
      int count = 0;
      for (int j = lo; j <= hi; j++) pick_[count++] = mag_[j];
      across_[k] = middle(count);
    }
    for (int k = 0; k <= half; k++) {
      const double h = js::pow(along_[k] * p.lean, p.sharp), q = js::pow(across_[k], p.sharp), total = h + q;
      out[k] = total > 1e-12 ? h / total : 0.5;
    }
  }

private:
  // The middle value of the first `count` numbers of pick_ (a small insertion sort: the lists are short).
  double middle(int count) {
    for (int i = 1; i < count; i++) {
      const double v = pick_[i];
      int j = i - 1;
      while (j >= 0 && pick_[j] > v) { pick_[j + 1] = pick_[j]; j--; }
      pick_[j + 1] = v;
    }
    return pick_[count >> 1];
  }

  int top_;
  int frames_ = 0;
  std::vector<float> history_;
  std::vector<double> mag_, along_, across_;
  double pick_[kMaskMaxFrames > kMaskMaxBins ? kMaskMaxFrames : kMaskMaxBins] = {};
};

}  // namespace wax

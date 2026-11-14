// Shared by the spectral effects: the sound is cut into overlapping Hann-windowed frames, a quarter of a frame apart,
// each frame is turned into a spectrum, and after the effect has changed the spectrum the frames are turned back into
// sound and overlapped (Hann again: four frames add up to 1.5, which Resynth divides out). One Analyser listens to one
// signal; one Resynth makes one output. The dry sound, delayed(), comes out one frame late, to match.
//
//   Analyser a(2048); Resynth r(2048);
//   for each sample: if (a.push(x)) { a.fill(re, im); ...change bins 0..n/2...; mirrorSpectrum(re, im, n); r.add(re, im, a.t()); }
//                    out = r.take(a.t());  dry = a.delayed();
// Port of webtracker src/audio/nodes/stft.js.
#pragma once

#include <algorithm>
#include <vector>

#include "core/js_math.h"
#include "nodes/fft.h"

namespace wax {

inline constexpr int kStftMaxSize = 4096;

// The frame length for a `size` parameter value (0, 1, 2): 1024, 2048 or 4096.
inline int stftSizeOf(double value) {
  constexpr int sizes[3] = {1024, 2048, 4096};
  return sizes[std::min(2, std::max(0, jsRoundInt(value)))];
}

// A sine of amplitude 1 comes out of Analyser::fill with a peak of this height (the sum of the window, halved).
inline double stftPeakOf(int n) { return n / 4.0; }

// The Hann window for a frame length, made once per length.
const std::vector<double>& hannWindow(int n);

class Analyser {
public:
  explicit Analyser(int n) : n_(n), hop_(n / 4), buf_(static_cast<std::size_t>(n), 0.0f), window_(&hannWindow(n)), plan_(&fftPlan(n)) {}

  // One sample in. True when a frame is due (every hop, once a whole frame has been heard).
  bool push(float x) {
    buf_[static_cast<std::size_t>(t_ % n_)] = x;
    t_++;
    return t_ >= n_ && t_ % hop_ == 0;
  }
  // The latest frame as a spectrum: re and im hold bins 0 to n - 1 (the top half mirrors the bottom).
  void fill(double* re, double* im) const {
    for (int j = 0; j < n_; j++) {
      re[j] = buf_[static_cast<std::size_t>((t_ - n_ + j) % n_)] * (*window_)[static_cast<std::size_t>(j)];
      im[j] = 0;
    }
    plan_->run(re, im);
  }
  // The sample that went in one frame ago: the dry sound, in time with the frames' output.
  double delayed() const { return buf_[static_cast<std::size_t>(t_ % n_)]; }
  long t() const { return t_; }

private:
  int n_, hop_;
  long t_ = 0;
  std::vector<float> buf_;
  const std::vector<double>* window_;
  const FftPlan* plan_;
};

class Resynth {
public:
  explicit Resynth(int n) : n_(n), acc_(static_cast<std::size_t>(n * 2), 0.0f), window_(&hannWindow(n)), plan_(&fftPlan(n)) {}

  // Turns a spectrum back into sound (re and im are used up) and adds the frame in; `t` is the analyser's clock.
  void add(double* re, double* im, long t) {
    const long mask = n_ * 2 - 1;
    plan_->run(re, im, true);
    for (int j = 0; j < n_; j++) {
      float& slot = acc_[static_cast<std::size_t>((t - n_ + j + n_ * 2) & mask)];
      slot = static_cast<float>(slot + (re[j] * (*window_)[static_cast<std::size_t>(j)]) / 1.5);
    }
  }
  // The finished sample for this moment (and the place is cleared for the next frames).
  double take(long t) {
    if (t < n_) return 0;
    float& slot = acc_[static_cast<std::size_t>((t - n_ + n_ * 2) & (n_ * 2 - 1))];
    const double v = slot;
    slot = 0;
    return v;
  }

private:
  int n_;
  std::vector<float> acc_;
  const std::vector<double>* window_;
  const FftPlan* plan_;
};

// Fills the top half of a spectrum from its bottom half (bins 0 to n/2), so it turns back into real sound.
inline void mirrorSpectrum(double* re, double* im, int n) {
  const int half = n >> 1;
  im[0] = 0;
  im[half] = 0;
  for (int k = 1; k < half; k++) { re[n - k] = re[k]; im[n - k] = -im[k]; }
}

}  // namespace wax

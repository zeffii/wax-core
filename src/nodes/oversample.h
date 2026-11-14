// Oversampling for nonlinear stages: factor 1, 2 or 4, built from half-band FIR stages (41 taps, Blackman window).
// run(x, shape) takes one input sample and returns one output sample, calling shape() at the higher rate.
// latency() is the delay it adds, in input samples.
// Port of webtracker src/audio/nodes/oversample.js.
#pragma once

#include <cmath>
#include <vector>

#include "nodes/shared_dsp.h"
#include "core/js_math.h"

namespace wax {

namespace oversample_detail {

inline constexpr int kHalf = 20;   // the filter reaches this many samples either side of its centre

// The taps of a half-band lowpass: the centre and the odd offsets (the even ones are zero).
struct Taps {
  std::vector<int> offset;
  std::vector<double> weight;

  Taps() {
    for (int j = 0; j <= 2 * kHalf; j++) {
      const int n = j - kHalf;
      if (n != 0 && n % 2 == 0) continue;
      const double sinc = n == 0 ? 1 : js::sin((kPi * n) / 2) / ((kPi * n) / 2);
      const double window = 0.42 + 0.5 * js::cos((kPi * n) / kHalf) + 0.08 * js::cos((2 * kPi * n) / kHalf);
      offset.push_back(j);
      weight.push_back(0.5 * sinc * window);
    }
  }
};

inline const Taps& taps() {
  static const Taps shared;
  return shared;
}

class HalfBand {
public:
  double push(double x) {
    const Taps& t = taps();
    const int length = 2 * kHalf + 1;
    buf_[pos_] = x;
    double sum = 0;
    for (std::size_t k = 0; k < t.offset.size(); k++) {
      int at = pos_ - t.offset[k];
      if (at < 0) at += length;
      sum += t.weight[k] * buf_[at];
    }
    if (++pos_ == length) pos_ = 0;
    return sum;
  }

private:
  double buf_[2 * kHalf + 1] = {};
  int pos_ = 0;
};

}  // namespace oversample_detail

// The delay in samples of the half-band filters for an oversampling factor of 1, 2 or 4.
inline int oversampleLatency(int factor) { return factor == 4 ? 30 : factor == 2 ? 20 : 0; }

class Oversampler {
public:
  explicit Oversampler(int factor) : levels_(factor == 4 ? 2 : factor == 2 ? 1 : 0), latency_(oversampleLatency(factor)) {}

  int latency() const { return latency_; }

  template <typename Shape>
  double run(double x, const Shape& shape) {
    if (levels_ == 0) return shape(x);
    if (levels_ == 1) return twice(0, x, shape);
    return twice(0, x, [&](double v) { return twice(1, v, shape); });
  }

private:
  // One stage: the sample goes up to twice the rate (itself, then a zero), `inner` works on both, and the two results
  // come back down to one.
  template <typename Inner>
  double twice(int level, double x, const Inner& inner) {
    const double a = inner(up_[level].push(2 * x));
    const double b = inner(up_[level].push(0));
    down_[level].push(a);
    return down_[level].push(b);
  }

  int levels_;
  int latency_;
  oversample_detail::HalfBand up_[2], down_[2];
};

// A fixed delay in samples, to line the dry signal up with an oversampled one.
class FixedDelay {
public:
  explicit FixedDelay(int max) : buf_(static_cast<std::size_t>(max + 1), 0.0) {}

  double push(double x, int samples) {
    const int length = static_cast<int>(buf_.size());
    buf_[static_cast<std::size_t>(pos_)] = x;
    int at = pos_ - samples;
    if (at < 0) at += length;
    if (++pos_ == length) pos_ = 0;
    return buf_[static_cast<std::size_t>(at)];
  }

private:
  std::vector<double> buf_;
  int pos_ = 0;
};

}  // namespace wax

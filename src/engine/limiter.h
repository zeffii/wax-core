// The master limiter: a lookahead peak limiter that keeps the mix under a ceiling without ever letting a peak through.
// Port of webtracker src/audio/limiter.js (the comments there explain how it works).
#pragma once

#include <vector>

namespace wax {

class Limiter {
public:
  explicit Limiter(double sampleRate);

  struct Result {
    double reductionDb = 0;   // the most it held back in this block
    double peakDb = 0;        // the loudest input, after the boost
  };

  // In place. boostDb goes in before the limiter.
  Result process(float* left, float* right, int n, double ceilingDb = -0.3, double boostDb = 0, double releaseMs = 120);

  int latency() const { return look_ - 1; }

private:
  double sampleRate_;
  int look_;
  std::vector<float> delayedLeft_, delayedRight_;   // the delayed sound
  std::vector<float> need_;                         // the gain each of the last `look` samples needs
  std::vector<float> held_;                         // the held gain, for the smoothing window
  int pos_ = 0;
  double sum_;
  double release_ = 1;
};

}  // namespace wax

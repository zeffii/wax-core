// A radix-2 FFT with its tables made once per size. run(re, im) transforms in place (forward: e^-i);
// run(re, im, true) is the inverse and divides by n. The arrays hold exactly n numbers.
// Port of webtracker src/audio/fft.js.
#pragma once

#include <cstdint>
#include <vector>

namespace wax {

class FftPlan {
public:
  explicit FftPlan(int n);

  int size() const { return n_; }
  void run(double* re, double* im, bool inverse = false) const;

private:
  int n_;
  std::vector<std::uint32_t> reversed_;   // each index with its bits the other way round
  std::vector<double> cos_, sin_;
};

// The plan for a size (a power of two), made the first time it is asked for and shared from then on.
const FftPlan& fftPlan(int n);

}  // namespace wax

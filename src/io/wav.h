// Stereo sound files: written as 32-bit float (what the reference renders are), read from 16, 24 or 32-bit float WAVs.
#pragma once

#include <string>
#include <vector>

namespace wax {

struct StereoAudio {
  int sampleRate = 44100;
  std::vector<float> left;
  std::vector<float> right;

  std::size_t frames() const { return left.size(); }
};

// Throws std::runtime_error when the file cannot be read or is not a WAV this reads. A mono file fills both sides.
StereoAudio readWav(const std::string& path);

// 32-bit float, no clipping.
void writeWav(const std::string& path, const StereoAudio& audio);

// How two renders differ.
struct AudioDifference {
  std::size_t frames = 0;        // compared (the shorter of the two)
  std::size_t lengthGap = 0;     // how many frames longer one is than the other
  double peakDb = -300;          // the largest difference of any one sample, in dB (0 dB = full scale)
  double rmsDb = -300;           // the difference as a whole
  std::size_t worstFrame = 0;    // where the largest difference is
};

AudioDifference compareAudio(const StereoAudio& a, const StereoAudio& b);

}  // namespace wax

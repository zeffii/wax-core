// The kaleidoscope's geometry and frame step: a band of the spectrum is repeated up and down the range (mirrored, repeated, or
// as an overtone series). A tone moved to another place keeps its phase in step from frame to frame, so the effect works on the
// tones themselves (the peaks of each frame) and writes each one into every tile as a clean sinusoid.
// Port of the playing part of webtracker src/audio/kalcore.js (the panel's half, pictureOf, is for the window).
#pragma once

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

}  // namespace wax::kal

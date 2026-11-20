// The spread table behind the pad synth (padsynth) and the spread oscillator (spreadosc): a few harmonics, each smeared into a band of
// frequencies with random phases, made into a table by one inverse FFT and played at a speed (the PADsynth idea). A table is made for each
// octave band and channel, and kept by its settings. Port of webtracker src/audio/nodes/padcore.js, to the last bit.
#pragma once

#include <memory>
#include <vector>

namespace wax {

struct SpreadSettings {
  double bandwidth, spread, tilt, evenodd, stretch;
  int harmonics;
};

struct SpreadTables {
  int size = 0;
  std::vector<float> left, right;
};

int spreadBandOf(double note);
double spreadBandPitch(int band);

// The tables for a band and settings (made the first time, then kept; shared by every node and thread).
std::shared_ptr<const SpreadTables> spreadTables(double sampleRate, int quality, int band, const SpreadSettings& settings);

// Reads a pair of tables at a speed: each channel has its own place in its table and its own slow wobble of the speed (`motion`, in cents).
class SpreadPlayer {
public:
  explicit SpreadPlayer(double sampleRate) : sampleRate_(sampleRate) {}
  void restart();   // a new note starts the tables at a random place and the wobble at a random phase
  // `speed`: table steps per sample (the note's frequency over the table's pitch). Fills outL / outR for n samples.
  void render(const SpreadTables& tables, int n, double speed, double motion, double width, float* outL, float* outR);

private:
  double sampleRate_;
  double posL_ = 0, posR_ = 0, lfoL_ = 0, lfoR_ = 0;
  std::uint32_t rng_ = 0x1234abcd;
  bool fresh_ = false;
};

}  // namespace wax

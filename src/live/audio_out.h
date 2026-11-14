// A sound card, played by a Player. miniaudio is kept out of this header: it is one very large file.
#pragma once

#include <memory>
#include <string>

#include "live/player.h"

namespace wax {

class AudioOut {
public:
  // `useNull`: miniaudio's null device (time runs, nothing is heard). Throws std::runtime_error when no device opens.
  AudioOut(Player& player, double sampleRate, bool useNull = false);
  ~AudioOut();
  AudioOut(const AudioOut&) = delete;
  AudioOut& operator=(const AudioOut&) = delete;

  void start();
  std::string deviceName() const;
  std::string backendName() const;
  double sampleRate() const;   // what the device really runs at

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace wax

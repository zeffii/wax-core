// A sound card, played by a Player. miniaudio is kept out of this header: it is one very large file.
#pragma once

#include <memory>
#include <string>

#include "live/player.h"

namespace wax {

class AudioOut {
public:
  // `useNull`: miniaudio's null device (time runs, nothing is heard). Throws std::runtime_error when no device opens.
  AudioOut(Player& player, double sampleRate, bool useNull = false, int bufferFrames = 0);
  ~AudioOut();
  AudioOut(const AudioOut&) = delete;
  AudioOut& operator=(const AudioOut&) = delete;

  void start();
  std::string deviceName() const;
  std::string backendName() const;
  double sampleRate() const;   // what the device really runs at
  int bufferFrames() const;    // the size of one callback, as the device was given it (0 asked: the device's own choice)
  // How busy the callback is: the share of the time a buffer lasts that filling it took (the worst of the recent ones),
  // and how many buffers took longer than they last (each one is a click).
  double load() const;
  long lateBuffers() const;

  struct Impl;   // (public only so the audio callback in audio_out.cpp can name it)

private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace wax

// A small ADSR state machine shared by the synth nodes (the env node keeps its own copy). Attack is linear; decay and
// release are exponential, with times to fall by 60 dB. A note on restarts the attack from the current level, note off
// releases, a cut zeroes it.
// Port of webtracker src/audio/nodes/adsr.js.
#pragma once

#include <algorithm>
#include <cmath>
#include "core/js_math.h"

namespace wax {

class Adsr {
public:
  explicit Adsr(double sampleRate) : sampleRate_(sampleRate) {}

  // times in ms; call once per block
  void set(double attack, double decay, double sustain, double release) {
    static const double ln60Db = js::log(0.001);
    const double ms = sampleRate_ * 0.001;
    attackStep_ = 1 / (std::max(0.1, attack) * ms);
    decayCoef_ = js::exp(ln60Db / (std::max(1.0, decay) * ms));
    releaseCoef_ = js::exp(ln60Db / (std::max(1.0, release) * ms));
    sustain_ = sustain;
  }

  bool idle() const { return stage_ == Stage::Idle; }
  bool held() const { return stage_ == Stage::Attacking || stage_ == Stage::Decaying; }   // stage 1 or 2 of the JS
  void on() { stage_ = Stage::Attacking; }
  void off() { if (stage_ == Stage::Attacking || stage_ == Stage::Decaying) stage_ = Stage::Releasing; }
  void cut() { stage_ = Stage::Idle; level_ = 0; }

  double next() {
    if (stage_ == Stage::Attacking) {
      level_ += attackStep_;
      if (level_ >= 1) { level_ = 1; stage_ = Stage::Decaying; }
    } else if (stage_ == Stage::Decaying) {
      level_ = sustain_ + (level_ - sustain_) * decayCoef_;
      if (level_ - sustain_ < 1e-6) level_ = sustain_;
    } else if (stage_ == Stage::Releasing) {
      level_ *= releaseCoef_;
      if (level_ < 1e-6) { level_ = 0; stage_ = Stage::Idle; }
    }
    return level_;
  }

  double level() const { return level_; }

private:
  enum class Stage { Idle, Attacking, Decaying, Releasing };

  double sampleRate_;
  Stage stage_ = Stage::Idle;
  double level_ = 0;
  double attackStep_ = 1, decayCoef_ = 0.99, releaseCoef_ = 0.99, sustain_ = 1;
};

}  // namespace wax

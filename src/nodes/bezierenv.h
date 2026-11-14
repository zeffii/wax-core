// A drawn envelope (a Bezier spline, see bezier.h) played by a note: the part a synth needs to use a drawing where it would
// otherwise have an ADSR. With a sustain knot it plays up to the knot and waits there while the note is held, then plays on to the
// last anchor; without one it is one shot. A new note slides from where the envelope was to the drawing in 2 ms. Port of webtracker
// src/audio/nodes/bezierenv.js.
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

#include "core/js_math.h"
#include "nodes/bezier.h"

namespace wax {

class BezierEnvelope {
public:
  BezierEnvelope(double sampleRate, bezier::Spline fallback)
      : sampleRate_(sampleRate), fallback_(std::move(fallback)), slide_(js::exp(-1 / (kSlideS * sampleRate))), length_(sampleRate) {
    setDrawing(nullptr);
  }

  // The drawing `name` of a node's data (data.env.<name>), or the fallback.
  void setDrawing(const Json* data, const char* name = "amp") {
    const bezier::Spline clean = bezier::drawingOf(data, name, fallback_);
    table_ = bezier::envelopeTable(clean);
    knot_ = bezier::sustainKnot(clean);
    endLevel_ = table_[table_.size() - 1];
  }

  // how long the whole drawing takes, in ms
  void setTime(double ms) { length_ = std::max(1.0, ms * 0.001 * sampleRate_); }

  bool idle() const { return phase_ == Phase::Idle || phase_ == Phase::Ended; }

  void on() {
    offset_ = phase_ == Phase::Idle ? 0 : value_ - bezier::readEnvelope(table_, 0);
    phase_ = Phase::Run;
    position_ = 0;
  }

  void off() {
    if (!knot_ || (phase_ != Phase::Run && phase_ != Phase::Hold)) return;
    if (phase_ == Phase::Hold) { position_ = knot_->x * length_; phase_ = Phase::Release; return; }
    const double release = ((1 - knot_->x) * length_) / sampleRate_;
    fadeLength_ = fadeLeft_ = jsRound(std::min(kFadeMaxS, std::max(kFadeMinS, release)) * sampleRate_);
    fadeFrom_ = shape_;
    phase_ = Phase::Fade;
  }

  void cut() { phase_ = Phase::Idle; value_ = 0; offset_ = 0; }

  // the next value, one sample on
  double next() {
    if (phase_ == Phase::Idle) return 0;
    double x = position_ / length_;
    if (phase_ == Phase::Run && knot_ && x >= knot_->x) { phase_ = Phase::Hold; x = knot_->x; }
    if (phase_ == Phase::Ended) shape_ = endLevel_;
    else if (phase_ == Phase::Fade) {
      shape_ = endLevel_ + (fadeFrom_ - endLevel_) * (fadeLeft_ / fadeLength_);
      if (--fadeLeft_ < 0) phase_ = Phase::Ended;
    } else {
      shape_ = bezier::readEnvelope(table_, x);
      if (x >= 1) phase_ = Phase::Ended;
      else if (phase_ != Phase::Hold) position_ += 1;
    }
    offset_ *= slide_;
    value_ = shape_ + offset_;
    return value_;
  }

private:
  enum class Phase { Idle, Run, Hold, Release, Fade, Ended };
  static constexpr double kFadeMinS = 0.01, kFadeMaxS = 0.06, kSlideS = 0.002;

  double sampleRate_;
  bezier::Spline fallback_;
  double slide_;
  double length_;
  Phase phase_ = Phase::Idle;
  double position_ = 0, value_ = 0, shape_ = 0, offset_ = 0, fadeFrom_ = 0, fadeLeft_ = 0, fadeLength_ = 1;
  bezier::Table table_{};
  std::optional<bezier::Knot> knot_;
  double endLevel_ = 0;
};

}  // namespace wax

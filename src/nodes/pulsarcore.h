// Pulsar synthesis (after Curtis Roads), shared by the `pulsar` synth and the `pulsetrain` source. A pulsar train is a short
// burst of sound, the pulsaret, fired over and over: `fundamental` is how often one starts, `formant` how fast the pulsaret
// itself plays, and a drawn `window` is laid over each. `mask` drops a share of them, `jitter` moves their start times and
// `scatter` throws each to a random place in the stereo field. Port of webtracker src/audio/nodes/pulsarcore.js.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/bezier.h"
#include "nodes/shared_dsp.h"

namespace wax::pulsar {

// A window that is a plain hump, until one is drawn.
inline bezier::Spline defaultWindow() {
  const double r0[2] = {0.22, 0}, l1[2] = {-0.2, 0}, r1[2] = {0.2, 0}, l2[2] = {-0.22, 0};
  return {bezier::makeAnchor(0, 0, nullptr, r0), bezier::makeAnchor(0.5, 1, l1, r1), bezier::makeAnchor(1, 0, l2, nullptr)};
}

class PulsarTrain {
public:
  PulsarTrain() : table_(bezier::envelopeTable(defaultWindow())) {}

  // `data` is the node's data (data.env.window is the drawn window), or null.
  void setWindow(const Json* data) { table_ = bezier::envelopeTable(bezier::drawingOf(data, "window", defaultWindow())); }
  void reset() { phase_ = 0; length_ = 1; q_ = 2; }

  // inc: fundamental / sampleRate; fInc: formant / sampleRate (both per sample); the rest are the settings. Returns left in out[0].
  void step(double inc, double fInc, int wave, double mask, double jitter, double scatter, double out[2]) {
    phase_ += inc;
    if (phase_ >= length_) {
      phase_ -= length_;
      if (jitter > 0) { const double r = rand(); length_ = 1 + (r * 2 - 1) * jitter * 0.5; } else length_ = 1;
      q_ = inc > 0 ? std::min(1.0, phase_ / inc) * fInc : 0;
      q_ = std::min(q_, 0.999);
      if (mask <= 0) on_ = true; else on_ = rand() >= mask;
      double pan = 0;
      if (scatter > 0) { const double r = rand(); pan = (r * 2 - 1) * scatter; }
      const double a = ((pan + 1) * kPi) / 4;
      gl_ = js::cos(a) * kSqrt2;
      gr_ = js::sin(a) * kSqrt2;
    }
    double v = 0;
    if (q_ < 1) {
      if (on_) { const double w = pulsaretWave(wave, q_); v = w * bezier::readEnvelope(table_, q_); }
      q_ += fInc;
    }
    out[0] = v * gl_;
    out[1] = v * gr_;
  }

private:
  double pulsaretWave(int wave, double q) {
    switch (wave) {
      case 0: return js::sin(kTwoPi * q);
      case 1: return js::sin(kPi * q) * 2 - 1.273;
      case 2: return q < 0.25 ? 4 * q : q < 0.75 ? 2 - 4 * q : 4 * q - 4;
      case 3: return 2 * q - 1;
      case 4: return q < 0.5 ? 1 : -1;
      default: return rand() * 2 - 1;
    }
  }
  double rand() {
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    return seed_ / 4294967296.0;
  }

  bezier::Table table_;
  double phase_ = 0, length_ = 1, q_ = 2, gl_ = 1, gr_ = 1;
  bool on_ = true;
  std::uint32_t seed_ = 0x9e3779b9u;
};

}  // namespace wax::pulsar

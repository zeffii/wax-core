// The saturation stage shared by the two saturators (`saturator` and `mbsat`): drive a curve, keep what it makes, take the offset
// away, and keep the loudness about the same whatever the drive. One Stage is one channel of one band.
//   soft: tanh   tape: a gentler arctangent   tube: lopsided, adds even harmonics   clip: a cubic soft clip
// `bias` pushes the signal off-centre into the curve. The curve runs at 1x, 2x or 4x (`quality`: standard is 2x, eco 1x, hq 4x);
// the dry sound has to be delayed by latency() to match. Port of webtracker src/audio/nodes/saturation.js.
#pragma once

#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/oversample.h"
#include "nodes/shared_dsp.h"

namespace wax::saturation {

inline constexpr int kFactors[3] = {2, 1, 4};   // the oversampling each quality uses

inline int typeOf(double value) { return std::min(3, std::max(0, jsRoundInt(value))); }
inline int qualityOf(double value) { return std::min(2, std::max(0, jsRoundInt(value))); }
inline double dbToGain(double db) { return js::pow(10, db / 20); }

inline double shapeOf(int type, double x) {
  switch (type) {
    case 0: return js::tanh(x);
    case 1: return (2 / kPi) * js::atan((x * kPi) / 2);
    case 2: return x >= 0 ? js::tanh(x) : 1.8 * js::tanh(x * 0.35);
    default: return x > 1.5 ? 1 : x < -1.5 ? -1 : x * (1 - (x * x * 4) / 27);
  }
}

// What the curve needs for a block: its drive (a gain), its bias, the offset, and the factor that keeps the loudness level.
struct Settings {
  int type;
  double gain, bias, offset, norm;
  double curve(double v) const { return shapeOf(type, v * gain + bias) - offset; }
};

inline Settings settings(double type, double driveDb, double bias) {
  Settings s;
  s.type = typeOf(type);
  s.gain = dbToGain(driveDb);
  s.bias = bias;
  s.offset = shapeOf(s.type, bias);
  s.norm = 0.5 / shapeOf(s.type, 0.5 * s.gain);
  return s;
}

class Stage {
public:
  Stage() : chains_{Oversampler(kFactors[0]), Oversampler(kFactors[1]), Oversampler(kFactors[2])} {}

  int latency(double quality) const { return chains_[qualityOf(quality)].latency(); }

  // One sample through the curve; `hpK` is the one-pole that follows the offset.
  double run(double x, const Settings& s, double quality, double hpK) {
    double wet = chains_[qualityOf(quality)].run(x, [&](double v) { return s.curve(v); }) * s.norm;
    hp_ += (wet - hp_) * hpK;
    wet -= hp_;
    return wet;
  }

private:
  Oversampler chains_[3];
  double hp_ = 0;
};

inline double offsetK(double sampleRate) { return 1 - js::exp((-2 * kPi * 12) / sampleRate); }

}  // namespace wax::saturation

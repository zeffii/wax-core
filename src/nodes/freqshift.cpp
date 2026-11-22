// Frequency shifter: every frequency in the sound moves by the same number of Hz. A 90 degree shifted copy of the sound
// (a pair of allpass chains, the usual cheap Hilbert transform) is mixed with a sine and a cosine at the shift
// frequency. `feedback` sends the shifted sound back in. Stereo, both sides shifted by the same oscillator.
// Port of webtracker src/audio/nodes/freqshift.js.
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kShift = 0, kFeedback, kMix };

// coefficients of the two chains (squared inside): 90 degrees apart over most of the audio range
constexpr double kA1[4] = {0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737};
constexpr double kA2[4] = {0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278};

struct Hilbert {
  double x1[4] = {}, x2[4] = {}, y1[4] = {}, y2[4] = {};   // per section: input and output, 1 and 2 samples ago, chain 1 ...
  double u1[4] = {}, u2[4] = {}, v1[4] = {}, v2[4] = {};   // ... and chain 2
  double delay = 0, i = 0, q = 0;

  void run(double x) {
    double a = x, b = x;
    for (int k = 0; k < 4; k++) {
      const double ya = (kA1[k] * kA1[k]) * (a + y2[k]) - x2[k];
      x2[k] = x1[k]; x1[k] = a; y2[k] = y1[k]; y1[k] = ya; a = ya;
      const double yb = (kA2[k] * kA2[k]) * (b + v2[k]) - u2[k];
      u2[k] = u1[k]; u1[k] = b; v2[k] = v1[k]; v1[k] = yb; b = yb;
    }
    q = b;
    i = delay; delay = a;   // the first chain is one sample late
  }
};

class FreqShift final : public Node {
public:
  explicit FreqShift(const NodeContext& context) : sampleRate_(context.sampleRate) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const double inc = params[slots[kShift]] / sampleRate_, feedback = params[slots[kFeedback]], mix = params[slots[kMix]];
    for (int i = 0; i < n; i++) {
      const double c = js::cos(2 * kPi * phase_), s = js::sin(2 * kPi * phase_);
      for (int ch = 0; ch < 2; ch++) {
        const double dry = inputs[ch][i];
        h_[ch].run(dry + fb_[ch] * feedback);
        const double wet = h_[ch].i * c + h_[ch].q * s;
        fb_[ch] = std::abs(wet) < 1e-20 ? 0 : js::tanh(wet);
        outputs[ch][i] = static_cast<float>(dry + (wet - dry) * mix);
      }
      phase_ += inc;
      phase_ -= std::floor(phase_);
    }
  }

private:
  double sampleRate_;
  Hilbert h_[2];
  double phase_ = 0, fb_[2] = {0, 0};
};

}  // namespace

void registerFreqShift(NodeRegistry& registry) {
  NodeType type;
  type.type = "freqshift";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.isEffect = true;
  type.dryAtZero = "mix";
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.params = {
    {"shift", -1000, 1000, 7},   // Hz, up or down
    {"feedback", 0, 0.9, 0},
    {"mix", 0, 1, 0.5},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<FreqShift>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

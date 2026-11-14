// Low-frequency oscillator: a control source that free-runs. With reset = 1 a note restarts it. shape: 0 sine,
// 1 triangle, 2 saw, 3 square. Output: with range 0 the wave times amount (a swing either side of zero); with range 1
// the wave runs between min and max, whatever they are.
// `steps` is how many times per tick the wave is worked out; between updates the last value is held. 0 works it out for
// every sample. A wire into a parameter only reads the end of each block anyway, so a few updates per tick sound the
// same and cost far less than a sine for every sample.
// Port of webtracker src/audio/nodes/lfo.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kRate = 0, kShape, kAmount, kReset, kRange, kMin, kMax, kSteps };

class Lfo final : public Node {
public:
  explicit Lfo(const NodeContext& context) : sampleRate_(context.sampleRate), clock_(context.clock) {}

  void note(const NoteEvent& event) override { if (event.type == NoteType::On) pendingReset_ = true; }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* out = outputs[0];
    const double inc = params[slots[kRate]] / sampleRate_, amount = params[slots[kAmount]];
    const int shape = jsRoundInt(params[slots[kShape]]);
    const bool between = params[slots[kRange]] >= 0.5f;
    const double lo = params[slots[kMin]], half = (params[slots[kMax]] - lo) * 0.5;
    if (pendingReset_ && params[slots[kReset]] >= 0.5f) phase_ = 0;
    pendingReset_ = false;
    const int steps = jsRoundInt(params[slots[kSteps]]);
    const double every = steps > 0 ? std::max(1.0, (clock_ ? clock_->tickSamples : sampleRate_ * 0.125) / steps) : 1;
    for (int i = 0; i < n; i++) {
      if (until_ <= 0) {
        double v;
        if (shape == 0) v = std::sin(kTwoPi * phase_);
        else if (shape == 1) v = phase_ < 0.25 ? 4 * phase_ : phase_ < 0.75 ? 2 - 4 * phase_ : 4 * phase_ - 4;
        else if (shape == 2) v = 2 * phase_ - 1;
        else v = phase_ < 0.5 ? 1 : -1;
        held_ = between ? lo + (v + 1) * half : v * amount;
        until_ += every;
      }
      until_--;
      out[i] = static_cast<float>(held_);
      phase_ += inc;
      if (phase_ >= 1) phase_ -= 1;
    }
  }

private:
  double sampleRate_;
  const Clock* clock_;
  double phase_ = 0, until_ = 0, held_ = 0;
  bool pendingReset_ = false;
};

}  // namespace

void registerLfo(NodeRegistry& registry) {
  NodeType type;
  type.type = "lfo";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Control}};
  type.params = {
    {"rate", 0.01, 50, 2},            // Hz
    {"shape", 0, 3, 0, true},
    {"amount", 0, 10, 1},
    {"reset", 0, 1, 0, true},
    {"range", 0, 1, 0, true},         // 0: swing by amount, 1: between min and max
    {"min", -1000, 1000, 0},
    {"max", -1000, 1000, 1},
    {"steps", 0, 256, 0, true},       // updates per tick, 0: every sample
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Lfo>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

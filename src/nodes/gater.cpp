// Trance gate: the sound is switched on and off by a pattern of steps (levels drawn in the panel, kept in the node's
// `data.gate`), locked to the song's tempo. Each step lasts `div` ticks, the pattern is `steps` long, `shift` rotates
// it, `depth` is how far the gate closes and `smooth` how long it takes to open and close. While the song is not
// playing the gate runs on its own at the same tempo. Port of webtracker src/audio/nodes/gater.js.
#include <algorithm>
#include <array>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kDiv = 0, kSteps, kShift, kDepth, kSmooth };

constexpr int kGateSteps = 64;
constexpr double kDefaultGate[16] = {1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 0};
constexpr double kDivisions[8] = {0.25, 0.5, 1, 2, 3, 4, 6, 8};

// whatever the knob says snaps to the nearest step length, so a step always lands on a musical grid
double snapDiv(double v) {
  double best = 1;
  for (const double d : kDivisions) if (std::abs(d - v) < std::abs(best - v)) best = d;
  return best;
}

// up to 64 levels from 0 to 1; a shorter list repeats to fill the rest
std::array<double, kGateSteps> normalizeGate(const Json* data) {
  std::vector<double> points(kDefaultGate, kDefaultGate + 16);
  if (data && data->contains("gate") && (*data)["gate"].is_array() && !(*data)["gate"].empty()) {
    bool ok = true;
    std::vector<double> given;
    for (const auto& v : (*data)["gate"]) {
      if (!v.is_number()) { ok = false; break; }
      given.push_back(v.get<double>());
    }
    if (ok) points = given;
  }
  std::array<double, kGateSteps> gate{};
  for (int i = 0; i < kGateSteps; i++) gate[i] = std::min(1.0, std::max(0.0, points[i % points.size()]));
  return gate;
}

class Gater final : public Node {
public:
  explicit Gater(const NodeContext& context)
      : sampleRate_(context.sampleRate), clock_(context.clock), gate_(normalizeGate(context.data)) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inL = inputs[0];
    const float* inR = inputs[1];
    float* outL = outputs[0];
    float* outR = outputs[1];
    const double div = snapDiv(params[slots[kDiv]]);
    const int steps = std::min(kGateSteps, std::max(1, jsRoundInt(params[slots[kSteps]])));
    const int shift = jsRoundInt(params[slots[kShift]]);
    const double depth = params[slots[kDepth]];
    const double coef = 1 - js::exp(-1 / (sampleRate_ * std::max(0.1, static_cast<double>(params[slots[kSmooth]])) * 0.001));
    const double tickSamples = clock_ ? clock_->tickSamples : sampleRate_ * 0.125;
    const double perSample = 1 / (tickSamples * div);   // steps per sample
    const bool playing = clock_ && clock_->playing;
    double pos = playing ? clock_->songPos / div : free_;
    for (int i = 0; i < n; i++) {
      const double k = std::fmod(std::fmod(std::floor(pos) + shift, steps) + steps, steps);
      const double target = 1 - depth * (1 - gate_[static_cast<std::size_t>(k)]);
      gain_ += (target - gain_) * coef;
      outL[i] = static_cast<float>(inL[i] * gain_);
      outR[i] = static_cast<float>(inR[i] * gain_);
      pos += perSample;
    }
    if (!playing) free_ = pos;
    if (!playing) free_ = std::fmod(free_, 1024);
  }

private:
  double sampleRate_;
  const Clock* clock_;
  std::array<double, kGateSteps> gate_;
  double gain_ = 1, free_ = 0;
};

}  // namespace

void registerGater(NodeRegistry& registry) {
  NodeType type;
  type.type = "gater";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.isEffect = true;
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.params = {
    {"div", 0.25, 8, 1},                // ticks per step
    {"steps", 1, kGateSteps, 16, true},
    {"shift", 0, kGateSteps - 1, 0, true},
    {"depth", 0, 1, 1},
    {"smooth", 0.1, 50, 4},             // ms
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Gater>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

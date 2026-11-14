// Noise gate, stereo-linked: the louder channel drives the detector and both channels get the same gain. The gate opens when the
// level rises above the threshold and closes once it has stayed below the threshold minus the hysteresis for the hold time;
// closing lowers the gain to `range` dB. Attack and release smooth the gain. Port of webtracker src/audio/nodes/gate.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kThreshold = 0, kHyst, kAttack, kHold, kRelease, kRange };
constexpr double kLn10 = 2.302585092994046, kLin = kLn10 / 20;   // Math.LN10 / 20

class Gate final : public Node {
public:
  explicit Gate(const NodeContext& context) : sampleRate_(context.sampleRate), fall_(coef(3)) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double threshold = p(kThreshold), hyst = p(kHyst);
    const double T = js::pow(10, threshold / 20), Tc = js::pow(10, (threshold - hyst) / 20);
    const double ca = coef(p(kAttack)), cr = coef(p(kRelease)), hold = p(kHold) * 0.001 * sampleRate_;
    const double floorGain = js::exp(p(kRange) * kLin);
    for (int i = 0; i < n; i++) {
      const double l = inputs[0][i], r = inputs[1][i], level = std::max(std::abs(l), std::abs(r));
      env_ = level > env_ ? level : env_ * fall_;
      if (env_ >= T) { open_ = true; held_ = hold; }
      else if (open_) {
        if (env_ >= Tc) held_ = hold;
        else if (held_-- <= 0) open_ = false;
      }
      const double target = open_ ? 1 : floorGain;
      gain_ = target > gain_ ? target + (gain_ - target) * ca : target + (gain_ - target) * cr;
      outputs[0][i] = static_cast<float>(l * gain_);
      outputs[1][i] = static_cast<float>(r * gain_);
    }
  }

private:
  double coef(double ms) const { return js::exp(-1 / (ms * 0.001 * sampleRate_)); }

  double sampleRate_, fall_;
  double env_ = 0, gain_ = 1, held_ = 0;
  bool open_ = false;
};

}  // namespace

void registerGate(NodeRegistry& registry) {
  NodeType type;
  type.type = "gate";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {
    {"threshold", -80, 0, -40},
    {"hyst", 0, 24, 6},
    {"attack", 0.05, 100, 1},
    {"hold", 0, 2000, 80},
    {"release", 1, 2000, 120},
    {"range", -80, 0, -80},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Gate>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

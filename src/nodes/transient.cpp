// Transient designer: a fast and a slow envelope follower on the stereo sum; where the fast one is above the slow one a note is
// attacking, where it is below, the sound is sustaining. attack and sustain are gains from -1 to +1 (up to 12 dB) on the two
// parts. Port of webtracker src/audio/nodes/transient.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kAttack = 0, kSustain, kSpeed };
constexpr double kMaxDb = 12, kRangeDb = 6, kDeadzoneDb = 1.5;

class Transient final : public Node {
public:
  explicit Transient(const NodeContext& context) : sampleRate_(context.sampleRate), smooth_(coef(2)) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double attack = p(kAttack), sustain = p(kSustain), speed = p(kSpeed);
    const double fa = coef(1 / speed), fr = coef(20 / speed), sa = coef(25 / speed), sr = coef(120 / speed);
    for (int i = 0; i < n; i++) {
      const double l = inputs[0][i], r = inputs[1][i];
      const double level = std::abs(l + r) * 0.5;
      fast_ = level > fast_ ? level + (fast_ - level) * fa : level + (fast_ - level) * fr;
      slow_ = level > slow_ ? level + (slow_ - level) * sa : level + (slow_ - level) * sr;
      const double d = 20 * js::log10((fast_ + 1e-6) / (slow_ + 1e-6));
      const double up = std::max(d - kDeadzoneDb, 0.0), down = std::max(-d - kDeadzoneDb, 0.0);
      const double db = attack * kMaxDb * std::min(up / kRangeDb, 1.0) + sustain * kMaxDb * std::min(down / kRangeDb, 1.0);
      const double target = js::pow(10, db / 20);
      gain_ = target + (gain_ - target) * smooth_;
      outputs[0][i] = static_cast<float>(l * gain_);
      outputs[1][i] = static_cast<float>(r * gain_);
    }
    if (std::abs(fast_) < 1e-20) fast_ = 0;
    if (std::abs(slow_) < 1e-20) slow_ = 0;
  }

private:
  double coef(double ms) const { return js::exp(-1 / (ms * 0.001 * sampleRate_)); }

  double sampleRate_, smooth_;
  double fast_ = 0, slow_ = 0, gain_ = 1;
};

}  // namespace

void registerTransient(NodeRegistry& registry) {
  NodeType type;
  type.type = "transient";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {{"attack", -1, 1, 0}, {"sustain", -1, 1, 0}, {"speed", 0.25, 4, 1}};
  type.create = [](const NodeContext& context) { return std::make_unique<Transient>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

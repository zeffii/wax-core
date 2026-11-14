// Envelope follower: listens to a signal and outputs how loud it is as a control signal (0 to 1) for a parameter port.
// `attack`/`release` smooth the level, `floor` dB is 0 and `gain` dB above it reaches 1; peak or rms; `invert` flips it.
// Port of webtracker src/audio/nodes/follower.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kAttack = 0, kRelease, kFloor, kGain, kMode, kInvert };

class Follower final : public Node {
public:
  explicit Follower(const NodeContext& context) : sampleRate_(context.sampleRate) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    float* out = outputs[0];
    const double ca = js::exp(-1 / (sampleRate_ * g(kAttack) * 0.001)), cr = js::exp(-1 / (sampleRate_ * g(kRelease) * 0.001));
    const bool rms = g(kMode) >= 1;
    for (int i = 0; i < n; i++) {
      const double l = inputs[0][i], r = inputs[1][i];
      const double x = rms ? (l * l + r * r) * 0.5 : std::max(std::abs(l), std::abs(r));
      env_ = x > env_ ? ca * env_ + (1 - ca) * x : cr * env_ + (1 - cr) * x;
    }
    const double level = std::max(env_, 1e-12);
    const double db = (rms ? 10 : 20) * js::log10(level) + g(kGain);
    double v = std::min(1.0, std::max(0.0, (db - g(kFloor)) / -g(kFloor)));
    if (g(kInvert) >= 1) v = 1 - v;
    for (int i = 0; i < n; i++) out[i] = static_cast<float>(last_ + (v - last_) * (static_cast<double>(i + 1) / n));
    last_ = v;
  }

private:
  double sampleRate_;
  double env_ = 0, last_ = 0;
};

}  // namespace

void registerFollower(NodeRegistry& registry) {
  NodeType type;
  type.type = "follower";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Param}};
  type.params = {{"attack", 0.1, 500, 5}, {"release", 1, 5000, 150}, {"floor", -80, -6, -48}, {"gain", -24, 24, 0}, {"mode", 0, 1, 0, true}, {"invert", 0, 1, 0, true}};
  type.create = [](const NodeContext& context) { return std::make_unique<Follower>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Sweep: one knob that filters "as if it had always been part of the signal". At 0 the sound is untouched; turning down closes a
// lowpass from the top, turning up opens a highpass from the bottom; the resonance grows in with the distance from the middle.
// A zero-delay state-variable filter, one or two stages. `mod` adds to the knob. Mono: runs as a left/right pair on a stereo
// signal. Port of webtracker src/audio/nodes/sweep.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kSweep = 0, kRes, kSlope };
constexpr double kLo = 20, kHi = 18000, kFade = 0.05;

double clip(double x) { return x > 3 ? 1 : x < -3 ? -1 : x * (27 + x * x) / (27 + 9 * x * x); }
double flush(double v) { return std::abs(v) < 1e-18 ? 0 : v; }

class Sweep final : public Node {
public:
  explicit Sweep(const NodeContext& context)
      : sr_(context.sampleRate), modOn_(context.connected.size() > 1 && context.connected[1]),
        ss_(1 - js::exp(-1 / (context.sampleRate * 0.012))) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* input = inputs[0];
    float* out = outputs[0];
    const float* mod = modOn_ ? inputs[1] : nullptr;
    auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double target = p(kSweep), res = std::min(std::max(p(kRes), 0.0), 1.0);
    const bool two = p(kSlope) >= 0.5;
    if (!primed_) { cur_ = target; primed_ = true; }
    for (int i = 0; i < n; i += 4) {
      const int m = std::min(4, n - i);
      const double goal = std::max(-1.0, std::min(1.0, target + (mod ? static_cast<double>(mod[i]) : 0.0)));
      cur_ += (goal - cur_) * std::min(1.0, ss_ * 4);
      const double a = std::abs(cur_);
      const bool low = cur_ < 0;
      const double fc = low ? 20000 * js::pow(40.0 / 20000, a) : kLo * js::pow(kHi / kLo, a);
      const double wet = std::min(1.0, a / kFade), q = res * std::min(1.0, a * 3);
      const double g = js::tan(kPi * std::min(std::max(fc, 5.0), sr_ * 0.45) / sr_), k = 2 - 1.95 * q;
      const double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
      const double b1 = 1 / (1 + g * (g + 1.4)), b2 = g * b1, b3 = g * b2;
      for (int j = i; j < i + m; j++) {
        const double x = input[j];
        if (wet == 0) { out[j] = static_cast<float>(x); continue; }
        double v3 = x - st_[1];
        const double v1 = 2 * clip(0.5 * (a1 * st_[0] + a2 * v3));
        const double v2 = st_[1] + a2 * st_[0] + a3 * v3;
        st_[0] = flush(2 * v1 - st_[0]); st_[1] = flush(2 * v2 - st_[1]);
        double y = low ? v2 : x - k * v1 - v2;
        if (two) {
          v3 = y - st_[3];
          const double w1 = b1 * st_[2] + b2 * v3, w2 = st_[3] + b2 * st_[2] + b3 * v3;
          st_[2] = flush(2 * w1 - st_[2]); st_[3] = flush(2 * w2 - st_[3]);
          y = low ? w2 : y - 1.4 * w1 - w2;
        }
        out[j] = static_cast<float>(x + (y - x) * wet);
      }
    }
  }

private:
  double sr_;
  bool modOn_;
  double ss_;
  double st_[4] = {0, 0, 0, 0};
  double cur_ = 0;
  bool primed_ = false;
};

}  // namespace

void registerSweep(NodeRegistry& registry) {
  NodeType type;
  type.type = "sweep";
  type.inputs = {"in", "mod"};
  type.outputs = {"out"};
  type.dual = true;
  type.isEffect = true;
  type.kinds = {{"mod", PortKind::Control}};
  type.params = {{"sweep", -1, 1, 0}, {"resonance", 0, 1, 0.25}, {"slope", 0, 1, 0, true}};
  type.create = [](const NodeContext& context) { return std::make_unique<Sweep>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Exciter: high-passes the signal, saturates that band, high-passes the result again (to drop the low products and any DC), and
// adds it back: out = in + amount * wet. `even` biases the saturator, which adds even harmonics. Stereo in and out.
// Port of webtracker src/audio/nodes/exciter.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kFreq = 0, kDrive, kAmount, kEven };
constexpr double kK = 1.4142;   // damping: Butterworth

class Exciter final : public Node {
public:
  explicit Exciter(const NodeContext& context) : sampleRate_(context.sampleRate) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double target = p(kFreq);
    if (fc_ < 0) fc_ = target;
    fc_ += (target - fc_) * (1 - js::exp(-n / (sampleRate_ * 0.02)));
    const double g = js::tan(kPi * std::min(std::max(fc_, 20.0), sampleRate_ * 0.45) / sampleRate_);
    const double a1 = 1 / (1 + g * (g + kK)), a2 = g * a1, a3 = g * a2;
    const double drive = p(kDrive), amount = p(kAmount);
    const double bias = p(kEven) * 0.8, bias0 = js::tanh(bias);
    for (int c = 0; c < 2; c++) {
      const float* x = inputs[c];
      float* out = outputs[c];
      double* s = state_[c];
      for (int i = 0; i < n; i++) {
        const double xi = x[i];
        double v3 = xi - s[1];
        double v1 = a1 * s[0] + a2 * v3;
        const double v2 = s[1] + a2 * s[0] + a3 * v3;
        s[0] = 2 * v1 - s[0]; s[1] = 2 * v2 - s[1];
        const double band = xi - kK * v1 - v2;
        const double sat = js::tanh(band * drive + bias) - bias0;
        v3 = sat - s[3];
        v1 = a1 * s[2] + a2 * v3;
        const double w2 = s[3] + a2 * s[2] + a3 * v3;
        s[2] = 2 * v1 - s[2]; s[3] = 2 * w2 - s[3];
        out[i] = static_cast<float>(xi + amount * (sat - kK * v1 - w2));
      }
      for (int k = 0; k < 4; k++) if (std::abs(s[k]) < 1e-20) s[k] = 0;
    }
  }

private:
  double sampleRate_;
  double state_[2][4] = {};
  double fc_ = -1;
};

}  // namespace

void registerExciter(NodeRegistry& registry) {
  NodeType type;
  type.type = "exciter";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {{"freq", 500, 12000, 3000}, {"drive", 1, 20, 4}, {"amount", 0, 1, 0.3}, {"even", 0, 1, 0.3}};
  type.create = [](const NodeContext& context) { return std::make_unique<Exciter>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

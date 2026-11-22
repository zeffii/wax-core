// Haas widener: one side is played a few milliseconds after the other, a little duller and quieter. One delay line and a
// one-pole filter. Port of webtracker src/audio/nodes/haas.js.
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kTime = 0, kGain, kSide, kDamp, kMix };
constexpr double kMaxMs = 45;

class Haas final : public Node {
public:
  explicit Haas(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        size_(static_cast<int>(std::ceil(context.sampleRate * kMaxMs / 1000)) + 4),
        buf_(static_cast<std::size_t>(size_), 0.0f),
        slew_(1 - js::exp(-1 / (context.sampleRate * 0.05))) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inL = inputs[0];
    const float* inR = inputs[1];
    float* outL = outputs[0];
    float* outR = outputs[1];
    const double target = params[slots[kTime]] * sampleRate_ / 1000, gain = params[slots[kGain]], mix = params[slots[kMix]];
    const bool right = params[slots[kSide]] >= 0.5;
    const double c = 1 - js::exp(-2 * kPi * params[slots[kDamp]] / sampleRate_);
    if (d_ < 0) d_ = target;
    for (int i = 0; i < n; i++) {
      d_ += (target - d_) * slew_;
      const double x = right ? inR[i] : inL[i];
      buf_[static_cast<std::size_t>(w_)] = std::fabs(x) < 1e-20 ? 0.0f : static_cast<float>(x);
      double p = w_ - d_;
      if (p < 0) p += size_;
      const int j = static_cast<int>(p);
      const double f = p - j;
      const double a = buf_[static_cast<std::size_t>(j)];
      const double s = a + (buf_[static_cast<std::size_t>(j + 1 == size_ ? 0 : j + 1)] - a) * f;
      lp_ += (s - lp_) * c;
      const double late = x + (lp_ * gain - x) * mix;
      if (right) { outL[i] = inL[i]; outR[i] = static_cast<float>(late); }
      else { outL[i] = static_cast<float>(late); outR[i] = inR[i]; }
      if (++w_ == size_) w_ = 0;
    }
  }

private:
  double sampleRate_;
  int size_;
  std::vector<float> buf_;
  double slew_;
  int w_ = 0;
  double d_ = -1, lp_ = 0;
};

}  // namespace

void registerHaas(NodeRegistry& registry) {
  NodeType type;
  type.type = "haas";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {
    {"time", 0, kMaxMs, 14},          // ms
    {"gain", 0, 1.2, 0.9},
    {"side", 0, 1, 1, true},          // which side is late: 0 left, 1 right
    {"damp", 1000, 20000, 9000},      // Hz: the late side loses its top
    {"mix", 0, 1, 1},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Haas>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

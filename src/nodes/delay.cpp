// Mono feedback delay with a one-second maximum. Shared state, not per voice.
// Port of webtracker src/audio/nodes/delay.js and of delayBlock() in assembly/dsp.ts (the web app runs that kernel).
#include <algorithm>
#include <cmath>
#include <vector>

#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kTime = 0, kFeedback, kMix };

class Delay final : public Node {
public:
  explicit Delay(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        size_(static_cast<int>(std::ceil(context.sampleRate)) + 2),
        line_(static_cast<std::size_t>(size_), 0.0f),
        slew_(1 - std::exp(-1 / (context.sampleRate * 0.05))) {}   // per sample, so the glide does not depend on the block size

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* in = inputs[0];
    float* out = outputs[0];
    const double target = std::min(std::max(params[slots[kTime]] * 0.001 * sampleRate_, 1.0), static_cast<double>(size_ - 3));
    if (d_ < 0) d_ = target;
    const double fb = params[slots[kFeedback]], mix = params[slots[kMix]];
    for (int i = 0; i < n; i++) {
      d_ += (target - d_) * slew_;
      double r = w_ - d_;
      if (r < 0) r += size_;
      const int k = static_cast<int>(r);
      const double f = r - k;
      const int k1 = k + 1 == size_ ? 0 : k + 1;
      const double b0 = line_[static_cast<std::size_t>(k)], b1 = line_[static_cast<std::size_t>(k1)];
      const double y = b0 + (b1 - b0) * f;
      const double x = in[i];
      const double v = x + y * fb;
      line_[static_cast<std::size_t>(w_)] = std::abs(v) < 1e-20 ? 0.0f : static_cast<float>(v);   // no slow denormals in a long tail
      out[i] = static_cast<float>(x + y * mix);
      if (++w_ == size_) w_ = 0;
    }
  }

private:
  double sampleRate_;
  int size_;
  std::vector<float> line_;
  double slew_;
  int w_ = 0;
  double d_ = -1;   // the delay in samples, gliding to its target (negative until first use)
};

}  // namespace

void registerDelay(NodeRegistry& registry) {
  NodeType type;
  type.type = "delay";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.dual = true;   // runs as a left/right pair when a stereo signal arrives
  type.isEffect = true;
  type.params = {
    {"time", 1, 1000, 300},
    {"feedback", 0, 0.95, 0.35},
    {"mix", 0, 1, 0.25},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Delay>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Ensemble: the chorus of a string machine. The sound (mixed to mono) runs through three short delays whose lengths are moved
// by a slow and a fast wobble at once, each delay a third of a turn behind the last. Port of webtracker src/audio/nodes/ensemble.js.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kSlow = 0, kFast, kDepth, kShimmer, kWidth, kTone, kMix };
constexpr double kBaseMs = 6, kSlowMs = 1.6, kFastMs = 0.22, kMaxMs = 12;
constexpr int kLines = 3;

double delayMs(int line, double slowPhase, double fastPhase, double depth, double shimmer) {
  const double turn = 2 * kPi, behind = static_cast<double>(line) / kLines;
  return kBaseMs + depth * (kSlowMs * js::sin(turn * (slowPhase + behind)) + 2 * shimmer * kFastMs * js::sin(turn * (fastPhase + behind)));
}

class Ensemble final : public Node {
public:
  explicit Ensemble(const NodeContext& context)
      : sampleRate_(context.sampleRate), size_(static_cast<int>(std::ceil((context.sampleRate * kMaxMs) / 1000)) + 4), line_(size_, 0.0f) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double sr = sampleRate_;
    const double slowInc = g(kSlow) / sr, fastInc = g(kFast) / sr, depth = g(kDepth), shimmer = g(kShimmer);
    const double width = g(kWidth), mix = g(kMix), toneRate = 1 - js::exp((-2 * kPi * g(kTone)) / sr);
    const double near = (1 + width) / 2, far = (1 - width) / 2, toSamples = sr / 1000;
    for (int i = 0; i < n; i++) {
      const double inL = inputs[0][i], inR = inputs[1][i];
      const double dry = 0.5 * (inL + inR);
      line_[write_] = static_cast<float>(std::abs(dry) < 1e-20 ? 0 : dry);
      slowPhase_ += slowInc; if (slowPhase_ >= 1) slowPhase_ -= 1;
      fastPhase_ += fastInc; if (fastPhase_ >= 1) fastPhase_ -= 1;
      for (int k = 0; k < kLines; k++) {
        double at = write_ - delayMs(k, slowPhase_, fastPhase_, depth, shimmer) * toSamples;
        if (at < 0) at += size_;
        const int j = static_cast<int>(at);
        const double f = at - j;
        const int next = j + 1 == size_ ? 0 : j + 1;
        const double a = line_[j], b = line_[next];
        dull_[k] = static_cast<float>(static_cast<double>(dull_[k]) + (a + (b - a) * f - dull_[k]) * toneRate);
        heard_[k] = dull_[k];
      }
      const double wetL = (static_cast<double>(heard_[0]) * near + static_cast<double>(heard_[1]) * far + static_cast<double>(heard_[2]) * 0.7) * 0.62;
      const double wetR = (static_cast<double>(heard_[0]) * far + static_cast<double>(heard_[1]) * near + static_cast<double>(heard_[2]) * 0.7) * 0.62;
      outputs[0][i] = static_cast<float>(inL + (wetL - inL) * mix);
      outputs[1][i] = static_cast<float>(inR + (wetR - inR) * mix);
      if (++write_ == size_) write_ = 0;
    }
  }

private:
  double sampleRate_;
  int size_;
  std::vector<float> line_;
  float heard_[kLines] = {0, 0, 0}, dull_[kLines] = {0, 0, 0};
  int write_ = 0;
  double slowPhase_ = 0, fastPhase_ = 0;
};

}  // namespace

void registerEnsemble(NodeRegistry& registry) {
  NodeType type;
  type.type = "ensemble";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.dryAtZero = "mix";
  type.params = {{"slow", 0.05, 2, 0.6}, {"fast", 2, 10, 6}, {"depth", 0, 1, 0.6}, {"shimmer", 0, 1, 0.5},
                 {"width", 0, 1, 0.8}, {"tone", 1000, 16000, 8000}, {"mix", 0, 1, 1}};
  type.create = [](const NodeContext& context) { return std::make_unique<Ensemble>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

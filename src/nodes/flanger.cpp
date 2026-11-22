// Stereo flanger: a very short delay swept by an lfo and added to the dry signal, which makes a comb filter that moves
// up and down. `delay` is the middle of the sweep and `depth` how far it swings, both in ms; `feedback` sends the
// delayed signal back in (negative values invert it). The right channel's lfo is offset by `width` (a quarter cycle at
// 1). out = dry + wet * mix. Port of webtracker src/audio/nodes/flanger.js.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kRate = 0, kDepth, kDelay, kFeedback, kWidth, kMix, kPhase, kGlide };

constexpr double kMaxMs = 25;

class Flanger final : public Node {
public:
  explicit Flanger(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        clock_(context.clock),
        size_(static_cast<int>(std::ceil(context.sampleRate * kMaxMs / 1000)) + 4),
        ms_(context.sampleRate / 1000) {
    buf_[0].assign(static_cast<std::size_t>(size_), 0.0f);
    buf_[1].assign(static_cast<std::size_t>(size_), 0.0f);
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    if (clock_) {   // a write to `phase` puts the sweep at that point in its cycle, then it runs on
      const std::uint32_t stamp = clock_->stamps[static_cast<std::size_t>(slots[kPhase])];
      if (stamp != seen_) {
        const double target = jsMod(params[slots[kPhase]], 1), d = phase_ + err_ - target, g = params[slots[kGlide]];
        err_ = g > 0 ? d - jsRound(d) : 0;
        phase_ = target;
        decay_ = js::exp(-3 / (sampleRate_ * g / 1000 + 1));
      }
      seen_ = stamp;
    }
    const double inc = params[slots[kRate]] / sampleRate_, depth = params[slots[kDepth]], delay = params[slots[kDelay]];
    const double fb = params[slots[kFeedback]], offset = 0.25 * params[slots[kWidth]], mix = params[slots[kMix]];
    for (int i = 0; i < n; i++) {
      for (int c = 0; c < 2; c++) {
        const double lfo = js::sin(2 * kPi * (phase_ + err_ + (c ? offset : 0)));
        const double pos = std::max(1.0, std::min(static_cast<double>(size_ - 3), (delay + depth * lfo) * ms_));
        double r = w_ - pos;
        if (r < 0) r += size_;
        const int k = static_cast<int>(r), k1 = k + 1 == size_ ? 0 : k + 1;
        const double f = r - k;
        std::vector<float>& b = buf_[c];
        const double wet = b[static_cast<std::size_t>(k)] + (static_cast<double>(b[static_cast<std::size_t>(k1)]) - b[static_cast<std::size_t>(k)]) * f;
        const double dry = inputs[c][i];
        const double v = dry + fb * wet;
        b[static_cast<std::size_t>(w_)] = static_cast<float>(std::abs(v) < 1e-20 ? 0 : v);
        outputs[c][i] = static_cast<float>(dry + wet * mix);
      }
      if (++w_ == size_) w_ = 0;
      err_ = std::abs(err_) < 1e-7 ? 0 : err_ * decay_;
      phase_ += inc;
      if (phase_ >= 1) phase_ -= 1;
    }
  }

private:
  double sampleRate_;
  const Clock* clock_;
  int size_;
  double ms_;
  std::vector<float> buf_[2];
  int w_ = 0;
  double phase_ = 0, err_ = 0, decay_ = 0;
  std::uint32_t seen_ = 0;
};

}  // namespace

void registerFlanger(NodeRegistry& registry) {
  NodeType type;
  type.type = "flanger";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.dryAtZero = "mix";
  type.params = {
    {"rate", 0.02, 8, 0.25},            // Hz
    {"depth", 0, 8, 1.5},               // ms either side of the delay
    {"delay", 0.2, 10, 2.5},            // ms
    {"feedback", -0.95, 0.95, 0.5},
    {"width", 0, 1, 1},
    {"mix", 0, 1, 0.5},
    {"phase", 0, 1, 0},                 // where in its cycle the sweep restarts when a pattern writes this
    {"glide", 0, 500, 20},              // ms: how long the sweep takes to slide to a written phase
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Flanger>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

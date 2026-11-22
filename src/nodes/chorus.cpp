// Stereo chorus: up to four modulated taps per channel, spread around the lfo cycle so they drift against each other.
// The right channel's lfo is offset by `width` (a quarter cycle at 1), which is what makes it wide. out = dry + wet * mix.
// Port of webtracker src/audio/nodes/chorus.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kRate = 0, kDepth, kDelay, kVoices, kWidth, kMix, kPhase, kGlide };

constexpr double kMaxMs = 60;

class Chorus final : public Node {
public:
  explicit Chorus(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        clock_(context.clock),
        size_(static_cast<int>(std::ceil(context.sampleRate * kMaxMs / 1000)) + 4),
        bufLeft_(static_cast<std::size_t>(size_), 0.0f),
        bufRight_(static_cast<std::size_t>(size_), 0.0f),
        slew_(1 - js::exp(-1 / (context.sampleRate * 0.05))),   // per sample, so parameter moves do not click
        ms_(context.sampleRate / 1000) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    if (clock_) {   // a write to `phase` (a Phs column in the pattern) puts the sweep at that point in its cycle, then it runs on
      const std::uint32_t stamp = clock_->stamps[static_cast<std::size_t>(slots[kPhase])];
      if (stamp != seen_) {
        const double target = jsMod(params[slots[kPhase]], 1), d = phase_ + err_ - target, glide = params[slots[kGlide]];
        err_ = glide > 0 ? d - jsRound(d) : 0;
        phase_ = target;
        decay_ = js::exp(-3 / (sampleRate_ * glide / 1000 + 1));
      }
      seen_ = stamp;
    }
    const float* inLeft = inputs[0];
    const float* inRight = inputs[1];
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    const double inc = params[slots[kRate]] / sampleRate_;
    const int voices = std::max(1, jsRoundInt(params[slots[kVoices]]));
    const double depthTarget = params[slots[kDepth]], delayTarget = params[slots[kDelay]];
    const double offset = 0.25 * params[slots[kWidth]], mix = params[slots[kMix]];
    if (depth_ < 0) { depth_ = depthTarget; delay_ = delayTarget; }
    for (int i = 0; i < n; i++) {
      depth_ += (depthTarget - depth_) * slew_;
      delay_ += (delayTarget - delay_) * slew_;
      double wl = 0, wr = 0;
      for (int v = 0; v < voices; v++) {
        const double p = phase_ + err_ + static_cast<double>(v) / voices;
        wl += read(bufLeft_, (delay_ + depth_ * js::sin(kTwoPi * p)) * ms_);
        wr += read(bufRight_, (delay_ + depth_ * js::sin(kTwoPi * (p + offset))) * ms_);
      }
      const float l = inLeft[i], r = inRight[i];
      outLeft[i] = static_cast<float>(l + (wl / voices) * mix);
      outRight[i] = static_cast<float>(r + (wr / voices) * mix);
      bufLeft_[static_cast<std::size_t>(w_)] = std::abs(l) < 1e-20 ? 0.0f : l;
      bufRight_[static_cast<std::size_t>(w_)] = std::abs(r) < 1e-20 ? 0.0f : r;
      if (++w_ == size_) w_ = 0;
      err_ = std::abs(err_) < 1e-7 ? 0 : err_ * decay_;
      phase_ += inc;
      if (phase_ >= 1) phase_ -= 1;
    }
  }

private:
  // the delay line `pos` samples back from where it is being written, between two samples
  double read(const std::vector<float>& buf, double pos) const {
    double r = w_ - pos;
    if (r < 0) r += size_;
    const int k = static_cast<int>(r);
    const double f = r - k;
    const int k1 = k + 1 == size_ ? 0 : k + 1;
    const double a = buf[static_cast<std::size_t>(k)], b = buf[static_cast<std::size_t>(k1)];
    return a + (b - a) * f;
  }

  double sampleRate_;
  const Clock* clock_;
  int size_;
  std::vector<float> bufLeft_, bufRight_;
  double slew_, ms_;
  int w_ = 0;
  double phase_ = 0, depth_ = -1, delay_ = -1;
  double err_ = 0, decay_ = 0;   // the gap between the sweep and where it is heading, shrinking to nothing so a phase write slides
  std::uint32_t seen_ = 0;       // a fresh node starts at phase 0 anyway, so a write made before its first block still counts
};

}  // namespace

void registerChorus(NodeRegistry& registry) {
  NodeType type;
  type.type = "chorus";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.dryAtZero = "mix";
  type.params = {
    {"rate", 0.05, 8, 0.6},          // Hz
    {"depth", 0, 15, 4},             // ms
    {"delay", 5, 35, 15},            // ms
    {"voices", 1, 4, 3, true},
    {"width", 0, 1, 1},
    {"mix", 0, 1, 0.5},
    {"phase", 0, 1, 0},              // where in its cycle the sweep restarts when a pattern writes this (the sweep then runs on)
    {"glide", 0, 500, 20},           // ms: how long the sweep takes to slide to a written phase (0 jumps, which can click)
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Chorus>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

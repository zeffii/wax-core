// Shimmer reverb: the hall reverb with a pitch shifter in its feedback. What the reverb produces is shifted (by default
// an octave up) and fed back in, so every pass of the tail climbs. `shimmer` is how much comes back round, `pitch` the
// interval in semitones, `tone` rolls the highs off the fed-back sound. The reverb itself is the hall's. The loop runs
// a block behind. out = dry * (1 - mix) + wet * mix. Port of webtracker src/audio/nodes/shimmer.js.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kSize = 0, kDecay, kDamp, kPredelay, kPitch, kShimmer, kTone, kWidth, kMix };

constexpr double kWindowMs = 80;

class Shimmer final : public Node {
public:
  explicit Shimmer(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        w_size_(jsRoundInt(kWindowMs * 0.001 * context.sampleRate)),
        buf_(static_cast<std::size_t>(w_size_ + 2), 0.0f) {
    NodeContext inner;
    inner.sampleRate = context.sampleRate;
    core_ = makeHall(inner);
    for (int i = 0; i < 8; i++) innerSlots_[i] = i;
    resize(static_cast<std::size_t>(context.maxBlock));
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inL = inputs[0];
    const float* inR = inputs[1];
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (fb_.size() < static_cast<std::size_t>(n)) resize(static_cast<std::size_t>(n));
    auto g = [&](int i) -> double { return params[slots[i]]; };
    // the reverb alone, fully wet, with the shifted feedback from the last block added to its input
    inner_[0] = params[slots[kSize]]; inner_[1] = params[slots[kDecay]]; inner_[2] = params[slots[kDamp]]; inner_[3] = params[slots[kPredelay]];
    inner_[4] = 0.8f; inner_[5] = 0.4f; inner_[6] = params[slots[kWidth]]; inner_[7] = 1;
    for (int i = 0; i < n; i++) { mixL_[i] = inL[i] + fb_[i]; mixR_[i] = inR[i] + fb_[i]; }
    const float* hallIn[2] = {mixL_.data(), mixR_.data()};
    float* hallOut[2] = {wetL_.data(), wetR_.data()};
    core_->process(hallIn, hallOut, n, inner_, innerSlots_);
    // pitch shift the wet mono sound for the next block: four read points sliding through the buffer, crossfaded
    const double ratio = js::pow(2, g(kPitch) / 12), amount = g(kShimmer);
    const double dph = (1 - ratio) / w_size_;
    const double R = js::exp(-2 * kPi * 20 / sampleRate_), Ro = js::exp(-2 * kPi * 5 / sampleRate_);
    const double c = 1 - js::exp(-2 * kPi * (500 + 14000 * js::pow(g(kTone), 2)) / sampleRate_);
    const int length = static_cast<int>(buf_.size());
    for (int i = 0; i < n; i++) {
      // the reverb leaks a little DC on one side, and a shifter passes DC straight round the loop until it saturates,
      // so the wet sound loses it on the way out and the feedback again on the way in
      yl_ = wetL_[i] - xl_ + Ro * yl_; xl_ = wetL_[i]; wetL_[i] = static_cast<float>(yl_);
      yr_ = wetR_[i] - xr_ + Ro * yr_; xr_ = wetR_[i]; wetR_[i] = static_cast<float>(yr_);
      const double m = (static_cast<double>(wetL_[i]) + wetR_[i]) * 0.5;
      hy_ = m - hx_ + R * hy_; hx_ = m;
      buf_[static_cast<std::size_t>(w_)] = static_cast<float>(hy_);
      if (++w_ == length) w_ = 0;
      ph_ += dph;
      if (ph_ >= 1) ph_ -= 1; else if (ph_ < 0) ph_ += 1;
      double s = 0;   // four read points a quarter of a window apart, so no one fade ever leaves a hole
      for (int k = 0; k < 4; k++) {
        const double q = ph_ + 0.25 * k - (ph_ + 0.25 * k >= 1 ? 1 : 0);
        s += read(1 + q * w_size_) * (0.5 - 0.5 * js::cos(2 * kPi * q));
      }
      s *= 0.5;
      lp_ += c * (s - lp_);
      fb_[i] = static_cast<float>(softClip(lp_ * 1.5) * amount * 0.9);   // soft clip: the loop can never run away
    }
    const double mix = g(kMix);
    for (int i = 0; i < n; i++) {
      outL[i] = static_cast<float>(inL[i] + (wetL_[i] - static_cast<double>(inL[i])) * mix);
      outR[i] = static_cast<float>(inR[i] + (wetR_[i] - static_cast<double>(inR[i])) * mix);
    }
  }

private:
  void resize(std::size_t n) {
    fb_.assign(n, 0.0f); mixL_.assign(n, 0.0f); mixR_.assign(n, 0.0f); wetL_.assign(n, 0.0f); wetR_.assign(n, 0.0f);
  }

  double read(double d) const {   // d samples back, linear
    double r = w_ - d;
    const int length = static_cast<int>(buf_.size());
    if (r < 0) r += length;
    const int k = static_cast<int>(r), k1 = k + 1 == length ? 0 : k + 1;
    const double f = r - k;
    return static_cast<double>(buf_[static_cast<std::size_t>(k)]) +
           (static_cast<double>(buf_[static_cast<std::size_t>(k1)]) - buf_[static_cast<std::size_t>(k)]) * f;
  }

  double sampleRate_;
  int w_size_;
  std::vector<float> buf_;
  std::unique_ptr<Node> core_;
  float inner_[8] = {};
  int innerSlots_[8] = {};
  int w_ = 0;
  double ph_ = 0, lp_ = 0, hx_ = 0, hy_ = 0, xl_ = 0, yl_ = 0, xr_ = 0, yr_ = 0;
  std::vector<float> fb_, mixL_, mixR_, wetL_, wetR_;
};

}  // namespace

void registerShimmer(NodeRegistry& registry) {
  NodeType type;
  type.type = "shimmer";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.isEffect = true;
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.params = {
    {"size", 0.5, 2, 1.2},
    {"decay", 0.2, 20, 5},
    {"damp", 0, 1, 0.35},
    {"predelay", 0, 200, 20},
    {"pitch", -12, 24, 12},        // semitones
    {"shimmer", 0, 0.95, 0.4},     // how much of the shifted tail comes back
    {"tone", 0, 1, 0.6},           // brightness of the fed-back sound
    {"width", 0, 1, 1},
    {"mix", 0, 1, 0.4},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Shimmer>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

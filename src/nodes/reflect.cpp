// Early reflections: the first echoes a room gives a sound, from the box of a room you set (image-source method, see
// spacecore). Each mirror image of the source is a tap of one delay line, with the delay and level of the way it travelled
// and the side it comes from. Up to 32 taps. Port of webtracker src/audio/nodes/reflect.js.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"
#include "nodes/spacecore.h"

namespace wax {
namespace {

enum Param { kWidth = 0, kDepth, kHeight, kSx, kSy, kLx, kLy, kAbsorb, kTaps, kDamp, kMix };
constexpr double kMaxSeconds = 0.6;
constexpr int kMaxTaps = space::kMaxTaps;

class Reflect final : public Node {
public:
  explicit Reflect(const NodeContext& context)
      : sampleRate_(context.sampleRate), size_(static_cast<int>(std::ceil(context.sampleRate * kMaxSeconds)) + 8), buf_(size_, 0.0f),
        slew_(1 - js::exp(-1 / (context.sampleRate * 0.03))) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    float key[9];
    for (int i = 0; i < 9; i++) key[i] = params[slots[i]];
    if (!haveKey_ || !std::equal(key, key + 9, key_)) {
      haveKey_ = true;
      std::copy(key, key + 9, key_);
      const space::Room room = space::reflections(p(kWidth), p(kDepth), p(kHeight), p(kSx), p(kSy), p(kLx), p(kLy), p(kAbsorb), p(kTaps));
      live_ = 0;
      for (const space::Tap& t : room.list) {
        const double rel = (t.delay - room.direct) * sampleRate_;
        if (rel < 1 || rel > size_ - 4) continue;
        const double a = (t.pan + 1) * kPi / 4;
        tDelay_[live_] = static_cast<float>(rel);
        tl_[live_] = static_cast<float>(js::cos(a) * t.gain);
        tr_[live_] = static_cast<float>(js::sin(a) * t.gain);
        live_++;
      }
      if (fresh_) {
        std::copy(tDelay_, tDelay_ + kMaxTaps, delay_);
        std::copy(tl_, tl_ + kMaxTaps, gl_);
        std::copy(tr_, tr_ + kMaxTaps, gr_);
        fresh_ = false;
      }
      for (int i = live_; i < kMaxTaps; i++) { tl_[i] = 0; tr_[i] = 0; }
    }
    const double mix = p(kMix), lp = 1 - js::exp(-2 * kPi * p(kDamp) / sampleRate_);
    for (int i = 0; i < n; i++) {
      const double inL = inputs[0][i], inR = inputs[1][i];
      const double x = (inL + inR) * 0.5;
      buf_[w_] = static_cast<float>(std::abs(x) < 1e-20 ? 0 : x);
      double l = 0, r = 0;
      for (int t = 0; t < kMaxTaps; t++) {
        delay_[t] = static_cast<float>(static_cast<double>(delay_[t]) + (static_cast<double>(tDelay_[t]) - delay_[t]) * slew_);
        gl_[t] = static_cast<float>(static_cast<double>(gl_[t]) + (static_cast<double>(tl_[t]) - gl_[t]) * slew_);
        gr_[t] = static_cast<float>(static_cast<double>(gr_[t]) + (static_cast<double>(tr_[t]) - gr_[t]) * slew_);
        if (gl_[t] < 1e-5 && gr_[t] < 1e-5 && tl_[t] == 0 && tr_[t] == 0) continue;
        double rp = w_ - static_cast<double>(delay_[t]);
        if (rp < 0) rp += size_;
        const int j = static_cast<int>(rp);
        const double f = rp - j;
        const double a = buf_[j], b = buf_[j + 1 == size_ ? 0 : j + 1];
        const double s = a + (b - a) * f;
        l += s * gl_[t];
        r += s * gr_[t];
      }
      lpL_ += (l - lpL_) * lp;
      lpR_ += (r - lpR_) * lp;
      outputs[0][i] = static_cast<float>(inL + lpL_ * mix);
      outputs[1][i] = static_cast<float>(inR + lpR_ * mix);
      if (++w_ == size_) w_ = 0;
    }
  }

private:
  double sampleRate_;
  int size_;
  std::vector<float> buf_;
  double slew_;
  int w_ = 0, live_ = 0;
  bool haveKey_ = false, fresh_ = true;
  float key_[9] = {};
  float delay_[kMaxTaps] = {}, gl_[kMaxTaps] = {}, gr_[kMaxTaps] = {};
  float tDelay_[kMaxTaps] = {}, tl_[kMaxTaps] = {}, tr_[kMaxTaps] = {};
  double lpL_ = 0, lpR_ = 0;
};

}  // namespace

void registerReflect(NodeRegistry& registry) {
  NodeType type;
  type.type = "reflect";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.dryAtZero = "mix";
  type.params = {
    {"width", 2, 60, 8}, {"depth", 2, 60, 11}, {"height", 2, 20, 4}, {"srcx", 0, 1, 0.3}, {"srcy", 0, 1, 0.75},
    {"lisx", 0, 1, 0.55}, {"lisy", 0, 1, 0.2}, {"absorb", 0, 0.95, 0.3}, {"taps", 1, kMaxTaps, 16, true},
    {"damp", 1000, 20000, 9000}, {"mix", 0, 1.5, 0.6},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Reflect>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

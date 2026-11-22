// Shiva delay: a stereo delay with up to 16 taps you place yourself. Each tap has a time (a fraction of `length`), a
// level and a pan, stored on the node as `data.taps`, a flat list [time, level, pan, time, level, pan, ...]. The input
// is summed to mono into one line. The latest tap feeds back (through a lowpass, `damp`). Dry stays stereo.
// Port of webtracker src/audio/nodes/shiva.js.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kLength = 0, kFeedback, kDamp, kMix };

constexpr int kMaxTaps = 16;
constexpr double kMaxLengthMs = 2000;

std::vector<double> defaultTaps() {
  std::vector<double> taps;
  for (int i = 0; i < 6; i++) {
    taps.push_back((i + 1) / 6.0);
    taps.push_back(jsRound(js::pow(0.7, i) * 1000) / 1000);
    taps.push_back(i % 2 ? 0.6 : -0.6);
  }
  return taps;
}

// Any stored list becomes whole, clamped triples, in the order given (at least one tap, at most kMaxTaps).
std::vector<double> normalizeTaps(const Json* data) {
  std::vector<double> points;
  bool ok = data && data->contains("taps") && (*data)["taps"].is_array() && (*data)["taps"].size() >= 3;
  if (ok) {
    for (const auto& v : (*data)["taps"]) {
      if (!v.is_number()) { ok = false; break; }
      points.push_back(v.get<double>());
    }
  }
  if (!ok) return defaultTaps();
  std::vector<double> out;
  for (std::size_t i = 0; i + 2 < points.size() && out.size() < kMaxTaps * 3; i += 3) {
    out.push_back(std::min(1.0, std::max(0.001, points[i])));
    out.push_back(std::min(1.0, std::max(0.0, points[i + 1])));
    out.push_back(std::min(1.0, std::max(-1.0, points[i + 2])));
  }
  return out;
}

class Shiva final : public Node {
public:
  explicit Shiva(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        size_(static_cast<int>(std::ceil(context.sampleRate * kMaxLengthMs / 1000)) + 8),
        buf_(static_cast<std::size_t>(size_), 0.0f),
        slew_(1 - js::exp(-1 / (context.sampleRate * 0.03))) {
    const std::vector<double> t = normalizeTaps(context.data);
    count_ = static_cast<int>(t.size() / 3);
    for (int k = 0; k < count_; k++) {
      tt_[k] = t[k * 3];
      const double a = (t[k * 3 + 2] + 1) * kPi / 4;
      tl_[k] = t[k * 3 + 1] * kSqrt2 * js::cos(a);
      tr_[k] = t[k * 3 + 1] * kSqrt2 * js::sin(a);
      if (tt_[k] > tt_[latest_]) latest_ = k;
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inL = inputs[0];
    const float* inR = inputs[1];
    float* outL = outputs[0];
    float* outR = outputs[1];
    const double target = std::min(std::max(static_cast<double>(params[slots[kLength]]), 1.0), kMaxLengthMs) * 0.001 * sampleRate_;
    const double fb = params[slots[kFeedback]], damp = params[slots[kDamp]], mix = params[slots[kMix]];
    const double a = 1 - damp * 0.95;   // one-pole lowpass coefficient
    if (len_ < 0) len_ = target;
    if (fresh_) {
      for (int k = 0; k < kMaxTaps; k++) { dly_[k] = tt_[k] * target; gl_[k] = tl_[k]; gr_[k] = tr_[k]; }
      fresh_ = false;
    }
    for (int i = 0; i < n; i++) {
      len_ += (target - len_) * slew_;
      double wl = 0, wr = 0, last = 0;
      for (int k = 0; k < kMaxTaps; k++) {
        gl_[k] += (tl_[k] - gl_[k]) * slew_;
        gr_[k] += (tr_[k] - gr_[k]) * slew_;
        dly_[k] += (tt_[k] * len_ - dly_[k]) * slew_;
        if (gl_[k] < 1e-6 && gr_[k] < 1e-6 && k >= count_) continue;
        double r = w_ - std::max(dly_[k], 1.0);
        if (r < 0) r += size_;
        const int j = static_cast<int>(r), j1 = j + 1 == size_ ? 0 : j + 1;
        const double f = r - j;
        const double v = static_cast<double>(buf_[static_cast<std::size_t>(j)]) +
                         (static_cast<double>(buf_[static_cast<std::size_t>(j1)]) - buf_[static_cast<std::size_t>(j)]) * f;
        wl += v * gl_[k];
        wr += v * gr_[k];
        if (k == latest_) last = v;
      }
      const double l = inL[i], r = inR[i];
      outL[i] = static_cast<float>(l + wl * mix);
      outR[i] = static_cast<float>(r + wr * mix);
      lp_ += (last - lp_) * a;
      const double v = (l + r) * 0.5 + lp_ * fb;
      buf_[static_cast<std::size_t>(w_)] = static_cast<float>(std::abs(v) < 1e-20 ? 0 : v);   // no slow denormals in a long tail
      if (++w_ == size_) w_ = 0;
    }
  }

private:
  double sampleRate_;
  int size_;
  std::vector<float> buf_;
  double slew_;
  int w_ = 0;
  double lp_ = 0;
  double dly_[kMaxTaps] = {}, gl_[kMaxTaps] = {}, gr_[kMaxTaps] = {};
  double tt_[kMaxTaps] = {}, tl_[kMaxTaps] = {}, tr_[kMaxTaps] = {};
  int count_ = 0, latest_ = 0;
  bool fresh_ = true;
  double len_ = -1;
};

}  // namespace

void registerShiva(NodeRegistry& registry) {
  NodeType type;
  type.type = "shiva";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.isEffect = true;
  type.dryAtZero = "mix";
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.params = {
    {"length", 20, kMaxLengthMs, 600},   // ms: where a tap at the far right of the view sits
    {"feedback", 0, 0.95, 0.3},
    {"damp", 0, 1, 0.3},
    {"mix", 0, 1, 0.4},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Shiva>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

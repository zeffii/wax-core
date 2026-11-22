// Comb filter: the signal is mixed with a copy of itself delayed by one period of `freq`; `feedback` makes the peaks ring,
// `ff` mixes in the delayed input once, `damp` darkens the feedback path, `spread` detunes the right channel in cents.
// out = dry + (comb - dry) * mix. Port of webtracker src/audio/nodes/comb.js.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kFreq = 0, kFeedback, kFf, kDamp, kSpread, kMix };
constexpr double kMinHz = 20;

class Comb final : public Node {
public:
  explicit Comb(const NodeContext& context)
      : sampleRate_(context.sampleRate), size_(static_cast<int>(std::ceil(context.sampleRate / kMinHz)) + 8),
        glide_(1 - js::exp(-1 / (context.sampleRate * 0.004))) {
    for (int c = 0; c < 2; c++) {
      xb_[c].assign(static_cast<std::size_t>(size_), 0.0f);
      yb_[c].assign(static_cast<std::size_t>(size_), 0.0f);
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double freq = p(kFreq), fb = p(kFeedback), ff = p(kFf), damp = p(kDamp), mix = p(kMix);
    const double cents = p(kSpread), norm = 1 / (1 + 0.8 * std::abs(fb));
    double target[2] = {sampleRate_ / (freq * js::pow(2, cents / 1200)), sampleRate_ / (freq * js::pow(2, -cents / 1200))};
    for (double& d : target) d = std::max(2.0, std::min(static_cast<double>(size_ - 3), d));
    if (!primed_) { len_[0] = target[0]; len_[1] = target[1]; primed_ = true; }
    const double a = 1 - 0.95 * damp;
    for (int i = 0; i < n; i++) {
      for (int c = 0; c < 2; c++) {
        std::vector<float>& xb = xb_[c];
        std::vector<float>& yb = yb_[c];
        len_[c] += (target[c] - len_[c]) * glide_;
        double r = w_ - len_[c];
        if (r < 0) r += size_;
        const int k = static_cast<int>(r), k1 = k + 1 == size_ ? 0 : k + 1;
        const double f = r - k;
        const double xk = xb[static_cast<std::size_t>(k)], xk1 = xb[static_cast<std::size_t>(k1)];
        const double yk = yb[static_cast<std::size_t>(k)], yk1 = yb[static_cast<std::size_t>(k1)];
        const double xd = xk + (xk1 - xk) * f, yd = yk + (yk1 - yk) * f;
        lp_[c] += a * (yd - lp_[c]);
        if (std::abs(lp_[c]) < 1e-20) lp_[c] = 0;
        const double x = inputs[c][i];
        const double y = (x + ff * xd + fb * lp_[c]);
        xb[static_cast<std::size_t>(w_)] = static_cast<float>(x);
        yb[static_cast<std::size_t>(w_)] = static_cast<float>(std::abs(y) < 1e-20 ? 0 : y);
        outputs[c][i] = static_cast<float>(x + (y * norm - x) * mix);
      }
      if (++w_ == size_) w_ = 0;
    }
    if (!std::isfinite(lp_[0] + lp_[1])) {
      lp_[0] = lp_[1] = 0;
      for (int c = 0; c < 2; c++) {
        std::fill(xb_[c].begin(), xb_[c].end(), 0.0f);
        std::fill(yb_[c].begin(), yb_[c].end(), 0.0f);
      }
    }
  }

private:
  double sampleRate_;
  int size_;
  double glide_;
  std::vector<float> xb_[2], yb_[2];
  double lp_[2] = {0, 0}, len_[2] = {0, 0};
  int w_ = 0;
  bool primed_ = false;
};

}  // namespace

void registerComb(NodeRegistry& registry) {
  NodeType type;
  type.type = "comb";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.dryAtZero = "mix";
  type.params = {
    {"freq", kMinHz, 8000, 220},
    {"feedback", -0.995, 0.995, 0.85},
    {"ff", -1, 1, 0},
    {"damp", 0, 1, 0.3},
    {"spread", 0, 100, 0},
    {"mix", 0, 1, 1},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Comb>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

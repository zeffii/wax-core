// Parametric equaliser, stereo: a low cut, a low shelf, three peaking bands, a high shelf and a high cut (RBJ biquads; the cuts
// are second-order Butterworth). A band with 0 dB gain is skipped, the low cut is off at 20 Hz and the high cut at 20 kHz.
// Port of webtracker src/audio/nodes/eq.js (without the response drawing).
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

constexpr int kBands = 5, kStages = kBands + 2;
enum Kind { kLowShelf, kPeak, kHighShelf, kHighpass, kLowpass };
const Kind kKinds[kBands] = {kLowShelf, kPeak, kPeak, kPeak, kHighShelf};
const double kFreqs[kBands] = {100, 350, 1200, 3500, 9000};
constexpr double kSqrt1_2 = 0.7071067811865476;

struct Coef { double b0, b1, b2, a1, a2; };

Coef biquad(Kind kind, double f, double gainDb, double q, double sr) {
  const double w = 2 * kPi * std::min(std::max(f, 5.0), sr * 0.45) / sr, cw = js::cos(w), sw = js::sin(w);
  const double A = js::pow(10, gainDb / 40), alpha = sw / (2 * q);
  double b0, b1, b2, a0, a1, a2;
  if (kind == kPeak) {
    b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
  } else if (kind == kLowShelf || kind == kHighShelf) {
    const double t = 2 * std::sqrt(A) * alpha, s = kind == kLowShelf ? 1 : -1;
    b0 = A * ((A + 1) - s * (A - 1) * cw + t);
    b1 = s * 2 * A * ((A - 1) - s * (A + 1) * cw);
    b2 = A * ((A + 1) - s * (A - 1) * cw - t);
    a0 = (A + 1) + s * (A - 1) * cw + t;
    a1 = -s * 2 * ((A - 1) + s * (A + 1) * cw);
    a2 = (A + 1) + s * (A - 1) * cw - t;
  } else if (kind == kHighpass) {
    b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
  } else {
    b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
  }
  return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

class Eq final : public Node {
public:
  explicit Eq(const NodeContext& context) : sampleRate_(context.sampleRate) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto value = [&](int i) { return static_cast<double>(params[slots[i]]); };
    struct Stage { int slot; Coef coef; };
    std::vector<Stage> stages;
    const double lowcut = value(0), highcut = value(1);
    if (lowcut > 20.5) stages.push_back({0, biquad(kHighpass, lowcut, 0, kSqrt1_2, sampleRate_)});
    for (int k = 1; k <= kBands; k++) {
      const int at = 2 + (k - 1) * 3;
      const double gain = value(at + 1);
      if (std::abs(gain) < 0.01) continue;
      stages.push_back({k, biquad(kKinds[k - 1], value(at), gain, value(at + 2), sampleRate_)});
    }
    if (highcut < 19990) stages.push_back({kBands + 1, biquad(kLowpass, highcut, 0, kSqrt1_2, sampleRate_)});
    for (int ch = 0; ch < 2; ch++) {
      const float* x = inputs[ch];
      float* y = outputs[ch];
      if (x != y) std::copy(x, x + n, y);
      for (int si = 0; si < kStages; si++) {
        bool used = false;
        for (const Stage& s : stages) if (s.slot == si) used = true;
        if (!used) { z1_[ch][si] = 0; z2_[ch][si] = 0; }
      }
      for (const Stage& s : stages) {
        const int si = s.slot;
        const Coef& c = s.coef;
        double s1 = z1_[ch][si], s2 = z2_[ch][si];
        for (int i = 0; i < n; i++) {
          const double v = y[i], o = c.b0 * v + s1;
          s1 = c.b1 * v - c.a1 * o + s2;
          s2 = c.b2 * v - c.a2 * o;
          y[i] = static_cast<float>(o);
        }
        z1_[ch][si] = std::abs(s1) < 1e-20 ? 0 : s1;
        z2_[ch][si] = std::abs(s2) < 1e-20 ? 0 : s2;
      }
    }
  }

private:
  double sampleRate_;
  double z1_[2][kStages] = {};
  double z2_[2][kStages] = {};
};

}  // namespace

void registerEq(NodeRegistry& registry) {
  NodeType type;
  type.type = "eq";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {{"lowcut", 20, 1000, 20}, {"highcut", 1000, 20000, 20000}};
  for (int k = 1; k <= kBands; k++) {
    const std::string n = std::to_string(k);
    type.params.push_back({"f" + n, 20, 20000, kFreqs[k - 1]});
    type.params.push_back({"g" + n, -18, 18, 0});
    type.params.push_back({"q" + n, 0.2, 10, k == 1 || k == kBands ? 0.71 : 1});
  }
  type.create = [](const NodeContext& context) { return std::make_unique<Eq>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Waveshaper: y = curve(x * drive), the curve a table of 129 points (data.curve, soft clipping by default), read with linear
// interpolation; inputs beyond +-1 are clipped to the ends. `oversample` runs it at 1x, 2x or 4x; the dry signal is delayed to
// match. out = (dry * (1 - mix) + wet * mix) * level. Port of webtracker src/audio/nodes/waveshaper.js and curves.js.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/oversample.h"

namespace wax {
namespace {

enum Param { kDrive = 0, kMix, kOversample, kLevel };
constexpr int kCurvePoints = 129;
constexpr int kFactors[3] = {1, 2, 4};

std::vector<float> softCurve() {
  std::vector<float> out(kCurvePoints);
  for (int i = 0; i < kCurvePoints; i++) {
    const double x = (2.0 * i) / (kCurvePoints - 1) - 1;
    const double y = js::tanh(2 * x) / js::tanh(2);
    out[static_cast<std::size_t>(i)] = static_cast<float>(jsRound(std::min(1.0, std::max(-1.0, y)) * 10000) / 10000);
  }
  return out;
}

// Resamples a stored curve of any length to kCurvePoints values, or falls back to soft clipping.
std::vector<float> normalizeCurve(const Json* data) {
  if (!data || !data->contains("curve") || !(*data)["curve"].is_array()) return softCurve();
  std::vector<double> points;
  for (const auto& v : (*data)["curve"]) {
    if (!v.is_number()) return softCurve();
    points.push_back(v.get<double>());
  }
  const int n = static_cast<int>(points.size());
  if (n < 2) return softCurve();
  std::vector<float> out(kCurvePoints);
  for (int i = 0; i < kCurvePoints; i++) {
    const double p = (static_cast<double>(i) / (kCurvePoints - 1)) * (n - 1), kf = std::floor(p), f = p - kf;
    const int k = static_cast<int>(kf);
    const double a = points[static_cast<std::size_t>(k)], b = points[static_cast<std::size_t>(std::min(k + 1, n - 1))];
    out[static_cast<std::size_t>(i)] = static_cast<float>(std::min(1.0, std::max(-1.0, a + (b - a) * f)));
  }
  return out;
}

class WaveShaper final : public Node {
public:
  explicit WaveShaper(const NodeContext& context)
      : table_(normalizeCurve(context.data)),
        chains_{{Oversampler(1), Oversampler(1)}, {Oversampler(2), Oversampler(2)}, {Oversampler(4), Oversampler(4)}},
        delays_{FixedDelay(48), FixedDelay(48)} {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const double drive = params[slots[kDrive]], mix = params[slots[kMix]], level = params[slots[kLevel]];
    const int mode = std::min(2, std::max(0, jsRoundInt(params[slots[kOversample]])));
    for (int c = 0; c < 2; c++) {
      Oversampler& chain = chains_[mode][c];
      const int wait = chain.latency();
      for (int i = 0; i < n; i++) {
        const double wet = chain.run(inputs[c][i], [&](double v) { return shape(v * drive); });
        const double dry = delays_[c].push(inputs[c][i], wait);
        outputs[c][i] = static_cast<float>((dry + (wet - dry) * mix) * level);
      }
    }
  }

private:
  double shape(double x) const {
    const double p = (std::min(std::max(x, -1.0), 1.0) + 1) * 0.5 * (kCurvePoints - 1);
    const int k = static_cast<int>(p), k1 = k + 1 < kCurvePoints ? k + 1 : k;
    const double f = p - k;
    const double a = table_[static_cast<std::size_t>(k)], b = table_[static_cast<std::size_t>(k1)];
    return a + (b - a) * f;
  }

  std::vector<float> table_;
  Oversampler chains_[3][2];
  FixedDelay delays_[2];
};

}  // namespace

void registerWaveShaper(NodeRegistry& registry) {
  NodeType type;
  type.type = "waveshaper";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.latency = [](const std::map<std::string, double>& settings, double) {
    return static_cast<double>(oversampleLatency(kFactors[std::min(2, std::max(0, jsRoundInt(settings.at("oversample"))))]));
  };
  type.params = {{"drive", 0.1, 24, 1}, {"mix", 0, 1, 1}, {"oversample", 0, 2, 1, true}, {"level", 0, 2, 1}};
  type.create = [](const NodeContext& context) { return std::make_unique<WaveShaper>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

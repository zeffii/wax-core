// Saturator, stereo: warmth and thickness from a curve. `drive` (dB, level kept about the same), `type` (soft, tape, tube, clip),
// `bias`, `bass` (everything below it stays out of the curve), `tone` (a lowpass after it), `mix`, `out`. `quality`: standard
// runs the curve at 2x oversampling, eco at 1x, hq at 4x. Port of webtracker src/audio/nodes/saturator.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/oversample.h"
#include "nodes/saturation.h"

namespace wax {
namespace {

enum Param { kDrive = 0, kType, kBias, kBass, kTone, kMix, kOut, kQuality };

class Saturator final : public Node {
public:
  explicit Saturator(const NodeContext& context)
      : sampleRate_(context.sampleRate), hpK_(saturation::offsetK(context.sampleRate)),
        dryDelay_{FixedDelay(48), FixedDelay(48)}, lowDelay_{FixedDelay(48), FixedDelay(48)} {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double quality = p(kQuality);
    const saturation::Settings s = saturation::settings(p(kType), p(kDrive), p(kBias));
    const double bass = p(kBass);
    const bool split = bass > 20.5;
    const double bassK = 1 - js::exp((-2 * kPi * std::min(bass, sampleRate_ * 0.45)) / sampleRate_);
    const double toneHz = p(kTone);
    const bool toneOn = toneHz < 19900;
    const double toneK = 1 - js::exp((-2 * kPi * std::min(toneHz, sampleRate_ * 0.45)) / sampleRate_);
    const double mix = p(kMix), out = saturation::dbToGain(p(kOut));
    for (int c = 0; c < 2; c++) {
      const float* x = inputs[c];
      float* y = outputs[c];
      saturation::Stage& stage = stages_[c];
      const int wait = stage.latency(quality);
      for (int i = 0; i < n; i++) {
        const double xi = x[i];
        double low = 0, high = xi;
        if (split) { lowpass_[c] += (xi - lowpass_[c]) * bassK; low = lowpass_[c]; high = xi - low; }
        double wet = stage.run(high, s, quality, hpK_) + (split ? lowDelay_[c].push(low, wait) : 0);
        if (toneOn) { tone_[c] += (wet - tone_[c]) * toneK; wet = tone_[c]; } else tone_[c] = wet;
        const double dry = dryDelay_[c].push(xi, wait);
        y[i] = static_cast<float>((dry + (wet - dry) * mix) * out);
      }
    }
  }

private:
  double sampleRate_, hpK_;
  saturation::Stage stages_[2];
  FixedDelay dryDelay_[2], lowDelay_[2];
  double lowpass_[2] = {0, 0}, tone_[2] = {0, 0};
};

}  // namespace

void registerSaturator(NodeRegistry& registry) {
  NodeType type;
  type.type = "saturator";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.latency = [](const std::map<std::string, double>& settings, double) {
    return static_cast<double>(oversampleLatency(saturation::kFactors[saturation::qualityOf(settings.at("quality"))]));
  };
  type.params = {
    {"drive", 0, 36, 12},
    {"type", 0, 3, 0, true},
    {"bias", -1, 1, 0},
    {"bass", 20, 500, 20},
    {"tone", 1000, 20000, 20000},
    {"mix", 0, 1, 1},
    {"out", -24, 12, 0},
    {"quality", 0, 2, 0, true},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Saturator>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Multiband saturator: four bands (as `split`), each saturated on its own with its own drive and gain, added together again.
// `type`, `bias` and `quality` are shared; `solo` hears one band alone. Port of webtracker src/audio/nodes/mbsat.js.
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/oversample.h"
#include "nodes/saturation.h"
#include "nodes/splitcore.h"

namespace wax {
namespace {

constexpr int kShared = 9;   // f1 f2 f3 type bias solo mix out quality, then drive and gain for each band
enum Param { kF1 = 0, kF2, kF3, kType, kBias, kSolo, kMix, kOut, kQuality };
constexpr double kDefaultDrive[4] = {6, 8, 10, 12};
const char* const kBands[4] = {"low", "lowmid", "highmid", "high"};

class MbSat final : public Node {
public:
  explicit MbSat(const NodeContext& context)
      : sampleRate_(context.sampleRate), hpK_(saturation::offsetK(context.sampleRate)), dryDelay_{FixedDelay(48), FixedDelay(48)} {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    double g[3];
    cross_.update(p(kF1), p(kF2), p(kF3), n, sampleRate_, g);
    for (int k = 0; k < 8; k++) if (static_cast<int>(band_[k].size()) < n) band_[k].resize(n);
    for (int c = 0; c < 2; c++) {
      splitcore::Channel& channel = split_[c];
      channel.set(g[0], g[1], g[2]);
      float* const bands[4] = {band_[c * 4].data(), band_[c * 4 + 1].data(), band_[c * 4 + 2].data(), band_[c * 4 + 3].data()};
      for (int i = 0; i < n; i++) channel.run(inputs[c][i], bands, i);
      channel.flush();
    }
    const double type = p(kType), bias = p(kBias), mix = p(kMix), out = saturation::dbToGain(p(kOut));
    const int solo = jsRoundInt(p(kSolo));
    const double quality = p(kQuality);
    const int wait = stages_[0][0].latency(quality);
    for (int c = 0; c < 2; c++) sums_[c].assign(n, 0.0);
    for (int b = 0; b < 4; b++) {
      const int o = kShared + b * 2;
      const saturation::Settings s = saturation::settings(type, p(o), bias);
      const double gain = saturation::dbToGain(p(o + 1));
      const bool active = solo == 0 || solo == b + 1;
      for (int c = 0; c < 2; c++) {
        const float* band = band_[c * 4 + b].data();
        saturation::Stage& stage = stages_[c][b];
        std::vector<double>& sum = sums_[c];
        for (int i = 0; i < n; i++) {
          const double wet = stage.run(band[i], s, quality, hpK_);
          if (active) sum[i] += wet * gain;
        }
      }
    }
    for (int c = 0; c < 2; c++) {
      for (int i = 0; i < n; i++) {
        const double dry = dryDelay_[c].push(inputs[c][i], wait);
        outputs[c][i] = static_cast<float>((dry + (sums_[c][i] - dry) * mix) * out);
      }
    }
  }

private:
  double sampleRate_, hpK_;
  splitcore::Channel split_[2];
  splitcore::Crossovers cross_;
  std::vector<float> band_[8];
  saturation::Stage stages_[2][4];
  FixedDelay dryDelay_[2];
  std::vector<double> sums_[2];
};

}  // namespace

void registerMbSat(NodeRegistry& registry) {
  NodeType type;
  type.type = "mbsat";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.latency = [](const std::map<std::string, double>& settings, double) {
    return static_cast<double>(oversampleLatency(saturation::kFactors[saturation::qualityOf(settings.at("quality"))]));
  };
  type.params = {{"f1", 30, 2000, 120}, {"f2", 100, 8000, 800}, {"f3", 500, 18000, 5000}, {"type", 0, 3, 0, true},
                 {"bias", -1, 1, 0}, {"solo", 0, 4, 0, true}, {"mix", 0, 1, 1}, {"out", -24, 12, 0}, {"quality", 0, 2, 0, true}};
  for (int b = 0; b < 4; b++) {
    type.params.push_back({std::string(kBands[b]) + "Drive", 0, 36, kDefaultDrive[b]});
    type.params.push_back({std::string(kBands[b]) + "Gain", -24, 12, 0});
  }
  type.create = [](const NodeContext& context) { return std::make_unique<MbSat>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

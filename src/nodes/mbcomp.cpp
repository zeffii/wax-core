// Multiband compressor, stereo: four bands (as `split`), each compressed on its own with its own threshold, ratio, attack,
// release and gain (the knee is shared), stereo-linked per band, added together again. `solo` hears one band; `mix` blends with
// the dry sound; `out` is the gain at the end. Port of webtracker src/audio/nodes/mbcomp.js.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/splitcore.h"

namespace wax {
namespace {

constexpr int kShared = 7, kPer = 5;   // f1 f2 f3 knee solo mix out, then thresh, ratio, attack, release, gain for each band
enum Param { kF1 = 0, kF2, kF3, kKnee, kSolo, kMix, kOut };
const double kDb = 20 / 2.302585092994046, kLin = 2.302585092994046 / 20;
const char* const kBands[4] = {"low", "lowmid", "highmid", "high"};
const char* const kPerNames[kPer] = {"Thresh", "Ratio", "Attack", "Release", "Gain"};
constexpr double kDefaults[4][kPer] = {{-22, 3, 30, 200, 0}, {-22, 3, 18, 150, 0}, {-24, 3, 8, 110, 0}, {-26, 3, 3, 80, 0}};
constexpr double kPerMin[kPer] = {-60, 1, 0.1, 5, -24}, kPerMax[kPer] = {0, 20, 200, 2000, 24};

class MbComp final : public Node {
public:
  explicit MbComp(const NodeContext& context) : sampleRate_(context.sampleRate) {}

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
    const double W = p(kKnee), mix = p(kMix), out = js::exp(p(kOut) * kLin);
    const int solo = jsRoundInt(p(kSolo));
    for (int b = 0; b < 4; b++) {
      const int o = kShared + b * kPer;
      const double T = p(o), R = std::max(1.0, p(o + 1)), ca = coef(p(o + 2)), cr = coef(p(o + 3)), makeup = js::exp(p(o + 4) * kLin);
      float* L = band_[b].data();
      float* Rr = band_[4 + b].data();
      double red = reduction_[b];
      const bool active = solo == 0 || solo == b + 1;
      for (int i = 0; i < n; i++) {
        const double level = kDb * js::log(std::max(std::max(std::abs(static_cast<double>(L[i])), std::abs(static_cast<double>(Rr[i]))), 1e-9));
        const double over = level - T;
        double target;
        if (2 * over < -W) target = 0;
        else if (W > 0 && 2 * std::abs(over) <= W) target = (1 / R - 1) * js::pow(over + W / 2, 2) / (2 * W);
        else target = over / R - over;
        red = target < red ? target + (red - target) * ca : target + (red - target) * cr;
        const double gain = active ? js::exp(red * kLin) * makeup : 0;
        L[i] = static_cast<float>(static_cast<double>(L[i]) * gain);
        Rr[i] = static_cast<float>(static_cast<double>(Rr[i]) * gain);
      }
      reduction_[b] = std::abs(red) < 1e-9 ? 0 : red;
    }
    for (int i = 0; i < n; i++) {
      const double inL = inputs[0][i], inR = inputs[1][i];
      const double l = static_cast<double>(band_[0][i]) + band_[1][i] + band_[2][i] + band_[3][i];
      const double r = static_cast<double>(band_[4][i]) + band_[5][i] + band_[6][i] + band_[7][i];
      outputs[0][i] = static_cast<float>((inL + (l - inL) * mix) * out);
      outputs[1][i] = static_cast<float>((inR + (r - inR) * mix) * out);
    }
  }

private:
  double coef(double ms) const { return js::exp(-1 / (ms * 0.001 * sampleRate_)); }

  double sampleRate_;
  splitcore::Channel split_[2];
  splitcore::Crossovers cross_;
  std::vector<float> band_[8];
  double reduction_[4] = {0, 0, 0, 0};
};

}  // namespace

void registerMbComp(NodeRegistry& registry) {
  NodeType type;
  type.type = "mbcomp";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {{"f1", 30, 2000, 120}, {"f2", 100, 8000, 800}, {"f3", 500, 18000, 5000}, {"knee", 0, 24, 6}, {"solo", 0, 4, 0, true}, {"mix", 0, 1, 1}, {"out", -24, 24, 0}};
  for (int b = 0; b < 4; b++)
    for (int k = 0; k < kPer; k++) type.params.push_back({std::string(kBands[b]) + kPerNames[k], kPerMin[k], kPerMax[k], kDefaults[b][k]});
  type.create = [](const NodeContext& context) { return std::make_unique<MbComp>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

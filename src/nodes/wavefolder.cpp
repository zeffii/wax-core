// Wave folder: the louder the wave is pushed in, the more times it turns round at the edge and comes back, so a plain
// sine grows harmonics. `symmetry` moves the wave off centre first, so the folds are uneven and add the even harmonics.
// The `fold` input (control, added to the amount) lets an envelope or LFO sweep the timbre. The folder runs at 2x or
// 4x the rate (`oversample`) to keep the new harmonics from folding back as aliasing; the dry signal is delayed to match.
// Port of webtracker src/audio/nodes/wavefolder.js.
#include <algorithm>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/oversample.h"
#include "nodes/westcore.h"

namespace wax {
namespace {

enum Param { kFold = 0, kSymmetry, kDrive, kMix, kOversample, kLevel };

constexpr int kFactors[3] = {1, 2, 4};

int oversampleMode(double setting) { return std::min(2, std::max(0, jsRoundInt(setting))); }

class WaveFolder final : public Node {
public:
  explicit WaveFolder(const NodeContext& context)
      : foldOn_(context.connected.size() > 1 && context.connected[1]),
        chains_{{Oversampler(1), Oversampler(1)}, {Oversampler(2), Oversampler(2)}, {Oversampler(4), Oversampler(4)}},
        delays_{FixedDelay(48), FixedDelay(48)} {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* mod = inputs[2];
    const double base = params[slots[kFold]], sym = params[slots[kSymmetry]] * 0.6, drive = params[slots[kDrive]];
    const double mix = params[slots[kMix]], level = params[slots[kLevel]];
    const int mode = oversampleMode(params[slots[kOversample]]);
    for (int c = 0; c < 2; c++) {
      Oversampler& chain = chains_[mode][c];
      const float* x = inputs[c];
      float* out = outputs[c];
      const int wait = chain.latency();
      for (int i = 0; i < n; i++) {
        const double amount = clamp(base + (foldOn_ ? mod[i] : 0), 0, 1);
        const double wet = dc_[c].run(chain.run(x[i], [&](double v) { return foldWave(v * drive, amount, sym); }));
        const double dry = delays_[c].push(x[i], wait);
        out[i] = static_cast<float>((dry + (wet - dry) * mix) * level);
      }
    }
  }

private:
  bool foldOn_;
  Oversampler chains_[3][2];   // 1x, 2x and 4x, for each channel
  FixedDelay delays_[2];
  DcBlock dc_[2];
};

}  // namespace

void registerWaveFolder(NodeRegistry& registry) {
  NodeType type;
  type.type = "wavefolder";
  type.inputs = {"in", "fold"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"fold", PortKind::Control}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.latency = [](const std::map<std::string, double>& settings, double) { return static_cast<double>(oversampleLatency(kFactors[oversampleMode(settings.at("oversample"))])); };
  type.params = {
    {"fold", 0, 1, 0.4},
    {"symmetry", -1, 1, 0},
    {"drive", 0.1, 4, 1},              // input gain before the folder
    {"mix", 0, 1, 1},
    {"oversample", 0, 2, 1, true},     // 0: 1x, 1: 2x, 2: 4x
    {"level", 0, 2, 1},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<WaveFolder>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

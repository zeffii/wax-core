// Spread oscillator: the sound source of the pad synth on its own, with no envelope; see nodes/padcore.h. Silent until the first note; the
// last note sets the pitch. `fm` bends the pitch, in octaves. Port of webtracker src/audio/nodes/spreadosc.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/padcore.h"

namespace wax {
namespace {

enum Param { kBandwidth = 0, kSpread, kTilt, kEvenOdd, kStretch, kHarmonics, kWidth, kMotion, kTune, kLevel, kQuality };

class SpreadOsc final : public Node {
public:
  explicit SpreadOsc(const NodeContext& context)
      : sampleRate_(context.sampleRate), fmOn_(!context.connected.empty() && context.connected[0]), player_(context.sampleRate) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { if (!started_) fresh_ = true; note_ = event.note; started_ = true; }
    else if (event.type == NoteType::Cut) started_ = false;
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (!started_) { std::fill(outL, outL + n, 0.0f); std::fill(outR, outR + n, 0.0f); return; }
    auto g = [&](int i) -> double { return params[slots[i]]; };
    if (fresh_) {
      band_ = spreadBandOf(note_);
      tables_ = spreadTables(sampleRate_, jsRoundInt(g(kQuality)), band_, {g(kBandwidth), g(kSpread), g(kTilt), g(kEvenOdd), g(kStretch), jsRoundInt(g(kHarmonics))});
      player_.restart();
      fresh_ = false;
    }
    const double f0 = 440 * js::pow(2, (note_ + g(kTune) - 69) / 12);
    const double speed = (f0 * (fmOn_ ? js::pow(2, inputs[0][0]) : 1)) / spreadBandPitch(band_);
    player_.render(*tables_, n, speed, g(kMotion), g(kWidth), outL, outR);
    const double level = g(kLevel);
    for (int i = 0; i < n; i++) { outL[i] = static_cast<float>(outL[i] * level); outR[i] = static_cast<float>(outR[i] * level); }
  }

private:
  double sampleRate_;
  bool fmOn_;
  SpreadPlayer player_;
  std::shared_ptr<const SpreadTables> tables_;
  int note_ = 60, band_ = 0;
  bool started_ = false, fresh_ = false;
};

}  // namespace

void registerSpreadOsc(NodeRegistry& registry) {
  NodeType type;
  type.type = "spreadosc";
  type.inputs = {"fm"};
  type.outputs = {"out"};
  type.kinds = {{"fm", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"bandwidth", 1, 400, 45},
    {"spread", 0, 2, 1},
    {"tilt", -12, 12, 0},
    {"evenodd", -1, 1, 0},
    {"stretch", 0, 1, 0},
    {"harmonics", 1, 64, 32, true},
    {"width", 0, 1, 0.8},
    {"motion", 0, 1, 0.4},
    {"tune", -48, 48, 0},
    {"level", 0, 1, 0.5},
    {"quality", 0, 2, 0, true},   // 0 standard, 1 eco (a shorter table), 2 high (a longer one)
  };
  type.create = [](const NodeContext& context) { return std::make_unique<SpreadOsc>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

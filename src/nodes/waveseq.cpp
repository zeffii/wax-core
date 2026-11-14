// Wave seq: one wave sequence on its own. Silent until the first note; each note starts the sequence again from its first step.
// Port of webtracker src/audio/nodes/waveseq.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/vectorcore.h"

namespace wax {
namespace {

enum Param { kFreq = 0, kTrack, kTune, kFine, kRate, kSmode, kLevel };

class WaveSeq final : public Node {
public:
  explicit WaveSeq(const NodeContext& context) : sampleRate_(context.sampleRate), fmOn_(!context.connected.empty() && context.connected[0]), clock_(context.clock), player_(0) {
    const Json* raw = context.data && context.data->is_object() && context.data->contains("lanes") ? &(*context.data)["lanes"] : nullptr;
    player_.setSteps(vec::cleanLanes(raw, 1)[0]);
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { player_.start(started_); note_ = event.note; started_ = true; }
    else if (event.type == NoteType::Cut) started_ = false;
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* out = outputs[0];
    if (!started_) { std::fill(out, out + n, 0.0f); return; }
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double semis = g(kTune) + g(kFine) / 100 + (fmOn_ ? static_cast<double>(inputs[0][0]) * 12 : 0);
    const double hz = (g(kTrack) >= 0.5 ? 440 * js::pow(2, (note_ - 69) / 12.0) : g(kFreq)) * js::pow(2, semis / 12);
    player_.tune(hz / sampleRate_);
    const int mode = jsRoundInt(g(kSmode));
    const double ticksPerSample = g(kRate) / (clock_ ? clock_->tickSamples : sampleRate_ * 0.125), level = g(kLevel);
    for (int i = 0; i < n; i++) out[i] = static_cast<float>(player_.next(mode, ticksPerSample) * level);
  }

private:
  double sampleRate_;
  bool fmOn_;
  const Clock* clock_;
  vec::LanePlayer player_;
  int note_ = 60;
  bool started_ = false;
};

}  // namespace

void registerWaveSeq(NodeRegistry& registry) {
  NodeType type;
  type.type = "waveseq";
  type.inputs = {"fm"};
  type.outputs = {"out"};
  type.kinds = {{"fm", PortKind::Control}};
  type.params = {{"freq", 1, 5000, 110}, {"track", 0, 1, 1, true}, {"tune", -48, 48, 0}, {"fine", -100, 100, 0}, {"rate", 0.125, 8, 1}, {"smode", 0, 2, 1, true}, {"level", 0, 1, 0.6}};
  type.create = [](const NodeContext& context) { return std::make_unique<WaveSeq>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

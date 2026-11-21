// Ladder: a four-pole (24 dB) resonant lowpass with a built-in LFO and a dry/wet mix, made to sit in a chain and be swept.
// Zero-delay feedback, saturation in front of the poles, and a resonance that reaches self-oscillation. Mono: runs as a
// left/right pair on a stereo signal. Port of webtracker src/audio/nodes/ladder.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kCut = 0, kRes, kDrive, kRate, kDepth, kShape, kMix, kLevel, kPhase, kKeytrack };

double sat(double x) {
  const double x2 = x * x;
  return x > 3 ? 1 : x < -3 ? -1 : x * (27 + x2) / (27 + 9 * x2);
}
double flush(double v) { return std::abs(v) < 1e-18 ? 0 : v; }

class Ladder final : public Node {
public:
  explicit Ladder(const NodeContext& context)
      : sr_(context.sampleRate), modOn_(context.connected.size() > 1 && context.connected[1]),
        cs_(1 - js::exp(-4 / (context.sampleRate * 0.004))), ms_(1 - js::exp(-1 / (context.sampleRate * 0.02))) {}

  void note(const NoteEvent& event) override {   // (in a voice's own chain: before the voices node)
    if (event.type == NoteType::On) keyOctaves_ = (event.note - 60) / 12.0;
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* input = inputs[0];
    float* out = outputs[0];
    const float* mod = modOn_ ? inputs[1] : nullptr;
    auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double base = p(kCut) * js::pow(2, p(kKeytrack) * keyOctaves_), res = std::min(std::max(p(kRes), 0.0), 1.0);
    const double drive = 1 + p(kDrive) * 5, inc = p(kRate) / sr_, depth = p(kDepth);
    const double shift = p(kPhase);
    const int shape = jsRoundInt(p(kShape));
    const double mixT = p(kMix), levT = p(kLevel);
    const double k = res * 4.3, comp = 1 + res * 0.6, wet = 1 + res * res * 4, kick = res > 0.97 ? 1e-5 : 0;
    if (mixNow_ < 0) { mixNow_ = mixT; lvl_ = levT; }
    for (int i = 0; i < n; i += 4) {
      const int m = std::min(4, n - i);
      double v;
      const double ph = std::fmod(phase_ + shift, 1.0);
      if (shape == 0) v = js::sin(2 * kPi * ph);
      else if (shape == 1) v = ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4;
      else if (shape == 2) v = 1 - 2 * ph;
      else v = ph < 0.5 ? 1 : -1;
      phase_ += inc * m;
      if (phase_ >= 1) phase_ -= 1;
      double oct = v * depth;
      if (mod) oct += std::max(-8.0, std::min(8.0, static_cast<double>(mod[i])));
      const double target = base * js::pow(2, oct);
      if (fc_ < 0) fc_ = target;
      fc_ += (target - fc_) * cs_;
      const double gg = js::tan(kPi * std::min(std::max(fc_, 5.0), sr_ * 0.45) / sr_);
      const double G = gg / (1 + gg), G2 = G * G, G3 = G2 * G, G4 = G2 * G2, h = 1 - G, norm = 1 / (1 + k * G4);
      for (int j = i; j < i + m; j++) {
        mixNow_ += (mixT - mixNow_) * ms_;
        lvl_ += (levT - lvl_) * ms_;
        const double dry = input[j];
        const double S1 = h * s_[0], S2 = h * s_[1], S3 = h * s_[2], S4 = h * s_[3];
        const double sigma = G3 * S1 + G2 * S2 + G * S3 + S4;
        const double x1 = sat(((dry + kick) * comp * drive - k * sigma) * norm);
        const double y1 = G * x1 + S1, y2 = G * y1 + S2, y3 = G * y2 + S3, y4 = G * y3 + S4;
        s_[0] = flush(2 * y1 - s_[0]); s_[1] = flush(2 * y2 - s_[1]); s_[2] = flush(2 * y3 - s_[2]); s_[3] = flush(2 * y4 - s_[3]);
        out[j] = static_cast<float>((dry + (y4 * wet / drive - dry) * mixNow_) * lvl_);
      }
    }
  }

private:
  double sr_;
  bool modOn_;
  double cs_, ms_;
  double s_[4] = {0, 0, 0, 0};
  double phase_ = 0, fc_ = -1, mixNow_ = -1, lvl_ = -1;
  double keyOctaves_ = 0;   // where the last note is, in octaves from middle C
};

}  // namespace

void registerLadder(NodeRegistry& registry) {
  NodeType type;
  type.type = "ladder";
  type.inputs = {"in", "mod"};   // mod is in octaves, added to the LFO and applied block by block
  type.outputs = {"out"};
  type.dual = true;
  type.isEffect = true;
  type.kinds = {{"mod", PortKind::Control}};
  type.params = {
    {"cutoff", 20, 18000, 1200},
    {"resonance", 0, 1, 0.3},
    {"drive", 0, 1, 0.2},
    {"rate", 0.01, 20, 0.5},
    {"depth", 0, 4, 0},
    {"shape", 0, 3, 0, true},
    {"mix", 0, 1, 1},
    {"level", 0, 2, 1},
    {"phase", 0, 1, 0},
    {"keytrack", 0, 1, 0},   // how far the cutoff follows the note, 1 an octave per octave from middle C
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Ladder>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

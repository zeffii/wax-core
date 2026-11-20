// Pad synth: a few harmonics, each smeared into a band of frequencies with random phases (the PADsynth idea), made into a table and played
// at the note's speed; see nodes/padcore.h. Monophonic, last note wins, with glide. Port of webtracker src/audio/nodes/padsynth.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/adsr.h"
#include "nodes/all_nodes.h"
#include "nodes/padcore.h"

namespace wax {
namespace {

enum Param { kBandwidth = 0, kSpread, kTilt, kEvenOdd, kStretch, kHarmonics, kWidth, kMotion, kTune, kGlide, kAtk, kDec, kSus, kRel, kVel, kLevel, kQuality };

class PadSynth final : public Node {
public:
  explicit PadSynth(const NodeContext& context)
      : sampleRate_(context.sampleRate), pitchOn_(!context.connected.empty() && context.connected[0]), amp_(context.sampleRate), player_(context.sampleRate) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      if (amp_.idle()) { pos_ = event.note; fresh_ = true; }   // (a note on a sounding voice only moves the pitch: the table stays)
      note_ = event.note; velocity_ = event.vel;
      amp_.on();
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) {
      amp_.off();
    } else if (event.type == NoteType::Cut) {
      amp_.cut();
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (amp_.idle()) { std::fill(outL, outL + n, 0.0f); std::fill(outR, outR + n, 0.0f); return; }
    auto g = [&](int i) -> double { return params[slots[i]]; };
    amp_.set(g(kAtk), g(kDec), g(kSus), g(kRel));
    const double glideMs = g(kGlide);
    if (glideMs > 0.5) pos_ += (note_ - pos_) * (1 - js::exp(-n / (sampleRate_ * glideMs * 0.001 / 3)));
    else pos_ = note_;
    if (fresh_) {
      band_ = spreadBandOf(note_);
      tables_ = spreadTables(sampleRate_, jsRoundInt(g(kQuality)), band_, {g(kBandwidth), g(kSpread), g(kTilt), g(kEvenOdd), g(kStretch), jsRoundInt(g(kHarmonics))});
      player_.restart();
      fresh_ = false;
    }
    const double f0 = 440 * js::pow(2, (pos_ + g(kTune) - 69) / 12);
    const double speed = (f0 * (pitchOn_ ? js::pow(2, inputs[0][0]) : 1)) / spreadBandPitch(band_);
    player_.render(*tables_, n, speed, g(kMotion), g(kWidth), outL, outR);
    const double level = g(kLevel) * (1 - g(kVel) + g(kVel) * velocity_);
    for (int i = 0; i < n; i++) {
      const double e = amp_.next() * level;
      outL[i] = static_cast<float>(outL[i] * e); outR[i] = static_cast<float>(outR[i] * e);
    }
  }

private:
  double sampleRate_;
  bool pitchOn_;
  Adsr amp_;
  SpreadPlayer player_;
  std::shared_ptr<const SpreadTables> tables_;
  int note_ = 60, band_ = 0;
  double pos_ = 60, velocity_ = 1;
  bool fresh_ = true;
};

}  // namespace

void registerPadSynth(NodeRegistry& registry) {
  NodeType type;
  type.type = "padsynth";
  type.inputs = {"pitch"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"out", PortKind::Stereo}};
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
    {"glide", 0, 2000, 0},
    {"atk", 0.1, 5000, 900},
    {"dec", 1, 5000, 800},
    {"sus", 0, 1, 0.9},
    {"rel", 1, 5000, 1500},
    {"vel", 0, 1, 0.4},
    {"level", 0, 1, 0.5},
    {"quality", 0, 2, 0, true},   // 0 standard, 1 eco (a shorter table), 2 high (a longer one)
  };
  type.create = [](const NodeContext& context) { return std::make_unique<PadSynth>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

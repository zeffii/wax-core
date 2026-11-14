// Pulsar synth: a pulsar train played from notes. Monophonic, the last note wins, with optional glide. The note sets the
// fundamental, `formant` the colour (following the note by `track`), a drawn sweep moves the formant after each note starts.
// Port of webtracker src/audio/nodes/pulsar.js. And pulsetrain: the same oscillator on its own, silent until the first note.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/adsr.h"
#include "nodes/all_nodes.h"
#include "nodes/bezier.h"
#include "nodes/pulsarcore.h"

namespace wax {
namespace {

enum Param { kFormant = 0, kTrack, kWave, kTune, kSweep, kSweepMs, kMask, kJitter, kScatter, kGlide, kAtk, kDec, kSus, kRel, kVel, kLevel };

class Pulsar final : public Node {
public:
  explicit Pulsar(const NodeContext& context)
      : sampleRate_(context.sampleRate), pitchOn_(context.connected.size() > 0 && context.connected[0]),
        formOn_(context.connected.size() > 1 && context.connected[1]), amp_(context.sampleRate),
        sweepTable_(bezier::envelopeTable(bezier::drawingOf(context.data, "sweep", bezier::Shape::Fall))) {
    train_.setWindow(context.data);
  }

  bool busy() const override { return !amp_.idle(); }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      if (amp_.idle()) { train_.reset(); pos_ = event.note; }
      note_ = event.note; velocity_ = event.vel; age_ = 0;
      amp_.on();
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) amp_.off();
    else amp_.cut();
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (amp_.idle()) {
      std::fill(outL, outL + n, 0.0f);
      std::fill(outR, outR + n, 0.0f);
      return;
    }
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double sr = sampleRate_;
    amp_.set(g(kAtk), g(kDec), g(kSus), g(kRel));
    const double glideMs = g(kGlide);
    if (glideMs > 0.5) pos_ += (note_ - pos_) * (1 - js::exp(-static_cast<double>(n) / (sr * glideMs * 0.001 / 3)));
    else pos_ = note_;
    const int wave = jsRoundInt(g(kWave));
    const double mask = g(kMask), jitter = g(kJitter), scatter = g(kScatter);
    const double sweepLen = std::max(1.0, g(kSweepMs) * 0.001 * sr), sweepOct = g(kSweep);
    const double baseHz = 440 * js::pow(2, (pos_ + g(kTune) - 69) / 12);
    const double formantHz = g(kFormant) * js::pow(2, (g(kTrack) * (pos_ - 60)) / 12);
    const double level = g(kLevel) * (1 - g(kVel) + g(kVel) * velocity_) * 2;
    const double nyq = sr * 0.45;
    for (int i = 0; i < n; i++) {
      const double a = amp_.next();
      const double fm = pitchOn_ ? js::pow(2, static_cast<double>(inputs[0][i])) : 1;
      const double oct = sweepOct * bezier::readEnvelope(sweepTable_, age_ / sweepLen) + (formOn_ ? static_cast<double>(inputs[1][i]) : 0.0);
      const double fHz = std::min(nyq, formantHz * js::pow(2, oct));
      double v[2];
      train_.step(std::min(0.45, (baseHz * fm) / sr), fHz / sr, wave, mask, jitter, scatter, v);
      outL[i] = static_cast<float>(v[0] * a * level);
      outR[i] = static_cast<float>(v[1] * a * level);
      if (age_ < sweepLen) age_++;
    }
  }

private:
  double sampleRate_;
  bool pitchOn_, formOn_;
  Adsr amp_;
  pulsar::PulsarTrain train_;
  bezier::Table sweepTable_;
  int note_ = 60;
  double pos_ = 60, velocity_ = 1, age_ = 0;
};

enum TrainParam { kTFreq = 0, kTTrack, kTTune, kTFormant, kTWave, kTMask, kTJitter, kTScatter, kTLevel };

class PulseTrain final : public Node {
public:
  explicit PulseTrain(const NodeContext& context)
      : sampleRate_(context.sampleRate), fmOn_(context.connected.size() > 0 && context.connected[0]),
        formOn_(context.connected.size() > 1 && context.connected[1]) {
    train_.setWindow(context.data);
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { if (!started_) train_.reset(); note_ = event.note; started_ = true; }
    else if (event.type == NoteType::Cut) started_ = false;
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (!started_) {
      std::fill(outL, outL + n, 0.0f);
      std::fill(outR, outR + n, 0.0f);
      return;
    }
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double sr = sampleRate_;
    const double base = g(kTTrack) >= 0.5 ? 440 * js::pow(2, (note_ + g(kTTune) - 69) / 12) : g(kTFreq) * js::pow(2, g(kTTune) / 12);
    const int wave = jsRoundInt(g(kTWave));
    const double mask = g(kTMask), jitter = g(kTJitter), scatter = g(kTScatter), level = g(kTLevel);
    const double formant = g(kTFormant), nyq = sr * 0.45;
    for (int i = 0; i < n; i++) {
      const double inc = std::min(0.45, (base * (fmOn_ ? js::pow(2, static_cast<double>(inputs[0][i])) : 1)) / sr);
      const double fHz = std::min(nyq, formant * (formOn_ ? js::pow(2, static_cast<double>(inputs[1][i])) : 1));
      double v[2];
      train_.step(inc, fHz / sr, wave, mask, jitter, scatter, v);
      outL[i] = static_cast<float>(v[0] * level);
      outR[i] = static_cast<float>(v[1] * level);
    }
  }

private:
  double sampleRate_;
  bool fmOn_, formOn_;
  pulsar::PulsarTrain train_;
  int note_ = 60;
  bool started_ = false;
};

}  // namespace

void registerPulsar(NodeRegistry& registry) {
  {
    NodeType type;
    type.type = "pulsar";
    type.inputs = {"pitch", "form"};
    type.outputs = {"out"};
    type.kinds = {{"pitch", PortKind::Control}, {"form", PortKind::Control}, {"out", PortKind::Stereo}};
    type.params = {
      {"formant", 40, 12000, 900}, {"track", 0, 1, 0.4}, {"wave", 0, 5, 0, true}, {"tune", -24, 24, 0}, {"sweep", -4, 4, 1.5},
      {"sweepms", 5, 5000, 400}, {"mask", 0, 1, 0}, {"jitter", 0, 1, 0}, {"scatter", 0, 1, 0.4}, {"glide", 0, 2000, 0},
      {"atk", 0.1, 5000, 2}, {"dec", 1, 5000, 300}, {"sus", 0, 1, 0.8}, {"rel", 1, 5000, 200}, {"vel", 0, 1, 0.5}, {"level", 0, 1, 0.5},
    };
    type.create = [](const NodeContext& context) { return std::make_unique<Pulsar>(context); };
    registry.add(std::move(type));
  }
  {
    NodeType type;
    type.type = "pulsetrain";
    type.inputs = {"fm", "form"};
    type.outputs = {"out"};
    type.kinds = {{"fm", PortKind::Control}, {"form", PortKind::Control}, {"out", PortKind::Stereo}};
    type.params = {
      {"freq", 1, 5000, 110}, {"track", 0, 1, 1, true}, {"tune", -48, 48, 0}, {"formant", 40, 12000, 900}, {"wave", 0, 5, 0, true},
      {"mask", 0, 1, 0}, {"jitter", 0, 1, 0}, {"scatter", 0, 1, 0}, {"level", 0, 1, 0.6},
    };
    type.create = [](const NodeContext& context) { return std::make_unique<PulseTrain>(context); };
    registry.add(std::move(type));
  }
}

}  // namespace wax

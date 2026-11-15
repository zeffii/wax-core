// Compass: a vector synth with wave sequencing. Four lanes (wave sequences) at the points of a diamond, a drawn path that moves the
// mix, a low-pass filter and an amplitude, each with a drawn envelope. Monophonic, last note wins. Port of webtracker
// src/audio/nodes/compass.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/bezierenv.h"
#include "nodes/shared_dsp.h"
#include "nodes/svf.h"
#include "nodes/vectorcore.h"

namespace wax {
namespace {

enum Param {
  kX = 0, kY, kPath, kPathms, kPmode, kRate, kSmode, kTune, kFine, kAtune, kBtune, kCtune, kDtune, kDetune, kWidth, kGlide, kCutoff, kRes, kFenv, kFtime, kFtrack,
  kAtime, kVel, kLevel
};
const int kLaneTune[4] = {kAtune, kBtune, kCtune, kDtune};
const double kLaneDetune[4] = {-1, 0.33, 1, -0.33};
const double kLanePan[4] = {-1, 0, 1, 0};
constexpr int kFilterEvery = 16;

bezier::Spline defaultAmp() {
  const double r0[2] = {0, 0}, l1[2] = {0, 0}, r1[2] = {0.1, 0}, l2[2] = {-0.5, 0};
  bezier::Spline s = {bezier::makeAnchor(0, 0, nullptr, r0), bezier::makeAnchor(0.02, 1, l1, r1), bezier::makeAnchor(1, 0, l2, nullptr)};
  s[1].sustain = true;
  return s;
}

bezier::Spline defaultFilter() {
  const double r0[2] = {0.05, -0.5}, l1[2] = {-0.55, 0};
  return {bezier::makeAnchor(0, 1, nullptr, r0), bezier::makeAnchor(1, 0, l1, nullptr)};
}

class Compass final : public Node {
public:
  explicit Compass(const NodeContext& context)
      : sampleRate_(context.sampleRate), clock_(context.clock), pitchOn_(conn(context, 0)), xOn_(conn(context, 1)), yOn_(conn(context, 2)),
        amp_(context.sampleRate, defaultAmp()), filterEnv_(context.sampleRate, defaultFilter()), filterL_(context.sampleRate), filterR_(context.sampleRate) {
    amp_.setDrawing(context.data, "amp");
    filterEnv_.setDrawing(context.data, "filter");
    for (int i = 0; i < 4; i++) players_.emplace_back(i);
    const Json* lanes = context.data && context.data->is_object() && context.data->contains("lanes") ? &(*context.data)["lanes"] : nullptr;
    const std::vector<vec::Lane> clean = vec::cleanLanes(lanes, 4);
    for (int i = 0; i < 4; i++) players_[static_cast<std::size_t>(i)].setSteps(clean[static_cast<std::size_t>(i)]);
    const Json* path = context.data && context.data->is_object() && context.data->contains("path") ? &(*context.data)["path"] : nullptr;
    path_ = vec::cleanPath(path);
  }

  bool busy() const override { return !amp_.idle(); }

  void data(const Json& data) override {
    amp_.setDrawing(&data, "amp");
    filterEnv_.setDrawing(&data, "filter");
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      const bool fromSilence = amp_.idle();
      if (fromSilence) { pitchNow_ = event.note; filterL_.reset(); filterR_.reset(); fresh_ = true; }
      for (vec::LanePlayer& player : players_) player.start(!fromSilence);
      note_ = event.note; velocity_ = event.vel; age_ = 0;
      amp_.on(); filterEnv_.on();
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) { amp_.off(); filterEnv_.off(); }
    else { amp_.cut(); filterEnv_.cut(); }
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
    amp_.setTime(g(kAtime)); filterEnv_.setTime(g(kFtime));

    const double glideMs = g(kGlide);
    if (glideMs > 0.5) pitchNow_ += (note_ - pitchNow_) * (1 - js::exp(-n / (sr * glideMs * 0.001 / 3)));
    else pitchNow_ = note_;
    const double bend = pitchOn_ ? static_cast<double>(inputs[0][0]) * 12 : 0;
    const double basePitch = pitchNow_ + g(kTune) + g(kFine) / 100 + bend, detune = g(kDetune) / 100;
    for (int i = 0; i < 4; i++) {
      const double semis = basePitch + g(kLaneTune[i]) + detune * kLaneDetune[i];
      players_[static_cast<std::size_t>(i)].tune((440 * js::pow(2, (semis - 69) / 12)) / sr);
    }

    vec::pathAt(path_, jsRoundInt(g(kPmode)), (age_ + n) / (g(kPathms) * 0.001 * sr), position_);
    const double follow = g(kPath);
    const double x = g(kX) + follow * position_[0] + (xOn_ ? static_cast<double>(inputs[1][0]) : 0);
    const double y = g(kY) + follow * position_[1] + (yOn_ ? static_cast<double>(inputs[2][0]) : 0);
    vec::diamondWeights(std::max(-1.0, std::min(1.0, x)), std::max(-1.0, std::min(1.0, y)), target_);
    if (fresh_) { for (int k = 0; k < 4; k++) weights_[k] = target_[k]; fresh_ = false; }
    for (int k = 0; k < 4; k++) step_[k] = static_cast<float>((static_cast<double>(target_[k]) - weights_[k]) / n);

    const double width = g(kWidth);
    for (int k = 0; k < 4; k++) {
      const double angle = ((kLanePan[k] * width + 1) * kPi) / 4;
      gainL_[k] = static_cast<float>(js::cos(angle) * kSqrt2);
      gainR_[k] = static_cast<float>(js::sin(angle) * kSqrt2);
    }

    const int seqMode = jsRoundInt(g(kSmode));
    const double ticksPerSample = g(kRate) / (clock_ ? clock_->tickSamples : sr * 0.125);
    const double cutoff = g(kCutoff) * js::pow(2, (g(kFtrack) * (pitchNow_ - 60)) / 12), resonance = g(kRes), envOctaves = g(kFenv);
    const double level = g(kLevel) * (1 - g(kVel) + g(kVel) * velocity_);

    for (int i = 0; i < n; i++) {
      double left = 0, right = 0;
      for (int k = 0; k < 4; k++) {
        weights_[k] = static_cast<float>(static_cast<double>(weights_[k]) + step_[k]);
        const double sample = players_[static_cast<std::size_t>(k)].next(seqMode, ticksPerSample) * weights_[k];
        left += sample * gainL_[k]; right += sample * gainR_[k];
      }
      const double sweep = filterEnv_.next();
      if (i % kFilterEvery == 0) {
        const double hz = std::min(20000.0, cutoff * js::pow(2, envOctaves * sweep));
        filterL_.set(hz, resonance); filterR_.set(hz, resonance);
      }
      const double loudness = amp_.next() * level;
      outL[i] = static_cast<float>(filterL_.next(left) * loudness);
      outR[i] = static_cast<float>(filterR_.next(right) * loudness);
    }
    age_ += n;
  }

private:
  static bool conn(const NodeContext& context, std::size_t i) { return context.connected.size() > i && context.connected[i]; }

  double sampleRate_;
  const Clock* clock_;
  bool pitchOn_, xOn_, yOn_;
  BezierEnvelope amp_, filterEnv_;
  std::vector<vec::LanePlayer> players_;
  vec::Path path_;
  LowPassSvf filterL_, filterR_;
  double position_[2] = {0, 0};
  float target_[4] = {}, weights_[4] = {}, step_[4] = {}, gainL_[4] = {}, gainR_[4] = {};
  double note_ = 60, pitchNow_ = 60, velocity_ = 1, age_ = 0;
  bool fresh_ = true;
};

}  // namespace

void registerCompass(NodeRegistry& registry) {
  NodeType type;
  type.type = "compass";
  type.inputs = {"pitch", "x", "y"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"x", PortKind::Control}, {"y", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"x", -1, 1, 0}, {"y", -1, 1, 0}, {"path", 0, 1, 1}, {"pathms", 10, 60000, 4000}, {"pmode", 0, 2, 1, true}, {"rate", 0.125, 8, 1}, {"smode", 0, 2, 1, true},
    {"tune", -24, 24, 0}, {"fine", -100, 100, 0}, {"atune", -24, 24, 0, true}, {"btune", -24, 24, 0, true}, {"ctune", -24, 24, 0, true}, {"dtune", -24, 24, 0, true},
    {"detune", 0, 50, 0}, {"width", 0, 1, 0.3}, {"glide", 0, 2000, 0}, {"cutoff", 20, 20000, 20000}, {"res", 0, 1, 0.1}, {"fenv", -8, 8, 0}, {"ftime", 5, 20000, 600},
    {"ftrack", 0, 1, 0}, {"atime", 5, 20000, 800}, {"vel", 0, 1, 0.5}, {"level", 0, 1, 0.5},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Compass>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Khufu: the voice of a string machine. One sawtooth per register (an octave under the note, the note, an octave above), all cut
// from the same running phase like a divide-down organ, through a low cut and a gentle low-pass, shaped by a drawn amplitude
// envelope; a slow vibrato is built in. Monophonic. Port of webtracker src/audio/nodes/khufu.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/bezierenv.h"
#include "nodes/shared_dsp.h"
#include "nodes/svf.h"

namespace wax {
namespace {

enum Param { kLow = 0, kMid, kHigh, kTone, kLowCut, kVib, kVibRate, kTune, kFine, kATime, kVel, kLevel };

// a slow rise, held, and a long fall when the note ends
bezier::Spline defaultAmp() {
  const double r0[2] = {0.08, 0.5}, l1[2] = {-0.1, 0}, r1[2] = {0.1, 0}, l2[2] = {-0.5, 0};
  bezier::Spline s = {bezier::makeAnchor(0, 0, nullptr, r0), bezier::makeAnchor(0.25, 1, l1, r1), bezier::makeAnchor(1, 0, l2, nullptr)};
  s[1].sustain = true;
  return s;
}

// A falling sawtooth at `phase` (0 to 1) whose jump is rounded over one sample either side (polyBLEP).
double cleanSaw(double phase, double inc) {
  double value = 1 - 2 * phase;
  if (phase < inc) { const double t = phase / inc; value -= t + t - t * t - 1; }
  else if (phase > 1 - inc) { const double t = (phase - 1) / inc; value -= t * t + t + t + 1; }
  return value;
}

class Khufu final : public Node {
public:
  explicit Khufu(const NodeContext& context)
      : sampleRate_(context.sampleRate), pitchOn_(!context.connected.empty() && context.connected[0]), amp_(context.sampleRate, defaultAmp()), tone_(context.sampleRate) {
    amp_.setDrawing(context.data, "amp");
  }

  bool busy() const override { return !amp_.idle(); }

  void data(const Json& data) override { amp_.setDrawing(&data, "amp"); }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      if (amp_.idle()) { tone_.reset(); cutState_ = 0; }
      note_ = event.note; velocity_ = event.vel;
      amp_.on();
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) amp_.off();
    else amp_.cut();
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    if (amp_.idle()) {
      std::fill(outputs[0], outputs[0] + n, 0.0f);
      std::fill(outputs[1], outputs[1] + n, 0.0f);
      return;
    }
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double sr = sampleRate_;
    amp_.setTime(g(kATime));
    tone_.set(g(kTone), 0.15);
    const double cut = 1 - js::exp((-2 * kPi * g(kLowCut)) / sr);
    const double levels[3] = {g(kLow), g(kMid), g(kHigh)};
    const double semis = note_ + g(kTune) + g(kFine) / 100 + (pitchOn_ ? static_cast<double>(inputs[0][0]) * 12 : 0.0);
    const double lowHz = 440 * js::pow(2, (semis - 12 - 69) / 12);
    const double vibDepth = g(kVib) / 1200, vibInc = g(kVibRate) / sr;
    const double loudness = g(kLevel) * (1 - g(kVel) + g(kVel) * velocity_) * 0.35;
    for (int i = 0; i < n; i++) {
      vibPhase_ += vibInc; if (vibPhase_ >= 1) vibPhase_ -= 1;
      const double inc = std::min(0.11, (lowHz * js::pow(2, vibDepth * js::sin(2 * kPi * vibPhase_))) / sr);
      phase_ += inc; if (phase_ >= 1) phase_ -= 1;
      double mix = 0;
      for (int r = 0; r < 3; r++) {
        if (levels[r] <= 0) continue;
        const double times = static_cast<double>(1 << r), p = phase_ * times;
        mix += cleanSaw(p - std::floor(p), inc * times) * levels[r];
      }
      cutState_ += (mix - cutState_) * cut;
      const double value = tone_.next(mix - cutState_) * amp_.next() * loudness;
      outputs[0][i] = outputs[1][i] = static_cast<float>(value);
    }
  }

private:
  double sampleRate_;
  bool pitchOn_;
  BezierEnvelope amp_;
  LowPassSvf tone_;
  double phase_ = 0, vibPhase_ = 0, cutState_ = 0, velocity_ = 1;
  int note_ = 60;
};

}  // namespace

void registerKhufu(NodeRegistry& registry) {
  {
    NodeType type;
    type.type = "khufu";
    type.inputs = {"pitch"};
    type.outputs = {"out"};
    type.kinds = {{"pitch", PortKind::Control}, {"out", PortKind::Stereo}};
    type.params = {{"low", 0, 1, 0.3}, {"mid", 0, 1, 1}, {"high", 0, 1, 0.6}, {"tone", 300, 16000, 5000}, {"lowcut", 20, 1000, 120}, {"vib", 0, 50, 4},
                   {"vibrate", 0.5, 10, 5.2}, {"tune", -24, 24, 0}, {"fine", -100, 100, 0}, {"atime", 5, 20000, 1200}, {"vel", 0, 1, 0.3}, {"level", 0, 1, 0.5}};
    type.create = [](const NodeContext& context) { return std::make_unique<Khufu>(context); };
    registry.add(std::move(type));
  }
}

}  // namespace wax

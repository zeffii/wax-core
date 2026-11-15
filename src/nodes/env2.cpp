// Env 2: a control envelope drawn as a Bezier spline. A note on plays it from the first anchor over `time` ms; a sustain
// knot makes it wait while the note is held; without one it is one shot (and loops with `loop`). `length` ends each note
// after that many ticks. The output is the drawing times `amount`, with `vel` as the share the velocity scales.
// Port of webtracker src/audio/nodes/env2.js.
#include <algorithm>
#include <cmath>
#include <limits>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/bezier.h"

namespace wax {
namespace {

enum Param { kTime = 0, kAmount, kLoop, kVel, kLength };
enum Phase { kIdle, kRun, kHold, kRelease, kFade };
constexpr double kFadeMin = 0.01, kFadeMax = 0.06;   // seconds

class Env2 final : public Node {
public:
  explicit Env2(const NodeContext& context)
      : sampleRate_(context.sampleRate), clock_(context.clock),
        points_(bezier::drawingOf(context.data, "main")), table_(bezier::envelopeTable(points_)), knot_(bezier::sustainKnot(points_)) {}

  void data(const Json& data) override {
    points_ = bezier::drawingOf(&data, "main");
    table_ = bezier::envelopeTable(points_);
    knot_ = bezier::sustainKnot(points_);
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { fresh_ = true; pendingOff_ = false; velocity_ = event.vel; }
    else if (event.type == NoteType::Off || event.type == NoteType::AllOff) pendingOff_ = true;
    else if (event.type == NoteType::Cut) { phase_ = kIdle; last_ = 0; offset_ = 0; }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* o = outputs[0];
    const double len = std::max(1.0, params[slots[kTime]] * 0.001 * sampleRate_);
    const bool loop = params[slots[kLoop]] >= 0.5;
    const double gain = params[slots[kAmount]] * (1 - params[slots[kVel]] + params[slots[kVel]] * velocity_);
    const double ticks = jsRound(params[slots[kLength]]);
    const double slide = js::exp(-1 / (0.002 * sampleRate_));
    if (fresh_) {   // a new note: from where the output was
      fresh_ = false;
      offset_ = std::fabs(last_) > 1e-3 ? last_ - bezier::readEnvelope(table_, 0) * gain : 0;
      phase_ = kRun; pos_ = 0;
      left_ = ticks > 0 ? std::max(1.0, jsRound(ticks * (clock_ ? clock_->tickSamples : sampleRate_ * 0.125))) : std::numeric_limits<double>::infinity();
    }
    for (int i = 0; i < n; i++) {
      if (phase_ == kIdle) { o[i] = 0; continue; }
      if (pendingOff_) { pendingOff_ = false; letGo(len); }
      if (left_ != std::numeric_limits<double>::infinity() && --left_ == 0) letGo(len);   // `length` ran out: the same as a note off
      double x = pos_ / len;
      if (phase_ == kRun) {
        if (knot_ && x >= knot_->x) { phase_ = kHold; pos_ = knot_->x * len; x = knot_->x; }
        else if (!knot_ && loop && x >= 1) { pos_ -= len * std::floor(x); x -= std::floor(x); }
      }
      if (phase_ == kFade) {
        const double end = table_[bezier::kEnvPoints];
        shape_ = end + (fadeFrom_ - end) * (fadeLeft_ / fadeLen_);
        if (fadeLeft_ > 0) fadeLeft_--;
      } else {
        shape_ = bezier::readEnvelope(table_, x);
      }
      last_ = shape_ * gain;
      offset_ *= slide;
      o[i] = static_cast<float>(last_ + offset_);
      if (phase_ == kRun || phase_ == kRelease) pos_ += 1;
    }
    if (std::fabs(offset_) < 1e-9) offset_ = 0;
  }

private:
  void letGo(double len) {   // the note has ended
    if (!knot_ || (phase_ != kRun && phase_ != kHold)) return;   // a one shot plays through
    if (phase_ == kRun) {   // ended before the knot: a straight fade from here to the last anchor's level
      fadeLen_ = fadeLeft_ = jsRound(std::min(kFadeMax, std::max(kFadeMin, (1 - knot_->x) * len / sampleRate_)) * sampleRate_);
      fadeFrom_ = shape_;
      phase_ = kFade;
      return;
    }
    pos_ = knot_->x * len;
    phase_ = kRelease;
  }

  double sampleRate_;
  const Clock* clock_;
  bezier::Spline points_;
  bezier::Table table_;
  std::optional<bezier::Knot> knot_;
  int phase_ = kIdle;
  double pos_ = 0, velocity_ = 1, left_ = std::numeric_limits<double>::infinity();
  bool fresh_ = false, pendingOff_ = false;
  double shape_ = 0, last_ = 0, offset_ = 0, fadeFrom_ = 0, fadeLeft_ = 0, fadeLen_ = 1;
};

}  // namespace

void registerEnv2(NodeRegistry& registry) {
  NodeType type;
  type.type = "env2";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Control}};
  type.params = {
    {"time", 1, 20000, 300},          // ms from the first anchor to the last
    {"amount", 0, 10, 1},
    {"loop", 0, 1, 0, true},
    {"vel", 0, 1, 0},
    {"length", 0, 256, 0, true},      // ticks, 0: the note off ends the note
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Env2>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

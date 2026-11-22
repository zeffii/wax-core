// Rotary: a rotating speaker cabinet. The sound is split at `cross`: the top goes to a horn that spins, the bottom to a drum that
// spins the other way, a little slower, with two microphones in front. Doppler, tremolo and tone per rotor and microphone; `speed`
// (stop, slow, fast) is approached, not jumped to. Port of webtracker src/audio/nodes/rotary.js.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"
#include "nodes/svf.h"

namespace wax {
namespace {

enum Param { kSpeed = 0, kSlow, kFast, kInertia, kDrive, kDoppler, kTremolo, kSpread, kBalance, kCross, kMix };
constexpr double kButterworth = 0.3;
constexpr double kMaxDelayMs = 4;

struct RotorSpec { double ratio, direction, rise, fall, dopplerMs, tremolo, dull, bright; };
constexpr RotorSpec kHorn = {1, 1, 0.9, 1.3, 0.55, 0.65, 2200, 14000};
constexpr RotorSpec kDrum = {0.86, -1, 4.5, 5.5, 0.18, 0.35, 20000, 20000};

double nextRotorSpeed(const RotorSpec& rotor, double speedNow, double target, double seconds, double inertia) {
  const double time = (target > speedNow ? rotor.rise : rotor.fall) * std::max(0.05, inertia);
  return speedNow + (target - speedNow) * (1 - js::exp(-seconds / (time / 3)));
}

class Rotor {
public:
  Rotor(const RotorSpec& spec, double sampleRate)
      : spec_(spec), sampleRate_(sampleRate), size_(static_cast<int>(std::ceil((sampleRate * kMaxDelayMs) / 1000)) + 4), line_(size_, 0.0f) {}

  double speed = 0;   // Hz

  // input: this rotor's share of the sound; micAngle: how far each microphone stands off centre, in turns
  void next(double input, double micAngle, double doppler, double tremolo, double out[2]) {
    line_[write_] = static_cast<float>(input);
    angle_ += (spec_.direction * speed) / sampleRate_;
    angle_ -= std::floor(angle_);
    const double turn = 2 * kPi;
    const double facingL = 0.5 + 0.5 * js::cos(turn * (angle_ + micAngle)), facingR = 0.5 + 0.5 * js::cos(turn * (angle_ - micAngle));
    double left = hear(facingL, doppler, tremolo), right = hear(facingR, doppler, tremolo);
    if (spec_.dull < spec_.bright) {
      const double span = spec_.bright - spec_.dull, rate = (2 * kPi) / sampleRate_;
      toneL_ += (left - toneL_) * std::min(1.0, (spec_.dull + span * facingL) * rate); left = toneL_;
      toneR_ += (right - toneR_) * std::min(1.0, (spec_.dull + span * facingR) * rate); right = toneR_;
    }
    if (++write_ == size_) write_ = 0;
    out[0] = left;
    out[1] = right;
  }

private:
  double hear(double facing, double doppler, double tremolo) const {
    const double delay = (0.3 + spec_.dopplerMs * doppler * (1 - facing)) * 0.001 * sampleRate_;
    double at = write_ - delay;
    if (at < 0) at += size_;
    const int k = static_cast<int>(at);
    const double f = at - k;
    const int next = k + 1 == size_ ? 0 : k + 1;
    const double a = line_[k], b = line_[next];
    return (a + (b - a) * f) * (1 - spec_.tremolo * tremolo * (1 - facing));
  }

  RotorSpec spec_;
  double sampleRate_;
  int size_;
  std::vector<float> line_;
  int write_ = 0;
  double angle_ = 0, toneL_ = 0, toneR_ = 0;
};

class Rotary final : public Node {
public:
  explicit Rotary(const NodeContext& context)
      : sampleRate_(context.sampleRate), horn_(kHorn, context.sampleRate), drum_(kDrum, context.sampleRate), splitA_(context.sampleRate), splitB_(context.sampleRate) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const int speed = jsRoundInt(g(kSpeed));
    const double target = speed <= 0 ? 0 : speed == 1 ? g(kSlow) : g(kFast), inertia = g(kInertia), seconds = n / sampleRate_;
    horn_.speed = nextRotorSpeed(kHorn, horn_.speed, target * kHorn.ratio, seconds, inertia);
    drum_.speed = nextRotorSpeed(kDrum, drum_.speed, target * kDrum.ratio, seconds, inertia);

    const double cross = g(kCross);
    splitA_.set(cross, kButterworth);
    splitB_.set(cross, kButterworth);
    const double push = 1 + g(kDrive) * 9, makeUp = 1 / js::pow(push, 0.6);
    const double micAngle = g(kSpread) * 0.25, doppler = g(kDoppler), tremolo = g(kTremolo), mix = g(kMix);
    const double balance = g(kBalance);
    const double hornLevel = js::sin((balance * kPi) / 2) * kSqrt2, drumLevel = js::cos((balance * kPi) / 2) * kSqrt2;
    for (int i = 0; i < n; i++) {
      const double inL = inputs[0][i], inR = inputs[1][i];
      const double dry = 0.5 * (inL + inR);
      const double driven = js::tanh(dry * push) * makeUp;
      const double low = splitB_.next(splitA_.next(driven)), high = driven - low;
      double top[2], bottom[2];
      horn_.next(high, micAngle, doppler, tremolo, top);
      drum_.next(low, micAngle, doppler, tremolo, bottom);
      const double wetL = top[0] * hornLevel + bottom[0] * drumLevel, wetR = top[1] * hornLevel + bottom[1] * drumLevel;
      outputs[0][i] = static_cast<float>(inL + (wetL - inL) * mix);
      outputs[1][i] = static_cast<float>(inR + (wetR - inR) * mix);
    }
  }

private:
  double sampleRate_;
  Rotor horn_, drum_;
  LowPassSvf splitA_, splitB_;
};

}  // namespace

void registerRotary(NodeRegistry& registry) {
  NodeType type;
  type.type = "rotary";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.dryAtZero = "mix";
  type.params = {{"speed", 0, 2, 1, true}, {"slow", 0.1, 3, 0.8}, {"fast", 3, 12, 6.7}, {"inertia", 0.1, 4, 1}, {"drive", 0, 1, 0.2}, {"doppler", 0, 2, 1},
                 {"tremolo", 0, 1, 0.7}, {"spread", 0, 1, 0.7}, {"balance", 0, 1, 0.5}, {"cross", 200, 3000, 800}, {"mix", 0, 1, 1}};
  type.create = [](const NodeContext& context) { return std::make_unique<Rotary>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

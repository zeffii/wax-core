// Bongo: a small hand drum, played with the fingers and the palm: a short bank of ringing modes (the ratios of a drum skin plus a
// high rim ring), a knock from the shell under it, and the noise of the hand. `strike` slides between a muted hit, an open tone and
// a slap. Monophonic: a new hit adds to what is ringing. Port of webtracker src/audio/nodes/bongo.js.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kStrike = 0, kRing, kSkin, kSnap, kCrack, kShell, kRim, kHuman, kTune, kVel, kLevel };

struct Mode { double ratio, weight; };
const Mode kModes[] = {{1, 1}, {1.593, 0.55}, {2.136, 0.4}, {2.296, 0.3}, {2.653, 0.22}, {2.917, 0.16}};
constexpr int kModeCount = 6;
constexpr double kRimRatio = 4.6;
constexpr int kCount = kModeCount + 1;

struct Strike { double decay, bright, crack, snap, level; };
constexpr Strike kMuted{0.2, -0.8, 0.15, 0.3, 0.8}, kOpen{1, 0, 0.25, 0.6, 1}, kSlap{0.5, 1.2, 1, 1, 1.05};
Strike mixStrike(const Strike& a, const Strike& b, double t) {
  return {a.decay + (b.decay - a.decay) * t, a.bright + (b.bright - a.bright) * t, a.crack + (b.crack - a.crack) * t,
          a.snap + (b.snap - a.snap) * t, a.level + (b.level - a.level) * t};
}
Strike strikeAt(double s) { return s < 0.5 ? mixStrike(kMuted, kOpen, s * 2) : mixStrike(kOpen, kSlap, (s - 0.5) * 2); }

class Bongo final : public Node {
public:
  explicit Bongo(const NodeContext& context) : sampleRate_(context.sampleRate) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { note_ = event.note; velocity_ = std::max(0.0, std::min(1.0, event.vel)); pending_ = true; }
    else if (event.type == NoteType::Cut) { for (double& a : amp_) a = 0; shellAmp_ = crackAmp_ = 0; }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double sr = sampleRate_;
    const double human = g(kHuman);
    const Strike s = strikeAt(g(kStrike));
    const double ring = g(kRing) * 0.001 * s.decay, skin = g(kSkin);
    const double base = 440 * js::pow(2, (note_ + g(kTune) - 69) / 12);

    if (pending_) {
      pending_ = false; age_ = 0;
      const double velAmt = g(kVel), loud = 1 - velAmt + velAmt * velocity_;
      const double r1 = rand(), r2 = rand();
      const double pitchJitter = 1 + r1 * 0.012 * human, levelJitter = 1 - std::abs(r2) * 0.25 * human;
      hitGain_ = loud * levelJitter * s.level;
      const double r3 = rand();
      const double bright = s.bright + (velocity_ - 0.7) * velAmt * 1.2 + r3 * 0.25 * human;
      for (int k = 0; k < kCount; k++) {
        const bool isRim = k == kModeCount;
        const double ratio = isRim ? kRimRatio : kModes[k].ratio;
        freq_[k] = base * ratio * pitchJitter;
        const double w = (isRim ? g(kRim) * 0.7 : kModes[k].weight) * js::pow(ratio, bright);
        amp_[k] += w * hitGain_;
        const double t60 = std::max(0.004, ring * js::pow(ratio, -skin * 1.6) * (isRim ? 0.5 : 1));
        decay_[k] = js::pow(10, -3 / (t60 * sr));
      }
      shellAmp_ = g(kShell) * hitGain_; crackAmp_ = g(kCrack) * s.crack * hitGain_; shellPhase_ = 0;
      pitchLift_ = g(kSnap) * s.snap * (0.5 + 0.5 * velocity_);
    }
    const double out = g(kLevel) * 0.3;
    const double shellFreq = base * 0.5, shellDecay = js::exp(-1 / (0.035 * sr)), crackDecay = js::exp(-1 / (0.004 * sr));
    const double hpCoef = js::exp(-2 * kPi * 1800 / sr), lpCoef = 1 - js::exp(-2 * kPi * 9000 / sr);
    for (int i = 0; i < n; i++) {
      const double lift = 1 + 0.35 * pitchLift_ * js::exp(-age_ / (0.006 * sr));
      double y = 0;
      for (int k = 0; k < kCount; k++) {
        if (amp_[k] < 1e-6) continue;
        phase_[k] += freq_[k] * lift / sr;
        phase_[k] -= std::floor(phase_[k]);
        y += js::sin(2 * kPi * phase_[k]) * amp_[k];
        amp_[k] *= decay_[k];
      }
      if (shellAmp_ > 1e-6) {
        shellPhase_ += shellFreq * (1 + 0.5 * js::exp(-age_ / (0.01 * sr))) / sr;
        shellPhase_ -= std::floor(shellPhase_);
        y += js::sin(2 * kPi * shellPhase_) * shellAmp_ * 0.8;
        shellAmp_ *= shellDecay;
      }
      if (crackAmp_ > 1e-5) {
        const double r = rand();
        hp_ = r - hpx_ + hpCoef * hp_; hpx_ = r;
        lp_ += (hp_ - lp_) * lpCoef;
        y += lp_ * crackAmp_ * 1.4;
        crackAmp_ *= crackDecay;
      }
      age_++;
      const double o = y * out;
      outL[i] = static_cast<float>(o);
      outR[i] = static_cast<float>(o);
    }
    if (!std::isfinite(hp_ + lp_ + shellPhase_)) { hp_ = lp_ = hpx_ = 0; for (double& a : amp_) a = 0; }
  }

private:
  double rand() {
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    return static_cast<double>(seed_) / 4294967296.0 * 2 - 1;
  }

  double sampleRate_;
  double phase_[kCount] = {}, amp_[kCount] = {}, decay_[kCount] = {}, freq_[kCount] = {};
  int note_ = 74;
  double velocity_ = 1;
  bool pending_ = false;
  double age_ = 1e9;
  double shellPhase_ = 0, shellAmp_ = 0, crackAmp_ = 0, hp_ = 0, hpx_ = 0, lp_ = 0, pitchLift_ = 0, hitGain_ = 1;
  std::uint32_t seed_ = 0x3c6ef372u;
};

}  // namespace

void registerBongo(NodeRegistry& registry) {
  NodeType type;
  type.type = "bongo";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.params = {
    {"strike", 0, 1, 0.5}, {"ring", 30, 1500, 260}, {"skin", 0, 1, 0.45}, {"snap", 0, 1, 0.4}, {"crack", 0, 1, 0.5},
    {"shell", 0, 1, 0.35}, {"rim", 0, 1, 0.25}, {"human", 0, 1, 0.3}, {"tune", -48, 48, 0}, {"vel", 0, 1, 0.6}, {"level", 0, 1, 0.6},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Bongo>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

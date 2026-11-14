// Comb-resonator percussion. A short burst of noise (the strike) is fed into four feedback comb filters tuned to the
// note. A comb is a delay line fed back on itself, so it rings at the pitch whose period is the delay and at every
// multiple of it; the loop passes through a lowpass (`damp`) so the highs die first. The four combs sit at ratios
// that run from harmonic (1 2 3 4: a pitched, tom-like or bell-like tone) to inharmonic and metallic (`spread`).
// `hollow` flips the feedback negative, which drops the pitch an octave and keeps only the odd multiples. `sweep`
// starts the combs high and lets them fall to pitch over `sdec` ms; the delay lines are modulated while they ring,
// which gives the pitch dive its metallic smear. `drive` saturates the sum. Monophonic: a new note strikes the combs
// again and the old ring carries on into it. Note off is ignored; percussion rings out.
// Port of webtracker src/audio/nodes/combperc.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kDecay = 0, kDamp, kSpread, kHollow, kClick, kBright, kTune, kSweep, kSdec, kDrive, kVel, kLevel };

constexpr int kSize = 1 << 15, kMask = kSize - 1;
constexpr double kHarm[4] = {1, 2, 3, 4};
constexpr double kMetal[4] = {1, 1.504, 2.742, 4.156};
constexpr double kWeight[4] = {1, 0.8, 0.6, 0.5};

class CombPerc final : public Node {
public:
  explicit CombPerc(const NodeContext& context) : sampleRate_(context.sampleRate) {
    for (auto& b : bufs_) b.assign(kSize, 0.0);
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      note_ = event.note;
      velocity_ = clamp(event.vel, 0, 1);
      pending_ = true;
      active_ = true;
    } else if (event.type == NoteType::Cut) {
      active_ = false;
      pending_ = false;
      strikeLeft_ = 0;
      sweepEnv_ = 0;
      quiet_ = 0;
      clearCombs();
    }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    if (!active_) {
      std::fill(outLeft, outLeft + n, 0.0f);
      std::fill(outRight, outRight + n, 0.0f);
      return;
    }
    const auto g = [&](Param p) { return static_cast<double>(params[slots[p]]); };
    const double spread = g(kSpread);
    const bool hollow = g(kHollow) >= 0.5;
    const double decay = g(kDecay) * 0.001, sweep = g(kSweep), drive = g(kDrive);
    double ratio[4];
    for (int k = 0; k < 4; k++) ratio[k] = kHarm[k] + (kMetal[k] - kHarm[k]) * spread;
    const double base = 440 * js::pow(2.0, (note_ + g(kTune) - 69) / 12);
    const double cLp = 1 - g(kDamp) * 0.88;   // loop lowpass coefficient
    const double sDecay = js::exp(-6.9 / (sampleRate_ * g(kSdec) * 0.001));
    if (pending_) {
      pending_ = false;
      strikeLen_ = std::max(1, jsRoundInt(g(kClick) * 0.001 * sampleRate_));
      strikeLeft_ = strikeLen_;
      strikeGain_ = 1 - g(kVel) + g(kVel) * velocity_;
      sweepEnv_ = 1;
      strikeLp_ = 0;
    }
    const double hard = 0.08 + g(kBright) * 0.92;
    const double level = g(kLevel) * 0.9;
    const double driveGain = 1 + drive * 5;
    const double driveNorm = std::sqrt(driveGain);
    for (int i = 0; i < n; i++) {
      // the strike: noise through a one-pole lowpass, fading out over its length
      double x = 0;
      if (strikeLeft_ > 0) {
        strikeLp_ += hard * (rand() - strikeLp_);
        x = strikeLp_ * strikeGain_ * (static_cast<double>(strikeLeft_) / strikeLen_);
        strikeLeft_--;
      }
      const double pitch = base * js::pow(2.0, sweep * sweepEnv_);
      sweepEnv_ *= sDecay;
      double sum = 0;
      for (int k = 0; k < 4; k++) {
        const double D = std::max(2.5, std::min(static_cast<double>(kSize - 4), sampleRate_ / (pitch * ratio[k])));
        const int di = static_cast<int>(std::floor(D));
        const double df = D - di;
        std::vector<double>& b = bufs_[k];
        const double a0 = b[static_cast<std::size_t>((w_ - di) & kMask)], a1 = b[static_cast<std::size_t>((w_ - di - 1) & kMask)];
        const double d = a0 + (a1 - a0) * df;
        lp_[k] += cLp * (d - lp_[k]);
        // loop gain from the decay time: one pass of this delay line
        double fb = js::pow(10.0, -3 * (D / sampleRate_) / decay);
        if (fb > 0.99995) fb = 0.99995;
        const double y = x * 0.5 * kWeight[k] + lp_[k] * (hollow ? -fb : fb);
        b[static_cast<std::size_t>(w_ & kMask)] = y;
        sum += y * kWeight[k];
      }
      w_ = (w_ + 1) & kMask;
      double s = sum * 0.5;
      s = drive > 0 ? js::tanh(s * driveGain) / driveNorm : s;
      // remove DC left by the feedback paths
      const double o = s - dcx_ + 0.995 * dc_;
      dcx_ = s;
      dc_ = o;
      outLeft[i] = outRight[i] = static_cast<float>(o * level);
      const double ao = std::abs(o);
      quiet_ = ao > quiet_ ? ao : quiet_ * 0.9995;
    }
    if (!std::isfinite(dc_ + dcx_)) { dc_ = dcx_ = 0; clearCombs(); }
    if (quiet_ < 1e-5 && strikeLeft_ == 0) {
      active_ = false;
      quiet_ = 0;
      dc_ = dcx_ = 0;
      clearCombs();
    }
  }

private:
  void clearCombs() {
    for (auto& b : bufs_) std::fill(b.begin(), b.end(), 0.0);
    std::fill(std::begin(lp_), std::end(lp_), 0.0);
  }

  // noise, -1..1
  double rand() {
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return seed_ / 2147483648.0 - 1;
  }

  double sampleRate_;
  std::vector<double> bufs_[4];   // the four comb delay lines
  double lp_[4] = {0, 0, 0, 0};   // their loop lowpass memories
  int w_ = 0;                     // where the combs are being written
  int note_ = 60;
  double velocity_ = 1;
  bool pending_ = false, active_ = false;
  int strikeLeft_ = 0, strikeLen_ = 1;
  double strikeGain_ = 0, strikeLp_ = 0, sweepEnv_ = 0, quiet_ = 0, dc_ = 0, dcx_ = 0;
  std::uint32_t seed_ = 0x2545f491;
};

}  // namespace

void registerCombPerc(NodeRegistry& registry) {
  NodeType type;
  type.type = "combperc";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.shared = true;   // one resonator bank for the instrument, never copied for each voice
  type.params = {
    {"decay", 20, 4000, 250},     // ms for the ring to fall 60 dB
    {"damp", 0, 1, 0.35},
    {"spread", 0, 1, 0.6},        // 0: harmonic, 1: metallic
    {"hollow", 0, 1, 0, true},
    {"click", 0.2, 20, 2},        // ms, length of the strike
    {"bright", 0, 1, 0.7},        // brightness of the strike
    {"tune", -48, 48, 0},
    {"sweep", 0, 4, 0.5},         // octaves above pitch at the start of the hit
    {"sdec", 2, 500, 40},         // ms for the sweep to fall 60 dB
    {"drive", 0, 1, 0.2},
    {"vel", 0, 1, 0.6},
    {"level", 0, 1, 0.6},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<CombPerc>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Woodblock. A struck piece of wood: a short burst of noise (the strike) rings four resonant modes at inharmonic
// ratios, and the energy leaks out of them as they ring. `damping` is how long the lowest mode takes to fall by 60 dB,
// `loss` makes each higher mode leak faster, `hardness` is the stick, `inharm` spreads the modes away from their
// natural ratios. The pitch follows the note from `root` when `keytrack` is 1. Two ADSRs: volume and pitch (`bend`
// semitones, starting high and falling). Monophonic per node, last note wins. Port of webtracker src/audio/nodes/woodblock.js.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/adsr.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param {
  kTone = 0, kKeytrack, kRoot, kInharm, kHardness, kDamping, kLoss, kBend, kVel, kLevel,
  kAttack, kDecay, kSustain, kRelease, kPatk, kPdec, kPsus, kPrel
};

constexpr double kRatio[4] = {1, 1.52, 2.31, 3.38};   // the modes of a small block of wood, roughly
constexpr int kStride = 16;                           // the resonators' tuning is updated every 16 samples while the pitch moves

class Woodblock final : public Node {
public:
  explicit Woodblock(const NodeContext& context) : sampleRate_(context.sampleRate), amp_(context.sampleRate), pit_(context.sampleRate) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      note_ = event.note; velocity_ = event.vel; hit_ = true; amp_.on(); pit_.on();
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) {
      amp_.off(); pit_.off();
    } else if (event.type == NoteType::Cut) {
      amp_.cut(); pit_.cut();
      for (int k = 0; k < 4; k++) { s_[k] = 0; c_[k] = 0; }
      strike_ = 0; hit_ = false;
    }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    bool ringing = false;
    for (int k = 0; k < 4; k++) if (std::abs(s_[k]) > 1e-6) ringing = true;
    if (amp_.idle() && !hit_ && strike_ < 1e-5 && !ringing) {
      std::fill(outL, outL + n, 0.0f); std::fill(outR, outR + n, 0.0f);
      return;
    }
    auto g = [&](int i) -> double { return params[slots[i]]; };
    amp_.set(g(kAttack), g(kDecay), g(kSustain), g(kRelease));
    pit_.set(g(kPatk), g(kPdec), g(kPsus), g(kPrel));
    const double damping = g(kDamping), loss = g(kLoss), bend = g(kBend), inharm = g(kInharm), hard = g(kHardness);
    const double keyed = g(kKeytrack) >= 1 ? js::pow(2, (note_ - g(kRoot)) / 12) : 1;
    const double base = g(kTone) * keyed;
    const double v = 1 - g(kVel) + g(kVel) * velocity_;
    const double level = g(kLevel) * v * 0.5;
    const double bright = 0.25 + 0.7 * hard * (0.5 + 0.5 * velocity_);   // each mode is this much quieter than the one below it
    for (int k = 0; k < 4; k++) {
      gain_[k] = js::pow(bright, k);
      r_[k] = js::exp(-6.9078 / (std::max(1.0, damping / (1 + loss * 3 * k)) * 0.001 * sampleRate_));   // 60 dB in damping ms, shorter for the higher modes
    }
    if (hit_) {   // the strike: a burst of noise, shorter and brighter for a hard stick
      hit_ = false; strike_ = v;
      strikeDecay_ = js::exp(-1 / (sampleRate_ * (0.0012 - 0.0009 * hard)));
    }
    const double cut = 0.15 + 0.8 * hard;   // one-pole low-pass on the burst: a soft stick is muffled
    for (int i = 0; i < n; i++) {
      if ((i & (kStride - 1)) == 0) {
        const double pe = pit_.level(), f = base * js::pow(2, bend * pe / 12);
        for (int k = 0; k < 4; k++) {
          const double fk = std::min(sampleRate_ * 0.45, f * kRatio[k] * (1 + inharm * 0.07 * k * (k % 2 ? 1 : -0.6)));
          e_[k] = 2 * js::sin(kPi * fk / sampleRate_);
        }
      }
      pit_.next();
      noise_ ^= noise_ << 13; noise_ ^= noise_ >> 17; noise_ ^= noise_ << 5;
      lp_ += cut * ((noise_ / 2147483648.0 - 1) - lp_);
      const double x = lp_ * strike_ * 0.6;
      strike_ *= strikeDecay_;
      double sum = 0;
      for (int k = 0; k < 4; k++) {
        c_[k] -= e_[k] * s_[k];
        s_[k] += e_[k] * c_[k] + x * gain_[k];
        s_[k] *= r_[k]; c_[k] *= r_[k];
        sum += s_[k];
      }
      const double y = sum * level * amp_.next() * 4;
      outL[i] = static_cast<float>(y); outR[i] = static_cast<float>(y);
    }
    const double total = s_[0] + s_[1] + s_[2] + s_[3] + c_[0] + c_[1] + c_[2] + c_[3];
    if (!std::isfinite(total)) for (int k = 0; k < 4; k++) { s_[k] = 0; c_[k] = 0; }
    for (int k = 0; k < 4; k++) if (std::abs(s_[k]) < 1e-9 && std::abs(c_[k]) < 1e-9) { s_[k] = 0; c_[k] = 0; }
  }

private:
  double sampleRate_;
  Adsr amp_, pit_;
  double s_[4] = {}, c_[4] = {}, e_[4] = {}, r_[4] = {}, gain_[4] = {};
  int note_ = 60;
  double velocity_ = 1, strike_ = 0, strikeDecay_ = 0.9, lp_ = 0;
  bool hit_ = false;
  uint32_t noise_ = 0x9e3779b9u;
};

}  // namespace

void registerWoodblock(NodeRegistry& registry) {
  NodeType type;
  type.type = "woodblock";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.params = {
    {"tone", 100, 4000, 900},          // Hz of the lowest mode at the root note
    {"keytrack", 0, 1, 1, true},
    {"root", 0, 127, 60},
    {"inharm", 0, 1, 0.3},
    {"hardness", 0, 1, 0.6},
    {"damping", 5, 1500, 140},         // ms, lowest mode to -60 dB
    {"loss", 0, 1, 0.6},
    {"bend", 0, 24, 4},                // semitones the pitch envelope adds
    {"vel", 0, 1, 0.6},
    {"level", 0, 1, 0.7},
    {"attack", 0.1, 500, 0.1},
    {"decay", 1, 2000, 160},
    {"sustain", 0, 1, 0},
    {"release", 1, 2000, 40},
    {"patk", 0.1, 500, 0.1},
    {"pdec", 1, 2000, 22},
    {"psus", 0, 1, 0},
    {"prel", 1, 2000, 20},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Woodblock>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

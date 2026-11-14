// Hand clap, in the manner of the classic drum machines: noise through a band-pass, shaped by an envelope that is a
// short run of quick bursts (the individual hands, `bursts` of them every `spacing` ms, each dying in `burstDecay` ms)
// followed by one longer tail (`tailDecay` ms, `tailLevel` against the bursts) that starts with the last burst.
// `tone` and `q` set the band-pass, `spread` makes the two channels carry different noise for width, `drive` rounds
// the peaks. A note with any number is a hit; the velocity is the digit (1 to F). A new hit restarts the sound.
// Port of webtracker src/audio/nodes/clap.js.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kTone = 0, kQ, kBursts, kSpacing, kBurstDecay, kTailDecay, kTailLevel, kSpread, kDrive, kVel, kLevel };

constexpr double kPi = 3.141592653589793;

class Clap final : public Node {
public:
  explicit Clap(const NodeContext& context) : sampleRate_(context.sampleRate), ms_(context.sampleRate * 0.001) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      pend_ = clamp(event.vel, 0, 1);
      if (pend_ == 0) pend_ = 0.01;
    } else if (event.type == NoteType::Cut) {
      since_ = -1;
      burst_ = tail_ = 0;
      pend_ = 0;
    }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    const auto g = [&](Param p) { return static_cast<double>(params[slots[p]]); };
    if (pend_ > 0) { velocity_ = pend_; pend_ = 0; since_ = 0; next_ = 0; burst_ = tail_ = 0; }
    if (since_ < 0 && burst_ < 1e-5 && tail_ < 1e-5) {
      std::fill(outLeft, outLeft + n, 0.0f);
      std::fill(outRight, outRight + n, 0.0f);
      return;
    }
    const int bursts = jsRoundInt(g(kBursts));
    const double gap = g(kSpacing) * ms_;
    const double bDec = std::exp(-4.6 / (g(kBurstDecay) * ms_)), tDec = std::exp(-4.6 / (g(kTailDecay) * ms_)), tailLevel = g(kTailLevel);
    const double spread = g(kSpread), drive = 1 + g(kDrive) * 4, v = 1 - g(kVel) + g(kVel) * velocity_, level = g(kLevel) * v * 1.6;
    // a trapezoidal state-variable band-pass, unity gain at the centre
    const double f = std::tan(kPi * std::min(0.45, g(kTone) / sampleRate_)), k = 1 / g(kQ);
    const double a1 = 1 / (1 + f * (f + k)), a2 = f * a1, a3 = f * a2;
    const double rh = std::exp(-2 * kPi * 250 / sampleRate_);   // a gentle high-pass to keep rumble out
    const double driveNorm = std::tanh(drive);
    for (int i = 0; i < n; i++) {
      if (since_ >= 0) {
        if (next_ < bursts && since_ >= jsRound(next_ * gap)) {
          burst_ = 1;
          next_++;
          if (next_ == bursts) tail_ = 1;   // the tail starts with the last burst
        }
        since_++;
      }
      const double env = burst_ + tail_ * tailLevel;
      const double common = rand(2);
      for (int c = 0; c < 2; c++) {
        const double x = common * (1 - spread) + rand(c) * spread;
        const double v3 = x - ic2_[c], v1 = a1 * ic1_[c] + a2 * v3, v2 = ic2_[c] + a2 * ic1_[c] + a3 * v3;
        ic1_[c] = 2 * v1 - ic1_[c];
        ic2_[c] = 2 * v2 - ic2_[c];
        const double bp = v1 * k;
        const double y = bp - hpx_[c] + rh * hp_[c];
        hpx_[c] = bp;
        hp_[c] = y;
        const double s = std::tanh(y * env * drive) / driveNorm * level;
        (c ? outRight : outLeft)[i] = static_cast<float>(s);
      }
      burst_ *= bDec;
      tail_ *= tDec;
    }
    if (burst_ < 1e-6) burst_ = 0;
    if (tail_ < 1e-6) tail_ = 0;
    if (burst_ == 0 && tail_ == 0 && next_ >= bursts) since_ = -1;
    if (!std::isfinite(ic1_[0] + ic2_[0] + ic1_[1] + ic2_[1] + hp_[0] + hp_[1])) {
      ic1_[0] = ic1_[1] = ic2_[0] = ic2_[1] = hp_[0] = hp_[1] = hpx_[0] = hpx_[1] = 0;
    }
  }

private:
  // three streams of noise: one for each channel and one common to both, -1..1
  double rand(int k) {
    std::uint32_t s = seed_[k];
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    seed_[k] = s;
    return s / 2147483648.0 - 1;
  }

  double sampleRate_, ms_;
  double pend_ = 0, velocity_ = 0;
  long since_ = -1;   // samples since the hit, -1 when there is none
  int next_ = 0;      // the next burst to start
  double burst_ = 0, tail_ = 0;
  double hp_[2] = {0, 0}, hpx_[2] = {0, 0}, ic1_[2] = {0, 0}, ic2_[2] = {0, 0};
  std::uint32_t seed_[3] = {0x1234567, 0x7654321, 0x2468ace};
};

}  // namespace

void registerClap(NodeRegistry& registry) {
  NodeType type;
  type.type = "clap";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.shared = true;   // one clap for the instrument, never copied for each voice
  type.params = {
    {"tone", 300, 4000, 1200},      // Hz, centre of the band-pass
    {"q", 0.4, 8, 1.8},
    {"bursts", 1, 6, 4, true},
    {"spacing", 2, 30, 10},         // ms between the bursts
    {"burstDecay", 0.5, 25, 8},     // ms for a burst to fall by 40 dB
    {"tailDecay", 20, 800, 220},    // ms for the tail to fall by 40 dB
    {"tailLevel", 0, 1, 0.6},
    {"spread", 0, 1, 0.5},
    {"drive", 0, 1, 0.2},
    {"vel", 0, 1, 0.6},
    {"level", 0, 1, 0.7},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Clap>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

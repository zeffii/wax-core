// Hi-hat of the classic analogue drum machine kind, closed and open in one voice. The metal is six square waves at
// fixed, inharmonic frequencies, summed, then a band-pass and a high-pass (`tone`), then an amplitude envelope. Closed
// and open share that one bank and differ only in their decay. A note with number 0 is a closed hit and number 1 an
// open hit (the hat lane's Cl and Op columns send them); the velocity is the digit, 1 to F. A closed hit chokes the
// open hat with a plain 8 ms fade (`choke` off lets the open tail ring). When both hit on the same row the open hat
// starts and the closed one cuts it at once. `snap` adds a short bright click at the start of a hit.
// Monophonic: a new hit of a kind restarts that kind's envelope.
// Port of webtracker src/audio/nodes/hat808.js.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kTune = 0, kTone, kSnap, kLevel, kCdec, kOdec, kClvl, kOlvl, kChoke };

constexpr double kPi = 3.141592653589793;
constexpr double kBank[6] = {205.3, 304.4, 369.6, 522.7, 540, 800};
constexpr double kChokeMs = 8;

// rounds the corner of a square wave's edge, so it aliases less
double polyBlep(double t, double dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1; }
  if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
  return 0;
}

class Hat808 final : public Node {
public:
  explicit Hat808(const NodeContext& context) : sampleRate_(context.sampleRate) {
    for (int i = 0; i < 6; i++) phase_[i] = i / 6.0;
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      const double v = clamp(event.vel, 0, 1);
      if (event.note == 1) pendOpen_ = v; else pendClosed_ = v;
    } else if (event.type == NoteType::Cut) {
      closed_ = open_ = click_ = 0;
      choking_ = false;
      pendClosed_ = pendOpen_ = 0;
    }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    const auto g = [&](Param p) { return static_cast<double>(params[slots[p]]); };
    const double ratio = std::pow(2.0, g(kTune) / 12);
    const double snap = g(kSnap), level = g(kLevel) * 0.45;
    const double clvl = g(kClvl), olvl = g(kOlvl);
    const bool chokeOn = g(kChoke) >= 0.5;
    const double cDecay = std::exp(-4.6 / (sampleRate_ * g(kCdec) * 0.001));
    const double oDecay = std::exp(-4.6 / (sampleRate_ * g(kOdec) * 0.001));
    const double clickDecay = std::exp(-1 / (sampleRate_ * 0.0015));
    // events: the open hat starts first, so a closed hit on the same row cuts it
    if (pendOpen_ > 0) { open_ = pendOpen_; choking_ = false; click_ = std::max(click_, pendOpen_); pendOpen_ = 0; }
    if (pendClosed_ > 0) {
      closed_ = pendClosed_;
      click_ = std::max(click_, pendClosed_);
      pendClosed_ = 0;
      if (chokeOn && open_ > 0) { choking_ = true; chokeStep_ = open_ / (kChokeMs * 0.001 * sampleRate_); }
    }
    // band-pass around 9 kHz, high-pass at `tone`
    const double f = std::tan(kPi * std::min(0.45, 9000 / sampleRate_)), kq = 1 / 0.8;
    const double a1 = 1 / (1 + f * (f + kq)), a2 = f * a1, a3 = f * a2;
    const double rh = std::exp(-2 * kPi * std::min(g(kTone), sampleRate_ * 0.45) / sampleRate_);
    for (int i = 0; i < n; i++) {
      double metal = 0;
      for (int k = 0; k < 6; k++) {
        const double dt = std::min(0.45, kBank[k] * ratio / sampleRate_);
        double p = phase_[k] + dt;
        if (p >= 1) p -= 1;
        phase_[k] = p;
        metal += (p < 0.5 ? 1 : -1) + polyBlep(p, dt) - polyBlep(std::fmod(p + 0.5, 1.0), dt);
      }
      noise_ ^= noise_ << 13;
      noise_ ^= noise_ >> 17;
      noise_ ^= noise_ << 5;
      const double amp = closed_ * clvl + open_ * olvl;
      const double x = metal * amp + snap * click_ * (noise_ / 2147483648.0 - 1) * 3;
      const double v3 = x - ic2_, v1 = a1 * ic1_ + a2 * v3, v2 = ic2_ + a2 * ic1_ + a3 * v3;   // trapezoidal state-variable filter: stable at any cutoff
      ic1_ = 2 * v1 - ic1_;
      ic2_ = 2 * v2 - ic2_;
      const double band = v1;
      const double y1 = band - hp1x_ + rh * hp1_;
      hp1x_ = band;
      hp1_ = y1;
      const double y2 = y1 - hp2x_ + rh * hp2_;
      hp2x_ = y1;
      hp2_ = y2;
      const float s = static_cast<float>(y2 * level);
      outLeft[i] = s;
      outRight[i] = s;
      closed_ *= cDecay;
      click_ *= clickDecay;
      if (choking_) {
        open_ -= chokeStep_;
        if (open_ <= 0) { open_ = 0; choking_ = false; }
      } else {
        open_ *= oDecay;
      }
    }
    if (closed_ < 1e-6) closed_ = 0;
    if (open_ < 1e-6) open_ = 0;
    if (!std::isfinite(ic1_ + ic2_ + hp1_ + hp2_)) ic1_ = ic2_ = hp1_ = hp2_ = hp1x_ = hp2x_ = 0;
  }

private:
  double sampleRate_;
  double phase_[6];
  double closed_ = 0, open_ = 0, click_ = 0;   // the three envelopes
  bool choking_ = false;
  double chokeStep_ = 0;
  double pendClosed_ = 0, pendOpen_ = 0;
  std::uint32_t noise_ = 0x1234567;
  // band-pass (state-variable) then high-pass (two one-pole stages)
  double ic1_ = 0, ic2_ = 0, hp1_ = 0, hp1x_ = 0, hp2_ = 0, hp2x_ = 0;
};

}  // namespace

void registerHat808(NodeRegistry& registry) {
  NodeType type;
  type.type = "hat808";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.shared = true;   // one hat for the instrument, never copied for each voice
  type.params = {
    {"tune", -12, 12, 0},            // semitones, moves the whole bank
    {"tone", 3000, 12000, 7000},     // Hz, high-pass: higher is thinner
    {"snap", 0, 1, 0.3},
    {"level", 0, 1, 0.6},
    {"cdec", 10, 200, 45},           // ms for the closed hat to fall 40 dB
    {"odec", 100, 2000, 400},        // ms for the open hat to fall 40 dB
    {"clvl", 0, 1, 1},               // closed hat level
    {"olvl", 0, 1, 1},               // open hat level
    {"choke", 0, 1, 1, true},        // 1: a closed hit cuts the open hat
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Hat808>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// West-coast voice in one node: a principal oscillator bent by a modulation oscillator (FM, and hard sync), pushed
// through a wave folder, then through a pluck gate (a vactrol that opens the gain and a low-pass together). A timbre
// envelope moves the fold and the FM; a strike envelope lights the gate. `drift` lets the fold, the FM and the pitch
// wander a little, and makes each note differ slightly from the last. `quality` runs the oscillator and the folder at
// two or four times the rate. The same parts are also separate nodes (wavefolder, pluckgate) to patch by hand.
// Port of webtracker src/audio/nodes/westcoast.js. (A wave drawn while the song plays arrives as a node message: not ported yet.)
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/adsr.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"
#include "nodes/waves.h"
#include "nodes/westcore.h"

namespace wax {
namespace {

enum Param {
  kWave = 0, kTune, kRatio, kModTune, kModWave, kFm, kSync, kFold, kSymmetry, kFoldEnv, kFmEnv, kTatk, kTdec, kTsus, kTrel, kMode, kBright,
  kRes, kTrack, kLpgAtk, kLpgDec, kAtk, kDec, kSus, kRel, kGlide, kVel, kLevel, kQuality, kDrift
};

constexpr double kFmRadians = 12;

double triangle(double p) { return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4; }

class WestCoast final : public Node {
public:
  explicit WestCoast(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        pitchOn_(context.connected[0]),
        foldOn_(context.connected[1]),
        amp_(context.sampleRate),
        tim_(context.sampleRate),
        table_(normalizeWave(context.data && context.data->contains("wave") ? &(*context.data)["wave"] : nullptr)),
        gate_(context.sampleRate) {
    // each voice wanders its own way, the same every time the song is rendered
    const std::uint32_t made = static_cast<std::uint32_t>(context.nextInstance ? context.nextInstance("westcoast") : 1);
    seed_ = 0x2545f491u ^ (made * 0x9e3779b9u);
    if (seed_ == 0) seed_ = 1;
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      if (amp_.idle() && gate_.idle()) { reset(); pos_ = event.note; }
      note_ = event.note;
      velocity_ = event.vel;
      perNote_ = rand();
      amp_.on();
      tim_.on();
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) {
      amp_.off();
      tim_.off();
    } else {
      amp_.cut();
      tim_.cut();
      gate_.reset();
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    if (amp_.idle() && gate_.idle()) {
      std::fill(outLeft, outLeft + n, 0.0f);
      std::fill(outRight, outRight + n, 0.0f);
      return;
    }
    const auto g = [&](Param p) { return static_cast<double>(params[slots[p]]); };
    const float* pitchIn = inputs[0];
    const float* foldIn = inputs[1];
    amp_.set(g(kAtk), g(kDec), g(kSus), g(kRel));
    tim_.set(g(kTatk), g(kTdec), g(kTsus), g(kTrel));
    const double glideMs = g(kGlide);
    if (glideMs > 0.5) pos_ += (note_ - pos_) * (1 - js::exp(-n / (sampleRate_ * glideMs * 0.001 / 3)));
    else pos_ = note_;
    const int wave = jsRoundInt(g(kWave)), modwave = jsRoundInt(g(kModWave)), quality = jsRoundInt(g(kQuality));
    const bool sync = g(kSync) >= 0.5;
    const double drift = g(kDrift);
    until_ -= n;   // a new place to wander to about every 0.4 s, reached smoothly
    if (until_ <= 0) { tFold_ = rand(); tFm_ = rand(); tPitch_ = rand(); until_ = sampleRate_ * 0.4; }
    const double slide = 1 - js::exp(-n / (sampleRate_ * 0.25));
    dFold_ += (tFold_ - dFold_) * slide;
    dFm_ += (tFm_ - dFm_) * slide;
    dPitch_ += (tPitch_ - dPitch_) * slide;
    const double inc = 440 * js::pow(2.0, (pos_ - 69 + g(kTune) + drift * dPitch_ * 0.06) / 12) / sampleRate_;
    const double incMod = inc * g(kRatio) * js::pow(2.0, g(kModTune) / 1200);
    const double fm0 = g(kFm), fmenv = g(kFmEnv), fold0 = g(kFold), foldenv = g(kFoldEnv), sym = g(kSymmetry) * 0.6;
    gate_.set(jsRoundInt(g(kMode)), g(kBright), g(kRes), g(kLpgAtk), g(kLpgDec), (pos_ - 60) * g(kTrack));
    const double strike = 1 - g(kVel) + g(kVel) * velocity_, level = g(kLevel);
    const int steps = quality == 2 ? 4 : quality == 1 ? 2 : 1;
    const double step = 1.0 / steps;
    const bool high = steps > 1;
    const double foldDrift = drift * (0.22 * dFold_ + 0.12 * perNote_), fmDrift = drift * 0.2 * dFm_;
    for (int i = 0; i < n; i++) {
      const double a = amp_.next(), t = tim_.next();
      const double pitchMul = pitchOn_ ? js::pow(2.0, pitchIn[i]) : 1;
      const double idx = clamp(fm0 + fmDrift + fmenv * t, 0, 1);
      const double index = idx * idx * kFmRadians;
      const double amount = clamp(fold0 + foldDrift + foldenv * t + (foldOn_ ? foldIn[i] : 0), 0, 1);
      double first = 0, last = 0, sum = 0;
      for (int s = 0; s < steps; s++) {
        const double m = modwave == 0 ? js::sin(kTwoPi * pm_) : triangle(pm_);
        double x = p_ + (index * m) / kTwoPi;
        x -= std::floor(x);
        const double y = foldWave(principal(wave, x, inc * pitchMul * step), amount, sym);
        if (s == 0) first = y; else last = y;
        sum += y;
        pm_ += incMod * pitchMul * step;
        p_ += inc * pitchMul * step;
        if (pm_ >= 1) { pm_ -= std::floor(pm_); if (sync) p_ = 0; }
        if (p_ >= 1) p_ -= std::floor(p_);
      }
      // a short low-pass over the half-steps before they are dropped to the output rate
      const double raw = steps == 4 ? (prev_ + sum + last) / 6 : high ? (prev_ + 2 * first + last) * 0.25 : first;
      if (high) prev_ = last;
      const float out = static_cast<float>(gate_.run(dcb_.run(raw), a * strike) * level);
      outLeft[i] = out;
      outRight[i] = out;
    }
  }

private:
  void reset() {
    p_ = 0;
    pm_ = 0;
    prev_ = 0;
    gate_.reset();
    dcb_.reset();
  }

  // -1..1
  double rand() {
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return seed_ / 4294967296.0 * 2 - 1;
  }

  // the principal oscillator: 0 sine, 1 triangle, 2 saw, 3 square, 4 drawn
  double principal(int wave, double x, double dt) const {
    if (wave == 0) return js::sin(kTwoPi * x);
    if (wave == 1) return triangle(x);
    if (wave == 2) return 2 * x - 1 - polyBlep(x, dt);
    if (wave == 3) return (x < 0.5 ? 1 : -1) + polyBlep(x, dt) - polyBlep(x < 0.5 ? x + 0.5 : x - 0.5, dt);
    const double t = x * kWavePoints;
    const int j = static_cast<int>(t);
    const double f = t - j;
    const double a = table_[static_cast<std::size_t>(j & (kWavePoints - 1))], b = table_[static_cast<std::size_t>((j + 1) & (kWavePoints - 1))];
    return a + (b - a) * f;
  }

  double sampleRate_;
  bool pitchOn_, foldOn_;
  Adsr amp_, tim_;   // the strike envelope and the timbre envelope
  WaveTable table_;
  PluckGate gate_;
  DcBlock dcb_;
  double p_ = 0, pm_ = 0;   // the phases of the principal and the modulation oscillator
  double note_ = 60, pos_ = 60, velocity_ = 1, prev_ = 0;
  std::uint32_t seed_ = 1;
  // the wander: where the fold, the FM and the pitch are, where they are heading, and when they get a new place to go
  double dFold_ = 0, dFm_ = 0, dPitch_ = 0, tFold_ = 0, tFm_ = 0, tPitch_ = 0, until_ = 0, perNote_ = 0;
};

}  // namespace

void registerWestCoast(NodeRegistry& registry) {
  NodeType type;
  type.type = "westcoast";
  type.inputs = {"pitch", "fold"};   // pitch in octaves; fold adds to the fold amount
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"fold", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"wave", 0, 4, 0, true},          // 0 sine, 1 triangle, 2 saw, 3 square, 4 drawn
    {"tune", -48, 48, 0},             // semitones
    {"ratio", 0.25, 8, 2},            // modulation oscillator's pitch against the principal's
    {"modtune", -100, 100, 0},        // cents, fine tuning of the modulation oscillator
    {"modwave", 0, 1, 0, true},       // 0 sine, 1 triangle
    {"fm", 0, 1, 0.2},                // how far the modulation oscillator bends the principal
    {"sync", 0, 1, 0, true},          // 1: the principal restarts every cycle of the modulation oscillator
    {"fold", 0, 1, 0.3},              // how hard the wave is pushed into the wave folder
    {"symmetry", -1, 1, 0},           // moves the wave off centre before it is folded
    {"foldenv", -1, 1, 0.5},          // how much the timbre envelope adds to `fold`
    {"fmenv", -1, 1, 0.3},            // how much the timbre envelope adds to `fm`
    {"tatk", 0.1, 5000, 1},           // timbre envelope, ms
    {"tdec", 1, 5000, 400},
    {"tsus", 0, 1, 0},
    {"trel", 1, 5000, 300},
    {"mode", 0, 2, 0, true},          // 0 gain and filter, 1 gain only, 2 filter only
    {"bright", 0, 1, 0.6},            // the gate's highest cutoff
    {"res", 0, 0.9, 0.05},            // the gate filter's resonance
    {"track", 0, 1, 0.5},             // how far the cutoff follows the note's pitch
    {"lpgatk", 0.2, 200, 2},          // how fast the vactrol opens, ms
    {"lpgdec", 10, 5000, 300},        // how slowly it lets go, ms
    {"atk", 0.1, 5000, 1},            // strike envelope, ms
    {"dec", 1, 5000, 350},
    {"sus", 0, 1, 0},
    {"rel", 1, 5000, 200},
    {"glide", 0, 2000, 0},            // ms
    {"vel", 0, 1, 0.6},
    {"level", 0, 1, 0.6},
    {"quality", 0, 2, 1, true},       // 0 standard, 1 high (the oscillator and folder run at twice the rate), 2 ultra (four times)
    {"drift", 0, 1, 0.5},             // how much the fold, fm and pitch wander, and how much each note differs from the last
  };
  type.create = [](const NodeContext& context) { return std::make_unique<WestCoast>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

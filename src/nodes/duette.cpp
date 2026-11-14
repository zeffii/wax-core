// Duette: a monophonic two-oscillator analogue-style voice with a resonant filter, one envelope and one LFO.
//   VCO 1  saw, triangle or square; SHAPE adds wave-folded harmonics (saw, triangle) or narrows the pulse (square)
//   VCO 2  saw, triangle or noise, 16' to 2' and +-1200 cents; SHAPE likewise (on noise it darkens it); `mod` syncs it
//          to VCO 1 or ring-modulates it with VCO 1
//   mixer  the two levels; then a resonant two-pole low-pass filter that rings by itself, then DRIVE
//   EG     one attack/decay envelope that is also the amplifier: A/D (a note is a blip), A/G/D (held for the gate,
//          `decay` is the release) or GATE (the amplifier is the gate itself, the target still moves with attack and
//          decay). Its target is the pitch of both oscillators, of VCO 2 alone, or the cutoff, by `egint`
//   LFO    saw, triangle or square at FAST (0.5 Hz to 2.8 kHz), SLOW (0.05 to 28 Hz) or 1-SHOT (it stops after half a
//          cycle), on the pitch, the shapes or the cutoff
//   plus portamento (always, or only when notes overlap), a per-note slide (the Sld column), cutoff key tracking and velocity.
// With the instrument's Voices at 1 it is one voice. With more, every note gets its own copy (envelope, LFO and filter
// each), and a note column keeps to one voice: the glide and slide work along each column, and the columns together
// play chords. A slide keeps the gate up: the envelope does not restart and the pitch glides to the new note.
// Port of webtracker src/audio/nodes/duette.js.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param {
  kVco1Wave = 0, kVco1Shape, kVco2Wave, kVco2Oct, kVco2Pitch, kVco2Shape, kMod, kMix1, kMix2, kCutoff, kRes, kEgType, kAttack, kDecay,
  kEgInt, kEgTarget, kLfoWave, kLfoMode, kLfoRate, kLfoInt, kLfoTarget, kDrive, kPort, kPortMode, kSlide, kKeytrack, kCutVel, kAmpVel,
  kTune, kLevel, kSld
};
enum class Stage { Idle, Attack, Held, Decay };

constexpr double kOct[4] = {-12, 0, 12, 24};

// a triangle fold of any size into -1..1
double fold(double x) {
  x = jsMod(x + 1, 4);
  if (x < 0) x += 4;
  return (x < 2 ? x : 4 - x) - 1;
}

class Duette final : public Node {
public:
  explicit Duette(const NodeContext& context) : sr_(context.sampleRate), clock_(context.clock) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { pending_ = true; pendNote_ = event.note; pendVel_ = clamp(event.vel, 0, 1); }
    else if (event.type == NoteType::Off) { if (!event.implied && gate_) { gate_ = false; if (stage_ != Stage::Idle) stage_ = Stage::Decay; } }
    else if (event.type == NoteType::AllOff) { if (gate_) { gate_ = false; if (stage_ != Stage::Idle) stage_ = Stage::Decay; } }
    else { gate_ = false; pending_ = false; cutting_ = true; if (stage_ != Stage::Idle) stage_ = Stage::Decay; }   // a cut: a quick fade, not a step
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    const auto g = [&](Param p) { return static_cast<double>(params[slots[p]]); };
    const auto whole = [&](Param p) { return static_cast<int>(params[slots[p]]); };   // (the JS truncates these with | 0)
    const std::int64_t sStamp = clock_ ? clock_->stamps[static_cast<std::size_t>(slots[kSld])] : 0;
    const bool slideFlag = sStamp != lastS_ && (lastS_ != -1 || sStamp > 0) && g(kSld) >= 1;   // did this row write the flag?
    lastS_ = sStamp;
    const double egtype = g(kEgType), port = g(kPort);
    const bool portAlways = g(kPortMode) >= 1;
    if (pending_) {
      const bool legato = gate_ && active_;
      note_ = pendNote_;
      velocity_ = pendVel_;
      const double glide = slideFlag && legato ? g(kSlide) : port > 0 && (portAlways || legato) ? port : 0;
      if (glide > 0) target_ = note_; else pitch_ = target_ = note_;
      if (!(slideFlag && legato)) {
        stage_ = Stage::Attack;
        cutting_ = false;
        if (!lfoOn_ || g(kLfoMode) == 2) { lfoPh_ = 0; lfoDone_ = false; }
      }
      lfoOn_ = true;
      gate_ = true;
      active_ = true;
      pending_ = false;
      glideMs_ = glide;
    }
    if (!active_) {
      std::fill(outLeft, outLeft + n, 0.0f);
      std::fill(outRight, outRight + n, 0.0f);
      return;
    }
    const int v1w = whole(kVco1Wave), v2w = whole(kVco2Wave), mod = whole(kMod);
    const double sh1 = g(kVco1Shape), sh2 = g(kVco2Shape), mix1 = g(kMix1), mix2 = g(kMix2);
    const double cutoff = g(kCutoff), res = g(kRes), drive = g(kDrive), level = g(kLevel) * 0.8;
    const double egint = g(kEgInt);
    const int egtarget = whole(kEgTarget);
    const double egOct = sign(egint) * egint * egint * 5;
    const int lwave = whole(kLfoWave), lmode = whole(kLfoMode), ltarget = whole(kLfoTarget);
    const double lint = g(kLfoInt);
    const double r0 = lmode == 0 ? 0.5 : 0.05, r1 = lmode == 0 ? 2800 : 28;   // fast, or slow
    const double lfoHz = r0 * js::pow(r1 / r0, g(kLfoRate));
    const double atk = std::max(1e-4, g(kAttack) * 0.001 * sr_);
    const double decCoef = js::exp(-6.9078 / (sr_ * (cutting_ ? 4 : g(kDecay)) * 0.001));
    const double glideCoef = glideMs_ > 0 ? 1 - js::exp(-3 / (sr_ * glideMs_ * 0.001)) : 1;
    const double tune = g(kTune), key = g(kKeytrack), cutvel = g(kCutVel), ampvel = g(kAmpVel);
    const double velGain = 1 - ampvel + ampvel * velocity_, driveGain = 1 + drive * 9, driveNorm = std::sqrt(driveGain);
    const double kq = 2 - 1.96 * res;   // the filter's damping: 2 is none, near 0 rings by itself
    const double vco2Offset = kOct[std::max(0, std::min(3, whole(kVco2Oct)))], vco2Cents = g(kVco2Pitch);
    double inc1 = 0, inc2 = 0, gg = 0, a1 = 0, lfoV = 0;
    const double lfoStep = lfoHz / sr_;
    for (int i = 0; i < n; i++) {
      // the LFO
      if (!(lmode == 2 && lfoDone_)) {
        lfoPh_ += lfoStep;
        if (lmode == 2 ? lfoPh_ >= 0.5 : lfoPh_ >= 1) {
          if (lmode == 2) { lfoPh_ = 0.5; lfoDone_ = true; } else lfoPh_ -= 1;
        }
      }
      lfoV = lwave == 0 ? 1 - 2 * lfoPh_ : lwave == 1 ? (lfoPh_ < 0.5 ? 4 * lfoPh_ - 1 : 3 - 4 * lfoPh_) : (lfoPh_ < 0.5 ? 1 : -1);
      // the envelope: it is the amplifier too
      if (stage_ == Stage::Attack) {
        eg_ += 1 / atk;
        if (eg_ >= 1) { eg_ = 1; stage_ = egtype == 1 ? Stage::Held : Stage::Decay; if (egtype == 1 && !gate_) stage_ = Stage::Decay; }
      } else if (stage_ == Stage::Decay) {
        eg_ *= decCoef;
        if (eg_ < 1e-4) { eg_ = 0; stage_ = Stage::Idle; }
      } else if (stage_ == Stage::Held && !gate_) {
        stage_ = Stage::Decay;
      }
      if (egtype == 2) { gateEnv_ += ((gate_ ? 1 : 0) - gateEnv_) * 0.01; vca_ = gateEnv_; } else vca_ = eg_;
      // the pitch, then the coefficients, every 8 samples
      if ((i & 7) == 0) {
        pitch_ += (target_ - pitch_) * (glideCoef == 1 ? 1 : 1 - js::pow(1 - glideCoef, 8));
        if (std::abs(target_ - pitch_) < 1e-3) pitch_ = target_;
        const double lfoSemis = ltarget == 0 ? lfoV * lint * lint * 24 : 0, egSemis = egtarget == 0 ? egOct * eg_ * 12 : 0, egSemis2 = egtarget == 1 ? egOct * eg_ * 12 : 0;
        inc1 = 440 * js::pow(2.0, (pitch_ + tune + lfoSemis + egSemis - 69) / 12) / sr_;
        inc2 = 440 * js::pow(2.0, (pitch_ + tune + vco2Offset + vco2Cents / 100 + lfoSemis + egSemis + egSemis2 - 69) / 12) / sr_;
        const double octs = (egtarget == 2 ? egOct * eg_ : 0) + (ltarget == 2 ? lfoV * lint * lint * 4 : 0) + key * (pitch_ - 60) / 12 + cutvel * (velocity_ - 0.5) * 3;
        const double fc = std::min(0.45 * sr_, std::max(20.0, cutoff * js::pow(2.0, octs)));
        gg = js::tan(kPi * fc / sr_);
        a1 = 1 / (1 + gg * (gg + kq));
      }
      const double shapeMod = ltarget == 1 ? lfoV * lint * 0.5 : 0;
      const double s1v = clamp(sh1 + shapeMod, 0, 1), s2v = clamp(sh2 + shapeMod, 0, 1);
      // VCO 1
      ph1_ += inc1;
      bool wrapped = false;
      if (ph1_ >= 1) { ph1_ -= 1; wrapped = true; }
      const double damp1 = 1 - std::min(1.0, inc1 * 14);   // folding aliases, so it fades out towards the top of the keyboard
      double o1;
      if (v1w == 0) o1 = fold((2 * ph1_ - 1 - polyBlep(ph1_, inc1)) * (1 + 4 * s1v * damp1));
      else if (v1w == 1) o1 = fold((ph1_ < 0.5 ? 4 * ph1_ - 1 : 3 - 4 * ph1_) * (1 + 4 * s1v * damp1));
      else { const double duty = 0.5 - 0.45 * s1v; o1 = (ph1_ < duty ? 1 : -1) + polyBlep(ph1_, inc1) - polyBlep(jsMod(ph1_ + 1 - duty, 1), inc1); }
      // VCO 2
      if (mod == 1 && wrapped) ph2_ = jsMod(ph1_ * (inc2 / inc1), 1);   // sync: it restarts with VCO 1
      else { ph2_ += inc2; if (ph2_ >= 1) ph2_ -= 1; }
      const double damp2 = 1 - std::min(1.0, inc2 * 14);
      double o2;
      if (v2w == 0) o2 = fold((2 * ph2_ - 1 - polyBlep(ph2_, inc2)) * (1 + 4 * s2v * damp2));
      else if (v2w == 1) o2 = fold((ph2_ < 0.5 ? 4 * ph2_ - 1 : 3 - 4 * ph2_) * (1 + 4 * s2v * damp2));
      else { const double w = noise(), k = 0.05 + 0.95 * (1 - s2v) * (1 - s2v); nz_ += (w - nz_) * k; o2 = nz_ * (0.6 + 0.4 * s2v) * 1.4; }
      if (mod == 2) o2 *= o1;
      const double x = (o1 * mix1 + o2 * mix2) * 0.9;
      // the filter: a two-pole state-variable low-pass, with soft limits on its states so that it rings but does not run away
      const double xin = softClip(x * 1.4);
      const double hp = (xin - (kq + gg) * s1_ - s2_) * a1;
      const double bp = gg * hp + s1_, lp = gg * bp + s2_;
      s1_ = softClip(gg * hp + bp);
      s2_ = softClip(gg * bp + lp);
      double y = lp;
      y = drive > 0 ? softClip(y * driveGain) / driveNorm * 1.3 : y;
      const double o = y * vca_ * velGain;
      const double r = o - dcx_ + 0.995 * dc_;
      dcx_ = o;
      dc_ = r;
      outLeft[i] = outRight[i] = static_cast<float>(r * level);
    }
    if (!std::isfinite(dc_ + dcx_ + s1_ + s2_)) s1_ = s2_ = dc_ = dcx_ = 0;
    if (stage_ == Stage::Idle && !gate_ && vca_ < 1e-5 && std::abs(gateEnv_) < 1e-5) { active_ = false; s1_ = s2_ = dc_ = dcx_ = 0; }
  }

private:
  // noise, -1..1
  double noise() {
    seed_ = seed_ * 1664525u + 1013904223u;
    return seed_ / 2147483648.0 - 1;
  }

  double sr_;
  const Clock* clock_;
  double ph1_ = 0, ph2_ = 0, pitch_ = 48, target_ = 48, note_ = 48;
  bool gate_ = false, active_ = false;
  bool pending_ = false;   // a note is waiting for the next block
  int pendNote_ = 0;
  double pendVel_ = 0;
  double velocity_ = 1;
  Stage stage_ = Stage::Idle;
  double eg_ = 0, vca_ = 0, gateEnv_ = 0;
  double lfoPh_ = 0;
  bool lfoDone_ = false, lfoOn_ = false;
  double s1_ = 0, s2_ = 0, dc_ = 0, dcx_ = 0;
  std::uint32_t seed_ = 22222;
  double nz_ = 0;
  std::int64_t lastS_ = -1;   // how often the Sld column had been written at the last block
  bool cutting_ = false;
  double glideMs_ = 0;        // the glide of the note now sounding
};

}  // namespace

void registerDuette(NodeRegistry& registry) {
  NodeType type;
  type.type = "duette";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.columnVoice = true;   // with several voices, a note column keeps to one voice, so the glide and slide work per column
  type.params = {
    {"vco1wave", 0, 2, 0, true},        // 0 saw, 1 triangle, 2 square
    {"vco1shape", 0, 1, 0},
    {"vco2wave", 0, 2, 0, true},        // 0 saw, 1 triangle, 2 noise
    {"vco2oct", 0, 3, 1, true},         // 16', 8', 4', 2'
    {"vco2pitch", -1200, 1200, 6},      // cents
    {"vco2shape", 0, 1, 0},
    {"mod", 0, 2, 0, true},             // 0 off, 1 sync, 2 ring
    {"mix1", 0, 1, 0.8},
    {"mix2", 0, 1, 0.7},
    {"cutoff", 30, 18000, 1800},        // Hz, before the envelope and LFO
    {"res", 0, 1, 0.35},
    {"egtype", 0, 2, 0, true},          // 0 A/D, 1 A/G/D, 2 GATE
    {"attack", 1, 3000, 2},             // ms
    {"decay", 5, 5000, 380},            // ms to fall 60 dB; the release in A/G/D
    {"egint", -1, 1, 0.45},
    {"egtarget", 0, 2, 2, true},        // 0 pitch, 1 pitch 2, 2 cutoff
    {"lfowave", 0, 2, 1, true},         // 0 saw, 1 triangle, 2 square
    {"lfomode", 0, 2, 1, true},         // 0 fast, 1 slow, 2 1-shot
    {"lforate", 0, 1, 0.45},
    {"lfoint", 0, 1, 0},
    {"lfotarget", 0, 2, 2, true},       // 0 pitch, 1 shape, 2 cutoff
    {"drive", 0, 1, 0.2},
    {"port", 0, 1000, 0},               // ms, 0 is off
    {"portmode", 0, 1, 0, true},        // 0 auto (legato only), 1 always
    {"slide", 1, 500, 60},              // ms, for a note with the Sld flag
    {"keytrack", 0, 1, 0},
    {"cutvel", 0, 1, 0},
    {"ampvel", 0, 1, 0.3},
    {"tune", -48, 48, 0},               // semitones
    {"level", 0, 1, 0.7},
    {"sld", 0, 1, 0, true},             // the Sld column: 1 slides into the note
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Duette>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

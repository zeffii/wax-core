// Snarl: a monophonic, semi-modular voice built around two resonant filters in a row (two oscillators, a high-pass and a low-pass
// each with its own peak, two envelopes, a modulation generator, portamento). Port of webtracker src/audio/nodes/snarl.js.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param {
  kVco1wave = 0, kVco1oct, kVco1pw, kVco2wave, kVco2oct, kVco2pw, kVco2pitch, kDetune, kMix1, kMix2, kNoise, kHpcut, kHppeak, kLpcut, kLppeak, kEdge,
  kEg1delay, kEg1attack, kEg1release, kEg1pitch, kEg1hp, kEg1lp, kAttack, kHold, kDecay, kSustain, kRelease,
  kMgwave, kMgrate, kMgpitch, kMghp, kMglp, kMgamp, kPort, kKeytrack, kVel, kTune, kLevel
};
enum State { Idle = 0, Delay, Attack, Hold, Decay, Sustain, Release };
const double kOct[4] = {-24, -12, 0, 12};

double polyBlep(double t, double dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1; }
  if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
  return 0;
}
double clip(double x) { return x > 3 ? 1 : x < -3 ? -1 : x * (27 + x * x) / (27 + 9 * x * x); }
double signedSquare(double x) { const double s = x > 0 ? 1.0 : (x < 0 ? -1.0 : x); return s * x * x; }

class Snarl final : public Node {
public:
  explicit Snarl(const NodeContext& context) : sr_(context.sampleRate) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      pend_ = true; pendNote_ = event.note; pendVel_ = std::max(0.0, std::min(1.0, event.vel)); pendOff_ = false;
      heldNote_ = event.note;
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) {
      if (event.implied) return;
      if (event.type == NoteType::Off && event.note != heldNote_) return;
      if (pend_) pendOff_ = true;
      else if (gate_) { gate_ = false; if (st1_ != Idle) st1_ = Release; if (st2_ != Idle) st2_ = Release; }
    } else {
      gate_ = false; pend_ = false; active_ = false; e1_ = e2_ = 0; st1_ = st2_ = Idle;
      f1_[0] = f1_[1] = f2_[0] = f2_[1] = 0;
    }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    auto g = [&](int k) { return static_cast<double>(params[slots[k]]); };
    const double sr = sr_;
    auto ms = [&](double v) { return std::max(1.0, v * 0.001 * sr); };
    if (pend_) {
      const bool legato = gate_ && active_;
      const double port = g(kPort);
      velocity_ = pendVel_;
      if (legato && port > 0) { target_ = pendNote_; glideMs_ = port; } else { pitch_ = target_ = pendNote_; glideMs_ = 0; }
      st1_ = Delay; wait1_ = ms(g(kEg1delay)) * (g(kEg1delay) > 0 ? 1 : 0); st2_ = Attack;
      gate_ = !pendOff_; active_ = true; pend_ = false;
    }
    if (!active_) {
      std::fill(outL, outL + n, 0.0f);
      std::fill(outR, outR + n, 0.0f);
      return;
    }
    const int v1w = static_cast<int>(g(kVco1wave)), v2w = static_cast<int>(g(kVco2wave));
    const double pw1 = 0.5 - 0.45 * g(kVco1pw), pw2 = 0.5 - 0.45 * g(kVco2pw);
    const double mix1 = g(kMix1), mix2 = g(kMix2), noiseMix = g(kNoise);
    const double hpPeak = g(kHppeak), lpPeak = g(kLppeak), edge = 1 + g(kEdge) * 5;
    const double kqH = 2 - 1.96 * hpPeak, kqL = 2 - 1.96 * lpPeak;
    const double eg1pitch = signedSquare(g(kEg1pitch)) * 24, eg1hp = signedSquare(g(kEg1hp)) * 5, eg1lp = signedSquare(g(kEg1lp)) * 5;
    const double mgPitch = js::pow(g(kMgpitch), 2) * 12, mgHp = js::pow(g(kMghp), 2) * 4, mgLp = js::pow(g(kMglp), 2) * 4, mgAmp = g(kMgamp);
    const int mgWave = static_cast<int>(g(kMgwave));
    const double mgStep = g(kMgrate) / sr;
    const double atk1 = ms(g(kEg1attack)), rel1 = js::exp(-6.9078 / ms(g(kEg1release)));
    const double atk2 = ms(g(kAttack)), hold2 = ms(g(kHold)), dec2 = js::exp(-6.9078 / ms(g(kDecay))), sus = g(kSustain), rel2 = js::exp(-6.9078 / ms(g(kRelease)));
    const double key = g(kKeytrack), velGain = 1 - g(kVel) + g(kVel) * velocity_, tune = g(kTune);
    const double o1Shift = kOct[static_cast<int>(g(kVco1oct))], o2Shift = kOct[static_cast<int>(g(kVco2oct))] + g(kVco2pitch) + g(kDetune) / 100;
    const double glideCoef = glideMs_ > 0 ? 1 - js::exp(-3 / (sr * glideMs_ * 0.001)) : 1, level = g(kLevel) * 0.8;
    double inc1 = 0, inc2 = 0, gH = 0, gL = 0, aH1 = 0, aL1 = 0, mg = 0;
    for (int i = 0; i < n; i++) {
      mgPh_ += mgStep;
      if (mgPh_ >= 1) { mgPh_ -= 1; held_ = rnd(); }
      mg = mgWave == 0 ? (mgPh_ < 0.5 ? 4 * mgPh_ - 1 : 3 - 4 * mgPh_) : mgWave == 1 ? 1 - 2 * mgPh_ : mgWave == 2 ? 2 * mgPh_ - 1 : mgWave == 3 ? (mgPh_ < 0.5 ? 1 : -1) : held_;
      if (!gate_) { if (st2_ != Release && st2_ != Idle) st2_ = Release; if (st1_ == Sustain) st1_ = Release; }
      if (st1_ == Delay) { if (--wait1_ <= 0) st1_ = Attack; }
      else if (st1_ == Attack) { e1_ += 1 / atk1; if (e1_ >= 1) { e1_ = 1; st1_ = gate_ ? Sustain : Release; } }
      else if (st1_ == Release) { e1_ *= rel1; if (e1_ < 1e-4) { e1_ = 0; st1_ = Idle; } }
      if (st2_ == Attack) { e2_ += 1 / atk2; if (e2_ >= 1) { e2_ = 1; wait2_ = hold2; st2_ = Hold; } }
      else if (st2_ == Hold) { if (--wait2_ <= 0) st2_ = Decay; }
      else if (st2_ == Decay) { e2_ = sus + (e2_ - sus) * dec2; if (std::fabs(e2_ - sus) < 1e-4) st2_ = Sustain; }
      else if (st2_ == Release) { e2_ *= rel2; if (e2_ < 1e-4) { e2_ = 0; st2_ = Idle; } }
      if ((i & 7) == 0) {
        pitch_ += (target_ - pitch_) * (glideCoef == 1 ? 1 : 1 - js::pow(1 - glideCoef, 8));
        if (std::fabs(target_ - pitch_) < 1e-3) pitch_ = target_;
        const double semis = pitch_ + tune + eg1pitch * e1_ + mg * mgPitch;
        inc1 = 440 * js::pow(2, (semis + o1Shift - 69) / 12) / sr;
        inc2 = 440 * js::pow(2, (semis + o2Shift - 69) / 12) / sr;
        const double track = key * (pitch_ - 60) / 12;
        const double fcH = std::min(0.45 * sr, std::max(15.0, g(kHpcut) * js::pow(2, eg1hp * e1_ + mg * mgHp + track)));
        const double fcL = std::min(0.45 * sr, std::max(20.0, g(kLpcut) * js::pow(2, eg1lp * e1_ + mg * mgLp + track)));
        gH = js::tan(kPi * fcH / sr); aH1 = 1 / (1 + gH * (gH + kqH));
        gL = js::tan(kPi * fcL / sr); aL1 = 1 / (1 + gL * (gL + kqL));
      }
      ph1_ += inc1; if (ph1_ >= 1) ph1_ -= 1;
      double o1;
      if (v1w == 0) o1 = 2 * ph1_ - 1 - polyBlep(ph1_, inc1);
      else if (v1w == 1) o1 = ph1_ < 0.5 ? 4 * ph1_ - 1 : 3 - 4 * ph1_;
      else if (v1w == 2) o1 = (ph1_ < pw1 ? 1 : -1) + polyBlep(ph1_, inc1) - polyBlep(std::fmod(ph1_ + 1 - pw1, 1.0), inc1);
      else o1 = rnd();
      ph2_ += inc2; if (ph2_ >= 1) ph2_ -= 1;
      double o2;
      if (v2w == 0) o2 = 2 * ph2_ - 1 - polyBlep(ph2_, inc2);
      else if (v2w == 2) o2 = (ph2_ < pw2 ? 1 : -1) + polyBlep(ph2_, inc2) - polyBlep(std::fmod(ph2_ + 1 - pw2, 1.0), inc2);
      else o2 = (ph2_ < 0.5 ? 1 : -1) + polyBlep(ph2_, inc2) - polyBlep(std::fmod(ph2_ + 0.5, 1.0), inc2);
      if (v2w == 3) o2 *= o1;
      const double nz = rnd();
      const double x = (o1 * mix1 + o2 * mix2 + nz * noiseMix) * 0.8;
      const double hIn = clip(x * 1.4);
      const double hh = (hIn - (kqH + gH) * f1_[0] - f1_[1]) * aH1, hb = gH * hh + f1_[0], hl = gH * hb + f1_[1];
      f1_[0] = clip(gH * hh + hb); f1_[1] = clip(gH * hb + hl);
      const double lIn = clip(hh * edge);
      const double lh = (lIn - (kqL + gL) * f2_[0] - f2_[1]) * aL1, lb = gL * lh + f2_[0], ll = gL * lb + f2_[1];
      f2_[0] = clip(gL * lh + lb); f2_[1] = clip(gL * lb + ll);
      const double y = ll / std::sqrt(edge) * 1.2;
      const double o = y * e2_ * velGain * (1 - mgAmp * 0.5 * (1 + mg));
      const double r = o - dcx_ + 0.995 * dc_;
      dcx_ = o; dc_ = r;
      outL[i] = outR[i] = static_cast<float>(r * level);
    }
    if (!std::isfinite(dc_ + dcx_ + f1_[0] + f1_[1] + f2_[0] + f2_[1])) { f1_[0] = f1_[1] = f2_[0] = f2_[1] = 0; dc_ = dcx_ = 0; }
    if (st2_ == Idle) { active_ = false; f1_[0] = f1_[1] = f2_[0] = f2_[1] = 0; dc_ = dcx_ = 0; }
  }

private:
  double rnd() { seed_ = seed_ * 1664525u + 1013904223u; return seed_ / 2147483648.0 - 1; }

  double sr_;
  int heldNote_ = -1;
  double ph1_ = 0, ph2_ = 0, pitch_ = 48, target_ = 48;
  bool gate_ = false, active_ = false;
  bool pend_ = false, pendOff_ = false;
  int pendNote_ = 0;
  double pendVel_ = 1, velocity_ = 1, glideMs_ = 0;
  double e1_ = 0, wait1_ = 0, e2_ = 0, wait2_ = 0;
  int st1_ = Idle, st2_ = Idle;
  double mgPh_ = 0, held_ = 0, dc_ = 0, dcx_ = 0;
  std::uint32_t seed_ = 7777;
  double f1_[2] = {0, 0}, f2_[2] = {0, 0};
};

}  // namespace

void registerSnarl(NodeRegistry& registry) {
  NodeType type;
  type.type = "snarl";
  type.inputs = {};
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.params = {
    {"vco1wave", 0, 3, 0, true}, {"vco1oct", 0, 3, 2, true}, {"vco1pw", 0, 1, 0.3}, {"vco2wave", 0, 3, 0, true}, {"vco2oct", 0, 3, 2, true}, {"vco2pw", 0, 1, 0.3},
    {"vco2pitch", -12, 12, 0}, {"detune", -100, 100, 8}, {"mix1", 0, 1, 0.8}, {"mix2", 0, 1, 0.6}, {"noise", 0, 1, 0}, {"hpcut", 20, 8000, 60}, {"hppeak", 0, 1, 0.2},
    {"lpcut", 30, 18000, 1400}, {"lppeak", 0, 1, 0.5}, {"edge", 0, 1, 0.3}, {"eg1delay", 0, 2000, 0}, {"eg1attack", 1, 3000, 5}, {"eg1release", 5, 5000, 300},
    {"eg1pitch", -1, 1, 0}, {"eg1hp", -1, 1, 0}, {"eg1lp", -1, 1, 0.4}, {"attack", 1, 3000, 3}, {"hold", 0, 2000, 0}, {"decay", 5, 5000, 400}, {"sustain", 0, 1, 0.7},
    {"release", 5, 5000, 180}, {"mgwave", 0, 4, 0, true}, {"mgrate", 0.05, 60, 4}, {"mgpitch", 0, 1, 0}, {"mghp", 0, 1, 0}, {"mglp", 0, 1, 0}, {"mgamp", 0, 1, 0},
    {"port", 0, 1000, 0}, {"keytrack", 0, 1, 0.3}, {"vel", 0, 1, 0.3}, {"tune", -48, 48, 0}, {"level", 0, 1, 0.7},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Snarl>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

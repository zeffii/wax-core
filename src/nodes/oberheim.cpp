// Oberheim-style synth voice: two oscillators (saw, pulse with pulse-width modulation, triangle), the second optionally
// hard-synced to the first with a filter-envelope sweep on its pitch, into a state-variable multimode filter after the
// SEM: 12 dB per octave, and one `mode` knob sweeps it from lowpass through notch to highpass (with a bandpass blend on
// top). Filter and amplitude each have an ADSR. A slow analog drift keeps the two oscillators and the cutoff wandering a
// little, each note lands at a random spot in the stereo field (`scatter`), and a vibrato LFO is built in. Monophonic,
// last note wins, with glide; the voices node makes it a polyphonic synth, each voice with its own drift.
// Port of webtracker src/audio/nodes/oberheim.js.
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
  kWave1 = 0, kWave2, kPw, kPwm, kPwmRate, kOsc2, kDetune, kTune2, kSync, kSweep, kNoise, kCutoff, kRes, kMode, kBp,
  kDrive, kFenv, kKeytrack, kFatk, kFdec, kFsus, kFrel, kAtk, kDec, kSus, kRel, kGlide, kVib, kVrate, kDrift,
  kScatter, kVel, kLevel
};

constexpr int kSubBlock = 8;
constexpr double kTwoPi2 = 2 * kPi;

// the polyBLEP correction for the sample just after a step, t in 0..1 samples since it
double B(double t) { return t * 2 - t * t - 1; }

double blep(double t, double dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1; }
  if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
  return 0;
}

double osc(int wave, double p, double dt, double pw) {
  if (wave == 0) return 2 * p - 1 - blep(p, dt);
  if (wave == 1) {
    double q = p + 1 - pw;
    if (q >= 1) q -= 1;
    return (p < pw ? 1 : -1) + blep(p, dt) - blep(q, dt);
  }
  return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
}

double sat(double z) { return z > 3 ? 1 : z < -3 ? -1 : z * (27 + z * z) / (27 + 9 * z * z); }

class Oberheim final : public Node {
public:
  explicit Oberheim(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        pitchOn_(context.connected[0]),
        cutOn_(context.connected[1]),
        amp_(context.sampleRate),
        fil_(context.sampleRate) {
    const std::uint32_t made = static_cast<std::uint32_t>(context.nextInstance ? context.nextInstance("oberheim") : 1);
    seed_ = 0x9e3779b9u * made;
    if (seed_ == 0) seed_ = 1;
    pwmPh_ = rand() * 0.5 + 0.5;
    driftC_ = 1 - js::exp(-kTwoPi2 * 0.6 / (sampleRate_ / kSubBlock));
    driftGain_ = std::sqrt((2 - driftC_) / driftC_) * 0.577;
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      if (amp_.idle()) { p1_ = 0; p2_ = 0.37; s1_ = s2_ = 0; pos_ = event.note; }
      note_ = event.note;
      velocity_ = event.vel;
      off2_ = rand();   // each note comes out a little different
      pan_ = rand();
      amp_.on();
      fil_.on();
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) {
      amp_.off();
      fil_.off();
    } else {
      amp_.cut();
      fil_.cut();
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (amp_.idle()) {
      std::fill(outL, outL + n, 0.0f);
      std::fill(outR, outR + n, 0.0f);
      return;
    }
    const auto g = [&](Param p) { return static_cast<double>(params[slots[p]]); };
    const float* pitchIn = inputs[0];
    const float* cutIn = inputs[1];
    const double sr = sampleRate_;
    amp_.set(g(kAtk), g(kDec), g(kSus), g(kRel));
    fil_.set(g(kFatk), g(kFdec), g(kFsus), g(kFrel));
    const double glideMs = g(kGlide);
    if (glideMs > 0.5) pos_ += (note_ - pos_) * (1 - js::exp(-n / (sr * glideMs * 0.001 / 3)));
    else pos_ = note_;
    const int w1 = jsRoundInt(g(kWave1)), w2 = jsRoundInt(g(kWave2));
    const double pwBase = g(kPw), pwm = g(kPwm);
    const double lvl2 = g(kOsc2), noise = g(kNoise), sweep = g(kSweep), drift = g(kDrift);
    const bool sync = g(kSync) >= 0.5;
    const double detune = g(kDetune), tune2 = g(kTune2) * 100, vib = g(kVib);
    const double pwmInc = g(kPwmRate) / sr, vibInc = g(kVrate) / sr;
    const double baseHz = 440 * js::pow(2, (pos_ - 69) / 12);
    const double cutoff = g(kCutoff), res = g(kRes), drive = 1 + g(kDrive) * 4, mode = g(kMode), bpm = g(kBp);
    const double R = 1 - res * 0.97;   // damping: 1 is no resonance
    const double fenv = g(kFenv), keyOct = (g(kKeytrack) * (pos_ - 60)) / 12;
    const double level = g(kLevel) * (1 - g(kVel) + g(kVel) * velocity_) * 1.2 * (1 - res * 0.25);
    const double nyq = sr * 0.45;
    const double scatter = g(kScatter) * pan_, a = (scatter + 1) * kPi / 4;
    const double gl = js::cos(a) * 1.4142135623730951, gr = js::sin(a) * 1.4142135623730951;
    double G = 0, den = 1, inc1 = 0, inc2 = 0, pw = pwBase;
    for (int i = 0; i < n; i++) {
      const double av = amp_.next(), f = fil_.next();
      if ((i & (kSubBlock - 1)) == 0) {
        d1_ += (rand() - d1_) * driftC_;
        d2_ += (rand() - d2_) * driftC_;
        d3_ += (rand() - d3_) * driftC_;
        pwmPh_ += pwmInc * kSubBlock; if (pwmPh_ >= 1) pwmPh_ -= 1;
        vibPh_ += vibInc * kSubBlock; if (vibPh_ >= 1) vibPh_ -= 1;
        const double v = vib * js::sin(kTwoPi2 * vibPh_);
        const double octs = pitchOn_ ? pitchIn[i] : 0;
        inc1 = baseHz * js::pow(2, octs + (v + drift * 6 * driftGain_ * d1_ + off2_ * drift * 3) / 1200) / sr;
        inc2 = baseHz * js::pow(2, octs + (v + detune + tune2 + sweep * 100 * f + drift * 6 * driftGain_ * d2_ - off2_ * drift * 3) / 1200) / sr;
        pw = std::max(0.05, std::min(0.95, pwBase + pwm * js::sin(kTwoPi2 * pwmPh_)));
        const double oct = fenv * f + keyOct + (cutOn_ ? static_cast<double>(cutIn[i]) : 0.0) + drift * 0.08 * driftGain_ * d3_;
        const double hz = std::min(nyq, std::max(10.0, cutoff * js::pow(2, oct)));
        G = js::tan((kPi * hz) / sr);
        den = 1 / (1 + 2 * R * G + G * G);
      }
      double v = osc(w1, p1_, inc1, pw);
      p1_ += inc1;
      bool wrapped = false;
      if (p1_ >= 1) { p1_ -= 1; wrapped = true; }
      if (lvl2 > 0) {
        double o2 = osc(w2, p2_, inc2, pw);
        p2_ += inc2;
        if (p2_ >= 1) p2_ -= 1;
        if (sync && wrapped) {   // oscillator 1 restarts: oscillator 2 jumps back with it, softened by a polyBLEP
          const double t = p1_ / inc1;   // how far into this sample the restart happened
          const double before = o2, np = t * inc2;
          p2_ = np;
          o2 = osc(w2, p2_, inc2, pw);
          if (w2 != 2) o2 += ((before - o2) * -0.5) * B(std::min(1.0, t));
        }
        v += lvl2 * o2;
      }
      if (noise > 0) {
        noiseSeed_ = static_cast<std::uint32_t>(static_cast<std::uint64_t>(noiseSeed_) * 1664525u + 1013904223u);
        v += noise * (noiseSeed_ / 2147483648.0 - 1);
      }
      // state variable filter (zero delay feedback), left and right states are the same signal so one is enough
      const double x = sat(v * 0.6 * drive);
      const double hp = (x - (2 * R + G) * s1_ - s2_) * den;
      const double bp = G * hp + s1_, lp = G * bp + s2_;
      s1_ = 2 * sat((G * hp + bp) * 0.5);
      s2_ = G * bp + lp;
      if (std::abs(s1_) < 1e-18) s1_ = 0;
      if (std::abs(s2_) < 1e-18) s2_ = 0;
      double y = (1 - mode) * lp + mode * hp;
      y = y * (1 - bpm) + bpm * bp * (0.5 + R);
      const double o = y * av * level / drive * 2.2;
      outL[i] = static_cast<float>(o * gl);
      outR[i] = static_cast<float>(o * gr);
    }
  }

private:
  double rand() {
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return seed_ / 4294967296.0 * 2 - 1;
  }

  double sampleRate_;
  bool pitchOn_, cutOn_;
  Adsr amp_, fil_;
  std::uint32_t seed_ = 1;
  double p1_ = 0, p2_ = 0, pwmPh_ = 0, vibPh_ = 0, note_ = 60, pos_ = 60, velocity_ = 1;
  double d1_ = 0, d2_ = 0, d3_ = 0, off2_ = 0, pan_ = 0;
  std::uint32_t noiseSeed_ = 12345;
  double s1_ = 0, s2_ = 0;
  double driftC_ = 0, driftGain_ = 0;
};

}  // namespace

void registerOberheim(NodeRegistry& registry) {
  NodeType type;
  type.type = "oberheim";
  type.inputs = {"pitch", "cut"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"cut", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"wave1", 0, 2, 0, true},       // 0 saw, 1 pulse, 2 triangle
    {"wave2", 0, 2, 1, true},
    {"pw", 0.05, 0.95, 0.5},        // pulse width
    {"pwm", 0, 0.45, 0.15},         // how far the width swings
    {"pwmrate", 0.1, 10, 0.8},      // Hz
    {"osc2", 0, 1, 0.8},            // level of oscillator 2
    {"detune", -100, 100, 6},       // cents, oscillator 2 against 1
    {"tune2", -24, 24, 0},          // semitones
    {"sync", 0, 1, 0, true},        // oscillator 2 restarts with oscillator 1
    {"sweep", 0, 48, 0},            // semitones the filter envelope adds to oscillator 2
    {"noise", 0, 1, 0},
    {"cutoff", 20, 20000, 1800},    // Hz
    {"res", 0, 1, 0.25},
    {"mode", 0, 1, 0},              // 0 lowpass, 0.5 notch, 1 highpass
    {"bp", 0, 1, 0},                // blend towards bandpass
    {"drive", 0, 1, 0.15},
    {"fenv", -8, 8, 2.5},           // octaves of cutoff the filter envelope adds
    {"keytrack", 0, 1, 0.5},
    {"fatk", 0.1, 5000, 2},
    {"fdec", 1, 5000, 400},
    {"fsus", 0, 1, 0.3},
    {"frel", 1, 5000, 300},
    {"atk", 0.1, 5000, 4},
    {"dec", 1, 5000, 300},
    {"sus", 0, 1, 0.8},
    {"rel", 1, 5000, 300},
    {"glide", 0, 2000, 0},          // ms
    {"vib", 0, 100, 0},             // vibrato depth, cents
    {"vrate", 0.5, 12, 5.5},        // Hz
    {"drift", 0, 1, 0.4},
    {"scatter", 0, 1, 0.4},         // how far from the centre each note may land
    {"vel", 0, 1, 0.5},
    {"level", 0, 1, 0.4},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Oberheim>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

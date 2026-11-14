// Subtractive synth in one node: two band-limited oscillators (polyBLEP saw and square, naive triangle and sine) with
// unison, a sub oscillator and noise, into a 24 dB/oct resonant ladder lowpass, shaped by a filter envelope and an
// amplitude envelope. Stereo out: the unison voices are spread across the field. Monophonic, the last note wins, with
// optional glide (a smoothing time in ms) and slide (a straight-line glide measured in ticks that starts from the
// previous note of the same pattern column). The `pitch` input is in octaves (like osc.fm) and moves both oscillators;
// `cut` is in octaves and moves the filter cutoff, so an lfo can drive either.
//
// The ladder is a zero-delay-feedback four-pole with tanh saturation at its input, so resonance and drive bite rather
// than just ring, and resonance 1 sings by itself.
// Port of webtracker src/audio/nodes/subsynth.js.
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
  kWave1 = 0, kWave2, kPw, kOsc2, kDetune, kTune2, kSub, kNoise, kUnison, kSpread, kWidth, kCutoff, kRes, kDrive, kFenv, kKeytrack,
  kFatk, kFdec, kFsus, kFrel, kAtk, kDec, kSus, kRel, kGlide, kSlide, kVel, kLevel
};
enum Input { kPitchIn = 0, kCutIn = 1 };

constexpr int kMaxUnison = 7, kSubBlock = 8;

double oscillator(int wave, double p, double dt, double pw) {
  if (wave == 0) return 2 * p - 1 - polyBlep(p, dt);
  if (wave == 1) {
    double q = p + 1 - pw;
    if (q >= 1) q -= 1;
    return (p < pw ? 1 : -1) + polyBlep(p, dt) - polyBlep(q, dt);
  }
  if (wave == 2) return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
  return js::sin(kTwoPi * p);
}

double flush(double v) { return std::abs(v) < 1e-18 ? 0 : v; }

class SubSynth final : public Node {
public:
  explicit SubSynth(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        clock_(context.clock),
        pitchOn_(context.connected[kPitchIn]),
        cutOn_(context.connected[kCutIn]),
        amp_(context.sampleRate),
        fil_(context.sampleRate) {
    for (int i = 0; i < kMaxUnison; i++) start_[i] = jsMod(i * 0.61803398875, 1);
    resetPhases();
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      ampFresh_ = amp_.idle();
      if (amp_.idle()) {
        resetPhases();
        std::fill(std::begin(s_), std::end(s_), 0.0);
        pos_ = event.note;
      }
      note_ = event.note;
      velocity_ = event.vel;
      latch_ = true;
      from_ = event.from;
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
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    if (amp_.idle()) {
      std::fill(outLeft, outLeft + n, 0.0f);
      std::fill(outRight, outRight + n, 0.0f);
      return;
    }
    const auto g = [&](Param p) { return static_cast<double>(params[slots[p]]); };
    const float* pitchIn = inputs[kPitchIn];
    const float* cutIn = inputs[kCutIn];
    amp_.set(g(kAtk), g(kDec), g(kSus), g(kRel));
    fil_.set(g(kFatk), g(kFdec), g(kFsus), g(kFrel));
    const double glideMs = g(kGlide);
    if (latch_) {   // the slide time in force when the note started (the pattern writes it just before)
      latch_ = false;
      const double ticks = g(kSlide), tickSamples = clock_ ? clock_->tickSamples : sampleRate_ * 0.125;
      if (ticks > 0) {
        if (from_ >= 0 && pos_ != from_ && ampFresh_) pos_ = from_;
        rate_ = std::abs(note_ - pos_) / (ticks * tickSamples);
      } else {
        rate_ = 0;
      }
      ampFresh_ = false;
    }
    if (rate_ > 0) {
      const double step = rate_ * n;
      pos_ = std::abs(note_ - pos_) <= step ? note_ : pos_ + sign(note_ - pos_) * step;
      if (pos_ == note_) rate_ = 0;
    } else if (glideMs > 0.5) {
      pos_ += (note_ - pos_) * (1 - js::exp(-n / (sampleRate_ * glideMs * 0.001 / 3)));
    } else {
      pos_ = note_;
    }
    const int w1 = jsRoundInt(g(kWave1)), w2 = jsRoundInt(g(kWave2));
    const double pw = g(kPw);
    const double lvl2 = g(kOsc2), sub = g(kSub), noise = g(kNoise);
    const int U = std::max(1, std::min(kMaxUnison, jsRoundInt(g(kUnison))));
    const double spread = g(kSpread), width = g(kWidth), detune = g(kDetune), tune2 = g(kTune2);
    const double baseHz = 440 * js::pow(2.0, (pos_ - 69) / 12);
    const double vnorm = 1 / std::sqrt(static_cast<double>(U));
    for (int u = 0; u < U; u++) {
      const double x = U == 1 ? 0 : (2.0 * u) / (U - 1) - 1;
      const double cents = x * spread;
      inc1_[u] = (baseHz * js::pow(2.0, cents / 1200)) / sampleRate_;
      inc2_[u] = (baseHz * js::pow(2.0, (tune2 * 100 + detune + cents) / 1200)) / sampleRate_;
      const double a = ((x * width + 1) * kPi) / 4;   // equal-power pan, centre at x = 0
      gl_[u] = js::cos(a) * kSqrt2 * vnorm;
      gr_[u] = js::sin(a) * kSqrt2 * vnorm;
    }
    const double subInc = baseHz / 2 / sampleRate_;
    const double cutoff = g(kCutoff), res = g(kRes), k = res * 4, drive = 1 + g(kDrive) * 4;
    const double fenv = g(kFenv), keyOct = (g(kKeytrack) * (pos_ - 60)) / 12;
    const double comp = 1 + res * 0.5;
    const double level = g(kLevel) * (1 - g(kVel) + g(kVel) * velocity_) * 1.5;
    const double nyq = sampleRate_ * 0.45;
    double G = 0, G2 = 0, G3 = 0, G4 = 0, norm = 1;
    double* s = s_;
    for (int i = 0; i < n; i++) {
      const double a = amp_.next(), f = fil_.next();
      if ((i & (kSubBlock - 1)) == 0) {
        const double oct = fenv * f + keyOct + (cutOn_ ? cutIn[i] : 0);
        const double hz = std::min(nyq, std::max(10.0, cutoff * js::pow(2.0, oct)));
        const double gg = js::tan((kPi * hz) / sampleRate_);
        G = gg / (1 + gg);
        G2 = G * G;
        G3 = G2 * G;
        G4 = G2 * G2;
        norm = 1 / (1 + k * G4);
      }
      const double fm = pitchOn_ ? js::pow(2.0, pitchIn[i]) : 1;
      double l = 0, r = 0;
      for (int u = 0; u < U; u++) {
        const double d1 = inc1_[u] * fm, d2 = inc2_[u] * fm;
        const double v = oscillator(w1, ph1_[u], d1, pw) + (lvl2 > 0 ? lvl2 * oscillator(w2, ph2_[u], d2, pw) : 0);
        l += v * gl_[u];
        r += v * gr_[u];
        double q1 = ph1_[u] + d1; if (q1 >= 1) q1 -= 1; ph1_[u] = q1;
        double q2 = ph2_[u] + d2; if (q2 >= 1) q2 -= 1; ph2_[u] = q2;
      }
      double c = 0;
      if (sub > 0) c += sub * js::sin(kTwoPi * subPhase_);
      subPhase_ += subInc * fm;
      if (subPhase_ >= 1) subPhase_ -= 1;
      if (noise > 0) {
        seed_ = seed_ * 1664525u + 1013904223u;
        c += noise * (seed_ / 2147483648.0 - 1);
      }
      l += c;
      r += c;
      // ladder, left then right: y = G x + (1 - G) s for each stage, solved with the feedback
      for (int ch = 0; ch < 2; ch++) {
        const int o = ch * 4;
        const double h = 1 - G;
        const double S1 = h * s[o], S2 = h * s[o + 1], S3 = h * s[o + 2], S4 = h * s[o + 3];
        const double sigma = G3 * S1 + G2 * S2 + G * S3 + S4;
        const double z = ((ch ? r : l) * comp * drive - k * sigma) * norm, z2 = z * z;
        const double x1 = z > 3 ? 1 : z < -3 ? -1 : z * (27 + z2) / (27 + 9 * z2);   // rational tanh
        const double y1 = G * x1 + S1, y2 = G * y1 + S2, y3 = G * y2 + S3, y4 = G * y3 + S4;
        s[o] = flush(2 * y1 - s[o]);
        s[o + 1] = flush(2 * y2 - s[o + 1]);
        s[o + 2] = flush(2 * y3 - s[o + 2]);
        s[o + 3] = flush(2 * y4 - s[o + 3]);
        if (ch) r = y4; else l = y4;
      }
      outLeft[i] = static_cast<float>((l * a * level) / drive);
      outRight[i] = static_cast<float>((r * a * level) / drive);
    }
  }

private:
  void resetPhases() {
    for (int i = 0; i < kMaxUnison; i++) { ph1_[i] = start_[i]; ph2_[i] = jsMod(start_[i] + 0.37, 1); }
    subPhase_ = 0;
  }

  double sampleRate_;
  const Clock* clock_;
  bool pitchOn_, cutOn_;
  Adsr amp_, fil_;
  double ph1_[kMaxUnison], ph2_[kMaxUnison], inc1_[kMaxUnison] = {}, inc2_[kMaxUnison] = {};
  double gl_[kMaxUnison] = {}, gr_[kMaxUnison] = {}, start_[kMaxUnison];
  double subPhase_ = 0, note_ = 60, pos_ = 60, velocity_ = 1;
  std::uint32_t seed_ = 12345;
  // a new note: read `slide` once, in the next block, and move pos in a straight line
  bool latch_ = false, ampFresh_ = false;
  double from_ = -1, rate_ = 0;
  double s_[8] = {};   // ladder states: 4 left, 4 right
};

}  // namespace

void registerSubSynth(NodeRegistry& registry) {
  NodeType type;
  type.type = "subsynth";
  type.inputs = {"pitch", "cut"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"cut", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"wave1", 0, 3, 0, true},       // 0 saw, 1 square, 2 triangle, 3 sine
    {"wave2", 0, 3, 0, true},
    {"pw", 0.05, 0.95, 0.5},        // pulse width of the square waves
    {"osc2", 0, 1, 0.6},            // level of oscillator 2 (oscillator 1 is 1)
    {"detune", -100, 100, 7},       // cents, oscillator 2 against 1
    {"tune2", -24, 24, 0},          // semitones, oscillator 2 against 1
    {"sub", 0, 1, 0},               // sine one octave below
    {"noise", 0, 1, 0},
    {"unison", 1, 7, 1, true},
    {"spread", 0, 50, 12},          // cents, outermost unison voices
    {"width", 0, 1, 0.7},           // stereo spread of the unison voices
    {"cutoff", 20, 20000, 1200},    // Hz
    {"res", 0, 1, 0.2},
    {"drive", 0, 1, 0.2},
    {"fenv", -8, 8, 3},             // octaves of cutoff the filter envelope adds
    {"keytrack", 0, 1, 0.5},
    {"fatk", 0.1, 5000, 1},         // filter envelope, ms
    {"fdec", 1, 5000, 250},
    {"fsus", 0, 1, 0.2},
    {"frel", 1, 5000, 150},
    {"atk", 0.1, 5000, 2},          // amplitude envelope, ms
    {"dec", 1, 5000, 300},
    {"sus", 0, 1, 0.8},
    {"rel", 1, 5000, 150},
    {"glide", 0, 2000, 0},          // ms
    {"slide", 0, 15.9375, 0},       // ticks: a note that follows another in its column slides to its pitch in this long
    {"vel", 0, 1, 0.5},
    {"level", 0, 1, 0.4},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<SubSynth>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

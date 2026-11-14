// Piano by physical modelling. A note is a hammer striking one, two or three strings (one in the bass, three in the
// treble), and the sound of those strings coming through a wooden body.
//
//   hammer    a half-sine force pulse. A hard blow is shorter (brighter, louder); the felt makes it longer in the bass
//             and when `hard` is low. It enters the strings through a comb at the strike point, `pos`.
//   strings   waveguides: a delay line the length of one period, a fractional-delay allpass to tune it exactly, a chain
//             of allpasses that makes the high partials run slightly sharp (`inharm`), and a lowpass in the loop so the
//             highs die first (`bright`). The strings of a note are detuned a little (`detune`) so they beat; one of
//             them decays quickly and the others slowly, the two-stage decay (`prompt` is how much of the first).
//   body      three broad resonances below 600 Hz, and a soft thump of the hammer on the keybed.
//   damper    note off lets the damper fall on the strings, over `release` ms, unless the pedal is down.
//
// Monophonic like the other synths: the voices node gives each key its own copy. Port of webtracker src/audio/nodes/piano.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kDecay = 0, kPrompt, kHard, kPos, kBright, kInharm, kDetune, kBody, kThump, kWidth, kPedal, kRelease, kTune, kVel, kLevel };

constexpr int kSize = 1 << 14, kMask = kSize - 1, kSections = 3, kMaxStr = 3;

// phase delay, in samples, of the first order allpass (a + z^-1) / (1 + a z^-1) at angular frequency w
double allpassDelay(double a, double w) {
  const double num = js::atan2(-js::sin(w), a + js::cos(w)), den = js::atan2(-a * js::sin(w), 1 + a * js::cos(w));
  return -(num - den) / w;
}

// loudness correction by pitch (0 is A0, 1 is C8), so notes across the keyboard come out at about the same level
double norm(double pf) {
  static const double loud[8][2] = {{21, 0.7}, {36, 0.7}, {48, 0.85}, {60, 1}, {72, 1.6}, {84, 3.3}, {96, 5.7}, {108, 8}};
  const double midi = 21 + pf * 87;
  for (int i = 1; i < 8; i++) {
    if (midi <= loud[i][0]) {
      const double m0 = loud[i - 1][0], g0 = loud[i - 1][1], m1 = loud[i][0], g1 = loud[i][1];
      return g0 + (g1 - g0) * (midi - m0) / (m1 - m0);
    }
  }
  return loud[7][1];
}

double clampTo(double x, double a, double b) { return std::min(b, std::max(a, x)); }

struct BodyFilter { double b0, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0; };

class Piano final : public Node {
public:
  explicit Piano(const NodeContext& context) : fs_(context.sampleRate), pitchOn_(!context.connected.empty() && context.connected[0]) {
    for (auto& b : buf_) b.assign(kSize, 0.0);
    // the body: three resonant bandpass filters (constant 0 dB peak gain), run on the sum of the strings
    const double table[3][2] = {{95, 4}, {230, 5}, {540, 4}};
    for (int i = 0; i < 3; i++) {
      const double w0 = 2 * kPi * table[i][0] / fs_, al = js::sin(w0) / (2 * table[i][1]), a0 = 1 + al;
      bq_[i] = {al / a0, -al / a0, -2 * js::cos(w0) / a0, (1 - al) / a0};
    }
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { note_ = event.note; velocity_ = event.vel; pending_ = true; active_ = true; keyUp_ = false; }
    else if (event.type == NoteType::Off || event.type == NoteType::AllOff) keyUp_ = true;
    else if (event.type == NoteType::Cut) { active_ = false; keyUp_ = false; ns_ = 0; for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.0); }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (!active_) { std::fill(outL, outL + n, 0.0f); std::fill(outR, outR + n, 0.0f); return; }
    auto g = [&](int i) -> double { return params[slots[i]]; };
    if (pending_) { pending_ = false; strike(g, pitchOn_ ? inputs[0][0] : 0.0); }
    if (keyUp_ && !damped_ && g(kPedal) < 0.5) damped_ = true;   // the damper falls, or the pedal comes up
    const double level = g(kLevel) * 2, body = g(kBody);
    for (int s = 0; s < ns_; s++) gs_[s] = loop_[s] * (damped_ ? damp_[s] : 1);
    double peak = 0;
    for (int i = 0; i < n; i++) {
      // the hammer: a half-sine pulse, entering through the strike-point comb
      double e = 0;
      if (k_ < strikeEnd_) {
        e = (k_ >= 0 && k_ < Lc_ ? js::sin(kPi * k_ / Lc_) : 0) - (k_ - dpos_ >= 0 && k_ - dpos_ < Lc_ ? js::sin(kPi * (k_ - dpos_) / Lc_) : 0);
      }
      k_++;
      double sum = 0;
      for (int s = 0; s < ns_; s++) {
        std::vector<double>& b = buf_[static_cast<std::size_t>(s)];
        double x = b[static_cast<std::size_t>((w_ - delay_[s]) & kMask)];
        double y = ia_[s] * x + ix_[s] - ia_[s] * iy_[s]; ix_[s] = x; iy_[s] = y; x = y;   // fractional delay
        for (int q = 0; q < kSections; q++) {   // stiffness: the allpass chain
          const int j = s * kSections + q;
          y = dispA_ * x + dx_[j] - dispA_ * dy_[j]; dx_[j] = x; dy_[j] = y; x = y;
        }
        lp_[s] = (1 - lc_[s]) * x + lc_[s] * lp_[s];   // the highs die first
        sum += lp_[s] * weight_[s];
        b[static_cast<std::size_t>(w_ & kMask)] = lp_[s] * gs_[s] + e * amp_ * weight_[s];
      }
      w_ = (w_ + 1) & kMask;
      // the thump of the hammer on the keybed: lowpassed noise that dies in about 12 ms
      thumpLp_ += 0.35 * (rand() - thumpLp_);
      const double t = thumpLp_ * thumpEnv_ * thumpAmp_;
      thumpEnv_ *= thumpDecay_;
      const double dry = sum + t;
      double resonance = 0;
      static const double bodyGain[3] = {0.9, 0.7, 0.45};
      for (int r = 0; r < 3; r++) {
        BodyFilter& f = bq_[r];
        const double o = f.b0 * dry + f.b2 * f.x2 - f.a1 * f.y1 - f.a2 * f.y2;
        f.x2 = f.x1; f.x1 = dry; f.y2 = f.y1; f.y1 = o;
        resonance += o * bodyGain[r];
      }
      const double m = (dry + body * resonance * 2) * level;
      outL[i] = static_cast<float>(m * panL_); outR[i] = static_cast<float>(m * panR_);
      const double a = std::abs(m);
      if (a > peak) peak = a;
    }
    quiet_ = peak < 1e-6 ? quiet_ + 1 : 0;
    if (quiet_ > 8 && k_ > strikeEnd_) active_ = false;
  }

private:
  double rand() {
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    return seed_ / 4294967296.0 * 2 - 1;
  }

  // Set the strings up for a note with the current parameters.
  template <typename G>
  void strike(G& g, double pitchOct) {
    const double fs = fs_;
    const double midi = note_ + g(kTune) + pitchOct * 12;
    const double f0 = 440 * js::pow(2, (midi - 69) / 12);
    const double pf = clampTo((midi - 21) / 87, 0, 1);
    ns_ = midi < 31 ? 1 : midi < 48 ? 2 : 3;
    // Stiffness. A real string's partials run sharp, f(n) = n f0 sqrt(1 + B n^2). The allpass chain bends the loop's
    // delay so that the sixth partial lands where that formula puts it (the coefficient is found by bisection on the
    // chain's exact delay); the others follow along.
    const double B = g(kInharm) * 2 * 4e-4 * js::pow(2, (midi - 60) / 18);
    const double T0 = fs / f0, w0 = 2 * kPi * f0 / fs, N6 = 6;
    const double want = 0.5 * js::log2(1 + B * N6 * N6);   // octaves sharp, at the sixth partial
    auto got = [&](double a) { return -js::log2(1 + kSections * (allpassDelay(a, N6 * w0) - allpassDelay(a, w0)) / T0); };
    double lo = 0, hi = T0 > 40 ? 0.95 : 0.4;
    for (int it = 0; it < 30; it++) { const double mid = (lo + hi) / 2; if (got(-mid) < want) lo = mid; else hi = mid; }
    dispA_ = -lo;
    const double cUser = 0.62 + (0.03 - 0.62) * g(kBright);
    const double t60 = clampTo(g(kDecay) * js::pow(2, -(midi - 60) / 36), 0.3, 40);
    const double spread3[3] = {-1, 0, 1}, spread2[2] = {-1, 1}, spread1[1] = {0};
    const double* spread = ns_ == 1 ? spread1 : ns_ == 2 ? spread2 : spread3;
    const double prompt = g(kPrompt);
    for (int s = 0; s < kMaxStr; s++) { std::fill(buf_[static_cast<std::size_t>(s)].begin(), buf_[static_cast<std::size_t>(s)].end(), 0.0); ix_[s] = iy_[s] = lp_[s] = 0; }
    std::fill(std::begin(dx_), std::end(dx_), 0.0); std::fill(std::begin(dy_), std::end(dy_), 0.0);
    for (int s = 0; s < ns_; s++) {
      const double f = f0 * js::pow(2, spread[s] * g(kDetune) / 1200), wf = 2 * kPi * f / fs;
      const double T = fs / f;
      const double strT60 = ns_ == 1 ? t60 * 0.6 : s == 0 ? t60 * 0.15 : t60;   // one quick string, the rest slow
      const double target = js::pow(10, -3 * (T / fs) / strT60);
      // The loop lowpass takes a little off the fundamental on every trip. Where that would be more than the string
      // may lose (the treble, with many trips a second) it is eased until the fundamental decays at the wanted rate.
      auto mag = [&](double c) { return (1 - c) / jsHypot(1 - c * js::cos(wf), c * js::sin(wf)); };
      double c = cUser;
      if (mag(c) < target / 0.99995) {
        double l = 0, h = c;
        for (int it = 0; it < 24; it++) { const double mid = (l + h) / 2; if (mag(mid) >= target / 0.99995) l = mid; else h = mid; }
        c = l;
      }
      lc_[s] = c;
      const double lpMag = mag(c);
      const double lpDelay = js::atan2(c * js::sin(wf), 1 - c * js::cos(wf)) / wf;
      const double D = T - lpDelay - kSections * allpassDelay(dispA_, wf);
      const int Dint = static_cast<int>(std::max(2.0, std::floor(D - 0.5)));
      double d = D - Dint;
      for (int it = 0; it < 4; it++) d += (D - Dint) - allpassDelay((1 - d) / (1 + d), wf);   // the interpolator's delay at this frequency, not its delay at DC
      d = clampTo(d, 0.3, 1.9);
      ia_[s] = (1 - d) / (1 + d);
      delay_[s] = Dint;
      loop_[s] = std::min(0.99995, target / lpMag);
      damp_[s] = js::pow(10, -3 * (T / fs) / (g(kRelease) * 0.001));
      weight_[s] = ns_ == 1 ? 1 : s == 0 ? prompt * 1.4 : (1 - prompt) * 1.4 / (ns_ - 1);
    }
    const double hard = g(kHard), v = velocity_;
    const double Tc = 0.0032 * js::pow(f0 / 65.4, -0.45) * (0.6 + 0.9 * (1 - hard)) * (1.15 - 0.5 * v);   // seconds of contact: shorter for a hard felt, a hard blow and a high note
    Lc_ = std::max(4.0, std::min(Tc * fs, 0.5 * fs / f0));   // a blow no longer than half a period, or the string would hardly feel it
    dpos_ = std::max(1.0, jsRound(g(kPos) * fs / f0));
    const double vm = 1 - g(kVel) + g(kVel) * v;
    amp_ = vm * vm * 0.5 * std::min(1.0, fs / f0 / Lc_) * js::pow(2, -pf * 0.4) * norm(pf);
    k_ = 0; strikeEnd_ = Lc_ + dpos_ + 1;
    thumpEnv_ = 1; thumpDecay_ = js::exp(-1 / (0.012 * fs)); thumpAmp_ = g(kThump) * vm * vm * 0.5;
    const double pos = 0.5 + (pf - 0.5) * g(kWidth);
    panL_ = js::cos(pos * kPi / 2) * kSqrt2; panR_ = js::sin(pos * kPi / 2) * kSqrt2;
    damped_ = false; quiet_ = 0;
  }

  double fs_;
  bool pitchOn_;
  std::vector<double> buf_[kMaxStr];
  // per string: allpass interpolation (a, x1, y1), dispersion (x1, y1 per section), loop lowpass state, gains
  double ia_[kMaxStr] = {}, ix_[kMaxStr] = {}, iy_[kMaxStr] = {}, lp_[kMaxStr] = {};
  double dx_[kMaxStr * kSections] = {}, dy_[kMaxStr * kSections] = {};
  double gs_[kMaxStr] = {}, loop_[kMaxStr] = {}, damp_[kMaxStr] = {}, weight_[kMaxStr] = {}, lc_[kMaxStr] = {};
  int delay_[kMaxStr] = {};
  int ns_ = 0, w_ = 0;
  double dispA_ = 0;
  int note_ = 60;
  double velocity_ = 1;
  bool active_ = false, keyUp_ = false, damped_ = false, pending_ = false;
  double k_ = 0, Lc_ = 100, dpos_ = 10, amp_ = 0, strikeEnd_ = 0;
  double thumpEnv_ = 0, thumpDecay_ = 0, thumpAmp_ = 0, thumpLp_ = 0;
  std::uint32_t seed_ = 0x2545f491u;
  double quiet_ = 0, panL_ = 1, panR_ = 1;
  BodyFilter bq_[3] = {};
};

}  // namespace

void registerPiano(NodeRegistry& registry) {
  NodeType type;
  type.type = "piano";
  type.inputs = {"pitch"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"decay", 0.5, 40, 9},         // seconds for the after sound to fall 60 dB at middle C
    {"prompt", 0, 1, 0.45},
    {"hard", 0, 1, 0.5},
    {"pos", 0.04, 0.5, 0.12},
    {"bright", 0, 1, 0.55},
    {"inharm", 0, 1, 0.5},
    {"detune", 0, 15, 1.6},        // cents between the strings of a note
    {"body", 0, 1, 0.5},
    {"thump", 0, 1, 0.25},
    {"width", 0, 1, 0.5},
    {"pedal", 0, 1, 0},
    {"release", 20, 3000, 250},    // ms the damper takes to still a released string
    {"tune", -48, 48, 0},
    {"vel", 0, 1, 0.7},
    {"level", 0, 1, 0.5},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Piano>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Piano by physical modelling. A note is a hammer striking one, two or three strings (one in the bass, three in the
// treble), and the sound of those strings coming through a wooden body.
//
//   hammer    a half-sine force pulse. A hard blow is shorter (brighter, louder); the felt makes it longer in the bass
//             and when `hard` is low. It enters the strings through a comb at the strike point, `pos`.
//             The pulse is band-limited before it is sampled: each sample is the half-sine smoothed by a Hann kernel four samples
//             wide (which has a null at the Nyquist frequency), so the short, hard blow of a high note does not alias. The
//             smoothing delays the blow by two samples.
//   strings   waveguides: a delay line the length of one period, a fractional-delay allpass to tune it exactly, a chain
//             of allpasses that makes the high partials run slightly sharp (`inharm`), and a lowpass in the loop so the
//             highs die first (`bright`). The strings of a note are detuned a little (`detune`) so they beat; one of
//             them decays quickly and the others slowly, the two-stage decay (`prompt` is how much of the first).
//   felt      the hammer as physics: a mass on a nonlinear felt spring (force grows as compression to the power 2.5 to 3.3)
//             pressing on a rigid string, so a hard blow is shorter, sharper and brighter. 0 is the plain pulse, 1 is all physics.
//   board     the soundboard as short, damped resonances from 90 Hz to 5 kHz (32; 12 at quality eco). 0 leaves them out.
//   knock     the damper felt's tick when a key comes up.
//   muffle    the wood takes the edge off the percussive part: the attack goes through a lowpass that starts dark and opens
//             over some 25 ms. 0 is off.
//   scatter   the felt dissipates the blow unevenly: the muffle's opening is jagged, a correlated random wobble of the cutoff
//             that dies away with the attack. 0 is the smooth rise.
//   contact   how long the hammer stays on the string, as a multiple of what the pitch, `hard` and the blow give (1): longer
//             is a softer, rounder front and a heavier thud, shorter a sharper click. A high note's blow is held to half a
//             period of its string at 1; a longer contact lets it run on. 1 changes nothing.
//   strike    the treble's sharp hammer, layered under the lower notes: a note is also given the short hard blow that a note
//             high on the keyboard gets, through the same strike point, softened by a lowpass that follows the key and fading
//             out towards the treble. The blow carries the same push as the note's own, only briefer. 0 is off.
//   body      three broad resonances below 600 Hz, and a soft thump of the hammer on the keybed.
//   damper    note off lets the damper fall on the strings, over `release` ms, unless the pedal is down.
//
// Monophonic like the other synths: the voices node gives each key its own copy. Port of webtracker src/audio/nodes/piano.js.
#include <algorithm>
#include <array>
#include <map>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kDecay = 0, kPrompt, kHard, kPos, kBright, kInharm, kDetune, kBody, kThump, kWidth, kPedal, kRelease, kTune, kVel, kLevel, kFelt, kBoard, kKnock, kMuffle, kScatter, kQuality, kContact, kStrike, kRealism };

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

// ---- the felt hammer (as in the web's piano.js) ------------------------------------------------------------------------------
// The mass of the hammer (kg) by MIDI note, from measurements of grand pianos.
constexpr double kStrikeNote = 96;   // the key whose hammer blow the `strike` layer borrows
constexpr int kPulseMax = 4096;
constexpr int kPulseOversample = 8, kPulseDelay = 2;
// A half-sine of `length` samples (not a whole number), band-limited before it is sampled: the pulse is averaged against a Hann
// kernel four samples wide, worked out at kPulseOversample points per sample, and delayed by kPulseDelay samples (half the kernel).
int smoothPulse(double* out, double length) {
  const int count = std::min(kPulseMax, static_cast<int>(std::ceil(length)) + 2 * kPulseDelay);
  for (int k = 0; k < count; k++) {
    const double centre = k - kPulseDelay;
    double sum = 0;
    for (int m = 0; m < 4 * kPulseOversample; m++) {
      const double u = (m + 0.5) / kPulseOversample - 2, t = centre + u;
      if (t >= 0 && t < length) sum += js::sin(kPi * t / length) * 0.25 * (1 + js::cos(kPi * u / 2));
    }
    out[k] = sum / kPulseOversample;
  }
  return count;
}   // the longest pulse kept, in samples
struct PulseResult { int length; double seconds; };

// The same notes come back again and again (a pattern repeats, six voices share a chord), and working out a note's pulses is the most
// expensive thing a piano does, so what was found for a given set of numbers is kept (per thread, a few dozen at most) and reused. The result is
// exactly what working it out again would give.
struct PulseCache {
  struct Smooth { std::vector<double> samples; };
  struct Felt { double stiffness = 0; PulseResult now{}; std::vector<double> pulse; double refSum = 0; };
  std::map<double, Smooth> smooth;                    // length in samples -> the band-limited half-sine
  std::map<std::array<double, 5>, Felt> felt;         // (rate, midi, hardness, contact, velocity) -> the felt hammer's pulse
};
PulseCache& pulseCache() {
  thread_local PulseCache cache;
  if (cache.smooth.size() > 64) cache.smooth.clear();   // (a long automation sweep makes new numbers all the time: start over rather than grow)
  if (cache.felt.size() > 64) cache.felt.clear();
  return cache;
}

constexpr int kSubsteps = 8;      // the hammer is stepped this many times per sample, so the stiff felt stays stable
constexpr double kHammerSpeed = 3;   // m/s at velocity 1
constexpr int kBoardModes = 32;

double hammerMass(double midi) {
  static const double table[5][2] = {{21, 9e-3}, {48, 5.5e-3}, {60, 4e-3}, {84, 2.8e-3}, {108, 2e-3}};
  if (midi <= table[0][0]) return table[0][1];
  for (int i = 1; i < 5; i++) {
    if (midi <= table[i][0]) {
      const double x0 = table[i - 1][0], y0 = table[i - 1][1], x1 = table[i][0], y1 = table[i][1];
      return y0 + (y1 - y0) * (midi - x0) / (x1 - x0);
    }
  }
  return table[4][1];
}

// Run the hammer into the (rigid) string. `out` gets the force at each sample (N).
PulseResult hammerPulse(double* out, double fs, double speed, double mass, double K, double p, int limit) {
  const double dt = 1 / (fs * kSubsteps);
  double x = 0, v = speed;
  int steps = 0;
  bool started = false;
  const int cap = std::min(kPulseMax, limit);
  for (int k = 0; k < cap; k++) {
    double sum = 0;
    for (int s = 0; s < kSubsteps; s++) {
      const double force = x > 0 ? K * js::pow(x, p) : 0;
      v -= force / mass * dt;
      x += v * dt;
      sum += force;
      steps++;
      if (force > 0) started = true;
      else if (started) { out[k] = sum / kSubsteps; return {k + 1, steps * dt}; }
    }
    out[k] = sum / kSubsteps;
  }
  return {cap, steps * dt};
}

// The felt stiffness that gives a contact of `seconds` (bisection on its logarithm).
double stiffnessFor(double* scratch, double fs, double speed, double mass, double p, double seconds) {
  double lo = js::log(1e6), hi = js::log(1e13);
  for (int it = 0; it < 40; it++) {
    const double mid = 0.5 * (lo + hi);
    const double got = hammerPulse(scratch, fs, speed, mass, js::exp(mid), p, kPulseMax).seconds;
    if (got > seconds) lo = mid; else hi = mid;
  }
  return js::exp(0.5 * (lo + hi));
}

struct BoardMode { double b0, b2, a1, a2, gain, x1 = 0, x2 = 0, y1 = 0, y2 = 0; };

BoardMode boardMode(int j, int count, double fs) {
  const double t = static_cast<double>(j) / (count - 1);
  const double f = std::min(0.45 * fs, 90 * js::pow(5000.0 / 90, t) * (1 + 0.05 * js::sin(j * 2.7)));
  const double q = 22 + 14 * js::sin(j * 1.3 + 0.5) + 30 * (1 - t);
  const double gain = js::pow(f / 200, -0.4) * (0.65 + 0.35 * js::cos(j * 1.7));
  const double w0 = 2 * kPi * f / fs, al = js::sin(w0) / (2 * q), a0 = 1 + al;
  return {al / a0, -al / a0, -2 * js::cos(w0) / a0, (1 - al) / a0, gain};
}

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
    pulse_.assign(kPulseMax, 0.0); scratch_.assign(kPulseMax, 0.0);
    for (int j = 0; j < kBoardModes; j++) board_[j] = boardMode(j, kBoardModes, fs_);
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { ringing_ = active_ && event.note == note_; note_ = event.note; velocity_ = event.vel; pending_ = true; active_ = true; keyUp_ = false; }
    else if (event.type == NoteType::Off || event.type == NoteType::AllOff) keyUp_ = true;
    else if (event.type == NoteType::Cut) { active_ = false; keyUp_ = false; ns_ = 0; for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.0); }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (!active_) { std::fill(outL, outL + n, 0.0f); std::fill(outR, outR + n, 0.0f); return; }
    auto g = [&](int i) -> double { return params[slots[i]]; };
    if (pending_) { pending_ = false; strike(g, pitchOn_ ? inputs[0][0] : 0.0, ringing_ && g(kRealism) >= 0.5); }
    if (keyUp_ && !damped_ && g(kPedal) < 0.5) { damped_ = true; knockEnv_ = 1; }   // the damper falls, or the pedal comes up
    const double level = g(kLevel) * 2, body = g(kBody);
    const double muffle = g(kMuffle), scatter = g(kScatter), boardAmount = g(kBoard);
    const int modes = g(kQuality) >= 0.5 ? 12 : kBoardModes;
    const double stride = static_cast<double>(kBoardModes) / modes;
    for (int s = 0; s < ns_; s++) gs_[s] = loop_[s] * (damped_ ? damp_[s] : 1);
    double peak = 0;
    for (int i = 0; i < n; i++) {
      // the hammer: a half-sine pulse, entering through the strike-point comb
      double e = 0;
      if (k_ < strikeEnd_) {
        const int kk = static_cast<int>(k_), kkd = kk - static_cast<int>(dpos_);
        e = (kk < plainLen_ ? plainPulse_[static_cast<std::size_t>(kk)] : 0) - (kkd >= 0 && kkd < plainLen_ ? plainPulse_[static_cast<std::size_t>(kkd)] : 0);
      }
      k_++;
      // the felt hammer's force through the same comb
      const int ki = static_cast<int>(k_) - 1, kd = ki - static_cast<int>(dpos_);
      const double ep = pulseLen_ == 0 ? 0 : (ki < pulseLen_ ? pulse_[static_cast<std::size_t>(ki)] : 0) - (kd >= 0 && kd < pulseLen_ ? pulse_[static_cast<std::size_t>(kd)] : 0);
      double drive = (1 - felt_) * e * amp_ + felt_ * ep * pulseAmp_;
      if (ampR_ > 0) {   // the layered blow: a short half-sine through the same comb, then the key's lowpass
        const int kr = static_cast<int>(k_) - 1, kdr = kr - static_cast<int>(dpos_);
        const double er = (kr < blowLen_ ? blowPulse_[static_cast<std::size_t>(kr)] : 0) - (kdr >= 0 && kdr < blowLen_ ? blowPulse_[static_cast<std::size_t>(kdr)] : 0);
        strikeLp_ += strikeCoef_ * (er * ampR_ - strikeLp_); drive += strikeLp_;
      }
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
        b[static_cast<std::size_t>(w_ & kMask)] = lp_[s] * gs_[s] + drive * weight_[s];
      }
      w_ = (w_ + 1) & kMask;
      // the thump of the hammer on the keybed: lowpassed noise that dies in about 12 ms
      thumpLp_ += 0.35 * (rand() - thumpLp_);
      const double t = thumpLp_ * thumpEnv_ * thumpAmp_;
      thumpEnv_ *= thumpDecay_;
      // the damper's tick: softer, shorter noise than the hammer's thump
      double tick = 0;
      if (knockEnv_ > 1e-4) { knockLp_ += 0.5 * (rand() - knockLp_); tick = knockLp_ * knockEnv_ * knockAmp_; knockEnv_ *= 0.9965; }
      double dry = sum + t + tick;
      if (muffle > 0) {   // one pole lowpass, fully open (coefficient 1) once the envelope has gone
        // the darkness is the envelope, wobbled by smoothed noise (its own generator, so the thump is not disturbed)
        double dark = muffle * muffleEnv_ * 0.92;
        if (scatter > 0) {
          scatterSeed_ ^= scatterSeed_ << 13; scatterSeed_ ^= scatterSeed_ >> 17; scatterSeed_ ^= scatterSeed_ << 5;
          scatterLp_ += 0.08 * (scatterSeed_ / 4294967296.0 * 2 - 1 - scatterLp_);
          dark = std::min(0.97, dark * std::max(0.0, 1 + scatter * scatterLp_ * 5));
        }
        muffleState_ += (1 - dark) * (dry - muffleState_);
        muffleEnv_ *= muffleDecay_;
        dry = muffleState_;
      }
      double resonance = 0;
      static const double bodyGain[3] = {0.9, 0.7, 0.45};
      for (int r = 0; r < 3; r++) {
        BodyFilter& f = bq_[r];
        const double o = f.b0 * dry + f.b2 * f.x2 - f.a1 * f.y1 - f.a2 * f.y2;
        f.x2 = f.x1; f.x1 = dry; f.y2 = f.y1; f.y1 = o;
        resonance += o * bodyGain[r];
      }
      double wood = 0;
      if (boardAmount > 0) {
        for (int j = 0; j < modes; j++) {
          BoardMode& f = board_[static_cast<int>(std::floor(j * stride))];
          const double o = f.b0 * dry + f.b2 * f.x2 - f.a1 * f.y1 - f.a2 * f.y2;
          f.x2 = f.x1; f.x1 = dry; f.y2 = f.y1; f.y1 = o;
          wood += o * f.gain;
        }
      }
      const double m = (dry + body * resonance * 2 + boardAmount * wood) * level;
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
  // `again`: the same key hit while its strings still ring (realism). The strings keep what they are doing and the hammer hits them anew.
  void strike(G& g, double pitchOct, bool again) {
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
    if (!again) {
      for (int s = 0; s < kMaxStr; s++) { std::fill(buf_[static_cast<std::size_t>(s)].begin(), buf_[static_cast<std::size_t>(s)].end(), 0.0); ix_[s] = iy_[s] = lp_[s] = 0; }
      std::fill(std::begin(dx_), std::end(dx_), 0.0); std::fill(std::begin(dy_), std::end(dy_), 0.0);
    }
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
    const double contact = g(kContact);
    const double Tn = 0.0032 * js::pow(f0 / 65.4, -0.45) * (0.6 + 0.9 * (1 - hard)) * (1.15 - 0.5 * v), Tc = Tn * contact;   // seconds of contact: shorter for a hard felt, a hard blow and a high note
    Lc_ = std::max(4.0, std::min(Tc * fs, 0.5 * fs / f0 * std::max(1.0, contact)));
    const double lcNatural = std::max(4.0, std::min(Tn * fs, 0.5 * fs / f0));   // the length at contact 1: a longer or shorter contact keeps the blow's push, so the level stays   // a blow no longer than half a period, or the string would hardly feel it
    dpos_ = std::max(1.0, jsRound(g(kPos) * fs / f0));
    const double vm = 1 - g(kVel) + g(kVel) * v;
    amp_ = vm * vm * 0.5 * std::min(1.0, fs / f0 / Lc_) * js::pow(2, -pf * 0.4) * norm(pf) * (lcNatural / Lc_);
    felt_ = g(kFelt);
    pulseLen_ = 0;
    if (felt_ > 0) {
      // the felt hammer: the stiffness is found so that a middling blow lasts as long as the plain pulse's would, then a
      // harder or softer blow, felt and string change it from there
      const double mass = hammerMass(midi), p = 2.5 + 0.8 * pf, vm1 = 1;
      auto tcAt = [&](double vel, double h) { return 0.0032 * js::pow(f0 / 65.4, -0.45) * (0.6 + 0.9 * (1 - h)) * (1.15 - 0.5 * vel); };
      PulseCache::Felt& found = pulseCache().felt[{fs, midi, hard, contact, vm}];
      if (found.pulse.empty()) {
        found.stiffness = stiffnessFor(scratch_.data(), fs, 0.7 * kHammerSpeed, mass, p, 0.75 * tcAt(0.7, 0.5) * contact) * js::exp(3.66 * (hard - 0.5));
        found.pulse.assign(kPulseMax, 0.0);
        found.now = hammerPulse(found.pulse.data(), fs, vm * kHammerSpeed, mass, found.stiffness, p, kPulseMax);
        const PulseResult ref = hammerPulse(scratch_.data(), fs, vm1 * kHammerSpeed, mass, found.stiffness, p, kPulseMax);
        for (int i = 0; i < ref.length; i++) found.refSum += scratch_[static_cast<std::size_t>(i)];
      }
      std::copy(found.pulse.begin(), found.pulse.end(), pulse_.begin());
      const PulseResult now = found.now;
      const double refSum = found.refSum;
      // the loudest blow is as loud as the plain pulse's loudest, so switching `felt` on changes the touch, not the level
      const double Lc1 = std::max(4.0, std::min(tcAt(1, hard) * fs, 0.5 * fs / f0));
      const double oldImpulse = std::min(1.0, fs / f0 / Lc1) * Lc1 * 2 / kPi;
      pulseLen_ = now.length;
      pulseAmp_ = 0.35 * js::pow(2, -pf * 0.4) * norm(pf) * oldImpulse / refSum * js::pow(vm, 0.6);
    }
    // the treble's blow (as a note at kStrikeNote would get it), with the same push as this note's own pulse and less of it as the
    // note rises; its lowpass sits at a fixed number of harmonics above the fundamental, so it follows the key
    {
      const double fR = 440 * js::pow(2, (kStrikeNote - 69) / 12.0);
      const double TcR = 0.0032 * js::pow(fR / 65.4, -0.45) * (0.6 + 0.9 * (1 - hard)) * (1.15 - 0.5 * v);
      Lr_ = std::max(4.0, std::min(TcR * fs, 0.5 * fs / fR));
      ampR_ = g(kStrike) * std::min(1.0, std::max(0.0, (kStrikeNote - midi) / 48)) * 0.5 * amp_ * Lc_ / Lr_;
      strikeCoef_ = 1 - js::exp(-2 * kPi * std::min(12000.0, std::max(500.0, f0 * 8)) / fs); strikeLp_ = 0;
    }
    const auto smoothed = [&](double* out, double length) {   // (kept: see PulseCache)
      auto& entry = pulseCache().smooth[length];
      if (entry.samples.empty()) { entry.samples.assign(kPulseMax + 1, 0.0); entry.samples[0] = smoothPulse(entry.samples.data() + 1, length); }
      std::copy(entry.samples.begin() + 1, entry.samples.end(), out);
      return static_cast<int>(entry.samples[0]);
    };
    plainLen_ = smoothed(plainPulse_.data(), Lc_); blowLen_ = ampR_ > 0 ? smoothed(blowPulse_.data(), Lr_) : 0;
    k_ = 0; strikeEnd_ = std::max({static_cast<double>(plainLen_), static_cast<double>(pulseLen_), static_cast<double>(blowLen_)}) + dpos_ + 1;
    for (BoardMode& m : board_) m.x1 = m.x2 = m.y1 = m.y2 = 0;
    knockEnv_ = 0; knockLp_ = 0; muffleEnv_ = 1; muffleState_ = 0; scatterLp_ = 0; scatterSeed_ = 0x1b873593u; muffleDecay_ = js::exp(-1 / (0.025 * fs));
    knockAmp_ = g(kKnock) * vm * vm * 0.08;
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
  bool ringing_ = false;   // the note on that is waiting was for the key already sounding
  bool active_ = false, keyUp_ = false, damped_ = false, pending_ = false;
  double k_ = 0, Lc_ = 100, dpos_ = 10, amp_ = 0, strikeEnd_ = 0;
  double thumpEnv_ = 0, thumpDecay_ = 0, thumpAmp_ = 0, thumpLp_ = 0;
  std::uint32_t seed_ = 0x2545f491u;
  double quiet_ = 0, panL_ = 1, panR_ = 1;
  BodyFilter bq_[3] = {};
  // the felt hammer's pulse, scratch space for finding its stiffness, the soundboard, the damper's tick
  std::vector<double> pulse_, scratch_;
  int pulseLen_ = 0;
  double pulseAmp_ = 0, felt_ = 0;
  std::vector<double> plainPulse_ = std::vector<double>(kPulseMax), blowPulse_ = std::vector<double>(kPulseMax);   // the plain hammer's pulse, and the layered treble blow's
  int plainLen_ = 0, blowLen_ = 0;
  double Lr_ = 0, ampR_ = 0, strikeLp_ = 0, strikeCoef_ = 1;   // the layered treble blow: its length, its height, and the lowpass that follows the key
  BoardMode board_[kBoardModes] = {};
  double knockEnv_ = 0, knockAmp_ = 0, knockLp_ = 0;
  double muffleEnv_ = 0, muffleState_ = 0, muffleDecay_ = 0, scatterLp_ = 0;
  std::uint32_t scatterSeed_ = 0;
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
    {"felt", 0, 1, 0},             // 0: the plain half-sine pulse, 1: the felt hammer model
    {"board", 0, 1, 0},            // the soundboard's resonances
    {"knock", 0, 1, 0},            // the damper's tick on note off
    {"muffle", 0, 1, 0},           // how dull the first moments are
    {"scatter", 0, 1, 0},          // how jagged the muffle's opening is
    {"quality", 0, 1, 0, true},    // 0 standard, 1 eco
    {"contact", 0.25, 4, 1},       // how long the hammer stays on the string, times the natural length
    {"strike", 0, 1, 0},           // the treble's hammer blow layered under the lower notes
    {"realism", 0, 1, 0, true},    // 0: every note gets a voice; 1: a key struck again while it rings hits the same strings (one string, one voice)
  };
  type.restrikeParam = "realism";
  type.create = [](const NodeContext& context) { return std::make_unique<Piano>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

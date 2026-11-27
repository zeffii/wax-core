// Piano 2: a piano as one coupled mechanical system, not a sound with a piano's envelope. Port of webtracker
// src/audio/nodes/piano2.js (the comments there describe the model in full; the short version follows).
//
//   hammer    a mass on a nonlinear felt spring with hysteresis, pressing on the string; the string gives way and pushes back, and
//             the waves returning from its ends reach the hammer while it is still there (it can touch twice).
//   strings   waveguides struck at a point (`pos`): the hammer reads and drives the waves going both ways, which makes the comb of
//             missing harmonics by itself. A fractional-delay allpass, a stiffness chain (`inharm`) and a loop lowpass (`bright`).
//   bridge    every string ends on one bridge, whose give is the soundboard (modal admittance, `board`), solved exactly so that it
//             is passive. Strings moving together lose energy to it quickly (the prompt sound), against each other hardly at all
//             (the after sound): the two-stage decay and the beating come out of the coupling.
//   phantom   sums and differences of the partials, from a string stretching as it swings wide.
//   sympathy  three unstruck strings (octave above and below, twelfth above), held nearly still by dampers until the pedal is down.
//   una       the soft pedal: the hammer misses one string of three.
//   variance  every blow a little different (the same every time from the same state).
//
// Monophonic like the other synths: the voices node gives each key its own copy.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kDecay = 0, kHard, kPos, kBright, kInharm, kDetune, kBoard, kPhantom, kSympathy, kUna, kVariance, kThump, kKnock, kWidth, kPedal, kRelease, kTune, kVel, kLevel, kQuality, kSing };

constexpr int kSize = 1 << 14, kMask = kSize - 1, kSections = 3, kMaxStr = 6;
constexpr int kModes = 40, kModesEco = 16, kHammerSub = 6;
// sing: the strings fed by their own bridge (see piano2.js)
constexpr double kSingGain = 1.6, kSingQ = 40, kSingJumpFrom = 0.6, kSingJumpTo = 0.9, kSingCapSeconds = 0.1;
constexpr double kHammerSeconds = 0.03, kCoupling = 0.03, kDry = 0.5;
constexpr double kHammerSpeed = 4, kFeltHysteresis = 1.2e-4;

double clampTo(double x, double a, double b) { return std::min(b, std::max(a, x)); }

// phase delay, in samples, of the first order allpass (a + z^-1) / (1 + a z^-1) at angular frequency w
double allpassDelay(double a, double w) {
  const double num = js::atan2(-js::sin(w), a + js::cos(w)), den = js::atan2(-a * js::sin(w), 1 + a * js::cos(w));
  return -(num - den) / w;
}

// piecewise linear table lookup
template <std::size_t N>
double lookup(const double (&table)[N][2], double x) {
  if (x <= table[0][0]) return table[0][1];
  for (std::size_t i = 1; i < N; i++) {
    if (x <= table[i][0]) {
      const double x0 = table[i - 1][0], y0 = table[i - 1][1], x1 = table[i][0], y1 = table[i][1];
      return y0 + (y1 - y0) * (x - x0) / (x1 - x0);
    }
  }
  return table[N - 1][1];
}

const double kHammerMass[5][2] = {{21, 9e-3}, {48, 5.5e-3}, {60, 4e-3}, {84, 2.8e-3}, {108, 2e-3}};   // kg by key
const double kFeltK[5][2] = {{21, 4e8}, {48, 2e9}, {60, 4.5e9}, {84, 1.5e10}, {108, 4e10}};            // N/m^p by key
const double kLoud[12][2] = {{21, 0.322}, {28, 0.277}, {36, 0.0826}, {44, 0.0796}, {52, 0.0406}, {60, 0.0526}, {68, 0.0773}, {76, 0.1306}, {84, 0.2257}, {92, 0.3396}, {100, 0.6}, {108, 1}};

// the string's impedance (kg/s) by key
double stringImpedance(double midi) { return clampTo(0.65 * js::pow(2, -(midi - 60) / 24), 0.05, 6); }

// The first blow of a high note is a spike far above the rest of the note; past 0.6 it is bent smoothly towards 1.
double softClip(double x) {
  const double a = std::abs(x);
  if (a < 0.6) return x;
  const double sign = x > 0 ? 1.0 : x < 0 ? -1.0 : 0.0;
  return sign * (0.6 + 0.4 * js::tanh((a - 0.6) / 0.4));
}

struct Mode { double b0, b2, a1, a2, weight; };

// A mode is a bandpass with 0 dB at its peak (a passive admittance); frequency, Q and weight are fixed by the index.
Mode boardMode(int j, int count, double fs) {
  const double t = static_cast<double>(j) / (count - 1);
  const double f = std::min(0.45 * fs, 70 * js::pow(6500.0 / 70, js::pow(t, 0.85)) * (1 + 0.06 * js::sin(j * 2.7 + 0.4)));
  const double q = 7 + 5 * js::sin(j * 1.3 + 0.5) + 16 * js::pow(1 - t, 2);
  const double weight = js::pow(f / 300, -0.25) * (0.6 + 0.4 * js::cos(j * 1.7));
  const double w0 = 2 * kPi * f / fs, al = js::sin(w0) / (2 * q), a0 = 1 + al;
  return {al / a0, -al / a0, -2 * js::cos(w0) / a0, (1 - al) / a0, weight};
}

class Piano2 final : public Node {
public:
  explicit Piano2(const NodeContext& context) : fs_(context.sampleRate), pitchOn_(!context.connected.empty() && context.connected[0]) {
    for (auto& b : buf_) b.assign(kSize, 0.0);
    for (int j = 0; j < kModes; j++) modes_[j] = boardMode(j, kModes, fs_);
    dcCoef_ = 1 - js::exp(-2 * kPi * 30 / fs_);
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { note_ = event.note; velocity_ = event.vel; pending_ = true; active_ = true; keyUp_ = false; }
    else if (event.type == NoteType::Off || event.type == NoteType::AllOff) keyUp_ = true;
    else if (event.type == NoteType::Cut) { active_ = false; keyUp_ = false; hamOn_ = false; ns_ = nt_ = 0; for (auto& b : buf_) std::fill(b.begin(), b.end(), 0.0); }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (!active_) { std::fill(outL, outL + n, 0.0f); std::fill(outR, outR + n, 0.0f); return; }
    auto g = [&](int i) -> double { return params[slots[i]]; };
    if (pending_) { pending_ = false; strike(g, pitchOn_ ? inputs[0][0] : 0.0); }
    const bool pedalDown = g(kPedal) >= 0.5;
    if (keyUp_ && !damped_ && !pedalDown) { damped_ = true; knockEnv_ = 1; }   // the damper falls, or the pedal comes up
    const double level = g(kLevel) * 2, phantom = g(kPhantom), knock = g(kKnock) * 0.08;
    const double zeta = 0.002 + g(kBoard) * kCoupling;
    const double symWeight = g(kSympathy) * 0.5;
    // the strings' loop gains: struck strings are open, or damped once the key is up; the others are held still without the pedal
    for (int s = 0; s < ns_; s++) loopNow_[s] = damped_ ? loopDamped_[s] : loopOpen_[s];
    for (int s = ns_; s < nt_; s++) loopNow_[s] = pedalDown ? loopOpen_[s] : loopSym_[s];
    for (int s = 0; s < ns_; s++) zw_[s] = 1;
    for (int s = ns_; s < nt_; s++) zw_[s] = symWeight;
    double zTotal = 0;
    for (int s = 0; s < nt_; s++) zTotal += zw_[s];
    double b0t = 0;
    for (int j = 0; j < modeCount_; j++) b0t += mc_[j] * mb0_[j];
    double peak = 0;
    const double dtS = 1 / (fs_ * kHammerSub);
    for (int i = 0; i < n; i++) {
      // the hammer, step by step through the sample, then its mean force into the strings at the strike point
      if (hamOn_) {
        double vs0 = 0;
        for (int s = 0; s < ns_; s++) vs0 += share_[s] * (buf_[s][static_cast<std::size_t>((w_ - tap_[s]) & kMask)] + buf_[s][static_cast<std::size_t>((w_ - delay_[s] + tap_[s]) & kMask)]);
        double force = lastForce_, sum = 0;
        for (int u = 0; u < kHammerSub; u++) {
          const double vs = vs0 + force * hamKappa_ / (hamZ_ * hamTotal_);   // the string's velocity at the point: the waves' plus the push of the felt
          const double x = yh_ - ys_, rate = vh_ - vs;
          if (x > 0) { const double xp = js::pow(x, hamP_); force = std::max(0.0, hamK_ * (xp + kFeltHysteresis * hamP_ * xp / x * rate)); } else force = 0;
          vh_ -= force / hamMass_ * dtS; yh_ += vh_ * dtS; ys_ += vs * dtS;
          sum += force;
        }
        lastForce_ = force;
        const double mean = sum / kHammerSub;
        for (int s = 0; s < ns_; s++) {
          const double inject = mean * share_[s] / (2 * hamZ_);
          buf_[s][static_cast<std::size_t>((w_ - tap_[s]) & kMask)] += inject; buf_[s][static_cast<std::size_t>((w_ - delay_[s] + tap_[s]) & kMask)] += inject;
        }
        if (++hamAge_ > hamEnd_ || (hamAge_ > 8 && yh_ - ys_ < -2e-3)) hamOn_ = false;
      }
      // the waves reaching the bridge
      double S = 0;
      for (int s = 0; s < nt_; s++) {
        std::vector<double>& b = buf_[s];
        double x = b[static_cast<std::size_t>((w_ - delay_[s]) & kMask)];
        double y = ia_[s] * x + ix_[s] - ia_[s] * iy_[s]; ix_[s] = x; iy_[s] = y; x = y;
        for (int q = 0; q < kSections; q++) { const int j = s * kSections + q; y = dispA_[s] * x + dx_[j] - dispA_[s] * dy_[j]; dx_[j] = x; dy_[j] = y; x = y; }
        lp_[s] = (1 - lc_[s]) * x + lc_[s] * lp_[s];
        arrive_[s] = lp_[s] * loopNow_[s];
        S += zw_[s] * arrive_[s];
      }
      // sing: the bridge motion, through the two narrow filters and the limiter, put back on the struck strings at the strike point
      if (singGain_ > 0) {
        const double sig = S - dcState_;
        if (singAge_ < singCapEnd_) singAge_++;
        double fed = 0;
        for (int p = 0; p < 2; p++) {
          const double y = singB0_[p] * (sig - singX2_[p]) - singA1_[p] * singY1_[p] - singA2_[p] * singY2_[p];
          singX2_[p] = singX1_[p]; singX1_[p] = sig; singY2_[p] = singY1_[p]; singY1_[p] = y;
          const double ay = std::abs(y);
          if (p == 0 && singAge_ < singCapEnd_ && ay > singCap_) singCap_ = ay;   // the level the feed may reach: the fundamental's, just after the blow
          singEnv_[p] = ay > singEnv_[p] ? ay : singEnv_[p] * 0.9998;
          const double held = std::min(singEnv_[p], singCap_);
          fed += (p == 0 ? 1 - singOctave_ : std::min(1.0, 2 * singOctave_)) * (singEnv_[p] > 1e-12 ? js::tanh(1.5 * y / singEnv_[p]) * held : 0);
        }
        if (!damped_) {
          const double inject = singGain_ * fed;
          for (int s = 0; s < ns_; s++) {
            const double part = inject * share_[s] * ns_;
            buf_[s][static_cast<std::size_t>((w_ - tap_[s]) & kMask)] += part; buf_[s][static_cast<std::size_t>((w_ - delay_[s] + tap_[s]) & kMask)] += part;
          }
        }
      }
      // the phantom partials: the square of the motion, without its mean, put back on the bridge force
      if (phantom > 0) {
        const double m = S * S; nonlinHp_ += 0.004 * (m - nonlinHp_);
        const double a = std::abs(S); nonlinEnv_ = a > nonlinEnv_ ? a : nonlinEnv_ * 0.99995;
        S += phantom * 0.5 * (m - nonlinHp_) / (nonlinEnv_ + 1e-9);
      }
      // the bridge junction, solved with the board's own direct term so that it is passive and exact
      double st = 0;
      for (int j = 0; j < modeCount_; j++) st += mc_[j] * (mb2_[j] * mx2_[j] - ma1_[j] * my1_[j] - ma2_[j] * my2_[j]);
      const double v = zeta * (2 * b0t * S + st) / (1 + zTotal * zeta * b0t);
      const double drive = 2 * S - zTotal * v;
      for (int j = 0; j < modeCount_; j++) {
        const double o = mb0_[j] * drive + mb2_[j] * mx2_[j] - ma1_[j] * my1_[j] - ma2_[j] * my2_[j];
        mx2_[j] = mx1_[j]; mx1_[j] = drive; my2_[j] = my1_[j]; my1_[j] = o;
      }
      for (int s = 0; s < nt_; s++) buf_[s][static_cast<std::size_t>(w_ & kMask)] = arrive_[s] - v;
      w_ = (w_ + 1) & kMask;
      // the key's thud and the damper's tick
      thumpLp_ += 0.35 * (rand() - thumpLp_);
      const double t = thumpLp_ * thumpEnv_ * thumpAmp_;
      thumpEnv_ *= thumpDecay_;
      double tick = 0;
      if (knockEnv_ > 1e-4) { knockLp_ += 0.5 * (rand() - knockLp_); tick = knockLp_ * knockEnv_ * knock; knockEnv_ *= 0.9965; }
      dcState_ += dcCoef_ * (S - dcState_);   // a struck string keeps a mean displacement, and a steady force on the bridge, which the board does not pass
      const double m = softClip(((v / zeta + kDry * 2 * (S - dcState_)) * outAmp_ + t + tick) * level);
      outL[i] = static_cast<float>(m * panL_); outR[i] = static_cast<float>(m * panR_);
      const double a = std::abs(m);
      if (a > peak) peak = a;
    }
    quiet_ = peak < 1e-6 ? quiet_ + 1 : 0;
    if (quiet_ > 8 && !hamOn_) active_ = false;
  }

private:
  double rand() {
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    return seed_ / 4294967296.0 * 2 - 1;
  }

  // Set the modes used for this note (every note meets the same board; eco takes a spread subset of it).
  void chooseModes(int count) {
    for (int j = 0; j < count; j++) {
      const Mode& m = modes_[static_cast<int>(jsRound(j * (kModes - 1) / static_cast<double>(std::max(1, count - 1))))];
      mb0_[j] = m.b0; mb2_[j] = m.b2; ma1_[j] = m.a1; ma2_[j] = m.a2; mc_[j] = m.weight;
    }
    double s = 0;
    for (int j = 0; j < count; j++) s += mc_[j];
    for (int j = 0; j < count; j++) mc_[j] /= s;
  }

  // Tune string s to frequency f (note number midiS) with the given 60 dB decay time (seconds) and brightness; returns the loop
  // lowpass's gain at the fundamental.
  double tuneString(int s, double f, double midiS, double t60, double cUser, double stiff) {
    const double fs = fs_;
    const double T0 = fs / f, w0 = 2 * kPi * f / fs, N6 = 6;
    const double B = stiff * 2 * 4e-4 * js::pow(2, (midiS - 60) / 18);
    const double want = 0.5 * js::log2(1 + B * N6 * N6);
    auto got = [&](double a) { return -js::log2(1 + kSections * (allpassDelay(a, N6 * w0) - allpassDelay(a, w0)) / T0); };
    double lo = 0, hi = T0 > 40 ? 0.95 : 0.4;
    for (int it = 0; it < 30; it++) { const double mid = (lo + hi) / 2; if (got(-mid) < want) lo = mid; else hi = mid; }
    dispA_[s] = -lo;
    const double target = js::pow(10, -3 * (T0 / fs) / t60);
    auto mag = [&](double c) { return (1 - c) / jsHypot(1 - c * js::cos(w0), c * js::sin(w0)); };
    double c = cUser;
    if (mag(c) < target / 0.99995) {
      double a = 0, b = c;
      for (int it = 0; it < 24; it++) { const double mid = (a + b) / 2; if (mag(mid) >= target / 0.99995) a = mid; else b = mid; }
      c = a;
    }
    lc_[s] = c;
    const double lpDelay = js::atan2(c * js::sin(w0), 1 - c * js::cos(w0)) / w0;
    const double D = T0 - lpDelay - kSections * allpassDelay(dispA_[s], w0);
    const int Dint = static_cast<int>(std::max(8.0, std::floor(D - 0.5)));
    double d = D - Dint;
    for (int it = 0; it < 4; it++) d += (D - Dint) - allpassDelay((1 - d) / (1 + d), w0);
    d = clampTo(d, 0.3, 1.9);
    ia_[s] = (1 - d) / (1 + d);
    delay_[s] = Dint;
    loopOpen_[s] = std::min(0.99995, target / mag(c));
    return mag(c);
  }

  // Set everything up for a note with the current parameters.
  template <typename G>
  void strike(G& g, double pitchOct) {
    const double fs = fs_;
    const bool quality = g(kQuality) >= 0.5;
    const double variance = g(kVariance);
    const double midi = note_ + g(kTune) + pitchOct * 12;
    const double f0 = 440 * js::pow(2, (midi - 69) / 12);
    const double pf = clampTo((midi - 21) / 87, 0, 1);
    ns_ = midi < 31 ? 1 : midi < 48 ? 2 : 3;
    double tuned[3];
    int tunedCount = 0;
    if (!quality) {
      for (const double r : {2.0, 3.0, 0.5}) if (f0 * r < 0.4 * fs && f0 * r > 20) tuned[tunedCount++] = r;
    }
    nt_ = ns_ + tunedCount;
    const double cUser = 0.62 + (0.03 - 0.62) * g(kBright);
    const double t60 = clampTo(g(kDecay) * js::pow(2, -(midi - 60) / 36), 0.3, 40);
    const double spread3[3] = {-1, 0, 1}, spread2[2] = {-1, 1}, spread1[1] = {0};
    const double* spread = ns_ == 1 ? spread1 : ns_ == 2 ? spread2 : spread3;
    for (int s = 0; s < kMaxStr; s++) { std::fill(buf_[s].begin(), buf_[s].end(), 0.0); ix_[s] = iy_[s] = lp_[s] = 0; }
    std::fill(std::begin(dx_), std::end(dx_), 0.0); std::fill(std::begin(dy_), std::end(dy_), 0.0);
    const double detune = g(kDetune);
    double spreadOf[3];
    for (int s = 0; s < 3; s++) spreadOf[s] = rand() * variance;
    for (int s = 0; s < ns_; s++) {
      const double cents = spread[s] * detune * (1 + 0.5 * spreadOf[s]) + 0.6 * spreadOf[s] * detune;
      const double f = f0 * js::pow(2, cents / 1200);
      tuneString(s, f, midi, t60 * (1 + 0.15 * spreadOf[s]), cUser, g(kInharm));
      loopDamped_[s] = std::min(loopOpen_[s], js::pow(10, -3 * (1 / f) / (g(kRelease) * 0.001)));
    }
    for (int i = 0; i < tunedCount; i++) {
      const int s = ns_ + i;
      const double r = tuned[i], fs2 = f0 * r;
      const double mag = tuneString(s, fs2, midi + 12 * js::log2(r), t60 * 1.2, cUser, g(kInharm));
      loopSym_[s] = std::min(0.99995, js::pow(10, -3 * (1 / fs2) / 0.8) / mag);   // held nearly still: 0.8 s to fall 60 dB
      loopDamped_[s] = loopSym_[s];
    }
    // the hammer: which way the blow is shared among the strings, where it strikes, how hard
    const double una = g(kUna);
    double total = 0, sq = 0;
    for (int s = 0; s < ns_; s++) { share_[s] = ns_ == 3 && s == 2 ? 1 - una : 1; total += share_[s]; sq += share_[s] * share_[s]; }
    for (int s = 0; s < ns_; s++) share_[s] = share_[s] / std::max(total, 1e-6);
    hamKappa_ = sq / std::max(total * total, 1e-6);
    hamTotal_ = total;
    const double pos = clampTo(g(kPos) * (1 + 0.25 * variance * rand()), 0.03, 0.5);
    for (int s = 0; s < ns_; s++) tap_[s] = static_cast<int>(std::max(1.0, jsRound(pos * delay_[s] / 2)));
    const double vm = 1 - g(kVel) + g(kVel) * velocity_;
    hamMass_ = lookup(kHammerMass, midi);
    hamP_ = 2.5 + 0.8 * pf;
    hamK_ = lookup(kFeltK, midi) * js::exp(3.66 * (g(kHard) - 0.5)) * (1 + 0.06 * variance * rand());
    hamZ_ = stringImpedance(midi);
    yh_ = ys_ = 0; vh_ = std::max(0.05, vm) * kHammerSpeed * (0.4 + 0.6 * vm); lastForce_ = 0;
    hamOn_ = true; hamAge_ = 0; hamEnd_ = static_cast<int>(jsRound(kHammerSeconds * fs));
    for (int j = 0; j < kModes; j++) mx1_[j] = mx2_[j] = my1_[j] = my2_[j] = 0;
    modeCount_ = quality ? kModesEco : kModes; chooseModes(modeCount_);
    nonlinHp_ = 0; nonlinEnv_ = 1e-6; dcState_ = 0;
    thumpEnv_ = 1; thumpDecay_ = js::exp(-1 / (0.012 * fs)); thumpAmp_ = g(kThump) * vm * vm * 0.5;
    knockEnv_ = 0; knockLp_ = 0;
    outAmp_ = lookup(kLoud, 21 + pf * 87);
    {
      const double b = g(kSing), jump = clampTo((b - kSingJumpFrom) / (kSingJumpTo - kSingJumpFrom), 0, 1);
      singOctave_ = jump * jump * (3 - 2 * jump);
      singGain_ = b * b * kSingGain / static_cast<double>(std::max(1, delay_[0]));
      singCap_ = 0; singAge_ = 0; singCapEnd_ = static_cast<int>(jsRound(kSingCapSeconds * fs));
      for (int p = 0; p < 2; p++) {
        const double w0 = 2 * kPi * f0 * (p + 1) / fs, al = js::sin(w0) / (2 * kSingQ), a0 = 1 + al;
        singB0_[p] = al / a0; singA1_[p] = -2 * js::cos(w0) / a0; singA2_[p] = (1 - al) / a0;
        singX1_[p] = singX2_[p] = singY1_[p] = singY2_[p] = singEnv_[p] = 0;
      }
    }
    const double place = 0.5 + (pf - 0.5) * g(kWidth);
    panL_ = js::cos(place * kPi / 2) * kSqrt2; panR_ = js::sin(place * kPi / 2) * kSqrt2;
    damped_ = false; quiet_ = 0;
  }

  double fs_;
  bool pitchOn_;
  std::vector<double> buf_[kMaxStr];
  // per string: the interpolating allpass, the stiffness chain, the loop lowpass, the loop gains and the strike taps
  double ia_[kMaxStr] = {}, ix_[kMaxStr] = {}, iy_[kMaxStr] = {}, lp_[kMaxStr] = {}, lc_[kMaxStr] = {};
  double dispA_[kMaxStr] = {}, dx_[kMaxStr * kSections] = {}, dy_[kMaxStr * kSections] = {};
  int delay_[kMaxStr] = {}, tap_[kMaxStr] = {};
  double loopOpen_[kMaxStr] = {}, loopDamped_[kMaxStr] = {}, loopSym_[kMaxStr] = {};
  double share_[kMaxStr] = {};     // the hammer's share of the blow on each struck string
  double arrive_[kMaxStr] = {};    // the wave reaching the bridge from each string
  double loopNow_[kMaxStr] = {}, zw_[kMaxStr] = {};   // this block's loop gains, and each string's weight on the bridge
  int ns_ = 0, nt_ = 0, w_ = 0;
  int note_ = 60;
  double velocity_ = 1;
  bool active_ = false, keyUp_ = false, damped_ = false, pending_ = false;
  // the hammer
  bool hamOn_ = false;
  int hamAge_ = 0, hamEnd_ = 0;
  double yh_ = 0, vh_ = 0, ys_ = 0, hamMass_ = 0.004, hamK_ = 1e9, hamP_ = 2.8, hamZ_ = 0.65, hamKappa_ = 1, hamTotal_ = 1, lastForce_ = 0;
  // the board: the fixed modes, and the ones in use with their states
  std::array<Mode, kModes> modes_{};
  double mx1_[kModes] = {}, mx2_[kModes] = {}, my1_[kModes] = {}, my2_[kModes] = {};
  double mb0_[kModes] = {}, mb2_[kModes] = {}, ma1_[kModes] = {}, ma2_[kModes] = {}, mc_[kModes] = {};
  int modeCount_ = 0;
  std::uint32_t seed_ = 0x2545f491u;
  double dcCoef_ = 0, dcState_ = 0, nonlinHp_ = 0, nonlinEnv_ = 1e-6;
  double thumpEnv_ = 0, thumpDecay_ = 0, thumpAmp_ = 0, thumpLp_ = 0, knockEnv_ = 0, knockLp_ = 0;
  double quiet_ = 0, panL_ = 1, panR_ = 1, outAmp_ = 1;
  // sing: a narrow bandpass on the fundamental and one on the octave (RBJ, 0 dB at the peak), each with a level follower
  double singB0_[2] = {}, singA1_[2] = {}, singA2_[2] = {}, singX1_[2] = {}, singX2_[2] = {}, singY1_[2] = {}, singY2_[2] = {}, singEnv_[2] = {};
  double singGain_ = 0, singOctave_ = 0, singCap_ = 0;
  int singAge_ = 0, singCapEnd_ = 0;
};

}  // namespace

void registerPiano2(NodeRegistry& registry) {
  NodeType type;
  type.type = "piano2";
  type.inputs = {"pitch"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"decay", 0.5, 40, 14},        // seconds for the after sound to fall 60 dB at middle C
    {"hard", 0, 1, 0.6},
    {"pos", 0.04, 0.5, 0.12},
    {"bright", 0, 1, 0.75},
    {"inharm", 0, 1, 0.5},
    {"detune", 0, 15, 2.2},        // cents between the strings of a note
    {"board", 0, 1, 0.45},         // how far the bridge gives: the coupling of the strings and the soundboard
    {"phantom", 0, 1, 0.3},
    {"sympathy", 0, 1, 0.3},
    {"una", 0, 1, 0},
    {"variance", 0, 1, 0.3},
    {"thump", 0, 1, 0.25},
    {"knock", 0, 1, 0.3},
    {"width", 0, 1, 0.5},
    {"pedal", 0, 1, 0},
    {"release", 20, 3000, 250},    // ms the damper takes to still a released string
    {"tune", -48, 48, 0},
    {"vel", 0, 1, 1},
    {"level", 0, 1, 0.5},
    {"quality", 0, 1, 0, true},    // 0 standard, 1 eco
    {"sing", 0, 1, 0},             // the strings fed by their own bridge: 0 a plain piano, 1 an overblown note
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Piano2>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

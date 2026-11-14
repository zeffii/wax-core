// Fourpole: a synth after the design of Fuzzpilz's Buzz machine Orange Kid. Two table oscillators (ten waves, mixed
// 1+2, 1-2, 2-1, 1*2, 1/2, 2/1), two linear ADSRs, a vibrato, glide, and four resonant two-pole filters (lowpass or
// highpass, each able to slave to filter 1) in eight routings. What an envelope moves follows the machine's law:
// modulated = normal * (1 + amount * envelope). One voice (put it behind `voices`).
// Waves are 32768-point tables read without interpolation, like the original. Port of webtracker
// src/audio/nodes/fourpole.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

constexpr int N = 32768;

enum Param {
  kWave1 = 0, kWave2, kSeed, kPhase, kTranspose, kDetune, kMix, kPstart, kPend, kAtk, kDec, kSus, kRel, kAtk2, kDec2,
  kSus2, kRel2, kF1Type, kF1Cut, kF1Res, kF2Type, kF2Cut, kF2Res, kF3Type, kF3Cut, kF3Res, kF4Type, kF4Cut, kF4Res,
  kRoute, kLimit, kInertia, kGlide, kVibDepth, kVibRate, kE1vr, kE1vd, kE1cut, kE1res, kE1pit, kE2vr, kE2vd, kE2cut,
  kE2res, kE2pit, kE2amp, kTuning, kDetail, kVel, kLevel
};

// ---- wave tables (shared by every voice) ------------------------------------------------------------------------
struct Table {
  std::vector<double> t, prefix;   // prefix: so the mean of any window is one subtraction
};

void lowpass4(std::vector<double>& t) {   // RBJ lowpass, w0 = 0.002 pi, Q 0.5, four passes
  const double w0 = 0.002 * kPi, c = js::cos(w0), al = js::sin(w0), a0 = 1 + al;
  const double b0 = (1 - c) / 2 / a0, b1 = (1 - c) / a0, a1 = 2 * c / a0, a2 = -(1 - al) / a0;
  for (int p = 0; p < 4; p++) {
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (int i = 0; i < N; i++) {
      const double x = t[static_cast<std::size_t>(i)], y = b0 * x + b1 * x1 + b0 * x2 + a1 * y1 + a2 * y2;
      x2 = x1; x1 = x; y2 = y1; y1 = y;
      t[static_cast<std::size_t>(i)] = y;
    }
  }
}

class MakeRand {
public:
  explicit MakeRand(std::uint32_t seed) : s_(seed) {}
  double operator()() {
    s_ = s_ * 214013u + 2531011u;
    return static_cast<double>((s_ >> 16) & 0x7fff);
  }
private:
  std::uint32_t s_;
};

std::shared_ptr<const Table> buildTable(int wave, int seed) {
  static std::mutex mutex;
  static std::map<int, std::shared_ptr<const Table>> cache;
  static std::deque<int> order;
  const int key = wave * 100000 + (wave >= 8 ? seed : 0);
  std::lock_guard<std::mutex> lock(mutex);
  const auto found = cache.find(key);
  if (found != cache.end()) return found->second;

  auto table = std::make_shared<Table>();
  std::vector<double>& t = table->t;
  t.assign(N, 0.0);
  if (wave == 0) {
    for (int i = 0; i < N; i++) t[i] = js::sin(kPi * i / 16384);
  } else if (wave == 7) {
    for (int i = 0; i < N; i++) t[i] = js::cos(kPi * i / 32768);
  } else {
    if (wave == 1) {
      for (int i = 0; i < N; i++) {
        if (i < 8192) t[i] = i / 8192.0;
        else if (i < 16384) t[i] = (16384 - i) / 8192.0;
        else t[i] = -t[i - 16384];
      }
    } else if (wave == 2) {
      for (int i = 0; i < N; i++) t[i] = i < 16384 ? 1 : -1;
    } else if (wave == 3) {
      for (int i = 0; i < N; i++) t[i] = (i - 16384) / 16384.0;
    } else if (wave == 4) {
      for (int i = 0; i < N; i++) t[i] = (16384 - i) / 16384.0;
    } else if (wave == 5 || wave == 6) {
      for (int i = 0; i < 16384; i++) t[i] = i < 8192 ? (wave == 5 ? i / 8192.0 : (i - 16384) * 3 / 32768.0) : (8192 - i) / 8192.0;
      for (int i = 16384; i < N; i++) t[i] = -t[i - 16384];
    } else if (wave == 8) {
      MakeRand r(static_cast<std::uint32_t>(seed));
      for (int i = 0; i < N; i++) t[i] = r() / 16384 - 1;
    } else {   // noise 2: five random points, cosine-joined, mirrored
      MakeRand r(static_cast<std::uint32_t>(seed));
      double pt[5];
      for (int k = 0; k < 5; k++) pt[k] = r() / 16384 - 1;
      for (int k = 0; k < 4; k++) {
        for (int j = 0; j < 4096; j++) t[k * 4096 + j] = pt[k + 1] + (pt[k] - pt[k + 1]) * (1 + js::cos(kPi * j / 4096)) / 2;
      }
      for (int i = 16384; i < N; i++) t[i] = t[N - 1 - i];
      double m = 0;
      for (int i = 0; i < N; i++) m += t[i];
      m /= N;
      for (int i = 0; i < N; i++) t[i] -= m;
    }
    lowpass4(t);
    if (wave >= 8) {   // the smoothing leaves noise very quiet; bring it to full scale
      double m = 0;
      for (int i = 0; i < N; i++) m = std::max(m, std::abs(t[i]));
      if (m > 0) for (int i = 0; i < N; i++) t[i] /= m;
    }
  }
  table->prefix.assign(N + 1, 0.0);
  for (int i = 0; i < N; i++) table->prefix[i + 1] = table->prefix[i] + t[i];
  if (cache.size() > 24) { cache.erase(order.front()); order.pop_front(); }
  cache[key] = table;
  order.push_back(key);
  return table;
}

// ---- parts ------------------------------------------------------------------------------------------------------
constexpr double kLimits[7] = {22050, 12000, 6000, 3000, 1500, 750, 325};
const double kNoteMult[7] = {1, 2, 3, 4, 1.0 / 2, 1.0 / 3, 1.0 / 4};
// for each route: stages of filter indices; the outputs of one stage are summed and feed the next
const std::vector<std::vector<int>> kRoutes[8] = {
  {{0}, {1}, {2}, {3}}, {{0, 1}, {2}, {3}}, {{0}, {1}, {2, 3}}, {{0}, {1, 2}, {3}},
  {{0}, {1, 2, 3}}, {{0, 1}, {2, 3}}, {{0, 1, 2}, {3}}, {{0, 1, 2, 3}},
};

class Env {   // linear ADSR over 0..1; release falls at the rate that would empty it in `rel` samples
public:
  int stage = 0;
  void set(double sr, double atk, double dec, double sus, double rel) {
    const double ms = sr * 0.001;
    a_ = 1 / std::max(1.0, atk * ms); d_ = (1 - sus) / std::max(1.0, dec * ms); s_ = sus; r_ = 1 / std::max(1.0, rel * ms);
  }
  void on() { stage = 1; }
  void off() { if (stage) stage = 4; }
  void cut() { stage = 0; level_ = 0; }
  double next() {
    switch (stage) {
      case 1: level_ += a_; if (level_ >= 1) { level_ = 1; stage = 2; } break;
      case 2: level_ -= d_; if (level_ <= s_) { level_ = s_; stage = 3; } break;
      case 4: level_ -= r_; if (level_ < 1.0 / 2048) { level_ = 0; stage = 0; } break;
      default: break;
    }
    return level_;
  }
private:
  double level_ = 0, a_ = 1, d_ = 1, s_ = 0, r_ = 1;
};

double denOf(double d) { return std::abs(d) < 0.01 ? (d < 0 ? -0.01 : 0.01) : d; }

class Fourpole final : public Node {
public:
  explicit Fourpole(const NodeContext& context)
      : sr_(context.sampleRate), pitchOn_(context.connected[0]), cutOn_(context.connected[1]) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      if (!active_ || e1_.stage == 0) { reset(); pos_ = event.note; }
      note_ = event.note; velocity_ = event.vel; active_ = true;
      e1_.on(); e2_.on(); cnt_ = 0;
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) {
      e1_.off(); e2_.off();
    } else {
      e1_.cut(); e2_.cut(); active_ = false;
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (!active_ || e1_.stage == 0) {
      std::fill(outL, outL + n, 0.0f);
      std::fill(outR, outR + n, 0.0f);
      active_ = false;
      return;
    }
    const double sr = sr_;
    const auto g = [&](Param p) { return static_cast<double>(params[slots[p]]); };
    e1_.set(sr, g(kAtk), g(kDec), g(kSus), g(kRel));
    e2_.set(sr, g(kAtk2), g(kDec2), g(kSus2), g(kRel2));
    const int w1 = jsRoundInt(g(kWave1)), w2 = jsRoundInt(g(kWave2)), seed = jsRoundInt(g(kSeed));
    const double ps = std::min(g(kPstart), g(kPend)), pe = std::max(g(kPstart), g(kPend));
    const int lo = std::min(N - 1, static_cast<int>(std::floor(ps * N)));
    const int hi = std::max(lo + 1, std::min(N, static_cast<int>(std::ceil(pe * N))));
    const int span = hi - lo;
    if (!haveTabs_ || w1 != cw1_ || w2 != cw2_ || seed != cseed_) {
      haveTabs_ = true; cw1_ = w1; cw2_ = w2; cseed_ = seed;
      tabs1_ = buildTable(w1, seed);
      tabs2_ = w2 < 10 ? buildTable(w2, seed) : nullptr;
    }
    const double mean1 = (tabs1_->prefix[static_cast<std::size_t>(hi)] - tabs1_->prefix[static_cast<std::size_t>(lo)]) / span;
    const double mean2 = tabs2_ ? (tabs2_->prefix[static_cast<std::size_t>(hi)] - tabs2_->prefix[static_cast<std::size_t>(lo)]) / span : 0;
    const double* T1 = tabs1_->t.data();
    const double* T2 = tabs2_ ? tabs2_->t.data() : nullptr;
    const int mixMode = jsRoundInt(g(kMix));
    const double tuning = g(kTuning);
    const int detail = std::max(1, jsRoundInt(g(kDetail)));
    const double transpose = g(kTranspose) + g(kDetune) / 100, phOff = g(kPhase);
    const double glideMs = g(kGlide);
    if (glideMs > 0.5) pos_ += (note_ - pos_) * (1 - js::exp(-n / (sr * glideMs * 0.001 / 3)));
    else pos_ = note_;
    const double vd0 = g(kVibDepth), vibDepth = 0.95 * vd0 * vd0 * vd0, vibRate = g(kVibRate);
    const double e1vr = g(kE1vr), e1vd = g(kE1vd), e1cut = g(kE1cut), e1res = g(kE1res), e1pit = g(kE1pit);
    const double e2vr = g(kE2vr), e2vd = g(kE2vd), e2cut = g(kE2cut), e2res = g(kE2res), e2pit = g(kE2pit), e2amp = g(kE2amp);
    const std::vector<std::vector<int>>& route = kRoutes[jsRoundInt(g(kRoute))];
    const int limit = jsRoundInt(g(kLimit));
    const double inertia = g(kInertia);
    const double level = g(kLevel) * (1 - g(kVel) + g(kVel) * velocity_);
    const auto ft = [&](Param p) { const double t = g(p); return t == 0 ? 0.0 : t == 1 ? -1.0 : t - 1; };   // 0 off, -1 slave, 1 lowpass, 2 highpass
    const double types[4] = {g(kF1Type), ft(kF2Type), ft(kF3Type), ft(kF4Type)};
    const double cuts[4] = {g(kF1Cut), g(kF2Cut), g(kF3Cut), g(kF4Cut)}, ress[4] = {g(kF1Res), g(kF2Res), g(kF3Res), g(kF4Res)};
    const double baseHz = tuning * js::pow(2, (pos_ - 69) / 12);
    const float* pitchIn = inputs[0];
    const float* cutIn = inputs[1];
    for (int i = 0; i < n; i++) {
      const double a = e1_.next(), b = e2_.next();
      if (cnt_ == 0) {
        cnt_ = detail;
        const double vr = vibRate * (1 + e1vr * a) * (1 + e2vr * b), vd = vibDepth * (1 + e1vd * a) * (1 + e2vd * b);
        m1_ = vd; m2_ = vr;
        const double pm = (1 + e1pit * a) * (1 + e2pit * b);
        const double fm = pitchOn_ ? js::pow(2, pitchIn[i]) : 1;
        inc_ = baseHz * pm * fm / sr;
        amp_ = a * (1 + e2amp * b);
        const double noteHz = baseHz * pm * fm;
        const double cm = (1 + e1cut * a) * (1 + e2cut * b) * (cutOn_ ? js::pow(2, cutIn[i]) : 1), rm = (1 + e1res * a) * (1 + e2res * b);
        const double maxHz = limit < 7 ? kLimits[limit] : noteHz * kNoteMult[limit - 7];
        const double k = inertia > 0 ? 1 - js::exp(-detail / (sr * inertia * 0.001)) : 1;
        for (int f = 0; f < 4; f++) {
          const double ty = types[f] < 0 ? types[0] : types[f];
          on_[f] = ty > 0;
          if (!on_[f]) continue;
          const double cp = f > 0 && cuts[f] > 1000 ? cuts[0] : cuts[f], rp = f > 0 && ress[f] < 50 ? ress[0] : ress[f];
          const double hz = std::min(std::min(22000.0, sr * 0.49), std::max(20.0, cp / 1000 * maxHz * cm));
          const double q = std::min(127.0, std::max(1.0, rp * 0.02 * rm));
          if (qNow_[f] > 0) { cutHz_[f] += (hz - cutHz_[f]) * k; qNow_[f] += (q - qNow_[f]) * k; }
          else { cutHz_[f] = hz; qNow_[f] = q; }
          coef(f, cutHz_[f], qNow_[f], ty == 2);
        }
      }
      cnt_--;
      // oscillators
      lfo_ += m2_ / sr; if (lfo_ >= 1) lfo_ -= 1;
      const double step = inc_ * (1 + m1_ * js::sin(2 * kPi * lfo_));
      const double s1 = T1[std::min(N - 1, static_cast<int>(lo + ph1_ * span))] - mean1;
      double v = s1;
      if (T2) {
        double p2 = ph2_ + phOff;
        p2 -= std::floor(p2);
        const double s2 = T2[std::min(N - 1, static_cast<int>(lo + p2 * span))] - mean2;
        switch (mixMode) {
          case 0: v = s1 + s2; break;
          case 1: v = s1 - s2; break;
          case 2: v = s2 - s1; break;
          case 3: v = s1 * s2; break;
          case 4: v = std::max(-4.0, std::min(4.0, s1 / denOf(s2))); break;
          default: v = std::max(-4.0, std::min(4.0, s2 / denOf(s1)));
        }
      }
      ph1_ += step; ph1_ -= std::floor(ph1_);
      ph2_ += step * js::pow(2, transpose / 12); ph2_ -= std::floor(ph2_);
      // filters
      double x = v;
      for (const std::vector<int>& stage : route) {
        double sum = 0;
        bool any = false;
        for (const int f : stage) if (on_[f]) { sum += run(f, x); any = true; }
        if (any) x = sum;
      }
      const float o = static_cast<float>(x * amp_ * level);
      outL[i] = o;
      outR[i] = o;
    }
  }

private:
  void reset() {
    for (int f = 0; f < 4; f++) { x1_[f] = x2_[f] = y1_[f] = y2_[f] = 0; qNow_[f] = 0; }
    ph1_ = ph2_ = lfo_ = 0;
  }

  void coef(int i, double hz, double q, bool hpass) {
    const double w0 = 2 * kPi * hz / sr_, c = js::cos(w0), al = js::sin(w0) / q, a0 = 1 + al;
    b0_[i] = (1 - c) / 2 / a0; b1_[i] = (1 - c) / a0; a1_[i] = 2 * c / a0; a2_[i] = -(1 - al) / a0; hp_[i] = hpass;
  }

  double run(int i, double x) {
    const double y = b0_[i] * x + b1_[i] * x1_[i] + b0_[i] * x2_[i] + a1_[i] * y1_[i] + a2_[i] * y2_[i];
    x2_[i] = x1_[i]; x1_[i] = x; y2_[i] = y1_[i];
    const double v = std::abs(y) < 1e-18 ? 0 : y;
    y1_[i] = v;
    return hp_[i] ? x - v : v;
  }

  double sr_;
  bool pitchOn_, cutOn_;
  Env e1_, e2_;
  double x1_[4] = {}, x2_[4] = {}, y1_[4] = {}, y2_[4] = {};
  double b0_[4] = {}, b1_[4] = {}, a1_[4] = {}, a2_[4] = {};
  bool hp_[4] = {}, on_[4] = {};
  double cutHz_[4] = {}, qNow_[4] = {};
  double ph1_ = 0, ph2_ = 0, lfo_ = 0, note_ = 60, pos_ = 60, velocity_ = 1;
  bool active_ = false;
  int cnt_ = 0;
  bool haveTabs_ = false;
  int cw1_ = 0, cw2_ = 0, cseed_ = 0;
  std::shared_ptr<const Table> tabs1_, tabs2_;
  double amp_ = 0, m1_ = 0, m2_ = 0, inc_ = 0;
};

}  // namespace

void registerFourpole(NodeRegistry& registry) {
  NodeType type;
  type.type = "fourpole";
  type.inputs = {"pitch", "cut"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"cut", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"wave1", 0, 9, 0, true},        // sine, triangle, square, upsaw, downsaw, sawoid 1-3, noise 1, noise 2
    {"wave2", 0, 10, 10, true},      // as wave 1; 10 is off
    {"seed", 0, 65534, 1, true},     // seed of the two noise waves
    {"phase", 0, 1, 0},              // phase of oscillator 2 against 1, in cycles
    {"transpose", -36, 36, 0, true}, // semitones, oscillator 2 against 1
    {"detune", -100, 100, 0},        // cents, oscillator 2 against 1
    {"mix", 0, 5, 0, true},          // 1+2, 1-2, 2-1, 1*2, 1/2, 2/1
    {"pstart", 0, 1, 0},             // part of the wave table one cycle plays
    {"pend", 0, 1, 1},
    {"atk", 0.14, 46000, 16},        // envelope 1 (amplitude), ms
    {"dec", 0.14, 46000, 64},
    {"sus", 0, 1, 0.25},
    {"rel", 0.14, 46000, 64},
    {"atk2", 0.14, 46000, 16},       // envelope 2 (modulation), ms
    {"dec2", 0.14, 46000, 64},
    {"sus2", 0, 1, 0.25},
    {"rel2", 0.14, 46000, 64},
    {"f1type", 0, 2, 1, true},       // off, lowpass, highpass
    {"f1cut", 0, 1000, 1000, true},
    {"f1res", 50, 1000, 50, true},
    {"f2type", 0, 3, 0, true},       // off, slave, lowpass, highpass
    {"f2cut", 0, 1001, 1001, true},
    {"f2res", 49, 1000, 49, true},
    {"f3type", 0, 3, 0, true},
    {"f3cut", 0, 1001, 1001, true},
    {"f3res", 49, 1000, 49, true},
    {"f4type", 0, 3, 0, true},
    {"f4cut", 0, 1001, 1001, true},
    {"f4res", 49, 1000, 49, true},
    {"route", 0, 7, 0, true},        // 1>2>3>4, 1+2>3>4, 1>2>3+4, 1>2+3>4, 1>2+3+4, 1+2>3+4, 1+2+3>4, 1+2+3+4
    {"limit", 0, 13, 0, true},       // what a cutoff of 100% means: 22050 12000 6000 3000 1500 750 325 Hz, note, x2, x3, x4, /2, /3, /4
    {"inertia", 0, 8192, 0},         // ms the filters take to follow a change
    {"glide", 0, 8192, 0},           // ms
    {"vibdepth", -1, 1, 0},          // vibrato depth (cubic: small values are subtle)
    {"vibrate", 0, 22, 5},           // Hz
    {"e1vr", -1, 1, 0},              // envelope 1 -> vibrato rate, depth, cutoff, resonance, pitch
    {"e1vd", -1, 1, 0},
    {"e1cut", -1, 4, 0},
    {"e1res", -1, 4, 0},
    {"e1pit", -1, 4, 0},
    {"e2vr", -1, 1, 0},              // envelope 2 -> the same, and amplitude
    {"e2vd", -1, 1, 0},
    {"e2cut", -1, 4, 0},
    {"e2res", -1, 4, 0},
    {"e2pit", -1, 4, 0},
    {"e2amp", -1, 4, 0},
    {"tuning", 420, 460, 440},       // Hz of A
    {"detail", 1, 256, 16, true},    // samples between modulation updates
    {"vel", 0, 1, 0.5},
    {"level", 0, 2, 0.5},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Fourpole>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Amp: a valve amplifier chain in one node (preamp stages, tone stack, power stage with sag, depth and presence, cabinet).
// Stereo in and out, two independent sides. Port of webtracker src/audio/nodes/amp.js.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/oversample.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kGain = 0, kStages, kBright, kBias, kBass, kMid, kTreble, kVoicing, kMaster, kSag, kPresence, kDepth, kCab, kCabtone, kLevel, kMix, kOversample };

struct Stack { double bass, mid, q, treble; };
const Stack kStacks[3] = {{100, 420, 0.6, 3500}, {170, 650, 0.8, 2600}, {130, 1200, 0.7, 4200}};

enum Kind { None, Hp, Lp, Peak, Low, High };
struct CabFilter { Kind kind; double f, q, g; };
const std::vector<CabFilter> kCabs[4] = {
  {},
  {{Hp, 85, 0.7, 0}, {Peak, 120, 1.2, 3}, {Peak, 2400, 1, 4}, {Lp, 5500, 0.7, 0}, {Lp, 5500, 0.7, 0}},
  {{Hp, 70, 0.7, 0}, {Peak, 110, 1, 5}, {Peak, 1800, 0.9, 3}, {Peak, 3300, 1.4, 2}, {Lp, 5000, 0.7, 0}, {Lp, 5000, 0.7, 0}},
  {{Hp, 75, 0.7, 0}, {Peak, 100, 0.9, 4}, {Peak, 750, 0.8, -2}, {Peak, 2600, 1.4, 5}, {Lp, 4500, 0.7, 0}, {Lp, 4500, 0.7, 0}},
};
constexpr int kCabSlots = 6;

using Coefs = std::array<double, 5>;
const Coefs kThrough = {1, 0, 0, 0, 0};

double db(double x) { return js::pow(10, x / 20); }

struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void set(const Coefs& c) { b0 = c[0]; b1 = c[1]; b2 = c[2]; a1 = c[3]; a2 = c[4]; }
  double run(double x) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

Coefs coefficients(Kind type, double f, double q, double gainDb, double sr) {
  const double fc = std::max(10.0, std::min(sr * 0.45, f)), w = kTwoPi * fc / sr, cs = js::cos(w), sn = js::sin(w);
  const double A = js::pow(10, gainDb / 40), alpha = sn / (2 * q);
  double b0, b1, b2, a0, a1, a2;
  switch (type) {
    case Hp: b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = b0; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
    case Lp: b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = b0; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
    case Peak: b0 = 1 + alpha * A; b1 = -2 * cs; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cs; a2 = 1 - alpha / A; break;
    case Low: {
      const double r = 2 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1) - (A - 1) * cs + r); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - r);
      a0 = (A + 1) + (A - 1) * cs + r; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - r;
      break;
    }
    default: {
      const double r = 2 * std::sqrt(A) * alpha;
      b0 = A * ((A + 1) + (A - 1) * cs + r); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - r);
      a0 = (A + 1) - (A - 1) * cs + r; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - r;
    }
  }
  return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

double onePole(double f, double sr) { return 1 - js::exp(-kTwoPi * std::min(f, sr * 0.45) / sr); }

struct Preamp {
  double gains[4] = {1, 1, 1, 1};
  int stages = 2;
  double bias = 0.3;
  double hp[4] = {0, 0, 0, 0}, lp[4] = {0, 0, 0, 0}, hpK[4] = {0, 0, 0, 0}, lpK = 0;
  double tick(double x) {
    for (int i = 0; i < stages; i++) {
      x *= gains[i];
      hp[i] += (x - hp[i]) * hpK[i];
      x -= hp[i];
      const double b = i == 0 ? bias : bias * 0.7;
      const double tb = js::tanh(b);
      x = (js::tanh(x + b) - tb) / (1 - js::pow(tb, 2));
      lp[i] += (x - lp[i]) * lpK;
      x = lp[i];
    }
    return x;
  }
};

struct Power {
  double g = 1, norm = 1, sag = 0, env = 0, atk = 0, rel = 0;
  double tick(double x) {
    const double a = std::fabs(x);
    env += (a - env) * (a > env ? atk : rel);
    const double gg = g / (1 + sag * 2 * env);
    return js::tanh(x * gg) * norm;
  }
};

struct Side {
  Side() : os1{Oversampler(1), Oversampler(2), Oversampler(4)}, os2{Oversampler(1), Oversampler(2), Oversampler(4)}, delay(160) {}
  Oversampler os1[3], os2[3];
  Preamp pre;
  Power power;
  FixedDelay delay;
  double inHp = 0, bright = 0;
  Biquad tone[3], out[2], cab[kCabSlots];
};

class Amp final : public Node {
public:
  explicit Amp(const NodeContext& context) : sampleRate_(context.sampleRate) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double sampleRate = sampleRate_;
    const double gain = p(kGain);
    const int stages = std::min(4, std::max(1, jsRoundInt(p(kStages))));
    const int os = std::min(2, std::max(0, jsRoundInt(p(kOversample))));
    const double factors[3] = {1, 2, 4};
    const double rate = sampleRate * factors[os];
    const double mix = p(kMix), level = p(kLevel), bright = p(kBright);
    const double total = (gain / 10) * 52;
    const double perStage = db(total / stages);
    const Stack& stack = kStacks[std::min(2, std::max(0, jsRoundInt(p(kVoicing))))];
    auto knob = [](double v) { return ((v - 5) / 5) * 14; };
    const Coefs toneCoefs[3] = {coefficients(Low, stack.bass, 0.7, knob(p(kBass)), sampleRate), coefficients(Peak, stack.mid, stack.q, knob(p(kMid)), sampleRate), coefficients(High, stack.treble, 0.7, knob(p(kTreble)), sampleRate)};
    const Coefs outCoefs[2] = {coefficients(Low, 95, 0.7, (p(kDepth) / 10) * 8, sampleRate), coefficients(High, 4500, 0.7, (p(kPresence) / 10) * 9, sampleRate)};
    const int cabIndex = std::min(3, std::max(0, jsRoundInt(p(kCab))));
    const double cabTone = p(kCabtone);
    const std::vector<CabFilter>& cabSpec = kCabs[cabIndex];
    Coefs cabCoefs[kCabSlots];
    for (int i = 0; i < kCabSlots; i++) {
      if (i >= static_cast<int>(cabSpec.size())) { cabCoefs[i] = kThrough; continue; }
      const CabFilter& s = cabSpec[static_cast<std::size_t>(i)];
      cabCoefs[i] = coefficients(s.kind, s.kind == Lp ? s.f * (0.7 + 0.6 * cabTone) : s.f, s.q, s.kind == Peak && s.f > 1000 ? s.g * (0.5 + cabTone) : s.g, sampleRate);
    }
    const double g0 = 0.5 + (p(kMaster) / 10) * 7.5;
    const double inK = onePole(28, sampleRate), brightK = onePole(1500, sampleRate);
    const double brightAmount = bright * 1.6 * (1 - gain / 10);
    for (int c = 0; c < 2; c++) {
      Side& s = sides_[c];
      const float* x = inputs[c];
      float* out = outputs[c];
      Preamp& pre = s.pre;
      Power& pw = s.power;
      pre.stages = stages; pre.bias = p(kBias);
      for (int i = 0; i < 4; i++) {
        pre.gains[i] = perStage * (i == 0 ? 1 : 1.15);
        pre.hpK[i] = onePole(35 * js::pow(1.8, i), rate);
      }
      pre.lpK = onePole(11000, rate);
      pw.g = g0; pw.norm = 1 / js::pow(g0, 0.6); pw.sag = p(kSag);
      pw.atk = onePole(160, rate); pw.rel = onePole(7, rate);
      for (int i = 0; i < 3; i++) s.tone[i].set(toneCoefs[i]);
      for (int i = 0; i < 2; i++) s.out[i].set(outCoefs[i]);
      for (int i = 0; i < kCabSlots; i++) s.cab[i].set(cabCoefs[i]);
      Oversampler& o1 = s.os1[os];
      Oversampler& o2 = s.os2[os];
      const int wait = o1.latency() + o2.latency();
      auto preTick = [&](double v) { return pre.tick(v); };
      auto powerTick = [&](double v) { return pw.tick(v); };
      for (int i = 0; i < n; i++) {
        const double dry = x[i];
        double v = dry;
        s.inHp += (v - s.inHp) * inK; v -= s.inHp;
        s.bright += (v - s.bright) * brightK; v += (v - s.bright) * brightAmount;
        v = o1.run(v, preTick);
        for (int k = 0; k < 3; k++) v = s.tone[k].run(v);
        v = o2.run(v, powerTick);
        v = s.out[0].run(v); v = s.out[1].run(v);
        if (cabIndex > 0) for (int k = 0; k < kCabSlots; k++) v = s.cab[k].run(v);
        const double d = s.delay.push(dry, wait);
        out[i] = static_cast<float>((d + (v - d) * mix) * level);
      }
    }
  }

private:
  double sampleRate_;
  Side sides_[2];
};

}  // namespace

void registerAmp(NodeRegistry& registry) {
  NodeType type;
  type.type = "amp";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.latency = [](const std::map<std::string, double>& settings, double) {
    const int factors[3] = {1, 2, 4};
    return static_cast<double>(2 * oversampleLatency(factors[std::min(2, std::max(0, jsRoundInt(settings.at("oversample"))))]));
  };
  type.params = {
    {"gain", 0, 10, 5}, {"stages", 1, 4, 2, true}, {"bright", 0, 1, 0}, {"bias", 0, 1, 0.3}, {"bass", 0, 10, 5}, {"mid", 0, 10, 5}, {"treble", 0, 10, 5},
    {"voicing", 0, 2, 1, true}, {"master", 0, 10, 4}, {"sag", 0, 1, 0.4}, {"presence", 0, 10, 3}, {"depth", 0, 10, 3}, {"cab", 0, 3, 1, true},
    {"cabtone", 0, 1, 0.5}, {"level", 0, 2, 0.7}, {"mix", 0, 1, 1}, {"oversample", 0, 2, 1, true},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Amp>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

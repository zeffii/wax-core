// Groove: what playing a record does to a sound (warp, wear, tracing, dust, scratch, pops, surface hiss, rumble).
// Port of webtracker src/audio/nodes/groove.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/oversample.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kSpeed = 0, kWarp, kWear, kTracing, kDust, kScratch, kStuck, kPops, kSurface, kRumble, kMix, kQuality };
constexpr int kMaxVoices = 14, kBuf = 2048, kMask = kBuf - 1;
const double kRpm[3] = {100.0 / 3, 45, 78};

struct Rng {
  std::uint32_t seed;
  double operator()() {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed / 4294967296.0;
  }
};

struct Kernels {
  std::vector<float> dust, scratch, pop;
};

std::vector<float> ring(double sr, double hz, double ms, double seconds, bool edge) {
  const int length = jsRoundInt(sr * seconds);
  std::vector<float> k(length);
  for (int j = 0; j < length; j++) {
    const double t = j / sr, attack = edge ? std::min(1.0, j / 6.0) : 1.0;
    k[j] = static_cast<float>(js::sin(2 * kPi * hz * t) * js::exp(-t / (ms / 1000)) * attack);
  }
  return k;
}

Kernels kernels(double sr, bool simulation) {
  Kernels k;
  if (simulation) {
    k.dust.resize(20);
    for (int j = 0; j < 20; j++) { const double x = j - 6; k.dust[j] = static_cast<float>(x * js::exp(-(x * x) / 7) * 0.9); }
  } else {
    k.dust = {1.0f, static_cast<float>(-0.7), static_cast<float>(0.25), static_cast<float>(-0.08)};
  }
  k.scratch = ring(sr, 3300, 1.4, 0.012, simulation);
  k.pop = ring(sr, 210, 13, 0.07, true);
  return k;
}

struct Clicks {
  const std::vector<float>* k[kMaxVoices] = {};
  int pos[kMaxVoices] = {};
  float amp[kMaxVoices] = {};
  int live = 0;
  void add(const std::vector<float>& kernel, double a) {
    for (int v = 0; v < kMaxVoices; v++) if (!k[v]) { k[v] = &kernel; pos[v] = 0; amp[v] = static_cast<float>(a); live++; return; }
  }
  double next() {
    double sum = 0;
    for (int v = 0; v < kMaxVoices && live; v++) {
      const std::vector<float>* kernel = k[v];
      if (!kernel) continue;
      sum += static_cast<double>((*kernel)[pos[v]]) * static_cast<double>(amp[v]);
      if (++pos[v] >= static_cast<int>(kernel->size())) { k[v] = nullptr; live--; }
    }
    return sum;
  }
};

struct Kept { double at, amp; };

class Groove final : public Node {
public:
  explicit Groove(const NodeContext& context)
      : sr_(context.sampleRate), K_{kernels(context.sampleRate, false), kernels(context.sampleRate, true)},
        rand_{Rng{0x2545f491u}, Rng{0x9e3779b9u}}, randShared_{0x1b873593u},
        over_{Oversampler(4), Oversampler(4)}, latDelay_{FixedDelay(256), FixedDelay(256)}, dryDelay_{FixedDelay(256), FixedDelay(256)} {
    std::fill(ring_[0], ring_[0] + kBuf, 0.0f);
    std::fill(ring_[1], ring_[1] + kBuf, 0.0f);
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto g = [&](int k) { return static_cast<double>(params[slots[k]]); };
    const double sr = sr_;
    const bool sim = jsRoundInt(g(kQuality)) == 1;
    const Kernels& kernel = K_[sim ? 1 : 0];
    const double rpm = kRpm[std::max(0, std::min(2, jsRoundInt(g(kSpeed))))], turnStep = rpm / 60 / sr;
    const double warp = g(kWarp), wear = g(kWear), tracing = g(kTracing), dust = g(kDust), scratch = g(kScratch), stuck = g(kStuck), pops = g(kPops);
    const double surface = g(kSurface), rumble = g(kRumble), mix = g(kMix);
    auto pole = [&](double fc) { return 1 - js::exp(-2 * kPi * std::min(fc, sr * 0.45) / sr); };
    const double depthTo = warp * 2.2 * sr * 0.001;
    const double lossA = pole(17000 * js::pow(0.2, wear)), lossB = pole(17000 * js::pow(0.2, wear) * 1.3), sideA = pole(140);
    const double gain = 1 + 4 * tracing, bias = 0.12 * tracing, biasT = js::tanh(bias), norm = 0.25 / js::tanh(0.25 * gain);
    auto curve = [&](double v) { return (js::tanh(gain * v + bias) - biasT) * norm; };
    const double dustP = dust > 0 ? (dust * dust * 140 + 4 * dust) / sr : 0, dustAmp = 0.006 + 0.05 * dust;
    const double scratchP = scratch > 0 ? scratch * 2.5 / sr : 0, scratchAmp = 0.05 + 0.25 * scratch;
    const double popP = pops > 0 ? pops * 0.8 / sr : 0, popAmp = 0.08 + 0.3 * pops;
    const double hissA = pole(9000), hissHpA = pole(700), hissGain = surface * 0.011;
    const double rumA = pole(18), rumGain = rumble * 1.0;
    const bool idle = warp <= 1e-4 && depth_ <= 1e-4;
    const float* dry[2] = {inputs[0], inputs[1]};
    float* out[2] = {outputs[0], outputs[1]};
    double x[2], y[2], dryAt[2];
    const int latency = oversampleLatency(4);
    for (int i = 0; i < n; i++) {
      prevTurn_ = turn_; turn_ += turnStep; if (turn_ >= 1) turn_ -= 1;
      depth_ += (depthTo - depth_) * 0.002;
      double swing = 0;
      if (depth_ > 1e-4) swing = js::sin(2 * kPi * turn_) * 0.75 + js::sin(4 * kPi * turn_ + 0.7) * 0.25;
      const double mean = 2 + depth_, d = mean + depth_ * swing;
      x[0] = dry[0][i]; x[1] = dry[1][i];
      for (int c = 0; c < 2; c++) {
        float* buf = ring_[c];
        buf[w_] = static_cast<float>(x[c]);
        if (idle) { y[c] = x[c]; dryAt[c] = x[c]; continue; }
        const double pos = w_ - d;
        const int i0 = static_cast<int>(std::floor(pos));
        const double f = pos - i0;
        const double ym = buf[(i0 - 1) & kMask], y0 = buf[i0 & kMask], y1 = buf[(i0 + 1) & kMask], y2 = buf[(i0 + 2) & kMask];
        const double c1 = 0.5 * (y1 - ym), c2 = ym - 2.5 * y0 + 2 * y1 - 0.5 * y2, c3 = 0.5 * (y2 - ym) + 1.5 * (y0 - y1);
        y[c] = ((c3 * f + c2) * f + c1) * f + y0;
        dryAt[c] = buf[(w_ - jsRoundInt(mean)) & kMask];
      }
      w_ = (w_ + 1) & kMask;
      if (wear > 1e-4) {
        for (int c = 0; c < 2; c++) { lp1_[c] += (y[c] - lp1_[c]) * lossA; lp2_[c] += (lp1_[c] - lp2_[c]) * lossB; y[c] = lp2_[c]; }
        const double m = (y[0] + y[1]) * 0.5, s = (y[0] - y[1]) * 0.5;
        sideLp_ += (s - sideLp_) * sideA;
        const double s2 = s - sideLp_ * wear;
        y[0] = m + s2; y[1] = m - s2;
      }
      for (int c = 0; c < 2; c++) {
        if (sim) y[c] = tracing > 0 ? over_[c].run(y[c], curve) : latDelay_[c].push(y[c], latency);
        else if (tracing > 0) y[c] = curve(y[c]);
        if (dustP > 0 && rand_[c]() < dustP) {
          const double r1 = rand_[c]();
          const double e = -js::log(std::max(1e-6, r1));
          const double r2 = rand_[c]();
          clicks_[c].add(kernel.dust, (r2 < 0.5 ? -1 : 1) * dustAmp * std::min(6.0, 0.4 + e));
        }
        if (scratchP > 0 && rand_[c]() < scratchP) {
          const double r1 = rand_[c]();
          const double r2 = rand_[c]();
          const double a = (r1 < 0.5 ? -1 : 1) * scratchAmp * (0.4 + r2);
          clicks_[c].add(kernel.scratch, a);
          if (stuck > 0 && c == 0) { if (kept_.size() >= 3) kept_.erase(kept_.begin()); kept_.push_back({turn_, a}); }
        }
        if (popP > 0 && rand_[c]() < popP) {
          const double r1 = rand_[c]();
          const double r2 = rand_[c]();
          clicks_[c].add(kernel.pop, (r1 < 0.5 ? -1 : 1) * popAmp * (0.5 + r2));
        }
        if (clicks_[c].live) y[c] += clicks_[c].next();
        if (hissGain > 0) {
          const double white = rand_[c]() * 2 - 1;
          hiss_[c] += (white - hiss_[c]) * hissA;
          hissHp_[c] += (hiss_[c] - hissHp_[c]) * hissHpA;
          y[c] += (hiss_[c] - hissHp_[c]) * hissGain * 2;
        }
      }
      if (stuck > 0 && !kept_.empty()) {
        const std::vector<Kept> copy = kept_;
        for (const Kept& k : copy) {
          const bool passed = prevTurn_ <= turn_ ? (k.at > prevTurn_ && k.at <= turn_) : (k.at > prevTurn_ || k.at <= turn_);
          if (passed && randShared_() < stuck) { clicks_[0].add(kernel.scratch, k.amp); clicks_[1].add(kernel.scratch, k.amp * 0.9); }
        }
      }
      if (rumGain > 0) {
        rum1_ += ((randShared_() * 2 - 1) - rum1_) * rumA;
        rum2_ += (rum1_ - rum2_) * rumA;
        const double r = rum2_ * rumGain;
        y[0] += r; y[1] += r;
      }
      for (int c = 0; c < 2; c++) {
        const double clean = sim ? dryDelay_[c].push(dryAt[c], latency) : dryAt[c];
        out[c][i] = static_cast<float>(clean + (y[c] - clean) * mix);
      }
    }
  }

private:
  double sr_;
  Kernels K_[2];
  Rng rand_[2], randShared_;
  Clicks clicks_[2];
  float ring_[2][kBuf];
  Oversampler over_[2];
  FixedDelay latDelay_[2], dryDelay_[2];
  double lp1_[2] = {0, 0}, lp2_[2] = {0, 0}, sideLp_ = 0, hiss_[2] = {0, 0}, hissHp_[2] = {0, 0}, rum1_ = 0, rum2_ = 0;
  int w_ = 0;
  double turn_ = 0, prevTurn_ = 0, depth_ = 0;
  std::vector<Kept> kept_;
};

}  // namespace

void registerGroove(NodeRegistry& registry) {
  NodeType type;
  type.type = "groove";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.latency = [](const std::map<std::string, double>& settings, double) { return jsRoundInt(settings.at("quality")) == 1 ? static_cast<double>(oversampleLatency(4)) : 0.0; };
  type.params = {
    {"speed", 0, 2, 0, true}, {"warp", 0, 1, 0}, {"wear", 0, 1, 0}, {"tracing", 0, 1, 0}, {"dust", 0, 1, 0}, {"scratch", 0, 1, 0},
    {"stuck", 0, 1, 0}, {"pops", 0, 1, 0}, {"surface", 0, 1, 0}, {"rumble", 0, 1, 0}, {"mix", 0, 1, 1}, {"quality", 0, 1, 0, true},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Groove>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

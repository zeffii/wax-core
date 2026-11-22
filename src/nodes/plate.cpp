// Plate reverb: a brightness filter and four allpass diffusers on the way in, then a "tank" of two branches that feed
// each other in a figure of eight. Each branch is a modulated allpass, a delay, a damping low-pass, another allpass and
// a second delay; the output is picked from several points inside the tank, with different points for left and right.
// The input is summed to mono. out = dry * (1 - mix) + wet * mix.
// Port of webtracker src/audio/nodes/plate.js.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kPredelay = 0, kBandwidth, kDecay, kDamp, kDiffusion, kMod, kWidth, kMix };

constexpr double kRefRate = 29761;   // the sample rate the delay lengths below are for
constexpr int kInputAllpass[4] = {142, 107, 379, 277};
// each branch of the tank: modulated allpass, delay, allpass, delay
constexpr int kTankLeft[4] = {672, 4453, 1800, 3720}, kTankRight[4] = {908, 4217, 2656, 3163};

// A delay line that is written at `w`.
struct Line {
  std::vector<float> buf;
  int w = 0;

  int size() const { return static_cast<int>(buf.size()); }
  void resize(int length) { buf.assign(static_cast<std::size_t>(length), 0.0f); }
  // the sample `d` back from where it is written
  double tap(int d) const {
    int i = w - d;
    if (i < 0) i += size();
    return buf[static_cast<std::size_t>(i)];
  }
  // writes a sample and returns the one it replaces (the oldest)
  double swap(double value) {
    const double oldest = buf[static_cast<std::size_t>(w)];
    buf[static_cast<std::size_t>(w)] = static_cast<float>(value);
    if (++w == size()) w = 0;
    return oldest;
  }
};

struct Branch {
  Line mod;         // the modulated allpass
  int modDelay = 0;
  Line delay1;
  double lowpass = 0;
  Line allpass;
  Line delay2;
};

// where the output is picked from: which line, how far back, and with which sign
enum Source { kL1, kL3, kL4, kR1, kR3, kR4 };
struct TapSpec {
  Source source;
  int delay;
  double sign;
};
constexpr TapSpec kTapsLeft[7] = {{kR1, 266, 1}, {kR1, 2974, 1}, {kR3, 1913, -1}, {kR4, 1996, 1}, {kL1, 1990, -1}, {kL3, 187, -1}, {kL4, 1066, -1}};
constexpr TapSpec kTapsRight[7] = {{kL1, 353, 1}, {kL1, 3627, 1}, {kL3, 1228, -1}, {kL4, 2673, 1}, {kR1, 2111, -1}, {kR3, 335, -1}, {kR4, 121, -1}};

double flushed(double v) { return std::abs(v) < 1e-20 ? 0 : v; }

class Plate final : public Node {
public:
  explicit Plate(const NodeContext& context) : sampleRate_(context.sampleRate), scale_(context.sampleRate / kRefRate) {
    maxExcursion_ = 16 * std::max(1.0, scale_);
    preSize_ = static_cast<int>(std::ceil(0.2 * sampleRate_)) + 4;
    pre_.assign(static_cast<std::size_t>(preSize_), 0.0f);
    for (int q = 0; q < 4; q++) inputAllpass_[q].resize(length(kInputAllpass[q]));
    build(left_, kTankLeft);
    build(right_, kTankRight);
    for (int q = 0; q < 7; q++) {
      tapsLeft_[q] = resolve(kTapsLeft[q]);
      tapsRight_[q] = resolve(kTapsRight[q]);
    }
    rate_[0] = 0.9 / sampleRate_;
    rate_[1] = 1.1 / sampleRate_;
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inLeft = inputs[0];
    const float* inRight = inputs[1];
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    const double preTarget = std::min(params[slots[kPredelay]] * sampleRate_ / 1000, static_cast<double>(preSize_ - 3));
    const double bandwidth = params[slots[kBandwidth]];
    const double bw = 0.1 + 0.899 * (bandwidth * bandwidth);
    const double damp = 0.9 * params[slots[kDamp]];
    const double dif = params[slots[kDiffusion]], g1 = 0.4 + 0.35 * dif, g2 = 0.3 + 0.32 * dif, gt1 = 0.35 + 0.35 * dif, gt2 = 0.25 + 0.25 * dif;
    const double rt60 = params[slots[kDecay]];
    // one branch's trip round the tank, in seconds
    const double half = static_cast<double>(length(kTankLeft[0]) + length(kTankLeft[1]) + length(kTankLeft[2]) + length(kTankLeft[3])) / sampleRate_;
    const double decay = std::min(0.9995, js::pow(10.0, -3 * half / rt60));
    const double excursion = params[slots[kMod]] * maxExcursion_, width = params[slots[kWidth]], mix = params[slots[kMix]];
    if (preCur_ < 0) preCur_ = preTarget;
    // the modulation is slow: its sine is worked out at the start and end of the block and the read position slides between
    const double inv = 1.0 / n;
    for (int j = 0; j < 2; j++) {
      const double a0 = excursion * (0.5 + 0.5 * js::sin(kTwoPi * phase_[j]));
      phase_[j] += rate_[j] * n;
      if (phase_[j] >= 1) phase_[j] -= 1;
      const double a1 = excursion * (0.5 + 0.5 * js::sin(kTwoPi * phase_[j]));
      at_[j] = a0;
      step_[j] = (a1 - a0) * inv;
    }
    for (int i = 0; i < n; i++) {
      const float l = inLeft[i], r = inRight[i];
      preCur_ += (preTarget - preCur_) * 0.001;
      pre_[static_cast<std::size_t>(pw_)] = static_cast<float>((static_cast<double>(l) + r) * 0.5);
      double rp = pw_ - preCur_;
      if (rp < 0) rp += preSize_;
      const int pk = static_cast<int>(rp);
      const double pf = rp - pk;
      const double p0 = pre_[static_cast<std::size_t>(pk)], p1 = pre_[static_cast<std::size_t>(pk + 1 == preSize_ ? 0 : pk + 1)];
      double x = p0 + (p1 - p0) * pf;
      if (++pw_ == preSize_) pw_ = 0;
      inputLowpass_ += bw * (x - inputLowpass_);
      x = inputLowpass_;
      for (int q = 0; q < 4; q++) {
        Line& line = inputAllpass_[q];
        const double g = q < 2 ? g1 : g2;
        const double delayed = line.buf[static_cast<std::size_t>(line.w)], v = x - g * delayed;
        line.swap(flushed(v));
        x = g * v + delayed;
      }
      // the two branches work on what the other produced one sample ago
      const double newLeft = runBranch(left_, x + rightEnd_, 0, gt1, gt2, damp, decay);
      const double newRight = runBranch(right_, x + leftEnd_, 1, gt1, gt2, damp, decay);
      leftEnd_ = newLeft;
      rightEnd_ = newRight;
      double wl = 0, wr = 0;
      for (int q = 0; q < 7; q++) {
        wl += tapsLeft_[q].sign * tapsLeft_[q].line->tap(tapsLeft_[q].delay);
        wr += tapsRight_[q].sign * tapsRight_[q].line->tap(tapsRight_[q].delay);
      }
      wl *= 0.6;
      wr *= 0.6;
      const double mid = (wl + wr) * 0.5;
      wl = mid + (wl - mid) * width;
      wr = mid + (wr - mid) * width;
      outLeft[i] = static_cast<float>(l + (wl - l) * mix);
      outRight[i] = static_cast<float>(r + (wr - r) * mix);
    }
  }

private:
  struct Tap {
    const Line* line = nullptr;
    int delay = 0;
    double sign = 1;
  };

  // a published delay length at this sample rate
  int length(int d) const { return std::max(2, jsRoundInt(d * scale_)); }

  void build(Branch& branch, const int lengths[4]) {
    branch.modDelay = length(lengths[0]);
    branch.mod.resize(branch.modDelay + static_cast<int>(std::ceil(maxExcursion_)) + 4);
    branch.delay1.resize(length(lengths[1]));
    branch.allpass.resize(length(lengths[2]));
    branch.delay2.resize(length(lengths[3]));
  }

  Tap resolve(const TapSpec& spec) const {
    const Line* line = spec.source == kL1 ? &left_.delay1 : spec.source == kL3 ? &left_.allpass : spec.source == kL4 ? &left_.delay2
                     : spec.source == kR1 ? &right_.delay1 : spec.source == kR3 ? &right_.allpass : &right_.delay2;
    return {line, std::min(line->size() - 1, length(spec.delay)), spec.sign};
  }

  double runBranch(Branch& b, double x, int j, double gt1, double gt2, double damp, double decay) {
    // modulated allpass: its read position wanders a few samples
    Line& m = b.mod;
    const int size = m.size();
    double rd = m.w - (b.modDelay + at_[j]);
    while (rd < 0) rd += size;
    const int ki = static_cast<int>(rd);
    const double f = rd - ki;
    const int k1 = ki + 1 == size ? 0 : ki + 1;
    const double m0 = m.buf[static_cast<std::size_t>(ki)], m1 = m.buf[static_cast<std::size_t>(k1)];
    const double delayed = m0 + (m1 - m0) * f;
    const double v = x - gt1 * delayed;
    m.swap(flushed(v));
    at_[j] += step_[j];
    double y = gt1 * v + delayed;
    const double o1 = b.delay1.swap(y);
    b.lowpass += (1 - damp) * (o1 - b.lowpass);
    y = b.lowpass * decay;
    const double ad = b.allpass.buf[static_cast<std::size_t>(b.allpass.w)], av = y - gt2 * ad;
    b.allpass.swap(flushed(av));
    y = gt2 * av + ad;
    return b.delay2.swap(y);
  }

  double sampleRate_, scale_, maxExcursion_ = 16;
  int preSize_ = 0;
  std::vector<float> pre_;
  int pw_ = 0;
  Line inputAllpass_[4];
  Branch left_, right_;
  Tap tapsLeft_[7], tapsRight_[7];
  double inputLowpass_ = 0, leftEnd_ = 0, rightEnd_ = 0, preCur_ = -1;
  double phase_[2] = {0.1, 0.6}, rate_[2] = {0, 0}, at_[2] = {0, 0}, step_[2] = {0, 0};
};

}  // namespace

void registerPlate(NodeRegistry& registry) {
  NodeType type;
  type.type = "plate";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.dryAtZero = "mix";
  type.params = {
    {"predelay", 0, 200, 0},        // ms
    {"bandwidth", 0, 1, 0.8},       // brightness of what goes in
    {"decay", 0.2, 20, 2},          // seconds to fall by 60 dB
    {"damp", 0, 1, 0.3},
    {"diffusion", 0, 1, 0.8},
    {"mod", 0, 1, 0.4},
    {"width", 0, 1, 1},
    {"mix", 0, 1, 0.3},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Plate>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

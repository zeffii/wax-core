// Tape, stereo: what a recording on tape does to a sound. `type` picks the kind of tape and machine, `age` how worn it
// is. In the order the sound meets it: wow (slow pitch drift, shared by both channels) and flutter (fast, a little
// different in each) by a moving delay; a low cut; tape saturation; the head bump (a low boost); the loss of highs;
// dropouts (short dips in level and brightness, in each channel on its own); and hiss. `mix` blends with the clean
// sound, which is delayed to match. Port of webtracker src/audio/nodes/tape.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kType = 0, kAge, kDrive, kWow, kFlutter, kHiss, kDrop, kTone, kMix };

struct Kind { double fc, hpf, bumpHz, bumpDb, drive, hiss, wow, flutter, drop; int poles; };

constexpr Kind kKinds[6] = {
  {19000, 20, 70, 1.2, 0.6, -72, 0.25, 0.25, 0.15, 2},
  {15000, 25, 80, 1.5, 0.8, -64, 0.5, 0.6, 0.3, 2},
  {11000, 30, 90, 2.2, 1.0, -56, 0.7, 0.8, 0.5, 2},
  {6500, 40, 100, 2.5, 1.2, -48, 1.0, 1.0, 1.0, 2},
  {3400, 280, 420, 3.0, 1.4, -42, 1.2, 1.3, 0.8, 3},
  {7500, 35, 90, 2.5, 1.6, -46, 0.9, 1.1, 0.9, 3},
};

constexpr int kSize = 4096, kMask = kSize - 1;
constexpr double kCentreMs = 4.5, kWowMs = 2.6, kFlutterMs = 0.12;   // the fixed delay, and the most the wobble moves it

// what a block needs, worked out once per block
struct Block {
  double flutMs, wowMs, hpA, gain, bias, biasT, makeup, b0, b1, b2, a1, a2, lossA, lossB;
  int poles;
  double dropP, dropA, hissA, hissGain, mix;
};

class Rand {
public:
  double operator()() {
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    return seed_ / 4294967296.0;
  }
private:
  std::uint32_t seed_ = 0x2545f491u;
};

class Tape final : public Node {
public:
  explicit Tape(const NodeContext& context)
      : sr_(context.sampleRate), wowArr_(static_cast<std::size_t>(context.maxBlock), 0.0f) {
    lpa_ = pole(1.2);
    norm_ = 1 / (3 * std::sqrt((1.0 / 3) * lpa_ / (2 - lpa_)));
    left_.init(0, this);
    right_.init(1, this);
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto g = [&](int k) -> double { return params[slots[k]]; };
    const Kind& kind = kKinds[std::max(0, std::min(5, jsRoundInt(g(kType))))];
    const double age = g(kAge), wear = 0.35 + 0.65 * age;
    if (wowArr_.size() < static_cast<std::size_t>(n)) wowArr_.assign(static_cast<std::size_t>(n), 0.0f);
    // the wow: a slow sine and a slow random drift, the same for both channels
    const double wowRate = 0.55 / sr_;
    for (int i = 0; i < n; i++) {
      wowPh_ += wowRate; if (wowPh_ > 1) wowPh_ -= 1;
      wowNz_ += (rand_() * 2 - 1 - wowNz_) * lpa_;
      wowArr_[static_cast<std::size_t>(i)] = static_cast<float>(std::max(-1.0, std::min(1.0, js::sin(2 * kPi * wowPh_) * 0.65 + wowNz_ * norm_ * 0.35)));
    }
    const double fc = std::max(700.0, kind.fc * (1 - 0.65 * age) * js::pow(2, g(kTone) * 1.2));
    const double driveG = 1 + g(kDrive) * kind.drive * 5, bias = 0.08 * g(kDrive);
    // peaking filter for the head bump (RBJ cookbook)
    const double A = js::pow(10, kind.bumpDb / 40), w0 = 2 * kPi * kind.bumpHz / sr_, al = js::sin(w0) / (2 * 0.9), cw = js::cos(w0), a0 = 1 + al / A;
    Block P;
    P.flutMs = kFlutterMs * g(kFlutter) * kind.flutter * wear;
    P.wowMs = kWowMs * g(kWow) * kind.wow * wear;
    P.hpA = pole(kind.hpf); P.gain = driveG; P.bias = bias; P.biasT = js::tanh(bias); P.makeup = 1 / std::sqrt(driveG);
    P.b0 = (1 + al * A) / a0; P.b1 = -2 * cw / a0; P.b2 = (1 - al * A) / a0; P.a1 = -2 * cw / a0; P.a2 = (1 - al / A) / a0;
    P.lossA = pole(fc); P.lossB = pole(1200); P.poles = kind.poles;
    P.dropP = g(kDrop) * kind.drop * (0.1 + 3.5 * age) / sr_; P.dropA = pole(160);
    P.hissA = pole(fc * 1.3); P.hissGain = js::pow(10, kind.hiss / 20) * g(kHiss) * 2 * (0.6 + 0.8 * age) * 1.7;
    P.mix = g(kMix);
    left_.run(inputs[0], outputs[0], n, P);
    right_.run(inputs[1], outputs[1], n, P);
  }

private:
  double pole(double fc) const { return 1 - js::exp(-2 * kPi * std::min(fc, sr_ * 0.45) / sr_); }

  struct Channel {
    Tape* tape = nullptr;
    int side = 0;
    std::vector<float> buf = std::vector<float>(kSize, 0.0f);
    int w = 0;
    double hp = 0, hx1 = 0, hx2 = 0, hy1 = 0, hy2 = 0, p1 = 0, p2 = 0, p3 = 0, lp2 = 0, nzLp = 0, hissLp = 0, dropLeft = 0, dropDepth = 0, drop = 0;
    double flPh = 0, flPh2 = 0, flNz = 0;
    double flA = 0, flNorm = 0;
    int centre = 0;

    void init(int s, Tape* t) {
      tape = t; side = s;
      flPh = s * 1.7; flPh2 = s * 0.9;
      flA = t->pole(25);
      flNorm = 1 / (3 * std::sqrt((1.0 / 3) * flA / (2 - flA)));
      centre = jsRoundInt(kCentreMs * 0.001 * t->sr_);
    }

    void run(const float* inp, float* out, int n, const Block& P) {
      const double sr = tape->sr_;
      Rand& rand = tape->rand_;
      for (int i = 0; i < n; i++) {
        const double x = inp[i];
        buf[static_cast<std::size_t>(w)] = static_cast<float>(x);
        // the moving delay: wow from the shared array, flutter of its own
        flPh += 7.3 / sr; flPh2 += 11.9 / sr;
        if (flPh > 1) flPh -= 1;
        if (flPh2 > 1) flPh2 -= 1;
        flNz += (rand() * 2 - 1 - flNz) * flA;
        const double flut = (js::sin(2 * kPi * flPh) * 0.6 + js::sin(2 * kPi * flPh2) * 0.35 + flNz * flNorm * 0.3) * P.flutMs;
        const double d = std::max(2.0, (kCentreMs + static_cast<double>(tape->wowArr_[static_cast<std::size_t>(i)]) * P.wowMs + flut) * 0.001 * sr);
        const double pos = w - d, fl = std::floor(pos), f = pos - fl;
        const int i0 = static_cast<int>(fl);
        const double ym = buf[static_cast<std::size_t>((i0 - 1) & kMask)], y0 = buf[static_cast<std::size_t>(i0 & kMask)];
        const double y1 = buf[static_cast<std::size_t>((i0 + 1) & kMask)], y2 = buf[static_cast<std::size_t>((i0 + 2) & kMask)];
        const double c1 = 0.5 * (y1 - ym), c2 = ym - 2.5 * y0 + 2 * y1 - 0.5 * y2, c3 = 0.5 * (y2 - ym) + 1.5 * (y0 - y1);
        double y = ((c3 * f + c2) * f + c1) * f + y0;
        const double dry = buf[static_cast<std::size_t>((w - centre) & kMask)];
        w = (w + 1) & kMask;
        // low cut, saturation
        hp += (y - hp) * P.hpA; y -= hp;
        y = (js::tanh(P.gain * y + P.bias) - P.biasT) * P.makeup;
        // the head bump
        const double b = P.b0 * y + P.b1 * hx1 + P.b2 * hx2 - P.a1 * hy1 - P.a2 * hy2;
        hx2 = hx1; hx1 = y; hy2 = hy1; hy1 = b; y = b;
        // the loss of highs
        p1 += (y - p1) * P.lossA; y = p1;
        p2 += (y - p2) * P.lossA; y = p2;
        if (P.poles == 3) { p3 += (y - p3) * P.lossA; y = p3; }
        // dropouts: now and then the level dips for a few ms, and the highs go with it
        if (P.dropP > 0 && dropLeft <= 0 && rand() < P.dropP) { dropLeft = (0.005 + rand() * 0.04) * sr; dropDepth = 0.3 + rand() * 0.6; }
        drop += ((dropLeft > 0 ? dropDepth : 0) - drop) * P.dropA;
        dropLeft--;
        if (drop > 1e-4) { lp2 += (y - lp2) * P.lossB; y = (y + (lp2 - y) * drop) * (1 - 0.6 * drop); } else lp2 = y;
        // hiss, brighter than white and limited by the same machine
        nzLp += (rand() * 2 - 1 - nzLp) * 0.1;
        const double nz = (rand() * 2 - 1) - nzLp;
        hissLp += (nz - hissLp) * P.hissA;
        y += hissLp * P.hissGain;
        out[i] = static_cast<float>(dry + (y - dry) * P.mix);
      }
    }
  };

  double sr_;
  std::vector<float> wowArr_;
  double lpa_ = 0, norm_ = 0;
  double wowPh_ = 0, wowNz_ = 0;
  Rand rand_;
  Channel left_, right_;
};

}  // namespace

void registerTape(NodeRegistry& registry) {
  NodeType type;
  type.type = "tape";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.isEffect = true;
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.params = {
    {"type", 0, 5, 2, true},
    {"age", 0, 1, 0.4},
    {"drive", 0, 1, 0.35},
    {"wow", 0, 1, 0.5},
    {"flutter", 0, 1, 0.5},
    {"hiss", 0, 1, 0.5},
    {"dropouts", 0, 1, 0.3},
    {"tone", -1, 1, 0},
    {"mix", 0, 1, 1},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Tape>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

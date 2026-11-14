// Prisma Canyon (the `prismacanyon` node): a tape echo with four playback heads. One tape loop is recorded and four
// heads read it at 1, 2, 3 and 4 times the repeat `time` (so every echo falls on a multiple of it, which on a song with
// ticks is a grid). Each head is its own delay line with a level (0 switches it off), a pan, and a darkening: the
// line's low-pass sits at `cutoff` lowered by `damp` (0 leaves it, 1 takes it four octaves down), so the far lines can
// be soft and the near ones bright.
//   intensity   how much of the heads goes back to the tape (past 1 it feeds on itself, held in check by the saturation)
//   bass, treble  tone of the sound that goes round the loop, so each repeat is warmer or thinner than the one before
//   echo, reverb, dry   the volume of the echoes, of a small spring-like reverb of the input, and of the input itself
//   wow, flutter  the tape speed drifting slowly and shivering; saturation  the tape squashing what is recorded
// Changing `time` moves the tape speed, so the echoes bend in pitch as it moves, as a tape echo does.
// Port of webtracker src/audio/nodes/canyon.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

// then level, pan, damp for each line
enum Param { kTime = 0, kIntensity, kCutoff, kBass, kTreble, kEcho, kReverb, kDry, kWow, kFlutter, kSaturation, kLine };

constexpr int kLines = 4, kPerLine = 3;
constexpr double kMaxTimeMs = 700;
constexpr double kLevels[kLines] = {1, 0.7, 0.5, 0.35}, kPans[kLines] = {-0.7, 0.7, -0.35, 0.35}, kDamps[kLines] = {0.1, 0.25, 0.4, 0.55};

// A shelving filter (the usual cookbook one): high or low, gain in dB.
class Shelf {
public:
  void set(bool high, double f, double db, double sr) {
    const double A = js::pow(10.0, db / 40), w = 2 * kPi * std::min(f, sr * 0.45) / sr, cs = js::cos(w), sn = js::sin(w);
    const double al = sn / 2 * kSqrt2, t = 2 * std::sqrt(A) * al;
    double b0, b1, b2, a0, a1, a2;
    if (high) {
      b0 = A * ((A + 1) + (A - 1) * cs + t); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - t);
      a0 = (A + 1) - (A - 1) * cs + t; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - t;
    } else {
      b0 = A * ((A + 1) - (A - 1) * cs + t); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - t);
      a0 = (A + 1) + (A - 1) * cs + t; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - t;
    }
    b0_ = b0 / a0; b1_ = b1 / a0; b2_ = b2 / a0; a1_ = a1 / a0; a2_ = a2 / a0;
  }
  double run(double x) {
    const double y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
    x2_ = x1_;
    x1_ = x;
    y2_ = y1_;
    y1_ = std::abs(y) < 1e-20 ? 0 : y;
    return y;
  }

private:
  double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
};

// A small reverb of the input, in the manner of a spring tank: four damped combs into two allpasses, a little
// different for each side.
class SpringVerb {
public:
  SpringVerb(double sr, int side) {
    const double scale = sr / 44100;
    const int combLengths[4] = {1557, 1617, 1491, 1422}, allpassLengths[2] = {225, 556};
    for (int k = 0; k < 4; k++) combs_[k].buf.assign(static_cast<std::size_t>(jsRoundInt((combLengths[k] + side * 23) * scale)), 0.0f);
    for (int k = 0; k < 2; k++) allpasses_[k].buf.assign(static_cast<std::size_t>(jsRoundInt((allpassLengths[k] + side * 11) * scale)), 0.0f);
  }
  double run(double x) {
    double sum = 0;
    for (Comb& c : combs_) {
      const double y = c.buf[c.i];
      c.lowpass += (y - c.lowpass) * 0.45;
      c.buf[c.i] = static_cast<float>(x + c.lowpass * 0.82);
      if (++c.i == c.buf.size()) c.i = 0;
      sum += y;
    }
    double v = sum * 0.25;
    for (Allpass& p : allpasses_) {
      const double y = p.buf[p.i], o = y - v;
      p.buf[p.i] = static_cast<float>(v + y * 0.5);
      if (++p.i == p.buf.size()) p.i = 0;
      v = o;
    }
    return v;
  }

private:
  struct Comb {
    std::vector<float> buf;
    std::size_t i = 0;
    double lowpass = 0;
  };
  struct Allpass {
    std::vector<float> buf;
    std::size_t i = 0;
  };
  Comb combs_[4];
  Allpass allpasses_[2];
};

class Canyon final : public Node {
public:
  explicit Canyon(const NodeContext& context) : sr_(context.sampleRate), verb_{SpringVerb(context.sampleRate, 0), SpringVerb(context.sampleRate, 1)} {
    size_ = static_cast<int>(std::ceil(sr_ * (kLines * kMaxTimeMs + 30) / 1000)) + 8;
    tape_[0].assign(static_cast<std::size_t>(size_), 0.0f);
    tape_[1].assign(static_cast<std::size_t>(size_), 0.0f);
    slew_ = 1 - js::exp(-1 / (sr_ * 0.08));
    noiseA_ = 1 - js::exp(-2 * kPi * 1.2 / sr_);
    flutterA_ = 1 - js::exp(-2 * kPi * 25 / sr_);
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inLeft = inputs[0];
    const float* inRight = inputs[1];
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    const auto g = [&](int k) { return static_cast<double>(params[slots[k]]); };
    const double target = std::min(kMaxTimeMs, std::max(20.0, g(kTime))) * 0.001 * sr_;
    if (d_ < 0) d_ = target;
    const double c = g(kIntensity), echo = g(kEcho), rev = g(kReverb), dry = g(kDry), sat = 1 + g(kSaturation) * 2.5, cutoff = g(kCutoff);
    const double wowMs = 1.8 * g(kWow), flMs = 0.12 * g(kFlutter);
    double a[kLines], gl[kLines], gr[kLines], pole[kLines], weight[kLines];
    double sum = 0;
    for (int k = 0; k < kLines; k++) {
      a[k] = g(kLine + k * kPerLine);
      sum += a[k];
      const double p = g(kLine + k * kPerLine + 1);
      // the pan only shapes what is heard; the loop gets the line as it is
      gl[k] = a[k] * std::min(1.0, 1 - p) * echo;
      gr[k] = a[k] * std::min(1.0, 1 + p) * echo;
      const double lineCutoff = cutoff * js::pow(2.0, -4 * g(kLine + k * kPerLine + 2));
      pole[k] = 1 - js::exp(-2 * kPi * std::min(lineCutoff, sr_ * 0.45) / sr_);
    }
    for (int k = 0; k < kLines; k++) weight[k] = sum > 1e-6 ? a[k] / sum : 0;
    for (Tone& s : shelves_) { s.bass.set(false, 150, g(kBass), sr_); s.treble.set(true, 3500, g(kTreble), sr_); }
    for (int i = 0; i < n; i++) {
      d_ += (target - d_) * slew_;
      wowPh_ += 0.55 / sr_; if (wowPh_ > 1) wowPh_ -= 1;
      flPh_ += 7.3 / sr_; if (flPh_ > 1) flPh_ -= 1;
      flPh2_ += 11.9 / sr_; if (flPh2_ > 1) flPh2_ -= 1;
      wowNz_ += (rand() - wowNz_) * noiseA_;
      flNz_ += (rand() - flNz_) * flutterA_;
      const double mod = (js::sin(kTwoPi * wowPh_) * 0.7 + wowNz_ * 2.5 * 0.3) * wowMs * 0.001 * sr_
                       + (js::sin(kTwoPi * flPh_) * 0.6 + js::sin(kTwoPi * flPh2_) * 0.35 + flNz_ * 0.9 * 0.3) * flMs * 0.001 * sr_;
      const double x[2] = {inLeft[i], inRight[i]};
      double heard[2], recorded[2];
      for (int ch = 0; ch < 2; ch++) {
        const std::vector<float>& buf = tape_[ch];
        double fb = 0, bus = 0;
        for (int k = 0; k < kLines; k++) {
          if (a[k] == 0 && weight[k] == 0) continue;
          double r = w_ - d_ * (k + 1) - mod;
          while (r < 0) r += size_;
          const int j = static_cast<int>(r);
          const double f = r - j;
          const int j1 = j + 1 == size_ ? 0 : j + 1;
          const double t0 = buf[static_cast<std::size_t>(j)], t1 = buf[static_cast<std::size_t>(j1)];
          double v = t0 + (t1 - t0) * f;
          double* s = lowpass_[k][ch];
          s[0] += (v - s[0]) * pole[k];
          s[1] += (s[0] - s[1]) * pole[k];
          v = s[1];
          fb += v * weight[k];
          bus += v * (ch ? gr[k] : gl[k]);
        }
        double loop = shelves_[ch].treble.run(shelves_[ch].bass.run(fb)) * c;
        const double hp = loop - dcx_[ch] + 0.995 * dc_[ch];
        dcx_[ch] = loop;
        dc_[ch] = hp;
        loop = hp;
        const double v = x[ch] + loop;
        recorded[ch] = js::tanh(sat * v) / sat;
        heard[ch] = bus;
      }
      const double m = (x[0] + x[1]) * 0.5 * 0.3;
      const double rl = rev > 0 ? verb_[0].run(m) * rev : 0, rr = rev > 0 ? verb_[1].run(m) * rev : 0;
      tape_[0][static_cast<std::size_t>(w_)] = static_cast<float>(std::abs(recorded[0]) < 1e-20 ? 0 : recorded[0]);
      tape_[1][static_cast<std::size_t>(w_)] = static_cast<float>(std::abs(recorded[1]) < 1e-20 ? 0 : recorded[1]);
      if (++w_ == size_) w_ = 0;
      outLeft[i] = static_cast<float>(x[0] * dry + heard[0] + rl);
      outRight[i] = static_cast<float>(x[1] * dry + heard[1] + rr);
    }
  }

private:
  struct Tone {
    Shelf bass, treble;
  };

  // noise, -1..1
  double rand() {
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return seed_ / 4294967296.0 * 2 - 1;
  }

  double sr_;
  int size_ = 0;
  std::vector<float> tape_[2];
  double lowpass_[kLines][2][2] = {};   // per line, per channel, two poles
  Tone shelves_[2];
  SpringVerb verb_[2];
  double dc_[2] = {0, 0}, dcx_[2] = {0, 0};
  int w_ = 0;
  double d_ = -1;   // the repeat time in samples, gliding to its target (negative until first use)
  double wowPh_ = 0, flPh_ = 0, flPh2_ = 0, flNz_ = 0, wowNz_ = 0;
  std::uint32_t seed_ = 0x9e3779b9u;
  double slew_ = 0, noiseA_ = 0, flutterA_ = 0;
};

}  // namespace

void registerCanyon(NodeRegistry& registry) {
  NodeType type;
  type.type = "prismacanyon";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {
    {"time", 20, kMaxTimeMs, 300},      // ms: the first head; the others are at 2, 3 and 4 times it
    {"intensity", 0, 1.15, 0.45},
    {"cutoff", 800, 18000, 9000},       // Hz: the brightest a line gets
    {"bass", -12, 12, 0},               // dB
    {"treble", -12, 12, 0},             // dB
    {"echo", 0, 1.5, 0.7},
    {"reverb", 0, 1, 0},
    {"dry", 0, 1, 1},
    {"wow", 0, 1, 0.25},
    {"flutter", 0, 1, 0.25},
    {"saturation", 0, 1, 0.3},
  };
  for (int k = 0; k < kLines; k++) {
    const std::string number = std::to_string(k + 1);
    type.params.push_back({"level" + number, 0, 1, kLevels[k]});
    type.params.push_back({"pan" + number, -1, 1, kPans[k]});
    type.params.push_back({"damp" + number, 0, 1, kDamps[k]});
  }
  type.create = [](const NodeContext& context) { return std::make_unique<Canyon>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Circuit: an effect made of the parts of an analogue signal path, each a small stage that can be switched on or off, wired in
// one of six orders (the algorithms). Stereo in and out, each channel with its own state. The stages are: slew (the output
// cannot move faster than `slew`), device (tanh of drive * x + bias), miller (the louder, the lower the roll-off), bandwidth
// (a one-pole lowpass), loop (a two-pole lowpass damped by the phase margin), converter (bits and jitter), noise (a hiss
// floor) and wire (one reflection). `drift` moves the bandwidth, the miller corner and the bias slowly and at random. The
// device runs at 1x, 2x or 4x; the dry signal is delayed to match. Port of webtracker src/audio/nodes/circuit.js.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/oversample.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param {
  kAlgorithm = 0, kMix, kLevel, kOversample, kDrift, kFeedback,
  kSlewOn, kSlew, kDeviceOn, kDrive, kBias, kMillerOn, kMiller, kMillerFreq,
  kBandwidthOn, kBandwidth, kLoopOn, kLoopFreq, kMargin,
  kConverterOn, kBits, kJitter, kNoiseOn, kHiss, kWireOn, kReflect, kLength
};

enum Stage { kSlewStage = 0, kDeviceStage, kMillerStage, kBandwidthStage, kLoopStage, kConverterStage, kNoiseStage, kWireStage, kStageCount };

// The order of the stages in each algorithm. Algorithm 2 also feeds the output back into the device.
constexpr std::array<std::array<Stage, kStageCount>, 6> kOrders = {{
    {kSlewStage, kDeviceStage, kMillerStage, kBandwidthStage, kLoopStage, kConverterStage, kNoiseStage, kWireStage},
    {kWireStage, kBandwidthStage, kDeviceStage, kMillerStage, kSlewStage, kLoopStage, kNoiseStage, kConverterStage},
    {kDeviceStage, kMillerStage, kBandwidthStage, kSlewStage, kLoopStage, kConverterStage, kNoiseStage, kWireStage},
    {kConverterStage, kSlewStage, kDeviceStage, kMillerStage, kBandwidthStage, kLoopStage, kNoiseStage, kWireStage},
    {kNoiseStage, kDeviceStage, kMillerStage, kSlewStage, kBandwidthStage, kLoopStage, kConverterStage, kWireStage},
    {kLoopStage, kDeviceStage, kMillerStage, kBandwidthStage, kSlewStage, kConverterStage, kNoiseStage, kWireStage},
}};
constexpr int kFactors[3] = {1, 2, 4};
constexpr double kMaxWireMs = 5;

// A noise source that gives the same numbers here and in the web app.
class Noise {
public:
  explicit Noise(std::uint32_t seed) : state_(seed) {}
  double next() {   // -1 to 1
    state_ = state_ * 1664525u + 1013904223u;
    return state_ / 2147483648.0 - 1;
  }

private:
  std::uint32_t state_;
};

double clampTo(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }
int asInt(double v, int hi) { return std::min(hi, std::max(0, jsRoundInt(v))); }

// What one channel remembers between samples.
struct Channel {
  double slew = 0, level = 0, miller = 0, bandwidth = 0, loopLow = 0, loopBand = 0, previous = 0, previousWet = 0, blocker = 0;
  std::vector<double> wire;
  int wirePos = 0;
  Noise noise, jitterNoise;
  Channel(int wireLength, std::uint32_t noiseSeed, std::uint32_t jitterSeed) : wire(static_cast<std::size_t>(wireLength), 0.0), noise(noiseSeed), jitterNoise(jitterSeed) {}
};

class Circuit final : public Node {
public:
  explicit Circuit(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        chains_{{Oversampler(1), Oversampler(1)}, {Oversampler(2), Oversampler(2)}, {Oversampler(4), Oversampler(4)}},
        dryDelays_{FixedDelay(48), FixedDelay(48)},
        wireLength_(static_cast<int>(std::ceil(context.sampleRate * kMaxWireMs / 1000)) + 2),
        channels_{Channel(wireLength_, 12345, 54321), Channel(wireLength_, 12345 + 977, 54321 + 313)},
        blockK_(1 - js::exp(-2 * kPi * 12 / context.sampleRate)),
        followK_(1 - js::exp(-1 / (context.sampleRate * 0.005))),
        drifter_(777) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const int algorithm = asInt(p(kAlgorithm), 5);
    const std::array<Stage, kStageCount>& order = kOrders[static_cast<std::size_t>(algorithm)];
    const double mix = p(kMix), level = p(kLevel);
    factor_ = asInt(p(kOversample), 2);
    feedback_ = algorithm == 2 ? p(kFeedback) : 0;
    // the slow wander: a new target now and then, approached over a second or two
    driftLeft_ -= n;
    if (driftLeft_ <= 0) { driftTarget_ = drifter_.next(); driftLeft_ = sampleRate_ * 1.2; }
    drift_ += (driftTarget_ - drift_) * (1 - js::exp(-n / (sampleRate_ * 1.5)));
    const double wander = drift_ * p(kDrift);
    const bool on[kStageCount] = {p(kSlewOn) >= 0.5, p(kDeviceOn) >= 0.5, p(kMillerOn) >= 0.5, p(kBandwidthOn) >= 0.5,
                                  p(kLoopOn) >= 0.5, p(kConverterOn) >= 0.5, p(kNoiseOn) >= 0.5, p(kWireOn) >= 0.5};

    slewStep_ = p(kSlew) / (sampleRate_ / 1000);
    drive_ = p(kDrive);
    bias_ = clampTo(p(kBias) + wander * 0.1, -1, 1);
    biasTanh_ = js::tanh(bias_);
    norm_ = 1 / js::tanh(drive_);
    millerAmount_ = p(kMiller) * 6;
    millerFreq_ = p(kMillerFreq) * js::pow(2, wander * 0.3);
    bandwidthK_ = 1 - js::exp(-2 * kPi * std::min(p(kBandwidth) * js::pow(2, wander * 0.3), sampleRate_ * 0.45) / sampleRate_);
    const double g = js::tan(kPi * std::min(p(kLoopFreq), sampleRate_ * 0.45) / sampleRate_);
    const double damping = 2 * clampTo(p(kMargin) / 100, 0.2, 0.95);   // k = 2 zeta, zeta about the phase margin over 100
    a1_ = 1 / (1 + g * (g + damping)); a2_ = g * a1_; a3_ = g * a2_;
    quantum_ = js::pow(2, asInt(p(kBits), 24) - 1);
    jitterAmount_ = p(kJitter) * 0.02;
    hiss_ = js::pow(10, p(kHiss) / 20) * std::sqrt(3.0);   // (a uniform noise of unit variance, times the level)
    reflect_ = p(kReflect);
    wireSamples_ = static_cast<int>(clampTo(static_cast<double>(jsRoundInt(p(kLength) * sampleRate_ / 1000)), 1, wireLength_ - 2));
    const int wait = on[kDeviceStage] ? chains_[factor_][0].latency() : 0;

    for (int c = 0; c < 2; c++) {
      const float* x = inputs[c];
      float* out = outputs[c];
      Channel& ch = channels_[c];
      for (int i = 0; i < n; i++) {
        double wet = x[i];
        for (const Stage stage : order) if (on[stage]) wet = runStage(stage, wet, ch, c);
        ch.previousWet = wet;
        const double dry = dryDelays_[c].push(x[i], wait);
        out[i] = static_cast<float>((dry + (wet - dry) * mix) * level);
      }
      if (std::abs(ch.loopLow) < 1e-20) ch.loopLow = 0;   // no slow denormals while silent
      if (std::abs(ch.loopBand) < 1e-20) ch.loopBand = 0;
    }
  }

private:
  // The device's curve: tanh of the driven, biased signal, the bias's own output taken off, scaled to unity for full scale.
  double curve(double v) const { return (js::tanh(v * drive_ + bias_) - biasTanh_) * norm_; }

  double runStage(Stage stage, double v, Channel& ch, int c) {
    switch (stage) {
      case kSlewStage:
        ch.slew += clampTo(v - ch.slew, -slewStep_, slewStep_);
        return ch.slew;
      case kDeviceStage: {
        const double wet = chains_[factor_][c].run(v - feedback_ * ch.previousWet, [&](double u) { return curve(u); });
        ch.blocker += (wet - ch.blocker) * blockK_;   // follows the offset
        return wet - ch.blocker;
      }
      case kMillerStage: {
        ch.level += (std::abs(v) - ch.level) * followK_;
        const double k = 1 - js::exp(-2 * kPi * millerFreq_ / (1 + millerAmount_ * ch.level) / sampleRate_);
        ch.miller += (v - ch.miller) * k;
        return ch.miller;
      }
      case kBandwidthStage:
        ch.bandwidth += (v - ch.bandwidth) * bandwidthK_;
        return ch.bandwidth;
      case kLoopStage: {   // a trapezoidal state-variable lowpass
        const double v3 = v - ch.loopLow, v1 = a1_ * ch.loopBand + a2_ * v3, v2 = ch.loopLow + a2_ * ch.loopBand + a3_ * v3;
        ch.loopBand = 2 * v1 - ch.loopBand;
        ch.loopLow = 2 * v2 - ch.loopLow;
        return v2;
      }
      case kConverterStage: {
        const double moved = v + jitterAmount_ * ch.jitterNoise.next() * (v - ch.previous);   // the error grows with the slope
        ch.previous = v;
        return std::floor(moved * quantum_ + 0.5) / quantum_;
      }
      case kNoiseStage:
        return v + hiss_ * ch.noise.next();
      default: {   // kWireStage
        ch.wire[static_cast<std::size_t>(ch.wirePos)] = v;
        const double behind = ch.wire[static_cast<std::size_t>((ch.wirePos - wireSamples_ + wireLength_) % wireLength_)];
        ch.wirePos = (ch.wirePos + 1) % wireLength_;
        return v + reflect_ * behind;
      }
    }
  }

  double sampleRate_;
  Oversampler chains_[3][2];
  FixedDelay dryDelays_[2];
  int wireLength_;
  Channel channels_[2];
  double blockK_, followK_;
  Noise drifter_;
  double drift_ = 0, driftTarget_ = 0, driftLeft_ = 0;   // the slow wander, -1 to 1

  // what the block's settings come to; the stages read it
  int factor_ = 0, wireSamples_ = 1;
  double slewStep_ = 0, drive_ = 1, bias_ = 0, biasTanh_ = 0, norm_ = 1, feedback_ = 0, millerAmount_ = 0, millerFreq_ = 1, bandwidthK_ = 0;
  double a1_ = 0, a2_ = 0, a3_ = 0, quantum_ = 1, jitterAmount_ = 0, hiss_ = 0, reflect_ = 0;
};

}  // namespace

void registerCircuit(NodeRegistry& registry) {
  NodeType type;
  type.type = "circuit";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.latency = [](const std::map<std::string, double>& settings, double) {
    return settings.at("device_on") != 0 ? static_cast<double>(oversampleLatency(kFactors[asInt(settings.at("oversample"), 2)])) : 0.0;
  };
  type.params = {
    {"algorithm", 0, 5, 0, true},
    {"mix", 0, 1, 1},
    {"level", 0, 2, 0.7},
    {"oversample", 0, 2, 1, true},          // 0: 1x, 1: 2x, 2: 4x
    {"drift", 0, 1, 0},
    {"feedback", 0, 0.9, 0.3},              // algorithm 2: how much of the output goes back into the device
    {"slew_on", 0, 1, 1, true},
    {"slew", 0.5, 400, 60},                 // full scale per millisecond
    {"device_on", 0, 1, 1, true},
    {"drive", 0.1, 20, 2},
    {"bias", -1, 1, 0.15},
    {"miller_on", 0, 1, 1, true},
    {"miller", 0, 1, 0.4},
    {"millerfreq", 500, 20000, 9000},       // Hz
    {"bandwidth_on", 0, 1, 1, true},
    {"bandwidth", 200, 20000, 14000},       // Hz
    {"loop_on", 0, 1, 0, true},
    {"loopfreq", 1000, 20000, 12000},       // Hz
    {"margin", 20, 90, 60},                 // degrees of phase margin
    {"converter_on", 0, 1, 0, true},
    {"bits", 2, 24, 16, true},
    {"jitter", 0, 1, 0},
    {"noise_on", 0, 1, 0, true},
    {"hiss", -100, -30, -80},               // dB
    {"wire_on", 0, 1, 0, true},
    {"reflect", -1, 1, 0.3},
    {"length", 0.02, kMaxWireMs, 0.5},      // ms
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Circuit>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

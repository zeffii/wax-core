// Additive synth: up to 32 sine partials whose levels you draw (the spectrum, kept in the node's `data.spectrum`),
// shaped by a few controls that move every partial at once: tilt, evenodd, stretch, drift, hdecay, width.
// Monophonic, last note wins, with glide. Partials are rotating phasors, so the cost is a few multiplies per partial per
// sample, and anything past 0.45 * the sample rate is skipped. Port of webtracker src/audio/nodes/addsynth.js.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/adsr.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kHarmonics = 0, kTilt, kEvenOdd, kStretch, kDrift, kHDecay, kWidth, kTune, kGlide, kAtk, kDec, kSus, kRel, kVel, kLevel };

constexpr int kHarmonicsMax = 32;

// A stored spectrum of any length, resampled to 32 values; falls back to a saw.
void normalizeSpectrum(const Json* data, float* out) {
  auto saw = [&] {
    for (int i = 0; i < kHarmonicsMax; i++) out[i] = static_cast<float>(jsRound(std::min(1.0, std::max(0.0, 1.0 / (i + 1))) * 1000) / 1000);
  };
  std::vector<double> a;
  if (!data || !data->contains("spectrum") || !(*data)["spectrum"].is_array()) { saw(); return; }
  for (const auto& v : (*data)["spectrum"]) {
    if (!v.is_number()) { saw(); return; }
    a.push_back(v.get<double>());
  }
  if (a.empty()) { saw(); return; }
  for (int i = 0; i < kHarmonicsMax; i++) {
    out[i] = static_cast<float>(std::min(1.0, std::max(0.0, a[std::min(static_cast<std::size_t>(i), a.size() - 1)])));
  }
}

class AddSynth final : public Node {
public:
  explicit AddSynth(const NodeContext& context)
      : sampleRate_(context.sampleRate), pitchOn_(!context.connected.empty() && context.connected[0]), amp_(context.sampleRate) {
    normalizeSpectrum(context.data, spectrum_);
    for (int i = 0; i < kHarmonicsMax; i++) {
      const double x = js::sin((i + 1) * 12.9898) * 43758.5453;
      drifts_[i] = (x - std::floor(x)) * 2 - 1;   // fixed pseudo-random, -1..1
    }
    const std::size_t block = static_cast<std::size_t>(std::max(128, context.maxBlock));
    env_.assign(block, 0.0); sumO_.assign(block, 0.0); sumE_.assign(block, 0.0);
    reset();
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      if (amp_.idle()) { reset(); pos_ = event.note; }
      else time_ = 0;   // a retrigger brightens again
      note_ = event.note; velocity_ = event.vel;
      amp_.on();
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) {
      amp_.off();
    } else if (event.type == NoteType::Cut) {
      amp_.cut();
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (amp_.idle()) { std::fill(outL, outL + n, 0.0f); std::fill(outR, outR + n, 0.0f); return; }
    auto g = [&](int i) -> double { return params[slots[i]]; };
    amp_.set(g(kAtk), g(kDec), g(kSus), g(kRel));
    const double glideMs = g(kGlide);
    if (glideMs > 0.5) pos_ += (note_ - pos_) * (1 - js::exp(-n / (sampleRate_ * glideMs * 0.001 / 3)));
    else pos_ = note_;
    const int count = jsRoundInt(g(kHarmonics));
    const double tilt = g(kTilt), eo = g(kEvenOdd);
    const double B = js::pow(g(kStretch), 2) * 0.004, drift = g(kDrift), hdecay = g(kHDecay), width = g(kWidth);
    const double f0 = 440 * js::pow(2, (pos_ + g(kTune) - 69) / 12);
    const double dt = n / sampleRate_;
    const double nyq = sampleRate_ * 0.45;
    const double pitchMul = pitchOn_ ? js::pow(2, inputs[0][0]) : 1;
    const double level = g(kLevel) * (1 - g(kVel) + g(kVel) * velocity_) * 0.4;
    // targets for this block, with the level normalised so a spectrum's shape does not change loudness
    double energy = 0;
    std::fill(std::begin(target_), std::end(target_), 0.0);
    for (int h = 0; h < count; h++) {
      const int m = h + 1;
      const double freq = f0 * pitchMul * m * std::sqrt(1 + B * m * m) * js::pow(2, (drift * 8 * drifts_[h]) / 1200);
      if (freq >= nyq) continue;
      double a = spectrum_[h] * js::pow(10, (tilt * js::log2(m)) / 20);
      a *= m % 2 ? std::min(1.0, 1 - eo) : std::min(1.0, 1 + eo);
      energy += a * a;
      target_[h] = a;
      const double w = (2 * kPi * freq) / sampleRate_;
      c_[h] = js::cos(w); sn_[h] = js::sin(w);
    }
    const double norm = energy > 1e-12 ? level / std::sqrt(energy) : 0;
    double peakA = 0;
    for (int h = 0; h < count; h++) peakA = std::max(peakA, target_[h] * norm);
    if (env_.size() < static_cast<std::size_t>(n)) { env_.assign(static_cast<std::size_t>(n), 0.0); sumO_.assign(static_cast<std::size_t>(n), 0.0); sumE_.assign(static_cast<std::size_t>(n), 0.0); }
    for (int i = 0; i < n; i++) env_[static_cast<std::size_t>(i)] = amp_.next();
    // Odd and even partials sit on opposite sides of the stereo field, so each group is added up on its own and panned
    // once at the end; the envelope too is applied once to the sums, not to every partial.
    std::fill(sumO_.begin(), sumO_.begin() + n, 0.0); std::fill(sumE_.begin(), sumE_.begin() + n, 0.0);
    const double quiet = peakA * 1e-4;   // a partial this far (-80 dB) below the loudest adds nothing to hear
    for (int h = 0; h < count; h++) {
      const double bright = hdecay > 0 ? js::exp(-hdecay * js::log2(h + 1) * (time_ + dt)) : 1;   // higher partials die faster
      const double a1 = target_[h] * norm * bright, a0 = aPrev_[h];
      if (a0 < quiet && a1 < quiet) { aPrev_[h] = a1; continue; }
      aPrev_[h] = a1;
      std::vector<double>& sum = (h & 1) ? sumE_ : sumO_;   // partial h + 1: even partials are the odd indexes
      double x = re_[h], y = im_[h];
      const double cc = c_[h], ss = sn_[h], step = (a1 - a0) / n;
      double a = a0;
      for (int i = 0; i < n; i++) {
        const double nx = x * cc - y * ss;
        y = x * ss + y * cc; x = nx;
        a += step;
        sum[static_cast<std::size_t>(i)] += y * a;
      }
      const double mag = 1 / std::sqrt(x * x + y * y);
      re_[h] = x * mag; im_[h] = y * mag;
    }
    {
      const double angO = ((-width + 1) * kPi) / 4, angE = ((width + 1) * kPi) / 4;
      const double olo = js::cos(angO) * kSqrt2, oro = js::sin(angO) * kSqrt2, ole = js::cos(angE) * kSqrt2, ore = js::sin(angE) * kSqrt2;
      for (int i = 0; i < n; i++) {
        const double e = env_[static_cast<std::size_t>(i)], o = sumO_[static_cast<std::size_t>(i)] * e, v = sumE_[static_cast<std::size_t>(i)] * e;
        outL[i] = static_cast<float>(o * olo + v * ole); outR[i] = static_cast<float>(o * oro + v * ore);
      }
    }
    time_ += dt;
  }

private:
  void reset() {
    for (int i = 0; i < kHarmonicsMax; i++) { re_[i] = 1; im_[i] = 0; aPrev_[i] = 0; }
    time_ = 0;
  }

  double sampleRate_;
  bool pitchOn_;
  Adsr amp_;
  float spectrum_[kHarmonicsMax] = {};
  double re_[kHarmonicsMax] = {}, im_[kHarmonicsMax] = {};
  double aPrev_[kHarmonicsMax] = {};   // gain at the end of the last block
  double drifts_[kHarmonicsMax] = {};
  double target_[kHarmonicsMax] = {}, c_[kHarmonicsMax] = {}, sn_[kHarmonicsMax] = {};
  std::vector<double> env_, sumO_, sumE_;
  int note_ = 60;
  double pos_ = 60, velocity_ = 1, time_ = 0;
};

}  // namespace

void registerAddSynth(NodeRegistry& registry) {
  NodeType type;
  type.type = "addsynth";
  type.inputs = {"pitch"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"harmonics", 1, kHarmonicsMax, kHarmonicsMax, true},
    {"tilt", -12, 12, 0},
    {"evenodd", -1, 1, 0},
    {"stretch", 0, 1, 0},
    {"drift", 0, 1, 0},
    {"hdecay", 0, 20, 0},
    {"width", 0, 1, 0.4},
    {"tune", -48, 48, 0},
    {"glide", 0, 2000, 0},
    {"atk", 0.1, 5000, 5},
    {"dec", 1, 5000, 400},
    {"sus", 0, 1, 0.8},
    {"rel", 1, 5000, 300},
    {"vel", 0, 1, 0.5},
    {"level", 0, 1, 0.5},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<AddSynth>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

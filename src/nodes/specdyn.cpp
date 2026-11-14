// Spectral compressor with a tilt: a compressor for every frequency bin at once, with the gain smoothed across neighbouring bins,
// then `tilt` dB per octave about 1 kHz. Stereo, each side on its own; latency one frame (`size`). Port of webtracker
// src/audio/nodes/specdyn.js.
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/stft.h"

namespace wax {
namespace {

enum Param { kThresh = 0, kRatio, kTilt, kAttack, kRelease, kSmooth, kMakeup, kMix, kSize };

class SpecDyn final : public Node {
public:
  explicit SpecDyn(const NodeContext& context)
      : sampleRate_(context.sampleRate), re_(kStftMaxSize), im_(kStftMaxSize), want_(kStftMaxSize / 2 + 1), soft_(kStftMaxSize / 2 + 1) {}

  void process(const float* const* inputs, float* const* outputs, int count, const float* params, const int* slots) override {
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const int size = stftSizeOf(params[slots[kSize]]);
    if (size != n_) start(size);
    const int hop = n_ / 4;
    const double mix = g(kMix);
    Settings p;
    p.threshold = g(kThresh);
    p.ratio = std::max(1.0, g(kRatio));
    p.tilt = g(kTilt);
    p.makeup = g(kMakeup);
    p.attack = js::exp(-hop / (sampleRate_ * g(kAttack) * 0.001));
    p.release = js::exp(-hop / (sampleRate_ * g(kRelease) * 0.001));
    p.reach = jsRoundInt(g(kSmooth) * 8);
    for (int c = 0; c < 2; c++) {
      Channel& s = *ch_[c];
      for (int i = 0; i < count; i++) {
        if (s.a.push(inputs[c][i])) { s.a.fill(re_.data(), im_.data()); frame(s, p); }
        const double dry = s.a.delayed(), wet = s.r.take(s.a.t());
        outputs[c][i] = static_cast<float>(dry + (wet - dry) * mix);
      }
    }
  }

private:
  struct Settings { double threshold, ratio, tilt, makeup, attack, release; int reach; };
  struct Channel {
    explicit Channel(int n) : a(n), r(n), gain(kStftMaxSize / 2 + 1, 0.0) {}
    Analyser a;
    Resynth r;
    std::vector<double> gain;   // each bin's gain now, in dB
  };

  void start(int size) {
    n_ = size;
    for (auto& c : ch_) c = std::make_unique<Channel>(n_);
  }

  void frame(Channel& s, const Settings& p) {
    const int half = n_ >> 1;
    const double peak = stftPeakOf(n_), binHz = sampleRate_ / n_;
    for (int k = 0; k <= half; k++) {
      const double level = 20 * js::log10(jsHypot(re_[k], im_[k]) / peak + 1e-9);
      const double over = level - p.threshold;
      want_[k] = over > 0 ? -over * (1 - 1 / p.ratio) : 0;
    }
    const int reach = p.reach;
    for (int k = 0; k <= half; k++) {
      if (reach == 0) { soft_[k] = want_[k]; continue; }
      double sum = 0;
      int count = 0;
      for (int j = std::max(0, k - reach); j <= std::min(half, k + reach); j++) { sum += want_[j]; count++; }
      soft_[k] = sum / count;
    }
    for (int k = 0; k <= half; k++) {
      const double g = s.gain[k], a = soft_[k] < g ? p.attack : p.release;
      s.gain[k] = a * g + (1 - a) * soft_[k];
      const double tilt = k == 0 ? 0 : p.tilt * js::log2((k * binHz) / 1000);
      const double f = js::pow(10, (s.gain[k] + tilt + p.makeup) / 20);
      re_[k] *= f; im_[k] *= f;
    }
    mirrorSpectrum(re_.data(), im_.data(), n_);
    s.r.add(re_.data(), im_.data(), s.a.t());
  }

  double sampleRate_;
  std::vector<double> re_, im_, want_, soft_;
  int n_ = 0;
  std::unique_ptr<Channel> ch_[2];
};

}  // namespace

void registerSpecDyn(NodeRegistry& registry) {
  NodeType type;
  type.type = "specdyn";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.latency = [](const std::map<std::string, double>& settings, double) { return static_cast<double>(stftSizeOf(settings.at("size")) - 1); };
  type.params = {{"threshold", -80, 0, -36}, {"ratio", 1, 20, 4}, {"tilt", -6, 6, 0}, {"attack", 1, 500, 20}, {"release", 10, 3000, 250},
                 {"smooth", 0, 1, 0.3}, {"makeup", -12, 24, 0}, {"mix", 0, 1, 1}, {"size", 0, 2, 1, true}};
  type.create = [](const NodeContext& context) { return std::make_unique<SpecDyn>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

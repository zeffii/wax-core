// Cross synthesis: the spectral shape of the `mod` input is laid over the sound at `in`; the carrier keeps its own phases.
// Stereo, each side on its own; latency one frame (`size`). Port of webtracker src/audio/nodes/xsynth.js. Also the two nodes that
// only hold settings: `listen` (the runtime plays the heard notes) and `oscout` (the Open Sound Control relay is a page feature).
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

enum Param { kAmount = 0, kSmooth, kDetail, kMix, kSize };
constexpr double kBoost = 30;   // the most any bin is lifted, in dB

class XSynth final : public Node {
public:
  explicit XSynth(const NodeContext&)
      : re_(kStftMaxSize), im_(kStftMaxSize), reM_(kStftMaxSize), imM_(kStftMaxSize), magC_(bins()), magM_(bins()), envC_(bins()), envM_(bins()), sum_(bins() + 1) {}

  void process(const float* const* inputs, float* const* outputs, int count, const float* params, const int* slots) override {
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const int size = stftSizeOf(params[slots[kSize]]);
    if (size != n_) start(size);
    const double mix = g(kMix);
    const double amount = g(kAmount), smooth = g(kSmooth) * 6, detail = g(kDetail);
    for (int c = 0; c < 2; c++) {
      Channel& s = *ch_[c];
      for (int i = 0; i < count; i++) {
        s.mod.push(inputs[2 + c][i]);
        if (s.carrier.push(inputs[c][i])) { s.carrier.fill(re_.data(), im_.data()); frame(s, amount, smooth, detail); }
        const double dry = s.carrier.delayed(), wet = s.out.take(s.carrier.t());
        outputs[c][i] = static_cast<float>(dry + (wet - dry) * mix);
      }
    }
  }

private:
  static std::size_t bins() { return kStftMaxSize / 2 + 1; }
  struct Channel {
    explicit Channel(int n) : carrier(n), mod(n), out(n) {}
    Analyser carrier, mod;
    Resynth out;
  };

  void start(int size) {
    n_ = size;
    for (auto& c : ch_) c = std::make_unique<Channel>(n_);
  }

  // The broad shape of a magnitude spectrum: a box average whose width grows with the bin, as pitch does.
  void envelope(const std::vector<double>& mag, std::vector<double>& out, int half, double smooth) {
    sum_[0] = 0;
    for (int k = 0; k <= half; k++) sum_[k + 1] = sum_[k] + mag[k];
    for (int k = 0; k <= half; k++) {
      const double r = std::max(1.0, std::floor(smooth * (2 + k * 0.06)));
      const int lo = std::max(0, static_cast<int>(k - r)), hi = std::min(half, static_cast<int>(k + r));
      out[k] = (sum_[hi + 1] - sum_[lo]) / (hi - lo + 1);
    }
  }

  void frame(Channel& s, double amount, double smooth, double detail) {
    const int half = n_ >> 1;
    const double floor = n_ * 1e-6, limit = js::pow(10, kBoost / 20);
    s.mod.fill(reM_.data(), imM_.data());
    for (int k = 0; k <= half; k++) { magC_[k] = jsHypot(re_[k], im_[k]); magM_[k] = jsHypot(reM_[k], imM_[k]); }
    envelope(magC_, envC_, half, smooth);
    envelope(magM_, envM_, half, smooth);
    for (int k = 0; k <= half; k++) {
      double g = js::pow((envM_[k] + floor) / (envC_[k] + floor), amount);
      if (detail > 0) g *= js::pow((magM_[k] + floor) / (envM_[k] + floor), detail);
      g = std::min(limit, g);
      re_[k] *= g; im_[k] *= g;
    }
    mirrorSpectrum(re_.data(), im_.data(), n_);
    s.out.add(re_.data(), im_.data(), s.carrier.t());
  }

  std::vector<double> re_, im_, reM_, imM_, magC_, magM_, envC_, envM_, sum_;
  int n_ = 0;
  std::unique_ptr<Channel> ch_[2];
};

class Inert final : public Node {
public:
  void process(const float* const*, float* const*, int, const float*, const int*) override {}
};

}  // namespace

void registerXSynth(NodeRegistry& registry) {
  {
    NodeType type;
    type.type = "xsynth";
    type.inputs = {"in", "mod"};
    type.outputs = {"out"};
    type.kinds = {{"in", PortKind::Stereo}, {"mod", PortKind::Stereo}, {"out", PortKind::Stereo}};
    type.isEffect = true;
    type.latency = [](const std::map<std::string, double>& settings, double) { return static_cast<double>(stftSizeOf(settings.at("size")) - 1); };
    type.params = {{"amount", 0, 1, 1}, {"smooth", 0, 1, 0.3}, {"detail", 0, 1, 0}, {"mix", 0, 1, 1}, {"size", 0, 2, 1, true}};
    type.create = [](const NodeContext& context) { return std::make_unique<XSynth>(context); };
    registry.add(std::move(type));
  }
  {
    NodeType type;
    type.type = "listen";
    type.shared = true;
    type.params = {{"on", 0, 1, 1, true}, {"transpose", -48, 48, 0, true}, {"level", 0, 2, 1}};
    type.create = [](const NodeContext&) { return std::make_unique<Inert>(); };
    registry.add(std::move(type));
  }
  {
    NodeType type;
    type.type = "oscout";
    type.shared = true;
    type.params = {{"mode", 0, 1, 0, true}, {"instrument", 0, 254, 0, true}, {"track", -1, 254, -1, true}, {"transpose", -48, 48, 0, true}};
    type.create = [](const NodeContext&) { return std::make_unique<Inert>(); };
    registry.add(std::move(type));
  }
}

}  // namespace wax

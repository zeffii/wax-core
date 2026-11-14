// Body blur: each frame is split into its body (held tones) and its hits; only the body is smoothed (across frequency, and over
// time) and rounded at the top, the hits go through untouched. Latency is one frame (`size`); the dry sound is delayed to match.
// Port of webtracker src/audio/nodes/bodyblur.js.
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/hpmask.h"
#include "nodes/stft.h"

namespace wax {
namespace {

enum Param { kSmooth = 0, kTime, kRound, kSplit, kSize, kQuality, kMix };
constexpr double kLean = 3, kLengthMs = 250, kWidthHz = 250;

class BodyBlur final : public Node {
public:
  explicit BodyBlur(const NodeContext& context)
      : sampleRate_(context.sampleRate), re_(kStftMaxSize), im_(kStftMaxSize), mag_(kStftMaxSize / 2 + 1), soft_(kStftMaxSize / 2 + 1), sum_(kStftMaxSize / 2 + 2) {}

  void process(const float* const* inputs, float* const* outputs, int count, const float* params, const int* slots) override {
    const int want = stftSizeOf(params[slots[kSize]]);
    if (want != n_) start(want);
    const int hop = n_ / 4;
    const double mix = params[slots[kMix]], time = params[slots[kTime]];
    Settings p;
    p.split = maskSettings(sampleRate_, n_, kLengthMs, kWidthHz, params[slots[kSplit]], kLean);
    p.radius = jsRoundInt(static_cast<double>(params[slots[kSmooth]]) * 8);
    p.a = time < 1 ? 0 : js::exp(-hop / (sampleRate_ * time * 0.001));
    p.cut = &highCut(params[slots[kRound]]);
    p.eco = params[slots[kQuality]] >= 0.5;
    for (int c = 0; c < 2; c++) {
      Channel& s = *ch_[c];
      for (int i = 0; i < count; i++) {
        if (s.a.push(inputs[c][i])) { s.a.fill(re_.data(), im_.data()); frame(s, p); }
        const double wet = s.out.take(s.a.t()), dry = s.a.delayed();
        outputs[c][i] = static_cast<float>(dry + (wet - dry) * mix);
      }
    }
  }

private:
  struct Settings {
    MaskSettings split;
    int radius = 0;
    double a = 0;
    const std::vector<double>* cut = nullptr;
    bool eco = false;
  };
  struct Channel {
    explicit Channel(int n) : a(n), out(n), mask(n), body(n / 2 + 1, 0.0), avg(n / 2 + 1, 0.0) {}
    Analyser a;
    Resynth out;
    Masker mask;
    std::vector<double> body, avg;
    bool primed = false;
    int count = 0;
  };

  void start(int size) {
    n_ = size;
    for (auto& c : ch_) c = std::make_unique<Channel>(n_);
  }

  // Blur across frequency: a box a few bins wide that keeps the body's energy.
  void blurAcross(int half, int radius) {
    sum_[0] = 0;
    double before = 0, after = 0;
    for (int k = 0; k <= half; k++) { sum_[k + 1] = sum_[k] + mag_[k]; before += mag_[k] * mag_[k]; }
    for (int k = 0; k <= half; k++) {
      const int lo = std::max(0, k - radius), hi = std::min(half, k + radius);
      soft_[k] = (sum_[hi + 1] - sum_[lo]) / (hi - lo + 1);
      after += soft_[k] * soft_[k];
    }
    const double keep = after > 1e-18 ? std::sqrt(before / after) : 1;
    for (int k = 0; k <= half; k++) soft_[k] *= keep;
  }

  void frame(Channel& s, const Settings& p) {
    const int half = n_ >> 1;
    s.mask.listen(re_.data(), im_.data());
    if (!p.eco || s.count % 2 == 0) s.mask.body(p.split, s.body.data());
    s.count++;
    for (int k = 0; k <= half; k++) mag_[k] = jsHypot(re_[k], im_[k]) * s.body[k];
    if (p.radius > 0) blurAcross(half, p.radius); else for (int k = 0; k <= half; k++) soft_[k] = mag_[k];
    if (!s.primed) { for (int k = 0; k <= half; k++) s.avg[k] = soft_[k]; s.primed = true; }
    else for (int k = 0; k <= half; k++) s.avg[k] = p.a * s.avg[k] + (1 - p.a) * soft_[k];
    const std::vector<double>& cut = *p.cut;
    for (int k = 0; k <= half; k++) {
      const double m = jsHypot(re_[k], im_[k]), body = s.avg[k] * cut[k];
      const double unit = m > 1e-12 ? body / m : 0, hit = 1 - s.body[k];
      re_[k] = re_[k] * hit + re_[k] * unit;
      im_[k] = im_[k] * hit + im_[k] * unit;
    }
    mirrorSpectrum(re_.data(), im_.data(), n_);
    s.out.add(re_.data(), im_.data(), s.a.t());
  }

  // The high cut's gain for every bin, worked out when `round` or the frame length changes.
  const std::vector<double>& highCut(double round) {
    if (cutFor_ == n_ && cutRound_ == round) return cut_;
    cutFor_ = n_; cutRound_ = round;
    cut_.assign(static_cast<std::size_t>(n_ / 2 + 1), 0.0);
    const double corner = 800 + 19200 * (1 - round) * (1 - round);
    for (std::size_t k = 0; k < cut_.size(); k++) {
      const double f = (static_cast<double>(k) * sampleRate_) / n_;
      cut_[k] = round <= 0 ? 1 : 1 / std::sqrt(1 + (f / corner) * (f / corner));
    }
    return cut_;
  }

  double sampleRate_;
  std::vector<double> re_, im_, mag_, soft_, sum_;
  int n_ = 0;
  std::unique_ptr<Channel> ch_[2];
  int cutFor_ = -1;
  double cutRound_ = -1;
  std::vector<double> cut_;
};

}  // namespace

void registerBodyBlur(NodeRegistry& registry) {
  NodeType type;
  type.type = "bodyblur";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.latency = [](const std::map<std::string, double>& settings, double) { return static_cast<double>(stftSizeOf(settings.at("size")) - 1); };
  type.params = {{"smooth", 0, 1, 0.4}, {"time", 0, 500, 80}, {"round", 0, 1, 0.25}, {"split", 1, 8, 2}, {"size", 0, 2, 1, true}, {"quality", 0, 1, 0, true}, {"mix", 0, 1, 1}};
  type.create = [](const NodeContext& context) { return std::make_unique<BodyBlur>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

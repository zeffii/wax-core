// Harmonic / percussive split: the held tones go to `harm`, the hits to `perc`, the two masks adding up to one (see hpmask.h).
// Stereo, each side on its own. Latency is one frame (`size`). Port of webtracker src/audio/nodes/hpss.js.
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

enum Param { kLength = 0, kWidth, kSharp, kSize };

class Hpss final : public Node {
public:
  explicit Hpss(const NodeContext& context)
      : sampleRate_(context.sampleRate), re_(kStftMaxSize), im_(kStftMaxSize), reP_(kStftMaxSize), imP_(kStftMaxSize), body_(kStftMaxSize / 2 + 1) {}

  void process(const float* const* inputs, float* const* outputs, int count, const float* params, const int* slots) override {
    const int want = stftSizeOf(params[slots[kSize]]);
    if (want != n_) start(want);
    const MaskSettings p = maskSettings(sampleRate_, n_, params[slots[kLength]], params[slots[kWidth]], params[slots[kSharp]]);
    for (int c = 0; c < 2; c++) {
      Channel& s = *ch_[c];
      const float* x = inputs[c];
      float* h = outputs[c];
      float* q = outputs[2 + c];
      for (int i = 0; i < count; i++) {
        if (s.a.push(x[i])) { s.a.fill(re_.data(), im_.data()); frame(s, p); }
        h[i] = static_cast<float>(s.harm.take(s.a.t()));
        q[i] = static_cast<float>(s.perc.take(s.a.t()));
      }
    }
  }

private:
  struct Channel {
    explicit Channel(int n) : a(n), harm(n), perc(n), mask(n) {}
    Analyser a;
    Resynth harm, perc;
    Masker mask;
  };

  void start(int size) {
    n_ = size;
    for (auto& c : ch_) c = std::make_unique<Channel>(n_);
  }

  // One frame of one channel: the spectrum is in re_ and im_.
  void frame(Channel& s, const MaskSettings& p) {
    const int half = n_ >> 1;
    s.mask.listen(re_.data(), im_.data());
    s.mask.body(p, body_.data());
    for (int k = 0; k <= half; k++) {
      const double mh = body_[k], mp = 1 - mh;
      reP_[k] = re_[k] * mp; imP_[k] = im_[k] * mp;
      re_[k] *= mh; im_[k] *= mh;
    }
    mirrorSpectrum(re_.data(), im_.data(), n_);
    mirrorSpectrum(reP_.data(), imP_.data(), n_);
    s.harm.add(re_.data(), im_.data(), s.a.t());
    s.perc.add(reP_.data(), imP_.data(), s.a.t());
  }

  double sampleRate_;
  std::vector<double> re_, im_, reP_, imP_, body_;
  int n_ = 0;
  std::unique_ptr<Channel> ch_[2];
};

}  // namespace

void registerHpss(NodeRegistry& registry) {
  NodeType type;
  type.type = "hpss";
  type.inputs = {"in"};
  type.outputs = {"harm", "perc"};
  type.kinds = {{"in", PortKind::Stereo}, {"harm", PortKind::Stereo}, {"perc", PortKind::Stereo}};
  type.isEffect = true;
  type.latency = [](const std::map<std::string, double>& settings, double) { return static_cast<double>(stftSizeOf(settings.at("size")) - 1); };
  type.params = {{"length", 40, 600, 250}, {"width", 50, 800, 250}, {"sharp", 1, 8, 2}, {"size", 0, 2, 1, true}};
  type.create = [](const NodeContext& context) { return std::make_unique<Hpss>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

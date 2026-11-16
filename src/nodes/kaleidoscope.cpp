// Kaleidoscope: a spectral effect that arrays a band of the sound (from `low` to `high` Hz) across the whole range, mirrored,
// repeated, or as an overtone series. Works frame by frame on the spectrum, so the latency is one frame. Stereo, each side
// on its own. Port of webtracker src/audio/nodes/kaleidoscope.js (without the picture the panel asks for).
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/kalcore.h"
#include "nodes/stft.h"

namespace wax {
namespace {

enum Param { kLow = 0, kHigh, kMode, kRotate, kCopies, kEdge, kLevel, kMix, kSize, kStretch, kDecay, kOvertones, kUnder };

struct Channel {
  Channel(int n, double sampleRate) : a(n), r(n), k(n, sampleRate) {}
  Analyser a;
  Resynth r;
  kal::Kaleidoscoper k;
};

class Kaleidoscope final : public Node {
public:
  explicit Kaleidoscope(const NodeContext& context)
      : probe_(context.probe), sampleRate_(context.sampleRate), re_(kStftMaxSize), im_(kStftMaxSize), reW_(kStftMaxSize), imW_(kStftMaxSize), reD_(kStftMaxSize), imD_(kStftMaxSize) {}

  void process(const float* const* inputs, float* const* outputs, int count, const float* params, const int* slots) override {
    const int want = stftSizeOf(params[slots[kSize]]);
    if (want != n_) {
      n_ = want;
      channels_[0] = std::make_unique<Channel>(n_, sampleRate_);
      channels_[1] = std::make_unique<Channel>(n_, sampleRate_);
    }
    kal::Settings settings{};
    settings.low = params[slots[kLow]];
    settings.high = params[slots[kHigh]];
    settings.mode = jsRoundInt(params[slots[kMode]]);
    settings.rotate = params[slots[kRotate]];
    settings.copies = jsRoundInt(params[slots[kCopies]]);
    settings.edge = params[slots[kEdge]];
    settings.level = js::pow(10, static_cast<double>(params[slots[kLevel]]) / 20);
    settings.stretch = params[slots[kStretch]];
    settings.decay = params[slots[kDecay]];
    settings.overtones = jsRoundInt(params[slots[kOvertones]]);
    settings.under = jsRoundInt(params[slots[kUnder]]);
    const double mix = params[slots[kMix]];
    const int half = n_ >> 1;
    const bool reporting = probe_ && probe_->watched();
    const int half0 = n_ >> 1;
    for (int c = 0; c < 2; c++) {
      Channel& s = *channels_[static_cast<std::size_t>(c)];
      const float* x = inputs[c];
      float* y = outputs[c];
      for (int i = 0; i < count; i++) {
        if (s.a.push(x[i])) {
          s.a.fill(re_.data(), im_.data());
          s.k.run(re_.data(), im_.data(), reW_.data(), imW_.data(), settings);
          if (c == 0 && reporting && ++frames_ % kReportEvery == 0) {   // the dry frame and the arrayed one, for the panel
            std::copy(re_.begin(), re_.begin() + half0 + 1, reD_.begin());
            std::copy(im_.begin(), im_.begin() + half0 + 1, imD_.begin());
            report();
          }
          for (int k = 0; k <= half; k++) {
            const std::size_t j = static_cast<std::size_t>(k);
            re_[j] += (reW_[j] - re_[j]) * mix;
            im_[j] += (imW_[j] - im_[j]) * mix;
          }
          mirrorSpectrum(re_.data(), im_.data(), n_);
          s.r.add(re_.data(), im_.data(), s.a.t());
        }
        y[i] = static_cast<float>(s.r.take(s.a.t()));
      }
    }
  }

private:
  static constexpr int kPoints = 200, kReportEvery = 4;   // values in the picture; frames between two pictures
  void report() {
    std::vector<float> input(kPoints), output(kPoints);
    kal::pictureOf(reD_.data(), imD_.data(), n_, sampleRate_, stftPeakOf(n_), input);
    kal::pictureOf(reW_.data(), imW_.data(), n_, sampleRate_, stftPeakOf(n_), output);
    probe_->publish(input, output);
  }

  std::shared_ptr<SpectrumProbe> probe_;
  int frames_ = 0;
  double sampleRate_;
  int n_ = 0;
  std::array<std::unique_ptr<Channel>, 2> channels_;
  std::vector<double> re_, im_, reW_, imW_, reD_, imD_;
};

}  // namespace

void registerKaleidoscope(NodeRegistry& registry) {
  NodeType type;
  type.type = "kaleidoscope";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {
    {"low", 40, 8000, 440},        // Hz: the bottom of the band
    {"high", 60, 16000, 1760},     // Hz: the top of the band
    {"mode", 0, 2, 0, true},       // 0 mirror, 1 repeat, 2 overtone
    {"rotate", -1, 1, 0},          // band widths
    {"copies", 0, 8, 0, true},     // 0: fill the range
    {"edge", 0, 6, 0.5},           // semitones
    {"level", -24, 12, -6},        // dB
    {"mix", 0, 1, 0.7},
    {"size", 0, 2, 1, true},       // 1024, 2048, 4096 samples
    {"stretch", 0, 1, 0},          // overtone mode: how far the multiples are pulled off the whole numbers
    {"decay", 0, 1, 0.5},          // overtone mode: how fast the higher copies get quieter
    {"overtones", 0, 32, 8, true}, // overtone mode: how many copies above the source (x2, x3 ...)
    {"under", 0, 16, 2, true},     // overtone mode: how many copies below it (x1/2, x1/3 ...)
  };
  type.latency = [](const std::map<std::string, double>& settings, double) { return static_cast<double>(stftSizeOf(settings.at("size")) - 1); };   // one frame
  type.create = [](const NodeContext& context) { return std::make_unique<Kaleidoscope>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

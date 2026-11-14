// Spectral blur: the sound is cut into overlapping frames, each is turned into a spectrum (an FFT), the spectrum is
// blurred like an image, and the frames are turned back into sound and overlapped. `freq` blurs across frequency,
// `time` blurs across time (each bin follows a slow running average), `freeze` stops the running average where it is,
// `phase` goes from the sound's own phases to random ones. The dry sound is delayed to match the frames. Stereo, the
// two sides blurred separately. Latency is one frame: 1024, 2048 or 4096 samples (`size`).
// Port of webtracker src/audio/nodes/blur.js, which carries its own FFT; so does this.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kFreq = 0, kTime, kFreeze, kPhase, kSize, kMix };

constexpr int kSizes[3] = {1024, 2048, 4096};
constexpr int kMax = 4096;

int sizeOf(double v) { return kSizes[std::min(2, std::max(0, jsRoundInt(v)))]; }

struct Twiddles {
  std::vector<double> cos, sin;
  std::vector<int> rev;
};

const Twiddles& twiddles(int n) {
  static std::map<int, Twiddles> tables;
  auto it = tables.find(n);
  if (it != tables.end()) return it->second;
  Twiddles t;
  t.cos.resize(static_cast<std::size_t>(n / 2));
  t.sin.resize(static_cast<std::size_t>(n / 2));
  t.rev.resize(static_cast<std::size_t>(n));
  for (int k = 0; k < n / 2; k++) { t.cos[static_cast<std::size_t>(k)] = js::cos((2 * kPi * k) / n); t.sin[static_cast<std::size_t>(k)] = js::sin((2 * kPi * k) / n); }
  int bits = 0;
  while ((1 << bits) < n) bits++;
  for (int i = 0; i < n; i++) {
    int r = 0;
    for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
    t.rev[static_cast<std::size_t>(i)] = r;
  }
  return tables.emplace(n, std::move(t)).first->second;
}

// radix-2 FFT in place; inverse flips the sign of the exponent (the caller scales)
void fft(double* re, double* im, int n, bool inverse) {
  const Twiddles& tw = twiddles(n);
  for (int i = 0; i < n; i++) {
    const int j = tw.rev[static_cast<std::size_t>(i)];
    if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
  }
  const double sign = inverse ? 1 : -1;
  for (int size = 2; size <= n; size <<= 1) {
    const int half = size >> 1, step = n / size;
    for (int start = 0; start < n; start += size) {
      for (int k = 0, w = 0; k < half; k++, w += step) {
        const double wr = tw.cos[static_cast<std::size_t>(w)], wi = sign * tw.sin[static_cast<std::size_t>(w)];
        const int a = start + k, b = a + half;
        const double xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - xr; im[b] = im[a] - xi;
        re[a] += xr; im[a] += xi;
      }
    }
  }
}

const std::vector<double>& windowFor(int size) {
  static std::map<int, std::vector<double>> windows;
  auto it = windows.find(size);
  if (it != windows.end()) return it->second;
  std::vector<double> w(static_cast<std::size_t>(size));
  for (int i = 0; i < size; i++) w[static_cast<std::size_t>(i)] = 0.5 - 0.5 * js::cos((2 * kPi * i) / size);
  return windows.emplace(size, std::move(w)).first->second;
}

struct Settings {
  double freq;
  bool freeze;
  double phase;
  double a;
};

class Blur final : public Node {
public:
  explicit Blur(const NodeContext& context) : sampleRate_(context.sampleRate) {
    for (int c = 0; c < 2; c++) {
      inb_[c].assign(kMax, 0.0f);
      acc_[c].assign(kMax * 2, 0.0f);
      smooth_[c].assign(kMax / 2 + 1, 0.0);
      outPh_[c].assign(kMax / 2 + 1, 0.0);
      lastPh_[c].assign(kMax / 2 + 1, 0.0);
      adv_[c].assign(kMax / 2 + 1, 0.0);
    }
    re_.assign(kMax, 0.0); im_.assign(kMax, 0.0); mag_.assign(kMax / 2 + 1, 0.0); ph_.assign(kMax / 2 + 1, 0.0); pre_.assign(kMax / 2 + 2, 0.0);
  }

  void process(const float* const* inputs, float* const* outputs, int count, const float* params, const int* slots) override {
    const int want = sizeOf(params[slots[kSize]]);
    if (want != n_) reset(want);
    const double mix = params[slots[kMix]];
    Settings p;
    p.freq = static_cast<double>(params[slots[kFreq]]) * 24;
    p.freeze = params[slots[kFreeze]] >= 0.5f;
    p.phase = params[slots[kPhase]];
    p.a = params[slots[kTime]] < 1 ? 0 : js::exp(-hop_ / (sampleRate_ * params[slots[kTime]] * 0.001));
    for (int i = 0; i < count; i++) {
      const std::int64_t idx = t_ % n_;
      inb_[0][static_cast<std::size_t>(idx)] = inputs[0][i]; inb_[1][static_cast<std::size_t>(idx)] = inputs[1][i];
      t_++;
      if (t_ >= n_ && t_ % hop_ == 0) { frame(0, p); frame(1, p); }
      const std::int64_t back = t_ % n_;   // the sample that went in one frame ago, to match the delay of the blurred sound
      const std::int64_t at = (t_ - n_ + kMax * 2) & (kMax * 2 - 1);
      for (int c = 0; c < 2; c++) {
        const double dry = inb_[c][static_cast<std::size_t>(back)], wet = t_ >= n_ ? static_cast<double>(acc_[c][static_cast<std::size_t>(at)]) : 0;
        acc_[c][static_cast<std::size_t>(at)] = 0;
        outputs[c][i] = static_cast<float>(dry + (wet - dry) * mix);
      }
    }
  }

private:
  double rnd() {
    seed_ = static_cast<std::uint32_t>(static_cast<std::uint64_t>(seed_) * 1664525u + 1013904223u);
    return seed_ / 4294967296.0;
  }

  void reset(int size) {
    n_ = size; hop_ = size / 4; t_ = 0;
    for (int c = 0; c < 2; c++) {
      std::fill(inb_[c].begin(), inb_[c].end(), 0.0f); std::fill(acc_[c].begin(), acc_[c].end(), 0.0f);
      std::fill(smooth_[c].begin(), smooth_[c].end(), 0.0); primed_[c] = false;
      std::fill(outPh_[c].begin(), outPh_[c].end(), 0.0); std::fill(lastPh_[c].begin(), lastPh_[c].end(), 0.0); std::fill(adv_[c].begin(), adv_[c].end(), 0.0);
    }
  }

  // one frame of one channel: the last n input samples in, the blurred frame added to the output
  void frame(int c, const Settings& p) {
    const std::vector<double>& w = windowFor(n_);
    const int half = n_ >> 1;
    double* re = re_.data();
    double* im = im_.data();
    for (int j = 0; j < n_; j++) { re[j] = inb_[c][static_cast<std::size_t>((t_ - n_ + j) % n_)] * w[static_cast<std::size_t>(j)]; im[j] = 0; }
    fft(re, im, n_, false);
    for (int k = 0; k <= half; k++) { mag_[static_cast<std::size_t>(k)] = jsHypot(re[k], im[k]); ph_[static_cast<std::size_t>(k)] = js::atan2(im[k], re[k]); }
    // blur across frequency: a box whose width grows with the bin, as pitch does
    if (p.freq > 0) {
      pre_[0] = 0;
      for (int k = 0; k <= half; k++) pre_[static_cast<std::size_t>(k + 1)] = pre_[static_cast<std::size_t>(k)] + mag_[static_cast<std::size_t>(k)];
      double* blurred = re;   // (re is free until the inverse)
      for (int k = 0; k <= half; k++) {
        const double r = std::min(static_cast<double>(half), std::floor(p.freq * (1 + k * 0.12)));
        const int lo = static_cast<int>(std::max(0.0, k - r)), hi = static_cast<int>(std::min(static_cast<double>(half), k + r));
        blurred[k] = (pre_[static_cast<std::size_t>(hi + 1)] - pre_[static_cast<std::size_t>(lo)]) / (hi - lo + 1);
      }
      for (int k = 0; k <= half; k++) mag_[static_cast<std::size_t>(k)] = blurred[k];
    }
    // blur across time: every bin follows a running average
    std::vector<double>& s = smooth_[c];
    if (!primed_[c]) {
      for (int k = 0; k <= half; k++) s[static_cast<std::size_t>(k)] = mag_[static_cast<std::size_t>(k)];
      primed_[c] = true;
    } else if (!p.freeze) {
      const double a = p.a;
      for (int k = 0; k <= half; k++) s[static_cast<std::size_t>(k)] = a * s[static_cast<std::size_t>(k)] + (1 - a) * mag_[static_cast<std::size_t>(k)];
    }
    std::vector<double>& o = outPh_[c];
    std::vector<double>& lp = lastPh_[c];
    std::vector<double>& av = adv_[c];
    const double expect = (2 * kPi * hop_) / n_;
    for (int k = 0; k <= half; k++) {
      const std::size_t i = static_cast<std::size_t>(k);
      if (p.freeze) {
        o[i] += av[i];
      } else {   // the phase that really advanced from the last frame: the bin's own advance plus the deviation, wrapped
        double d = ph_[i] - lp[i] - expect * k;
        d -= 2 * kPi * jsRound(d / (2 * kPi));
        av[i] = expect * k + d; o[i] = ph_[i];
      }
      lp[i] = ph_[i];
    }
    for (int k = 0; k <= half; k++) {
      const std::size_t i = static_cast<std::size_t>(k);
      const double m = s[i], angle = o[i] + p.phase * (rnd() * 2 - 1) * kPi;
      re[k] = m * js::cos(angle); im[k] = m * js::sin(angle);
      if (k > 0 && k < half) { re[n_ - k] = re[k]; im[n_ - k] = -im[k]; }
    }
    im[0] = 0; im[half] = 0;
    fft(re, im, n_, true);
    const double scale = 1 / (n_ * 1.5);   // the inverse's 1/n, and the Hann-squared overlap of four frames (1.5)
    std::vector<float>& out = acc_[c];
    for (int j = 0; j < n_; j++) {
      const std::size_t at = static_cast<std::size_t>((t_ - n_ + j + kMax * 2) & (kMax * 2 - 1));
      out[at] = static_cast<float>(static_cast<double>(out[at]) + re[j] * w[static_cast<std::size_t>(j)] * scale);
    }
  }

  double sampleRate_;
  std::vector<float> inb_[2], acc_[2];
  std::vector<double> smooth_[2], outPh_[2], lastPh_[2], adv_[2];
  bool primed_[2] = {false, false};
  std::vector<double> re_, im_, mag_, ph_, pre_;
  int n_ = 2048, hop_ = 512;
  std::int64_t t_ = 0;
  std::uint32_t seed_ = 12345;
};

}  // namespace

void registerBlur(NodeRegistry& registry) {
  NodeType type;
  type.type = "blur";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.isEffect = true;
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.latency = [](const std::map<std::string, double>& settings, double) { return static_cast<double>(sizeOf(settings.at("size")) - 1); };   // one frame
  type.params = {
    {"freq", 0, 1, 0.3},
    {"time", 0, 5000, 400},      // ms: how long the running average takes to settle
    {"freeze", 0, 1, 0, true},
    {"phase", 0, 1, 0},          // 0 the sound's own phases, 1 random
    {"size", 0, 2, 1, true},     // frame length: 1024, 2048, 4096 samples
    {"mix", 0, 1, 1},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Blur>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Spectral synth: reads the strength of a sample's harmonics at `position` and plays that timbre on the note, as a bank of up to
// 64 sine partials. Monophonic, last note wins, with glide. Port of webtracker src/audio/nodes/specsynth.js.
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "core/js_math.h"
#include "nodes/adsr.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kSample = 0, kPosition, kScan, kBlur, kContrast, kPartials, kRoot, kTilt, kEvenodd, kStretch, kWidth, kTune, kGlide, kVel, kLevel, kAttack, kDecay, kSustain, kRelease };
constexpr int kPartialsMax = 64;
constexpr int kN = 4096, kHop = 1024;

void fft(std::vector<double>& re, std::vector<double>& im) {
  const int n = static_cast<int>(re.size());
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double a = -2 * kPi / len, wr = js::cos(a), wi = js::sin(a);
    for (int i = 0; i < n; i += len) {
      double cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) {
        const int x = i + k, y = x + len / 2;
        const double tr = re[y] * cr - im[y] * ci, ti = re[y] * ci + im[y] * cr;
        re[y] = re[x] - tr; im[y] = im[x] - ti; re[x] += tr; im[x] += ti;
        const double nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
      }
    }
  }
}

const std::vector<double>& window() {
  static const std::vector<double> w = [] {
    std::vector<double> v(kN);
    for (int i = 0; i < kN; i++) v[i] = 0.5 - 0.5 * js::cos(2 * kPi * i / kN);
    return v;
  }();
  return w;
}

struct Bank {
  std::shared_ptr<const SampleMessage> msg;
  double rate = 44100, gain = 1;
  std::map<int, std::vector<float>> spectra;   // magnitude spectra by frame, worked out when first asked for
};

int frameCount(std::size_t length) { return std::max(1, static_cast<int>(std::ceil(static_cast<double>(length) / kHop))); }

const std::vector<float>& spectrumAt(Bank& S, int frame) {
  auto found = S.spectra.find(frame);
  if (found != S.spectra.end()) return found->second;
  const std::vector<float>& data = S.msg->data;
  const std::vector<float>& right = S.msg->right;
  std::vector<double> re(kN, 0.0), im(kN, 0.0);
  const int centre = frame * kHop - kN / 2;
  const std::vector<double>& w = window();
  for (int i = 0; i < kN; i++) {
    const int at = centre + i;
    if (at >= 0 && at < static_cast<int>(data.size())) {
      const double d = data[static_cast<std::size_t>(at)];
      re[i] = (!right.empty() ? (d + static_cast<double>(right[static_cast<std::size_t>(at)])) * 0.5 : d) * w[i];
    }
  }
  fft(re, im);
  std::vector<float> mag(kN / 2);
  for (int k = 0; k < kN / 2; k++) mag[k] = static_cast<float>(jsHypot(re[k], im[k]));
  return S.spectra.emplace(frame, std::move(mag)).first->second;
}

double harmonicLevel(const std::vector<float>& mag, double rate, double f0, int m) {
  const double bin = f0 * m * kN / rate;
  const int lo = std::max(1, jsRoundInt(bin) - 1), hi = std::min(kN / 2 - 1, jsRoundInt(bin) + 1);
  if (bin > kN / 2 - 2) return 0;
  double v = 0;
  for (int k = lo; k <= hi; k++) if (mag[k] > v) v = mag[k];
  return v;
}

class SpecSynth final : public Node {
public:
  explicit SpecSynth(const NodeContext& context)
      : sampleRate_(context.sampleRate), pitchOn_(!context.connected.empty() && context.connected[0]), amp_(context.sampleRate),
        env_(context.maxBlock, 0.0), sumO_(context.maxBlock, 0.0), sumE_(context.maxBlock, 0.0) {
    reset();
  }

  void sample(const std::shared_ptr<const SampleMessage>& message) override {
    const std::size_t slot = static_cast<std::size_t>(std::max(0, message->slot));
    if (bank_.size() <= slot) bank_.resize(slot + 1);
    auto S = std::make_unique<Bank>();
    S->msg = message;
    S->rate = message->rate;
    S->gain = message->volume * message->globalVolume;
    bank_[slot] = std::move(S);
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      if (amp_.idle()) { reset(); pos_ = event.note; }
      note_ = event.note; velocity_ = event.vel;
      amp_.on();
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) amp_.off();
    else amp_.cut();
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const int pick = std::max(0, std::min(255, static_cast<int>(std::floor(g(kSample)))));
    Bank* S = pick < static_cast<int>(bank_.size()) ? bank_[static_cast<std::size_t>(pick)].get() : nullptr;
    if (amp_.idle() || !S) {
      std::fill(outL, outL + n, 0.0f);
      std::fill(outR, outR + n, 0.0f);
      return;
    }
    const double sr = sampleRate_;
    amp_.set(g(kAttack), g(kDecay), g(kSustain), g(kRelease));
    const double glideMs = g(kGlide);
    if (glideMs > 0.5) pos_ += (note_ - pos_) * (1 - js::exp(-n / (sr * glideMs * 0.001 / 3)));
    else pos_ = note_;
    const double dt = n / sr, nyq = sr * 0.45;
    const int count = jsRoundInt(g(kPartials));
    const double tilt = g(kTilt), eo = g(kEvenodd), B = js::pow(g(kStretch), 2) * 0.004, width = g(kWidth);
    const double f0 = 440 * js::pow(2, (pos_ + g(kTune) - 69) / 12) * (pitchOn_ ? js::pow(2, static_cast<double>(inputs[0][0])) : 1);
    scanned_ += g(kScan) * dt;
    double at = g(kPosition) + scanned_;
    at -= std::floor(at);
    const int frames = frameCount(S->msg->data.size());
    const double fx = at * (frames - 1);
    const int f1 = static_cast<int>(std::floor(fx));
    const double fr = fx - f1;
    const std::vector<float>& A = spectrumAt(*S, f1);
    const std::vector<float>& Bm = spectrumAt(*S, std::min(frames - 1, f1 + 1));
    const double rootHz = 440 * js::pow(2, (g(kRoot) - 69) / 12);
    double top = 1e-12;
    for (int h = 0; h < count; h++) {
      const double v = harmonicLevel(A, S->rate, rootHz, h + 1) * (1 - fr) + harmonicLevel(Bm, S->rate, rootHz, h + 1) * fr;
      levels_[h] = v;
      if (v > top) top = v;
    }
    const double contrast = g(kContrast), blur = g(kBlur);
    const double follow = fresh_ || blur <= 0 ? 1 : 1 - js::exp(-dt / (blur * blur * 2 + 1e-4));
    fresh_ = false;
    double energy = 0;
    std::fill(target_, target_ + kPartialsMax, 0.0);
    for (int h = 0; h < count; h++) {
      const int m = h + 1;
      const double freq = f0 * m * std::sqrt(1 + B * m * m);
      smooth_[h] += (js::pow(levels_[h] / top, contrast) - smooth_[h]) * follow;
      if (freq >= nyq) continue;
      double a = smooth_[h] * js::pow(10, (tilt * js::log2(m)) / 20);
      a *= m % 2 ? std::min(1.0, 1 - eo) : std::min(1.0, 1 + eo);
      energy += a * a;
      target_[h] = a;
      const double w = (2 * kPi * freq) / sr;
      c_[h] = js::cos(w); sn_[h] = js::sin(w);
    }
    const double level = g(kLevel) * (1 - g(kVel) + g(kVel) * velocity_) * 0.4 * S->gain;
    const double norm = energy > 1e-12 ? level / std::sqrt(energy) : 0;
    double peakA = 0;
    for (int h = 0; h < count; h++) peakA = std::max(peakA, target_[h] * norm);
    if (static_cast<int>(env_.size()) < n) env_.assign(n, 0.0);
    for (int i = 0; i < n; i++) env_[i] = amp_.next();
    if (static_cast<int>(sumO_.size()) < n) { sumO_.assign(n, 0.0); sumE_.assign(n, 0.0); }
    std::fill(sumO_.begin(), sumO_.begin() + n, 0.0);
    std::fill(sumE_.begin(), sumE_.begin() + n, 0.0);
    const double quiet = peakA * 1e-4;
    for (int h = 0; h < count; h++) {
      const double a1 = target_[h] * norm, a0 = aPrev_[h];
      if (a0 < quiet && a1 < quiet) { aPrev_[h] = a1; continue; }
      aPrev_[h] = a1;
      std::vector<double>& sum = (h & 1) ? sumE_ : sumO_;
      double x = re_[h], y = im_[h];
      const double cc = c_[h], ss = sn_[h], step = (a1 - a0) / n;
      double a = a0;
      for (int i = 0; i < n; i++) {
        const double nx = x * cc - y * ss;
        y = x * ss + y * cc; x = nx;
        a += step;
        sum[i] += y * a;
      }
      const double mag = 1 / std::sqrt(x * x + y * y);
      re_[h] = x * mag; im_[h] = y * mag;
    }
    const double angO = ((-width + 1) * kPi) / 4, angE = ((width + 1) * kPi) / 4;
    const double olo = js::cos(angO) * kSqrt2, oro = js::sin(angO) * kSqrt2, ole = js::cos(angE) * kSqrt2, ore = js::sin(angE) * kSqrt2;
    for (int i = 0; i < n; i++) {
      const double e = env_[i], o = sumO_[i] * e, v = sumE_[i] * e;
      outL[i] = static_cast<float>(o * olo + v * ole);
      outR[i] = static_cast<float>(o * oro + v * ore);
    }
  }

private:
  void reset() {
    std::fill(re_, re_ + kPartialsMax, 1.0);
    std::fill(im_, im_ + kPartialsMax, 0.0);
    std::fill(aPrev_, aPrev_ + kPartialsMax, 0.0);
    std::fill(smooth_, smooth_ + kPartialsMax, 0.0);
    scanned_ = 0;
    fresh_ = true;
  }

  double sampleRate_;
  bool pitchOn_;
  Adsr amp_;
  std::vector<std::unique_ptr<Bank>> bank_;
  double re_[kPartialsMax], im_[kPartialsMax], aPrev_[kPartialsMax], smooth_[kPartialsMax], target_[kPartialsMax] = {}, c_[kPartialsMax] = {}, sn_[kPartialsMax] = {}, levels_[kPartialsMax] = {};
  std::vector<double> env_, sumO_, sumE_;
  double velocity_ = 1, pos_ = 60, scanned_ = 0;
  int note_ = 60;
  bool fresh_ = true;
};

}  // namespace

void registerSpecSynth(NodeRegistry& registry) {
  NodeType type;
  type.type = "specsynth";
  type.inputs = {"pitch"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"sample", 0, 255, 0, true}, {"position", 0, 1, 0.2}, {"scan", -1, 1, 0}, {"blur", 0, 1, 0.1}, {"contrast", 0.25, 3, 1}, {"partials", 1, 64, 48, true},
    {"root", 0, 127, 60}, {"tilt", -12, 12, 0}, {"evenodd", -1, 1, 0}, {"stretch", 0, 1, 0}, {"width", 0, 1, 0.4}, {"tune", -48, 48, 0}, {"glide", 0, 2000, 0},
    {"vel", 0, 1, 0.5}, {"level", 0, 1, 0.5}, {"attack", 0.1, 5000, 10}, {"decay", 1, 5000, 400}, {"sustain", 0, 1, 0.8}, {"release", 1, 5000, 300},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<SpecSynth>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

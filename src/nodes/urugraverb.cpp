// urugraVerb: a smooth stereo reverb in the Arguru and Freeverb line. Four damped feedback combs per channel (the right
// one read a little further back), six all-pass diffusers in series, a gentle low-pass and a mid/side width stage; a very
// slow sine moves the read positions a little. out = in * dry + reverb * wet.
// Port of webtracker src/audio/nodes/urugraverb.js.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kPredelay = 0, kSize, kDecay, kDamp, kMod, kDry, kWet, kWidth };

constexpr double kCombA[4] = {29.7, 37.1, 41.3, 43.7}, kCombB[4] = {38, 41, 47, 53};   // delay in ms: A + size * B
constexpr double kExtra[4] = {-23, -29, -37, -43};   // how much further back the right channel reads, in samples
constexpr double kModL[4] = {1.0, 0.8, -0.7, 1.05}, kModR[4] = {-0.6, -0.5, 0.45, -0.55};
constexpr double kApA[6] = {12.7, 9.3, 7.1, 5.9, 4.3, 3.1}, kApB[6] = {9, 8, 7, 6, 5, 4};
constexpr int kAp = 2048;

double clampTo(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

// one tap of a circular buffer at a fractional distance behind the write position, interpolated
double tap(const std::vector<float>& buf, int size, double pos, double back) {
  double r = pos - back;
  r -= std::floor(r / size) * size;
  const int k = static_cast<int>(r);
  const double f = r - k;
  const double a = buf[static_cast<std::size_t>(k)], b = buf[static_cast<std::size_t>(k + 1 == size ? 0 : k + 1)];
  return a + (b - a) * f;
}

class UrugraVerb final : public Node {
public:
  explicit UrugraVerb(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        size_(1 << static_cast<int>(std::ceil(js::log2(std::max(8192.0, context.sampleRate * 0.25))))),
        ms_(context.sampleRate * 0.001),
        glide_(1 - js::exp(-1 / (context.sampleRate * 0.01))) {
    for (int c = 0; c < 4; c++) { combL_[static_cast<std::size_t>(c)].assign(static_cast<std::size_t>(size_), 0.0f); combR_[static_cast<std::size_t>(c)].assign(static_cast<std::size_t>(size_), 0.0f); }
    for (int i = 0; i < 6; i++) { apL_[static_cast<std::size_t>(i)].assign(kAp, 0.0f); apR_[static_cast<std::size_t>(i)].assign(kAp, 0.0f); }
    dsNow_.fill(-1);
    apNow_.fill(-1);
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inL = inputs[0];
    const float* inR = inputs[1];
    float* outL = outputs[0];
    float* outR = outputs[1];
    const auto g_ = [&](int p) -> double { return params[slots[p]]; };
    const double size = g_(kSize), decay = g_(kDecay), dampP = g_(kDamp), mod = g_(kMod), dry = g_(kDry), wet = g_(kWet), width = g_(kWidth);
    const double pdT = clampTo(g_(kPredelay) * ms_, 1, 5000), fb = clampTo(decay, 0.1, 0.93), dampK = clampTo(dampP, 0.05, 0.9);
    const double g = clampTo(0.5 + decay * 0.25, 0.4, 0.75);
    std::array<double, 4> dsT;
    std::array<double, 6> apT;
    for (int i = 0; i < 4; i++) dsT[static_cast<std::size_t>(i)] = clampTo((kCombA[i] + size * kCombB[i]) * ms_, 50, std::min(7000.0, static_cast<double>(size_ - 4000)));
    for (int i = 0; i < 6; i++) apT[static_cast<std::size_t>(i)] = clampTo((kApA[i] + size * kApB[i]) * ms_, 20, 1800);
    if (pdNow_ < 0) { pdNow_ = pdT; dsNow_ = dsT; apNow_ = apT; }
    const double modAmt = mod * 7.5, coeff = js::exp(-2 * kPi * (4200 + (1 - dampK) * 6500) / sampleRate_);
    for (int s = 0; s < n; s++) {
      const double x1 = inL[s], x2 = inR[s];
      phase_ += 0.11 / sampleRate_;
      if (phase_ > 1) phase_ -= 1;
      const double modSig = js::sin(phase_ * 2 * kPi);
      pdNow_ += (pdT - pdNow_) * glide_;
      double sumL = 0, sumR = 0;
      for (int c = 0; c < 4; c++) {
        const std::size_t ci = static_cast<std::size_t>(c);
        dsNow_[ci] += (dsT[ci] - dsNow_[ci]) * glide_;
        std::vector<float>& bl = combL_[ci];
        std::vector<float>& br = combR_[ci];
        const double cl = tap(bl, size_, w_, dsNow_[ci] + pdNow_ - modSig * modAmt * kModL[c]);
        const double cr = tap(br, size_, w_, dsNow_[ci] + pdNow_ - kExtra[c] - modSig * modAmt * kModR[c]);
        damp_[0][ci] = cl * (1 - dampK) + damp_[0][ci] * dampK;
        damp_[1][ci] = cr * (1 - dampK) + damp_[1][ci] * dampK;
        bl[static_cast<std::size_t>(w_)] = static_cast<float>(x1 + damp_[0][ci] * fb);
        br[static_cast<std::size_t>(w_)] = static_cast<float>(x2 + damp_[1][ci] * fb);
        sumL += cl; sumR += cr;
      }
      if (++w_ == size_) w_ = 0;
      double l = sumL * 0.25, r = sumR * 0.25;
      for (int i = 0; i < 6; i++) {
        const std::size_t ii = static_cast<std::size_t>(i);
        apNow_[ii] += (apT[ii] - apNow_[ii]) * glide_;
        const double dly = apNow_[ii] + modSig * mod * 2.8;
        const int p = apPos_[ii];
        const double bL = tap(apL_[ii], kAp, p, dly), tL = l + g * bL;
        apL_[ii][static_cast<std::size_t>(p)] = static_cast<float>(tL);
        l = bL - g * tL;
        const double bR = tap(apR_[ii], kAp, p, dly + 13 + i * 3.5), tR = r + g * bR;
        apR_[ii][static_cast<std::size_t>(p)] = static_cast<float>(tR);
        r = bR - g * tR;
        apPos_[ii] = p + 1 == kAp ? 0 : p + 1;
      }
      lpL_ = l + (lpL_ - l) * coeff;
      lpR_ = r + (lpR_ - r) * coeff;
      if (std::fabs(lpL_) < 1e-20) lpL_ = 0;
      if (std::fabs(lpR_) < 1e-20) lpR_ = 0;
      const double mid = (lpL_ + lpR_) * 0.5, side = (lpL_ - lpR_) * 0.5 * width;
      outL[s] = static_cast<float>(x1 * dry + (mid + side) * wet);
      outR[s] = static_cast<float>(x2 * dry + (mid - side) * wet);
    }
    if (!std::isfinite(lpL_ + lpR_)) {
      lpL_ = lpR_ = 0;
      for (auto& b : combL_) std::fill(b.begin(), b.end(), 0.0f);
      for (auto& b : combR_) std::fill(b.begin(), b.end(), 0.0f);
      for (auto& b : apL_) std::fill(b.begin(), b.end(), 0.0f);
      for (auto& b : apR_) std::fill(b.begin(), b.end(), 0.0f);
      damp_[0].fill(0); damp_[1].fill(0);
    }
  }

private:
  double sampleRate_;
  int size_;
  double ms_, glide_;
  std::array<std::vector<float>, 4> combL_, combR_;
  std::array<double, 4> damp_[2] = {};
  int w_ = 0;
  std::array<std::vector<float>, 6> apL_, apR_;
  std::array<int, 6> apPos_{};
  double lpL_ = 0, lpR_ = 0, phase_ = 0;
  std::array<double, 4> dsNow_{};
  std::array<double, 6> apNow_{};
  double pdNow_ = -1;
};

}  // namespace

void registerUrugraVerb(NodeRegistry& registry) {
  NodeType type;
  type.type = "urugraVerb";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {
    {"predelay", 1, 90, 18},
    {"size", 0, 1, 0.65},
    {"decay", 0, 1, 0.78},
    {"damp", 0, 1, 0.35},
    {"mod", 0, 1, 0.22},
    {"dry", 0, 1, 1},
    {"wet", 0, 1, 0.42},
    {"width", 0, 2, 1},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<UrugraVerb>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

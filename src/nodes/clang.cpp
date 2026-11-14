// Clang: struck metal. A body is hit once and rings through 28 resonances (modes) with their own pitch, loudness and decay;
// `shape` goes from a thin round plate to a large hollow barrel. A thump underneath, a sizzle of noise, a beater whose
// hardness is `strike`. Monophonic (the voices node makes it polyphonic). Port of webtracker src/audio/nodes/clang.js.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kShape = 0, kDamping, kHighdamp, kThump, kStrike, kPosition, kStretch, kWash, kWidth, kTune, kVel, kLevel, kRelease };

constexpr int kModes = 28;

double golden(double k) { return std::fmod(k * 0.6180339887, 1); }

struct Ratios {
  std::array<double, kModes> plate, barrel;
  Ratios() {
    const double p[12] = {1, 1.73, 2.33, 3.9, 4.15, 5.95, 6.72, 7.33, 9.9, 10.4, 12.5, 13.2};
    const double b[12] = {1, 1.6, 2.12, 2.4, 3.0, 3.55, 4.18, 4.8, 5.6, 6.3, 7.2, 8.1};
    for (int k = 0; k < 12; k++) { plate[static_cast<std::size_t>(k)] = p[k]; barrel[static_cast<std::size_t>(k)] = b[k]; }
    for (int k = 12; k < kModes; k++) {
      plate[static_cast<std::size_t>(k)] = plate[static_cast<std::size_t>(k) - 1] + 1.1 + 0.8 * golden(k);
      barrel[static_cast<std::size_t>(k)] = barrel[static_cast<std::size_t>(k) - 1] + 0.85 + 0.3 * golden(k);
    }
  }
};
const Ratios& ratios() { static const Ratios r; return r; }

class Clang final : public Node {
public:
  explicit Clang(const NodeContext& context) : sampleRate_(context.sampleRate), pitchOn_(!context.connected.empty() && context.connected[0]) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { note_ = event.note; velocity_ = event.vel; pending_ = true; held_ = true; active_ = true; }
    else if (event.type == NoteType::Off || event.type == NoteType::AllOff) held_ = false;
    else if (event.type == NoteType::Cut) { active_ = false; held_ = false; y1_.fill(0); y2_.fill(0); }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (!active_) { std::fill(outL, outL + n, 0.0f); std::fill(outR, outR + n, 0.0f); return; }
    const auto g = [&](int p) -> double { return params[slots[p]]; };
    const double sr = sampleRate_;
    const double base = 440 * js::pow(2, (note_ + g(kTune) - 69) / 12 + (pitchOn_ ? inputs[0][0] : 0.0f));
    aim(g, base);
    if (pending_) { pending_ = false; age_ = 0; amp_ = 1; thumpPhase_ = 0; gain_ = 1 - g(kVel) + g(kVel) * velocity_; }
    const double strike = g(kStrike), thump = g(kThump), shape = g(kShape), wash = g(kWash);
    const double fc = 600 * js::pow(30, strike), beat = 1 - js::exp(-2 * kPi * fc / sr);   // the beater's brightness
    const double click = 0.0004 * sr * (1 + 8 * (1 - strike));                              // how long the beater is in contact
    const double washLen = (0.3 + 1.5 * (1 - g(kDamping))) * sr;
    const double rel = js::pow(10, -3 / (g(kRelease) * 0.001 * sr));
    const double thumpTau = (0.03 + 0.2 * shape) * sr, thumpF0 = base * (0.5 - 0.15 * shape);
    const double drive = 1 + 2.5 * thump, out = g(kLevel) * gain_ * 1.1 / js::tanh(drive * 0.5);
    for (int i = 0; i < n; i++) {
      if (!held_) amp_ *= rel;
      const double hit = age_ < click ? (age_ == 0 ? 1 : 0.5 * rand()) * (1 - age_ / click) : 0;
      lp_ += (hit - lp_) * beat;
      const double sizzle = wash * js::exp(-age_ / washLen) * rand() * 0.15 * amp_;
      double L = 0, R = 0;
      for (int k = 0; k < used_; k++) {
        const std::size_t m = static_cast<std::size_t>(k);
        const double y = gk_[m] * lp_ + washK_[m] * sizzle + a1_[m] * y1_[m] - a2_[m] * y2_[m];
        y2_[m] = y1_[m]; y1_[m] = y;
        L += y * panL_[m]; R += y * panR_[m];
      }
      double body = 0;
      if (thump > 0) {
        const double e = js::exp(-age_ / thumpTau);
        thumpPhase_ += thumpF0 * (1 + 1.5 * js::exp(-age_ / (0.012 * sr))) / sr;
        thumpPhase_ -= std::floor(thumpPhase_);
        body = js::sin(2 * kPi * thumpPhase_) * e * thump * 1.2 * std::min(1.0, age_ / 8);
      }
      age_++;
      outL[i] = static_cast<float>(js::tanh((L * amp_ + body) * drive * 0.5) * out);
      outR[i] = static_cast<float>(js::tanh((R * amp_ + body) * drive * 0.5) * out);
    }
    // finished when what rings has fallen far below hearing (and the thump is over)
    if (age_ > thumpTau * 5 && age_ > sr * 0.05) {
      double e = 0;
      for (int k = 0; k < used_; k++) e += y1_[static_cast<std::size_t>(k)] * y1_[static_cast<std::size_t>(k)] + y2_[static_cast<std::size_t>(k)] * y2_[static_cast<std::size_t>(k)];
      if (e * amp_ * amp_ < 1e-14 || amp_ < 1e-5) { active_ = false; y1_.fill(0); y2_.fill(0); }
    }
  }

private:
  // The resonators for the settings and the note: coefficients, loudness, and how much sizzle each one gets.
  template <class G>
  void aim(const G& g, double base) {
    const double shape = g(kShape), stretch = g(kStretch), strike = g(kStrike), position = g(kPosition), width = g(kWidth), high = g(kHighdamp);
    const double t60 = 6 * js::pow(0.08 / 6, g(kDamping));   // seconds for the lowest resonance to fall 60 dB
    const double slope = (0.35 + 0.65 * shape) * (0.5 + 1.5 * high);
    const double tilt = 0.25 + 1.45 * shape;
    const double reach = 1 + 9 * strike;
    const Ratios& table = ratios();
    used_ = 0;
    for (int k = 0; k < kModes; k++) {
      const std::size_t m = static_cast<std::size_t>(k);
      const double ratio = js::pow(js::exp(js::log(table.plate[m]) + (js::log(table.barrel[m]) - js::log(table.plate[m])) * shape), stretch);
      const double f = base * ratio;
      if (f > sampleRate_ * 0.45) { gk_[m] = 0; continue; }
      used_ = k + 1;
      const double t = std::max(0.004, t60 * js::pow(ratio, -slope));
      const double r = js::pow(10, -3 / (t * sampleRate_)), th = 2 * kPi * f / sampleRate_;
      a1_[m] = 2 * r * js::cos(th); a2_[m] = r * r;
      const double where = 0.3 + 0.7 * std::fabs(js::cos(kPi * position * (k + 1) * 0.37));
      const double w = js::pow(k + 1, -tilt) * where * std::min(1.0, reach / (k + 1)) * (k < 3 ? 1 : 0.8 + 0.2 * strike);
      gk_[m] = w * js::sin(th) * 1.6;
      washK_[m] = k >= 6 ? gk_[m] * 0.5 : 0;
      const double p = (golden(k + 1) * 2 - 1) * width;
      panL_[m] = std::sqrt(0.5 * (1 - p)); panR_[m] = std::sqrt(0.5 * (1 + p));
    }
  }

  double rand() {
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    return static_cast<double>(seed_) / 4294967296.0 * 2 - 1;
  }

  double sampleRate_;
  bool pitchOn_;
  int note_ = 60;
  double velocity_ = 1;
  bool held_ = false, active_ = false, pending_ = false;
  double age_ = 0, amp_ = 1, gain_ = 1, lp_ = 0, thumpPhase_ = 0;
  std::array<double, kModes> y1_{}, y2_{}, a1_{}, a2_{}, gk_{}, washK_{}, panL_{}, panR_{};
  int used_ = 0;
  std::uint32_t seed_ = 0x1234abcdu;
};

}  // namespace

void registerClang(NodeRegistry& registry) {
  NodeType type;
  type.type = "clang";
  type.inputs = {"pitch"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"shape", 0, 1, 0.25},
    {"damping", 0, 1, 0.45},
    {"highdamp", 0, 1, 0.5},
    {"thump", 0, 1, 0.3},
    {"strike", 0, 1, 0.6},
    {"position", 0, 1, 0.4},
    {"stretch", 0.6, 1.6, 1},
    {"wash", 0, 1, 0.3},
    {"width", 0, 1, 0.4},
    {"tune", -48, 48, 0},
    {"vel", 0, 1, 0.5},
    {"level", 0, 1, 0.5},
    {"release", 5, 3000, 400},   // ms
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Clang>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

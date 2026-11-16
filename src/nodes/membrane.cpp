// Membrane: a drum skin. A circular membrane that is struck rings in a fixed family of patterns (modes); mode (m, n) sits at the
// n-th zero of the Bessel function J_m over the first zero of J_0, found here by the same search as the web app does (not a table),
// and how loud each mode is comes from the Bessel function at the radius that is hit. Monophonic; a new note hits again.
// Port of webtracker src/audio/nodes/membrane.js (without the picture of the node).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/membrane_view.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kPosition = 0, kStrike, kDamping, kHighdamp, kStiffness, kGlide, kShell, kWidth, kTune, kVel, kLevel, kRelease, kQuality };

constexpr int kSteps = 256, kMaxModes = 40, kEcoModes = 16;
constexpr double kStrikeAngle = 0.4;

struct Tables { double sin[kSteps], cos[kSteps]; };
const Tables& tables() {
  static const Tables t = [] {
    Tables r{};
    for (int k = 0; k < kSteps; k++) { r.sin[k] = js::sin(2 * kPi * k / kSteps); r.cos[k] = js::cos(2 * kPi * k / kSteps); }
    return r;
  }();
  return t;
}

// J_m(x): (1 / 2 pi) times the integral of cos(m t - x sin t) over a full turn, summed over 256 points.
double bessel(int m, double x) {
  const Tables& t = tables();
  double sum = 0;
  for (int k = 0; k < kSteps; k++) {
    const double tt = 2 * kPi * k / kSteps;
    sum += js::cos(m * tt - x * t.sin[k]);
  }
  return sum / kSteps;
}

std::vector<double> zerosOfBessel(int m, double limit) {
  std::vector<double> zeros;
  double x0 = m == 0 ? 0.5 : m + 0.5, f0 = bessel(m, x0);
  for (double x1 = x0 + 0.05; x1 <= limit; x1 += 0.05) {
    const double f1 = bessel(m, x1);
    if (f0 == 0 || (f0 < 0) != (f1 < 0)) {
      double lo = x0, hi = x1, flo = f0;
      for (int i = 0; i < 40; i++) {
        const double mid = (lo + hi) / 2, fm = bessel(m, mid);
        if ((fm < 0) == (flo < 0)) { lo = mid; flo = fm; } else hi = mid;
      }
      zeros.push_back((lo + hi) / 2);
    }
    x0 = x1; f0 = f1;
  }
  return zeros;
}

struct Mode { int m, n; double j, ratio, norm; };

struct Modes { std::vector<Mode> all; Mode main; };
const Modes& modes() {
  static const Modes result = [] {
    Modes r;
    const double j01 = zerosOfBessel(0, 3)[0];
    std::vector<Mode> all;
    for (int m = 0; m <= 14; m++) {
      const std::vector<double> zs = zerosOfBessel(m, j01 * 12);
      for (std::size_t i = 0; i < zs.size(); i++) {
        const double j = zs[i];
        const double slope = std::abs(bessel(m + 1, j));
        all.push_back({m, static_cast<int>(i) + 1, j, j / j01, 1 / std::max(slope, 1e-3) * (m == 0 ? 1 : kSqrt2)});
      }
    }
    std::stable_sort(all.begin(), all.end(), [](const Mode& a, const Mode& b) { return a.ratio < b.ratio; });
    all.resize(std::min<std::size_t>(all.size(), kMaxModes));
    r.all = all;
    for (const Mode& k : all) if (k.m == 1 && k.n == 1) { r.main = k; break; }
    return r;
  }();
  return result;
}

struct Described { int m; double ratio, t60, weight; };

std::vector<Described> describeModes(double position, double stiffness, double damping, double highdamp, double quality) {
  const std::vector<Mode>& all = modes().all;
  const std::size_t limit = quality >= 0.5 ? kEcoModes : all.size();
  const double t60 = 3 * js::pow(0.05 / 3, damping);
  const double slope = 0.3 + 1.2 * highdamp;
  std::vector<Described> out;
  for (std::size_t i = 0; i < limit; i++) {
    const Mode& md = all[i];
    Described d;
    d.m = md.m;
    d.ratio = md.ratio * std::sqrt(1 + stiffness * 0.02 * md.ratio * md.ratio);
    d.t60 = std::max(0.004, t60 * js::pow(md.ratio, -slope) * (md.m == 0 && md.n == 1 ? 1 : 0.85));
    d.weight = bessel(md.m, md.j * position * 0.9) * md.norm * js::pow(md.ratio, -0.35);
    out.push_back(d);
  }
  return out;
}

class Membrane final : public Node {
public:
  explicit Membrane(const NodeContext& context)
      : sampleRate_(context.sampleRate), pitchOn_(!context.connected.empty() && context.connected[0]) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { note_ = event.note; velocity_ = event.vel; pending_ = true; held_ = true; active_ = true; }
    else if (event.type == NoteType::Off || event.type == NoteType::AllOff) held_ = false;
    else { active_ = false; held_ = false; y1_.fill(0); y2_.fill(0); }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (!active_) {
      std::fill(outL, outL + n, 0.0f);
      std::fill(outR, outR + n, 0.0f);
      return;
    }
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double sr = sampleRate_;
    const double mainMode = modes().main.ratio;
    const double main = 440 * js::pow(2, (note_ + g(kTune) - 69) / 12 + (pitchOn_ ? static_cast<double>(inputs[0][0]) : 0.0));
    if (pending_) { pending_ = false; age_ = 0; amp_ = 1; shellPhase_ = 0; gain_ = 1 - g(kVel) + g(kVel) * velocity_; }
    const double strike = g(kStrike), glide = g(kGlide), shell = g(kShell);
    const double lift = 1 + glide * 0.5 * (0.4 + 0.6 * gain_) * js::exp(-age_ / (0.045 * sr));
    aim(g, main, lift, 1 + 1.4 * g(kPosition));
    const double fc = 300 * js::pow(60, strike), beat = 1 - js::exp(-2 * kPi * fc / sr);
    const double click = 0.0004 * sr;
    const double rel = js::pow(10, -3 / (g(kRelease) * 0.001 * sr));
    const double shellTau = 0.09 * sr, shellF0 = main / mainMode * 0.5;
    const double out = g(kLevel) * gain_ * 1.1;
    for (int i = 0; i < n; i++) {
      if (!held_) amp_ *= rel;
      double hit = age_ == 0 ? 1 : 0;
      if (age_ < click) { const double r = rand(); hit += strike * 0.5 * (r - prevRand_) * (1 - age_ / click); prevRand_ = r; }
      lp_ += (hit - lp_) * beat;
      lp2_ += (lp_ - lp2_) * beat;
      double L = 0, R = 0;
      for (int k = 0; k < used_; k++) {
        const double y = lp2_ + a1_[k] * y1_[k] - a2_[k] * y2_[k];
        y2_[k] = y1_[k]; y1_[k] = y;
        L += y * gL_[k]; R += y * gR_[k];
      }
      double body = 0;
      if (shell > 0) {
        shellPhase_ += shellF0 * (1 + 0.8 * js::exp(-age_ / (0.02 * sr))) / sr;
        shellPhase_ -= std::floor(shellPhase_);
        body = js::sin(2 * kPi * shellPhase_) * js::exp(-age_ / shellTau) * shell * 1.1 * std::min(1.0, age_ / 8);
      }
      age_++;
      outL[i] = static_cast<float>(js::tanh((L + body) * amp_) * out);
      outR[i] = static_cast<float>(js::tanh((R + body) * amp_) * out);
    }
    if (age_ > shellTau * 5 && age_ > sr * 0.05) {
      double e = 0;
      for (int k = 0; k < used_; k++) e += y1_[k] * y1_[k] + y2_[k] * y2_[k];
      if (e * amp_ * amp_ < 1e-14 || amp_ < 1e-5) { active_ = false; y1_.fill(0); y2_.fill(0); }
    }
  }

private:
  template <typename G>
  void aim(const G& g, double main, double lift, double loudness) {
    const std::vector<Described> ms = describeModes(g(kPosition), g(kStiffness), g(kDamping), g(kHighdamp), g(kQuality));
    const double spread = g(kWidth) * 1.2;
    const double base = main * lift / modes().main.ratio;
    used_ = 0;
    for (std::size_t kk = 0; kk < ms.size(); kk++) {
      const int k = static_cast<int>(kk);
      const Described& md = ms[kk];
      const double f = base * md.ratio;
      if (f > sampleRate_ * 0.45) { gL_[k] = gR_[k] = 0; continue; }
      used_ = k + 1;
      const double r = js::pow(10, -3 / (md.t60 * sampleRate_)), th = 2 * kPi * f / sampleRate_;
      a1_[k] = 2 * r * js::cos(th); a2_[k] = r * r;
      const double w = md.weight * js::sin(th) * 0.12 * loudness;
      gL_[k] = w * js::cos(md.m * (kStrikeAngle - spread));
      gR_[k] = w * js::cos(md.m * (kStrikeAngle + spread));
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
  double age_ = 0, amp_ = 1, gain_ = 1;
  double lp_ = 0, lp2_ = 0, shellPhase_ = 0, prevRand_ = 0;
  std::array<double, kMaxModes> y1_{}, y2_{}, a1_{}, a2_{}, gL_{}, gR_{};
  int used_ = 0;
  std::uint32_t seed_ = 0x2468ace1u;
};

}  // namespace

double membraneBessel(int m, double x) { return bessel(m, x); }

std::vector<MembraneMode> membraneModes(double position, double stiffness, double damping, double highdamp, double quality) {
  const std::vector<Mode>& all = modes().all;
  const std::size_t limit = quality >= 0.5 ? kEcoModes : all.size();
  const double t60 = 3 * js::pow(0.05 / 3, damping), slope = 0.3 + 1.2 * highdamp;
  std::vector<MembraneMode> out;
  for (std::size_t i = 0; i < limit; i++) {
    const Mode& md = all[i];
    MembraneMode d;
    d.m = md.m; d.n = md.n; d.j = md.j;
    d.ratio = md.ratio * std::sqrt(1 + stiffness * 0.02 * md.ratio * md.ratio);
    d.t60 = std::max(0.004, t60 * js::pow(md.ratio, -slope) * (md.m == 0 && md.n == 1 ? 1 : 0.85));
    d.weight = bessel(md.m, md.j * position * 0.9) * md.norm * js::pow(md.ratio, -0.35);
    out.push_back(d);
  }
  return out;
}

void registerMembrane(NodeRegistry& registry) {
  NodeType type;
  type.type = "membrane";
  type.inputs = {"pitch"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"position", 0, 1, 0.55}, {"strike", 0, 1, 0.55}, {"damping", 0, 1, 0.5}, {"highdamp", 0, 1, 0.5}, {"stiffness", 0, 1, 0.15},
    {"glide", 0, 1, 0.35}, {"shell", 0, 1, 0.25}, {"width", 0, 1, 0.4}, {"tune", -48, 48, 0}, {"vel", 0, 1, 0.5}, {"level", 0, 1, 0.5},
    {"release", 5, 3000, 300}, {"quality", 0, 1, 0, true},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Membrane>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

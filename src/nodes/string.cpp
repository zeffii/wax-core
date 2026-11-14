// Plucked string (Karplus-Strong). A note strikes a delay line the length of one period with a burst of noise; the line
// feeds back on itself through a gentle lowpass, so the sound rings at the note's pitch and the high harmonics die
// first. `decay` is how long the note takes to fall by 60 dB, `damp` how fast the highs die away, `bright` how bright
// the strike is, `pos` where along the string it is plucked. Note off damps the string over `release` ms.
// Monophonic; a new note strikes the string again. Port of webtracker src/audio/nodes/string.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kDecay = 0, kDamp, kBright, kPos, kTune, kVel, kLevel, kRelease };

constexpr int kSize = 1 << 16, kMask = kSize - 1;

class StringNode final : public Node {
public:
  explicit StringNode(const NodeContext& context)
      : sampleRate_(context.sampleRate), pitchOn_(!context.connected.empty() && context.connected[0]), buf_(kSize, 0.0) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { note_ = event.note; velocity_ = event.vel; pending_ = true; held_ = true; active_ = true; }
    else if (event.type == NoteType::Off || event.type == NoteType::AllOff) held_ = false;
    else if (event.type == NoteType::Cut) { active_ = false; held_ = false; std::fill(buf_.begin(), buf_.end(), 0.0); amp_ = 0; }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (!active_) { std::fill(outL, outL + n, 0.0f); std::fill(outR, outR + n, 0.0f); return; }
    auto g = [&](int i) -> double { return params[slots[i]]; };
    const double T = period(g(kTune), pitchOn_ ? inputs[0][0] : 0.0);
    const double s = g(kDamp) * 0.5;           // weight of the older sample in the loop filter
    const double D = std::max(1.5, T - s);     // the read delay; with the filter's own delay the loop adds up to one period
    if (pending_) {
      pending_ = false;
      const double vm = 1 - g(kVel) + g(kVel) * velocity_;
      const double hard = std::min(1.0, g(kBright) * (1 - g(kVel) * 0.5 * (1 - velocity_)));
      strike(T, hard, g(kPos));
      gain_ = vm;
    }
    // loop gain from the decay time: the line is passed through once per period
    const double roundTrip = T / sampleRate_;
    double loop = js::pow(10, -3 * roundTrip / g(kDecay));
    if (!held_) loop *= js::pow(10, -3 * roundTrip / (g(kRelease) * 0.001));
    loop = std::min(0.99995, loop);
    const double out = g(kLevel) * gain_ * 0.7;
    const int di = static_cast<int>(std::floor(D));
    const double df = D - di;
    for (int i = 0; i < n; i++) {
      // two taps D and D+1 samples back, each linearly interpolated, then the lowpass (1-s, s)
      const double a0 = buf_[static_cast<std::size_t>((w_ - di) & kMask)], a1 = buf_[static_cast<std::size_t>((w_ - di - 1) & kMask)], a2 = buf_[static_cast<std::size_t>((w_ - di - 2) & kMask)];
      const double t0 = a0 + (a1 - a0) * df, t1 = a1 + (a2 - a1) * df;
      const double y = ((1 - s) * t0 + s * t1) * loop;
      buf_[static_cast<std::size_t>(w_ & kMask)] = y;
      w_ = (w_ + 1) & kMask;
      outL[i] = outR[i] = static_cast<float>(y * out);
      const double ay = std::abs(y);
      amp_ = ay > amp_ ? ay : amp_ * 0.999;
    }
    if (amp_ < 1e-5) { active_ = false; std::fill(buf_.begin(), buf_.end(), 0.0); amp_ = 0; }
  }

private:
  double rand() {
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    return seed_ / 4294967296.0 * 2 - 1;
  }

  double period(double tune, double octaves) const {
    return std::max(2.5, std::min(kSize / 2.0 - 4, sampleRate_ / (440 * js::pow(2, (note_ + tune - 69) / 12 + octaves))));
  }

  // Fill the last `len` samples of the line with the strike.
  void strike(double len, double bright, double pos) {
    const int n = std::max(2, jsRoundInt(len));
    const double c = std::max(0.03, bright);   // one pole lowpass on the noise: smaller is duller
    std::vector<double> e(static_cast<std::size_t>(n));
    double y = 0;
    for (int i = 0; i < n; i++) { y += c * (rand() - y); e[static_cast<std::size_t>(i)] = y; }
    const int d = std::max(1, jsRoundInt(pos * n));
    std::vector<double> comb(static_cast<std::size_t>(n));
    for (int i = 0; i < n; i++) comb[static_cast<std::size_t>(i)] = e[static_cast<std::size_t>(i)] - e[static_cast<std::size_t>((i - d + n) % n)];   // plucking at a point cancels the harmonics with a node there
    double mean = 0;
    for (int i = 0; i < n; i++) mean += comb[static_cast<std::size_t>(i)];
    mean /= n;
    double peak = 1e-9;
    for (int i = 0; i < n; i++) { comb[static_cast<std::size_t>(i)] -= mean; peak = std::max(peak, std::abs(comb[static_cast<std::size_t>(i)])); }
    std::fill(buf_.begin(), buf_.end(), 0.0);
    for (int i = 0; i < n; i++) buf_[static_cast<std::size_t>((w_ - n + i) & kMask)] = comb[static_cast<std::size_t>(i)] / peak;
  }

  double sampleRate_;
  bool pitchOn_;
  std::vector<double> buf_;
  int w_ = 0, note_ = 60;
  double velocity_ = 1, amp_ = 0, gain_ = 1;
  bool active_ = false, held_ = false, pending_ = false;
  std::uint32_t seed_ = 0x9e3779b9u;
};

}  // namespace

void registerString(NodeRegistry& registry) {
  NodeType type;
  type.type = "string";
  type.inputs = {"pitch"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"decay", 0.05, 30, 3},        // seconds to fall 60 dB
    {"damp", 0, 1, 0.3},
    {"bright", 0, 1, 0.6},
    {"pos", 0.02, 0.5, 0.15},
    {"tune", -48, 48, 0},
    {"vel", 0, 1, 0.5},
    {"level", 0, 1, 0.5},
    {"release", 5, 3000, 150},     // ms
  };
  type.create = [](const NodeContext& context) { return std::make_unique<StringNode>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

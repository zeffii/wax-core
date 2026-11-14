// Oscillator. Silent until the first note. Monophonic: one voice per instrument, and the last note sets the pitch.
// A note restarts the phase at the phase parameter (reset = 1), so every hit starts the same way, which is what a bass
// drum needs; the sync input does the same on a rising edge (above 0.5). Waves are naive, so the saw, square and the
// shapes with corners alias at high pitch.
// Waves: 0 sine, 1 saw, 2 square, 3 triangle, 4 pulse (its width is `width`), 5 half sine (the top half of a sine, the
// bottom flat), 6 rectified sine (all the humps the same way up), 7 staircase (a saw in eight steps), 8 drawn (a single
// cycle drawn by hand, read with linear interpolation).
// Port of webtracker src/audio/nodes/osc.js. (A wave drawn while the song plays arrives as a node message: not ported yet.)
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/waves.h"

namespace wax {
namespace {

enum Param { kFreq = 0, kTune, kWave, kTrack, kReset, kPhase, kLevel, kWidth };
enum Input { kFm = 0, kSync, kPm };

constexpr double kPi = 3.141592653589793;
constexpr double kTwoPi = 2 * kPi;
constexpr double kLn2 = 0.6931471805599453;
constexpr double kHalfSineMean = 1 / kPi;   // the average of the half sine (before doubling), taken off so the wave sits on zero

class Osc final : public Node {
public:
  explicit Osc(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        fmOn_(context.connected[kFm]),
        syncOn_(context.connected[kSync]),
        pmOn_(context.connected[kPm]),
        table_(normalizeWave(context.data && context.data->contains("wave") ? &(*context.data)["wave"] : nullptr)) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { note_ = event.note; started_ = true; pendingReset_ = true; }
    else if (event.type == NoteType::Cut) started_ = false;
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* out = outputs[0];
    if (!started_) { std::fill(out, out + n, 0.0f); return; }
    const float* fm = inputs[kFm];
    const float* sync = inputs[kSync];
    const float* pm = inputs[kPm];
    const double tune = params[slots[kTune]], restart = params[slots[kPhase]];
    const double base = (params[slots[kTrack]] >= 0.5f
      ? 440 * std::pow(2.0, (note_ + tune - 69) / 12)
      : params[slots[kFreq]] * std::pow(2.0, tune / 12)) / sampleRate_;
    const int wave = jsRoundInt(params[slots[kWave]]);
    const double level = params[slots[kLevel]], width = params[slots[kWidth]];
    if (pendingReset_ && params[slots[kReset]] >= 0.5f) phase_ = restart;
    pendingReset_ = false;
    for (int i = 0; i < n; i++) {
      if (syncOn_) {
        const double s = sync[i];
        if (s > 0.5 && prevSync_ <= 0.5) phase_ = restart;
        prevSync_ = s;
      }
      double v, ph = phase_;
      if (pmOn_) { ph += pm[i] / kTwoPi; ph -= std::floor(ph); }
      if (wave == 0) v = std::sin(kTwoPi * ph);
      else if (wave == 1) v = 2 * ph - 1;
      else if (wave == 2) v = ph < 0.5 ? 1 : -1;
      else if (wave == 3) v = ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4;
      else if (wave == 4) v = ph < width ? 1 : -1;
      else if (wave == 5) v = (ph < 0.5 ? std::sin(kTwoPi * ph) : 0) * 2 - 2 * kHalfSineMean;
      else if (wave == 6) v = std::abs(std::sin(kTwoPi * ph)) * 2 - 1;
      else if (wave == 7) v = std::floor(ph * 8) / 3.5 - 1;
      else {
        const double x = ph * kWavePoints;
        const int j = static_cast<int>(x);
        const double f = x - j;
        const double a = table_[static_cast<std::size_t>(j & (kWavePoints - 1))], b = table_[static_cast<std::size_t>((j + 1) & (kWavePoints - 1))];
        v = a + (b - a) * f;
      }
      out[i] = static_cast<float>(v * level);
      phase_ += fmOn_ ? base * std::exp(fm[i] * kLn2) : base;
      if (phase_ >= 1) phase_ -= std::floor(phase_);
    }
  }

private:
  double sampleRate_;
  bool fmOn_, syncOn_, pmOn_;
  WaveTable table_;
  double phase_ = 0;
  int note_ = 60;
  bool started_ = false, pendingReset_ = false;
  double prevSync_ = 0;
};

}  // namespace

void registerOsc(NodeRegistry& registry) {
  NodeType type;
  type.type = "osc";
  type.inputs = {"fm", "sync", "pm"};   // fm is in octaves, per sample; pm bends the phase, in radians
  type.outputs = {"out"};
  type.kinds = {{"fm", PortKind::Control}, {"sync", PortKind::Control}, {"pm", PortKind::Control}};
  type.params = {
    {"freq", 1, 20000, 440},      // Hz, used when track is 0
    {"tune", -48, 48, 0},         // semitones
    {"wave", 0, 8, 0, true},      // 0 sine, 1 saw, 2 square, 3 triangle, 4 pulse, 5 half sine, 6 rectified sine, 7 staircase, 8 drawn
    {"track", 0, 1, 1, true},     // 1: pitch follows the note
    {"reset", 0, 1, 1, true},     // 1: a note restarts the phase
    {"phase", 0, 1, 0},           // where a restart begins, in cycles
    {"level", 0, 1, 1},
    {"width", 0.02, 0.98, 0.5},   // how much of the cycle the pulse is high
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Osc>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Small control-rate nodes: random and times.
// Ports of webtracker src/audio/nodes/random.js and times.js.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

// ---- random ----
// A control source that wanders. mode 0 picks a new value `rate` times a second; mode 1 picks one on each note; mode 2
// picks one each time the `trig` input fires (a rise through 0.5, or a change between two values above 0), so any gate,
// envelope, lfo or note trigger can drive it. `smooth` is how far it slides to each new value: 0 jumps (sample and
// hold), 1 takes the whole time to arrive. Values are between -1 and 1 times `amount` (bipolar 1), or between 0 and
// `amount` (bipolar 0). `low` and `high` set the range the picks fall in (0 to 1), `lean` is the value inside it they
// favour, and `focus` is how strongly: 0 is even, 1 crowds most picks close to `lean` while still reaching both ends.

enum RandomParam { kMode = 0, kRate, kSmooth, kAmount, kBipolar, kLow, kHigh, kLean, kFocus };

// An even pick u (0 to 1) bent so the values bunch up around c (0 to 1) without leaving the ends out.
double lean(double u, double c, double focus) {
  const double g = 1 + focus * 4;
  return u < 0.5 ? c * (1 - std::pow(1 - 2 * u, g)) : c + (1 - c) * std::pow(2 * u - 1, g);
}

class Random final : public Node {
public:
  explicit Random(const NodeContext& context) : sampleRate_(context.sampleRate), trigOn_(!context.connected.empty()) {
    // each node gets its own sequence, the same every time the song is rendered
    const std::uint32_t made = static_cast<std::uint32_t>(context.nextInstance ? context.nextInstance("random") : 1);
    seed_ = 0x9e3779b9u * made;
    if (seed_ == 0) seed_ = 1;
  }

  void note(const NoteEvent& event) override { if (event.type == NoteType::On) fresh_ = true; }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* out = outputs[0];
    const int mode = jsRoundInt(params[slots[kMode]]);
    const double rate = params[slots[kRate]], smooth = params[slots[kSmooth]];
    const double amount = params[slots[kAmount]];
    const bool bipolar = params[slots[kBipolar]] >= 0.5f;
    const double lowParam = params[slots[kLow]], highParam = params[slots[kHigh]];
    const double low = std::min(lowParam, highParam), high = std::max(lowParam, highParam);
    const double focus = params[slots[kFocus]], c = high > low ? clamp((params[slots[kLean]] - low) / (high - low), 0, 1) : 0.5;
    const double period = sampleRate_ / rate;
    const double time = std::max(1.0, smooth * (mode == 0 ? period : sampleRate_ * 0.25) * 0.5);   // how long the slide takes, about
    const float* trig = trigOn_ ? inputs[0] : nullptr;
    const double coef = smooth <= 0 ? 1 : 1 - std::exp(-1 / time);
    for (int i = 0; i < n; i++) {
      bool fired = false;
      if (mode == 2) {
        const double t = trig ? trig[i] : 0;
        fired = (t > 0.5 && prev_ <= 0.5) || (t > 0.001 && prev_ > 0.001 && std::abs(t - prev_) > 1e-4);
        prev_ = t;
      }
      const bool pick = mode == 0 ? --left_ <= 0 : mode == 1 ? fresh_ : fired;
      if (pick) { target_ = low + (high - low) * lean(rand(), c, focus); left_ = period; fresh_ = false; }
      value_ += (target_ - value_) * coef;
      out[i] = static_cast<float>((bipolar ? value_ * 2 - 1 : value_) * amount);
    }
  }

private:
  // 0..1
  double rand() {
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return seed_ / 4294967296.0;
  }

  double sampleRate_;
  bool trigOn_;
  std::uint32_t seed_ = 1;
  double target_ = 0, value_ = 0, left_ = 0, prev_ = 0;
  bool fresh_ = false;
};

// ---- times ----
// Control rate multiply. Out = `in` x `by`. Wire a second control into `mod` to multiply the two; with nothing there
// `by` is the number typed in. A negative number flips the signal.

class Times final : public Node {
public:
  explicit Times(const NodeContext& context) : modOn_(context.connected.size() > 1 && context.connected[1]) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* out = outputs[0];
    const float* in = inputs[0];
    const float* mod = inputs[1];
    const double by = params[slots[0]];
    if (modOn_) for (int i = 0; i < n; i++) out[i] = static_cast<float>(static_cast<double>(in[i]) * mod[i] * by);
    else for (int i = 0; i < n; i++) out[i] = static_cast<float>(in[i] * by);
  }

private:
  bool modOn_;
};

}  // namespace

void registerControlNodes(NodeRegistry& registry) {
  NodeType random;
  random.type = "random";
  random.inputs = {"trig"};
  random.outputs = {"out"};
  random.kinds = {{"trig", PortKind::Control}, {"out", PortKind::Control}};
  random.params = {
    {"mode", 0, 2, 0, true},        // 0: free running, 1: a new value on each note, 2: on each trigger
    {"rate", 0.05, 40, 1},          // Hz
    {"smooth", 0, 1, 0.7},
    {"amount", 0, 10, 1},
    {"bipolar", 0, 1, 1, true},
    {"low", 0, 1, 0},
    {"high", 0, 1, 1},
    {"lean", 0, 1, 0.5},
    {"focus", 0, 1, 0},
  };
  random.create = [](const NodeContext& context) { return std::make_unique<Random>(context); };
  registry.add(std::move(random));

  NodeType times;
  times.type = "times";
  times.inputs = {"in", "mod"};
  times.outputs = {"out"};
  times.kinds = {{"in", PortKind::Control}, {"mod", PortKind::Control}, {"out", PortKind::Control}};
  times.params = {{"by", -1000, 1000, 1}};
  times.create = [](const NodeContext& context) { return std::make_unique<Times>(context); };
  registry.add(std::move(times));
}

}  // namespace wax

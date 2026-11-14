// Ctrl: a control object that glides a value from one point to another, timed in subticks (a sixteenth of a tick), for a
// parameter port. `to` glides from where the value is now, `from` jumps; `curve` is linear, log, ease out, ease in or a random
// stagger (reproducible from `seed`). One copy runs for every voice. Port of webtracker src/audio/nodes/ctrl.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kTo = 0, kFrom, kTime, kCurve, kSeed, kSteps, kWobble };
constexpr int kMaxSteps = 32;

// Shape of a glide at t (0 to 1), for the curves that are plain functions of time.
double shapeOf(int curve, double t) {
  if (t <= 0) return 0;
  if (t >= 1) return 1;
  switch (curve) {
    case 1: return js::log10(1 + 9 * t);
    case 2: return js::sin(t * kPi / 2);
    case 3: return 1 - js::cos(t * kPi / 2);
    default: return t;
  }
}

struct Plan { std::vector<double> times, levels; };

// The random stagger for a seed: the moments (rising, the last at 1) at which the value steps, and where it stands after each.
Plan stagger(double seed, double steps, double wobble) {
  std::uint32_t s = (static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(seed) + 1)) * 0x9e3779b1u) ^ 0x85ebca6bu;
  auto rand = [&s]() {
    s += 0x6d2b79f5u;
    std::uint32_t t = s;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return static_cast<double>(t ^ (t >> 14)) / 4294967296.0;
  };
  const int count = std::max(2, std::min(kMaxSteps, jsRoundInt(steps)));
  Plan plan;
  plan.times.resize(count);
  for (int i = 0; i < count; i++) { const double r = rand(); plan.times[i] = 0.04 + r * 0.96; }
  std::sort(plan.times.begin(), plan.times.end());
  const double top = plan.times[count - 1];
  for (int i = 0; i < count; i++) plan.times[i] /= top;
  std::vector<double> weights(count);
  double total = 0;
  for (int i = 0; i < count; i++) { const double r = rand(); weights[i] = 0.25 + r * 0.75; }
  for (int i = 0; i < count; i++) total += weights[i];
  plan.levels.resize(count);
  double acc = 0;
  for (int i = 0; i < count; i++) {
    acc += weights[i];
    if (i == count - 1) { plan.levels[i] = 1; continue; }
    const double r = rand();
    plan.levels[i] = std::min(1.5, std::max(-0.5, acc / total + (r - 0.5) * wobble));
  }
  return plan;
}

class Ctrl final : public Node {
public:
  explicit Ctrl(const NodeContext& context) : clock_(context.clock), value_(context.init ? *context.init : 0.5) {}

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* out = outputs[0];
    const double to = params[slots[kTo]], from = params[slots[kFrom]];
    const std::uint32_t toStamp = clock_->stamps[static_cast<std::size_t>(slots[kTo])], fromStamp = clock_->stamps[static_cast<std::size_t>(slots[kFrom])];
    if (!ready_) { ready_ = true; seenTo_ = toStamp; seenFrom_ = fromStamp; seenToValue_ = to; seenFromValue_ = from; }
    if (fromStamp != seenFrom_ || from != seenFromValue_) {
      seenFrom_ = fromStamp; seenFromValue_ = from;
      value_ = from; gliding_ = false;
    }
    if (toStamp != seenTo_ || to != seenToValue_) {
      seenTo_ = toStamp; seenToValue_ = to;
      const double subticks = std::max(0.0, jsRound(params[slots[kTime]]));
      len_ = subticks * clock_->tickSamples / 16;
      start_ = value_; target_ = to; pos_ = 0; curve_ = jsRoundInt(params[slots[kCurve]]);
      hasPlan_ = curve_ == 4;
      if (hasPlan_) plan_ = stagger(params[slots[kSeed]], params[slots[kSteps]], params[slots[kWobble]]);
      if (len_ < 1) { value_ = target_; gliding_ = false; } else gliding_ = true;
    }
    for (int i = 0; i < n; i++) {
      if (gliding_) {
        pos_ += 1;
        const double t = pos_ / len_;
        if (t >= 1) { value_ = target_; gliding_ = false; }
        else if (hasPlan_) {
          int k = -1;
          while (k + 1 < static_cast<int>(plan_.times.size()) && plan_.times[k + 1] <= t) k++;
          value_ = start_ + (target_ - start_) * (k < 0 ? 0 : plan_.levels[k]);
        } else value_ = start_ + (target_ - start_) * shapeOf(curve_, t);
      }
      out[i] = static_cast<float>(value_);
    }
  }

private:
  const Clock* clock_;
  double value_;
  bool ready_ = false, gliding_ = false, hasPlan_ = false;
  std::uint32_t seenTo_ = 0, seenFrom_ = 0;
  double seenToValue_ = 0, seenFromValue_ = 0;
  double pos_ = 0, len_ = 0, start_ = 0, target_ = 0;
  int curve_ = 0;
  Plan plan_;
};

}  // namespace

void registerCtrl(NodeRegistry& registry) {
  NodeType type;
  type.type = "ctrl";
  type.inputs = {};
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Param}};
  type.shared = true;
  type.params = {{"to", 0, 1, 0.5}, {"from", 0, 1, 0.5}, {"time", 0, 255, 16, true}, {"curve", 0, 4, 0, true},
                 {"seed", 0, 255, 0, true}, {"steps", 2, kMaxSteps, 8, true}, {"wobble", 0, 1, 0}};
  type.create = [](const NodeContext& context) { return std::make_unique<Ctrl>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

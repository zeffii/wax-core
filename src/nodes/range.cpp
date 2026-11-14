// Range: scales a 0..1 signal onto real values you type in and hands the port it feeds the 0..1 it expects. With nothing on the
// input, `at` stands in. Port of webtracker src/audio/nodes/range.js.
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kLo = 0, kHi, kAt };

class Range final : public Node {
public:
  explicit Range(const NodeContext& context) : wired_(!context.connected.empty() && context.connected[0]), target_(context.target) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* out = outputs[0];
    const double lo = params[slots[kLo]], hi = params[slots[kHi]];
    if (!wired_) {
      const double x = params[slots[kAt]];
      if (x != lastX_ || lo != lastLo_ || hi != lastHi_) { lastX_ = x; lastLo_ = lo; lastHi_ = hi; last_ = map(x, lo, hi); }
      std::fill(out, out + n, static_cast<float>(last_));
      return;
    }
    for (int i = 0; i < n; i++) {
      const double x = std::min(1.0, std::max(0.0, static_cast<double>(inputs[0][i])));
      if (x != lastX_ || lo != lastLo_ || hi != lastHi_) { lastX_ = x; lastLo_ = lo; lastHi_ = hi; last_ = map(x, lo, hi); }
      out[i] = static_cast<float>(last_);
    }
  }

private:
  double map(double x, double lo, double hi) const {
    const bool log = target_ && target_->log && lo > 0 && hi > 0;
    const double v0 = log ? lo * js::pow(hi / lo, x) : lo + x * (hi - lo);
    if (!target_) return v0;
    const double v = target_->isInt ? jsRound(v0) : v0;
    double r;
    if (target_->log) r = v > 0 ? js::log(v / target_->min) / js::log(target_->max / target_->min) : 0;
    else r = (v - target_->min) / (target_->max - target_->min);
    return std::isfinite(r) ? std::min(1.0, std::max(0.0, r)) : 0;
  }

  bool wired_;
  std::optional<RangeTarget> target_;
  double lastX_ = std::numeric_limits<double>::quiet_NaN(), lastLo_ = lastX_, lastHi_ = lastX_, last_ = 0;
};

}  // namespace

void registerRange(NodeRegistry& registry) {
  NodeType type;
  type.type = "range";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Control}, {"out", PortKind::Control}};
  type.params = {{"lo", -100000, 100000, 0}, {"hi", -100000, 100000, 1}, {"at", 0, 1, 0.5}};
  type.create = [](const NodeContext& context) { return std::make_unique<Range>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

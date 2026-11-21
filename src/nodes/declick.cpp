// Click remover, stereo: a note-start fade, a dc blocker that is fast just after a note, and a slew limit. Port of webtracker
// src/audio/nodes/declick.js; the fade follows the notes only where the node sits in a voice's own chain, before the voices node.
#include <algorithm>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kFade = 0, kDc, kSettle, kSlew };
constexpr double kSettleBoost = 16;   // how much higher the dc corner is while a note is settling

class Declick final : public Node {
public:
  explicit Declick(const NodeContext& context) : sampleRate_(context.sampleRate) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) since_ = 0;
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const double fadeSamples = params[slots[kFade]] * 0.001 * sampleRate_;
    const double dc = params[slots[kDc]];
    const double settleSamples = params[slots[kSettle]] * 0.001 * sampleRate_;
    const double slew = params[slots[kSlew]];
    const double steady = dc > 0 ? pole(dc) : 0, fast = dc > 0 ? pole(dc * kSettleBoost) : 0;
    for (int i = 0; i < n; i++) {
      const double t = since_ + i;
      double l = inputs[0][i], r = inputs[1][i];
      if (t < fadeSamples) {
        const double u = t / fadeSamples, g = u * u * (3 - 2 * u);
        l *= g; r *= g;
      }
      if (dc > 0) {
        const double R = t < settleSamples ? fast : steady;
        const double ol = l - xl_ + R * yl_, orr = r - xr_ + R * yr_;
        xl_ = l; xr_ = r; yl_ = ol; yr_ = orr;
        l = ol; r = orr;
      }
      if (slew < 1) {
        l = sl_ + std::max(-slew, std::min(slew, l - sl_));
        r = sr_ + std::max(-slew, std::min(slew, r - sr_));
        sl_ = l; sr_ = r;
      }
      outputs[0][i] = static_cast<float>(l);
      outputs[1][i] = static_cast<float>(r);
    }
    since_ = std::min(since_ + n, 1e9);
  }

private:
  double pole(double hz) const { return js::exp(-2 * kPi * std::min(hz, sampleRate_ / 8) / sampleRate_); }

  double sampleRate_;
  double since_ = 1e9;   // samples since the last note started
  double xl_ = 0, yl_ = 0, xr_ = 0, yr_ = 0;
  double sl_ = 0, sr_ = 0;
};

}  // namespace

void registerDeclick(NodeRegistry& registry) {
  NodeType type;
  type.type = "declick";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {{"fade", 0, 50, 3}, {"dc", 0, 100, 10}, {"settle", 0, 200, 20}, {"slew", 0.001, 1, 1}};
  type.create = [](const NodeContext& context) { return std::make_unique<Declick>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

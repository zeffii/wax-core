// Envelope effect: shapes the level of whatever passes through it with an ADSR you trigger by hand (the `trig` parameter: above 0
// starts it, 0 releases; a different non-zero value while on restarts the attack). Before the first trigger and once released the
// signal sits at `range` dB. Also the note trigger that feeds it. Port of webtracker src/audio/nodes/envfx.js and notetrig.js.
#include <algorithm>
#include <cmath>
#include <set>

#include "core/js_math.h"
#include "nodes/adsr.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kTrig = 0, kAttack, kDecay, kSustain, kRelease, kRange };
constexpr double kLin = 2.302585092994046 / 20;   // Math.LN10 / 20

class EnvFx final : public Node {
public:
  explicit EnvFx(const NodeContext& context) : env_(context.sampleRate) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const int trig = jsRoundInt(params[slots[kTrig]]);
    if (trig != prev_) {
      if (trig > 0) env_.on(); else env_.off();
      prev_ = trig;
    }
    env_.set(params[slots[kAttack]], params[slots[kDecay]], params[slots[kSustain]], params[slots[kRelease]]);
    const double floor = js::exp(static_cast<double>(params[slots[kRange]]) * kLin);
    for (int i = 0; i < n; i++) {
      const double g = floor + (1 - floor) * env_.next();
      outputs[0][i] = static_cast<float>(static_cast<double>(inputs[0][i]) * g);
      outputs[1][i] = static_cast<float>(static_cast<double>(inputs[1][i]) * g);
    }
  }

private:
  Adsr env_;
  int prev_ = 0;
};

// Note trigger: turns the notes played on its instrument into signals for parameter ports (trig for an envfx, gate, vel).
class NoteTrig final : public Node {
public:
  explicit NoteTrig(const NodeContext&) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      const bool overlapping = !held_.empty();
      held_.insert(event.note);
      velocity_ = event.vel;
      if (!(legato_ && overlapping)) k_ = k_ == 1 ? 2 : 1;
    } else if (event.type == NoteType::Off) {
      held_.erase(event.note);
      if (held_.empty()) k_ = 0;
    } else {   // all off or cut
      held_.clear();
      k_ = 0;
    }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    legato_ = params[slots[0]] >= 1;
    std::fill(outputs[0], outputs[0] + n, static_cast<float>(k_ / 15.0));
    std::fill(outputs[1], outputs[1] + n, held_.empty() ? 0.0f : 1.0f);
    std::fill(outputs[2], outputs[2] + n, static_cast<float>(std::max(0.0, std::min(1.0, velocity_))));
  }

private:
  std::set<int> held_;
  int k_ = 0;
  double velocity_ = 0;
  bool legato_ = false;
};

}  // namespace

void registerEnvFx(NodeRegistry& registry) {
  {
    NodeType type;
    type.type = "envfx";
    type.inputs = {"in"};
    type.outputs = {"out"};
    type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
    type.isEffect = true;
    type.params = {{"trig", 0, 15, 0, true}, {"attack", 0.1, 5000, 5}, {"decay", 1, 5000, 300}, {"sustain", 0, 1, 0.7}, {"release", 1, 5000, 300}, {"range", -80, 0, -80}};
    type.create = [](const NodeContext& context) { return std::make_unique<EnvFx>(context); };
    registry.add(std::move(type));
  }
  {
    NodeType type;
    type.type = "notetrig";
    type.inputs = {};
    type.outputs = {"trig", "gate", "vel"};
    type.kinds = {{"trig", PortKind::Param}, {"gate", PortKind::Param}, {"vel", PortKind::Param}};
    type.shared = true;
    type.params = {{"legato", 0, 1, 0, true}};
    type.create = [](const NodeContext& context) { return std::make_unique<NoteTrig>(context); };
    registry.add(std::move(type));
  }
}

}  // namespace wax

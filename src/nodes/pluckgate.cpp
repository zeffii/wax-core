// Pluck gate: loudness and brightness under one control. A strike envelope lights a vactrol (a lamp and a photocell:
// quick to open, slow to let go), and the vactrol opens the gain and a low-pass filter together, so a sound dies with
// its top end first, like a struck object. mode 0 opens both, 1 the gain alone, 2 the filter alone.
// Without a cv input the gate strikes on each note (the strike envelope, per instrument, like the env node). With one,
// that signal (0..1) lights the lamp instead, so any envelope, LFO or gate pattern can play it.
// `open` is how quickly the lamp opens and `close` how slowly it lets go (ms). `track` moves the open cutoff with the note.
// Port of webtracker src/audio/nodes/pluckgate.js.
#include "core/js_math.h"
#include "nodes/adsr.h"
#include "nodes/all_nodes.h"
#include "nodes/westcore.h"

namespace wax {
namespace {

enum Param { kMode = 0, kBright, kRes, kTrack, kOpen, kClose, kAtk, kDec, kSus, kRel, kVel, kLevel };

class PluckGateNode final : public Node {
public:
  explicit PluckGateNode(const NodeContext& context)
      : cvOn_(context.connected.size() > 1 && context.connected[1]), env_(context.sampleRate), gates_{PluckGate(context.sampleRate), PluckGate(context.sampleRate)} {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { note_ = event.note; velocity_ = event.vel; env_.on(); }
    else if (event.type == NoteType::Off || event.type == NoteType::AllOff) env_.off();
    else { env_.cut(); gates_[0].reset(); gates_[1].reset(); }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inLeft = inputs[0];
    const float* inRight = inputs[1];
    const float* cv = inputs[2];
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    const auto g = [&](Param p) { return static_cast<double>(params[slots[p]]); };
    env_.set(g(kAtk), g(kDec), g(kSus), g(kRel));
    for (PluckGate& gate : gates_) gate.set(jsRoundInt(g(kMode)), g(kBright), g(kRes), g(kOpen), g(kClose), (note_ - 60) * g(kTrack));
    const double strike = 1 - g(kVel) + g(kVel) * velocity_, level = g(kLevel);
    for (int i = 0; i < n; i++) {
      const double target = cvOn_ ? clamp(cv[i], 0, 1) : env_.next() * strike;
      outLeft[i] = static_cast<float>(gates_[0].run(inLeft[i], target) * level);
      outRight[i] = static_cast<float>(gates_[1].run(inRight[i], target) * level);
    }
  }

private:
  bool cvOn_;
  Adsr env_;
  PluckGate gates_[2];
  int note_ = 60;
  double velocity_ = 1;
};

}  // namespace

void registerPluckGate(NodeRegistry& registry) {
  NodeType type;
  type.type = "pluckgate";
  type.inputs = {"in", "cv"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"cv", PortKind::Control}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {
    {"mode", 0, 2, 0, true},
    {"bright", 0, 1, 0.8},
    {"res", 0, 0.9, 0.1},
    {"track", 0, 1, 0.5},
    {"open", 0.2, 200, 2},
    {"close", 10, 5000, 300},
    {"atk", 0.1, 5000, 1},
    {"dec", 1, 5000, 350},
    {"sus", 0, 1, 0},
    {"rel", 1, 5000, 200},
    {"vel", 0, 1, 0.6},
    {"level", 0, 2, 1},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<PluckGateNode>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

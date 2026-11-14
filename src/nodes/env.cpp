// ADSR envelope, one per instrument (monophonic). A note on restarts the attack from the current level; note off,
// all-off and the end of the pattern release it; a cut zeroes it. Decay and release are exponential and the times are
// to fall by 60 dB. The output is level * amount, so amount sets the size of the swing (for example the number of
// octaves when it drives an oscillator's fm input). vel is how much the note's velocity scales the output.
// Port of webtracker src/audio/nodes/env.js.
#include <algorithm>
#include <cmath>

#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param { kAttack = 0, kDecay, kSustain, kRelease, kAmount, kVel };
enum class Stage { Idle, Attacking, Decaying, Releasing };

const double kLn60Db = std::log(0.001);

class Env final : public Node {
public:
  explicit Env(const NodeContext& context) : sampleRate_(context.sampleRate) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { stage_ = Stage::Attacking; velocity_ = event.vel; }
    else if (event.type == NoteType::Off || event.type == NoteType::AllOff) { if (stage_ == Stage::Attacking || stage_ == Stage::Decaying) stage_ = Stage::Releasing; }
    else if (event.type == NoteType::Cut) { stage_ = Stage::Idle; level_ = 0; }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* out = outputs[0];
    const double attackStep = 1 / (std::max(0.1, static_cast<double>(params[slots[kAttack]])) * 0.001 * sampleRate_);
    const double decayCoef = std::exp(kLn60Db / (std::max(1.0, static_cast<double>(params[slots[kDecay]])) * 0.001 * sampleRate_));
    const double releaseCoef = std::exp(kLn60Db / (std::max(1.0, static_cast<double>(params[slots[kRelease]])) * 0.001 * sampleRate_));
    const double sustain = params[slots[kSustain]];
    const double vel = params[slots[kVel]];
    const double scale = params[slots[kAmount]] * (1 - vel + vel * velocity_);
    for (int i = 0; i < n; i++) {
      if (stage_ == Stage::Attacking) {
        level_ += attackStep;
        if (level_ >= 1) { level_ = 1; stage_ = Stage::Decaying; }
      } else if (stage_ == Stage::Decaying) {
        level_ = sustain + (level_ - sustain) * decayCoef;
        if (level_ - sustain < 1e-6) level_ = sustain;   // settles exactly, no denormals
      } else if (stage_ == Stage::Releasing) {
        level_ *= releaseCoef;
        if (level_ < 1e-6) { level_ = 0; stage_ = Stage::Idle; }
      }
      out[i] = static_cast<float>(level_ * scale);
    }
  }

private:
  double sampleRate_;
  Stage stage_ = Stage::Idle;
  double level_ = 0, velocity_ = 1;
};

}  // namespace

void registerEnv(NodeRegistry& registry) {
  NodeType type;
  type.type = "env";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Control}};
  type.params = {
    {"attack", 0.1, 5000, 1},     // ms
    {"decay", 1, 5000, 200},      // ms
    {"sustain", 0, 1, 0},
    {"release", 1, 5000, 100},    // ms
    {"amount", 0, 10, 1},
    {"vel", 0, 1, 0},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Env>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

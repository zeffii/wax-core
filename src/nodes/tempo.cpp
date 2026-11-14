// The master lane's parameters. It has no ports and makes no sound: the sequencer reads bpm, swing and ticks per beat
// from it every tick (so a pattern can change the tempo), and applies volume to the whole mix, then the limiter.
// Port of webtracker src/audio/nodes/tempo.js.
#include "nodes/all_nodes.h"

namespace wax {
namespace {

class Tempo final : public Node {
public:
  void process(const float* const*, float* const*, int, const float*, const int*) override {}
};

}  // namespace

void registerTempo(NodeRegistry& registry) {
  NodeType type;
  type.type = "tempo";
  type.params = {
    {"bpm", 0, 999, 120, true},       // beats per minute (never below 20 when played)
    {"swing", 0, 100, 50, true},      // percent: 50 is straight, 75 the most
    {"tpb", 0, 48, 4, true},          // ticks per beat (at least 1)
    {"volume", 0, 1, 1},              // the whole mix
    {"limit", 0, 1, 0, true},         // the master limiter after the volume: 1 on, 0 off (off to start with)
    {"ceiling", -12, 0, -0.3},        // dB: nothing leaves above this
    {"boost", 0, 18, 0},              // dB into the limiter: louder, with the peaks held under the ceiling
    {"release", 10, 1000, 120},       // ms for the limiter to let go after a peak
    {"carve", 0, 1, 0, true},         // the mixer's carve stage: 1 on, 0 off (adds a window of delay while on)
    {"cvamt", 0, 1, 0.5},             // how strongly quieter lanes give way where another lane is louder
    {"cvdepth", 0, 15, 6},            // dB: the most any lane is dipped in a band
    {"cvfocus", 0, 1, 0.6},           // band width: 0 is an octave, 1 is a sixth of one
  };
  type.create = [](const NodeContext&) { return std::make_unique<Tempo>(); };
  registry.add(std::move(type));
}

}  // namespace wax

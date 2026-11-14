// Sidechain input: the finished sound of another instrument (the `source`, chosen in the node's panel), as a stereo
// signal inside this instrument's graph. Feed it to an envelope follower and the follower's level to a parameter port
// to duck this instrument under the other (a pad under a kick); or just mix it in. It hears the other instrument as it
// sounds after its own graph, in the same block, so there is no delay. A meta machine has no sound of its own, so
// listening to one is listening to the machines it plays, added together; `parts` narrows that to some of them.
// Port of webtracker src/audio/nodes/sidechain.js.
#include <algorithm>

#include "nodes/all_nodes.h"

namespace wax {
namespace {

class Sidechain final : public Node {
public:
  explicit Sidechain(const NodeContext& context) : tap_(context.tap) {}

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    const std::optional<StereoView> from = tap_ ? tap_() : std::nullopt;
    if (!from) {
      std::fill(outLeft, outLeft + n, 0.0f);
      std::fill(outRight, outRight + n, 0.0f);
      return;
    }
    const double gain = params[slots[0]];
    for (int i = 0; i < n; i++) {
      outLeft[i] = static_cast<float>(from->left[i] * gain);
      outRight[i] = static_cast<float>(from->right[i] * gain);
    }
  }

private:
  std::function<std::optional<StereoView>()> tap_;
};

}  // namespace

void registerSidechain(NodeRegistry& registry) {
  NodeType type;
  type.type = "sidechain";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.shared = true;   // one copy for all voices
  type.params = {{"level", 0, 2, 1}};
  type.create = [](const NodeContext& context) { return std::make_unique<Sidechain>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

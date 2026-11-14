// Band splitter: four stereo bands (low, lmid, hmid, high) from three Linkwitz-Riley crossovers in series, phase-aligned.
// Port of webtracker src/audio/nodes/split.js.
#include <vector>

#include "nodes/all_nodes.h"
#include "nodes/splitcore.h"

namespace wax {
namespace {

enum Param { kF1 = 0, kF2, kF3 };

class Split final : public Node {
public:
  explicit Split(const NodeContext& context) : sampleRate_(context.sampleRate) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const double f1 = params[slots[kF1]], f2 = params[slots[kF2]], f3 = params[slots[kF3]];
    double g[3];
    cross_.update(f1, f2, f3, n, sampleRate_, g);
    for (int c = 0; c < 2; c++) {
      splitcore::Channel& channel = ch_[c];
      channel.set(g[0], g[1], g[2]);
      float* const bands[4] = {outputs[c], outputs[2 + c], outputs[4 + c], outputs[6 + c]};
      for (int i = 0; i < n; i++) channel.run(inputs[c][i], bands, i);
      channel.flush();
    }
  }

private:
  double sampleRate_;
  splitcore::Channel ch_[2];
  splitcore::Crossovers cross_;
};

}  // namespace

void registerSplit(NodeRegistry& registry) {
  NodeType type;
  type.type = "split";
  type.inputs = {"in"};
  type.outputs = {"low", "lmid", "hmid", "high"};
  type.kinds = {{"in", PortKind::Stereo}, {"low", PortKind::Stereo}, {"lmid", PortKind::Stereo}, {"hmid", PortKind::Stereo}, {"high", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {{"f1", 30, 2000, 120}, {"f2", 100, 8000, 800}, {"f3", 500, 18000, 5000}};
  type.create = [](const NodeContext& context) { return std::make_unique<Split>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

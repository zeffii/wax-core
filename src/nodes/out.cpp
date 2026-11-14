// Output stage: gain and equal-power balance on a stereo input (a mono wire feeds both channels, so a mono source is
// panned as before). The master node of a graph.
// Port of webtracker src/audio/nodes/out.js.
#include <cmath>

#include "nodes/all_nodes.h"

namespace wax {
namespace {

// (two more parameters, `master` and `lead`, are read by the runtime, not here)
enum Param { kGain = 0, kPan = 1 };

constexpr double kPi = 3.141592653589793;
constexpr double kSqrt2 = 1.4142135623730951;

class Out final : public Node {
public:
  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inLeft = inputs[0];
    const float* inRight = inputs[1];
    float* left = outputs[0];
    float* right = outputs[1];
    const double g = params[slots[kGain]] * kSqrt2;
    const double a = (params[slots[kPan]] + 1.0) * kPi / 4;
    const double gl = g * std::cos(a), gr = g * std::sin(a);
    if (gl0_ < 0) { gl0_ = gl; gr0_ = gr; }
    const double dl = (gl - gl0_) / n, dr = (gr - gr0_) / n;   // ramp across the block
    for (int i = 0; i < n; i++) {
      gl0_ += dl;
      gr0_ += dr;
      left[i] = static_cast<float>(inLeft[i] * gl0_);
      right[i] = static_cast<float>(inRight[i] * gr0_);
    }
    gl0_ = gl;
    gr0_ = gr;
  }

private:
  double gl0_ = -1, gr0_ = 0;   // the gains the last block ended on (-1: no block yet)
};

}  // namespace

void registerOut(NodeRegistry& registry) {
  NodeType type;
  type.type = "out";
  type.master = true;
  type.inputs = {"in"};
  type.outputs = {"left", "right"};
  type.kinds = {{"in", PortKind::Stereo}};
  type.params = {
    {"gain", 0, 1.5, 0.8},
    {"pan", -1, 1, 0},
    {"master", 0, 1, 1, true},   // 1: the sound goes to the mix; 0: it does not, but a sidechain node in another instrument still hears it
    {"lead", 0, 1, 0, true},     // 1: the carve stage (in the mixer) never dips this lane; the others give way to it
  };
  type.create = [](const NodeContext&) { return std::make_unique<Out>(); };
  registry.add(std::move(type));
}

}  // namespace wax

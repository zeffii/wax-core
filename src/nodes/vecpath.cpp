// Vector path: a control source that travels a drawn path across a diamond after each note starts, and puts out where it is as x
// and y. Port of webtracker src/audio/nodes/vecpath.js.
#include <algorithm>

#include "nodes/all_nodes.h"
#include "nodes/vectorcore.h"

namespace wax {
namespace {

enum Param { kPathms = 0, kPmode, kAmount, kX, kY };

class VecPath final : public Node {
public:
  explicit VecPath(const NodeContext& context) : sampleRate_(context.sampleRate) {
    const Json* raw = context.data && context.data->is_object() && context.data->contains("path") ? &(*context.data)["path"] : nullptr;
    path_ = vec::cleanPath(raw);
  }

  void note(const NoteEvent& event) override { if (event.type == NoteType::On) age_ = 0; }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outX = outputs[0];
    float* outY = outputs[1];
    age_ += n;
    vec::pathAt(path_, jsRoundInt(params[slots[kPmode]]), age_ / (static_cast<double>(params[slots[kPathms]]) * 0.001 * sampleRate_), position_);
    const double amount = params[slots[kAmount]];
    const double x = static_cast<double>(params[slots[kX]]) + amount * position_[0], y = static_cast<double>(params[slots[kY]]) + amount * position_[1];
    if (fresh_) { lastX_ = x; lastY_ = y; fresh_ = false; }
    for (int i = 0; i < n; i++) {
      const double t = static_cast<double>(i + 1) / n;
      outX[i] = static_cast<float>(lastX_ + (x - lastX_) * t);
      outY[i] = static_cast<float>(lastY_ + (y - lastY_) * t);
    }
    lastX_ = x; lastY_ = y;
  }

private:
  double sampleRate_;
  vec::Path path_;
  double age_ = 0, lastX_ = 0, lastY_ = 0, position_[2] = {0, 0};
  bool fresh_ = true;
};

}  // namespace

void registerVecPath(NodeRegistry& registry) {
  NodeType type;
  type.type = "vecpath";
  type.inputs = {};
  type.outputs = {"x", "y"};
  type.kinds = {{"x", PortKind::Control}, {"y", PortKind::Control}};
  type.params = {{"pathms", 10, 60000, 4000}, {"pmode", 0, 2, 1, true}, {"amount", 0, 1, 1}, {"x", -1, 1, 0}, {"y", -1, 1, 0}};
  type.create = [](const NodeContext& context) { return std::make_unique<VecPath>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

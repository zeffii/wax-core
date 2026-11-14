// The shader machine's parameters: no ports, no sound. `shader` picks which of the project's shaders is shown and k1 to
// k16 are the values it reads every frame. The window that draws the picture is not ported yet (a dummy for now); the
// parameters exist so patterns can write them and songs that use the shader lane load. Port of webtracker src/audio/nodes/glsl.js.
#include "nodes/all_nodes.h"

namespace wax {
namespace {

class Glsl final : public Node {
public:
  void process(const float* const*, float* const*, int, const float*, const int*) override {}
};

}  // namespace

void registerGlsl(NodeRegistry& registry) {
  NodeType type;
  type.type = "glsl";
  type.params.push_back({"shader", 0, 15, 0, true});   // which of the project's shaders the picture shows (0 to F)
  for (int i = 1; i <= 16; i++) type.params.push_back({"k" + std::to_string(i), 0, 1, 0.5});
  type.create = [](const NodeContext&) { return std::make_unique<Glsl>(); };
  registry.add(std::move(type));
}

}  // namespace wax

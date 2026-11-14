#include "edit/sync.h"

#include <stdexcept>

#include "core/node_registry.h"

namespace wax {

std::vector<std::string> syncLanes(Document& document, Engine& engine) {
  engine.collectRetired();
  std::vector<std::string> problems;
  for (const std::string& lane : document.takeChangedLanes()) {
    try {
      if (!engine.sendLane(document.project(), lane)) document.markLaneChanged(lane);   // the queue is full: later
    } catch (const std::runtime_error& e) {
      problems.push_back(e.what());
      document.markLaneChanged(lane);
    }
  }
  return problems;
}

void syncParams(Document& document, Engine& engine) {
  const Project& project = document.project();
  const NodeRegistry& types = builtinNodes();
  for (const std::string& address : document.takeChangedParams()) {
    for (const Instrument& instrument : project.instruments) {
      for (const GraphNode& node : instrument.graph.nodes) {
        const NodeType* type = types.find(node.type);
        if (!type) continue;
        for (const ParamSpec& spec : type->params) {
          if (instrument.id + "." + node.id + "." + spec.name != address) continue;
          engine.setParam(address, node.param(spec.name, spec.def));
        }
      }
    }
  }
}

}  // namespace wax

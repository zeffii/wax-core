#include "edit/sync.h"
#include "seq/loop.h"

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

void syncData(Document& document, Engine& engine) {
  engine.collectRetired();
  const Project& project = document.project();
  for (const std::string& address : document.takeChangedData()) {
    const std::size_t cut = address.find('\x1f');
    const std::string instrumentId = address.substr(0, cut), nodeId = address.substr(cut + 1);
    for (const Instrument& instrument : project.instruments) {
      if (instrument.id != instrumentId) continue;
      for (const GraphNode& node : instrument.graph.nodes) {
        if (node.id != nodeId) continue;
        if (!engine.sendData(instrumentId, nodeId, node.data)) document.markDataChanged(address);   // the queue is full: later
      }
    }
  }
}

std::vector<std::string> syncGraphs(Document& document, Engine& engine) {
  engine.collectRetired();
  std::vector<std::string> problems;
  for (const std::string& instrumentId : document.takeChangedGraphs()) {
    const std::string why = engine.sendGraph(document.project(), instrumentId);
    if (!why.empty()) problems.push_back(instrumentId + ": " + why);
  }
  return problems;
}

std::vector<std::string> syncSamples(Document& document, Engine& engine) {
  std::vector<std::string> problems;
  for (const std::string& id : document.takeChangedSamples()) {
    const std::string why = engine.sendSample(document.project(), id);
    if (why.empty()) continue;
    document.markSampleChanged(id);
    problems.push_back(id + ": " + why);
  }
  return problems;
}

void syncLoop(Document& document, Engine& engine) {
  if (!document.takeLoopChanged()) return;
  engine.setLength(document.project().song.length);
  const LoopRange loop = loopOf(document.project().song);
  engine.setLoop(loop.on ? loop.start : 0, loop.on ? loop.end : 0);
}

}  // namespace wax

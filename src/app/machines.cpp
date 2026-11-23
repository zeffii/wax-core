#include "app/machines.h"

#include <algorithm>
#include <set>

#include "app/native_additions.h"
#include "core/node_registry.h"

namespace wax::app {

namespace {

struct MachineKind { const char* kind; const char* label; bool needsSample; const char* sampleNode; bool allSamples; const char* json; };
struct MachineLeaf { const char* path; const char* kind; const char* name; };

#include "app/machine_table.inc"

}  // namespace

const std::vector<MachineNode>& machineTree() {
  static const std::vector<MachineNode> tree = [] {
    std::vector<MachineNode> top;
    for (const MachineLeaf& leaf : kMachineLeaves) {
      std::vector<MachineNode>* level = &top;
      std::string path;
      std::string rest = leaf.path;
      while (!rest.empty()) {   // down through the groups, making each the first time it is met
        const std::size_t slash = rest.find('/');
        const std::string part = rest.substr(0, slash);
        rest = slash == std::string::npos ? "" : rest.substr(slash + 1);
        path += (path.empty() ? "" : "/") + part;
        auto found = std::find_if(level->begin(), level->end(), [&](const MachineNode& n) { return n.kind.empty() && n.name == part; });
        if (found == level->end()) { MachineNode group; group.name = part; group.path = path; level->push_back(group); found = level->end() - 1; }
        level = &found->children;
      }
      MachineNode machine;
      machine.name = leaf.name;
      machine.path = path + "/" + leaf.name;
      machine.kind = leaf.kind;
      machine.disabled = true;
      for (const MachineKind& k : kMachineKinds) {
        if (machine.kind != k.kind) continue;
        machine.needsSample = k.needsSample;
        machine.disabled = false;
        const Json definition = Json::parse(k.json);
        if (definition.contains("graph") && definition["graph"].contains("nodes")) {   // a machine whose nodes are not in this engine yet is not offered
          for (const Json& node : definition["graph"]["nodes"]) if (!builtinNodes().find(node.value("type", std::string()))) machine.disabled = true;
        }
      }
      level->push_back(machine);
    }
    return top;
  }();
  return tree;
}

Json machineJson(const std::string& kind, const std::vector<std::string>& sampleIds) {
  for (const MachineKind& k : kMachineKinds) {
    if (kind != k.kind) continue;
    Json instrument = Json::parse(k.json);
    applyNativeMachineAdditions(kind, instrument);
    if (k.needsSample && k.sampleNode[0] && !sampleIds.empty()) {
      Json samples = Json::object();
      if (k.allSamples) samples[k.sampleNode] = sampleIds; else samples[k.sampleNode] = sampleIds.front();
      instrument["samples"] = samples;
    }
    return instrument;
  }
  return Json();
}

std::string uniqueName(const std::string& base, const std::vector<std::string>& taken) {
  const std::set<std::string> used(taken.begin(), taken.end());
  std::string id = base;
  for (int i = 2; used.count(id); i++) id = base + std::to_string(i);
  return id;
}

}  // namespace wax::app

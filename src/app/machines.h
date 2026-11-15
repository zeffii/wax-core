// The kinds of instrument that can be added, laid out as the web app's machine tree. A fresh instrument of each kind is the web app's own
// (generated into machine_table.inc by tools/gen-machines.mjs), so a machine starts as it does there.
#pragma once

#include <string>
#include <vector>

#include "project/project.h"

namespace wax::app {

struct MachineNode {
  std::string name;            // as shown
  std::string path;            // groups: "synths/analogue"; machines: the same plus the machine's name
  std::string kind;            // a machine's kind ("" for a group)
  bool disabled = false;       // needs something that is not here yet (a sample)
  std::vector<MachineNode> children;
};

const std::vector<MachineNode>& machineTree();

// A fresh instrument of this kind (null when there is no such kind).
Json machineJson(const std::string& kind);

// The id for a new instrument or lane: `base`, then base2, base3 ... the first not taken.
std::string uniqueName(const std::string& base, const std::vector<std::string>& taken);

}  // namespace wax::app

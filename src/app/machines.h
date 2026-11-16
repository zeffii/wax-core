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
  bool disabled = false;       // its nodes are not in this engine
  bool needsSample = false;    // plays samples: offered once the project has one
  std::vector<MachineNode> children;
};

const std::vector<MachineNode>& machineTree();

// A fresh instrument of this kind (null when there is no such kind). A machine that plays samples is given them (`sampleIds`: the first one, or
// all of them as its slots, as the web app does).
Json machineJson(const std::string& kind, const std::vector<std::string>& sampleIds = {});

// The id for a new instrument or lane: `base`, then base2, base3 ... the first not taken.
std::string uniqueName(const std::string& base, const std::vector<std::string>& taken);

}  // namespace wax::app

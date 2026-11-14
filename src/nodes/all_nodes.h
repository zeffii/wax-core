// One register function per ported node file. Adding a node: write nodes/<name>.cpp with a registerXxx(),
// declare it here, call it in all_nodes.cpp and list the .cpp in CMakeLists.txt.
#pragma once

#include "core/node_registry.h"

namespace wax {

void registerGain(NodeRegistry& registry);

}  // namespace wax

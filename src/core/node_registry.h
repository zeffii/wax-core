// Every node type the native engine knows, by the name used in .wax files.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/node.h"

namespace wax {

class NodeRegistry {
public:
  // Adds a type. An effect gets its "bypass" parameter here, as in webtracker's nodes/index.js.
  void add(NodeType type);

  // Returns nullptr for a type that has not been ported yet.
  const NodeType* find(const std::string& type) const;

  std::vector<std::string> typeNames() const;   // sorted

private:
  std::unordered_map<std::string, NodeType> types_;
};

// The registry with every ported node in it (see nodes/all_nodes.cpp).
const NodeRegistry& builtinNodes();

}  // namespace wax

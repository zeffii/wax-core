#include "core/node_registry.h"

#include <algorithm>
#include <stdexcept>

namespace wax {

void NodeRegistry::add(NodeType type) {
  if (types_.count(type.type)) throw std::logic_error("node type \"" + type.type + "\" is registered twice");
  if (type.isEffect) type.params.push_back({"bypass", 0, 1, 0, true});
  const std::string name = type.type;
  types_.emplace(name, std::move(type));
}

const NodeType* NodeRegistry::find(const std::string& type) const {
  const auto found = types_.find(type);
  return found == types_.end() ? nullptr : &found->second;
}

std::vector<std::string> NodeRegistry::typeNames() const {
  std::vector<std::string> names;
  names.reserve(types_.size());
  for (const auto& entry : types_) names.push_back(entry.first);
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace wax

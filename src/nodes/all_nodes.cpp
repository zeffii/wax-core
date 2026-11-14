#include "nodes/all_nodes.h"

namespace wax {

const NodeRegistry& builtinNodes() {
  static const NodeRegistry registry = [] {
    NodeRegistry r;
    registerGain(r);
    return r;
  }();
  return registry;
}

}  // namespace wax

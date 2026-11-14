#include "edit/sync.h"

#include <stdexcept>

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

}  // namespace wax

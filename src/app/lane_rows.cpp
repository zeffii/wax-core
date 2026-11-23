#include "app/lane_rows.h"

#include "app/pattern_view.h"

namespace wax::app {

void LaneRows::update(const Document& doc) {
  if (revision_ == doc.revision()) return;
  for (const Lane& lane : doc.project().lanes) {
    Rows fresh = flattenLane(lane, doc.project().song.length);
    Entry& entry = lanes_[lane.id];
    if (fresh != entry.rows) { entry.rows = std::move(fresh); entry.version++; }   // (only a lane whose rows really changed)
  }
  revision_ = doc.revision();
}

const LaneRows::Rows& LaneRows::of(const std::string& laneId) const {
  static const Rows none;
  const auto found = lanes_.find(laneId);
  return found == lanes_.end() ? none : found->second.rows;
}

int LaneRows::version(const std::string& laneId) const {
  const auto found = lanes_.find(laneId);
  return found == lanes_.end() ? 0 : found->second.version;
}

}  // namespace wax::app

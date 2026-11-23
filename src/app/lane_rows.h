// The song as the tables show it: every lane flattened into one row of cell texts per tick. Rebuilt when the document changes (not every
// frame), and a lane counts how many times its rows really changed, so what is drawn from them (the pattern pictures) is only made again
// for the lane that was edited.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "edit/document.h"

namespace wax::app {

class LaneRows {
public:
  using Rows = std::vector<std::vector<std::string>>;

  // Brings the rows up to date with the document (does nothing when it has not changed since the last call).
  void update(const Document& doc);

  // A lane's rows (empty for a lane that is not there).
  const Rows& of(const std::string& laneId) const;

  // How many times the lane's rows have changed since the song was opened.
  int version(const std::string& laneId) const;

private:
  struct Entry {
    Rows rows;
    int version = 0;
  };
  std::map<std::string, Entry> lanes_;
  int revision_ = -1;   // the document's revision the rows show
};

}  // namespace wax::app

// The line under the pattern grid: what the column under the caret is, its range, and what the cell holds in real units.
// Port of webtracker src/seq/describe.js (describeColumn).
#pragma once

#include <string>
#include <vector>

#include "project/project.h"
#include "seq/columns.h"

namespace wax::app {

// `lane` is the lane shown, `rows` its cells (rows[tick][column]), `tick` and `column` the caret. A blank cell shows what holds there:
// the last value written above it, or the parameter's default when the column has never been written.
std::string describeColumn(const Project& project, const Lane& lane, const std::vector<Column>& columns,
                           const std::vector<std::vector<std::string>>& rows, int tick, int column);

}  // namespace wax::app

// Onset detection for auto-slice: where do the hits in a sample start? Port of webtracker src/project/onsets.js (spectral flux, a threshold that
// follows the local average, peaks picked from it, each pulled back to where the sound actually starts).
#pragma once

#include <vector>

namespace wax {

struct OnsetSettings {
  int from = 0, to = -1;        // the frames to look at (to < 0: the end)
  double sensitivity = 0.5;     // 0..1: how small a rise still counts (higher finds more)
  double minGapMs = 30;         // no two closer than this
};

// The onsets as frame numbers in increasing order. `right` (empty for mono) is averaged in.
std::vector<int> detectOnsets(const std::vector<float>& left, const std::vector<float>& right, double rate, const OnsetSettings& settings = OnsetSettings());

}  // namespace wax

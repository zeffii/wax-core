#include "nodes/spacecore.h"

#include <algorithm>

#include "core/js_math.h"

namespace wax::space {
namespace {

const std::vector<std::vector<int>>& chords() {
  static const std::vector<std::vector<int>> steps = {
    {0, 7}, {0, 4, 7}, {0, 3, 7}, {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 5, 7}, {0, 3, 6, 9}, {0, 2, 4, 6, 8, 10}};
  return steps;
}

}  // namespace

double midiHz(double midi) { return 440 * js::pow(2, (midi - 69) / 12); }

std::vector<int> stringsOf(double root, double chord, double count) {
  const auto& all = chords();
  const std::vector<int>& steps = all[static_cast<std::size_t>(std::max(0, std::min(static_cast<int>(all.size()) - 1, jsRoundInt(chord))))];
  const int size = static_cast<int>(steps.size());
  const std::size_t wanted = static_cast<std::size_t>(std::max(1, jsRoundInt(count)));
  std::vector<int> out;
  for (int i = 0; out.size() < wanted; i++) out.push_back(jsRoundInt(root) + 12 * (i / size) + steps[static_cast<std::size_t>(i % size)]);
  return out;
}

std::vector<int> wantedStrings(const std::vector<int>& bag, double partials, double count) {
  std::vector<int> out;
  const int most = std::max(1, jsRoundInt(partials));
  for (int h = 1; h <= most; h++) {
    for (const int m : bag) {
      const int p = jsRoundInt(m + 12 * js::log2(h));
      if (p >= 24 && p <= 120 && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
  }
  const std::size_t keep = static_cast<std::size_t>(std::max(1, jsRoundInt(count)));
  if (out.size() > keep) out.resize(keep);
  return out;
}

}  // namespace wax::space

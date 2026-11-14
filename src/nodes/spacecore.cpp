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

Room reflections(double w, double d, double h, double sFracX, double sFracY, double lFracX, double lFracY, double absorb, double taps) {
  constexpr int kOrder = 3;
  const double z = std::min(1.5, h / 2), sx = sFracX * w, sy = sFracY * d, lx = lFracX * w, ly = lFracY * d;
  const double dist0 = std::max(0.3, jsHypot(sx - lx, sy - ly));
  const double keep = 1 - std::min(0.98, std::max(0.0, absorb));
  std::vector<Tap> list;
  auto axis = [](int n, double size, double at) { return n % 2 == 0 ? n * size + at : n * size + size - at; };
  for (int nx = -kOrder; nx <= kOrder; nx++)
    for (int ny = -kOrder; ny <= kOrder; ny++)
      for (int nz = -kOrder; nz <= kOrder; nz++) {
        const int order = std::abs(nx) + std::abs(ny) + std::abs(nz);
        if (order == 0 || order > kOrder) continue;
        const double x = axis(nx, w, sx), y = axis(ny, d, sy), zz = axis(nz, h, z);
        const double dx = x - lx, dy = y - ly, dz = zz - z, dist = std::max(0.3, jsHypot(dx, dy, dz));
        const double flat = jsHypot(dx, dy);
        list.push_back({dist / kSpeed, dist0 / dist * js::pow(keep, order), flat < 1e-6 ? 0 : dx / flat, order});
      }
  std::stable_sort(list.begin(), list.end(), [](const Tap& a, const Tap& b) { return a.delay < b.delay; });
  const int count = std::max(1, std::min(kMaxTaps, jsRoundInt(taps)));
  list.resize(std::min<size_t>(list.size(), count));
  return {dist0 / kSpeed, list};
}

}  // namespace wax::space

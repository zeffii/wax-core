#include "nodes/drawings.h"

#include <map>

namespace wax {

const std::vector<DrawingInfo>& drawingsOf(const std::string& nodeType) {
  static const std::map<std::string, std::vector<DrawingInfo>> table = {
      {"env2", {{"main", "envelope", "time", true}}},
      {"drumsynth2", {{"pitch", "pitch", "", false}, {"amp", "volume", "", false}, {"noise", "noise", "", false}}},
      {"hat2", {{"closed", "closed hat", "", false}, {"open", "open hat", "", false}}},
      {"compass", {{"amp", "volume", "atime", true}, {"filter", "filter", "", false}}},
      {"khufu", {{"amp", "volume", "atime", true}}},
      {"pulsar", {{"sweep", "sweep", "sweepms", false}}},
      {"twin", {{"a", "envelope 1", "time1", true}, {"b", "envelope 2", "time2", true}}},
  };
  static const std::vector<DrawingInfo> none;
  const auto found = table.find(nodeType);
  return found == table.end() ? none : found->second;
}

bezier::Spline defaultDrawing(const std::string& nodeType, const std::string& name) {
  using bezier::makeAnchor;
  if (nodeType == "drumsynth2") {
    static const double pitchR[] = {0.015, -0.7}, pitchL[] = {-0.6, 0};
    static const double ampR[] = {0.06, 0}, ampL[] = {-0.55, 0};
    static const double noiseR[] = {0.02, -0.55}, noiseL[] = {-0.4, 0};
    if (name == "pitch") return {makeAnchor(0, 1, nullptr, pitchR), makeAnchor(1, 0, pitchL, nullptr)};
    if (name == "amp") return {makeAnchor(0, 1, nullptr, ampR), makeAnchor(1, 0, ampL, nullptr)};
    if (name == "noise") return {makeAnchor(0, 1, nullptr, noiseR), makeAnchor(1, 0, noiseL, nullptr)};
  } else if (nodeType == "hat2") {
    static const double closedR[] = {0.02, -0.3}, closedL[] = {-0.45, 0};
    static const double openR[] = {0.04, -0.15}, openL[] = {-0.5, 0};
    if (name == "closed") return {makeAnchor(0, 1, nullptr, closedR), makeAnchor(1, 0, closedL, nullptr)};
    if (name == "open") return {makeAnchor(0, 1, nullptr, openR), makeAnchor(1, 0, openL, nullptr)};
  } else if (nodeType == "khufu" && name == "amp") {   // a slow rise, held, and a long fall when the note ends
    const double r0[2] = {0.08, 0.5}, l1[2] = {-0.1, 0}, r1[2] = {0.1, 0}, l2[2] = {-0.5, 0};
    bezier::Spline s = {makeAnchor(0, 0, nullptr, r0), makeAnchor(0.25, 1, l1, r1), makeAnchor(1, 0, l2, nullptr)};
    s[1].sustain = true;
    return s;
  } else if (nodeType == "compass") {
    if (name == "amp") {
      const double r0[2] = {0, 0}, l1[2] = {0, 0}, r1[2] = {0.1, 0}, l2[2] = {-0.5, 0};
      bezier::Spline s = {makeAnchor(0, 0, nullptr, r0), makeAnchor(0.02, 1, l1, r1), makeAnchor(1, 0, l2, nullptr)};
      s[1].sustain = true;
      return s;
    }
    if (name == "filter") {
      const double r0[2] = {0.05, -0.5}, l1[2] = {-0.55, 0};
      return {makeAnchor(0, 1, nullptr, r0), makeAnchor(1, 0, l1, nullptr)};
    }
  }
  return bezier::shapeOf(bezier::Shape::Fall);
}

}  // namespace wax

#include "seq/curve_edit.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "seq/automation.h"

namespace wax {

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();
double clamp01(double x) { return std::min(1.0, std::max(0.0, x)); }

const char* pinName(CurvePin pin) { return pin == CurvePin::Start ? "start" : pin == CurvePin::End ? "end" : ""; }

double edgeAt(const CurveSpan& span, CurvePin edge) { return edge == CurvePin::Start ? span.start : span.start + span.length; }

// New kinds for a changed list of anchors: two that were neighbours keep their segment, any other pair is a gap.
// `origin[i]` is the index each new anchor had in the old list (-1: a new anchor).
void reknit(LaneCurve& curve, const std::vector<CurvePoint>& oldPts, const std::vector<char>& oldKinds, const std::vector<CurvePoint>& pts, const std::vector<int>& origin) {
  std::vector<char> kinds;
  for (std::size_t i = 0; i + 1 < pts.size(); i++) {
    const int a = origin[i], b = origin[i + 1];
    kinds.push_back(a >= 0 && b == a + 1 ? (static_cast<std::size_t>(a) < oldKinds.size() ? oldKinds[static_cast<std::size_t>(a)] : 'c') : 'g');
  }
  (void)oldPts;
  curve.pts = pts;
  curve.kinds = kinds;
}

}  // namespace

LaneCurve readLaneCurve(const Json& json) {
  LaneCurve curve;
  curve.raw = json.is_object() ? json : Json::object();
  for (const Json& p : curve.raw.value("pts", Json::array())) {
    CurvePoint point;
    point.t = p.value("t", 0.0);
    point.v = p.value("v", 0.0);
    const std::string pin = p.value("pin", std::string());
    point.pin = pin == "start" ? CurvePin::Start : pin == "end" ? CurvePin::End : CurvePin::None;
    curve.pts.push_back(point);
  }
  for (const Json& k : curve.raw.value("kinds", Json::array()))
    curve.kinds.push_back(k.is_string() && !k.get<std::string>().empty() ? k.get<std::string>()[0] : 'c');
  curve.kinds.resize(curve.pts.empty() ? 0 : curve.pts.size() - 1, 'c');
  return curve;
}

Json writeLaneCurve(const LaneCurve& curve) {
  Json out = curve.raw.is_object() ? curve.raw : Json::object();
  Json pts = Json::array(), kinds = Json::array();
  for (const CurvePoint& p : curve.pts) {
    Json point = Json::object();
    point["t"] = p.t;
    point["v"] = p.v;
    if (p.pin != CurvePin::None) point["pin"] = pinName(p.pin);
    pts.push_back(point);
  }
  for (std::size_t i = 0; i + 1 < curve.pts.size(); i++) kinds.push_back(std::string(1, curve.kindAt(i)));
  out["pts"] = pts;
  out["kinds"] = kinds;
  return out;
}

LaneCurve newLaneCurve(const std::string& target, double t, double v) {
  LaneCurve curve;
  curve.raw = Json::object();
  curve.raw["target"] = target;
  curve.raw["on"] = true;
  curve.pts.push_back({t, v, CurvePin::None});
  return curve;
}

std::vector<CurveSpan> curveSpans(const Lane& lane) {
  std::vector<CurveSpan> out;
  for (const Placement& placement : lane.placements) {
    int length = placement.length;
    for (const Pattern& pattern : lane.patterns)
      if (pattern.id == placement.pattern) length = std::min(length, static_cast<int>(pattern.rows.size()));
    out.push_back({placement.pattern, placement.start, length});
  }
  return out;
}

std::vector<CurveEdge> edgesOf(const Lane& lane) {
  std::vector<CurveEdge> out;
  for (const CurveSpan& span : curveSpans(lane)) {
    out.push_back({static_cast<double>(span.start), CurvePin::Start, span.pattern});
    out.push_back({static_cast<double>(span.start + span.length), CurvePin::End, span.pattern});
  }
  return out;
}

double curveValueAt(const LaneCurve& curve, double t) {
  const std::vector<CurvePoint>& p = curve.pts;
  if (p.empty() || t < p[0].t) return kNaN;
  if (t >= p.back().t) return t == p.back().t ? p.back().v : kNaN;
  std::size_t i = 0;
  while (p[i + 1].t <= t) i++;
  return curveSegmentValue(p[i].t, p[i].v, p[i + 1].t, p[i + 1].v, curve.kindAt(i), t);
}

bool segmentMiddle(const LaneCurve& curve, std::size_t i, double& t, double& v) {
  if (i + 1 >= curve.pts.size()) return false;
  const CurvePoint &a = curve.pts[i], &b = curve.pts[i + 1];
  t = (a.t + b.t) / 2;
  const double value = curveValueAt(curve, t);
  v = std::isnan(value) ? (a.v + b.v) / 2 : value;
  return true;
}

CurveSnap snapTick(const Lane& lane, double t, bool edges, int rows, int reach, int length) {
  if (edges) {
    bool found = false;
    CurveSnap best;
    for (const CurveEdge& edge : edgesOf(lane)) {
      if (edge.t > length || std::abs(edge.t - t) > reach) continue;
      const bool closer = !found || std::abs(edge.t - t) < std::abs(best.t - t) || (edge.t == best.t && edge.edge == CurvePin::Start);   // where an end meets a start, the start wins
      if (closer) { best = {edge.t, edge.edge}; found = true; }
    }
    if (found) return best;
  }
  if (rows > 0) return {std::round(t / rows) * rows, CurvePin::None};
  return {std::round(t), CurvePin::None};
}

int addPoint(LaneCurve& curve, double t, double v, CurvePin pin) {
  std::vector<CurvePoint>& p = curve.pts;
  for (std::size_t k = 0; k < p.size(); k++)
    if (p[k].t == t) { p[k].v = v; if (pin != CurvePin::None) p[k].pin = pin; return static_cast<int>(k); }
  std::size_t i = 0;
  while (i < p.size() && p[i].t <= t) i++;
  p.insert(p.begin() + static_cast<std::ptrdiff_t>(i), {t, v, pin});
  if (p.size() > 1) {   // the new anchor splits a segment: both halves take its kind
    const std::size_t at = i == 0 ? 0 : i - 1;
    const char kind = at < curve.kinds.size() ? curve.kinds[at] : 'c';
    curve.kinds.insert(curve.kinds.begin() + static_cast<std::ptrdiff_t>(std::min(at, curve.kinds.size())), kind);
  }
  curve.kinds.resize(p.size() - 1, 'c');
  return static_cast<int>(i);
}

bool removePoint(LaneCurve& curve, int i) {
  if (i < 0 || i >= static_cast<int>(curve.pts.size())) return false;
  curve.pts.erase(curve.pts.begin() + i);
  if (!curve.kinds.empty()) curve.kinds.erase(curve.kinds.begin() + std::min<std::ptrdiff_t>(i, static_cast<std::ptrdiff_t>(curve.kinds.size()) - 1));
  curve.kinds.resize(curve.pts.empty() ? 0 : curve.pts.size() - 1, 'c');
  return true;
}

void movePoint(LaneCurve& curve, int i, double t, double v) {
  std::vector<CurvePoint>& p = curve.pts;
  if (i < 0 || i >= static_cast<int>(p.size())) return;
  const std::size_t k = static_cast<std::size_t>(i);
  const double lo = k > 0 ? p[k - 1].t + 1 : 0, hi = k + 1 < p.size() ? p[k + 1].t - 1 : std::numeric_limits<double>::infinity();
  p[k].t = std::min(hi, std::max(lo, std::round(t)));
  p[k].v = clamp01(v);
  p[k].pin = CurvePin::None;   // dragging lets go of a pin; dropping on an edge snaps and pins again
}

bool toggleGap(LaneCurve& curve, int i) {
  if (i < 0 || i + 1 >= static_cast<int>(curve.pts.size())) return false;
  char& kind = curve.kinds[static_cast<std::size_t>(i)];
  kind = kind == 'g' ? 'c' : 'g';
  return true;
}

int followPins(LaneCurve& curve, const std::vector<CurveSpan>& before, const std::vector<CurveSpan>& after) {
  int freed = 0;
  for (CurvePoint& point : curve.pts) {
    if (point.pin == CurvePin::None) continue;
    const CurveSpan* was = nullptr;
    for (const CurveSpan& span : before) if (edgeAt(span, point.pin) == point.t) { was = &span; break; }
    if (!was) { point.pin = CurvePin::None; freed++; continue; }
    // the same pattern, nearest to where it was (a pattern can be placed several times)
    const CurveSpan* now = nullptr;
    for (const CurveSpan& span : after)
      if (span.pattern == was->pattern && (!now || std::abs(span.start - was->start) < std::abs(now->start - was->start))) now = &span;
    if (!now) { point.pin = CurvePin::None; freed++; continue; }
    point.t = edgeAt(*now, point.pin);
  }
  // anchors that landed on one tick keep the later one; order is restored
  std::vector<int> order(curve.pts.size());
  for (std::size_t i = 0; i < order.size(); i++) order[i] = static_cast<int>(i);
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return curve.pts[static_cast<std::size_t>(a)].t < curve.pts[static_cast<std::size_t>(b)].t; });
  std::vector<CurvePoint> pts;
  std::vector<int> origin;
  for (const int k : order) {
    if (!pts.empty() && pts.back().t == curve.pts[static_cast<std::size_t>(k)].t) { pts.pop_back(); origin.pop_back(); }
    pts.push_back(curve.pts[static_cast<std::size_t>(k)]);
    origin.push_back(k);
  }
  const std::vector<CurvePoint> oldPts = curve.pts;
  const std::vector<char> oldKinds = curve.kinds;
  reknit(curve, oldPts, oldKinds, pts, origin);
  return freed;
}

void clipCurve(LaneCurve& curve, int length) {
  while (!curve.pts.empty() && curve.pts.back().t > length) curve.pts.pop_back();
  curve.kinds.resize(curve.pts.empty() ? 0 : curve.pts.size() - 1, 'c');
}

std::vector<std::pair<int, int>> curveSections(const LaneCurve& curve) {
  std::vector<std::pair<int, int>> runs;
  int from = 0;
  const int count = static_cast<int>(curve.pts.size());
  for (int i = 0; i < count; i++) {
    if (i == count - 1 || curve.kindAt(static_cast<std::size_t>(i)) == 'g') { runs.push_back({from, i}); from = i + 1; }
  }
  return runs;
}

bool addSection(LaneCurve& curve, double t0, double t1, double v) {
  for (const CurvePoint& p : curve.pts) if (p.t >= t0 && p.t <= t1) return false;
  std::vector<CurvePoint> pts;
  std::vector<int> origin;
  int added = -1;
  for (std::size_t i = 0; i < curve.pts.size(); i++) if (curve.pts[i].t < t0) { pts.push_back(curve.pts[i]); origin.push_back(static_cast<int>(i)); }
  added = static_cast<int>(pts.size());
  pts.push_back({t0, v, CurvePin::Start}); origin.push_back(-1);
  pts.push_back({t1, v, CurvePin::End}); origin.push_back(-1);
  for (std::size_t i = 0; i < curve.pts.size(); i++) if (curve.pts[i].t > t1) { pts.push_back(curve.pts[i]); origin.push_back(static_cast<int>(i)); }
  const std::vector<CurvePoint> oldPts = curve.pts;
  const std::vector<char> oldKinds = curve.kinds;
  reknit(curve, oldPts, oldKinds, pts, origin);
  curve.kinds[static_cast<std::size_t>(added)] = 'c';
  return true;
}

bool removeSection(LaneCurve& curve, double t0, double t1) {
  std::vector<CurvePoint> pts;
  std::vector<int> origin;
  for (std::size_t i = 0; i < curve.pts.size(); i++) if (curve.pts[i].t < t0 || curve.pts[i].t > t1) { pts.push_back(curve.pts[i]); origin.push_back(static_cast<int>(i)); }
  if (pts.size() == curve.pts.size() || pts.empty()) return false;
  const std::vector<CurvePoint> oldPts = curve.pts;
  const std::vector<char> oldKinds = curve.kinds;
  reknit(curve, oldPts, oldKinds, pts, origin);
  return true;
}

int extendCurve(const Lane& lane, LaneCurve& curve, int i, int length) {
  if (i < 0 || i != static_cast<int>(curve.pts.size()) - 1) return -1;
  const CurvePoint last = curve.pts[static_cast<std::size_t>(i)];
  const CurveEdge* next = nullptr;
  const std::vector<CurveEdge> edges = edgesOf(lane);
  for (const CurveEdge& e : edges) if (e.edge == CurvePin::End && e.t > last.t && e.t <= length && (!next || e.t < next->t)) next = &e;
  if (!next) return -1;
  curve.pts.push_back({next->t, last.v, CurvePin::End});
  curve.kinds.push_back('c');
  return static_cast<int>(curve.pts.size()) - 1;
}

int skipCurve(const Lane& lane, LaneCurve& curve, int i, int length) {
  if (i < 0 || i != static_cast<int>(curve.pts.size()) - 1) return -1;
  const CurvePoint last = curve.pts[static_cast<std::size_t>(i)];
  std::vector<CurveSpan> starts;
  for (const CurveSpan& s : curveSpans(lane)) if (s.start >= last.t && s.start < length) starts.push_back(s);
  std::stable_sort(starts.begin(), starts.end(), [](const CurveSpan& a, const CurveSpan& b) { return a.start < b.start; });
  if (starts.size() < 2) return -1;
  const CurveSpan& next = starts[1];
  const double end = std::min<double>(length, next.start + next.length);
  curve.pts.push_back({static_cast<double>(next.start), last.v, CurvePin::Start});
  curve.pts.push_back({end, last.v, CurvePin::End});
  curve.kinds.push_back('g');
  curve.kinds.push_back('c');
  return static_cast<int>(curve.pts.size()) - 1;
}

}  // namespace wax

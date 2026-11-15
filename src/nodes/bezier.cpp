#include "nodes/bezier.h"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "core/js_math.h"

namespace wax::bezier {
namespace {

constexpr int kSteps = 40;   // how finely each segment is walked when the table is made

double clampTo(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }

}  // namespace

Anchor makeAnchor(double x, double y, const double* l, const double* r) {
  Anchor a;
  a.x = x; a.y = y;
  if (l) { a.hasLeft = true; a.left[0] = l[0]; a.left[1] = l[1]; }
  if (r) { a.hasRight = true; a.right[0] = r[0]; a.right[1] = r[1]; }
  return a;
}

Handles autoHandles(const Spline& a, std::size_t i) {
  const Anchor& p = a[i];
  const Anchor* prev = i > 0 ? &a[i - 1] : nullptr;
  const Anchor* next = i + 1 < a.size() ? &a[i + 1] : nullptr;
  if (!prev && !next) return {{0, 0}, {0, 0}};
  const Anchor& from = prev ? *prev : p;
  const Anchor& to = next ? *next : p;
  double tx = to.x - from.x, ty = to.y - from.y;
  double len = jsHypot(tx, ty);
  if (len == 0) len = 1;   // (|| 1)
  tx /= len; ty /= len;
  // an anchor at a turn (a peak or a valley) is kept level, so the curve does not overshoot it
  if (prev && next && (p.y - prev->y) * (next->y - p.y) <= 0) { tx = 1; ty = 0; }
  const double dxL = (prev ? std::fabs(prev->x - p.x) / 3 : 0) * -1;
  const double dxR = (next ? std::fabs(next->x - p.x) / 3 : 0) * 1;
  const double slope = tx > 1e-6 ? ty / tx : 0;
  Handles h{{0, 0}, {0, 0}};
  if (prev) { h.l[0] = dxL; h.l[1] = dxL * slope; }
  if (next) { h.r[0] = dxR; h.r[1] = dxR * slope; }
  return h;
}

Segment segmentOf(const Spline& a, std::size_t i) {
  const Anchor& p = a[i];
  const Anchor& q = a[i + 1];
  const Handles auto0 = autoHandles(a, i), auto1 = autoHandles(a, i + 1);
  const double* r = p.hasRight ? p.right : auto0.r;
  const double* l = q.hasLeft ? q.left : auto1.l;
  double c1x = clampTo(p.x + r[0], p.x, q.x), c2x = clampTo(q.x + l[0], p.x, q.x);
  if (c1x > c2x) c1x = c2x = (c1x + c2x) / 2;
  Segment s;
  s.x[0] = p.x; s.y[0] = p.y;
  s.x[1] = c1x; s.y[1] = clampTo(p.y + r[1], 0, 1);
  s.x[2] = c2x; s.y[2] = clampTo(q.y + l[1], 0, 1);
  s.x[3] = q.x; s.y[3] = q.y;
  return s;
}

namespace {

double bez(double p0, double p1, double p2, double p3, double t) {
  const double u = 1 - t;
  return u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3;
}

}  // namespace

// The spline walked as a line of (x, y) points.
std::vector<std::array<double, 2>> walk(const Spline& a) {
  std::vector<std::array<double, 2>> out;
  out.push_back({a[0].x, a[0].y});
  for (std::size_t i = 0; i + 1 < a.size(); i++) {
    const Segment s = segmentOf(a, i);
    for (int step = 1; step <= kSteps; step++) {
      const double t = static_cast<double>(step) / kSteps;
      out.push_back({bez(s.x[0], s.x[1], s.x[2], s.x[3], t), bez(s.y[0], s.y[1], s.y[2], s.y[3], t)});
    }
  }
  return out;
}

namespace {

bool finiteNumber(const Json& v) { return v.is_number() && std::isfinite(v.get<double>()); }

bool readHandle(const Json& p, const char* key, double out[2]) {
  if (!p.contains(key)) return false;
  const Json& h = p[key];
  if (!h.is_array() || h.size() < 2 || !finiteNumber(h[0]) || !finiteNumber(h[1])) return false;
  out[0] = h[0].get<double>(); out[1] = h[1].get<double>();
  return true;
}

}  // namespace

Spline shapeOf(Shape shape) {
  const auto make = [](std::initializer_list<std::tuple<double, double, const double*, const double*>> list) {
    Spline s;
    for (const auto& t : list) s.push_back(makeAnchor(std::get<0>(t), std::get<1>(t), std::get<2>(t), std::get<3>(t)));
    return s;
  };
  static const double fallR[] = {0.03, -0.5}, fallL[] = {-0.55, 0};
  static const double zero[] = {0, 0};
  static const double punchR0[] = {0.002, 0.4}, punchL1[] = {-0.006, 0}, punchR1[] = {0.06, -0.05}, punchL2[] = {-0.5, 0};
  static const double humpR0[] = {0.12, 0.05}, humpL1[] = {-0.15, 0}, humpR1[] = {0.15, 0}, humpL2[] = {-0.2, 0.05};
  static const double holdR0[] = {0.3, 0}, holdL1[] = {-0.2, 0}, holdR1[] = {0.1, -0.1}, holdL2[] = {-0.05, 0.2};
  switch (shape) {
    case Shape::Fall: return make({{0, 1, nullptr, fallR}, {1, 0, fallL, nullptr}});
    case Shape::Line: return make({{0, 1, nullptr, zero}, {1, 0, zero, nullptr}});
    case Shape::Punch: return make({{0, 0, nullptr, punchR0}, {0.02, 1, punchL1, punchR1}, {1, 0, punchL2, nullptr}});
    case Shape::Hump: return make({{0, 0, nullptr, humpR0}, {0.4, 1, humpL1, humpR1}, {1, 0, humpL2, nullptr}});
    case Shape::Hold: return make({{0, 1, nullptr, holdR0}, {0.7, 0.9, holdL1, holdR1}, {1, 0, holdL2, nullptr}});
    case Shape::Flat: return make({{0, 0, nullptr, zero}, {1, 0, zero, nullptr}});
  }
  return {};
}

Spline cleanSpline(const Json* stored, Shape fallback) { return cleanSpline(stored, shapeOf(fallback)); }

Spline cleanSpline(const Json* stored, const Spline& fallback) {
  if (!stored || !stored->is_array()) return fallback;
  Spline a;
  for (const Json& p : *stored) {
    if (!p.is_object() || !p.contains("x") || !p.contains("y") || !finiteNumber(p["x"]) || !finiteNumber(p["y"])) continue;
    Anchor n;
    n.x = clampTo(p["x"].get<double>(), 0, 1);
    n.y = clampTo(p["y"].get<double>(), 0, 1);
    n.hasLeft = readHandle(p, "l", n.left);
    n.hasRight = readHandle(p, "r", n.right);
    if (p.contains("t") && p["t"].is_string()) {
      const std::string t = p["t"].get<std::string>();
      if (t == "equal" || t == "free" || t == "sharp") n.kind = t;
    }
    n.sustain = p.contains("s") && p["s"].is_boolean() && p["s"].get<bool>();
    a.push_back(n);
  }
  if (a.size() < 2) return fallback;
  std::stable_sort(a.begin(), a.end(), [](const Anchor& p, const Anchor& q) { return p.x < q.x; });
  a.front().x = 0; a.back().x = 1;
  for (std::size_t i = 1; i < a.size(); i++)
    if (a[i].x - a[i - 1].x < 0.002) a[i].x = std::min(1.0, a[i - 1].x + 0.002);   // anchors never sit on top of each other
  a.back().x = 1;
  a.back().sustain = false;
  bool seen = false;
  for (Anchor& p : a) { if (p.sustain && seen) p.sustain = false; else if (p.sustain) seen = true; }   // one knot only
  return a;
}

std::optional<Knot> sustainKnot(const Spline& spline) {
  for (std::size_t i = 0; i < spline.size(); i++)
    if (spline[i].sustain) return Knot{static_cast<int>(i), spline[i].x, spline[i].y};
  return std::nullopt;
}

Table envelopeTable(const Spline& spline) {
  const auto line = walk(spline);
  Table table{};
  std::size_t j = 0;
  for (int k = 0; k <= kEnvPoints; k++) {
    const double x = static_cast<double>(k) / kEnvPoints;
    while (j + 2 < line.size() && line[j + 1][0] < x) j++;
    const double x0 = line[j][0], y0 = line[j][1], x1 = line[j + 1][0], y1 = line[j + 1][1];
    table[static_cast<std::size_t>(k)] = static_cast<float>(x1 > x0 ? y0 + (y1 - y0) * clampTo((x - x0) / (x1 - x0), 0, 1) : y1);
  }
  return table;
}

double readEnvelope(const Table& table, double pos) {
  const double x = clampTo(pos, 0, 1) * kEnvPoints;
  const int k = std::min(kEnvPoints - 1, static_cast<int>(x));
  const double f = x - k;
  return table[static_cast<std::size_t>(k)] + (static_cast<double>(table[static_cast<std::size_t>(k) + 1]) - table[static_cast<std::size_t>(k)]) * f;
}

Spline drawingOf(const Json* data, const char* name, Shape fallback) { return drawingOf(data, name, shapeOf(fallback)); }

Spline drawingOf(const Json* data, const char* name, const Spline& fallback) {
  if (data && data->is_object() && data->contains("env") && (*data)["env"].is_object() && (*data)["env"].contains(name))
    return cleanSpline(&(*data)["env"][name], fallback);
  return fallback;
}

}  // namespace wax::bezier

namespace wax::bezier {

Handles handlesOf(const Spline& spline, std::size_t i) {
  const Handles a = autoHandles(spline, i);
  const Anchor& p = spline[i];
  Handles h = a;
  if (p.hasLeft) { h.l[0] = p.left[0]; h.l[1] = p.left[1]; }
  if (p.hasRight) { h.r[0] = p.right[0]; h.r[1] = p.right[1]; }
  return h;
}

Split splitSegment(const Spline& spline, std::size_t i, double t) {
  const Segment s = segmentOf(spline, i);
  const auto mix = [t](const std::array<double, 2>& u, const std::array<double, 2>& v) { return std::array<double, 2>{u[0] + (v[0] - u[0]) * t, u[1] + (v[1] - u[1]) * t}; };
  const std::array<double, 2> p0{s.x[0], s.y[0]}, p1{s.x[1], s.y[1]}, p2{s.x[2], s.y[2]}, p3{s.x[3], s.y[3]};
  const auto a1 = mix(p0, p1), b1 = mix(p1, p2), c1 = mix(p2, p3), a2 = mix(a1, b1), b2 = mix(b1, c1), at = mix(a2, b2);
  Split out;
  out.point = makeAnchor(at[0], at[1], nullptr, nullptr);
  out.point.hasLeft = out.point.hasRight = true;
  out.point.left[0] = a2[0] - at[0]; out.point.left[1] = a2[1] - at[1];
  out.point.right[0] = b2[0] - at[0]; out.point.right[1] = b2[1] - at[1];
  out.left[0] = a1[0] - p0[0]; out.left[1] = a1[1] - p0[1];
  out.right[0] = c1[0] - p3[0]; out.right[1] = c1[1] - p3[1];
  return out;
}

Nearest nearestOnSpline(const Spline& spline, double x, double y, const double scale[2]) {
  Nearest best;
  for (std::size_t i = 0; i + 1 < spline.size(); i++) {
    const Segment s = segmentOf(spline, i);
    for (int k = 0; k <= kSteps * 2; k++) {
      const double t = static_cast<double>(k) / (kSteps * 2);
      const double px = bez(s.x[0], s.x[1], s.x[2], s.x[3], t), py = bez(s.y[0], s.y[1], s.y[2], s.y[3], t);
      const double d = std::hypot((px - x) * scale[0], (py - y) * scale[1]);
      if (best.segment < 0 || d < best.d) best = {static_cast<int>(i), t, px, py, d};
    }
  }
  return best;
}

Json toJson(const Spline& spline) {
  const auto round4 = [](double v) { return std::round(v * 10000) / 10000; };
  Json out = Json::array();
  for (const Anchor& a : spline) {
    Json p = Json::object();
    p["x"] = round4(a.x);
    p["y"] = round4(a.y);
    if (a.hasLeft) p["l"] = Json::array({round4(a.left[0]), round4(a.left[1])});
    else p["l"] = nullptr;
    if (a.hasRight) p["r"] = Json::array({round4(a.right[0]), round4(a.right[1])});
    else p["r"] = nullptr;
    if (!a.kind.empty()) p["t"] = a.kind;
    if (a.sustain) p["s"] = true;
    out.push_back(std::move(p));
  }
  return out;
}

}  // namespace wax::bezier

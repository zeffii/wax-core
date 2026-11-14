#include "seq/automation.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/js_math.h"

namespace wax {
namespace {

constexpr double kHandle = 0.42;   // handle length of a bezier segment, as a fraction of its time span
const double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Point {
  double t = 0, v = 0;
};

struct Curve {
  bool on = true;
  std::vector<Point> pts;
  std::vector<char> kinds;   // 'c', 'l' or 'g' for the segment after each point
  char kindAt(std::size_t i) const { return i < kinds.size() ? kinds[i] : 'c'; }
};

double clamp01(double x) { return std::min(1.0, std::max(0.0, x)); }

// The value of one segment at time tt, a bezier whose handles keep the value flat at both ends.
double segmentValue(const Point& a, const Point& b, char kind, double tt) {
  if (kind == 'g') return kNaN;
  if (b.t <= a.t) return b.v;
  const double x = (tt - a.t) / (b.t - a.t);
  if (kind == 'l') return a.v + (b.v - a.v) * x;
  // solve the bezier's time for x (its time handles are a third-ish of the span, so a few Newton steps do)
  const double t1 = kHandle, t2 = 1 - kHandle;   // x(u) control points 0, K, 1-K, 1
  double u = x;
  for (int i = 0; i < 6; i++) {
    const double m = 1 - u, xu = 3 * m * m * u * t1 + 3 * m * u * u * t2 + u * u * u;
    const double dx = 3 * m * m * t1 + 6 * m * u * (t2 - t1) + 3 * u * u * (1 - t2);
    if (std::abs(dx) < 1e-6) break;
    u = clamp01(u - (xu - x) / dx);
  }
  const double m = 1 - u;
  return a.v * (m * m * m + 3 * m * m * u) + b.v * (3 * m * u * u + u * u * u);   // y control points a.v, a.v, b.v, b.v
}

// The curve's value (0..1) at tick t: NaN before the first anchor, in a gap and after the last anchor.
double valueAt(const Curve& curve, double t) {
  const std::vector<Point>& p = curve.pts;
  if (p.empty() || t < p[0].t) return kNaN;
  if (t >= p.back().t) return t == p.back().t ? p.back().v : kNaN;
  std::size_t i = 0;
  while (p[i + 1].t <= t) i++;
  return segmentValue(p[i], p[i + 1], curve.kindAt(i), t);
}

struct Target {
  std::string id;
  int slot;
  double min, max;
  bool log;
  double taper;
  bool amp;
};

}  // namespace

std::vector<AutoCurve> buildAutomation(const Json& curves, const std::vector<Column>& columns, const std::string& instrument,
                                       const ParamRegistry& registry, int songLength) {
  std::vector<AutoCurve> out;
  if (!curves.is_array() || curves.empty()) return out;
  // the parameters of a lane that a curve can move: its parameter columns, plus the machine's output gain (Volume)
  std::vector<Target> targets;
  for (const Column& c : columns) {
    if (c.kind != ColumnKind::Param || c.slot < 0) continue;
    targets.push_back({c.node + "." + c.param, c.slot, c.min, c.max, c.log || (c.min > 0 && c.max / c.min >= 50), (c.node == "out" && c.param == "gain") ? 2.0 : 1.0, false});
  }
  const int amp = registry.slotOf(instrument + ".mix.amp");
  if (amp >= 0) {
    targets.erase(std::remove_if(targets.begin(), targets.end(), [](const Target& t) { return t.id == "out.gain"; }), targets.end());
    targets.insert(targets.begin(), {"out.gain", amp, 0, 1, false, 2.0, true});
  }
  const std::size_t size = static_cast<std::size_t>(songLength) * kAutoRes + 1;
  for (const Json& c : curves) {
    const std::string id = c.value("target", "");
    const auto t = std::find_if(targets.begin(), targets.end(), [&](const Target& x) { return x.id == id; });
    if (t == targets.end()) continue;
    Curve curve;
    curve.on = !(c.contains("on") && c["on"].is_boolean() && !c["on"].get<bool>());
    for (const Json& p : c.value("pts", Json::array())) curve.pts.push_back({p.value("t", 0.0), p.value("v", 0.0)});
    for (const Json& k : c.value("kinds", Json::array())) curve.kinds.push_back(k.is_string() && !k.get<std::string>().empty() ? k.get<std::string>()[0] : 'c');
    AutoCurve a;
    a.slot = t->slot; a.min = t->min; a.max = t->max; a.log = t->log; a.taper = t->taper; a.amp = t->amp;
    if (c.contains("rest") && c["rest"].is_number() && std::isfinite(c["rest"].get<double>())) { a.hasRest = true; a.rest = c["rest"].get<double>(); }
    a.table.resize(size);
    for (std::size_t i = 0; i < size; i++) a.table[i] = curve.on ? static_cast<float>(valueAt(curve, static_cast<double>(i) / kAutoRes)) : static_cast<float>(kNaN);
    out.push_back(std::move(a));
  }
  return out;
}

}  // namespace wax

#include "edit/spline_ops.h"

#include <algorithm>
#include <cmath>

namespace wax::bezier {

namespace {

double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }
double length(const double v[2]) { return std::hypot(v[0], v[1]); }

}  // namespace

const std::string& kindOf(const Anchor& anchor) {
  static const std::string smooth;
  return anchor.kind.empty() ? smooth : anchor.kind;
}

void materialize(Spline& spline, std::size_t i) {
  const Handles h = handlesOf(spline, i);
  Anchor& p = spline[i];
  const bool hasPrev = i > 0, hasNext = i + 1 < spline.size();
  p.hasLeft = p.hasRight = true;
  p.left[0] = hasPrev ? h.l[0] : 0; p.left[1] = hasPrev ? h.l[1] : 0;
  p.right[0] = hasNext ? h.r[0] : 0; p.right[1] = hasNext ? h.r[1] : 0;
}

int addAnchor(Spline& spline, double x, double y) {
  const double px = std::min(0.996, std::max(0.004, x));
  std::size_t k = 0;
  while (k < spline.size() && !(spline[k].x > px)) k++;
  Anchor a = makeAnchor(px, y, nullptr, nullptr);
  spline.insert(spline.begin() + static_cast<std::ptrdiff_t>(k), a);
  return static_cast<int>(k);
}

int insertOn(Spline& spline, const Nearest& at) {
  const std::size_t i = static_cast<std::size_t>(at.segment);
  const Split split = splitSegment(spline, i, at.t);
  Anchor& p = spline[i];
  Anchor& q = spline[i + 1];
  p.hasRight = true; p.right[0] = split.left[0]; p.right[1] = split.left[1];
  q.hasLeft = true; q.left[0] = split.right[0]; q.left[1] = split.right[1];
  spline.insert(spline.begin() + static_cast<std::ptrdiff_t>(i) + 1, split.point);
  return static_cast<int>(i) + 1;
}

bool removeAnchor(Spline& spline, std::size_t i) {
  if (i == 0 || i + 1 >= spline.size()) return false;
  spline.erase(spline.begin() + static_cast<std::ptrdiff_t>(i));
  return true;
}

void moveAnchor(Spline& spline, std::size_t i, double x, double y) {
  Anchor& p = spline[i];
  p.y = clamp01(y);
  if (i > 0 && i + 1 < spline.size()) p.x = std::min(spline[i + 1].x - kMinGap, std::max(spline[i - 1].x + kMinGap, clamp01(x)));
}

void setHandle(Spline& spline, std::size_t i, Side side, double x, double y, bool breakApart) {
  materialize(spline, i);
  Anchor& p = spline[i];
  const bool right = side == Side::Right;
  double* mine = right ? p.right : p.left;
  double* other = right ? p.left : p.right;
  mine[0] = right ? std::max(0.0, x - p.x) : std::min(0.0, x - p.x);
  mine[1] = y - p.y;
  if (breakApart && kindOf(p) != "free") p.kind = "free";
  const bool hasOther = right ? i > 0 : i + 1 < spline.size();
  if (!hasOther) return;
  const std::string& kind = kindOf(p);
  const double len = length(mine);
  if (kind == "equal") {
    other[0] = -mine[0]; other[1] = -mine[1];
  } else if (kind.empty() && len > 1e-9) {   // in line with this one, keeping its own length
    double keep = length(other);
    if (keep == 0) keep = len;
    other[0] = -mine[0] / len * keep; other[1] = -mine[1] / len * keep;
  }
}

void setKind(Spline& spline, std::size_t i, const std::string& kind) {
  if (i >= spline.size()) return;
  Anchor& p = spline[i];
  if (kind == "sharp") {
    p.kind = "sharp";
    p.hasLeft = p.hasRight = true;
    p.left[0] = p.left[1] = p.right[0] = p.right[1] = 0;
    return;
  }
  const bool wasSharp = kindOf(p) == "sharp";
  if (wasSharp) p.hasLeft = p.hasRight = false;   // out of a corner: smooth again by itself
  p.kind = kind == "smooth" ? std::string() : kind;
  const bool interior = i > 0 && i + 1 < spline.size();
  if (kind == "free") { materialize(spline, i); return; }
  if (kind == "equal" && interior) {
    materialize(spline, i);
    const double m = (length(p.left) + length(p.right)) / 2;
    double d = length(p.right);
    if (d == 0) d = 1;
    p.right[0] = p.right[0] / d * m; p.right[1] = p.right[1] / d * m;
    p.left[0] = -p.right[0]; p.left[1] = -p.right[1];
  } else if ((kind == "smooth" || kind.empty()) && p.hasLeft && p.hasRight && interior) {
    const double d = length(p.right);
    if (d > 1e-9) {
      const double k = length(p.left);
      p.left[0] = -p.right[0] / d * k; p.left[1] = -p.right[1] / d * k;
    }
  }
}

void setKnot(Spline& spline, std::size_t i) {
  if (i >= spline.size()) return;
  const bool was = spline[i].sustain;
  for (Anchor& p : spline) p.sustain = false;
  if (!was && i + 1 < spline.size()) spline[i].sustain = true;
}

void toggleLine(Spline& spline, std::size_t i) {
  if (i + 1 >= spline.size()) return;
  const Handles a = handlesOf(spline, i), b = handlesOf(spline, i + 1);
  Anchor& p = spline[i];
  Anchor& q = spline[i + 1];
  const bool flat = !a.r[0] && !a.r[1] && !b.l[0] && !b.l[1];
  const double dx = (q.x - p.x) / 3, dy = (q.y - p.y) / 3;
  p.hasRight = q.hasLeft = true;
  if (flat) { p.right[0] = dx; p.right[1] = dy; q.left[0] = -dx; q.left[1] = -dy; }
  else { p.right[0] = p.right[1] = 0; q.left[0] = q.left[1] = 0; }
}

void autoAll(Spline& spline) {
  for (Anchor& p : spline) { p.hasLeft = p.hasRight = false; p.kind.clear(); }
}

void flip(Spline& spline) {
  for (Anchor& p : spline) { p.y = 1 - p.y; p.left[1] = -p.left[1]; p.right[1] = -p.right[1]; }
}

void reverse(Spline& spline) {
  for (Anchor& p : spline) {
    p.sustain = false;
    p.x = 1 - p.x;
    std::swap(p.hasLeft, p.hasRight);
    const double l0 = p.left[0], l1 = p.left[1];
    p.left[0] = -p.right[0]; p.left[1] = p.right[1];
    p.right[0] = -l0; p.right[1] = l1;
  }
  std::reverse(spline.begin(), spline.end());
}

}  // namespace wax::bezier

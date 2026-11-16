#include "app/curve_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app/config.h"
#include "app/theme.h"

namespace wax::app {

namespace {

const Json& curvesOf(const Lane& lane) {
  static const Json none = Json::array();
  if (!lane.raw.is_object() || !lane.raw.contains("curves") || !lane.raw["curves"].is_array()) return none;
  return lane.raw["curves"];
}

double distance(ImVec2 a, ImVec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

}  // namespace

int CurveView::shownIndex(const Lane& lane) const {
  const int count = static_cast<int>(curvesOf(lane).size());
  const auto it = shown_.find(lane.id);
  return count == 0 ? 0 : std::clamp(it == shown_.end() ? 0 : it->second, 0, count - 1);
}

const Json* CurveView::shownCurve(const Lane& lane) const {
  const Json& curves = curvesOf(lane);
  return curves.empty() ? nullptr : &curves[static_cast<std::size_t>(shownIndex(lane))];
}

float CurveView::xOf(const LaneBox& box, double v) const {
  const float pad = 5 * scale_;
  return box.left + pad + static_cast<float>(v) * std::max(1.0f, box.width - 2 * pad);
}

double CurveView::valueOf(const LaneBox& box, float x) const {
  const float pad = 5 * scale_;
  return std::clamp(static_cast<double>((x - box.left - pad) / std::max(1.0f, box.width - 2 * pad)), 0.0, 1.0);
}

void CurveView::frame(const Project& project, std::vector<LaneBox> boxes, float top, float rowHeight, int cell, int firstTick, int lastTick, float scale) {
  project_ = &project;
  boxes_ = std::move(boxes);
  top_ = top;
  rowHeight_ = rowHeight;
  cell_ = std::max(1, cell);
  firstTick_ = firstTick;
  lastTick_ = lastTick;
  scale_ = scale;
  hover_ = Hit();
  if (!drag_.active && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) hover_ = hitTest(ImGui::GetIO().MousePos);
}

int CurveView::laneUnder(ImVec2 mouse) const {
  if (mouse.y < top_) return -1;
  for (std::size_t i = 0; i < boxes_.size(); i++)
    if (mouse.x >= boxes_[i].left && mouse.x < boxes_[i].left + boxes_[i].width) return static_cast<int>(i);
  return -1;
}

CurveView::Hit CurveView::hitTest(ImVec2 mouse) const {
  Hit best;
  double bestDistance = 1e9;
  if (mouse.y < top_) return best;
  for (std::size_t li = 0; li < boxes_.size(); li++) {
    const LaneBox& box = boxes_[li];
    const Json* json = box.lane ? shownCurve(*box.lane) : nullptr;
    if (!json || mouse.x < box.left - 4 * scale_ || mouse.x > box.left + box.width + 4 * scale_) continue;
    const LaneCurve curve = readLaneCurve(*json);
    for (std::size_t i = 0; i < curve.pts.size(); i++) {   // anchors first: they are small and sit on the line
      const double d = distance(mouse, ImVec2(xOf(box, curve.pts[i].v), yOf(curve.pts[i].t)));
      if (d < 7 * scale_ && d < bestDistance) { bestDistance = d; best = {Hit::Anchor, static_cast<int>(li), static_cast<int>(i)}; }
    }
    if (best.kind == Hit::Anchor) continue;
    const double t = tickOf(mouse.y);
    for (std::size_t i = 0; i + 1 < curve.pts.size(); i++) {
      const CurvePoint &a = curve.pts[i], &b = curve.pts[i + 1];
      if (t < a.t || t > b.t) continue;
      const char kind = curve.kindAt(i);
      const double v = kind == 'g' ? a.v + (b.v - a.v) * (b.t > a.t ? (t - a.t) / (b.t - a.t) : 0) : curveSegmentValue(a.t, a.v, b.t, b.v, kind, t);
      const double d = std::abs(mouse.x - xOf(box, v));
      if (d < 6 * scale_ && d < bestDistance) { bestDistance = d; best = {Hit::Segment, static_cast<int>(li), static_cast<int>(i)}; }
    }
  }
  return best;
}

void CurveView::draw(ImDrawList* draw, const Lane& lane, bool selected) {
  const auto at = std::find_if(boxes_.begin(), boxes_.end(), [&](const LaneBox& b) { return b.lane == &lane; });
  const Json* json = shownCurve(lane);
  if (at == boxes_.end() || !json) return;
  const int li = static_cast<int>(at - boxes_.begin());
  const LaneBox& box = *at;
  const LaneCurve curve = readLaneCurve(*json);
  const bool on = !(json->contains("on") && (*json)["on"].is_boolean() && !(*json)["on"].get<bool>());
  const ImU32 ink = withAlpha(selected ? kAccent : kText, (selected ? 235 : 150) * (on ? 1 : 0) + (on ? 0 : 60));
  const float thin = 1.2f * scale_;
  const int ticks0 = std::max(0, firstTick_), ticks1 = std::min(static_cast<int>(project_->song.length), lastTick_);
  for (std::size_t i = 0; i + 1 < curve.pts.size(); i++) {
    const CurvePoint &a = curve.pts[i], &b = curve.pts[i + 1];
    if (b.t < ticks0 || a.t > ticks1) continue;
    const bool picked = selected_.kind == Hit::Segment && selected_.lane == li && selected_.index == static_cast<int>(i);
    if (curve.kindAt(i) == 'g') {   // a gap: the curve is not there; a faint dotted chord shows where it would go
      const ImVec2 p = ImVec2(xOf(box, a.v), yOf(a.t)), q = ImVec2(xOf(box, b.v), yOf(b.t));
      const int dots = std::max(2, static_cast<int>(distance(p, q) / (5 * scale_)));
      for (int d = 0; d < dots; d += 2) draw->AddLine(ImVec2(p.x + (q.x - p.x) * d / dots, p.y + (q.y - p.y) * d / dots), ImVec2(p.x + (q.x - p.x) * (d + 1) / dots, p.y + (q.y - p.y) * (d + 1) / dots), withAlpha(kDimText, 120), thin);
      continue;
    }
    const double from = std::max<double>(a.t, ticks0), to = std::min<double>(b.t, ticks1);
    if (to <= from) continue;
    const int steps = std::max(2, static_cast<int>((to - from) / cell_ * 4));
    ImVec2 before;
    for (int s = 0; s <= steps; s++) {
      const double tick = from + (to - from) * s / steps;
      const double v = curveSegmentValue(a.t, a.v, b.t, b.v, curve.kindAt(i), tick);
      const ImVec2 now(xOf(box, v), yOf(tick));
      if (s > 0) draw->AddLine(before, now, ink, picked ? thin * 2 : thin);
      before = now;
    }
  }
  for (std::size_t i = 0; i < curve.pts.size(); i++) {
    const CurvePoint& p = curve.pts[i];
    if (p.t < ticks0 || p.t > ticks1) continue;
    const ImVec2 at2(xOf(box, p.v), yOf(p.t));
    const bool hot = (hover_.kind == Hit::Anchor && hover_.lane == li && hover_.index == static_cast<int>(i)) || (drag_.active && drag_.lane == li && drag_.point == static_cast<int>(i));
    const bool picked = selected_.kind == Hit::Anchor && selected_.lane == li && selected_.index == static_cast<int>(i);
    const float r = (hot || picked ? 4.0f : 2.8f) * scale_;
    if (p.pin != CurvePin::None) draw->AddRectFilled(ImVec2(at2.x - r, at2.y - r), ImVec2(at2.x + r, at2.y + r), ink);   // pinned to a pattern edge: a square
    else draw->AddCircleFilled(at2, r, ink);
    if (hot || picked) draw->AddCircle(at2, r + 2 * scale_, kText, 0, 1.0f);
  }
}

CurveEditRequest CurveView::replaced(int laneIndex, int curveIndex, const LaneCurve& curve, const std::string& label, bool fold) const {
  CurveEditRequest request;
  const Lane& lane = *boxes_[static_cast<std::size_t>(laneIndex)].lane;
  request.any = true;
  request.lane = lane.id;
  request.curves = curvesOf(lane);
  request.curves[static_cast<std::size_t>(curveIndex)] = writeLaneCurve(curve);
  request.label = label;
  request.fold = fold;
  return request;
}

CurveEditRequest CurveView::interact(const Targets& targets) {
  CurveEditRequest request;
  if (!project_) return request;
  const ImGuiIO& io = ImGui::GetIO();
  const bool inside = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
  const int length = project_->song.length;
  const int reach = std::max(1, static_cast<int>(6 * scale_ / rowHeight_ * cell_));   // 6 pixels, in ticks

  // dragging an anchor
  if (drag_.active) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      drag_ = Drag();
    } else {
      if (!drag_.moved && distance(io.MousePos, drag_.from) > 3 * scale_) drag_.moved = true;
      if (drag_.moved) {
        const LaneBox& box = boxes_[static_cast<std::size_t>(drag_.lane)];
        const int ci = shownIndex(*box.lane);
        LaneCurve curve = readLaneCurve(curvesOf(*box.lane)[static_cast<std::size_t>(ci)]);
        const CurveSnap snap = snapTick(*box.lane, tickOf(io.MousePos.y), !io.KeyAlt, io.KeyAlt ? 0 : cell_, reach, length);
        movePoint(curve, drag_.point, snap.t, valueOf(box, io.MousePos.x));
        if (snap.pin != CurvePin::None && curve.pts[static_cast<std::size_t>(drag_.point)].t == snap.t) curve.pts[static_cast<std::size_t>(drag_.point)].pin = snap.pin;
        request = replaced(drag_.lane, ci, curve, "Move anchor", drag_.edited);
        drag_.edited = true;
      }
    }
    return request;
  }

  if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hover_.kind == Hit::Anchor) {
    drag_ = Drag();
    drag_.active = true;
    drag_.lane = hover_.lane;
    drag_.point = hover_.index;
    drag_.from = io.MousePos;
    selected_ = hover_;
    return request;
  }
  if (inside && hover_.kind == Hit::Segment) {
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {   // a new anchor on the line
      const LaneBox& box = boxes_[static_cast<std::size_t>(hover_.lane)];
      const int ci = shownIndex(*box.lane);
      LaneCurve curve = readLaneCurve(curvesOf(*box.lane)[static_cast<std::size_t>(ci)]);
      const CurveSnap snap = snapTick(*box.lane, tickOf(io.MousePos.y), !io.KeyAlt, io.KeyAlt ? 0 : cell_, reach, length);
      double v = curveValueAt(curve, snap.t);
      if (std::isnan(v)) v = valueOf(box, io.MousePos.x);
      const int added = addPoint(curve, snap.t, v, snap.pin);
      selected_ = {Hit::Anchor, hover_.lane, added};
      return replaced(hover_.lane, ci, curve, "Add anchor");
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) selected_ = hover_;
  } else if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hover_.kind == Hit::None) {
    selected_ = Hit();
  }
  if (hover_.kind == Hit::Anchor && inside) {   // what the anchor is, near the pointer
    const LaneBox& box = boxes_[static_cast<std::size_t>(hover_.lane)];
    const Json& json = curvesOf(*box.lane)[static_cast<std::size_t>(shownIndex(*box.lane))];
    const LaneCurve curve = readLaneCurve(json);
    const CurvePoint& p = curve.pts[static_cast<std::size_t>(hover_.index)];
    std::string text = "tick " + std::to_string(static_cast<int>(p.t));
    for (const CurveTarget& t : targets(*box.lane))
      if (t.id == curve.target()) { char value[48]; std::snprintf(value, sizeof value, "  %s %.4g", t.label.c_str(), curveScaled(t, p.v)); text += value; }
    if (p.pin != CurvePin::None) text += p.pin == CurvePin::Start ? "  (pinned to a pattern's start)" : "  (pinned to a pattern's end)";
    ImGui::SetTooltip("%s", text.c_str());
  }

  if (inside && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    const int lane = hover_.kind != Hit::None ? hover_.lane : laneUnder(io.MousePos);
    if (lane >= 0) {
      menu_ = {lane, hover_.kind, hover_.index, tickOf(io.MousePos.y), valueOf(boxes_[static_cast<std::size_t>(lane)], io.MousePos.x)};
      ImGui::OpenPopup("curvemenu");
    }
  }
  if (ImGui::BeginPopup("curvemenu")) {
    request = menu(targets);
    ImGui::EndPopup();
  }
  return request;
}

CurveEditRequest CurveView::menu(const Targets& targets) {
  CurveEditRequest request;
  if (menu_.lane < 0 || menu_.lane >= static_cast<int>(boxes_.size())) return request;
  const LaneBox& box = boxes_[static_cast<std::size_t>(menu_.lane)];
  const Lane& lane = *box.lane;
  const Json& curves = curvesOf(lane);
  const int length = project_->song.length;
  const int reach = std::max(1, static_cast<int>(6 * scale_ / rowHeight_ * cell_));
  const bool has = !curves.empty();
  const int ci = shownIndex(lane);
  LaneCurve curve = has ? readLaneCurve(curves[static_cast<std::size_t>(ci)]) : LaneCurve();
  const std::vector<CurveTarget> all = targets(lane);
  const auto labelOf = [&](const std::string& id) {
    for (const CurveTarget& t : all) if (t.id == id) return t.label;
    return id;
  };
  const auto kindItems = [&](int segment) {
    static const struct { char kind; const char* name; } kinds[] = {{'c', "Curve"}, {'l', "Straight line"}, {'g', "Gap (the curve is not there)"}};
    for (const auto& k : kinds)
      if (ImGui::MenuItem(k.name, nullptr, curve.kindAt(static_cast<std::size_t>(segment)) == k.kind)) {
        curve.kinds[static_cast<std::size_t>(segment)] = k.kind;
        request = replaced(menu_.lane, ci, curve, "Change segment");
      }
  };

  if (has && menu_.kind == Hit::Anchor && menu_.index < static_cast<int>(curve.pts.size())) {
    const int i = menu_.index;
    if (ImGui::MenuItem("Delete anchor")) {
      removePoint(curve, i);
      if (curve.pts.empty()) {   // the last anchor goes: the curve with it
        request.any = true;
        request.lane = lane.id;
        request.curves = curves;
        request.curves.erase(request.curves.begin() + ci);
        request.label = "Delete curve";
      } else {
        request = replaced(menu_.lane, ci, curve, "Delete anchor");
      }
      selected_ = Hit();
    }
    if (curve.pts[static_cast<std::size_t>(i)].pin != CurvePin::None) {
      if (ImGui::MenuItem("Let go of the pin")) { curve.pts[static_cast<std::size_t>(i)].pin = CurvePin::None; request = replaced(menu_.lane, ci, curve, "Unpin anchor"); }
    } else {
      const CurveSnap edge = snapTick(lane, curve.pts[static_cast<std::size_t>(i)].t, true, 0, reach * 3, length);
      if (ImGui::MenuItem("Pin to the nearest pattern edge", nullptr, false, edge.pin != CurvePin::None)) {
        movePoint(curve, i, edge.t, curve.pts[static_cast<std::size_t>(i)].v);
        curve.pts[static_cast<std::size_t>(i)].pin = edge.pin;
        request = replaced(menu_.lane, ci, curve, "Pin anchor");
      }
    }
    if (i + 1 < static_cast<int>(curve.pts.size())) { ImGui::Separator(); ImGui::TextDisabled("The segment after it"); kindItems(i); }
    if (i + 1 == static_cast<int>(curve.pts.size())) {
      ImGui::Separator();
      LaneCurve grown = curve;
      const bool canExtend = extendCurve(lane, grown, i, length) >= 0;
      LaneCurve skipped = curve;
      const bool canSkip = skipCurve(lane, skipped, i, length) >= 0;
      if (ImGui::MenuItem("Extend over the next pattern", nullptr, false, canExtend)) request = replaced(menu_.lane, ci, grown, "Extend curve");
      if (ImGui::MenuItem("Skip a pattern, then carry on", nullptr, false, canSkip)) request = replaced(menu_.lane, ci, skipped, "Skip a pattern");
    }
    ImGui::Separator();
  } else if (has && menu_.kind == Hit::Segment && menu_.index + 1 < static_cast<int>(curve.pts.size())) {
    ImGui::TextDisabled("This segment");
    kindItems(menu_.index);
    ImGui::Separator();
  }

  // the lane's curves
  if (!all.empty() && ImGui::BeginMenu("Add a curve for")) {
    for (const CurveTarget& t : all) {
      bool taken = false;
      for (const Json& c : curves) if (c.value("target", std::string()) == t.id) taken = true;
      if (!ImGui::MenuItem(t.label.c_str(), nullptr, false, !taken)) continue;
      const CurveSnap snap = snapTick(lane, menu_.tick, true, cell_, reach, length);
      LaneCurve made = newLaneCurve(t.id, snap.t, menu_.value);
      made.pts[0].pin = snap.pin;
      request.any = true;
      request.lane = lane.id;
      request.curves = curves.is_array() ? curves : Json::array();
      request.curves.push_back(writeLaneCurve(made));
      request.label = "Add curve";
      shown_[lane.id] = static_cast<int>(request.curves.size()) - 1;
    }
    ImGui::EndMenu();
  }
  if (has && curves.size() > 1 && ImGui::BeginMenu("Show")) {
    for (std::size_t i = 0; i < curves.size(); i++)
      if (ImGui::MenuItem(labelOf(curves[i].value("target", std::string())).c_str(), nullptr, static_cast<int>(i) == ci)) { shown_[lane.id] = static_cast<int>(i); selected_ = Hit(); }
    ImGui::EndMenu();
  }
  if (has) {
    const bool on = !(curve.raw.contains("on") && curve.raw["on"].is_boolean() && !curve.raw["on"].get<bool>());
    if (ImGui::MenuItem(("Curve on: " + labelOf(curve.target())).c_str(), nullptr, on)) {
      curve.raw["on"] = !on;
      request = replaced(menu_.lane, ci, curve, on ? "Switch curve off" : "Switch curve on");
    }
    if (ImGui::MenuItem("Delete this curve")) {
      request.any = true;
      request.lane = lane.id;
      request.curves = curves;
      request.curves.erase(request.curves.begin() + ci);
      request.label = "Delete curve";
      selected_ = Hit();
    }
  } else if (all.empty()) {
    ImGui::TextDisabled("This machine has nothing to draw a curve for");
  }
  return request;
}

}  // namespace wax::app

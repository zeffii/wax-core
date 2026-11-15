#include "app/envelope_editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app/theme.h"
#include "app/widgets.h"
#include "edit/spline_ops.h"

namespace wax::app {

namespace {

using bezier::Anchor;
using bezier::Side;
using bezier::Spline;

constexpr ImU32 kPink = IM_COL32(236, 92, 152, 255);
constexpr ImU32 kArm = IM_COL32(76, 186, 224, 255);
constexpr ImU32 kInk = IM_COL32(205, 214, 222, 255);
constexpr double kLogK = 50;
constexpr double kMinStep = 0.004;

double clampTo(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }
ImU32 withAlpha(ImU32 colour, float alpha) { return (colour & 0x00FFFFFF) | (static_cast<ImU32>(alpha * 255) << 24); }

// How the drawing's time (0 to 1) maps to the width: straight, or log so that the start gets most of the room.
double toView(const EnvelopeView& v, double x) { return v.logTime ? std::log(1 + kLogK * x) / std::log(1 + kLogK) : x; }
double fromView(const EnvelopeView& v, double u) { return v.logTime ? (std::exp(u * std::log(1 + kLogK)) - 1) / kLogK : u; }
double viewSlope(const EnvelopeView& v, double x) { return v.logTime ? kLogK / ((1 + kLogK * x) * std::log(1 + kLogK)) : 1; }

double snapped(const EnvelopeView& v, double value, bool on) { return v.snapAt && on ? std::round(value * v.snapAt) / v.snapAt : value; }

struct Geometry {
  ImVec2 origin;
  float pl, pr, pt, pb, pw, ph;
  float x(const EnvelopeView& v, double t) const { return origin.x + pl + static_cast<float>(toView(v, t)) * pw; }
  float y(double level) const { return origin.y + pt + static_cast<float>(1 - level) * ph; }
};

// round times (ms) for the ruler of log time: 1, 2, 5, 10, 20, 50 ... up to the end of the drawing
std::vector<double> msMarks(double total) {
  std::vector<double> marks;
  for (double decade = 0.1; decade <= total; decade *= 10)
    for (const double m : {1.0, 2.0, 5.0}) {
      const double v = std::round(decade * m * 100) / 100;
      if (v <= total && v >= 1 && (marks.empty() || v > marks.back())) marks.push_back(v);
    }
  return marks;
}

std::string timeText(double ms) {
  char text[24];
  if (ms >= 1000) std::snprintf(text, sizeof text, "%gs", std::round(ms) / 1000);
  else std::snprintf(text, sizeof text, "%g", std::round(ms * 10) / 10);
  return text;
}

}  // namespace

EnvelopeResult drawEnvelopeCanvas(EnvelopeView& view, const char* id, Spline& spline, const EnvelopeSpec& spec, ImVec2 size) {
  EnvelopeResult result;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImDrawList* draw = ImGui::GetWindowDrawList();
  Geometry g;
  g.origin = ImGui::GetCursorScreenPos();
  g.pl = 48 * scale; g.pr = 12 * scale; g.pt = 18 * scale; g.pb = 20 * scale;
  g.pw = std::max(20.0f, size.x - g.pl - g.pr);
  g.ph = std::max(20.0f, size.y - g.pt - g.pb);
  const float grab = 8 * scale;

  ImGui::PushID(id);
  ImGui::InvisibleButton("##canvas", size);
  ImGui::PopID();
  const bool hovered = ImGui::IsItemHovered();
  const bool pressed = ImGui::IsItemActivated();
  const bool held = ImGui::IsItemActive();
  ImGuiIO& io = ImGui::GetIO();

  const auto handleOf = [&](std::size_t i, Side side) {
    const bezier::Handles h = bezier::handlesOf(spline, i);
    const double* q = side == Side::Left ? h.l : h.r;
    return ImVec2(g.x(view, spline[i].x + q[0]), g.y(spline[i].y + q[1]));
  };
  // the handles on show: both of the chosen anchor's (those it has a neighbour for), and the first anchor's own when it keeps one
  struct Shown { std::size_t i; Side side; };
  std::vector<Shown> shown;
  if (view.selected >= 0 && view.selected < static_cast<int>(spline.size())) {
    const std::size_t s = static_cast<std::size_t>(view.selected);
    if (s > 0) shown.push_back({s, Side::Left});
    if (s + 1 < spline.size()) shown.push_back({s, Side::Right});
  }
  if (spec.firstHandle) {
    bool has = false;
    for (const Shown& h : shown) if (h.i == 0) has = true;
    if (!has) shown.push_back({0, Side::Right});
  }

  const ImVec2 mouse = io.MousePos;
  const double mx = clampTo(fromView(view, clampTo((mouse.x - g.origin.x - g.pl) / g.pw, 0, 1)), 0, 1);
  const double my = clampTo(1 - (mouse.y - g.origin.y - g.pt) / g.ph, 0, 1);
  const auto near = [&](ImVec2 p) { return std::hypot(mouse.x - p.x, mouse.y - p.y) <= grab; };

  // what is under the mouse
  int kind = 0, index = -1;
  bezier::Nearest onCurve;
  if (hovered || view.dragKind) {
    for (const Shown& h : shown) if (near(handleOf(h.i, h.side))) { kind = h.side == Side::Left ? 2 : 3; index = static_cast<int>(h.i); break; }
    if (!kind) {
      for (std::size_t i = 0; i < spline.size(); i++) if (near(ImVec2(g.x(view, spline[i].x), g.y(spline[i].y)))) { kind = 1; index = static_cast<int>(i); break; }
    }
    if (!kind) {
      const double scaleXY[2] = {g.pw * viewSlope(view, mx), g.ph};
      onCurve = bezier::nearestOnSpline(spline, mx, my, scaleXY);
      if (onCurve.segment >= 0 && onCurve.d <= 6 * scale) { kind = 4; index = onCurve.segment; }
    }
  }
  if (!view.dragKind) { view.hoverKind = hovered ? kind : 0; view.hoverIndex = hovered ? index : -1; }

  const auto place = [&](bool freely) {   // where a dragged point lands: on the grid as drawn (so log time snaps to its own grid)
    double x = fromView(view, snapped(view, toView(view, mx), !freely)), y = snapped(view, my, !freely);
    return std::pair<double, double>(x, y);
  };
  const auto changed = [&](bool fold) { result.changed = true; result.fold = fold; };

  // press
  if (pressed) {
    view.focused = true;
    if (kind == 2 || kind == 3) {
      view.selected = index; view.dragKind = kind; view.dragIndex = index; view.dragStepped = false;
    } else if (kind == 1) {
      view.selected = index; view.dragKind = 1; view.dragIndex = index; view.dragStepped = false;
      view.fromX = spline[static_cast<std::size_t>(index)].x; view.fromY = spline[static_cast<std::size_t>(index)].y;
      view.fromMouseX = mx; view.fromMouseY = my;
    } else if (kind == 4) {
      view.selected = -1;   // a click on the curve itself adds nothing: double-click puts an anchor there
    } else {
      const auto [x, y] = place(io.KeyAlt);
      const int k = bezier::addAnchor(spline, x, y);
      view.selected = k; view.dragKind = 1; view.dragIndex = k; view.dragStepped = true;   // (a new anchor is a step even if it is not moved)
      view.fromX = spline[static_cast<std::size_t>(k)].x; view.fromY = spline[static_cast<std::size_t>(k)].y;
      view.fromMouseX = mx; view.fromMouseY = my;
      changed(false);
    }
  } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered) {
    view.focused = false;
  }

  // drag
  if (held && view.dragKind && !pressed && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
    auto [x, y] = place(io.KeyAlt);
    const std::size_t i = static_cast<std::size_t>(view.dragIndex);
    if (i < spline.size()) {
      if (view.dragKind == 1) {
        if (io.KeyCtrl && io.KeyShift) {   // along time only, or along level only, whichever way the mouse has gone further
          if (std::fabs(mx - view.fromMouseX) * g.pw > std::fabs(my - view.fromMouseY) * g.ph) y = view.fromY; else x = view.fromX;
        }
        bezier::moveAnchor(spline, i, x, y);
      } else {
        bezier::setHandle(spline, i, view.dragKind == 2 ? Side::Left : Side::Right, x, y, io.KeyAlt);
      }
      changed(view.dragStepped);
      view.dragStepped = true;
    }
  }
  if (!held) { view.dragKind = 0; view.dragIndex = -1; }

  // double-click and right-click
  if (hovered && !result.changed && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    if (kind == 1) {
      const std::size_t i = static_cast<std::size_t>(index);
      bezier::setKind(spline, i, bezier::kindOf(spline[i]) == "sharp" ? "smooth" : "sharp");
      view.selected = index; changed(false);
    } else if (kind == 4) {
      view.selected = bezier::insertOn(spline, onCurve); changed(false);
    }
  }
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    if (kind == 1) { if (bezier::removeAnchor(spline, static_cast<std::size_t>(index))) { view.selected = -1; changed(false); } }
    else if (kind == 4) { bezier::toggleLine(spline, static_cast<std::size_t>(index)); changed(false); }
  }

  // keys, while the canvas has them (single letters stay piano keys)
  if (view.focused && !ImGui::GetIO().WantTextInput && view.selected >= 0 && view.selected < static_cast<int>(spline.size())) {
    const std::size_t s = static_cast<std::size_t>(view.selected);
    const bool first = s == 0, last = s + 1 == spline.size();
    if ((ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) && bezier::removeAnchor(spline, s)) { view.selected = -1; changed(false); }
    else {
      const double step = io.KeyShift ? 10 : 1;
      Anchor& p = spline[s];
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { p.y = clampTo(p.y + step / g.ph, 0, 1); changed(false); }
      else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { p.y = clampTo(p.y - step / g.ph, 0, 1); changed(false); }
      else if (!first && !last && (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow))) {
        const double dir = ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? 1 : -1;
        p.x = clampTo(fromView(view, toView(view, p.x) + dir * step / g.pw), spline[s - 1].x + kMinStep, spline[s + 1].x - kMinStep);
        changed(false);
      }
    }
  }

  // ---- the drawing ----
  const float lineWidth = std::max(1.0f, scale);
  const float left = g.origin.x + g.pl, right = g.origin.x + g.pl + g.pw, top = g.y(1), bottom = g.y(0);
  const int stepsX = view.snapAt ? view.snapAt : 4, stepsY = view.snapAt ? view.snapAt : 4;
  const ImU32 faint = withAlpha(kInk, 0.09f), mid = withAlpha(kInk, 0.2f);
  std::vector<double> marks;
  if (view.logTime && spec.totalMs > 0) marks = msMarks(spec.totalMs);
  if (view.logTime && spec.totalMs > 0) for (const double m : marks) draw->AddLine(ImVec2(g.x(view, m / spec.totalMs), top), ImVec2(g.x(view, m / spec.totalMs), bottom), faint);
  else for (int k = 0; k <= stepsX; k++) draw->AddLine(ImVec2(g.x(view, static_cast<double>(k) / stepsX), top), ImVec2(g.x(view, static_cast<double>(k) / stepsX), bottom), faint);
  for (int k = 0; k <= stepsY; k++) draw->AddLine(ImVec2(left, g.y(static_cast<double>(k) / stepsY)), ImVec2(right, g.y(static_cast<double>(k) / stepsY)), faint);
  for (const double v : {0.0, 0.5, 1.0}) draw->AddLine(ImVec2(left, g.y(v)), ImVec2(right, g.y(v)), mid);
  if (!view.logTime) for (const double t : {0.0, 0.5, 1.0}) draw->AddLine(ImVec2(g.x(view, t), top), ImVec2(g.x(view, t), bottom), mid);

  const ImU32 label = withAlpha(kInk, 0.65f);
  draw->AddText(ImVec2(g.origin.x + g.pl, g.origin.y + 2 * scale), label, spec.title);
  const char* topLabel = spec.totalMs > 0 ? "100%" : "1";
  draw->AddText(ImVec2(left - 6 * scale - ImGui::CalcTextSize(topLabel).x, top - 5 * scale), label, topLabel);
  draw->AddText(ImVec2(left - 6 * scale - ImGui::CalcTextSize("0").x, bottom - 5 * scale), label, "0");
  const float axisY = bottom + 5 * scale;
  if (spec.totalMs > 0 && view.logTime) {   // the ruler: round times, closer together towards the tail, a label when there is room for it
    float lastRight = -1e9f;
    for (const double m : marks) {
      const std::string text = timeText(m);
      const float w = ImGui::CalcTextSize(text.c_str()).x, x = g.x(view, m / spec.totalMs);
      if (x - w / 2 > lastRight + 4 * scale && x + w / 2 < right + 14 * scale) { draw->AddText(ImVec2(x - w / 2, axisY), label, text.c_str()); lastRight = x + w / 2; }
    }
    const char* note = "log time, ms";
    draw->AddText(ImVec2(right - 70 * scale - ImGui::CalcTextSize(note).x, g.origin.y + 2 * scale), label, note);
  } else {
    for (const double t : {0.0, 0.5, 1.0}) {
      std::string text = spec.totalMs > 0 ? timeText(spec.totalMs * t) : std::to_string(static_cast<int>(t * 100)) + "%";
      if (spec.totalMs > 0 && t == 1) text += " ms";
      const float w = ImGui::CalcTextSize(text.c_str()).x, x = g.x(view, t);
      draw->AddText(ImVec2(t == 0 ? x : t == 1 ? x - w : x - w / 2, axisY), label, text.c_str());
    }
  }

  // the curve, and the area under it
  const auto line = bezier::walk(spline);
  const ImU32 tint = kAccent;
  {   // the area under it: one thin column per pixel, so no seams show
    const float column = std::max(1.0f, scale);
    std::size_t k = 1;
    for (float px = left; px < right; px += column) {
      const double t = fromView(view, (px + column * 0.5f - left) / g.pw);
      while (k + 1 < line.size() && line[k][0] < t) k++;
      const double x0 = line[k - 1][0], x1 = line[k][0];
      const double level = x1 > x0 ? line[k - 1][1] + (line[k][1] - line[k - 1][1]) * clampTo((t - x0) / (x1 - x0), 0, 1) : line[k][1];
      draw->AddRectFilled(ImVec2(px, g.y(level)), ImVec2(std::min(px + column, right), bottom), withAlpha(tint, 0.1f));
    }
  }
  const float curveWidth = (view.hoverKind == 4 ? 2.4f : 1.6f) * scale;
  for (std::size_t k = 1; k < line.size(); k++)
    draw->AddLine(ImVec2(g.x(view, line[k - 1][0]), g.y(line[k - 1][1])), ImVec2(g.x(view, line[k][0]), g.y(line[k][1])), tint, curveWidth);

  // the handles: a thin arm and a round end
  for (const Shown& h : shown) {
    const Anchor& p = spline[h.i];
    const ImVec2 tip = handleOf(h.i, h.side), root(g.x(view, p.x), g.y(p.y));
    const bool on = view.hoverKind == (h.side == Side::Left ? 2 : 3) && view.hoverIndex == static_cast<int>(h.i);
    draw->AddLine(root, tip, withAlpha(kArm, 0.85f), lineWidth);
    if (on) draw->AddCircleFilled(tip, 4.5f * scale, kArm); else draw->AddCircle(tip, 3.5f * scale, kArm, 0, lineWidth);
  }

  // the sustain knot: a dashed line, the part after it (the release) a little darker, and labels
  const auto knot = bezier::sustainKnot(spline);
  if (knot) {
    const float kx = g.x(view, spline[static_cast<std::size_t>(knot->index)].x);
    draw->AddRectFilled(ImVec2(kx, top), ImVec2(g.x(view, 1), bottom), withAlpha(kPink, 0.07f));
    for (float y = top; y < bottom; y += 6 * scale) draw->AddLine(ImVec2(kx, y), ImVec2(kx, std::min(bottom, y + 3 * scale)), withAlpha(kPink, 0.7f));
    draw->AddText(ImVec2(kx + 4 * scale, bottom - 15 * scale), kPink, "sustain");
    const float rw = ImGui::CalcTextSize("release").x;
    draw->AddText(ImVec2(right - rw - 2 * scale, bottom - 15 * scale), kPink, "release");
  }

  // the anchors: squares, pink when chosen, a ring on the first, a ring on the knot, a dot in an equal pair
  for (std::size_t i = 0; i < spline.size(); i++) {
    const Anchor& p = spline[i];
    const ImVec2 c(g.x(view, p.x), g.y(p.y));
    const bool chosen = static_cast<int>(i) == view.selected;
    const bool lit = chosen || (view.hoverKind == 1 && view.hoverIndex == static_cast<int>(i));
    const float r = (lit ? 4.5f : 3.5f) * scale;
    if (i == 0) draw->AddCircle(c, 8 * scale, withAlpha(kInk, 0.8f), 0, lineWidth);
    if (chosen) draw->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), kPink);
    draw->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), chosen ? kPink : kInk, 0, 0, 1.4f * scale);
    if (p.sustain) draw->AddCircle(c, 8 * scale, kPink, 0, 1.4f * scale);
    if (bezier::kindOf(p) == "equal") draw->AddCircleFilled(c, 1.3f * scale, IM_COL32(240, 150, 60, 255));
  }
  if (view.snapAt) {
    char text[16];
    std::snprintf(text, sizeof text, "snap 1/%d", view.snapAt);
    draw->AddText(ImVec2(right - ImGui::CalcTextSize(text).x, g.origin.y + 2 * scale), withAlpha(kInk, 0.5f), text);
  }
  return result;
}

EnvelopeResult drawEnvelopeTools(EnvelopeView& view, Spline& spline, const EnvelopeSpec& spec) {
  EnvelopeResult result;
  const auto act = [&](auto&& fn) { fn(); result.changed = true; result.fold = false; };
  const auto sameLine = [] { ImGui::SameLine(0, 12 * ImGui::GetStyle().FontScaleDpi); };
  const auto selected = [&]() -> int { return view.selected >= 0 && view.selected < static_cast<int>(spline.size()) ? view.selected : -1; };

  struct Named { const char* name; bezier::Shape shape; };
  for (const Named& n : {Named{"fall", bezier::Shape::Fall}, {"line", bezier::Shape::Line}, {"punch", bezier::Shape::Punch}, {"hump", bezier::Shape::Hump}, {"hold", bezier::Shape::Hold}, {"flat", bezier::Shape::Flat}}) {
    if (textButton(n.name)) act([&] { spline = bezier::shapeOf(n.shape); view.selected = -1; });
    sameLine();
  }
  ImGui::NewLine();
  for (const char* kind : {"sharp", "smooth", "equal", "free"}) {
    const int s = selected();
    if (textButton(kind, s >= 0 && (bezier::kindOf(spline[static_cast<std::size_t>(s)]) == kind || (std::string(kind) == "smooth" && bezier::kindOf(spline[static_cast<std::size_t>(s)]).empty()))) && s >= 0)
      act([&] { bezier::setKind(spline, static_cast<std::size_t>(s), kind); });
    sameLine();
  }
  if (textButton("auto")) act([&] { bezier::autoAll(spline); });
  sameLine();
  if (textButton("flip")) act([&] { bezier::flip(spline); });
  sameLine();
  if (textButton("reverse")) act([&] { bezier::reverse(spline); view.selected = -1; });
  if (spec.knot) {
    sameLine();
    const int s = selected();
    if (textButton("sustain", bezier::sustainKnot(spline).has_value()) && s >= 0) act([&] { bezier::setKnot(spline, static_cast<std::size_t>(s)); });
  }
  sameLine();
  char snap[16];
  if (view.snapAt) std::snprintf(snap, sizeof snap, "snap 1/%d", view.snapAt); else std::snprintf(snap, sizeof snap, "snap off");
  if (textButton(snap, view.snapAt != 0)) { static const int kSteps[] = {0, 8, 16, 32}; int k = 0; while (kSteps[k] != view.snapAt) k++; view.snapAt = kSteps[(k + 1) % 4]; }
  if (spec.totalMs > 0) {
    sameLine();
    if (textButton(view.logTime ? "time: log" : "time: linear", view.logTime)) view.logTime = !view.logTime;
  }
  return result;
}

}  // namespace wax::app

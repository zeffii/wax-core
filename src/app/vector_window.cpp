#include "app/vector_window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include "app/theme.h"
#include "app/widgets.h"
#include "nodes/vectorcore.h"

namespace wax::app {
namespace {

// ---- the pad's measures, in web pixels (all scaled by the UI scale when drawn)
constexpr float kPadW = 240, kCx = 120, kCy = 112, kR = 84, kBarX0 = 22, kBarX1 = kPadW - 22, kBarY = 238, kPadHPath = 262, kPadHPlain = 212, kGrab = 9;
// ---- the wave sequences'
constexpr float kSeqW = 520, kGutter = 22, kRight = 8, kRuler = 16, kRow = 46, kEdge = 5;
constexpr int kMinTicks = 8, kPixelsPerWave = 3;

const ImU32 kLaneColour[4] = {IM_COL32(45, 150, 225, 255), IM_COL32(240, 150, 26, 255), IM_COL32(48, 166, 100, 255), IM_COL32(231, 106, 76, 255)};   // A B C D
const char* const kLaneName[4] = {"A", "B", "C", "D"};
const ImU32 kOrange = IM_COL32(240, 160, 48, 255);

const GraphNode* findNode(const Project& project, const std::string& instrumentId, const std::string& nodeId) {
  for (const Instrument& instrument : project.instruments) {
    if (instrument.id != instrumentId) continue;
    for (const GraphNode& node : instrument.graph.nodes) if (node.id == nodeId) return &node;
  }
  return nullptr;
}

bool hasPad(const std::string& type) { return type == "compass" || type == "vecpath" || type == "vecmix"; }
bool hasPath(const std::string& type) { return type != "vecmix"; }
bool hasSequences(const std::string& type) { return type == "compass" || type == "waveseq"; }

double round3(double v) { return std::round(v * 1000) / 1000; }

// One reading of everything the window draws and edits, taken from the node and the live parameters.
struct View {
  const GraphNode* node = nullptr;
  const Engine* engine = nullptr;
  std::string instrument;
  vec::Path path;
  std::vector<vec::Lane> lanes;
  int laneCount = 0;
  double param(const char* name, double fallback) const {
    const std::string address = instrument + "." + node->id + "." + name;
    return engine->registry().slotOf(address) >= 0 ? engine->registry().get(address) : node->param(name, fallback);
  }
};

Json pathJson(const vec::Path& path) {
  Json out = Json::array();
  for (const vec::PathPoint& p : path) out.push_back({{"x", round3(p.x)}, {"y", round3(p.y)}, {"d", round3(p.d)}});
  return out;
}

Json lanesJson(const std::vector<vec::Lane>& lanes) {
  Json out = Json::array();
  for (const vec::Lane& lane : lanes) {
    Json steps = Json::array();
    for (const vec::Step& s : lane) steps.push_back({{"w", s.w}, {"len", s.len}, {"fade", std::round(s.fade * 100) / 100}, {"semi", s.semi}, {"lvl", std::round(s.lvl * 100) / 100}});
    out.push_back(steps);
  }
  return out;
}

// What a change to the window needs written: the one edit is folded into the drag before it.
struct Writer {
  Document& doc;
  VectorWindow& window;
  bool changed = false;
  void data(const char* key, const Json& value, const char* label) { doc.apply(std::make_unique<SetNodeData>(window.instrument, window.node, key, value, label), window.dragging); window.dragging = true; changed = true; }
  void param(const char* name, double value) { doc.apply(std::make_unique<SetParam>(window.instrument, window.node, name, value), window.dragging); window.dragging = true; changed = true; }
};

// ------------------------------------------------------------------------------------------------------------------------ the diamond

struct Stretch { int to; double length; };

void drawPad(VectorWindow& w, const View& view, Writer& write, float scale) {
  const bool withPath = hasPath(view.node->type);
  const bool closed = withPath && jsRoundInt(view.param("pmode", 0)) == vec::Loop && view.path.size() > 1;
  const float height = (withPath ? kPadHPath : kPadHPlain) * scale;
  const ImVec2 o = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("pad", ImVec2(kPadW * scale, height));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 ink = kText;
  const auto X = [&](double x) { return o.x + static_cast<float>(kCx + x * kR) * scale; };
  const auto Y = [&](double y) { return o.y + static_cast<float>(kCy - y * kR) * scale; };
  const auto P = [&](double x, double y) { return ImVec2(X(x), Y(y)); };
  const double sx = view.param("x", 0), sy = view.param("y", 0);

  // the time bar's marks: where each stretch of one trip ends, in web pixels
  std::vector<Stretch> list;
  if (withPath) {
    for (std::size_t i = 1; i < view.path.size(); i++) list.push_back({static_cast<int>(i), view.path[i].d});
    if (closed) list.push_back({0, view.path[0].d});
  }
  double total = 0;
  for (const Stretch& s : list) total += s.length;
  if (total <= 0) total = 1;
  std::vector<float> marks;
  {
    double at = 0;
    for (const Stretch& s : list) { at += s.length; marks.push_back(static_cast<float>(kBarX0 + (at / total) * (kBarX1 - kBarX0))); }
  }

  // ---- the diamond, its corners with the share each source has at the +
  dl->AddQuad(P(-1, 0), P(0, 1), P(1, 0), P(0, -1), withAlpha(ink, 100), 1.0f);
  dl->AddLine(P(-1, 0), P(1, 0), withAlpha(ink, 30));
  dl->AddLine(P(0, 1), P(0, -1), withAlpha(ink, 30));
  float weights[4];
  vec::diamondWeights(vec::clamp(sx, -1, 1), vec::clamp(sy, -1, 1), weights);
  static const double corner[4][2] = {{-1, 0}, {0, 1}, {1, 0}, {0, -1}};
  for (int k = 0; k < 4; k++) {
    const float lx = X(corner[k][0] * 1.2), ly = Y(corner[k][1] * 1.17);
    const ImVec2 size = ImGui::CalcTextSize(kLaneName[k]);
    dl->AddText(ImVec2(lx - size.x / 2, ly - size.y / 2), kLaneColour[k], kLaneName[k]);
    dl->AddRectFilled(ImVec2(lx - 10 * scale, ly + 9 * scale), ImVec2(lx + 10 * scale, ly + 11 * scale), withAlpha(ink, 60));
    dl->AddRectFilled(ImVec2(lx - 10 * scale, ly + 9 * scale), ImVec2(lx + (-10 + 20 * weights[k]) * scale, ly + 11 * scale), kLaneColour[k]);
  }
  if (withPath) {   // the path: points joined by thin lines, the first ringed, the way back dashed
    for (std::size_t i = 1; i < view.path.size(); i++) dl->AddLine(P(view.path[i - 1].x, view.path[i - 1].y), P(view.path[i].x, view.path[i].y), withAlpha(ink, 215), 1.5f);
    if (closed) dl->AddLine(P(view.path.back().x, view.path.back().y), P(view.path[0].x, view.path[0].y), withAlpha(ink, 120));
    for (std::size_t i = 0; i < view.path.size(); i++) {
      dl->AddCircleFilled(P(view.path[i].x, view.path[i].y), 3.5f * scale, static_cast<int>(i) == w.selectedPoint ? kOrange : ink);
      if (i == 0) dl->AddCircle(P(view.path[0].x, view.path[0].y), 7 * scale, withAlpha(ink, 180));
    }
    // the time bar: one trip from left to right, a mark where each point is reached
    const ImVec2 b0(o.x + kBarX0 * scale, o.y + kBarY * scale), b1(o.x + kBarX1 * scale, o.y + kBarY * scale);
    dl->AddLine(b0, b1, withAlpha(ink, 115));
    ImFont* font = ImGui::GetFont();
    const float small = ImGui::GetFontSize() * 0.85f;
    const double ms = view.param("pathms", 2000);
    char text[32];
    if (ms >= 1000) std::snprintf(text, sizeof text, ms >= 10000 ? "%.0f s" : "%.1f s", ms / 1000); else std::snprintf(text, sizeof text, "%d ms", static_cast<int>(std::lround(ms)));
    dl->AddText(font, small, ImVec2(b0.x, b0.y - 18 * scale), withAlpha(ink, 190), "one trip");
    dl->AddText(font, small, ImVec2(b1.x - font->CalcTextSizeA(small, FLT_MAX, 0, text).x, b0.y - 18 * scale), withAlpha(ink, 190), text);
    dl->AddText(font, small, ImVec2(b0.x - 3 * scale, b0.y + 7 * scale), withAlpha(ink, 190), "1");
    dl->AddLine(ImVec2(b0.x, b0.y - 4 * scale), ImVec2(b0.x, b0.y + 5 * scale), withAlpha(ink, 180));
    for (std::size_t s = 0; s < marks.size(); s++) {
      const float x = std::round(o.x + marks[s] * scale);
      const ImU32 colour = list[s].to == w.selectedPoint ? kOrange : ink;
      dl->AddLine(ImVec2(x, b0.y - 4 * scale), ImVec2(x, b0.y + 5 * scale), colour);
      dl->AddCircleFilled(ImVec2(x, b0.y), 2.5f * scale, colour);
      const std::string number = std::to_string(list[s].to + 1);
      dl->AddText(font, small, ImVec2(x - font->CalcTextSizeA(small, FLT_MAX, 0, number.c_str()).x / 2, b0.y + 7 * scale), withAlpha(ink, 190), number.c_str());
    }
  }
  {   // the +: where the mix is shifted to (a drag anywhere moves it when there is no path)
    double px, py;
    vec::insideDiamond(vec::clamp(sx, -1, 1), vec::clamp(sy, -1, 1), px, py);
    const float arm = (withPath ? 5 : 7) * scale;
    const ImVec2 c = P(px, py);
    dl->AddLine(ImVec2(c.x - arm, c.y), ImVec2(c.x + arm, c.y), kOrange, 1.5f);
    dl->AddLine(ImVec2(c.x, c.y - arm), ImVec2(c.x, c.y + arm), kOrange, 1.5f);
    if (!withPath) dl->AddCircle(c, arm + 4 * scale, kOrange, 0, 1.5f);
  }

  // ---- the mouse
  const ImGuiIO& io = ImGui::GetIO();
  const float mx = (io.MousePos.x - o.x) / scale, my = (io.MousePos.y - o.y) / scale;
  const auto near = [&](float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by) <= kGrab; };
  const auto sxOf = [&](double x) { return static_cast<float>(kCx + x * kR); };
  const auto syOf = [&](double y) { return static_cast<float>(kCy - y * kR); };
  const auto pointAt = [&]() { for (std::size_t i = 0; i < view.path.size(); i++) if (near(mx, my, sxOf(view.path[i].x), syOf(view.path[i].y))) return static_cast<int>(i); return -1; };
  const auto onShift = [&]() { return near(mx, my, sxOf(vec::clamp(sx, -1, 1)), syOf(vec::clamp(sy, -1, 1))); };
  const auto fromScreen = [&](double& x, double& y) { vec::insideDiamond(vec::clamp((mx - kCx) / kR, -1, 1), vec::clamp((kCy - my) / kR, -1, 1), x, y); };
  vec::Path path = view.path;

  if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    w.grab = 0;
    if (!withPath) w.grab = 2;
    else {
      int mark = -1;
      for (std::size_t s = 0; s < marks.size(); s++) if (std::fabs(mx - marks[s]) <= 6 && std::fabs(my - kBarY) <= 10) { mark = static_cast<int>(s); break; }
      const int hit = pointAt();
      if (mark >= 0) { w.grab = 3; w.grabIndex = mark; w.selectedPoint = list[static_cast<std::size_t>(mark)].to; }
      else if (hit >= 0) { w.grab = 1; w.grabIndex = hit; w.selectedPoint = hit; }
      else if (onShift()) w.grab = 2;
      else if (std::fabs(mx - kCx) + std::fabs(my - kCy) <= kR + 6 && static_cast<int>(path.size()) < vec::kMaxPoints) {   // empty space: a new point after the chosen one
        double x, y;
        fromScreen(x, y);
        const int at = std::min(w.selectedPoint, static_cast<int>(path.size()) - 1) + 1;
        path.insert(path.begin() + at, {x, y, 1});
        w.selectedPoint = at; w.grab = 1; w.grabIndex = at;
        write.data("path", pathJson(path), "path");
      }
    }
  }
  if (w.grab != 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (w.grab == 2) { double x, y; fromScreen(x, y); write.param("x", round3(x)); write.param("y", round3(y)); }
    else if (w.grab == 1 && w.grabIndex < static_cast<int>(path.size())) {
      double x, y;
      fromScreen(x, y);
      if (path[static_cast<std::size_t>(w.grabIndex)].x != x || path[static_cast<std::size_t>(w.grabIndex)].y != y) {
        path[static_cast<std::size_t>(w.grabIndex)].x = x; path[static_cast<std::size_t>(w.grabIndex)].y = y;
        write.data("path", pathJson(path), "path");
      }
    } else if (w.grab == 3 && w.grabIndex < static_cast<int>(list.size()) - 1) {   // a time mark: time moves between the stretch and the next, their sum stays
      const int s = w.grabIndex;
      const float start = s == 0 ? kBarX0 : marks[static_cast<std::size_t>(s - 1)], end = marks[static_cast<std::size_t>(s + 1)];
      const double both = list[static_cast<std::size_t>(s)].length + list[static_cast<std::size_t>(s + 1)].length, pixels = std::max(1.0f, end - start);
      const double first = vec::clamp((mx - start) / pixels * both, both * 0.03, both * 0.97);
      path[static_cast<std::size_t>(list[static_cast<std::size_t>(s)].to)].d = first;
      path[static_cast<std::size_t>(list[static_cast<std::size_t>(s + 1)].to)].d = both - first;
      write.data("path", pathJson(path), "path timing");
    }
  }
  if (ImGui::IsItemHovered() && withPath && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && path.size() > 1) {   // right-click a point: remove it
    const int hit = pointAt();
    if (hit >= 0) { path.erase(path.begin() + hit); w.selectedPoint = std::min(hit, static_cast<int>(path.size()) - 1); w.dragging = false; write.data("path", pathJson(path), "remove path point"); w.dragging = false; }
  }
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && onShift()) { w.dragging = false; write.param("x", 0); write.param("y", 0); w.dragging = false; }

  if (withPath) {   // the buttons under the pad
    const auto setPath = [&](const vec::Path& next, const char* label) { w.selectedPoint = 0; w.dragging = false; write.data("path", pathJson(next), label); w.dragging = false; };
    if (textButton("tour")) setPath(vec::defaultPath(), "path tour");
    ImGui::SameLine(0, 12 * scale);
    if (textButton("reverse")) {   // the same trip the other way round: each stretch keeps its time
      vec::Path back(view.path.rbegin(), view.path.rend());
      vec::Path next = back;
      for (std::size_t i = 0; i < back.size(); i++) next[i].d = back[(i + back.size() - 1) % back.size()].d;
      setPath(next, "reverse path");
    }
    ImGui::SameLine(0, 12 * scale);
    if (textButton("still")) setPath({{0, 0, 1}}, "path still");
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "click adds a point · drag moves it · right-click removes it");
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "drag a mark on the time bar to share out time");
  }
}

// ------------------------------------------------------------------------------------------------------------------------ the wave sequences

int laneTicks(const vec::Lane& lane) { int t = 0; for (const vec::Step& s : lane) t += s.len; return t; }

void drawWaveCycle(ImDrawList* dl, int wave, float x0, float x1, float mid, float height, ImU32 colour, float thickness) {
  const std::vector<float>& frames = bundledWaveFrames();
  const int span = std::max(2, static_cast<int>(std::lround(x1 - x0)));
  static std::vector<ImVec2> line;
  line.clear();
  for (int px = 0; px <= span; px++) {
    const int at = std::min(vec::kFrame - 1, static_cast<int>(static_cast<float>(px) / static_cast<float>(span) * vec::kFrame));
    line.push_back(ImVec2(x0 + static_cast<float>(px) / static_cast<float>(span) * (x1 - x0), mid - frames[static_cast<std::size_t>(wave) * vec::kFrame + static_cast<std::size_t>(at)] * height));
  }
  dl->AddPolyline(line.data(), static_cast<int>(line.size()), colour, 0, thickness);
}

void drawSequences(VectorWindow& w, const View& view, Writer& write, float scale) {
  const int count = view.laneCount;
  const float height = (kRuler + count * kRow + 4) * scale;
  const ImVec2 o = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("sequences", ImVec2(kSeqW * scale, height));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 ink = kText;
  std::vector<vec::Lane> lanes = view.lanes;
  w.laneChosen = std::clamp(w.laneChosen, 0, count - 1);
  w.stepChosen = std::clamp(w.stepChosen, 0, static_cast<int>(lanes[static_cast<std::size_t>(w.laneChosen)].size()) - 1);
  int shown = kMinTicks;
  for (const vec::Lane& lane : lanes) shown = std::max(shown, laneTicks(lane));
  const float tickWidth = (kSeqW - kGutter - kRight) / static_cast<float>(shown);
  const auto boxes = [&](int lane) { std::vector<std::pair<float, float>> b; float at = kGutter; for (const vec::Step& s : lanes[static_cast<std::size_t>(lane)]) { b.push_back({at, at + static_cast<float>(s.len) * tickWidth}); at += static_cast<float>(s.len) * tickWidth; } return b; };
  const auto px = [&](float x) { return o.x + x * scale; };
  const auto py = [&](float y) { return o.y + y * scale; };
  ImFont* font = ImGui::GetFont();
  const float small = ImGui::GetFontSize() * 0.85f;

  {   // the ruler: song ticks
    const int every = shown > 64 ? 16 : shown > 32 ? 8 : 4;
    for (int tick = 0; tick <= shown; tick++) {
      const float x = std::round(px(kGutter + static_cast<float>(tick) * tickWidth));
      const bool major = tick % every == 0;
      dl->AddLine(ImVec2(x, py(kRuler - (major ? 5 : 2))), ImVec2(x, py(kRuler)), withAlpha(ink, major ? 150 : 75));
      if (major && tick < shown) dl->AddText(font, small, ImVec2(x + 3 * scale, py(1)), withAlpha(ink, 190), std::to_string(tick).c_str());
    }
  }
  for (int lane = 0; lane < count; lane++) {
    const ImU32 colour = kLaneColour[lane % 4];
    const float top = kRuler + static_cast<float>(lane) * kRow, mid = top + kRow / 2, tall = kRow / 2 - 9;
    const auto& steps = lanes[static_cast<std::size_t>(lane)];
    const auto placed = boxes(lane);
    if (count > 1) dl->AddText(ImVec2(px(4), py(mid) - ImGui::GetFontSize() / 2), colour, kLaneName[lane]);
    dl->AddLine(ImVec2(px(kGutter), std::round(py(mid))), ImVec2(px(kSeqW - kRight), std::round(py(mid))), withAlpha(ink, 36));
    for (std::size_t i = 0; i < steps.size(); i++) {
      const vec::Step& s = steps[i];
      const float x0 = placed[i].first, x1 = placed[i].second;
      const bool chosen = w.laneChosen == lane && w.stepChosen == static_cast<int>(i);
      if (steps.size() > 1 && s.fade > 0) {   // the crossfade in: a small wedge along the top of the step
        const float reach = std::max(2.0f, static_cast<float>(s.fade) * (x1 - x0));
        dl->AddTriangleFilled(ImVec2(px(x0 + 1), py(top + 7)), ImVec2(px(x0 + reach), py(top + 7)), ImVec2(px(x0 + reach), py(top + 2)), withAlpha(colour, 100));
      }
      drawWaveCycle(dl, s.w, px(x0 + 2), px(x1 - 2), py(mid), tall * scale * static_cast<float>(std::max(0.08, s.lvl)), colour, (chosen ? 1.8f : 1.2f) * std::max(1.0f, scale));
      dl->AddLine(ImVec2(std::round(px(x1)), py(mid - 4)), ImVec2(std::round(px(x1)), py(mid + 5)), withAlpha(ink, 115));   // where the step ends
      if (s.semi != 0 && x1 - x0 > 18) { char t[16]; std::snprintf(t, sizeof t, "%+d", s.semi); dl->AddText(font, small, ImVec2(px(x0 + 3), py(top + kRow - 15)), withAlpha(ink, 180), t); }
      if (chosen) dl->AddRectFilled(ImVec2(px(x0 + 1), py(top + kRow - 3)), ImVec2(px(x0 + 1 + std::max(2.0f, x1 - x0 - 2)), py(top + kRow - 1)), kOrange);
    }
    if (steps.size() > 1 && jsRoundInt(view.param("smode", 0)) == vec::Loop) {   // a lane that loops, drawn again (faint) after its end
      const float length = static_cast<float>(laneTicks(steps)) * tickWidth;
      for (float start = kGutter + length; start < kSeqW - kRight - 4; start += length) {
        for (std::size_t i = 0; i < steps.size(); i++) {
          const float x0 = start + (placed[i].first - kGutter), x1 = std::min(kSeqW - kRight, start + (placed[i].second - kGutter));
          if (x1 - x0 < 4) continue;
          drawWaveCycle(dl, steps[i].w, px(x0 + 2), px(x1 - 2), py(mid), tall * scale * static_cast<float>(std::max(0.08, steps[i].lvl)), withAlpha(colour, 56), 1.0f);
        }
      }
    }
  }

  // ---- the mouse: a click chooses a step, a vertical drag picks another wave, a drag of the right edge sets the length
  const ImGuiIO& io = ImGui::GetIO();
  const float mx = (io.MousePos.x - o.x) / scale, my = (io.MousePos.y - o.y) / scale;
  struct Hit { int lane = -1, step = -1; bool edge = false; };
  const auto stepAt = [&]() {
    Hit hit;
    const int lane = static_cast<int>(std::floor((my - kRuler) / kRow));
    if (lane < 0 || lane >= count || my < kRuler) return hit;
    const auto placed = boxes(lane);
    for (std::size_t i = 0; i < placed.size(); i++) {
      if (std::fabs(mx - placed[i].second) <= kEdge) return Hit{lane, static_cast<int>(i), true};
      if (mx >= placed[i].first && mx < placed[i].second) return Hit{lane, static_cast<int>(i), false};
    }
    return hit;
  };
  if (ImGui::IsItemHovered() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) ImGui::SetMouseCursor(stepAt().edge ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
  if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const Hit hit = stepAt();
    w.grab = 0;
    if (hit.lane >= 0) {
      w.laneChosen = hit.lane; w.stepChosen = hit.step;
      const vec::Step& s = lanes[static_cast<std::size_t>(hit.lane)][static_cast<std::size_t>(hit.step)];
      if (hit.edge) { w.grab = 5; w.grabX0 = boxes(hit.lane)[static_cast<std::size_t>(hit.step)].first; w.grabWidth = tickWidth; }   // (the scale from when the drag began, so the edge stays under the mouse)
      else { w.grab = 4; w.grabFrom = my; w.grabWave = s.w; }
    }
  }
  if ((w.grab == 4 || w.grab == 5) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    vec::Step& s = lanes[static_cast<std::size_t>(w.laneChosen)][static_cast<std::size_t>(w.stepChosen)];
    const int before = s.w * 1000 + s.len;
    if (w.grab == 5) s.len = std::clamp(static_cast<int>(std::lround((mx - w.grabX0) / w.grabWidth)), 1, vec::kMaxLen);
    else s.w = std::clamp(w.grabWave + static_cast<int>(std::lround((w.grabFrom - my) / kPixelsPerWave)), 0, 255);
    if (s.w * 1000 + s.len != before) write.data("lanes", lanesJson(lanes), "wave steps");
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) w.grab = 0;

  // ---- the chosen step's settings, in words, and the buttons that rearrange the lane
  vec::Lane& lane = lanes[static_cast<std::size_t>(w.laneChosen)];
  vec::Step& step = lane[static_cast<std::size_t>(w.stepChosen)];
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kLaneColour[w.laneChosen % 4]), "%s%d", count > 1 ? kLaneName[w.laneChosen] : "step ", w.stepChosen + 1);
  const float field = 80 * scale;
  bool edited = false;
  int wave = step.w, length = step.len, semi = step.semi;
  float fade = static_cast<float>(step.fade), level = static_cast<float>(step.lvl) * 100;
  const auto labelled = [&](const char* label) { ImGui::SameLine(0, 14 * scale); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "%s", label); ImGui::SameLine(0, 4 * scale); ImGui::SetNextItemWidth(field); };
  labelled("wave");
  if (ImGui::DragInt("##wave", &wave, 0.3f, 0, 255, "%03d")) { step.w = std::clamp(wave, 0, 255); edited = true; }
  labelled("length");
  if (ImGui::DragInt("##length", &length, 0.15f, 1, vec::kMaxLen, "%d t")) { step.len = std::clamp(length, 1, vec::kMaxLen); edited = true; }
  labelled("fade");
  if (ImGui::DragFloat("##fade", &fade, 0.01f, 0, 1, "%.2f")) { step.fade = std::clamp<double>(fade, 0, 1); edited = true; }
  labelled("pitch");
  if (ImGui::DragInt("##pitch", &semi, 0.15f, -24, 24, "%+d st")) { step.semi = std::clamp(semi, -24, 24); edited = true; }
  labelled("level");
  if (ImGui::DragFloat("##level", &level, 1.0f, 0, 100, "%.0f%%")) { step.lvl = std::clamp<double>(level / 100, 0, 1); edited = true; }
  if (edited) write.data("lanes", lanesJson(lanes), "wave step");
  const auto rearrange = [&](const char* label, const char* done, const std::function<void()>& edit) {
    if (!textButton(label)) return;
    edit();
    w.stepChosen = std::clamp(w.stepChosen, 0, static_cast<int>(lane.size()) - 1);
    w.dragging = false;
    write.data("lanes", lanesJson(lanes), done);
    w.dragging = false;
  };
  const std::size_t at = static_cast<std::size_t>(w.stepChosen);
  rearrange("add step", "add wave step", [&]() { if (static_cast<int>(lane.size()) < vec::kMaxSteps) { lane.insert(lane.begin() + static_cast<std::ptrdiff_t>(at) + 1, step); w.stepChosen++; } });
  ImGui::SameLine(0, 12 * scale);
  rearrange("remove", "remove wave step", [&]() { if (lane.size() > 1) lane.erase(lane.begin() + static_cast<std::ptrdiff_t>(at)); });
  ImGui::SameLine(0, 12 * scale);
  rearrange("earlier", "move wave step", [&]() { if (at > 0) { std::swap(lane[at], lane[at - 1]); w.stepChosen--; } });
  ImGui::SameLine(0, 12 * scale);
  rearrange("later", "move wave step", [&]() { if (at + 1 < lane.size()) { std::swap(lane[at], lane[at + 1]); w.stepChosen++; } });
  ImGui::SameLine(0, 12 * scale);
  rearrange("one step", "single wave step", [&]() { const int wv = step.w; lane.assign(1, vec::Step()); lane[0].w = wv; w.stepChosen = 0; });
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "click a step · drag up or down for another wave · drag its right edge for its length");
}

}  // namespace

bool hasVectorWindow(const std::string& nodeType) { return hasPad(nodeType) || hasSequences(nodeType); }

void showVector(VectorWindow& window, const std::string& instrument, const std::string& node) {
  window.open = true; window.instrument = instrument; window.node = node;
  window.selectedPoint = 0; window.laneChosen = 0; window.stepChosen = 0; window.grab = 0; window.dragging = false;
}

bool drawVectorWindow(VectorWindow& window, Document& doc, const Engine& engine) {
  if (!window.open) return false;
  const GraphNode* node = findNode(doc.project(), window.instrument, window.node);
  if (!node || !hasVectorWindow(node->type)) { window.open = false; return false; }
  View view;
  view.node = node; view.engine = &engine; view.instrument = window.instrument;
  const Json* storedPath = node->data.is_object() && node->data.contains("path") ? &node->data["path"] : nullptr;
  const Json* storedLanes = node->data.is_object() && node->data.contains("lanes") ? &node->data["lanes"] : nullptr;
  view.path = vec::cleanPath(storedPath);
  view.laneCount = node->type == "compass" ? 4 : 1;
  view.lanes = vec::cleanLanes(storedLanes, view.laneCount);
  window.selectedPoint = std::clamp(window.selectedPoint, 0, static_cast<int>(view.path.size()) - 1);

  const float scale = ImGui::GetStyle().FontScaleDpi;
  const float width = ((hasPad(node->type) ? kPadW + 24 : 0) + (hasSequences(node->type) ? kSeqW + 24 : 0) + 24) * scale;
  ImGui::SetNextWindowSize(ImVec2(std::min(width, 1150.0f * scale), (hasPad(node->type) && hasPath(node->type) ? 430 : 330) * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(240 * scale, 100 * scale), ImGuiCond_FirstUseEver);
  const std::string title = std::string(node->type == "compass" ? "Compass" : node->type == "waveseq" ? "Wave sequence" : node->type == "vecpath" ? "Vector path" : "Vector mix") + "  " + window.instrument + "." + window.node + "###vector";
  Writer write{doc, window};
  if (ImGui::Begin(title.c_str(), &window.open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_HorizontalScrollbar)) {
    if (hasPad(node->type)) {
      ImGui::BeginGroup();
      drawPad(window, view, write, scale);
      ImGui::EndGroup();
      if (hasSequences(node->type)) ImGui::SameLine(0, 24 * scale);
    }
    if (hasSequences(node->type)) {
      ImGui::BeginGroup();
      drawSequences(window, view, write, scale);
      ImGui::EndGroup();
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) window.dragging = false;
  }
  ImGui::End();
  return write.changed;
}

}  // namespace wax::app

#include "app/node_pictures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "app/cardinal_picture.h"
#include "app/map_handle.h"
#include "app/offline_render.h"
#include "app/picture_slot.h"
#include "app/pogue_picture.h"
#include "app/space_pictures.h"
#include "app/theme.h"
#include "core/node.h"
#include "nodes/adsr.h"
#include "nodes/bezier.h"
#include "nodes/drawings.h"
#include "nodes/kalcore.h"
#include "nodes/membrane_view.h"
#include "nodes/pulsarcore.h"
#include "project/sample_edit.h"
#include "nodes/spacecore.h"
#include "nodes/twin_response.h"

namespace wax::app {
namespace {

constexpr double kPi = 3.14159265358979323846;
const ImU32 kOrange = IM_COL32(245, 160, 60, 255), kBlue = IM_COL32(90, 190, 235, 255), kPink = IM_COL32(240, 95, 150, 255);

double valueOf(const GraphNode& node, const NodeType& type, const std::string& name) {
  for (const ParamSpec& spec : type.params) if (spec.name == name) return node.param(name, spec.def);
  return 0;
}

ImU32 hsl(float h, float s, float l, int alpha = 255) {
  const float hh = h / 60.0f, k = (1 - std::fabs(2 * l - 1)) * s, x = k * (1 - std::fabs(std::fmod(hh, 2.0f) - 1)), m = l - k / 2;
  float r = 0, g = 0, b = 0;
  if (hh < 1) { r = k; g = x; } else if (hh < 2) { r = x; g = k; } else if (hh < 3) { g = k; b = x; } else if (hh < 4) { g = x; b = k; } else if (hh < 5) { r = x; b = k; } else { r = k; b = x; }
  return IM_COL32(static_cast<int>((r + m) * 255), static_cast<int>((g + m) * 255), static_cast<int>((b + m) * 255), alpha);
}

// The frame every picture sits in: under the tile (above it when the tile is low), with its title.
struct Frame { float x0, y0, w, h; float size; };
float gBelow = 0;   // how far under the tile the picture drawn last reaches (0 when it went above), for what is drawn after it

Frame frameFor(ImDrawList* dl, ImFont* font, const std::string& title, float w, float h, ImVec2 tileMin, ImVec2 tileMax, ImVec2 areaMin, ImVec2 areaMax, float scale) {
  w *= scale; h *= scale;
  const PictureSlot& slot = pictureSlot();
  const float x0 = slot.active ? slot.min.x : std::min(std::max(areaMin.x + 4 * scale, (tileMin.x + tileMax.x) / 2 - w / 2), std::max(areaMin.x + 4 * scale, areaMax.x - w - 4 * scale));
  const float below = tileMax.y + 16 * scale;
  const float y0 = slot.active ? slot.min.y : below + h <= areaMax.y - 4 * scale ? below : std::max(areaMin.y + 4 * scale, tileMin.y - h - 22 * scale);
  gBelow = !slot.active && below + h <= areaMax.y - 4 * scale ? h + 6 * scale : 0;
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), kBg);
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(128, 128, 128, 41));
  dl->AddRect(ImVec2(x0 + 0.5f, y0 + 0.5f), ImVec2(x0 + w - 0.5f, y0 + h - 0.5f), IM_COL32(128, 128, 128, 178));
  const float size = 11 * scale;
  dl->AddText(font, size, ImVec2(x0 + 8 * scale, y0 + 4 * scale), kText, title.c_str());
  return {x0, y0, w, h, size};
}

// ---- canyon: four delay lines against the song's ticks ----

constexpr int kCanyonLines = 4, kCanyonRepeats = 8;

void drawCanyon(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale, const PictureTiming& timing, ResponseHandles& handles) {
  const float PL = 30 * scale, PR = 36 * scale, PT = 20 * scale, ROW = 30 * scale, pw = f.w - PL - PR;
  const double time = valueOf(node, type, "time"), intensity = valueOf(node, type, "intensity"), cutoff = valueOf(node, type, "cutoff");
  std::array<double, kCanyonLines> levels{}, pans{}, damps{};
  for (int k = 0; k < kCanyonLines; k++) {
    const std::string n = std::to_string(k + 1);
    levels[static_cast<std::size_t>(k)] = valueOf(node, type, "level" + n); pans[static_cast<std::size_t>(k)] = valueOf(node, type, "pan" + n); damps[static_cast<std::size_t>(k)] = valueOf(node, type, "damp" + n);
  }
  // how loud each repeat is: every line feeds the next ones, in proportion to its level
  const int count = kCanyonRepeats + kCanyonLines;
  double sum = 0;
  for (const double l : levels) sum += l;
  std::vector<double> r(static_cast<std::size_t>(count) + 1, 0.0);
  r[0] = 1;
  for (int m = 1; m <= count; m++) {
    double v = 0;
    for (int k = 1; k <= std::min(kCanyonLines, m); k++) v += (sum > 1e-6 ? levels[static_cast<std::size_t>(k - 1)] / sum : 0) * r[static_cast<std::size_t>(m - k)];
    r[static_cast<std::size_t>(m)] = intensity * v;
  }
  const double span = (kCanyonRepeats + 0.6) * time;
  const auto xOf = [&](double ms) { return f.x0 + PL + static_cast<float>(ms / span) * pw; };
  const float top = f.y0 + PT, base = top + ROW * kCanyonLines;
  const double tickMs = timing.tickMs;
  char text[48];
  // the ruler: a mark for every tick while they are far enough apart, else for every beat, and a number on the beats
  if (tickMs > 0) {
    const double ticks = span / tickMs;
    const int perBeat = std::max(1, timing.perBeat);
    const int step = pw / ticks >= 5 * scale ? 1 : pw / (ticks / perBeat) >= 5 * scale ? perBeat : perBeat * 4;
    float lastLabel = -1e9f;
    for (int t = 0; t <= ticks; t += step) {
      const float x = std::round(xOf(t * tickMs)) + 0.5f;
      const bool beat = t % perBeat == 0;
      dl->AddLine(ImVec2(x, beat ? base : base + 3 * scale), ImVec2(x, base + (beat ? 10 : 7) * scale), textAlpha(beat ? 128 : 56));
      dl->AddLine(ImVec2(x, top), ImVec2(x, base), textAlpha(beat ? 26 : 10));
      if (beat && x - lastLabel > 22 * scale) { std::snprintf(text, sizeof text, "%d", t); dl->AddText(font, f.size, ImVec2(x - 3 * scale, base + 12 * scale), textAlpha(150), text); lastLabel = x; }
    }
    dl->AddText(font, f.size, ImVec2(f.x0 + f.w - PR + 4 * scale, base + 12 * scale), textAlpha(150), "ticks");
    std::snprintf(text, sizeof text, "%.0f ms · %.2f ticks", time, time / tickMs);
  } else {
    for (int m = 0; m <= kCanyonRepeats; m++) {
      const float x = std::round(xOf(m * time)) + 0.5f;
      dl->AddLine(ImVec2(x, base), ImVec2(x, base + 6 * scale), textAlpha(77));
      std::snprintf(text, sizeof text, "%d", static_cast<int>(std::lround(m * time)));
      dl->AddText(font, f.size, ImVec2(x - 8 * scale, base + 10 * scale), textAlpha(150), text);
    }
    std::snprintf(text, sizeof text, "%.0f ms", time);
  }
  dl->AddText(font, f.size, ImVec2(f.x0 + f.w - PR - ImGui::CalcTextSize(text).x * f.size / ImGui::GetFontSize() - 4 * scale, f.y0 + 4 * scale), textAlpha(190), text);
  const float x0 = std::round(xOf(0)) + 0.5f;
  dl->AddLine(ImVec2(x0, top - 2 * scale), ImVec2(x0, base), textAlpha(128));   // the dry sound, at the start
  static const float hues[kCanyonLines] = {196, 262, 322, 28};
  for (int k = 1; k <= kCanyonLines; k++) {
    const float rowTop = top + (k - 1) * ROW, floor = rowTop + ROW - 4 * scale, paler = 1 - 0.14f * (k - 1);
    dl->AddLine(ImVec2(f.x0 + PL, floor), ImVec2(f.x0 + PL + pw, floor), textAlpha(18));
    std::snprintf(text, sizeof text, "%d", k);
    dl->AddText(font, f.size, ImVec2(f.x0 + 8 * scale, rowTop + 4 * scale), textAlpha(165), text);
    for (int m = k; m <= kCanyonRepeats + k; m++) {
      const double amp = levels[static_cast<std::size_t>(k - 1)] * r[static_cast<std::size_t>(m - k)];
      if (amp <= 1e-4) continue;
      const double db = 20 * std::log10(amp), v = std::max(0.0, std::min(1.0, (db + 54) / 54));
      const float xx = xOf(m * time);
      if (xx > f.x0 + f.w - PR + 2 * scale) break;
      const float tall = static_cast<float>((ROW - 8 * scale) * v);
      const ImU32 colour = hsl(hues[k - 1], static_cast<float>(0.7 + 0.2 * v), static_cast<float>(0.38 + 0.28 * v), static_cast<int>((0.1 + 0.9 * v) * paler * 255));
      dl->AddRectFilled(ImVec2(std::round(xx) - 1, floor - tall), ImVec2(std::round(xx) + 2, floor + 1), colour);
      dl->AddRectFilled(ImVec2(std::round(xx) - 3, floor - 1), ImVec2(std::round(xx) + 4, floor + 1), colour);
      if (m == k) addMapHandle(handles, node, ImVec2(xx, floor - tall), ImVec2(xx, floor), ImVec2(1, -(ROW - 8 * scale)), "", axis("level" + std::to_string(k), 0, 1, 'b'));   // the first repeat's tip: how loud the line is
    }
    // the pan: a short track at the right, the dot left or right of its middle, and the cutoff the line is left with
    const float px = f.x0 + f.w - PR + 14 * scale, pc = px + static_cast<float>(pans[static_cast<std::size_t>(k - 1)]) * 9 * scale;
    dl->AddLine(ImVec2(px - 10 * scale, rowTop + 9.5f * scale), ImVec2(px + 10 * scale, rowTop + 9.5f * scale), textAlpha(77));
    dl->AddCircleFilled(ImVec2(pc, rowTop + 9.5f * scale), 3 * scale, hsl(hues[k - 1], 0.75f, 0.6f));
    const double hz = cutoff * std::pow(2.0, -4 * damps[static_cast<std::size_t>(k - 1)]);
    if (hz >= 1000) std::snprintf(text, sizeof text, hz >= 10000 ? "%.0fk" : "%.1fk", hz / 1000); else std::snprintf(text, sizeof text, "%d", static_cast<int>(std::lround(hz)));
    dl->AddText(font, f.size, ImVec2(px - 11 * scale, rowTop + 15 * scale), textAlpha(140), text);
    addMapHandle(handles, node, ImVec2(pc, rowTop + 9.5f * scale), ImVec2(px - 9 * scale, 0), ImVec2(18 * scale, 1), axis("pan" + std::to_string(k), -1, 1, 'l', 0.01));   // the pan dot
    addMapHandle(handles, node, ImVec2(px, rowTop + 19 * scale), ImVec2(0, rowTop + 4 * scale), ImVec2(1, ROW - 8 * scale), "", axis("damp" + std::to_string(k), 0, 1, 'l', 0.01));   // the cutoff the line is left with: down is darker
  }
}

// ---- ensemble: how long each of three delays is over a few seconds ----

void drawEnsemble(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale, ResponseHandles& handles) {
  constexpr double kBaseMs = 6, kSlowMs = 1.6, kFastMs = 0.22, kLow = 3.5, kHigh = 8.5, kSeconds = 4;
  const float PL = 34 * scale, PR = 8 * scale, TOP = 20 * scale, BOTTOM = 18 * scale, span = f.w - PL - PR, tall = f.h - TOP - BOTTOM;
  const double slow = valueOf(node, type, "slow"), fast = valueOf(node, type, "fast"), depth = valueOf(node, type, "depth"), shimmer = valueOf(node, type, "shimmer");
  const auto yOf = [&](double ms) { return f.y0 + TOP + static_cast<float>(1 - (ms - kLow) / (kHigh - kLow)) * tall; };
  char text[24];
  for (const int ms : {4, 6, 8}) {
    dl->AddLine(ImVec2(f.x0 + PL, std::round(yOf(ms)) + 0.5f), ImVec2(f.x0 + PL + span, std::round(yOf(ms)) + 0.5f), textAlpha(ms == 6 ? 64 : 31));
    std::snprintf(text, sizeof text, "%d ms", ms);
    dl->AddText(font, f.size, ImVec2(f.x0 + 2 * scale, yOf(ms) - f.size / 2), textAlpha(150), text);
  }
  dl->AddText(font, f.size, ImVec2(f.x0 + f.w - PR - 24 * scale, f.y0 + f.h - 14 * scale), textAlpha(150), "4 s");
  const ImU32 colours[3] = {kBlue, kOrange, hsl(145, 0.55f, 0.45f)};
  for (int line = 0; line < 3; line++) {
    std::vector<ImVec2> pts;
    for (int px = 0; px <= static_cast<int>(span) * 2; px++) {
      const double t = (px / 2.0 / span) * kSeconds, behind = line / 3.0;
      const double ms = kBaseMs + depth * (kSlowMs * std::sin(2 * kPi * (t * slow + behind)) + 2 * shimmer * kFastMs * std::sin(2 * kPi * (t * fast + behind)));
      pts.push_back(ImVec2(f.x0 + PL + px / 2.0f, yOf(ms)));
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), colours[line], 0, 1.3f * std::max(1.0f, scale));
  }
  // the first delay's first peak: along the axis is the time it takes (the slow rate), up and down how far it swings (the depth)
  const double peakAt = slow > 0 ? std::min(kSeconds, 1 / (4 * slow)) : kSeconds;
  ResponseHandles::Handle handle{node.id, "@ens", 0, ImVec2(f.x0 + PL + static_cast<float>(peakAt / kSeconds) * span, yOf(kBaseMs + depth * kSlowMs)), f.x0 + PL, f.y0 + TOP, span, tall};
  handles.list.push_back(handle);
}

// ---- haas: the early and late pulses, and the comb they make in mono ----

void drawHaas(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale, ResponseHandles& handles) {
  constexpr double kMaxMs = 45;
  const float PL = 34 * scale, PR = 12 * scale, top = 108 * scale;
  const double ms = valueOf(node, type, "time"), gain = valueOf(node, type, "gain"), mix = valueOf(node, type, "mix");
  const bool right = valueOf(node, type, "side") >= 0.5;
  const auto X = [&](double t) { return f.x0 + PL + static_cast<float>(t / kMaxMs) * (f.w - PL - PR); };
  char text[64];
  dl->AddRectFilled(ImVec2(X(1), f.y0 + 20 * scale), ImVec2(X(35), f.y0 + top - 6 * scale), withAlpha(kOrange, 20));   // where the ear takes it for one sound
  dl->AddText(font, f.size, ImVec2(X(4), f.y0 + 22 * scale), textAlpha(140), "one sound, from the early side");
  dl->AddText(font, f.size, ImVec2(X(37), f.y0 + 22 * scale), textAlpha(140), "echo");
  const float laneTop = f.y0 + 54 * scale, laneBottom = f.y0 + 94 * scale;   // left lane on top
  for (const float y : {laneTop, laneBottom}) dl->AddLine(ImVec2(f.x0 + PL, y), ImVec2(f.x0 + f.w - PR, y), textAlpha(64));
  for (int t = 0; t <= kMaxMs; t += 5) { std::snprintf(text, sizeof text, "%d", t); dl->AddText(font, f.size, ImVec2(X(t) - 4 * scale, f.y0 + top - 6 * scale), textAlpha(120), text); }
  dl->AddText(font, f.size, ImVec2(f.x0 + 12 * scale, laneTop - 14 * scale), kBlue, "L");
  dl->AddText(font, f.size, ImVec2(f.x0 + 12 * scale, laneBottom - 14 * scale), kPink, "R");
  const auto pulse = [&](float x, float y, double level, ImU32 colour) {
    const float tip = y - 26 * scale * static_cast<float>(std::clamp(level, 0.0, 1.2));
    dl->AddLine(ImVec2(x, y), ImVec2(x, tip), colour, 3 * scale);
    dl->AddCircleFilled(ImVec2(x, tip), 4 * scale, colour);
  };
  pulse(X(0), right ? laneTop : laneBottom, 1, right ? kBlue : kPink);
  pulse(X(ms), right ? laneBottom : laneTop, gain, right ? kPink : kBlue);
  // the late pulse's dot: along the time axis for the delay, up and down for its gain (gx: time 0, gw: the 45 ms, gy: the pulse's foot, gh: a full-height pulse)
  const float lateFoot = right ? laneBottom : laneTop;
  handles.list.push_back({node.id, "@haas", 0, ImVec2(X(ms), lateFoot - 26 * scale * static_cast<float>(std::clamp(gain, 0.0, 1.2))), X(0), lateFoot, X(kMaxMs) - X(0), 26 * scale});
  // the mono comb: the late pulse added to the early one
  const float y0 = f.y0 + top + 18 * scale, ph = f.h - top - 18 * scale - 28 * scale;
  const double lo = std::log10(20.0), hi = std::log10(20000.0);
  const auto fx = [&](double hz) { return f.x0 + PL + static_cast<float>((std::log10(hz) - lo) / (hi - lo)) * (f.w - PL - PR); };
  const auto yOf = [&](double db) { return y0 + static_cast<float>((8 - std::clamp(db, -18.0, 8.0)) / 26) * ph; };   // +8 dB at the top, -18 at the bottom
  for (const double hz : {100.0, 1000.0, 10000.0}) dl->AddLine(ImVec2(fx(hz), y0), ImVec2(fx(hz), y0 + ph), textAlpha(50));
  dl->AddLine(ImVec2(f.x0 + PL, yOf(0)), ImVec2(f.x0 + f.w - PR, yOf(0)), textAlpha(50));
  dl->AddText(font, f.size, ImVec2(f.x0 + PL, y0 - 14 * scale), textAlpha(150), "mono sum (what a mono speaker plays)");
  for (const auto& [hz, name] : {std::pair<double, const char*>{100, "100"}, {1000, "1k"}, {10000, "10k"}}) dl->AddText(font, f.size, ImVec2(fx(hz) - 6 * scale, y0 + ph + 5 * scale), textAlpha(150), name);
  dl->AddText(font, f.size, ImVec2(f.x0 + 4 * scale, yOf(0) - 4 * scale), textAlpha(150), "0 dB");
  std::vector<ImVec2> line;
  const double g = gain * mix;
  for (int i = 0; i <= 300; i++) {
    const double hz = std::pow(10.0, lo + (hi - lo) * i / 300), turn = 2 * kPi * hz * ms / 1000;
    const double mag = std::hypot(1 + g * std::cos(turn), g * std::sin(turn));
    line.push_back(ImVec2(fx(hz), yOf(20 * std::log10(std::max(1e-3, mag)))));
  }
  dl->AddPolyline(line.data(), static_cast<int>(line.size()), kOrange, 0, 1.5f * std::max(1.0f, scale));
  std::snprintf(text, sizeof text, "%s side %.1f ms late at %d%%", right ? "right" : "left", ms, static_cast<int>(std::lround(gain * 100)));
  dl->AddText(font, f.size, ImVec2(f.x0 + 8 * scale, f.y0 + f.h - 14 * scale), textAlpha(190), text);
}

// ---- reflect: the room from above, and the echoes in time ----

constexpr double kSoundSpeed = 343;

void drawReflect(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale, ResponseHandles& handles) {
  const float W = f.w, PAD = 14 * scale, planH = 250 * scale, echoH = 110 * scale;
  const double rw = valueOf(node, type, "width"), rd = valueOf(node, type, "depth"), rh = valueOf(node, type, "height"), absorb = valueOf(node, type, "absorb"), taps = valueOf(node, type, "taps");
  const double sx = valueOf(node, type, "srcx"), sy = valueOf(node, type, "srcy"), lx = valueOf(node, type, "lisx"), ly = valueOf(node, type, "lisy");
  const space::Room room = space::reflections(rw, rd, rh, sx, sy, lx, ly, absorb, taps);
  const float k = std::min((W - 2 * PAD - 120 * scale) / static_cast<float>(rw), (planH - 2 * PAD) / static_cast<float>(rd));
  const float x0 = f.x0 + PAD + 20 * scale, y0 = f.y0 + PAD + 14 * scale, pw = static_cast<float>(rw) * k, pd = static_cast<float>(rd) * k;
  char text[96];
  dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + pw, y0 + pd), withAlpha(kText, 180), 0, 0, 2.0f);
  dl->AddRectFilled(ImVec2(x0 + pw - 5 * scale, y0 + pd - 5 * scale), ImVec2(x0 + pw + 5 * scale, y0 + pd + 5 * scale), textAlpha(128));   // the corner to drag
  std::snprintf(text, sizeof text, "%.1f m", rw);
  dl->AddText(font, f.size, ImVec2(x0 + pw / 2 - 14 * scale, y0 + pd + 5 * scale), textAlpha(150), text);
  std::snprintf(text, sizeof text, "%.1f m", rd);
  dl->AddText(font, f.size, ImVec2(x0 + pw + 8 * scale, y0 + pd / 2), textAlpha(150), text);
  const ImVec2 s(x0 + static_cast<float>(sx) * pw, y0 + static_cast<float>(sy) * pd), l(x0 + static_cast<float>(lx) * pw, y0 + static_cast<float>(ly) * pd);
  dl->AddLine(s, l, withAlpha(kOrange, 90));
  dl->AddCircleFilled(s, 7 * scale, kOrange);
  dl->AddTriangleFilled(ImVec2(l.x, l.y - 9 * scale), ImVec2(l.x + 7 * scale, l.y + 6 * scale), ImVec2(l.x - 7 * scale, l.y + 6 * scale), kBlue);
  dl->AddText(font, f.size, ImVec2(s.x + 10 * scale, s.y - 5 * scale), textAlpha(180), "sound");
  dl->AddText(font, f.size, ImVec2(l.x + 10 * scale, l.y - 5 * scale), textAlpha(180), "you");
  handles.list.push_back({node.id, "@src", 0, s, f.x0 + PAD + 20 * scale, y0, pw, pd});
  handles.list.push_back({node.id, "@lis", 0, l, f.x0 + PAD + 20 * scale, y0, pw, pd});
  handles.list.push_back({node.id, "@size", 0, ImVec2(x0 + pw, y0 + pd), x0, y0, pw, pd});
  // the echoes in time, after the dry sound
  const float y1 = f.y0 + 14 * scale + planH + 8 * scale;
  double last = 0.03;
  for (const space::Tap& t : room.list) last = std::max(last, t.delay - room.direct);
  const double tmax = last * 1.05;
  dl->AddLine(ImVec2(f.x0 + PAD, y1 + echoH + 0.5f), ImVec2(f.x0 + W - PAD, y1 + echoH + 0.5f), textAlpha(64));
  for (const space::Tap& t : room.list) {
    const float x = f.x0 + PAD + static_cast<float>((t.delay - room.direct) / tmax) * (W - 2 * PAD), tall = static_cast<float>(std::clamp(t.gain, 0.0, 1.0)) * (echoH - 8 * scale);
    dl->AddLine(ImVec2(x, y1 + echoH), ImVec2(x, y1 + echoH - tall), t.pan < -0.2 ? kBlue : t.pan > 0.2 ? kPink : textAlpha(180), 2.0f);
  }
  std::snprintf(text, sizeof text, "0     %d ms after the dry sound   (left blue, right pink)", static_cast<int>(std::lround(tmax * 1000)));
  dl->AddText(font, f.size, ImVec2(f.x0 + PAD, y1 + echoH + 4 * scale), textAlpha(150), text);
  std::snprintf(text, sizeof text, "direct %.1f ms (%.1f m)   %d reflections", room.direct * 1000, room.direct * kSoundSpeed, static_cast<int>(room.list.size()));
  dl->AddText(font, f.size, ImVec2(f.x0 + 8 * scale, f.y0 + f.h - 14 * scale), textAlpha(190), text);
}

// ---- rotary: the horn and the drum turning ----

struct Rotors { double hornAngle = 0, drumAngle = 0.5, hornSpeed = 0, drumSpeed = 0; };

void drawRotary(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale, ResponseHandles& handles) {
  static std::map<std::string, Rotors> state;   // the rotors keep turning between frames, at the speeds the node's own rule gives them
  Rotors& r = state[node.id];
  const double seconds = std::min(0.1, static_cast<double>(ImGui::GetIO().DeltaTime));
  const int speed = static_cast<int>(std::lround(valueOf(node, type, "speed")));
  const double target = speed <= 0 ? 0 : speed == 1 ? valueOf(node, type, "slow") : valueOf(node, type, "fast"), inertia = std::max(0.05, valueOf(node, type, "inertia"));
  const auto next = [&](double now, double want, double rise, double fall) { return now + (want - now) * (1 - std::exp(-seconds / (((want > now ? rise : fall) * inertia) / 3))); };
  r.hornSpeed = next(r.hornSpeed, target * 1.0, 0.9, 1.3);
  r.drumSpeed = next(r.drumSpeed, target * 0.86, 4.5, 5.5);
  r.hornAngle = std::fmod(r.hornAngle + 1.0 * r.hornSpeed * seconds + 1, 1.0);
  r.drumAngle = std::fmod(r.drumAngle - r.drumSpeed * seconds + 1, 1.0);
  const float radius = 46 * scale, cy = f.y0 + 84 * scale;
  const double spread = valueOf(node, type, "spread");
  char text[48];
  for (int rotor = 0; rotor < 2; rotor++) {
    const float cx = f.x0 + (rotor ? 218 : 82) * scale;
    const double angle = (rotor ? r.drumAngle : r.hornAngle) * 2 * kPi - kPi / 2;
    const ImU32 colour = rotor ? kBlue : kOrange;
    dl->AddCircle(ImVec2(cx, cy), radius, textAlpha(90), 48);
    const ImVec2 tip(cx + static_cast<float>(std::cos(-angle)) * radius, cy - static_cast<float>(std::sin(-angle)) * radius);
    dl->AddLine(ImVec2(cx, cy), tip, colour, 1.5f * std::max(1.0f, scale));
    dl->AddCircleFilled(tip, 4 * scale, colour);
    dl->AddCircleFilled(ImVec2(cx, cy), 2 * scale, colour);
    const double off = spread * 0.25 * 2 * kPi;   // the two microphones in front, `spread` apart
    for (const int side : {-1, 1}) dl->AddCircleFilled(ImVec2(cx + static_cast<float>(std::sin(side * off)) * (radius + 12 * scale), cy + static_cast<float>(std::cos(side * off)) * (radius + 12 * scale)), 2.5f * scale, textAlpha(200));
    const double hz = rotor ? r.drumSpeed : r.hornSpeed;
    std::snprintf(text, sizeof text, hz < 1 ? "%s  %.2f Hz" : "%s  %.1f Hz", rotor ? "drum" : "horn", hz);
    dl->AddText(font, f.size, ImVec2(cx - 30 * scale, cy + radius + 22 * scale), textAlpha(190), text);
  }
  const char* names[3] = {"stop", "slow", "fast"};   // the switch
  for (int i = 0; i < 3; i++) {
    const ImVec2 at(f.x0 + (80 + i * 70) * scale, f.y0 + f.h - 16 * scale);
    dl->AddText(font, f.size, ImVec2(at.x - 12 * scale, at.y - 6 * scale), i == speed ? kAccent : textAlpha(190), names[i]);
    handles.list.push_back({node.id, std::string("@speed") + static_cast<char>('0' + i), 0, at});
  }
}

// ---- khufu: two cycles of the three registers, and what they add up to ----

void drawKhufu(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale, ResponseHandles& handles) {
  const float PL = 8 * scale, PR = 8 * scale, top = 22 * scale, span = f.w - PL - PR;
  const char* names[3] = {"low", "mid", "high"};
  const ImU32 colours[3] = {kBlue, kOrange, hsl(145, 0.55f, 0.45f)};
  double levels[3];
  for (int i = 0; i < 3; i++) levels[i] = valueOf(node, type, names[i]);
  const double total = std::max(1.0, levels[0] + levels[1] + levels[2]);
  const float mid = f.y0 + top + (f.h - top) / 2, tall = (f.h - top) / 2 - 8 * scale;
  float x = f.x0 + 100 * scale;   // the legend: each register's name and level, in its colour
  char text[32];
  for (int i = 0; i < 3; i++) {
    std::snprintf(text, sizeof text, "%s %d%%", names[i], static_cast<int>(std::lround(levels[i] * 100)));
    dl->AddText(font, f.size, ImVec2(x, f.y0 + 4 * scale), levels[i] > 0 ? colours[i] : withAlpha(colours[i], 100), text);
    x += static_cast<float>(std::strlen(text)) * f.size * 0.6f + 10 * scale;
  }
  dl->AddLine(ImVec2(f.x0 + PL, mid), ImVec2(f.x0 + f.w - PR, mid), textAlpha(40));
  const auto trace = [&](const double lv[3], ImU32 colour, float width) {
    std::vector<ImVec2> pts;
    for (int px = 0; px <= static_cast<int>(span) * 2; px++) {
      const double phase = std::fmod(px / 2.0 / span * 2, 1.0);
      double sum = 0;
      for (int i = 0; i < 3; i++) sum += (1 - 2 * std::fmod(phase * (1 << i), 1.0)) * lv[i];
      pts.push_back(ImVec2(f.x0 + PL + px / 2.0f, mid - static_cast<float>(sum / total) * tall));
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), colour, 0, width);
  };
  for (int i = 0; i < 3; i++) {
    if (levels[i] <= 0) continue;
    double only[3] = {0, 0, 0};
    only[i] = levels[i];
    trace(only, withAlpha(colours[i], 115), 1.0f);
  }
  trace(levels, kText, 1.6f * std::max(1.0f, scale));
  for (int i = 0; i < 3; i++) {   // a dot where each register's wave starts: up and down is its level
    const float dotX = f.x0 + PL + (6 + 14 * i) * scale;
    dl->AddCircleFilled(ImVec2(dotX, mid - static_cast<float>(levels[i] / total) * tall), 3.5f * scale, colours[i]);
    addMapHandle(handles, node, ImVec2(dotX, mid - static_cast<float>(levels[i] / total) * tall), ImVec2(0, mid), ImVec2(1, -tall / static_cast<float>(total)), "", axis(names[i], 0, 1, 'l', 0.01));
  }
}

// ---- membrane: the head with the pattern of one mode, and every mode as a stem ----

std::map<std::string, std::string>& pickedModes() { static std::map<std::string, std::string> picked; return picked; }   // node -> "m,n", kept for the session

void drawMembrane(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale, ResponseHandles& handles) {
  const float head = 78 * scale, hx = f.x0 + 92 * scale, hy = f.y0 + 100 * scale, sx = f.x0 + 196 * scale, sw = 150 * scale, sy = f.y0 + 22 * scale, sh = 112 * scale;
  const double position = valueOf(node, type, "position"), strike = valueOf(node, type, "strike");
  const std::vector<MembraneMode> modes = membraneModes(position, valueOf(node, type, "stiffness"), valueOf(node, type, "damping"), valueOf(node, type, "highdamp"), valueOf(node, type, "quality"));
  if (modes.empty()) return;
  // how loud a mode is at the strike point, 0 to 1: its weight, the beater's low-pass at a nominal drum pitch, a little for decay
  std::vector<double> level;
  const double fc = 300 * std::pow(60.0, strike);
  double top = 1e-9;
  for (const MembraneMode& m : modes) {
    const double hz = m.ratio * 160, lp = 1 / (1 + (hz / fc) * (hz / fc));
    level.push_back(std::fabs(m.weight) * lp * std::min(1.0, 0.25 + m.t60 * 2));
    top = std::max(top, level.back());
  }
  for (double& v : level) v /= top;
  const std::string& pickedKey = pickedModes()[node.id];
  std::size_t chosen = modes.size();
  for (std::size_t i = 0; i < modes.size(); i++) if (std::to_string(modes[i].m) + "," + std::to_string(modes[i].n) == pickedKey) chosen = i;
  if (chosen == modes.size()) { chosen = 0; for (std::size_t i = 0; i < modes.size(); i++) if (modes[i].m == 1 && modes[i].n == 1) { chosen = i; break; } }
  const MembraneMode& mode = modes[chosen];
  char text[96];
  // the head: the rim, then rings of dots, a filled dot moves up and a hollow one down, the size is how far
  dl->AddCircle(ImVec2(hx, hy), head + 0.5f, textAlpha(140), 64);
  dl->AddCircle(ImVec2(hx, hy), head * 0.9f, textAlpha(30), 64);
  struct Dot { double rho, theta, z; };
  std::vector<Dot> dots;
  constexpr int kRings = 11;
  for (int r = 1; r <= kRings; r++) {
    const double rho = r / (kRings + 0.6);
    const int count = std::max(6, static_cast<int>(std::lround(rho * 44)));
    for (int i = 0; i < count; i++) {
      const double theta = static_cast<double>(i) / count * 2 * kPi;
      dots.push_back({rho, theta, membraneBessel(mode.m, mode.j * rho) * std::cos(mode.m * theta)});
    }
  }
  dots.push_back({0, 0, membraneBessel(mode.m, 0)});
  double peak = 1e-9;
  for (const Dot& d : dots) peak = std::max(peak, std::fabs(d.z));
  for (const Dot& d : dots) {
    const float size = static_cast<float>(std::fabs(d.z) / peak * 3.4) * scale;
    if (size < 0.5f * scale) continue;
    const ImVec2 at(hx + static_cast<float>(std::cos(d.theta) * d.rho) * head, hy + static_cast<float>(std::sin(d.theta) * d.rho) * head);
    if (d.z > 0) dl->AddCircleFilled(at, size, kBlue); else dl->AddCircle(at, size, kOrange);
  }
  const ImVec2 cross(hx + static_cast<float>(std::cos(kMembraneStrikeAngle) * position * 0.9) * head, hy + static_cast<float>(std::sin(kMembraneStrikeAngle) * position * 0.9) * head);   // where it is struck
  dl->AddLine(ImVec2(cross.x - 5 * scale, cross.y), ImVec2(cross.x + 5 * scale, cross.y), kText, 1.4f);
  dl->AddLine(ImVec2(cross.x, cross.y - 5 * scale), ImVec2(cross.x, cross.y + 5 * scale), kText, 1.4f);
  // the stems: pitch as a multiple of the lowest, height how hard the strike sets the mode going. Click one to see its pattern.
  dl->AddLine(ImVec2(sx, sy + sh), ImVec2(sx + sw, sy + sh), textAlpha(50));
  for (int r = 1; r <= 7; r++) {
    const float x = sx + r / 8.0f * sw;
    dl->AddLine(ImVec2(std::round(x), sy + sh), ImVec2(std::round(x), sy + sh + 3 * scale), textAlpha(128));
    std::snprintf(text, sizeof text, "%d", r);
    dl->AddText(font, f.size, ImVec2(x - 2 * scale, sy + sh + 5 * scale), textAlpha(128), text);
  }
  dl->AddText(font, f.size, ImVec2(sx, sy + sh + 17 * scale), textAlpha(190), "pitch (times the lowest)");
  for (std::size_t i = 0; i < modes.size(); i++) {
    const float x = std::round(sx + static_cast<float>(modes[i].ratio / 8) * sw) + 0.5f, h = std::max(1.0f, static_cast<float>(level[i]) * sh);
    const bool on = i == chosen;
    dl->AddLine(ImVec2(x, sy + sh), ImVec2(x, sy + sh - h), on ? kBlue : withAlpha(kText, static_cast<int>(140 + 80 * level[i])), on ? 1.6f : 1.0f);
    dl->AddCircleFilled(ImVec2(x, sy + sh - h), (on ? 3 : 2) * scale, on ? kBlue : kText);
    if (level[i] > 0.45 || on) {
      std::snprintf(text, sizeof text, "%d,%d", modes[i].m, modes[i].n);
      dl->AddText(font, f.size, ImVec2(x - 7 * scale, sy + sh - h - 14 * scale), textAlpha(on ? 255 : 180), text);
    }
    handles.list.push_back({node.id, "@mode" + std::to_string(modes[i].m) + "," + std::to_string(modes[i].n), 0, ImVec2(x, sy + sh - h)});
  }
  const auto count = [](int n, const char* word) { return (n == 0 ? std::string("no") : std::to_string(n)) + " " + word + (n == 1 ? "" : "s"); };
  std::snprintf(text, sizeof text, "mode %d,%d · %.2fx · %s, %s", mode.m, mode.n, mode.ratio, count(mode.m, "nodal line").c_str(), count(mode.n - 1, "nodal circle").c_str());
  dl->AddText(font, f.size, ImVec2(f.x0 + 6 * scale, f.y0 + f.h - 14 * scale), textAlpha(190), text);
}

// ---- granular: the sample with the position, spray and scan; the grains; the cloud ----

// The sample a granular node plays (its `sample` parameter picks from the node's list), as one min/max pair for each of `width` columns.
const std::vector<std::pair<float, float>>& samplePeaks(const GraphNode& node, double slot, const PictureTiming& timing, int width) {
  static std::vector<std::pair<float, float>> peaks;
  static std::string key;
  static std::vector<std::pair<float, float>> none;
  if (!timing.project) return none;
  const Instrument* instrument = nullptr;
  for (const Instrument& i : timing.project->instruments) if (i.id == timing.instrument) instrument = &i;
  if (!instrument || !instrument->raw.is_object() || !instrument->raw.contains("samples") || !instrument->raw["samples"].is_object() || !instrument->raw["samples"].contains(node.id)) return none;
  const Json& mapped = instrument->raw["samples"][node.id];
  std::string id;
  if (mapped.is_string()) id = mapped.get<std::string>();
  else if (mapped.is_array() && !mapped.empty()) { const Json& pick = mapped[std::min<std::size_t>(mapped.size() - 1, static_cast<std::size_t>(std::max(0.0, slot)))]; if (pick.is_string()) id = pick.get<std::string>(); }
  if (id.empty() || !timing.project->samples.is_object() || !timing.project->samples.contains(id)) return none;
  const std::string now = id + "#" + std::to_string(timing.revision) + "#" + std::to_string(width);
  if (now != key) {
    const EditableSample sample = readSample(timing.project->samples[id]);
    peaks.assign(static_cast<std::size_t>(width), {0.0f, 0.0f});
    const std::size_t len = sample.left.size();
    for (int px = 0; px < width && len; px++) {
      const std::size_t a = len * static_cast<std::size_t>(px) / static_cast<std::size_t>(width), b = std::max(a + 1, len * static_cast<std::size_t>(px + 1) / static_cast<std::size_t>(width));
      float lo = 0, hi = 0;
      for (std::size_t i = a; i < b && i < len; i += std::max<std::size_t>(1, (b - a) / 24)) { lo = std::min(lo, sample.left[i]); hi = std::max(hi, sample.left[i]); }
      peaks[static_cast<std::size_t>(px)] = {lo, hi};
    }
    key = now;
  }
  return peaks;
}

double grainWindow(double p, double shape) {
  const double edge = std::min(p, 1 - p) * 2 / std::max(0.02, shape);
  return 0.5 - 0.5 * std::cos(kPi * std::min(1.0, std::max(0.0, edge)));
}

void drawGranular(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale, const PictureTiming& timing, ResponseHandles& handles) {
  const float PL = 8 * scale, PR = 8 * scale, pw = f.w - PL - PR;
  const auto v = [&](const char* name) { return valueOf(node, type, name); };
  char text[96];
  const auto right = [&](const char* t, float y) { dl->AddText(font, f.size, ImVec2(f.x0 + f.w - PR - ImGui::CalcTextSize(t).x * f.size / ImGui::GetFontSize(), y), textAlpha(190), t); };
  // the sample, with `position` marked, the band `spray` scatters the grains over, and an arrow for where `scan` takes the position in a second
  const float sy = f.y0 + 34 * scale, sh = 64 * scale, mid = sy + sh / 2;
  const double pos = v("position"), spray = v("spray"), scan = v("scan");
  dl->AddText(font, f.size, ImVec2(f.x0 + 90 * scale, f.y0 + 4 * scale), textAlpha(190), "sample");
  std::snprintf(text, sizeof text, "position %d%% ± %d%%%s", static_cast<int>(std::lround(pos * 100)), static_cast<int>(std::lround(spray * 100)), std::fabs(scan) > 0.0005 ? (std::string(" · scan ") + (scan > 0 ? "+" : "") + std::to_string(scan).substr(0, 5) + "/s").c_str() : "");
  right(text, f.y0 + 18 * scale);
  const auto band = [&](double a, double b) { dl->AddRectFilled(ImVec2(f.x0 + PL + static_cast<float>(std::max(0.0, a)) * pw, sy), ImVec2(f.x0 + PL + static_cast<float>(std::min(1.0, b)) * pw + 1, sy + sh), withAlpha(kBlue, 40)); };
  const double lo = pos - spray, hi = pos + spray;
  band(lo, hi);
  if (lo < 0) band(1 + lo, 1);
  if (hi > 1) band(0, hi - 1);
  dl->AddLine(ImVec2(f.x0 + PL, std::round(mid)), ImVec2(f.x0 + PL + pw, std::round(mid)), textAlpha(64));
  const auto& peaks = samplePeaks(node, v("sample"), timing, static_cast<int>(pw));
  if (peaks.empty()) dl->AddText(font, f.size, ImVec2(f.x0 + f.w / 2 - 80 * scale, mid - 5 * scale), textAlpha(190), "no sample: add one to the node");
  for (std::size_t px = 0; px < peaks.size(); px++) {
    const float amp = sh / 2 - 2 * scale, top = mid - std::min(1.0f, peaks[px].second) * amp, bottom = mid - std::max(-1.0f, peaks[px].first) * amp;
    dl->AddRectFilled(ImVec2(f.x0 + PL + static_cast<float>(px), top), ImVec2(f.x0 + PL + static_cast<float>(px) + 1, std::max(top + 1, bottom)), textAlpha(200));
  }
  const float px = f.x0 + PL + static_cast<float>(pos) * pw;
  dl->AddLine(ImVec2(px + 0.5f, sy), ImVec2(px + 0.5f, sy + sh), kBlue, 1.5f);
  dl->AddCircleFilled(ImVec2(px + 0.5f, sy), 3.5f * scale, kBlue);
  addMapHandle(handles, node, ImVec2(px + 0.5f, sy), ImVec2(f.x0 + PL, 0), ImVec2(pw, 1), axis("position", 0, 1, 'l', 0.001));   // the marker: the position
  const float sprayX = std::min(f.x0 + PL + pw, px + static_cast<float>(spray) * pw);
  dl->AddCircleFilled(ImVec2(sprayX, mid), 3 * scale, withAlpha(kBlue, 200));
  addMapHandle(handles, node, ImVec2(sprayX, mid), ImVec2(px, 0), ImVec2(pw, 1), axis("spray", 0, 1, 'l', 0.001));   // the band's edge: the spray
  if (std::fabs(scan) > 0.005) {
    const float to = f.x0 + PL + static_cast<float>(std::clamp(pos + scan, 0.0, 1.0)) * pw, dir = to >= px ? 1.0f : -1.0f, ay = sy + sh + 7 * scale;
    dl->AddLine(ImVec2(px, ay), ImVec2(to, ay), kOrange, 1.2f);
    dl->AddLine(ImVec2(to - dir * 4 * scale, ay - 3 * scale), ImVec2(to, ay), kOrange, 1.2f);
    dl->AddLine(ImVec2(to - dir * 4 * scale, ay + 3 * scale), ImVec2(to, ay), kOrange, 1.2f);
  }
  // the grains: a stretch of time with the grains that start in it, their shape and how many sound at once
  const double size = v("size"), density = std::max(0.1, v("density")), shape = v("shape");
  const float gy = f.y0 + 134 * scale, gh = 62 * scale, base = gy + gh;
  dl->AddText(font, f.size, ImVec2(f.x0 + PL, gy - 14 * scale), textAlpha(190), "grains");
  const double overlap = density * size / 1000;
  std::snprintf(text, sizeof text, "%d ms · %d/s · about %s at once", static_cast<int>(std::lround(size)), static_cast<int>(std::lround(density)), (overlap < 10 ? std::to_string(overlap).substr(0, 3) : std::to_string(static_cast<int>(std::lround(overlap)))).c_str());
  right(text, gy - 14 * scale);
  const double span = std::max(std::min(1000.0, size * 3), 200.0), msPx = pw / span, every = 1000 / density;
  dl->AddLine(ImVec2(f.x0 + PL, base), ImVec2(f.x0 + PL + pw, base), textAlpha(50));
  unsigned seed = 7;
  const auto jitter = [&]() { seed = (seed * 9301 + 49297) % 233280; return seed / 233280.0 - 0.5; };
  int index = 0;
  for (double t = 0; t < span && index < 400; t += every, index++) {
    const double t0 = t + jitter() * every * 0.6, rowH = (gh - 6 * scale) * (0.55 + 0.45 * std::fmod(index * 0.618, 1.0));
    const float x0 = f.x0 + PL + static_cast<float>(t0 * msPx), w = static_cast<float>(size * msPx);
    if (x0 + w < f.x0 + PL || x0 > f.x0 + PL + pw) continue;
    std::vector<ImVec2> pts;
    for (int k = 0; k <= 32; k++) pts.push_back(ImVec2(std::clamp(x0 + k / 32.0f * w, f.x0 + PL, f.x0 + PL + pw), base - static_cast<float>(grainWindow(k / 32.0, shape) * rowH)));
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), withAlpha(index % 2 ? kBlue : kOrange, 160), 0, 1.0f);
  }
  // the second grain's start: along the axis is the time between grains (the density)
  if (every * msPx < pw) {
    handles.list.push_back({node.id, "@gden", 0, ImVec2(f.x0 + PL + static_cast<float>(every * msPx), base), f.x0 + PL, 0, static_cast<float>(msPx), 1});
    dl->AddCircleFilled(ImVec2(f.x0 + PL + static_cast<float>(every * msPx), base), 3 * scale, withAlpha(kOrange, 220));
  }
  // the cloud: pitch (tune and the random cents), the chance of playing backwards, how wide the grains are spread
  const double tune = v("tune"), rand = v("pitchRand") / 100, rev = v("reverse"), spread = v("spread");
  const auto track = [&](const char* name, float y, const std::string& label, double from, double to) {
    dl->AddText(font, f.size, ImVec2(f.x0 + PL, y), textAlpha(190), name);
    const float x0 = f.x0 + PL + 54 * scale, w = 150 * scale;
    dl->AddLine(ImVec2(x0, y + 6 * scale), ImVec2(x0 + w, y + 6 * scale), textAlpha(64));
    if (to - from > 0.01) dl->AddLine(ImVec2(x0 + static_cast<float>(from) * w, y + 6 * scale), ImVec2(x0 + static_cast<float>(to) * w, y + 6 * scale), kBlue, 2.0f);
    dl->AddCircleFilled(ImVec2(x0 + static_cast<float>((from + to) / 2) * w, y + 6 * scale), 3 * scale, kBlue);
    const std::string which = name;
    if (which == "pitch") addMapHandle(handles, node, ImVec2(x0 + static_cast<float>((from + to) / 2) * w, y + 6 * scale), ImVec2(x0, 0), ImVec2(w, 1), axis("tune", -24, 24, 'l', 0.1));
    else if (which == "reverse") addMapHandle(handles, node, ImVec2(x0 + static_cast<float>(to) * w, y + 6 * scale), ImVec2(x0, 0), ImVec2(w, 1), axis("reverse", 0, 1, 'l', 0.01));
    else addMapHandle(handles, node, ImVec2(x0 + static_cast<float>(to) * w, y + 6 * scale), ImVec2(x0 + w / 2, 0), ImVec2(w / 2, 1), axis("spread", 0, 1, 'l', 0.01));
    dl->AddText(font, f.size, ImVec2(x0 + w + 10 * scale, y), textAlpha(190), label.c_str());
  };
  const auto semi = [](double s) { return (std::clamp(s, -24.0, 24.0) + 24) / 48; };
  const float cy = f.y0 + 214 * scale;
  std::snprintf(text, sizeof text, "%s%.1f st%s", tune > 0 ? "+" : "", tune, rand ? (" ± " + std::to_string(static_cast<int>(std::lround(v("pitchRand")))) + " ct").c_str() : "");
  track("pitch", cy, text, semi(tune - rand), semi(tune + rand));
  track("reverse", cy + 14 * scale, std::to_string(static_cast<int>(std::lround(rev * 100))) + "% backwards", 0, rev);
  track("spread", cy + 28 * scale, spread < 0.01 ? "centre" : std::to_string(static_cast<int>(std::lround(spread * 100))) + "% of the width", 0.5 - spread / 2, 0.5 + spread / 2);
}

// ---- kaleidoscope: the band the node takes and where its copies go (the spectra are not drawn: they need the live sound) ----

void drawKaleidoscope(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale, ResponseHandles& handles, const PictureTiming& timing) {
  constexpr double kFMin = 27.5, kFMax = 14080;
  const double lo = std::log2(kFMin), hi = std::log2(kFMax);
  const float pad = 14 * scale, panelH = 78 * scale, x0 = f.x0 + pad, pw = f.w - 2 * pad;
  const auto X = [&](double octave) { return x0 + static_cast<float>((octave - lo) / (hi - lo)) * pw; };
  const kal::Band band = kal::bandOf(valueOf(node, type, "low"), valueOf(node, type, "high"));
  const int mode = static_cast<int>(std::lround(valueOf(node, type, "mode"))), copies = static_cast<int>(std::lround(valueOf(node, type, "copies")));
  const double rotate = valueOf(node, type, "rotate");
  const float yIn = f.y0 + 34 * scale, yOut = yIn + panelH + 40 * scale;
  const auto panel = [&](float y, const char* title) {
    dl->AddText(font, f.size, ImVec2(x0, y - 15 * scale), textAlpha(160), title);
    for (int o = static_cast<int>(std::ceil(lo)); o <= hi; o++) dl->AddLine(ImVec2(std::round(X(o)) + 0.5f, y), ImVec2(std::round(X(o)) + 0.5f, y + panelH), textAlpha(30));
    dl->AddLine(ImVec2(x0, y + panelH + 0.5f), ImVec2(x0 + pw, y + panelH + 0.5f), textAlpha(90));
    static const std::pair<double, const char*> labels[] = {{55, "55"}, {110, "110"}, {220, "220"}, {440, "440"}, {880, "880"}, {1760, "1.8k"}, {3520, "3.5k"}, {7040, "7k"}, {14080, "14k"}};
    for (const auto& [hz, text] : labels) {
      const float x = X(std::log2(hz)), w = ImGui::CalcTextSize(text).x * f.size / ImGui::GetFontSize();
      dl->AddText(font, f.size * 0.85f, ImVec2(x - w * 0.425f, y + panelH + 3 * scale), textAlpha(120), text);
    }
  };
  panel(yIn, "BAND  (drag its ends, or the band)");
  // The spectra the node hears and makes, live while this picture is on screen.
  std::vector<float> heard, made;
  if (const auto probe = timing.probeOf ? timing.probeOf(timing.instrument, node.id) : nullptr) {
    probe->watch();
    probe->read(heard, made);
  }
  const auto spectrum = [&](const std::vector<float>& values, float y, ImU32 colour) {
    ImVec2 last{};
    for (std::size_t i = 0; i < values.size(); i++) {
      const ImVec2 at(x0 + pw * (static_cast<float>(i) + 0.5f) / static_cast<float>(values.size()), y + panelH * (1 - values[i]));
      if (i) dl->AddLine(last, at, colour, 1.2f * scale);
      last = at;
    }
  };
  if (heard.empty()) dl->AddText(font, f.size * 0.9f, ImVec2(x0 + pw / 2 - 110 * scale, yIn + panelH / 2 - 6 * scale), textAlpha(110), "play something through it to see the spectrum");
  spectrum(heard, yIn, textAlpha(170));
  const float bx0 = X(band.b0), bx1 = X(band.b0 + band.width);
  dl->AddRectFilled(ImVec2(bx0, yIn), ImVec2(bx1, yIn + panelH), withAlpha(kPink, 36));
  for (const float x : {bx0, bx1}) {
    dl->AddLine(ImVec2(std::round(x) + 0.5f, yIn), ImVec2(std::round(x) + 0.5f, yIn + panelH), kPink, 1.5f);
    dl->AddRectFilled(ImVec2(std::round(x) - 4 * scale, yIn - 4 * scale), ImVec2(std::round(x) + 4 * scale, yIn + 4 * scale), kPink);
  }
  handles.list.push_back({node.id, "@kallow", 0, ImVec2(bx0, yIn), x0, yIn, pw, panelH});
  handles.list.push_back({node.id, "@kalhigh", 0, ImVec2(bx1, yIn), x0, yIn, pw, panelH});
  handles.list.push_back({node.id, "@kalmove", 0, ImVec2((bx0 + bx1) / 2, yIn + panelH / 2), x0, yIn, pw, panelH});

  spectrum(made, yOut, kOrange);
  panel(yOut, mode == 0 ? "OUTPUT  (the band arrayed, every second copy mirrored)" : mode == 1 ? "OUTPUT  (the band repeated)" : "OUTPUT  (the band copied to its overtones and undertones)");
  if (mode == 2) {
    const double stretch = valueOf(node, type, "stretch"), decay = valueOf(node, type, "decay");
    const int up = static_cast<int>(std::lround(valueOf(node, type, "overtones"))), down = static_cast<int>(std::lround(valueOf(node, type, "under")));
    for (int k = -down; k <= up; k++) {
      const double a = band.b0 + std::log2(kal::overtoneRatio(k, stretch)), b = a + band.width, g = kal::overtoneGain(k, decay);
      if (b < lo || a > hi) continue;
      const float xa = std::max(x0, X(a)), xb = std::min(x0 + pw, X(b));
      if (xb <= xa) continue;
      dl->AddRectFilled(ImVec2(xa, yOut), ImVec2(xb, yOut + panelH), withAlpha(kPink, static_cast<int>(26 * g + 4)));
      if (a > lo) dl->AddLine(ImVec2(std::round(xa) + 0.5f, yOut), ImVec2(std::round(xa) + 0.5f, yOut + panelH), withAlpha(kPink, static_cast<int>(50 + 130 * g)));
      char name[8] = "";
      if (k == 0) std::snprintf(name, sizeof name, "source"); else if (k == -1) std::snprintf(name, sizeof name, "x1/2"); else if (k > 0 && k < 7) std::snprintf(name, sizeof name, "x%d", k + 1);
      if (name[0] && a > lo) dl->AddText(font, f.size * 0.9f, ImVec2(xa + 3 * scale, yOut + 3 * scale), kPink, name);
    }
  } else {
    const int first = static_cast<int>(std::floor((lo - band.b0 - rotate * band.width) / band.width)), last = static_cast<int>(std::ceil((hi - band.b0 - rotate * band.width) / band.width));
    for (int k = first; k <= last; k++) {
      if (copies > 0 && std::abs(k) > copies) continue;
      const double a = band.b0 + rotate * band.width + k * band.width, b = a + band.width;
      const float xa = std::max(x0, X(a)), xb = std::min(x0 + pw, X(b));
      if (xb <= xa) continue;
      const bool flipped = mode == 0 && ((k % 2) + 2) % 2 == 1;
      if (flipped) dl->AddRectFilled(ImVec2(xa, yOut), ImVec2(xb, yOut + panelH), withAlpha(kPink, 24));
      for (const double o : {a, b}) if (o > lo && o < hi) dl->AddLine(ImVec2(std::round(X(o)) + 0.5f, yOut), ImVec2(std::round(X(o)) + 0.5f, yOut + panelH), withAlpha(kPink, 150));
      if (xb - xa > 56 * scale) {
        const char* name = k == 0 ? "source" : flipped ? "< flipped" : "copy";
        const float w = ImGui::CalcTextSize(name).x * f.size / ImGui::GetFontSize();
        dl->AddText(font, f.size * 0.9f, ImVec2((xa + xb) / 2 - w / 2, yOut + panelH - 16 * scale), kPink, name);
      }
    }
  }
  const auto hz = [](double v) { char t[24]; if (v >= 1000) std::snprintf(t, sizeof t, "%.2f kHz", v / 1000); else std::snprintf(t, sizeof t, "%d Hz", static_cast<int>(std::lround(v))); return std::string(t); };
  char text[160];
  if (mode == 2) std::snprintf(text, sizeof text, "band %s - %s (%.2f oct)   overtones: %d up, %d down", hz(band.lo).c_str(), hz(band.hi).c_str(), band.width, static_cast<int>(std::lround(valueOf(node, type, "overtones"))), static_cast<int>(std::lround(valueOf(node, type, "under"))));
  else std::snprintf(text, sizeof text, "band %s - %s (%.2f oct)   copies: %s", hz(band.lo).c_str(), hz(band.hi).c_str(), band.width, copies == 0 ? "fill" : std::to_string(copies).c_str());
  dl->AddText(font, f.size, ImVec2(f.x0 + 8 * scale, f.y0 + f.h - 16 * scale), textAlpha(190), text);
}

// ---- drum synth: the node's own code run offline, so the pictures show what is heard ----

// A waveform as a line for each pixel column (its smallest and largest value), about the middle of its box.
void drawWave(ImDrawList* dl, const std::vector<float>& data, float x, float y, float w, float h, ImU32 colour, std::size_t from, std::size_t to, float scale) {
  const float mid = y + h / 2, amp = h / 2 - 2 * scale;
  dl->AddLine(ImVec2(x, std::round(mid) + 0.5f), ImVec2(x + w, std::round(mid) + 0.5f), textAlpha(64));
  to = std::min(to, data.size());
  if (to <= from) return;
  const int columns = std::max(1, static_cast<int>(w));
  for (int px = 0; px < columns; px++) {
    const std::size_t a = from + static_cast<std::size_t>(px) * (to - from) / columns, b = std::max(a + 1, from + static_cast<std::size_t>(px + 1) * (to - from) / columns);
    float lo = 0, hi = 0;
    for (std::size_t i = a; i < b && i < to; i++) { lo = std::min(lo, data[i]); hi = std::max(hi, data[i]); }
    const float y0 = mid - std::min(1.0f, hi) * amp, y1 = mid - std::max(-1.0f, lo) * amp;
    dl->AddLine(ImVec2(x + px + 0.5f, y0), ImVec2(x + px + 0.5f, std::max(y1, y0 + 1)), colour);
  }
}

// Millisecond marks along the bottom of a panel.
void drawMsAxis(ImDrawList* dl, ImFont* font, float size, float x, float w, double ms, float base, float scale) {
  static const double nice[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000};
  double step = 1000;
  for (const double n : nice) if (ms / n <= 6) { step = n; break; }
  char text[16];
  for (double t = 0; t <= ms; t += step) {
    const float px = std::round(x + static_cast<float>(t / ms) * w) + 0.5f;
    dl->AddLine(ImVec2(px, base - 3 * scale), ImVec2(px, base), textAlpha(110));
    std::snprintf(text, sizeof text, "%d", static_cast<int>(t));
    dl->AddText(font, size * 0.85f, ImVec2(px - 4 * scale, base - 14 * scale), textAlpha(110), text);
  }
}

void drawDrum(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale) {
  const ImU32 kAmp = IM_COL32(235, 160, 45, 255), kPit = IM_COL32(60, 160, 230, 255), kNoise = IM_COL32(60, 190, 110, 255), kClick = IM_COL32(200, 100, 210, 255);
  const auto v = [&](const char* name) { return valueOf(node, type, name); };
  std::vector<float> values;
  for (const ParamSpec& spec : type.params) values.push_back(static_cast<float>(node.param(spec.name, spec.def)));
  const float pad = 8 * scale, left = f.x0 + 34 * scale, pw = f.w - 42 * scale;
  const auto title = [&](float y, const std::string& text, const std::string& right = "") {
    dl->AddLine(ImVec2(f.x0, y - 1), ImVec2(f.x0 + f.w, y - 1), textAlpha(25));
    dl->AddText(font, f.size, ImVec2(f.x0 + pad, y + 2 * scale), textAlpha(190), text.c_str());
    if (!right.empty()) dl->AddText(font, f.size, ImVec2(f.x0 + f.w - pad - ImGui::CalcTextSize(right.c_str()).x * f.size / ImGui::GetFontSize(), y + 2 * scale), textAlpha(190), right.c_str());
  };
  const auto hz = [](double x) { char t[24]; if (x >= 1000) std::snprintf(t, sizeof t, "%.1fk", x / 1000); else std::snprintf(t, sizeof t, "%d", static_cast<int>(std::lround(x))); return std::string(t); };
  const bool holdSome = v("sustain") > 0.02 || v("psus") > 0.02 || v("nsus") > 0.02;
  const float envY = f.y0 + 22 * scale, envH = 104 * scale, clickY = envY + envH, clickH = 78 * scale, noteY = clickY + clickH, noteH = 96 * scale, zoomY = noteY + noteH, zoomH = 60 * scale;

  // the envelopes: one time axis for the three, long enough for the slowest to fade
  const double span = std::min(2500.0, std::max(60.0, std::max({v("attack") + v("decay") * 0.9, v("patk") + v("pdec") * 0.9, v("natk") + v("ndec") * 0.9, 1.0}) * 1.15 + (holdSome ? 80 + std::max({v("release"), v("prel"), v("nrel")}) * 0.6 : 0)));
  const int total = static_cast<int>(std::lround(span)), hold = holdSome ? static_cast<int>(std::lround(span * 0.55)) : 100000;
  const std::vector<float> amp = adsrCurve(v("attack"), v("decay"), v("sustain"), v("release"), hold, total), pit = adsrCurve(v("patk"), v("pdec"), v("psus"), v("prel"), hold, total), noi = adsrCurve(v("natk"), v("ndec"), v("nsus"), v("nrel"), hold, total);
  title(envY - 18 * scale, "envelopes", hz(v("startPitch") * v("pitchMul")) + " > " + hz(v("endPitch") * v("pitchMul")) + " Hz");
  const float ey = envY + 6 * scale, eh = envH - 30 * scale;
  dl->AddLine(ImVec2(left, ey), ImVec2(left + pw, ey), textAlpha(45));
  dl->AddLine(ImVec2(left, ey + eh), ImVec2(left + pw, ey + eh), textAlpha(45));
  const auto line = [&](const std::vector<float>& values01, ImU32 colour, const std::function<double(double)>& map, float thick) {
    ImVec2 last{};
    for (std::size_t i = 0; i < values01.size(); i++) {
      const ImVec2 at(left + static_cast<float>(i) / std::max<std::size_t>(1, values01.size() - 1) * pw, ey + eh - static_cast<float>(map(values01[i])) * eh);
      if (i) dl->AddLine(last, at, colour, thick);
      last = at;
    }
  };
  line(amp, kAmp, [&](double x) { return x * v("body"); }, 2.0f * scale);
  line(noi, kNoise, [&](double x) { return x * v("noise"); }, 1.6f * scale);
  const double lo = std::min(v("startPitch"), v("endPitch")), hi = std::max(v("startPitch"), v("endPitch")), lg = std::log(std::max(hi / lo, 1.01));
  line(pit, kPit, [&](double x) { return std::log((v("endPitch") + (v("startPitch") - v("endPitch")) * x) / lo) / lg; }, 1.8f * scale);
  drawMsAxis(dl, font, f.size, left, pw, span, envY + envH, scale);
  float lx = f.x0 + 82 * scale;
  for (const auto& [name, colour] : {std::pair<const char*, ImU32>{"volume", kAmp}, {"pitch", kPit}, {"noise", kNoise}}) {
    dl->AddRectFilled(ImVec2(lx, envY - 14 * scale), ImVec2(lx + 8 * scale, envY - 11 * scale), colour);
    dl->AddText(font, f.size, ImVec2(lx + 11 * scale, envY - 18 * scale), textAlpha(190), name);
    lx += 11 * scale + 6.5f * scale * std::strlen(name) + 8 * scale;
  }

  // the click on its own (a click from a sample is not drawn: the picture has no sample)
  const bool sampleMode = v("clickMode") >= 1;
  const double clickMs = std::min(150.0, std::max(2.0, v("clickLen") * 1.4));
  char text[96];
  if (sampleMode) std::snprintf(text, sizeof text, "from a sample, %.1f ms, tail %.1f ms", v("clickLen"), v("clickTail"));
  else std::snprintf(text, sizeof text, "synthesized, %s Hz, %.1f ms", hz(v("clickTone")).c_str(), v("clickLen"));
  title(clickY, "click", text);
  std::vector<float> clickOnly = values;
  const auto set = [&](std::vector<float>& vals, const char* name, double value) { vals[static_cast<std::size_t>(type.paramIndex(name))] = static_cast<float>(value); };
  set(clickOnly, "body", 0); set(clickOnly, "noise", 0); set(clickOnly, "drive", 0); set(clickOnly, "level", 1); set(clickOnly, "vel", 0); set(clickOnly, "click", 1);
  if (!sampleMode) {
    drawWave(dl, renderOffline(type, clickOnly, 60, 1, clickMs, 44100, -1), left, clickY + 16 * scale, pw, clickH - 30 * scale, kClick, 0, 1 << 30, scale);
    if (!(v("click") > 0)) dl->AddText(font, f.size, ImVec2(left + 6 * scale, clickY + 18 * scale), textAlpha(150), "click amount is 0");
  } else dl->AddText(font, f.size, ImVec2(left + 6 * scale, clickY + 18 * scale), textAlpha(150), "(the sample's own wave is not drawn)");
  drawMsAxis(dl, font, f.size, left, pw, clickMs, clickY + clickH, scale);

  // the root note, whole and the first 25 ms (a click from a sample is left out: it is silent here)
  const double noteMs = std::min(1600.0, std::max(80.0, std::max({v("attack") + v("decay") * 0.85, v("natk") + v("ndec") * 0.85 * (v("noise") > 0.02 ? 1 : 0), v("release") * (holdSome ? 0.5 : 0)}) * 1.1 + (holdSome ? 120 : 0)));
  const std::vector<float> hit = renderOffline(type, values, static_cast<int>(v("root")), 1, noteMs, 44100, holdSome ? noteMs * 0.6 : -1);
  float peak = 0;
  for (const float x : hit) peak = std::max(peak, std::fabs(x));
  static const char* const kinds[] = {"white", "pink", "brown", "blue", "violet"};
  std::snprintf(text, sizeof text, "peak %.2f, %s noise", peak, kinds[std::clamp(static_cast<int>(std::lround(v("noiseType"))), 0, 4)]);
  title(noteY, "root note, composed", text);
  drawWave(dl, hit, left, noteY + 16 * scale, pw, noteH - 30 * scale, textAlpha(220), 0, hit.size(), scale);
  drawMsAxis(dl, font, f.size, left, pw, noteMs, noteY + noteH, scale);
  const double zoomMs = std::min(25.0, noteMs);
  std::snprintf(text, sizeof text, "first %d ms", static_cast<int>(std::lround(zoomMs)));
  title(zoomY, text);
  if (v("click") > 0) dl->AddRectFilled(ImVec2(left, zoomY + 14 * scale), ImVec2(left + static_cast<float>(std::min(v("clickLen"), zoomMs) / zoomMs) * pw, zoomY + zoomH - 2 * scale), withAlpha(kClick, 30));
  drawWave(dl, hit, left, zoomY + 14 * scale, pw, zoomH - 16 * scale, textAlpha(220), 0, static_cast<std::size_t>(44100 * zoomMs / 1000), scale);
}

// ---- pulsar, pulse train: a few periods of the train as it sounds, the masked ones faint ----

// One pulsaret's shape at q (0..1), as the oscillator plays it; the noise wave takes its numbers from `rand` so the picture holds still.
double pulsaretShape(int wave, double q, const std::function<double()>& rand) {
  switch (wave) {
    case 0: return std::sin(6.283185307179586 * q);
    case 1: return std::sin(3.141592653589793 * q) * 2 - 1.273;
    case 2: return q < 0.25 ? 4 * q : q < 0.75 ? 2 - 4 * q : 4 * q - 4;
    case 3: return 2 * q - 1;
    case 4: return q < 0.5 ? 1 : -1;
    default: return rand() * 2 - 1;
  }
}

void drawPulsarTrain(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale, ResponseHandles& handles) {
  constexpr int kPeriods = 5;
  constexpr double kShownHz = 220;   // A3
  const double formant = std::max(1.0, valueOf(node, type, "formant")), mask = valueOf(node, type, "mask");
  const int wave = static_cast<int>(std::lround(valueOf(node, type, "wave")));
  const bezier::Table window = bezier::envelopeTable(bezier::drawingOf(&node.data, "window", pulsar::defaultWindow()));
  const double duty = kShownHz / formant;   // how much of a period one pulsaret fills
  const float left = f.x0 + 8 * scale, width = f.w - 16 * scale, top = f.y0 + 22 * scale, height = f.h - 28 * scale;
  const float mid = top + height / 2, amp = height * 0.4f, per = width / kPeriods;
  dl->AddLine(ImVec2(left, mid), ImVec2(left + width, mid), textAlpha(50));
  double seed = 7;
  const std::function<double()> rand = [&]() { seed = std::fmod(seed * 16807, 2147483647.0); return seed / 2147483647.0; };
  for (int p = 0; p < kPeriods; p++) {
    const bool kept = !(mask > 0 && std::fmod(p * 0.618, 1.0) < mask);
    const double length = std::min(1.0, duty);   // a pulsaret longer than the period is cut off by the next one
    const int steps = std::max(8, static_cast<int>(std::lround(per * length / (1.5f * scale))));
    ImVec2 last{};
    for (int i = 0; i <= steps; i++) {
      const double q = static_cast<double>(i) / steps * length / duty;
      const double v = pulsaretShape(wave, q, rand) * bezier::readEnvelope(window, q);
      const ImVec2 at(left + p * per + static_cast<float>(i) / steps * static_cast<float>(length) * per, mid - static_cast<float>(v) * amp);
      if (i) dl->AddLine(last, at, kept ? IM_COL32(235, 160, 45, 255) : textAlpha(46), 1.3f * scale);
      last = at;
    }
  }
  // the first pulsaret's end: along the period is how much of it the pulsaret fills (the formant)
  const float endX = left + static_cast<float>(std::min(1.0, duty)) * per;
  dl->AddCircleFilled(ImVec2(endX, mid), 3.5f * scale, IM_COL32(235, 160, 45, 255));
  handles.list.push_back({node.id, "@pulsar", 0, ImVec2(endX, mid), left, 0, per, 1});
  char text[96];
  std::snprintf(text, sizeof text, "%d periods at 220 Hz · formant %d Hz · duty %d%%", kPeriods, static_cast<int>(std::lround(formant)), static_cast<int>(std::lround(duty * 100)));
  dl->AddText(font, f.size * 0.9f, ImVec2(f.x0 + 8 * scale, f.y0 + f.h - 14 * scale), textAlpha(130), text);
}

// ---- the drawn drums (drum synth 2, hi-hat 2): their drawings and the hit they make, from the node's own code ----

}  // namespace

bool hasDrawnHitGrid(const std::string& nodeType) { return nodeType == "drumsynth2" || nodeType == "hat2"; }

ImU32 drawingColour(const std::string& nodeType, const std::string& drawing) {
  if (nodeType == "drumsynth2") return drawing == "pitch" ? IM_COL32(60, 160, 230, 255) : drawing == "amp" ? IM_COL32(235, 160, 45, 255) : drawing == "noise" ? IM_COL32(60, 190, 110, 255) : 0;
  if (nodeType == "hat2") return drawing == "closed" ? IM_COL32(60, 190, 210, 255) : drawing == "open" ? IM_COL32(235, 160, 45, 255) : 0;
  return 0;
}

double drawingMs(const GraphNode& node, const NodeType& type, const std::string& drawing) {
  const auto rate = [&](const char* name) { return std::max(0.01, valueOf(node, type, name)); };
  if (node.type == "drumsynth2") return drawing == "pitch" ? 250 / rate("prate") : drawing == "amp" ? 700 / rate("arate") : 400 / rate("nrate");
  if (node.type == "hat2") return drawing == "closed" ? 140 / rate("crate") : 700 / rate("orate");
  return 0;
}

int drawnHitCount(const std::string& nodeType) { return nodeType == "hat2" ? 2 : 1; }

void drawHitCell(ImDrawList* dl, ImFont* font, float size, const GraphNode& node, const NodeType& type, ImVec2 min, ImVec2 max, float scale, int hit) {
  const bool hat = node.type == "hat2";
  const auto v = [&](const char* name) { return valueOf(node, type, name); };
  std::vector<float> values;
  for (const ParamSpec& spec : type.params) values.push_back(static_cast<float>(node.param(spec.name, spec.def)));
  const char* title = hat ? (hit == 0 ? "closed hat" : "open hat") : "the hit";
  const int note = hat ? hit : static_cast<int>(v("root"));
  const double ms = hat ? (hit == 0 ? std::max(40.0, drawingMs(node, type, "closed") * 1.1) : std::max(80.0, drawingMs(node, type, "open") * 1.1))
                        : std::min(1500.0, std::max(150.0, drawingMs(node, type, "amp") * 1.05));
  const std::vector<float> wave = renderOffline(type, values, note, 1, ms, 44100, -1, &node.data);
  float peak = 0;
  for (const float x : wave) peak = std::max(peak, std::fabs(x));
  char text[64];
  std::snprintf(text, sizeof text, "peak %.2f, %d ms", peak, static_cast<int>(std::lround(ms)));
  const float pad = 8 * scale, left = min.x + 34 * scale, pw = (max.x - min.x) - 42 * scale;
  dl->AddText(font, size, ImVec2(min.x + pad, min.y), textAlpha(190), title);
  dl->AddText(font, size, ImVec2(max.x - pad - ImGui::CalcTextSize(text).x * size / ImGui::GetFontSize(), min.y), textAlpha(150), text);
  drawWave(dl, wave, left, min.y + 14 * scale, pw, (max.y - min.y) - 18 * scale, textAlpha(220), 0, wave.size(), scale);
  drawMsAxis(dl, font, size, left, pw, ms, max.y - 4 * scale, scale);
}

namespace {

void drawDrawnHit(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale, ResponseHandles& handles) {
  const bool hat = node.type == "hat2";
  struct Row { const char* key; const char* title; };
  const std::vector<Row> rows = hat ? std::vector<Row>{{"closed", "closed hat"}, {"open", "open hat"}} : std::vector<Row>{{"pitch", "pitch"}, {"amp", "volume"}, {"noise", "noise"}};
  const float pad = 8 * scale, left = f.x0 + 34 * scale, pw = f.w - 42 * scale, rowH = 56 * scale;
  float y = f.y0 + 22 * scale;
  char text[64];
  for (const Row& row : rows) {   // the drawings, with how long each takes
    const ImU32 colour = drawingColour(node.type, row.key);
    const bezier::Table table = bezier::envelopeTable(bezier::drawingOf(&node.data, row.key, defaultDrawing(node.type, row.key)));
    dl->AddText(font, f.size, ImVec2(f.x0 + pad, y), textAlpha(190), row.title);
    const float titleW = ImGui::CalcTextSize(row.title).x * f.size / ImGui::GetFontSize();
    dl->AddText(font, f.size * 0.9f, ImVec2(f.x0 + pad + titleW + 8 * scale, y + 1 * scale), withAlpha(colour, 150), "click to draw");
    handles.list.push_back({node.id, "@draw", 0, ImVec2(f.x0 + pad + titleW / 2, y + f.size / 2), 0, 0, 1, 1});   // opens the drawings window
    std::snprintf(text, sizeof text, "%d ms", static_cast<int>(std::lround(drawingMs(node, type, row.key))));
    dl->AddText(font, f.size, ImVec2(f.x0 + f.w - pad - ImGui::CalcTextSize(text).x * f.size / ImGui::GetFontSize(), y), textAlpha(150), text);
    const float top = y + 14 * scale, h = rowH - 20 * scale;
    dl->AddLine(ImVec2(left, top + h), ImVec2(left + pw, top + h), textAlpha(45));
    ImVec2 last{};
    for (int i = 0; i <= 160; i++) {
      const ImVec2 at(left + pw * i / 160.0f, top + h - static_cast<float>(bezier::readEnvelope(table, i / 160.0)) * h);
      if (i) dl->AddLine(last, at, colour, 1.8f * scale);
      last = at;
    }
    y += rowH;
  }
  for (int hit = 0; hit < drawnHitCount(node.type); hit++) {   // the hit(s)
    dl->AddLine(ImVec2(f.x0, y - 2 * scale), ImVec2(f.x0 + f.w, y - 2 * scale), textAlpha(25));
    drawHitCell(dl, font, f.size, node, type, ImVec2(f.x0, y), ImVec2(f.x0 + f.w, y + 80 * scale), scale, hit);
    y += 80 * scale;
  }
}

// ---- twin: the two filters' curves and the pair, with the overtones above each cutoff ----

void drawTwin(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale, ResponseHandles& handles) {
  constexpr double kTop = 24, kBottom = -48, kFLo = twin::kFLow, kFHi = twin::kFHigh;
  const ImU32 teal = hsl(175, 0.6f, 0.52f), amber = hsl(40, 0.95f, 0.62f);
  const float px = f.x0 + 34 * scale, pw = f.w - 34 * scale - 14 * scale, py = f.y0 + 34 * scale, ph = f.h - 34 * scale - 40 * scale, mid = py + ph / 2;
  const twin::Get get = [&](const char* name) { return valueOf(node, type, name); };
  double f1, f2; twin::cutoffs(get, f1, f2);
  const auto xf = [&](double hz) { return px + static_cast<float>(std::log(hz / kFLo) / std::log(kFHi / kFLo)) * pw; };
  const auto yd = [&](double db) { return py + ph - static_cast<float>((std::clamp(db, kBottom, kTop) - kBottom) / (kTop - kBottom)) * ph; };
  const auto vline = [&](double hz, float y0, float y1, ImU32 colour) { const float x = std::round(xf(hz)) + 0.5f; dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), colour); };
  for (const double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) vline(hz, py, py + ph, textAlpha(hz == 100 || hz == 1000 || hz == 10000 ? 40 : 22));
  for (const auto& [hz, name] : {std::pair<double, const char*>{100, "100"}, {1000, "1k"}, {10000, "10k"}}) dl->AddText(font, f.size, ImVec2(xf(hz) - 6 * scale, py + ph + 4 * scale), textAlpha(170), name);
  dl->AddLine(ImVec2(px, std::round(yd(0)) + 0.5f), ImVec2(px + pw, std::round(yd(0)) + 0.5f), textAlpha(80));
  for (const int db : {24, 0, -24, -48}) { char t[8]; std::snprintf(t, sizeof t, "%s%d", db > 0 ? "+" : "", db); dl->AddText(font, f.size, ImVec2(f.x0 + 4 * scale, yd(db) - f.size / 2), textAlpha(150), t); }
  dl->AddText(font, f.size, ImVec2(f.x0 + 100 * scale, f.y0 + 4 * scale), teal, "filter 1");
  dl->AddText(font, f.size, ImVec2(f.x0 + 150 * scale, f.y0 + 4 * scale), amber, "filter 2");
  dl->AddText(font, f.size, ImVec2(f.x0 + 200 * scale, f.y0 + 4 * scale), textAlpha(170), "both");

  // the overtones: filter 1's from the top, filter 2's from the bottom, shared ones the full height
  const std::vector<twin::Shared> shared = twin::sharedOvertones(f1, f2);
  const auto hang = [&](double base, ImU32 colour, bool fromTop, bool first) {
    for (const twin::Overtone& o : twin::overtones(base)) {
      if (o.f < kFLo) continue;
      bool isShared = false;
      for (const twin::Shared& s : shared) if ((first ? s.a : s.b) == o.n) isShared = true;
      const int alpha = static_cast<int>(255 * std::max(0.12, 0.6 - o.n * 0.05));
      if (isShared) vline(o.f, py + 12 * scale, py + ph, textAlpha(180));
      else if (fromTop) vline(o.f, py + 12 * scale, mid, withAlpha(colour, alpha));
      else vline(o.f, mid, py + ph, withAlpha(colour, alpha));
      char t[8]; std::snprintf(t, sizeof t, "%dx", o.n);
      dl->AddText(font, f.size, ImVec2(xf(o.f) - 5 * scale, fromTop ? py + 1 * scale : py + ph - 12 * scale), isShared ? textAlpha(220) : withAlpha(colour, std::min(255, alpha + 50)), t);
    }
  };
  hang(f1, teal, true, true); hang(f2, amber, false, false);

  const int steps = std::max(8, static_cast<int>(pw));
  const auto curve = [&](const std::function<double(double)>& db, ImU32 colour, float width) {
    std::vector<ImVec2> line;
    for (int i = 0; i <= steps; i += 2) { const double hz = kFLo * std::pow(kFHi / kFLo, static_cast<double>(i) / steps); line.push_back(ImVec2(px + static_cast<float>(i) / steps * pw, yd(db(hz)))); }
    dl->AddPolyline(line.data(), static_cast<int>(line.size()), colour, 0, width);
  };
  curve([&](double hz) { return twin::filterDb(hz, get, 1); }, withAlpha(teal, 140), 1.0f);
  curve([&](double hz) { return twin::filterDb(hz, get, 2); }, withAlpha(amber, 140), 1.0f);
  curve([&](double hz) { return twin::responseDb(hz, get); }, kText, 2.0f * std::max(1.0f, scale));
  for (int n = 1; n <= 2; n++) {   // a dot on each cutoff: drag it sideways (while locked, the second sets the ratio)
    const double hz = n == 1 ? f1 : f2;
    const ImVec2 at(xf(hz), yd(twin::filterDb(hz, get, n)));
    dl->AddCircleFilled(at, 4.5f * scale, n == 1 ? teal : amber);
    handles.list.push_back({node.id, n == 1 ? "@twin1" : "@twin2", n, at, px, py, pw, ph});
  }
  char text[128];
  const bool locked = std::lround(get("lock")) == 1;
  std::snprintf(text, sizeof text, "filter 1 %.0f Hz   filter 2 %.0f Hz%s", f1, f2, locked ? "   (locked)" : "");
  dl->AddText(font, f.size, ImVec2(f.x0 + 8 * scale, f.y0 + f.h - 16 * scale), textAlpha(190), text);
  if (!shared.empty()) {
    std::snprintf(text, sizeof text, "%d shared, first at %.0f Hz", static_cast<int>(shared.size()), shared[0].f);
    dl->AddText(font, f.size, ImVec2(f.x0 + f.w - 150 * scale, f.y0 + f.h - 16 * scale), textAlpha(190), text);
  }
}

}  // namespace

bool hasPicture(const std::string& nodeType) { return nodeType == "twin" || nodeType == "prismacanyon" || nodeType == "ensemble" || nodeType == "haas" || nodeType == "reflect" || nodeType == "rotary" || nodeType == "khufu" || nodeType == "membrane" || nodeType == "granular" || nodeType == "kaleidoscope" || nodeType == "drumsynth" || nodeType == "drumsynth2" || nodeType == "hat2" || nodeType == "pulsar" || nodeType == "pulsetrain" || nodeType == "pogue" || nodeType == "cardinal" || spacePictureSize(nodeType).w > 0; }

PictureSize pictureSize(const std::string& nodeType) {
  if (const PictureSize space = spacePictureSize(nodeType); space.w > 0) return space;
  static const std::map<std::string, PictureSize> sizes = {
      {"prismacanyon", {360, 20 + 30 * kCanyonLines + 24}}, {"twin", {480, 260}}, {"ensemble", {320, 150}}, {"haas", {600, 280}}, {"cardinal", {kCardinalWidth, kCardinalHeight}}, {"reflect", {560, 384}},
      {"granular", {360, 262}}, {"membrane", {360, 196}}, {"khufu", {310, 150}}, {"rotary", {300, 190}}, {"kaleidoscope", {460, 270}}, {"drumsynth", {360, 372}},
      {"drumsynth2", {360, 22 + 3 * 56 + 84}}, {"pulsar", {360, 130}}, {"pulsetrain", {360, 130}}, {"pogue", kPogueSize}, {"hat2", {360, 22 + 2 * 56 + 2 * 80 + 6}}};
  const auto found = sizes.find(nodeType);
  return found == sizes.end() ? PictureSize{} : found->second;
}

float drawNodePicture(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 tileMin, ImVec2 tileMax, ImVec2 areaMin, ImVec2 areaMax, float scale, const PictureTiming& timing, ResponseHandles& handles) {
  gBelow = 0;
  const PictureSize size = pictureSize(node.type);
  if (node.type == "prismacanyon") drawCanyon(dl, font, node, type, frameFor(dl, font, node.id + " echoes", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale), scale, timing, handles);
  else if (node.type == "twin") drawTwin(dl, font, node, type, frameFor(dl, font, node.id + " response", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale), scale, handles);
  else if (node.type == "ensemble") drawEnsemble(dl, font, node, type, frameFor(dl, font, node.id + " delays", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale), scale, handles);
  else if (node.type == "haas") drawHaas(dl, font, node, type, frameFor(dl, font, node.id + " sides", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale), scale, handles);
  else if (node.type == "reflect") drawReflect(dl, font, node, type, frameFor(dl, font, node.id + " room", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale), scale, handles);
  else if (node.type == "granular") drawGranular(dl, font, node, type, frameFor(dl, font, node.id + " grains", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale), scale, timing, handles);
  else if (node.type == "membrane") drawMembrane(dl, font, node, type, frameFor(dl, font, node.id + " membrane", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale), scale, handles);
  else if (node.type == "khufu") drawKhufu(dl, font, node, type, frameFor(dl, font, node.id + " registers", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale), scale, handles);
  else if (node.type == "rotary") drawRotary(dl, font, node, type, frameFor(dl, font, node.id + " cabinet", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale), scale, handles);
  else if (node.type == "kaleidoscope") drawKaleidoscope(dl, font, node, type, frameFor(dl, font, node.id + " band", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale), scale, handles, timing);
  else if (node.type == "drumsynth") drawDrum(dl, font, node, type, frameFor(dl, font, node.id + " hit", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale), scale);
  else if (node.type == "drumsynth2") drawDrawnHit(dl, font, node, type, frameFor(dl, font, node.id + " drawings", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale), scale, handles);
  else if (node.type == "pulsar" || node.type == "pulsetrain") drawPulsarTrain(dl, font, node, type, frameFor(dl, font, node.id + " pulse train", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale), scale, handles);
  else if (node.type == "cardinal") { const Frame f = frameFor(dl, font, "", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale); drawCardinal(dl, font, node, type, ImVec2(f.x0, f.y0), scale, handles); }
  else if (node.type == "pogue") { const Frame f = frameFor(dl, font, "", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale); drawPogue(dl, font, node, type, ImVec2(f.x0, f.y0), scale, timing, handles); }
  else if (spacePictureSize(node.type).w > 0) { const Frame f = frameFor(dl, font, "", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale); drawSpacePicture(dl, font, node, type, ImVec2(f.x0, f.y0), scale, timing, handles); }
  else if (node.type == "hat2") drawDrawnHit(dl, font, node, type, frameFor(dl, font, node.id + " drawings", size.w, size.h, tileMin, tileMax, areaMin, areaMax, scale), scale, handles);
  return gBelow;
}

std::vector<std::pair<std::string, double>> dragPictureHandle(const ResponseHandles::Handle& handle, const GraphNode& node, const NodeType& type, ImVec2 at) {
  const auto clampTo = [&](const std::string& name, double v) {
    for (const ParamSpec& spec : type.params) if (spec.name == name) return std::min(spec.max, std::max(spec.min, v));
    return v;
  };
  const double fx = std::clamp(static_cast<double>((at.x - handle.gx) / handle.gw), 0.0, 1.0), fy = std::clamp(static_cast<double>((at.y - handle.gy) / handle.gh), 0.0, 1.0);
  if (handle.param.rfind("@card", 0) == 0) return dragCardinalHandle(handle, node, type, at);
  if (handle.param.rfind("@pog", 0) == 0) return dragPogueHandle(handle, node, type, at);
  if (handle.param.rfind("@sp.", 0) == 0) return dragSpaceHandle(handle, node, type, at);
  if (handle.param.rfind("@set:", 0) == 0) {   // a button: set a parameter
    const std::size_t eq = handle.param.find('=');
    return {{handle.param.substr(5, eq - 5), clampTo(handle.param.substr(5, eq - 5), std::stod(handle.param.substr(eq + 1)))}};
  }
  if (handle.param.rfind("@flip:", 0) == 0) {   // a button: switch a 0/1 parameter
    const std::string name = handle.param.substr(6);
    return {{name, valueOf(node, type, name) >= 0.5 ? 0.0 : 1.0}};
  }
  if (handle.param.rfind("@nudge:", 0) == 0) {   // a button: add to a parameter, within its range
    const std::size_t eq = handle.param.find('=');
    const std::string name = handle.param.substr(7, eq - 7);
    return {{name, clampTo(name, valueOf(node, type, name) + std::stod(handle.param.substr(eq + 1)))}};
  }
  if (handle.param == "@pulsar") return {{"formant", clampTo("formant", std::round(220 / std::max(0.02, fx) ))}};
  if (handle.param == "@gden") return {{"density", clampTo("density", std::round(1000 / std::max(1.0f, (at.x - handle.gx) / handle.gw)))}};
  if (handle.param == "@ens") {   // gx, gy, gw, gh: the plot; 4 s across, 3.5 to 8.5 ms up
    const double t = std::max(0.02, fx * 4), ms = 3.5 + (1 - fy) * 5;
    return {{"slow", clampTo("slow", std::round(1 / (4 * t) * 100) / 100)}, {"depth", clampTo("depth", std::round(std::fabs(ms - 6) / 1.6 * 100) / 100)}};
  }
  if (handle.param.rfind("@map:", 0) == 0) return dragMapHandle(handle, type, at);
  if (handle.param == "@haas") return {{"time", clampTo("time", std::round(fx * 45 * 10) / 10)}, {"gain", clampTo("gain", std::round(std::clamp(static_cast<double>((handle.gy - at.y) / handle.gh), 0.0, 1.2) * 100) / 100)}};
  if (handle.param == "@src") return {{"srcx", fx}, {"srcy", fy}};
  if (handle.param == "@lis") return {{"lisx", fx}, {"lisy", fy}};
  if (handle.param == "@size") {   // the corner: pixels per metre is the plan's width over the room's width
    const double k = handle.gw / std::max(0.1, valueOf(node, type, "width"));
    return {{"width", clampTo("width", std::round((at.x - handle.gx) / k * 2) / 2)}, {"depth", clampTo("depth", std::round((at.y - handle.gy) / k * 2) / 2)}};
  }
  if (handle.param == "@twin1" || handle.param == "@twin2") {   // along the axis: filter 1's cutoff, filter 2's, or (locked) the ratio between them
    const double hz = std::clamp(twin::kFLow * std::pow(twin::kFHigh / twin::kFLow, fx), 20.0, 18000.0);
    if (handle.param == "@twin1") return {{"cutoff1", std::round(hz)}};
    if (std::lround(valueOf(node, type, "lock")) != 1) return {{"cutoff2", std::round(hz)}};
    const twin::Get get = [&](const char* name) { return valueOf(node, type, name); };
    double f1, f2; twin::cutoffs(get, f1, f2);
    return {{"ratio", clampTo("ratio", std::round(hz / f1 / std::pow(2.0, valueOf(node, type, "detune") / 1200) * 1000) / 1000)}};
  }
  if (handle.param.rfind("@kal", 0) == 0) {   // an end of the band, or the band as a whole (its width stays); the picture spans 27.5 Hz to 14 kHz
    const double lo = std::log2(27.5), hi = std::log2(14080), octave = lo + fx * (hi - lo);
    const kal::Band band = kal::bandOf(valueOf(node, type, "low"), valueOf(node, type, "high"));
    double b0 = band.b0, b1 = band.b0 + band.width;
    const double lowest = std::log2(kal::kLowMin), highest = std::log2(kal::kHighMax);
    if (handle.param == "@kallow") b0 = std::min(b1 - 0.34, std::max(lowest, octave));
    else if (handle.param == "@kalhigh") b1 = std::max(b0 + 0.34, std::min(highest, octave));
    else { const double shift = std::max(lowest - b0, std::min(highest - b1, octave - (b0 + b1) / 2)); b0 += shift; b1 += shift; }
    return {{"low", clampTo("low", std::round(std::pow(2.0, b0)))}, {"high", clampTo("high", std::round(std::pow(2.0, b1)))}};
  }
  if (handle.param.rfind("@mode", 0) == 0) { pickedModes()[node.id] = handle.param.substr(5); return {}; }
  if (handle.param.rfind("@speed", 0) == 0) return {{"speed", static_cast<double>(handle.param.back() - '0')}};
  return {};
}

}  // namespace wax::app

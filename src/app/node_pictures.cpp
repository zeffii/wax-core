#include "app/node_pictures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "app/theme.h"
#include "nodes/membrane_view.h"
#include "nodes/spacecore.h"

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

Frame frameFor(ImDrawList* dl, ImFont* font, const std::string& title, float w, float h, ImVec2 tileMin, ImVec2 tileMax, ImVec2 areaMin, ImVec2 areaMax, float scale) {
  w *= scale; h *= scale;
  const float x0 = std::min(std::max(areaMin.x + 4 * scale, (tileMin.x + tileMax.x) / 2 - w / 2), std::max(areaMin.x + 4 * scale, areaMax.x - w - 4 * scale));
  const float below = tileMax.y + 16 * scale;
  const float y0 = below + h <= areaMax.y - 4 * scale ? below : std::max(areaMin.y + 4 * scale, tileMin.y - h - 22 * scale);
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), kBg);
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), IM_COL32(128, 128, 128, 41));
  dl->AddRect(ImVec2(x0 + 0.5f, y0 + 0.5f), ImVec2(x0 + w - 0.5f, y0 + h - 0.5f), IM_COL32(128, 128, 128, 178));
  const float size = 11 * scale;
  dl->AddText(font, size, ImVec2(x0 + 8 * scale, y0 + 4 * scale), kText, title.c_str());
  return {x0, y0, w, h, size};
}

// ---- canyon: four delay lines against the song's ticks ----

constexpr int kCanyonLines = 4, kCanyonRepeats = 8;

void drawCanyon(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale, const PictureTiming& timing) {
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
    }
    // the pan: a short track at the right, the dot left or right of its middle, and the cutoff the line is left with
    const float px = f.x0 + f.w - PR + 14 * scale, pc = px + static_cast<float>(pans[static_cast<std::size_t>(k - 1)]) * 9 * scale;
    dl->AddLine(ImVec2(px - 10 * scale, rowTop + 9.5f * scale), ImVec2(px + 10 * scale, rowTop + 9.5f * scale), textAlpha(77));
    dl->AddCircleFilled(ImVec2(pc, rowTop + 9.5f * scale), 3 * scale, hsl(hues[k - 1], 0.75f, 0.6f));
    const double hz = cutoff * std::pow(2.0, -4 * damps[static_cast<std::size_t>(k - 1)]);
    if (hz >= 1000) std::snprintf(text, sizeof text, hz >= 10000 ? "%.0fk" : "%.1fk", hz / 1000); else std::snprintf(text, sizeof text, "%d", static_cast<int>(std::lround(hz)));
    dl->AddText(font, f.size, ImVec2(px - 11 * scale, rowTop + 15 * scale), textAlpha(140), text);
  }
}

// ---- ensemble: how long each of three delays is over a few seconds ----

void drawEnsemble(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale) {
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
}

// ---- haas: the early and late pulses, and the comb they make in mono ----

void drawHaas(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale) {
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

void drawKhufu(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, const Frame& f, float scale) {
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

}  // namespace

bool hasPicture(const std::string& nodeType) { return nodeType == "prismacanyon" || nodeType == "ensemble" || nodeType == "haas" || nodeType == "reflect" || nodeType == "rotary" || nodeType == "khufu" || nodeType == "membrane"; }

void drawNodePicture(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 tileMin, ImVec2 tileMax, ImVec2 areaMin, ImVec2 areaMax, float scale, const PictureTiming& timing, ResponseHandles& handles) {
  if (node.type == "prismacanyon") drawCanyon(dl, font, node, type, frameFor(dl, font, node.id + " echoes", 360, 20 + 30 * kCanyonLines + 24, tileMin, tileMax, areaMin, areaMax, scale), scale, timing);
  else if (node.type == "ensemble") drawEnsemble(dl, font, node, type, frameFor(dl, font, node.id + " delays", 320, 150, tileMin, tileMax, areaMin, areaMax, scale), scale);
  else if (node.type == "haas") drawHaas(dl, font, node, type, frameFor(dl, font, node.id + " sides", 400, 280, tileMin, tileMax, areaMin, areaMax, scale), scale);
  else if (node.type == "reflect") drawReflect(dl, font, node, type, frameFor(dl, font, node.id + " room", 560, 384, tileMin, tileMax, areaMin, areaMax, scale), scale, handles);
  else if (node.type == "membrane") drawMembrane(dl, font, node, type, frameFor(dl, font, node.id + " membrane", 360, 196, tileMin, tileMax, areaMin, areaMax, scale), scale, handles);
  else if (node.type == "khufu") drawKhufu(dl, font, node, type, frameFor(dl, font, node.id + " registers", 310, 150, tileMin, tileMax, areaMin, areaMax, scale), scale);
  else if (node.type == "rotary") drawRotary(dl, font, node, type, frameFor(dl, font, node.id + " cabinet", 300, 190, tileMin, tileMax, areaMin, areaMax, scale), scale, handles);
}

std::vector<std::pair<std::string, double>> dragPictureHandle(const ResponseHandles::Handle& handle, const GraphNode& node, const NodeType& type, ImVec2 at) {
  const auto clampTo = [&](const std::string& name, double v) {
    for (const ParamSpec& spec : type.params) if (spec.name == name) return std::min(spec.max, std::max(spec.min, v));
    return v;
  };
  const double fx = std::clamp(static_cast<double>((at.x - handle.gx) / handle.gw), 0.0, 1.0), fy = std::clamp(static_cast<double>((at.y - handle.gy) / handle.gh), 0.0, 1.0);
  if (handle.param == "@src") return {{"srcx", fx}, {"srcy", fy}};
  if (handle.param == "@lis") return {{"lisx", fx}, {"lisy", fy}};
  if (handle.param == "@size") {   // the corner: pixels per metre is the plan's width over the room's width
    const double k = handle.gw / std::max(0.1, valueOf(node, type, "width"));
    return {{"width", clampTo("width", std::round((at.x - handle.gx) / k * 2) / 2)}, {"depth", clampTo("depth", std::round((at.y - handle.gy) / k * 2) / 2)}};
  }
  if (handle.param.rfind("@mode", 0) == 0) { pickedModes()[node.id] = handle.param.substr(5); return {}; }
  if (handle.param.rfind("@speed", 0) == 0) return {{"speed", static_cast<double>(handle.param.back() - '0')}};
  return {};
}

}  // namespace wax::app

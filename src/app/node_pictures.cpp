#include "app/node_pictures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "app/theme.h"

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

}  // namespace

bool hasPicture(const std::string& nodeType) { return nodeType == "prismacanyon" || nodeType == "ensemble" || nodeType == "haas"; }

void drawNodePicture(ImDrawList* dl, ImFont* font, const GraphNode& node, const NodeType& type, ImVec2 tileMin, ImVec2 tileMax, ImVec2 areaMin, ImVec2 areaMax, float scale, const PictureTiming& timing) {
  if (node.type == "prismacanyon") drawCanyon(dl, font, node, type, frameFor(dl, font, node.id + " echoes", 360, 20 + 30 * kCanyonLines + 24, tileMin, tileMax, areaMin, areaMax, scale), scale, timing);
  else if (node.type == "ensemble") drawEnsemble(dl, font, node, type, frameFor(dl, font, node.id + " delays", 320, 150, tileMin, tileMax, areaMin, areaMax, scale), scale);
  else if (node.type == "haas") drawHaas(dl, font, node, type, frameFor(dl, font, node.id + " sides", 400, 280, tileMin, tileMax, areaMin, areaMax, scale), scale);
}

}  // namespace wax::app

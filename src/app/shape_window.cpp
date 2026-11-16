#include "app/shape_window.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>

#include "app/theme.h"
#include "app/widgets.h"

namespace wax::app {
namespace {

constexpr int kWavePoints = 128, kCurvePoints = 129;
constexpr double kTau = 6.283185307179586;

const GraphNode* findNode(const Project& project, const std::string& instrumentId, const std::string& nodeId) {
  for (const Instrument& instrument : project.instruments) {
    if (instrument.id != instrumentId) continue;
    for (const GraphNode& node : instrument.graph.nodes) if (node.id == nodeId) return &node;
  }
  return nullptr;
}

bool isWave(const std::string& type) { return type == "osc" || type == "westcoast" || type == "fmsynth"; }

std::vector<double> sineWave() {
  std::vector<double> w(kWavePoints);
  for (int i = 0; i < kWavePoints; i++) w[static_cast<std::size_t>(i)] = std::sin(kTau * i / kWavePoints);
  return w;
}

std::vector<double> softCurve() {
  std::vector<double> c(kCurvePoints);
  for (int i = 0; i < kCurvePoints; i++) { const double x = 2.0 * i / (kCurvePoints - 1) - 1; c[static_cast<std::size_t>(i)] = std::tanh(2 * x) / std::tanh(2.0); }
  return c;
}

// The node's stored points, resampled to `count` (a wave wraps round, a curve does not); the default when it has none.
std::vector<double> pointsOf(const GraphNode& node, const char* key, int count, bool wrap, const std::vector<double>& fallback) {
  std::vector<double> given;
  if (node.data.is_object() && node.data.contains(key) && node.data[key].is_array())
    for (const auto& v : node.data[key]) given.push_back(v.is_number() ? v.get<double>() : 0.0);
  if (given.size() < 2) return fallback;
  if (static_cast<int>(given.size()) == count) return given;
  std::vector<double> out(static_cast<std::size_t>(count));
  const int n = static_cast<int>(given.size());
  for (int i = 0; i < count; i++) {
    const double p = wrap ? static_cast<double>(i) / count * n : static_cast<double>(i) / (count - 1) * (n - 1);
    const int k = static_cast<int>(std::floor(p));
    const double f = p - k;
    const double a = given[static_cast<std::size_t>(k % n)], b = given[static_cast<std::size_t>(std::min(wrap ? (k + 1) % n : k + 1, n - 1))];
    out[static_cast<std::size_t>(i)] = std::clamp(a * (1 - f) + b * f, -1.0, 1.0);
  }
  return out;
}

struct Preset { const char* name; std::function<std::vector<double>()> make; };

// The delay's taps (data.taps, a flat list [time, level, pan, ...] with time a fraction of the delay's length): a stem for each tap (time along,
// level as height; the part right of 1 is where the feedback repeats them) and below it a dot for its pan. Click empty space to add a tap, drag a
// stem to move it, drag a pan dot up or down, double-click a stem to remove it (a pan dot: to centre it).
bool drawTaps(ShapeWindow& window, Document& doc, const GraphNode& node) {
  constexpr int kMaxTaps = 16;
  constexpr double kSpan = 1.5;
  struct Tap { double t, l, p; };
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextWindowSize(ImVec2(440 * scale, 260 * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(240 * scale, 120 * scale), ImGuiCond_FirstUseEver);
  const std::string title = "Taps  " + window.instrument + "." + window.node + "###shape";
  bool changed = false;
  if (ImGui::Begin(title.c_str(), &window.open, ImGuiWindowFlags_NoSavedSettings)) {
    std::vector<Tap> taps;
    std::vector<double> flat;
    if (node.data.is_object() && node.data.contains("taps") && node.data["taps"].is_array())
      for (const auto& v : node.data["taps"]) flat.push_back(v.is_number() ? v.get<double>() : 0.0);
    if (flat.size() < 3) for (int i = 0; i < 6; i++) { flat.push_back((i + 1) / 6.0); flat.push_back(std::round(std::pow(0.7, i) * 1000) / 1000); flat.push_back(i % 2 ? 0.6 : -0.6); }
    for (std::size_t i = 0; i + 2 < flat.size() && taps.size() < kMaxTaps; i += 3) taps.push_back({std::clamp(flat[i], 0.001, 1.0), std::clamp(flat[i + 1], 0.0, 1.0), std::clamp(flat[i + 2], -1.0, 1.0)});
    const auto emit = [&](const std::vector<Tap>& list, bool fold) {
      Json out = Json::array();
      for (const Tap& k : list) { out.push_back(std::round(k.t * 10000) / 10000); out.push_back(std::round(k.l * 1000) / 1000); out.push_back(std::round(k.p * 1000) / 1000); }
      doc.apply(std::make_unique<SetNodeData>(window.instrument, window.node, "taps", out, "taps"), fold);
      changed = true;
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(100.0f * scale, ImGui::GetContentRegionAvail().y - 34 * scale));
    const ImVec2 min = ImGui::GetCursorScreenPos(), max(min.x + size.x, min.y + size.y);
    ImGui::InvisibleButton("taps", size);
    const float pad = 8 * scale, levelTop = min.y + 18 * scale, levelH = (size.y - 18 * scale) * 0.7f, panTop = levelTop + levelH + 10 * scale, panH = max.y - panTop;
    const float plotW = size.x - 2 * pad;
    const auto xOf = [&](double t) { return min.x + pad + static_cast<float>(t / kSpan) * plotW; };
    const auto yLevel = [&](double l) { return levelTop + levelH - static_cast<float>(l) * levelH; };
    const auto yPan = [&](double p) { return panTop + static_cast<float>((p + 1) / 2) * panH; };
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // the tap nearest the mouse, in the lane it is over
    int nearest = -1, lane = 0;
    float best = 10 * scale;
    for (std::size_t i = 0; i < taps.size(); i++) {
      const float dx = std::fabs(xOf(taps[i].t) - mouse.x);
      if (mouse.y >= levelTop - 6 * scale && mouse.y <= levelTop + levelH + 6 * scale) {
        const float d = std::hypot(dx, std::fabs(mouse.y - yLevel(taps[i].l)) * 0.35f);
        if (d < best && mouse.y <= yLevel(0) + 6 * scale) { best = d; nearest = static_cast<int>(i); lane = 1; }
      } else if (mouse.y >= panTop - 6 * scale && mouse.y <= max.y + 6 * scale) {
        const float d = std::hypot(dx, mouse.y - yPan(taps[i].p));
        if (d < best) { best = d; nearest = static_cast<int>(i); lane = 2; }
      }
    }
    const auto move = [&](Tap& k, int where) {
      if (where == 1) { k.t = std::clamp(static_cast<double>((mouse.x - min.x - pad) / plotW) * kSpan, 0.001, 1.0); k.l = std::clamp(static_cast<double>((levelTop + levelH - mouse.y) / levelH), 0.0, 1.0); }
      else k.p = std::clamp(static_cast<double>((mouse.y - panTop) / panH) * 2 - 1, -1.0, 1.0);
    };
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && nearest >= 0) {   // remove a tap (a pan dot: centre it)
      if (lane == 2) taps[static_cast<std::size_t>(nearest)].p = 0;
      else if (taps.size() > 1) taps.erase(taps.begin() + nearest);
      window.grabTap = -1;
      emit(taps, false);
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      if (nearest < 0 && mouse.y >= levelTop && mouse.y <= levelTop + levelH && taps.size() < kMaxTaps) {   // empty space in the level lane: a new tap
        taps.push_back({0.5, 0.5, 0});
        nearest = static_cast<int>(taps.size()) - 1; lane = 1;
        move(taps.back(), 1);
        emit(taps, false);
        window.dragging = true;
      }
      window.grabTap = nearest; window.grabLane = lane;
      if (nearest >= 0 && !window.dragging) window.dragging = false;
    } else if (window.grabTap >= 0 && window.grabTap < static_cast<int>(taps.size()) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      move(taps[static_cast<std::size_t>(window.grabTap)], window.grabLane);
      emit(taps, window.dragging);
      window.dragging = true;
    } else { window.grabTap = -1; window.dragging = false; }

    for (int q = 0; q <= 4; q++) dl->AddLine(ImVec2(std::round(xOf(q / 4.0)) + 0.5f, levelTop), ImVec2(std::round(xOf(q / 4.0)) + 0.5f, max.y), textAlpha(q == 0 || q == 4 ? 100 : 38));
    dl->AddRectFilled(ImVec2(xOf(1) + 1, levelTop), ImVec2(xOf(kSpan), max.y), textAlpha(18));   // the feedback area
    dl->AddText(ImVec2(min.x + pad, min.y + 2 * scale), textAlpha(140), "0");
    dl->AddText(ImVec2(xOf(1) + 6 * scale, min.y + 2 * scale), textAlpha(140), "repeats");
    for (std::size_t i = 0; i < taps.size(); i++) {
      const float f = static_cast<float>((taps[i].p + 1) / 2);
      const ImU32 colour = IM_COL32(static_cast<int>(60 + 180 * f), 150, static_cast<int>(230 - 180 * f), 255);
      const bool hot = static_cast<int>(i) == nearest || static_cast<int>(i) == window.grabTap;
      const float x = std::round(xOf(taps[i].t)) + 0.5f;
      dl->AddLine(ImVec2(x, yLevel(0)), ImVec2(x, yLevel(taps[i].l)), colour, hot ? 3.0f : 2.0f);
      dl->AddCircleFilled(ImVec2(x, yLevel(taps[i].l)), (hot ? 5 : 3.5f) * scale, colour);
      dl->AddCircleFilled(ImVec2(x, yPan(taps[i].p)), (hot ? 5 : 3.5f) * scale, colour);
      for (int k = 1; k * taps[i].t < kSpan - 1e-9 && k < 4; k++) {   // where the feedback repeats it, fading
        const float rx = xOf(taps[i].t * (k + 1) > kSpan ? kSpan : taps[i].t * (k + 1));
        if (taps[i].t * (k + 1) > kSpan) break;
        dl->AddLine(ImVec2(rx, yLevel(0)), ImVec2(rx, yLevel(taps[i].l * std::pow(0.5, k))), withAlpha(colour, 90), 1.0f);
      }
    }
    struct Preset { const char* name; std::vector<Tap> taps; };
    std::vector<Tap> even, pingpong, accel, random;
    for (int i = 0; i < 8; i++) even.push_back({(i + 1) / 8.0, std::pow(0.75, i), i % 2 ? 0.7 : -0.7});
    for (int i = 0; i < 6; i++) pingpong.push_back({(i + 1) / 6.0, std::pow(0.8, i), i % 2 ? 1.0 : -1.0});
    for (int i = 0; i < 10; i++) accel.push_back({std::pow((i + 1) / 10.0, 1.8), std::pow(0.85, i) * 0.9, std::sin(i * 1.9) * 0.8});
    const int count = 5 + std::rand() % 8;
    for (int i = 0; i < count; i++) random.push_back({0.05 + 0.95 * std::rand() / RAND_MAX, 0.2 + 0.8 * std::rand() / RAND_MAX, 2.0 * std::rand() / RAND_MAX - 1});
    std::vector<Tap> standard;
    for (int i = 0; i < 6; i++) standard.push_back({(i + 1) / 6.0, std::round(std::pow(0.7, i) * 1000) / 1000, i % 2 ? 0.6 : -0.6});
    const Preset presets[] = {{"even", even}, {"ping-pong", pingpong}, {"accel", accel}, {"random", random}, {"default", standard}};
    for (const Preset& preset : presets) {
      if (textButton(preset.name)) emit(preset.taps, false);
      ImGui::SameLine(0, 14 * scale);
    }
    ImGui::NewLine();
  }
  ImGui::End();
  return changed;
}

// The additive synth's levels: one bar per partial (32), the fundamental on the left, each from 0 to 1 (data.spectrum).
bool drawSpectrum(ShapeWindow& window, Document& doc, const GraphNode& node) {
  constexpr int kHarmonics = 32;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextWindowSize(ImVec2(520 * scale, 220 * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(240 * scale, 120 * scale), ImGuiCond_FirstUseEver);
  const std::string title = "Spectrum  " + window.instrument + "." + window.node + "###shape";
  bool changed = false;
  if (ImGui::Begin(title.c_str(), &window.open, ImGuiWindowFlags_NoSavedSettings)) {
    std::vector<double> bars;   // as stored (a shorter list repeats its last value), else a saw
    if (node.data.is_object() && node.data.contains("spectrum") && node.data["spectrum"].is_array())
      for (const auto& v : node.data["spectrum"]) bars.push_back(v.is_number() ? std::clamp(v.get<double>(), 0.0, 1.0) : 0.0);
    if (bars.empty()) for (int i = 0; i < kHarmonics; i++) bars.push_back(1.0 / (i + 1));
    while (static_cast<int>(bars.size()) < kHarmonics) bars.push_back(bars.back());
    bars.resize(kHarmonics);
    const auto emit = [&](const std::vector<double>& values, bool fold) {
      Json out = Json::array();
      for (const double v : values) out.push_back(std::round(v * 1000) / 1000);
      doc.apply(std::make_unique<SetNodeData>(window.instrument, window.node, "spectrum", out, "spectrum"), fold);
      changed = true;
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(60.0f * scale, ImGui::GetContentRegionAvail().y - 40 * scale));
    const ImVec2 min = ImGui::GetCursorScreenPos(), max(min.x + size.x, min.y + size.y);
    ImGui::InvisibleButton("bars", size);
    const float barW = size.x / kHarmonics;
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      const int index = std::clamp(static_cast<int>(std::floor((mouse.x - min.x) / barW)), 0, kHarmonics - 1);
      const double v = std::clamp(1.0 - static_cast<double>((mouse.y - min.y) / (size.y - 2)), 0.0, 1.0);
      if (window.lastIndex < 0) bars[static_cast<std::size_t>(index)] = v;
      else {   // the bars passed over since the last mouse event
        const int step = index >= window.lastIndex ? 1 : -1, span = std::max(1, std::abs(index - window.lastIndex));
        for (int j = window.lastIndex, s = 0; s <= span; j += step, s++) bars[static_cast<std::size_t>(j)] = window.lastValue + (v - window.lastValue) * s / span;
      }
      window.lastIndex = index; window.lastValue = v;
      emit(bars, window.dragging);
      window.dragging = true;
    } else { window.dragging = false; window.lastIndex = -1; }
    for (int i = 0; i < kHarmonics; i++) {
      const float x = min.x + static_cast<float>(i) * barW, top = max.y - static_cast<float>(bars[static_cast<std::size_t>(i)]) * (size.y - 2);
      dl->AddRectFilled(ImVec2(x + 1, min.y), ImVec2(x + barW - 1, max.y), IM_COL32(128, 128, 128, 30));
      dl->AddRectFilled(ImVec2(x + 1, top), ImVec2(x + barW - 1, max.y), IM_COL32(235, 150, 60, 255));
    }
    struct Preset { const char* name; double (*level)(int); };
    const Preset presets[] = {
      {"saw", [](int n) { return 1.0 / n; }}, {"square", [](int n) { return n % 2 ? 1.0 / n : 0.0; }}, {"triangle", [](int n) { return n % 2 ? 1.0 / (n * n) : 0.0; }},
      {"organ", [](int n) { static const int at[] = {1, 2, 3, 4, 6, 8}; static const double v[] = {1, 0.8, 0.6, 0.5, 0.35, 0.3}; for (int k = 0; k < 6; k++) if (at[k] == n) return v[k]; return 0.0; }},
      {"hollow", [](int n) { return n % 2 ? std::exp(-(n - 1) / 6.0) : 0.0; }}, {"formant", [](int n) { return 0.12 / n + std::exp(-std::pow((n - 7) / 1.8, 2)); }},
      {"sine", [](int n) { return n == 1 ? 1.0 : 0.0; }}};
    for (const Preset& preset : presets) {
      if (textButton(preset.name)) {
        std::vector<double> values(kHarmonics);
        for (int i = 0; i < kHarmonics; i++) values[static_cast<std::size_t>(i)] = std::clamp(preset.level(i + 1), 0.0, 1.0);
        emit(values, false);
      }
      ImGui::SameLine(0, 14 * scale);
    }
    ImGui::NewLine();
  }
  ImGui::End();
  return changed;
}

}  // namespace

bool hasShape(const std::string& nodeType) { return isWave(nodeType) || nodeType == "waveshaper" || nodeType == "addsynth" || nodeType == "shiva" || nodeType == "fmsynth"; }

void showShape(ShapeWindow& window, const std::string& instrument, const std::string& node) {
  window.open = true; window.instrument = instrument; window.node = node; window.dragging = false; window.lastIndex = -1;
}

bool drawShapeWindow(ShapeWindow& window, Document& doc) {
  if (!window.open) return false;
  const GraphNode* node = findNode(doc.project(), window.instrument, window.node);
  if (!node || !hasShape(node->type)) { window.open = false; return false; }
  if (node->type == "addsynth") return drawSpectrum(window, doc, *node);
  if (node->type == "shiva") return drawTaps(window, doc, *node);
  const bool wave = isWave(node->type), fm = node->type == "fmsynth";   // (the FM synth keeps four waves, one for each operator, in data.waves)
  const char* key = fm ? "waves" : wave ? "wave" : "curve";
  const int count = wave ? kWavePoints : kCurvePoints;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextWindowSize(ImVec2((wave ? 420.0f : 300.0f) * scale, (wave ? 250.0f : 360.0f) * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(240 * scale, 120 * scale), ImGuiCond_FirstUseEver);
  const std::string title = std::string(wave ? "Wave  " : "Curve  ") + window.instrument + "." + window.node + "###shape";
  bool changed = false;
  if (ImGui::Begin(title.c_str(), &window.open, ImGuiWindowFlags_NoSavedSettings)) {
    if (fm) {   // which operator's wave
      for (int k = 0; k < 4; k++) {
        if (textButton(("op " + std::to_string(k + 1)).c_str(), window.op == k)) { window.op = k; window.dragging = false; window.lastIndex = -1; }
        ImGui::SameLine(0, 14 * scale);
      }
      ImGui::NewLine();
    }
    const auto operatorWave = [&](int k) {
      std::vector<double> given;
      if (node->data.is_object() && node->data.contains("waves") && node->data["waves"].is_array() && static_cast<std::size_t>(k) < node->data["waves"].size() && node->data["waves"][static_cast<std::size_t>(k)].is_array())
        for (const auto& v : node->data["waves"][static_cast<std::size_t>(k)]) given.push_back(v.is_number() ? v.get<double>() : 0.0);
      if (given.size() < 2) return sineWave();
      std::vector<double> out(kWavePoints);
      for (int i = 0; i < kWavePoints; i++) {
        const double x = static_cast<double>(i) / kWavePoints * static_cast<double>(given.size());
        const std::size_t a = static_cast<std::size_t>(std::floor(x)), b = (a + 1) % given.size();
        out[static_cast<std::size_t>(i)] = std::clamp(given[a % given.size()] * (1 - (x - std::floor(x))) + given[b] * (x - std::floor(x)), -1.0, 1.0);
      }
      return out;
    };
    std::vector<double> points = fm ? operatorWave(window.op) : pointsOf(*node, key, count, wave, wave ? sineWave() : softCurve());
    const auto emit = [&](const std::vector<double>& values, bool fold) {
      Json out = Json::array();
      if (fm) {
        for (int k = 0; k < 4; k++) {
          Json one = Json::array();
          for (const double v : k == window.op ? values : operatorWave(k)) one.push_back(std::round(v * 10000) / 10000);
          out.push_back(one);
        }
      } else for (const double v : values) out.push_back(std::round(v * 10000) / 10000);
      doc.apply(std::make_unique<SetNodeData>(window.instrument, window.node, key, out, fm ? "waves" : wave ? "wave" : "curve"), fold);
      changed = true;
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float side = std::min(ImGui::GetContentRegionAvail().x, std::max(80.0f * scale, ImGui::GetContentRegionAvail().y - 64 * scale));
    const ImVec2 size(wave ? ImGui::GetContentRegionAvail().x : side, wave ? std::max(80.0f * scale, ImGui::GetContentRegionAvail().y - 64 * scale) : side);
    const ImVec2 min = ImGui::GetCursorScreenPos(), max(min.x + size.x, min.y + size.y);
    const float pad = 8 * scale;
    ImGui::InvisibleButton("pad", size);
    const auto xOf = [&](double i) { return min.x + pad + static_cast<float>(i / (wave ? count : count - 1)) * (size.x - 2 * pad); };
    const auto yOf = [&](double v) { return min.y + pad + static_cast<float>((1 - v) / 2) * (size.y - 2 * pad); };

    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      const double fx = std::clamp(static_cast<double>((mouse.x - min.x - pad) / (size.x - 2 * pad)), 0.0, 1.0);
      int index = static_cast<int>(std::lround(fx * (wave ? count : count - 1)));
      if (wave) index %= count;
      double v = std::clamp(1.0 - 2.0 * (mouse.y - min.y - pad) / (size.y - 2 * pad), -1.0, 1.0);
      if (std::fabs(v) < 0.04) v = 0;   // a little pull to the centre line, and to the peaks
      if (v > 0.96) v = 1; else if (v < -0.96) v = -1;
      const auto set = [&](int i, double y) {
        points[static_cast<std::size_t>(i)] = y;
        if (!wave && window.symmetric) { points[static_cast<std::size_t>(count - 1 - i)] = -y; if (i == count / 2) points[static_cast<std::size_t>(i)] = 0; }
      };
      if (window.lastIndex < 0) set(index, v);
      else {   // the points passed over since the last mouse event
        const int step = index >= window.lastIndex ? 1 : -1, span = std::max(1, std::abs(index - window.lastIndex));
        for (int j = window.lastIndex, s = 0; s <= span; j += step, s++) set(((j % count) + count) % count, window.lastValue + (v - window.lastValue) * s / span);
      }
      window.lastIndex = index; window.lastValue = v;
      emit(points, window.dragging);
      window.dragging = true;
    } else { window.dragging = false; window.lastIndex = -1; }

    // the pad: the centre lines, the peaks dashed, the curve as a thin line (a wave also dots)
    dl->AddLine(ImVec2(min.x + pad, yOf(0)), ImVec2(max.x - pad, yOf(0)), textAlpha(56));
    if (!wave) {
      dl->AddLine(ImVec2(xOf((count - 1) / 2.0), yOf(1)), ImVec2(xOf((count - 1) / 2.0), yOf(-1)), textAlpha(56));
      dl->AddLine(ImVec2(xOf(0), yOf(-1)), ImVec2(xOf(count - 1), yOf(1)), textAlpha(40));   // the straight line, for reference
    } else {
      for (const double q : {0.25, 0.5, 0.75}) dl->AddLine(ImVec2(xOf(q * count), yOf(1)), ImVec2(xOf(q * count), yOf(-1)), textAlpha(56));
    }
    std::vector<ImVec2> line;
    for (int i = 0; i < count; i++) line.push_back(ImVec2(xOf(i), yOf(points[static_cast<std::size_t>(i)])));
    if (wave) line.push_back(ImVec2(xOf(count), yOf(points[0])));   // the cycle closes on its first point
    dl->AddPolyline(line.data(), static_cast<int>(line.size()), IM_COL32(235, 150, 60, 255), 0, 1.75f * std::max(1.0f, scale));
    if (wave) for (int i = 0; i < count; i += 8) dl->AddCircleFilled(line[static_cast<std::size_t>(i)], 2 * scale, IM_COL32(235, 150, 60, 255));

    std::vector<Preset> presets;
    if (wave) {
      const auto make = [](double (*fn)(double)) { return [fn] { std::vector<double> w(kWavePoints); for (int i = 0; i < kWavePoints; i++) w[static_cast<std::size_t>(i)] = fn(static_cast<double>(i) / kWavePoints); return w; }; };
      presets = {{"sine", make([](double t) { return std::sin(kTau * t); })},
                 {"triangle", make([](double t) { return t < 0.25 ? 4 * t : t < 0.75 ? 2 - 4 * t : 4 * t - 4; })},
                 {"saw", make([](double t) { return 2 * t - 1; })},
                 {"square", make([](double t) { return t < 0.5 ? 1.0 : -1.0; })},
                 {"organ", make([](double t) { return (std::sin(kTau * t) + 0.5 * std::sin(2 * kTau * t) + 0.25 * std::sin(4 * kTau * t)) / 1.6; })}};
    } else {
      const auto make = [](double (*fn)(double)) { return [fn] { std::vector<double> c(kCurvePoints); for (int i = 0; i < kCurvePoints; i++) c[static_cast<std::size_t>(i)] = std::clamp(fn(2.0 * i / (kCurvePoints - 1) - 1), -1.0, 1.0); return c; }; };
      presets = {{"linear", make([](double x) { return x; })},
                 {"soft", make([](double x) { return std::tanh(2 * x) / std::tanh(2.0); })},
                 {"hard", make([](double x) { return x * 2.5; })},
                 {"fold", make([](double x) { return std::sin(x * 3.14159265358979 * 1.5); })},
                 {"asym", make([](double x) { return x >= 0 ? std::tanh(2 * x) / std::tanh(2.0) : x * 2; })},
                 {"crush", make([](double x) { return std::round(x * 4) / 4; })}};
    }
    for (const Preset& preset : presets) {
      if (textButton(preset.name)) emit(preset.make(), false);
      ImGui::SameLine(0, 14 * scale);
    }
    ImGui::NewLine();
    if (wave) {   // edits of the cycle as it is
      const auto smooth = [&] { std::vector<double> w(points.size()); for (int i = 0; i < count; i++) w[static_cast<std::size_t>(i)] = (points[static_cast<std::size_t>((i + count - 1) % count)] + 2 * points[static_cast<std::size_t>(i)] + points[static_cast<std::size_t>((i + 1) % count)]) / 4; return w; };
      if (textButton("smooth")) emit(smooth(), false);
      ImGui::SameLine(0, 14 * scale);
      if (textButton("flip")) { std::vector<double> w = points; for (double& v : w) v = -v; emit(w, false); }
      ImGui::SameLine(0, 14 * scale);
      if (textButton("reverse")) { std::vector<double> w = points; std::reverse(w.begin(), w.end()); emit(w, false); }
      ImGui::SameLine(0, 14 * scale);
      if (textButton("mirror")) { std::vector<double> w = points; for (int i = count / 2; i < count; i++) w[static_cast<std::size_t>(i)] = points[static_cast<std::size_t>(count - i - 1)]; emit(w, false); }
      ImGui::SameLine(0, 14 * scale);
      if (textButton("random")) {
        std::vector<double> w(points.size());
        for (double& v : w) v = std::rand() / static_cast<double>(RAND_MAX) * 2 - 1;
        for (int pass = 0; pass < 6; pass++) { std::vector<double> next(w.size()); for (int i = 0; i < count; i++) next[static_cast<std::size_t>(i)] = (w[static_cast<std::size_t>((i + count - 1) % count)] + 2 * w[static_cast<std::size_t>(i)] + w[static_cast<std::size_t>((i + 1) % count)]) / 4; w = next; }
        double peak = 1e-9; for (const double v : w) peak = std::max(peak, std::fabs(v));
        for (double& v : w) v /= peak;
        emit(w, false);
      }
      ImGui::NewLine();
    } else if (textButton("symmetric", window.symmetric)) window.symmetric = !window.symmetric;
  }
  ImGui::End();
  return changed;
}

}  // namespace wax::app

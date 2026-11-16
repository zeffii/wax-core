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

bool isWave(const std::string& type) { return type == "osc"; }

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

}  // namespace

bool hasShape(const std::string& nodeType) { return isWave(nodeType) || nodeType == "waveshaper"; }

void showShape(ShapeWindow& window, const std::string& instrument, const std::string& node) {
  window.open = true; window.instrument = instrument; window.node = node; window.dragging = false; window.lastIndex = -1;
}

bool drawShapeWindow(ShapeWindow& window, Document& doc) {
  if (!window.open) return false;
  const GraphNode* node = findNode(doc.project(), window.instrument, window.node);
  if (!node || !hasShape(node->type)) { window.open = false; return false; }
  const bool wave = isWave(node->type);
  const char* key = wave ? "wave" : "curve";
  const int count = wave ? kWavePoints : kCurvePoints;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextWindowSize(ImVec2((wave ? 420.0f : 300.0f) * scale, (wave ? 250.0f : 360.0f) * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(240 * scale, 120 * scale), ImGuiCond_FirstUseEver);
  const std::string title = std::string(wave ? "Wave  " : "Curve  ") + window.instrument + "." + window.node + "###shape";
  bool changed = false;
  if (ImGui::Begin(title.c_str(), &window.open, ImGuiWindowFlags_NoSavedSettings)) {
    std::vector<double> points = pointsOf(*node, key, count, wave, wave ? sineWave() : softCurve());
    const auto emit = [&](const std::vector<double>& values, bool fold) {
      Json out = Json::array();
      for (const double v : values) out.push_back(std::round(v * 10000) / 10000);
      doc.apply(std::make_unique<SetNodeData>(window.instrument, window.node, key, out, wave ? "wave" : "curve"), fold);
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

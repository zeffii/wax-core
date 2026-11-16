#include "app/gate_window.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "app/theme.h"
#include "app/widgets.h"

namespace wax::app {
namespace {

constexpr int kSteps = 64;
const double kTrance[16] = {1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 0};

const GraphNode* findNode(const Project& project, const std::string& instrumentId, const std::string& nodeId) {
  for (const Instrument& instrument : project.instruments) {
    if (instrument.id != instrumentId) continue;
    for (const GraphNode& node : instrument.graph.nodes) if (node.id == nodeId) return &node;
  }
  return nullptr;
}

// The node's levels: up to 64, a shorter list repeating to fill the rest; the trance pattern when it has none.
std::vector<double> levelsOf(const GraphNode& node) {
  std::vector<double> given;
  if (node.data.is_object() && node.data.contains("gate") && node.data["gate"].is_array())
    for (const auto& v : node.data["gate"]) given.push_back(v.is_number() ? std::clamp(v.get<double>(), 0.0, 1.0) : 0.0);
  if (given.empty()) given.assign(kTrance, kTrance + 16);
  std::vector<double> levels(kSteps);
  for (int i = 0; i < kSteps; i++) levels[static_cast<std::size_t>(i)] = given[static_cast<std::size_t>(i) % given.size()];
  return levels;
}

std::vector<double> fromPattern(const char* pattern) {
  std::vector<double> levels;
  for (const char* c = pattern; *c; c++) levels.push_back(*c == 'x' ? 1.0 : *c == 'o' ? 0.5 : 0.0);
  return levels;
}

}  // namespace

bool hasGate(const std::string& nodeType) { return nodeType == "gater" || nodeType == "arp"; }

void showGate(GateWindow& window, const std::string& instrument, const std::string& node) {
  window.open = true; window.instrument = instrument; window.node = node; window.dragging = false;
}

bool drawGateWindow(GateWindow& window, Document& doc, const Engine& engine) {
  if (!window.open) return false;
  const GraphNode* node = findNode(doc.project(), window.instrument, window.node);
  if (!node || !hasGate(node->type)) { window.open = false; return false; }
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextWindowSize(ImVec2(520 * scale, 190 * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(240 * scale, 120 * scale), ImGuiCond_FirstUseEver);
  const std::string title = "Steps  " + window.instrument + "." + window.node + "###gate";
  bool changed = false;
  if (ImGui::Begin(title.c_str(), &window.open, ImGuiWindowFlags_NoSavedSettings)) {
    const std::string address = window.instrument + "." + window.node + ".steps";
    const double stepsNow = engine.registry().slotOf(address) >= 0 ? engine.registry().get(address) : node->param("steps", 16);
    const int maxSteps = node->type == "arp" ? 16 : kSteps;
    const int steps = std::clamp(static_cast<int>(std::lround(stepsNow)), 1, maxSteps);
    std::vector<double> levels = levelsOf(*node);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(60.0f * scale, ImGui::GetContentRegionAvail().y - 34 * scale));
    const ImVec2 min = ImGui::GetCursorScreenPos(), max(min.x + size.x, min.y + size.y);
    ImGui::InvisibleButton("bars", size);
    const float labelH = 12 * scale, pad = 4 * scale;
    const float barsTop = min.y + pad, barsBottom = max.y - labelH - pad, barW = (size.x - 2 * pad) / static_cast<float>(steps);
    const bool pressed = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (pressed) {   // draw: the bar under the mouse takes the height of the mouse (near the ends it snaps shut or open)
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      const int i = std::clamp(static_cast<int>(std::floor((mouse.x - min.x - pad) / barW)), 0, steps - 1);
      double v = std::clamp(static_cast<double>((barsBottom - mouse.y) / (barsBottom - barsTop)), 0.0, 1.0);
      v = v < 0.06 ? 0 : v > 0.94 ? 1 : v;
      if (std::fabs(levels[static_cast<std::size_t>(i)] - v) > 1e-9) {
        levels[static_cast<std::size_t>(i)] = std::round(v * 100) / 100;
        Json out = Json::array();
        for (const double l : levels) out.push_back(std::round(l * 100) / 100);
        doc.apply(std::make_unique<SetNodeData>(window.instrument, window.node, "gate", out, "step levels"), window.dragging);
        window.dragging = true;
        changed = true;
      }
    } else window.dragging = false;
    for (int i = 0; i < steps; i++) {
      const float x = min.x + pad + static_cast<float>(i) * barW;
      dl->AddRectFilled(ImVec2(x + 1, barsTop), ImVec2(x + barW - 1, barsBottom), IM_COL32(128, 128, 128, 41));
      const float top = barsBottom - static_cast<float>(levels[static_cast<std::size_t>(i)]) * (barsBottom - barsTop);
      dl->AddRectFilled(ImVec2(x + 1, top), ImVec2(x + barW - 1, barsBottom), i % 4 == 0 ? IM_COL32(235, 140, 40, 255) : IM_COL32(240, 170, 90, 255));
      if (barW >= 14 * scale || i % 4 == 0) dl->AddText(ImVec2(x + 1, barsBottom + 2 * scale), textAlpha(140), std::to_string(i + 1).c_str());
    }

    struct Preset { const char* name; std::vector<double> levels; };
    std::vector<double> swell, random;
    for (int i = 0; i < 16; i++) swell.push_back((i % 4) / 3.0);
    for (int i = 0; i < kSteps; i++) random.push_back(std::rand() % 100 < 65 ? 1.0 : std::rand() % 100 < 40 ? 0.5 : 0.0);
    const Preset presets[] = {{"trance", std::vector<double>(kTrance, kTrance + 16)}, {"16ths", fromPattern("xxxxxxxxxxxxxxxx")}, {"8ths", fromPattern("x.x.x.x.x.x.x.x.")},
                              {"offbeat", fromPattern("..x...x...x...x.")}, {"gallop", fromPattern("x.xxx.xxx.xxx.xx")}, {"swell", swell}, {"random", random}, {"open", fromPattern("xxxxxxxxxxxxxxxx")}};
    for (const Preset& preset : presets) {
      if (textButton(preset.name)) {
        Json out = Json::array();
        for (int i = 0; i < kSteps; i++) out.push_back(preset.levels[static_cast<std::size_t>(i) % preset.levels.size()]);
        doc.apply(std::make_unique<SetNodeData>(window.instrument, window.node, "gate", out, "step levels"));
        changed = true;
      }
      ImGui::SameLine(0, 14 * scale);
    }
    ImGui::NewLine();
  }
  ImGui::End();
  return changed;
}

}  // namespace wax::app

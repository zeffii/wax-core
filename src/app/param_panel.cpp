#include "app/param_panel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

#include "app/theme.h"
#include "app/units.h"
#include "app/widgets.h"
#include "core/node_registry.h"
#include "nodes/drawings.h"

namespace wax::app {

namespace {

constexpr float kGrip = 6;       // points
constexpr float kNormalWidth = 210, kMinWidth = 90, kMaxWidth = 520;

const Instrument* findInstrument(const Project& project, const std::string& id) {
  for (const Instrument& instrument : project.instruments) if (instrument.id == id) return &instrument;
  return nullptr;
}

ImVec4 colour(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

bool isLog(const ParamSpec& spec) { return spec.min > 0 && spec.max / spec.min >= 100; }

double toPos(const ParamSpec& spec, double value) {
  if (spec.max <= spec.min) return 0;
  const double p = isLog(spec) ? (std::log(std::max(value, spec.min)) - std::log(spec.min)) / (std::log(spec.max) - std::log(spec.min))
                               : (value - spec.min) / (spec.max - spec.min);
  return std::clamp(p, 0.0, 1.0);
}

double fromPos(const ParamSpec& spec, double p) {
  p = std::clamp(p, 0.0, 1.0);
  const double v = isLog(spec) ? std::exp(std::log(spec.min) + p * (std::log(spec.max) - std::log(spec.min))) : spec.min + p * (spec.max - spec.min);
  return spec.isInt ? std::round(v) : v;
}

}  // namespace

float paramDockSpan(const ParamPanelState& state) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const float width = state.width > 0 ? state.width : kNormalWidth * scale;
  return width + (kGrip + 4) * scale;
}

bool drawParamPanel(ParamPanelState& state, Document& doc, const Engine& engine, const std::string& instrumentId, float height) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  if (state.width <= 0) state.width = kNormalWidth * scale;
  const Instrument* instrument = findInstrument(doc.project(), instrumentId);
  if (state.instrument != instrumentId) { state.instrument = instrumentId; state.node.clear(); state.dragging = false; }
  const NodeRegistry& types = builtinNodes();

  // the grip on the left edge: drag to resize, double-click for the usual width
  ImGui::InvisibleButton("dockgrip", ImVec2(kGrip * scale, height));
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + kGrip * scale / 2, a.y), ImVec2(a.x + kGrip * scale / 2, b.y), kAccent, 1.0f);
  }
  if (ImGui::IsItemActive()) state.width = std::clamp(state.width - ImGui::GetIO().MouseDelta.x, kMinWidth * scale, kMaxWidth * scale);
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) state.width = kNormalWidth * scale;
  ImGui::SameLine(0, 0);

  bool changed = false;
  ImGui::BeginChild("paramdock", ImVec2(state.width, height), false, ImGuiWindowFlags_NoNav);
  const GraphNode* picked = nullptr;
  const NodeType* type = nullptr;
  if (instrument) {
    for (const GraphNode& node : instrument->graph.nodes) {
      const NodeType* t = types.find(node.type);
      if (!t || t->params.empty()) continue;
      if (!picked) { picked = &node; type = t; }
      if (node.id == state.node) { picked = &node; type = t; }
    }
  }
  if (!picked) {
    ImGui::TextColored(colour(kDimText), "no parameters");
    ImGui::EndChild();
    return false;
  }
  state.node = picked->id;

  // the title strip: the node's name, inverted
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetTextLineHeight() + 4 * scale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), kAccent);
    dl->AddText(ImVec2(p.x + 6 * scale, p.y + 2 * scale), IM_COL32(12, 16, 20, 255), (picked->id + "  " + picked->type).c_str());
    ImGui::Dummy(ImVec2(w, h));
  }
  if (!drawingsOf(picked->type).empty() && textButton("draw envelopes")) state.drawNode = picked->id;
  if (picked->type == "dvii" && textButton("edit voice")) state.dxNode = picked->id;

  std::set<std::string> wired;   // parameters a wire drives
  for (const Connection& c : instrument->graph.connections) {
    const std::string prefix = picked->id + ".@";
    if (c.to.rfind(prefix, 0) == 0) wired.insert(c.to.substr(prefix.size()));
  }

  ImGui::BeginChild("rows", ImVec2(0, 0), false, ImGuiWindowFlags_NoNav);
  const float rowH = ImGui::GetTextLineHeight() + 3 * scale;
  const float total = ImGui::GetContentRegionAvail().x;
  const float nameW = std::clamp(total * 0.36f, 52 * scale, 160 * scale);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (const ParamSpec& spec : type->params) {
    if (spec.name == "bypass") continue;
    const std::string address = instrumentId + "." + picked->id + "." + spec.name;
    const double value = engine.registry().slotOf(address) >= 0 ? engine.registry().get(address) : picked->param(spec.name, spec.def);
    const bool dim = wired.count(spec.name) > 0;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 barMin(p.x + nameW, p.y + 1), barMax(p.x + total, p.y + rowH - 1);

    // the name, as many characters as fit
    std::string name = spec.name;
    while (name.size() > 1 && ImGui::CalcTextSize(name.c_str()).x > nameW - 6 * scale) name.pop_back();
    dl->AddText(ImVec2(p.x + 2 * scale, p.y + 1.5f * scale), dim ? IM_COL32(80, 90, 100, 255) : kDimText, name.c_str());

    ImGui::PushID(spec.name.c_str());
    ImGui::SetCursorScreenPos(barMin);
    ImGui::InvisibleButton("bar", ImVec2(barMax.x - barMin.x, barMax.y - barMin.y));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    double next = value;
    bool set = false;
    if (active) {
      const float x = (ImGui::GetIO().MousePos.x - barMin.x) / std::max(1.0f, barMax.x - barMin.x);
      next = fromPos(spec, x);
      set = std::fabs(next - value) > 1e-12;
    } else if (hovered && ImGui::GetIO().MouseWheel != 0) {
      next = fromPos(spec, toPos(spec, value) + (ImGui::GetIO().MouseWheel > 0 ? 0.01 : -0.01));
      if (spec.isInt && next == std::round(value)) next = value + (ImGui::GetIO().MouseWheel > 0 ? 1 : -1);
      next = std::clamp(next, spec.min, spec.max);
      set = next != value;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { next = spec.def; set = next != value; }
    if (set) {
      doc.apply(std::make_unique<SetParam>(instrumentId, picked->id, spec.name, next), state.dragging && active);
      state.dragging = active;
      changed = true;
    } else if (!active) {
      state.dragging = false;
    }
    const double shown = set ? next : value;

    // the bar: a faint fill to the value, one thin accent mark, the value written inside
    dl->AddRectFilled(barMin, barMax, IM_COL32(255, 255, 255, hovered ? 14 : 8));
    const float x = barMin.x + static_cast<float>(toPos(spec, shown)) * (barMax.x - barMin.x);
    dl->AddRectFilled(barMin, ImVec2(x, barMax.y), IM_COL32(64, 190, 168, dim ? 20 : 48));
    dl->AddRectFilled(ImVec2(std::min(x, barMax.x - 2 * scale), barMin.y), ImVec2(std::min(x, barMax.x - 2 * scale) + 2 * scale, barMax.y), dim ? kDimText : kAccent);
    dl->AddText(ImVec2(barMin.x + 4 * scale, p.y + 1.5f * scale), dim ? IM_COL32(110, 122, 132, 255) : IM_COL32(214, 224, 230, 255), displayValue(picked->type, spec, shown).c_str());

    if (ImGui::BeginPopupContextItem("rowmenu")) {
      static char text[64];
      if (ImGui::IsWindowAppearing()) {
        std::snprintf(text, sizeof text, "%s", displayValue(picked->type, spec, shown).c_str());
        ImGui::SetKeyboardFocusHere();
      }
      ImGui::SetNextItemWidth(120 * scale);
      const bool entered = ImGui::InputText("##type", text, sizeof text, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
      if (entered) {
        double typed = 0;
        if (parseValue(picked->type, spec, text, typed)) { doc.apply(std::make_unique<SetParam>(instrumentId, picked->id, spec.name, typed)); changed = true; }
        ImGui::CloseCurrentPopup();
      }
      if (textButton("reset")) { doc.apply(std::make_unique<SetParam>(instrumentId, picked->id, spec.name, spec.def)); changed = true; ImGui::CloseCurrentPopup(); }
      ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::Dummy(ImVec2(1, 0));
  }
  ImGui::EndChild();
  ImGui::EndChild();
  return changed;
}

}  // namespace wax::app

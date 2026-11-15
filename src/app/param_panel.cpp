#include "app/param_panel.h"

#include <cmath>
#include <memory>
#include <vector>

#include "app/theme.h"
#include "app/widgets.h"
#include "core/node_registry.h"
#include "nodes/drawings.h"

namespace wax::app {

namespace {

const Instrument* findInstrument(const Project& project, const std::string& id) {
  for (const Instrument& instrument : project.instruments) if (instrument.id == id) return &instrument;
  return nullptr;
}

ImVec4 colour(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

}  // namespace

bool drawParamPanel(ParamPanelState& state, Document& doc, const Engine& engine, const std::string& instrumentId) {
  const Instrument* instrument = findInstrument(doc.project(), instrumentId);
  if (!instrument) return false;
  if (state.instrument != instrumentId) { state.instrument = instrumentId; state.node.clear(); state.dragging = false; }
  const NodeRegistry& types = builtinNodes();

  // the nodes that have parameters
  std::vector<const GraphNode*> nodes;
  for (const GraphNode& node : instrument->graph.nodes) {
    const NodeType* type = types.find(node.type);
    if (type && !type->params.empty()) nodes.push_back(&node);
  }
  if (nodes.empty()) { ImGui::TextColored(colour(kDimText), "no parameters"); return false; }
  const GraphNode* picked = nodes.front();
  for (const GraphNode* node : nodes) if (node->id == state.node) picked = node;
  state.node = picked->id;

  // node names: flat text, the picked one lit
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (i) ImGui::SameLine(0, 14);
    const bool on = nodes[i] == picked;
    ImGui::PushStyleColor(ImGuiCol_Text, on ? colour(kAccent) : colour(kDimText));
    ImGui::TextUnformatted(nodes[i]->id.c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemClicked()) { state.node = nodes[i]->id; state.dragging = false; }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }
  if (!drawingsOf(picked->type).empty()) {
    ImGui::SameLine(0, 24);
    if (textButton("draw envelopes")) state.drawNode = picked->id;
  }
  ImGui::Separator();

  const NodeType* type = types.find(picked->type);
  bool changed = false;
  ImGui::BeginChild("sliders", ImVec2(0, 3.4f * ImGui::GetTextLineHeightWithSpacing()), false, ImGuiWindowFlags_HorizontalScrollbar);
  for (std::size_t i = 0; i < type->params.size(); ++i) {
    const ParamSpec& spec = type->params[i];
    if (spec.name == "bypass") continue;
    const std::string address = instrumentId + "." + picked->id + "." + spec.name;
    double value = engine.registry().slotOf(address) >= 0 ? engine.registry().get(address) : picked->param(spec.name, spec.def);
    SliderSpec k;
    k.min = spec.min; k.max = spec.max; k.def = spec.def; k.isInt = spec.isInt;
    k.log = spec.min > 0 && spec.max / spec.min >= 100;
    if (i) ImGui::SameLine(0, 12);
    ImGui::BeginGroup();
    const SliderResult r = slider(address.c_str(), spec.name.c_str(), value, k);
    ImGui::EndGroup();
    if (r.changed) {
      doc.apply(std::make_unique<SetParam>(instrumentId, picked->id, spec.name, value), state.dragging);
      state.dragging = r.active;
      changed = true;
    } else if (!r.active) {
      state.dragging = false;
    }
  }
  ImGui::EndChild();
  return changed;
}

}  // namespace wax::app

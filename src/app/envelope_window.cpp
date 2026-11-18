#include "app/envelope_window.h"
#include "app/node_pictures.h"

#include <memory>

#include "app/theme.h"
#include "app/widgets.h"
#include "core/node_registry.h"
#include "nodes/drawings.h"

namespace wax::app {

namespace {

const GraphNode* findNode(const Project& project, const std::string& instrumentId, const std::string& nodeId) {
  for (const Instrument& instrument : project.instruments) {
    if (instrument.id != instrumentId) continue;
    for (const GraphNode& node : instrument.graph.nodes) if (node.id == nodeId) return &node;
  }
  return nullptr;
}

}  // namespace

EnvelopeSpec envelopeSpecFor(const GraphNode& node, const DrawingInfo& info, const Engine& engine, const std::string& instrumentId) {
  EnvelopeSpec spec;
  spec.title = info.title.c_str();
  spec.knot = info.knot;
  spec.colour = drawingColour(node.type, info.name);
  if (hasDrawnHitGrid(node.type)) { if (const NodeType* type = builtinNodes().find(node.type)) spec.totalMs = drawingMs(node, *type, info.name); }   // (these are drawn against the ms their rate gives)
  if (info.timeParam.empty()) return spec;
  const NodeType* type = builtinNodes().find(node.type);
  double fallback = 0;
  if (type) for (const ParamSpec& p : type->params) if (p.name == info.timeParam) fallback = p.def;
  const std::string address = instrumentId + "." + node.id + "." + info.timeParam;
  spec.totalMs = engine.registry().slotOf(address) >= 0 ? engine.registry().get(address) : node.param(info.timeParam, fallback);
  return spec;
}

void showEnvelopes(EnvelopeWindow& window, const std::string& instrument, const std::string& node) {
  if (window.instrument != instrument || window.node != node) { window.drawing.clear(); window.views.clear(); }
  window.instrument = instrument;
  window.node = node;
  window.open = true;
}

bool drawEnvelopeWindow(EnvelopeWindow& window, Document& doc, const Engine& engine) {
  if (!window.open) return false;
  const GraphNode* node = findNode(doc.project(), window.instrument, window.node);
  const std::vector<DrawingInfo>& drawings = node ? drawingsOf(node->type) : drawingsOf("");
  if (!node || drawings.empty()) { window.open = false; return false; }

  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextWindowSize(ImVec2(720 * scale, 340 * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(260 * scale, 120 * scale), ImGuiCond_FirstUseEver);
  const std::string title = "Envelope  " + window.instrument + "." + window.node + "###envelope";
  bool changed = false;
  if (ImGui::Begin(title.c_str(), &window.open)) {
    // the tabs, when there is more than one drawing: flat text
    if (window.drawing.empty()) window.drawing = drawings.front().name;
    const DrawingInfo* info = &drawings.front();
    for (const DrawingInfo& d : drawings) if (d.name == window.drawing) info = &d;
    if (drawings.size() > 1) {
      for (const DrawingInfo& d : drawings) {
        if (textButton(d.title.c_str(), d.name == info->name)) { window.drawing = d.name; info = &d; }
        ImGui::SameLine(0, 14 * scale);
      }
      ImGui::NewLine();
    }

    bezier::Spline spline = bezier::drawingOf(&node->data, info->name.c_str(), defaultDrawing(node->type, info->name));
    const EnvelopeSpec spec = envelopeSpecFor(*node, *info, engine, window.instrument);
    EnvelopeView& view = window.views[info->name];
    EnvelopeResult result = drawEnvelopeCanvas(view, info->name.c_str(), spline, spec, ImVec2(ImGui::GetContentRegionAvail().x, 190 * scale));
    EnvelopeResult tools = drawEnvelopeTools(view, spline, spec);
    if (tools.changed) result = tools;
    if (result.changed) {
      doc.apply(std::make_unique<SetDrawing>(window.instrument, window.node, info->name, bezier::toJson(spline)), result.fold);
      changed = true;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kDimText));
    ImGui::TextWrapped("press adds - double-click a curve inserts - double-click an anchor sharp/smooth - right-click removes an anchor or flips a segment line/curve");
    ImGui::PopStyleColor();
  }
  ImGui::End();
  return changed;
}

}  // namespace wax::app

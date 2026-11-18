#include "app/node_strip.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "app/config.h"
#include "app/envelope_window.h"
#include "app/node_envelope.h"
#include "app/node_pictures.h"
#include "app/picture_slot.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "core/node_registry.h"
#include "nodes/drawings.h"

namespace wax::app {

namespace {

constexpr float kGripHeight = 6;                         // points
constexpr float kHideBelow = 20, kMinShown = 60;         // web pixels: under the first the strip hides; the strip is never shorter than the second
constexpr float kDefaultHeight = 170;                    // web pixels
constexpr float kBarsWidth = 168, kBarNameWidth = 64;    // the parameter bars (web pixels)
constexpr float kDrawingWidth = 200;                     // one drawing's canvas
constexpr float kButtonsWidth = 120;
constexpr float kGap = 10;                               // between the columns of a node, and (twice) between nodes
constexpr float kSmallestPicture = 0.3f;                 // a picture is never drawn smaller than this part of its size

struct Layout {
  float scale = 1;
  float contentHeight = 0;   // under the node names
};

// One column's rectangle: x is where it starts (the cursor moves on by the width and a gap).
struct Column {
  ImVec2 min, max;
};

const GraphNode* findNode(const Instrument& instrument, const std::string& id) {
  for (const GraphNode& node : instrument.graph.nodes) if (node.id == id) return &node;
  return nullptr;
}

// The grip: a thin accent line along the strip's top edge with a short handle in the middle. Drag for the height (up is taller); all the way
// down hides the strip, and the same drag from the hidden strip brings it back. A double-click goes back to the standard height.
void drawGrip(NodeStripState& state, ImVec2 at, float width, float scale) {
  Settings& cfg = settings();
  ImGui::SetCursorScreenPos(at);
  ImGui::InvisibleButton("##stripgrip", ImVec2(width, kGripHeight * scale));
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
  if (ImGui::IsItemActivated()) state.dragHeight = cfg.stripShown ? cfg.stripHeight : 0;
  if (ImGui::IsItemActive()) {
    state.dragHeight -= ImGui::GetIO().MouseDelta.y / scale;
    if (state.dragHeight < kHideBelow) cfg.stripShown = false;
    else {
      cfg.stripShown = true;
      cfg.stripHeight = std::clamp(state.dragHeight, kMinShown, std::max(kMinShown, ImGui::GetIO().DisplaySize.y / scale * 0.6f));
    }
  }
  if (ImGui::IsItemDeactivated()) saveSettings();
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { cfg.stripShown = true; cfg.stripHeight = kDefaultHeight; saveSettings(); }
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const float y = std::floor(at.y) + 0.5f;
  draw->AddLine(ImVec2(at.x, y), ImVec2(at.x + width, y), kAccent, 1.0f);
  const float mid = at.x + width * 0.5f, half = 18 * scale;
  draw->AddLine(ImVec2(mid - half, y), ImVec2(mid + half, y), kAccent, hot ? 4.0f : 3.0f);
}

// The picture column of a node: `size` in web pixels is fitted into the content height (never above the normal scale). Returns the width used.
template <typename DrawPicture>
float drawPictureColumn(ImDrawList* dl, PictureSize size, ImVec2 at, const Layout& layout, DrawPicture&& draw) {
  const float fit = std::clamp(layout.contentHeight / (size.h * layout.scale), kSmallestPicture, 1.0f);
  const float shown = layout.scale * fit;
  const ImVec2 lo = at, hi(at.x + size.w * shown, at.y + layout.contentHeight);
  dl->PushClipRect(lo, hi, true);
  pictureSlot() = {true, lo};
  draw(shown, lo, hi);
  pictureSlot() = {};
  dl->PopClipRect();
  return size.w * shown;
}

}  // namespace

bool hasStripUi(const std::string& nodeType) {
  return nodeType != "out" && (hasPicture(nodeType) || hasResponse(nodeType) || hasEditorWindow(nodeType));
}

NodeStripResult drawNodeStrip(NodeStripState& state, ParamPanelState& panel, Document& doc, const Engine& engine, const std::string& instrumentId, ImVec2 areaMin, ImVec2 areaMax,
                              const std::function<std::shared_ptr<SpectrumProbe>(const std::string& instrument, const std::string& node)>& probeOf) {
  NodeStripResult result;
  const Instrument* instrument = nullptr;
  for (const Instrument& candidate : doc.project().instruments) if (candidate.id == instrumentId) instrument = &candidate;
  if (!instrument) return result;
  const NodeRegistry& types = builtinNodes();
  std::vector<const GraphNode*> shown;
  for (const GraphNode& node : instrument->graph.nodes) if (types.find(node.type) && hasStripUi(node.type)) shown.push_back(&node);
  state.response.list.clear();
  if (shown.empty()) return result;

  const float scale = ImGui::GetStyle().FontScaleDpi;
  Settings& cfg = settings();
  const float grip = kGripHeight * scale;
  const float width = areaMax.x - areaMin.x;
  const float height = cfg.stripShown ? std::min(cfg.stripHeight * scale, (areaMax.y - areaMin.y) * 0.9f) : grip;
  ImGui::SetNextWindowPos(ImVec2(areaMin.x, areaMax.y - height));
  ImGui::SetNextWindowSize(ImVec2(width, height));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(cfg.stripShown ? withAlpha(kBg, 245) : withAlpha(kBg, 0)));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse;
  const bool visible = ImGui::Begin("##nodestrip", nullptr, flags);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (!visible) { ImGui::End(); return result; }

  drawGrip(state, ImGui::GetWindowPos(), width, scale);
  if (!cfg.stripShown) { ImGui::End(); return result; }

  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);   // the same small face as the parameter panel
  const float nameHeight = ImGui::GetTextLineHeight() + 3 * scale;
  Layout layout;
  layout.scale = scale;
  layout.contentHeight = height - grip - nameHeight - 4 * scale - ImGui::GetStyle().ScrollbarSize;   // (the strip's own scrollbar runs along the bottom)
  const PictureTiming timing{60000.0 / (std::max(1.0, doc.project().song.bpm) * std::max(1, doc.project().song.ticksPerBeat)), std::max(1, doc.project().song.ticksPerBeat), &doc.project(), instrumentId, doc.revision(), probeOf};

  ImGui::SetCursorScreenPos(ImVec2(areaMin.x, areaMax.y - height + grip));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * scale, 0));
  ImGui::BeginChild("##stripscroll", ImVec2(width, height - grip), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);
  ImGui::PopStyleVar();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  float x = 0;   // how far along the strip, from `origin`

  for (const GraphNode* node : shown) {
    const NodeType& type = *types.find(node->type);
    const float groupStart = x;
    const float top = origin.y + nameHeight;
    ImGui::PushID(node->id.c_str());

    // the node's name (click to pick it), and a thin line before it
    const bool isPicked = panel.node == node->id;
    const std::string title = node->id + "  " + node->type;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + x + 6 * scale, origin.y));
    const ImVec2 titleSize = ImGui::CalcTextSize(title.c_str());
    ImGui::InvisibleButton("##pick", ImVec2(titleSize.x, nameHeight - scale));
    if (ImGui::IsItemClicked()) panel.node = node->id;
    dl->AddText(ImVec2(origin.x + x + 6 * scale, origin.y + 1.5f * scale), isPicked ? kAccent : kText, node->id.c_str());
    dl->AddText(ImVec2(origin.x + x + 6 * scale + ImGui::CalcTextSize((node->id + "  ").c_str()).x, origin.y + 1.5f * scale), kDimText, node->type.c_str());
    x += 6 * scale;

    // the parameter bars: all of them, the column scrolls
    if (!type.params.empty()) {
      const float barsWidth = kBarsWidth * scale;
      ImGui::SetCursorScreenPos(ImVec2(origin.x + x, top));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::BeginChild("##bars", ImVec2(barsWidth, layout.contentHeight), false, ImGuiWindowFlags_NoNav);
      ImGui::PopStyleVar();
      for (const ParamSpec& spec : type.params)
        if (drawParamBar(state.bars, doc, engine, instrumentId, *node, spec, barsWidth - 8 * scale, kBarNameWidth * scale)) result.paramsChanged = true;
      ImGui::EndChild();
      x += barsWidth + kGap * scale;
    }

    // the pictures
    if (hasResponse(node->type)) {
      x += drawPictureColumn(dl, kResponseSize, ImVec2(origin.x + x, top), layout, [&](float shownScale, ImVec2 lo, ImVec2 hi) {
             drawNodeResponse(dl, font, *node, type, lo, lo, lo, hi, shownScale, state.response);
           }) + kGap * scale;
    }
    if (hasPicture(node->type)) {
      x += drawPictureColumn(dl, pictureSize(node->type), ImVec2(origin.x + x, top), layout, [&](float shownScale, ImVec2 lo, ImVec2 hi) {
             drawNodePicture(dl, font, *node, type, lo, lo, lo, hi, shownScale, timing, state.response);
           }) + kGap * scale;
    }
    if (hasEnvelopes(node->type)) {
      x += drawPictureColumn(dl, kEnvelopeSize, ImVec2(origin.x + x, top), layout, [&](float shownScale, ImVec2 lo, ImVec2 hi) {
             drawNodeEnvelopes(dl, font, *node, type, lo, lo, lo, hi, shownScale);
           }) + kGap * scale;
    }

    // the drawings: one canvas each, edited here; the tools (point types, shapes, snap) are in the window
    for (const DrawingInfo& info : drawingsOf(node->type)) {
      const float canvasWidth = kDrawingWidth * scale;
      ImGui::SetCursorScreenPos(ImVec2(origin.x + x, top));
      ImGui::PushID(info.name.c_str());
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::BeginChild("##drawing", ImVec2(canvasWidth, layout.contentHeight), false, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::PopStyleVar();
      bezier::Spline spline = bezier::drawingOf(&node->data, info.name.c_str(), defaultDrawing(node->type, info.name));
      const EnvelopeSpec spec = envelopeSpecFor(*node, info, engine, instrumentId);
      EnvelopeView& view = state.views[node->id + "/" + info.name];
      const EnvelopeResult edit = drawEnvelopeCanvas(view, info.name.c_str(), spline, spec, ImVec2(canvasWidth, layout.contentHeight));
      if (edit.changed) {
        doc.apply(std::make_unique<SetDrawing>(instrumentId, node->id, info.name, bezier::toJson(spline)), edit.fold);
        result.dataChanged = true;
      }
      ImGui::EndChild();
      ImGui::PopID();
      x += canvasWidth + kGap * scale;
    }

    // the buttons of the node's editor windows
    if (hasEditorWindow(node->type)) {
      ImGui::SetCursorScreenPos(ImVec2(origin.x + x, top));
      ImGui::BeginGroup();
      drawEditorButtons(panel, *node);
      ImGui::EndGroup();
      x += kButtonsWidth * scale;
    }
    ImGui::PopID();
    dl->AddLine(ImVec2(origin.x + groupStart, origin.y + 2 * scale), ImVec2(origin.x + groupStart, origin.y + height - grip - 2 * scale), lineAlpha(90), 1.0f);
    x += kGap * scale;
  }
  ImGui::SetCursorScreenPos(ImVec2(origin.x + x, origin.y));
  ImGui::Dummy(ImVec2(1, 1));

  // the pictures' handles (a dot of the response, the room's source...): pressed, one is dragged as one undo step
  const ImGuiIO& io = ImGui::GetIO();
  if (state.responseDrag >= 0 && state.responseDrag < static_cast<int>(state.response.list.size())) {
    const ResponseHandles::Handle& handle = state.response.list[static_cast<std::size_t>(state.responseDrag)];
    const GraphNode* node = findNode(*instrument, handle.node);
    const NodeType* type = node ? types.find(node->type) : nullptr;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && node && type) {
      for (const auto& [name, value] : dragResponse(state.response, handle, *node, *type, io.MousePos)) {
        if (value == node->param(name, value + 1)) continue;
        doc.apply(std::make_unique<SetParam>(instrumentId, handle.node, name, value), !state.responseFresh);
        state.responseFresh = false;
        result.paramsChanged = true;
      }
    } else state.responseDrag = -1;
  } else if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    float best = 10 * scale;
    for (std::size_t i = 0; i < state.response.list.size(); i++) {
      const ImVec2 d(state.response.list[i].pos.x - io.MousePos.x, state.response.list[i].pos.y - io.MousePos.y);
      const float distance = std::sqrt(d.x * d.x + d.y * d.y);
      if (distance < best) { best = distance; state.responseDrag = static_cast<int>(i); state.responseFresh = true; }
    }
    if (state.responseDrag >= 0 && state.response.list[static_cast<std::size_t>(state.responseDrag)].param == "@draw") {   // a drawing's title: its window
      panel.drawNode = state.response.list[static_cast<std::size_t>(state.responseDrag)].node;
      state.responseDrag = -1;
    }
    if (state.responseDrag >= 0 && state.response.list[static_cast<std::size_t>(state.responseDrag)].once) {   // a click that acts at once (a step of a pattern)
      const ResponseHandles::Handle& handle = state.response.list[static_cast<std::size_t>(state.responseDrag)];
      const GraphNode* node = findNode(*instrument, handle.node);
      const NodeType* type = node ? types.find(node->type) : nullptr;
      if (node && type)
        for (const auto& [name, value] : dragResponse(state.response, handle, *node, *type, io.MousePos)) { doc.apply(std::make_unique<SetParam>(instrumentId, handle.node, name, value)); result.paramsChanged = true; }
      state.responseDrag = -1;
    }
  }
  ImGui::EndChild();
  ImGui::PopFont();
  ImGui::End();
  return result;
}

}  // namespace wax::app

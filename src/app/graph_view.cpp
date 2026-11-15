#include "app/graph_view.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>

#include "app/theme.h"

namespace wax::app {

namespace {

struct Tile { ImVec2 min, max; };

std::string nodeOfAddress(const std::string& address) { return address.substr(0, address.find('.')); }

}  // namespace

bool drawGraphView(GraphViewState& state, Document& doc, const std::string& instrumentId, std::string& picked, float height) {
  const Instrument* instrument = nullptr;
  for (const Instrument& i : doc.project().instruments) if (i.id == instrumentId) instrument = &i;
  if (!instrument) return false;
  const Graph& graph = instrument->graph;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  bool moved = false;

  ImGui::BeginChild("graph", ImVec2(0, height), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float tileH = 22 * scale;

  std::set<std::string> hasInput;
  for (const Connection& c : graph.connections) hasInput.insert(nodeOfAddress(c.to));

  // tiles: positions in web pixels, scaled; nodes without one are laid out in a row
  std::map<std::string, Tile> tiles;
  float right = 0;
  for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
    const GraphNode& n = graph.nodes[i];
    const float x = static_cast<float>(n.placed ? n.x : 24 + 130.0 * static_cast<double>(i));
    const float y = static_cast<float>(n.placed ? n.y : 40);
    const float w = ImGui::CalcTextSize(n.id.c_str()).x + (n.type == "out" ? 34 : 22) * scale;
    tiles[n.id] = {ImVec2(origin.x + x * scale * 0.8f, origin.y + (y - 100) * scale * 0.8f + 12 * scale),
                   ImVec2(origin.x + x * scale * 0.8f + w, origin.y + (y - 100) * scale * 0.8f + 12 * scale + tileH)};
    right = std::max(right, x * scale * 0.8f + w);
  }

  // wires: from the right edge of the sender to the left edge of the receiver, a gentle S
  for (const Connection& c : graph.connections) {
    const auto a = tiles.find(nodeOfAddress(c.from)), b = tiles.find(nodeOfAddress(c.to));
    if (a == tiles.end() || b == tiles.end()) continue;
    const ImVec2 p0(a->second.max.x, (a->second.min.y + a->second.max.y) / 2), p1(b->second.min.x, (b->second.min.y + b->second.max.y) / 2);
    const float k = std::max(24 * scale, std::fabs(p1.x - p0.x) * 0.4f);
    dl->AddBezierCubic(p0, ImVec2(p0.x + k, p0.y), ImVec2(p1.x - k, p1.y), p1, IM_COL32(120, 134, 146, 190), 1.2f);
    dl->AddCircleFilled(p1, 2.5f * scale, kDimText);
  }

  // tiles
  const ImGuiIO& io = ImGui::GetIO();
  for (const GraphNode& n : graph.nodes) {
    const Tile& t = tiles[n.id];
    const bool on = n.id == picked;
    const ImU32 line = on ? kAccent : IM_COL32(150, 165, 176, 255);
    const ImU32 fill = IM_COL32(20, 26, 32, 255);
    const ImVec2 mid((t.min.x + t.max.x) / 2, (t.min.y + t.max.y) / 2);
    if (n.type == "out") {
      // the speaker: a small box and a cone
      dl->AddRectFilled(t.min, ImVec2(t.min.x + tileH * 0.5f, t.max.y), fill);
      dl->AddQuadFilled(ImVec2(t.min.x + tileH * 0.5f, t.min.y + tileH * 0.25f), ImVec2(t.min.x + tileH, t.min.y), ImVec2(t.min.x + tileH, t.max.y), ImVec2(t.min.x + tileH * 0.5f, t.max.y - tileH * 0.25f), fill);
      dl->AddPolyline(std::array<ImVec2, 7>{t.min, ImVec2(t.min.x + tileH * 0.5f, t.min.y), ImVec2(t.min.x + tileH * 0.5f, t.min.y + tileH * 0.25f), ImVec2(t.min.x + tileH, t.min.y), ImVec2(t.min.x + tileH, t.max.y), ImVec2(t.min.x, t.max.y), t.min}.data(), 7, line, ImDrawFlags_None, 1.2f);
    } else if (n.type == "voices") {
      const float r = std::max(tileH * 0.5f + 2, (t.max.x - t.min.x) * 0.5f - 2 * scale);
      dl->AddCircleFilled(mid, r, fill, 24);
      dl->AddCircle(mid, r, line, 24, 1.2f);
    } else if (!hasInput.count(n.id)) {
      dl->AddRectFilled(t.min, t.max, fill, tileH * 0.5f);
      dl->AddRect(t.min, t.max, line, tileH * 0.5f, 0, 1.2f);
    } else {
      dl->AddRectFilled(t.min, t.max, fill, 2);
      dl->AddRect(t.min, t.max, line, 2, 0, 1.2f);
    }
    const ImVec2 size = ImGui::CalcTextSize(n.id.c_str());
    const float shift = n.type == "out" ? tileH * 0.5f : 0;   // the text sits beside the cone
    dl->AddText(ImVec2(mid.x + shift - size.x / 2, mid.y - size.y / 2), on ? kAccent : kDimText, n.id.c_str());

    ImGui::SetCursorScreenPos(t.min);
    ImGui::PushID(n.id.c_str());
    ImGui::InvisibleButton("tile", ImVec2(t.max.x - t.min.x, tileH));
    ImGui::PopID();
    if (ImGui::IsItemClicked()) { picked = n.id; state.held = n.id; state.grabX = io.MousePos.x - t.min.x; state.grabY = io.MousePos.y - t.min.y; }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }

  // dragging the held tile
  if (!state.held.empty()) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      state.held.clear();
    } else {
      const float nx = (io.MousePos.x - state.grabX - origin.x) / (scale * 0.8f);
      const float ny = (io.MousePos.y - state.grabY - origin.y - 12 * scale) / (scale * 0.8f) + 100;
      const GraphNode* held = nullptr;
      for (const GraphNode& n : graph.nodes) if (n.id == state.held) held = &n;
      if (held && (std::fabs(nx - held->x) > 0.5 || std::fabs(ny - held->y) > 0.5 || !held->placed)) {
        const bool drag = std::hypot(io.MouseDelta.x, io.MouseDelta.y) > 0 || held->placed;
        if (drag && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3)) {
          doc.apply(std::make_unique<MoveNode>(instrumentId, state.held, std::max(0.0f, nx), std::max(0.0f, ny)), true);
          moved = true;
        }
      }
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(origin.x + right + 40 * scale, origin.y));
  ImGui::Dummy(ImVec2(1, 1));
  ImGui::EndChild();
  return moved;
}

}  // namespace wax::app

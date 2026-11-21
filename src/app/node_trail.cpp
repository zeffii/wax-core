#include "app/node_trail.h"

#include <cmath>

#include "app/node_look.h"
#include "app/theme.h"
#include "core/node_registry.h"

namespace wax::app {

namespace {

// The nodes of the trail, top to bottom: node type names. Anything the graph knows by name can go here.
constexpr const char* kTrailNodes[] = {"env2", "filterpro", "haas", "delay", "plate", "eq", "compressor", "pogue", "chorus", "flanger"};

constexpr float kTileSize = 24;   // web pixels
constexpr float kTileGap = 6;
constexpr float kDragStart = 4;   // pixels the mouse must move from the press before it is a drag

// The tile's flat grey fill, as the graph's compact tiles have.
const ImU32 kFlat = IM_COL32(128, 128, 128, 51);

void drawTile(ImDrawList* dl, ImVec2 lo, float size, const std::string& type, float scale, bool lit) {
  const NodeLookInfo look = lookOf(type);
  const ImVec2 hi(lo.x + size, lo.y + size);
  dl->AddRectFilled(lo, hi, kBg, 2 * scale);
  dl->AddRectFilled(lo, hi, kFlat, 2 * scale);
  if (lit) dl->AddRect(lo, hi, kAccent, 2 * scale, 0, std::max(1.0f, scale));
  drawGlyph(dl, look.glyph, TileShape::Box, ImVec2(lo.x + size / 2, lo.y + size / 2), scale * 0.8f, kText);
}

}  // namespace

bool overTrail(ImVec2 point, ImVec2 min, ImVec2 max, float scale) {
  return point.x >= max.x - kTrailWidth * scale && point.x <= max.x && point.y >= min.y && point.y <= max.y;
}

TrailEvent drawNodeTrail(NodeTrailState& state, ImDrawList* dl, ImVec2 min, ImVec2 max, float scale, std::string& dragType) {
  TrailEvent event;
  const ImGuiIO& io = ImGui::GetIO();
  const float size = kTileSize * scale, strip = kTrailWidth * scale;
  dl->AddLine(ImVec2(max.x - strip, min.y), ImVec2(max.x - strip, max.y), kLine);   // a thin line parts the trail from the graph
  const NodeRegistry& types = builtinNodes();
  const ImVec2 mouse = io.MousePos;
  int index = 0;
  std::string hovered;
  for (const char* name : kTrailNodes) {
    const std::string type = name;
    if (!types.find(type)) continue;   // (a type this build lacks leaves no gap)
    const ImVec2 lo(max.x - strip + (strip - size) / 2, min.y + kTileGap * scale + static_cast<float>(index) * (size + kTileGap * scale));
    index++;
    const bool over = mouse.x >= lo.x && mouse.x <= lo.x + size && mouse.y >= lo.y && mouse.y <= lo.y + size;
    drawTile(dl, lo, size, type, scale, over && !state.dragging);
    if (over) hovered = type;
    if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { state.pressed = type; state.press = mouse; state.dragging = false; }
  }
  if (!hovered.empty() && !state.dragging && state.pressed.empty()) ImGui::SetTooltip("%s: click to place, drag into the graph (onto a wire to put it in the wire)", hovered.c_str());
  if (!state.pressed.empty()) {
    if (!state.dragging && std::hypot(mouse.x - state.press.x, mouse.y - state.press.y) > kDragStart * scale) state.dragging = true;
    if (state.dragging) {   // the tile follows the mouse
      dragType = state.pressed;
      drawTile(ImGui::GetForegroundDrawList(), ImVec2(mouse.x - size / 2, mouse.y - size / 2), size, state.pressed, scale, true);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      event.type = state.pressed;
      if (!state.dragging) event.kind = TrailEvent::Kind::Click;
      else if (!overTrail(mouse, min, max, scale)) { event.kind = TrailEvent::Kind::Drop; event.at = mouse; }   // (let go over the trail: nothing)
      state.pressed.clear();
      state.dragging = false;
      dragType.clear();
    }
  }
  return event;
}

}  // namespace wax::app

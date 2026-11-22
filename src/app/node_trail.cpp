#include "app/node_trail.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "app/insert_menu.h"
#include "app/node_look.h"
#include "app/theme.h"
#include "core/node_registry.h"

namespace wax::app {

namespace {

// The nodes of the trail, top to bottom: node type names. Anything the graph knows by name can go here.
constexpr const char* kTrailNodes[] = {"env2", "filterpro", "haas", "delay", "plate", "eq", "compressor", "pogue", "chorus", "flanger"};

constexpr float kTileSize = 34;   // web pixels
constexpr float kTileGap = 6;
constexpr float kDragStart = 4;   // pixels the mouse must move from the press before it is a drag

// The tile's flat grey fill, as the graph's compact tiles have.
const ImU32 kFlat = IM_COL32(128, 128, 128, 51);

// The colour of a category in the bank (the tile's outline and glyph).
ImU32 categoryColour(const std::string& category) {
  if (category == "Sources") return IM_COL32(29, 158, 117, 255);
  if (category == "Effects") return IM_COL32(127, 119, 221, 255);
  if (category == "Control") return IM_COL32(239, 159, 39, 255);
  if (category == "Mix & level") return IM_COL32(216, 90, 48, 255);
  if (category == "Distortion") return IM_COL32(212, 83, 126, 255);
  return IM_COL32(136, 135, 128, 255);
}

// The four letters under a tile. A type's first four letters, except where two types would share them.
std::string shortName(const std::string& type) {
  static const std::map<std::string, std::string> kNames = {
      {"compass", "cmps"}, {"compressor", "comp"}, {"waveseq", "wseq"}, {"waveshaper", "wshp"}, {"wavefolder", "wfld"}, {"wavetable", "wtbl"},
      {"pulsar", "pulr"}, {"pulsetrain", "ptrn"}, {"drumsynth", "drum"}, {"drumsynth2", "drm2"}, {"combperc", "cmbp"}, {"filterpro", "fpro"},
      {"chord", "chrd"}, {"specdyn", "sdyn"}, {"specpan", "span"}, {"specsynth", "ssyn"}, {"gater", "gatr"}, {"notetrig", "ntrg"}, {"notepan", "npan"}, {"bus-in", "busi"}};
  const auto found = kNames.find(type);
  return found != kNames.end() ? found->second : type.substr(0, 4);
}

constexpr int kBankColumns = 8;
constexpr float kBankGap = 4;       // between tiles
constexpr float kBankBandGap = 10;  // between the bands of two categories
constexpr float kBankPad = 8;

// A colour moved part of the way to white (the alpha stays).
ImU32 lighter(ImU32 colour, float amount) {
  const auto lift = [&](int shift) { const float c = static_cast<float>((colour >> shift) & 255); return static_cast<ImU32>(c + (255.0f - c) * amount) << shift; };
  return (colour & 0xFF000000u) | lift(IM_COL32_R_SHIFT) | lift(IM_COL32_G_SHIFT) | lift(IM_COL32_B_SHIFT);
}

// One tile: a flat tile with the node's glyph; in the bank (`outline` set) it has a thin outline and its glyph in that colour.
void drawTile(ImDrawList* dl, ImVec2 lo, float size, const std::string& type, float scale, bool lit, ImU32 outline = 0) {
  const NodeLookInfo look = lookOf(type);
  const ImVec2 hi(lo.x + size, lo.y + size);
  dl->AddRectFilled(lo, hi, kBg, 2 * scale);
  dl->AddRectFilled(lo, hi, kFlat, 2 * scale);
  if (outline) dl->AddRect(lo, hi, (outline & 0x00FFFFFFu) | (80u << IM_COL32_A_SHIFT), 2 * scale, 0, 1.0f);   // a faint hairline: the glyph carries the colour
  if (lit) dl->AddRect(lo, hi, kAccent, 2 * scale, 0, std::max(1.0f, scale) * (outline ? 2.0f : 1.0f));
  const ImU32 ink = outline ? lighter(outline, 0.4f) : kText;   // (in the bank the glyph and name are the category's colour, lifted toward white: bright, while the border stays faint)
  dl->PushClipRect(lo, hi, true);   // (a wide glyph stays inside its tile)
  drawGlyph(dl, look.glyph, TileShape::Box, ImVec2(lo.x + size / 2, lo.y + size * 0.37f), scale * 0.7f, ink);
  dl->PopClipRect();
  const float fontSize = ImGui::GetFontSize() * 0.72f;
  const std::string name = shortName(type);
  const float width = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0, name.c_str()).x;
  dl->AddText(ImGui::GetFont(), fontSize, ImVec2(lo.x + (size - width) / 2, hi.y - fontSize - 2 * scale), ink, name.c_str());
}

// The mouse going down on a tile starts a press; returns true while the mouse is over the tile.
bool tileHit(NodeTrailState& state, ImVec2 lo, float size, const std::string& type, bool visible) {
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool over = visible && mouse.x >= lo.x && mouse.x <= lo.x + size && mouse.y >= lo.y && mouse.y <= lo.y + size;
  if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { state.pressed = type; state.press = mouse; state.dragging = false; }
  return over;
}

}  // namespace

bool overTrail(ImVec2 point, ImVec2 min, ImVec2 max, float scale) {
  return point.x >= max.x - kTrailWidth * scale && point.x <= max.x && point.y >= min.y && point.y <= max.y;
}

TrailEvent drawNodeTrail(NodeTrailState& state, ImDrawList* dl, ImVec2 min, ImVec2 max, float scale, std::string& dragType, const std::set<std::string>& available) {
  TrailEvent event;
  const ImGuiIO& io = ImGui::GetIO();
  const float size = kTileSize * scale, strip = kTrailWidth * scale, gap = kTileGap * scale;
  dl->AddLine(ImVec2(max.x - strip, min.y), ImVec2(max.x - strip, max.y), kLine);   // a thin line parts the trail from the graph
  const NodeRegistry& types = builtinNodes();
  const ImVec2 mouse = io.MousePos;
  const std::vector<BankGroup> groups = menuBank(available);
  std::map<std::string, ImU32> colours;
  for (const BankGroup& g : groups) for (const std::string& t : g.types) colours[t] = categoryColour(g.category);

  const bool bankHover = state.over(mouse);
  // the bank's switch, the first tile of the strip: a 3 by 3 field of dots
  {
    const ImVec2 lo(max.x - strip + (strip - size) / 2, min.y + gap), hi(lo.x + size, lo.y + size);
    const bool over = !bankHover && mouse.x >= lo.x && mouse.x <= hi.x && mouse.y >= lo.y && mouse.y <= hi.y;
    dl->AddRectFilled(lo, hi, kBg, 2 * scale);
    dl->AddRectFilled(lo, hi, kFlat, 2 * scale);
    if (over || state.bankOpen) dl->AddRect(lo, hi, kAccent, 2 * scale, 0, std::max(1.0f, scale));
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) dl->AddCircleFilled(ImVec2(lo.x + size * (0.25f + 0.25f * static_cast<float>(c)), lo.y + size * (0.25f + 0.25f * static_cast<float>(r))), 1.5f * scale, kText);
    if (over) ImGui::SetTooltip("All nodes");
    if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) state.bankOpen = !state.bankOpen;
  }

  int index = 1;   // (the bank's switch is the first tile)
  std::string hovered, hoveredCategory;
  for (const char* name : kTrailNodes) {
    const std::string type = name;
    if (!types.find(type)) continue;   // (a type this build lacks leaves no gap)
    const ImVec2 lo(max.x - strip + (strip - size) / 2, min.y + gap + static_cast<float>(index) * (size + gap));
    index++;
    const bool over = tileHit(state, lo, size, type, !bankHover);
    drawTile(dl, lo, size, type, scale, over && !state.dragging);
    if (over) hovered = type;
  }

  // the bank: a band of tiles for each category
  state.bankMin = state.bankMax = ImVec2(0, 0);
  if (state.bankOpen) {
    const float tile = size + kBankGap * scale, pad = kBankPad * scale, band = kBankBandGap * scale, bar = 6 * scale;
    const float height = max.y - min.y;
    // as many columns as it takes to show the whole bank at the height there is (never more than half the graph's width); where it still
    // does not fit, it has a scrollbar
    const auto contentHeight = [&](int columns) {
      float h = pad * 2;
      for (const BankGroup& g : groups) h += std::ceil(static_cast<float>(g.types.size()) / static_cast<float>(columns)) * tile + band;
      return h;
    };
    const int widest = std::max(kBankColumns, static_cast<int>((std::max(0.0f, max.x - min.x) * 0.5f - pad * 2 - bar) / tile));
    int columns = kBankColumns;
    while (columns < widest && contentHeight(columns) > height) columns++;
    const float width = pad * 2 + static_cast<float>(columns) * tile - kBankGap * scale + bar;
    const ImVec2 lo(max.x - strip - width, min.y), hi(max.x - strip, max.y);
    state.bankMin = lo; state.bankMax = hi;
    const float content = contentHeight(columns);
    const float maxScroll = std::max(0.0f, content - height);
    if (bankHover && io.MouseWheel != 0) state.bankScroll -= io.MouseWheel * tile * 2;
    // the scrollbar down the bank's right side (drawn after the bank's background)
    ImVec2 thumbLo(0, 0), thumbHi(0, 0);
    if (maxScroll > 0) {
      const float thumb = std::max(24 * scale, height * height / content);
      const float travel = height - thumb;
      const ImVec2 barLo(hi.x - bar, lo.y);
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouse.x >= barLo.x && mouse.x <= hi.x && mouse.y >= lo.y && mouse.y <= hi.y) state.bankBarDrag = true;
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) state.bankBarDrag = false;
      if (state.bankBarDrag && travel > 0) state.bankScroll = (mouse.y - lo.y - thumb / 2) / travel * maxScroll;
      state.bankScroll = std::clamp(state.bankScroll, 0.0f, maxScroll);
      const float top = lo.y + (travel > 0 ? state.bankScroll / maxScroll * travel : 0);
      thumbLo = ImVec2(barLo.x + 2 * scale, top); thumbHi = ImVec2(hi.x - 1 * scale, top + thumb);
    }
    state.bankScroll = std::clamp(state.bankScroll, 0.0f, maxScroll);
    dl->AddRectFilled(lo, hi, kBg);
    dl->AddLine(ImVec2(lo.x, lo.y), ImVec2(lo.x, hi.y), kLine);
    if (thumbHi.y > thumbLo.y) dl->AddRectFilled(thumbLo, thumbHi, kDimText, 2 * scale);
    dl->PushClipRect(lo, hi, true);
    float y = lo.y + pad - state.bankScroll;
    for (const BankGroup& g : groups) {
      const ImU32 colour = categoryColour(g.category);
      for (std::size_t k = 0; k < g.types.size(); k++) {
        const ImVec2 at(lo.x + pad + static_cast<float>(k % static_cast<std::size_t>(columns)) * tile, y + static_cast<float>(k / static_cast<std::size_t>(columns)) * tile);
        const bool visible = at.y + size > lo.y && at.y < hi.y;
        const bool over = tileHit(state, at, size, g.types[k], visible);
        if (visible) drawTile(dl, at, size, g.types[k], scale, over && !state.dragging, colour);
        if (over) { hovered = g.types[k]; hoveredCategory = g.category; }
      }
      y += std::ceil(static_cast<float>(g.types.size()) / static_cast<float>(columns)) * tile + band;
    }
    dl->PopClipRect();
  }
  if (!hovered.empty() && !state.dragging && state.pressed.empty()) {
    if (hoveredCategory.empty()) ImGui::SetTooltip("%s: click to place, drag into the graph (onto a wire to put it in the wire)", hovered.c_str());
    else ImGui::SetTooltip("%s (%s): click to place, drag into the graph", hovered.c_str(), hoveredCategory.c_str());
  }
  if (!state.pressed.empty()) {
    if (!state.dragging && std::hypot(mouse.x - state.press.x, mouse.y - state.press.y) > kDragStart * scale) state.dragging = true;
    if (state.dragging) {   // the tile follows the mouse
      dragType = state.pressed;
      drawTile(ImGui::GetForegroundDrawList(), ImVec2(mouse.x - size / 2, mouse.y - size / 2), size, state.pressed, scale, true, colours.count(state.pressed) && state.bankOpen ? colours[state.pressed] : ImU32(0));
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      event.type = state.pressed;
      if (!state.dragging) event.kind = TrailEvent::Kind::Click;
      else if (!overTrail(mouse, min, max, scale) && !state.over(mouse)) { event.kind = TrailEvent::Kind::Drop; event.at = mouse; }   // (let go over the trail or the bank: nothing)
      state.pressed.clear();
      state.dragging = false;
      dragType.clear();
    }
  }
  return event;
}

}  // namespace wax::app

#include "app/graph_view.h"
#include "app/node_envelope.h"
#include "app/node_pictures.h"
#include "app/node_response.h"

#include <cstdio>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>

#include "app/node_look.h"
#include "app/config.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "core/node_registry.h"
#include "project/samples.h"

namespace wax::app {

namespace {

constexpr float kPortRadius = 4;     // a port's reach for the mouse; the compact look draws it half as big
constexpr float kStripStep = 11, kStripGap = 7;   // parameter ports hang in a strip of dots under the tile
constexpr float kGrid = 24, kGridMajor = 12;      // the dot grid, and the dots between the brighter ones
constexpr float kPlainRow = 16;      // port spacing on a node that draws its own face

std::string nodeOfAddress(const std::string& address) { return address.substr(0, address.find('.')); }
std::string portOfAddress(const std::string& address) { const auto dot = address.find('.'); return dot == std::string::npos ? "" : address.substr(dot + 1); }

// What each kind of wire is coloured (the web app's defaults).
ImU32 kindColour(PortKind kind) {
  switch (kind) {
    case PortKind::Control: return settings().wireControl;
    case PortKind::Stereo: return settings().wireStereo;
    case PortKind::Param: return settings().wireParam;
    default: return settings().wireAudio;
  }
}

PortKind portKind(const NodeType& type, const GraphNode& node, const std::string& name) {
  if (!name.empty() && name[0] == '@') return PortKind::Param;
  if (node.stereo && std::find(type.stereoOptional.begin(), type.stereoOptional.end(), name) != type.stereoOptional.end()) return PortKind::Stereo;
  const auto it = type.kinds.find(name);
  return it == type.kinds.end() ? PortKind::Audio : it->second;
}

struct Tile {
  const GraphNode* node = nullptr;
  const NodeType* type = nullptr;
  TileShape shape = TileShape::Box;
  bool compact = true;
  TileLayout layout;
  ImVec2 min, max;            // screen pixels
  std::vector<std::string> inputs;   // every input port: the type's, then one per '@' parameter
  float faceInputs = 0;       // how many of them sit on the tile's edge (the others hang in the strip)
  std::string glyph;
};

ImVec2 centreOf(const Tile& t) { return ImVec2((t.min.x + t.max.x) / 2, (t.min.y + t.max.y) / 2); }

// Where a port sits, in screen pixels (-1 index: the tile's middle edge, for a port the type does not have).
ImVec2 portPos(const Tile& t, bool output, int index, float scale) {
  const float w = t.max.x - t.min.x, h = t.max.y - t.min.y;
  if (!output && t.compact && index >= static_cast<int>(t.faceInputs)) return ImVec2(t.min.x + (8 + (index - t.faceInputs) * kStripStep) * scale, t.max.y + kStripGap * scale);
  const float n = output ? static_cast<float>(t.type->outputs.size()) : t.faceInputs;
  const float row = (t.compact ? kCompactRow : kPlainRow) * scale;
  const float y = t.min.y + (h - n * row) / 2 + row / 2 + static_cast<float>(std::max(index, 0)) * row;
  float x = output ? t.max.x : t.min.x;
  if (t.shape == TileShape::Circle) {   // keep the dot on the circle's edge
    const float r = w / 2, dy = y - (t.min.y + r), dx = std::sqrt(std::max(0.0f, r * r - dy * dy));
    x = t.min.x + r + (output ? dx : -dx);
  }
  return ImVec2(x, y);
}

float wireArm(ImVec2 a, ImVec2 b, float scale) {   // wires between close nodes lose their S-curve (the web's default distance is 60)
  const float straight = 60 * scale;
  return 50 * scale * std::clamp((std::hypot(b.x - a.x, b.y - a.y) - straight) / straight, 0.0f, 1.0f);
}

ImVec2 bezierAt(ImVec2 a, ImVec2 b, float arm, float t) {
  const ImVec2 c1(a.x + arm, a.y), c2(b.x - arm, b.y);
  const float u = 1 - t;
  const float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
  return ImVec2(w0 * a.x + w1 * c1.x + w2 * c2.x + w3 * b.x, w0 * a.y + w1 * c1.y + w2 * c2.y + w3 * b.y);
}

// A wire from a to b; control and parameter wires are dashed.
void drawWire(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 colour, float width, bool dashed, float scale) {
  const float arm = wireArm(a, b, scale);
  if (!dashed) { dl->AddBezierCubic(a, ImVec2(a.x + arm, a.y), ImVec2(b.x - arm, b.y), b, colour, width); return; }
  constexpr int kSteps = 48;
  ImVec2 prev = a;
  float walked = 0;
  const float dash = 7 * scale, gap = 5 * scale;
  for (int i = 1; i <= kSteps; i++) {
    const ImVec2 q = bezierAt(a, b, arm, static_cast<float>(i) / kSteps);
    if (std::fmod(walked, dash + gap) < dash) dl->AddLine(prev, q, colour, width);
    walked += std::hypot(q.x - prev.x, q.y - prev.y);
    prev = q;
  }
}

}  // namespace

bool drawGraphView(GraphViewState& state, Document& doc, const std::string& instrumentId, std::string& picked, float height) {
  const Instrument* instrument = nullptr;
  for (const Instrument& i : doc.project().instruments) if (i.id == instrumentId) instrument = &i;
  if (!instrument) return false;
  const Graph& graph = instrument->graph;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const NodeRegistry& types = builtinNodes();
  bool moved = false;

  ImGui::BeginChild("graph", ImVec2(0, height), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImGuiIO& io = ImGui::GetIO();
  const bool windowHovered = ImGui::IsWindowHovered();
  ImFont* font = ImGui::GetFont();
  const float nameSize = 11 * scale, labelSize = 10 * scale;
  auto textWidth = [&](float size, const char* s) { return font->CalcTextSizeA(size, FLT_MAX, 0, s).x; };

  // a very faint grid of dots, a brighter one every few
  {
    const ImVec2 lo = dl->GetClipRectMin(), hi = dl->GetClipRectMax();
    const float step = kGrid * scale;
    const int i0 = static_cast<int>(std::floor((lo.x - origin.x) / step)), j0 = static_cast<int>(std::floor((lo.y - origin.y) / step));
    for (int j = j0; origin.y + j * step < hi.y; j++) {
      for (int i = i0; origin.x + i * step < hi.x; i++) {
        const bool major = i % static_cast<int>(kGridMajor) == 0 && j % static_cast<int>(kGridMajor) == 0;
        const ImVec2 p(std::round(origin.x + i * step), std::round(origin.y + j * step));
        const float r = (major ? 1.0f : 0.75f) * std::max(1.0f, scale * 0.9f);
        dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), textAlpha(major ? 76 : 28));
      }
    }
  }

  // the tiles: each node's shape and size, from the type; a node the file gave no position is laid out in a row
  std::map<std::string, Tile> tiles;
  float right = 0, bottom = 0;
  int unplaced = 0;
  for (const GraphNode& n : graph.nodes) {
    const NodeType* type = types.find(n.type);
    if (!type || n.type == "tempo") continue;   // (the master's tempo node is not shown)
    Tile t;
    t.node = &n; t.type = type;
    const NodeLookInfo look = lookOf(n.type);
    t.shape = look.shape; t.glyph = look.glyph;
    t.compact = look.sizeW <= 0 && look.shape != TileShape::Dot;
    t.inputs = type->inputs;
    for (const std::string& p : n.ports) t.inputs.push_back("@" + p);
    t.faceInputs = static_cast<float>(t.compact ? type->inputs.size() : t.inputs.size());
    float w = look.sizeW, h = look.sizeH;
    if (t.compact) {
      t.layout = compactLayout(t.shape, std::vector<std::string>(t.inputs.begin(), t.inputs.begin() + static_cast<long>(t.faceInputs)), type->outputs);
      w = t.layout.w; h = t.layout.h;
    }
    const double x = n.placed ? n.x : 24 + 130.0 * unplaced, y = n.placed ? n.y : 40;
    if (!n.placed) unplaced++;
    t.min = ImVec2(origin.x + static_cast<float>(x) * scale, origin.y + static_cast<float>(y) * scale);
    t.max = ImVec2(t.min.x + w * scale, t.min.y + h * scale);
    right = std::max(right, t.max.x - origin.x);
    bottom = std::max(bottom, t.max.y - origin.y);
    tiles[n.id] = t;
  }

  // keep the graph clear of the exposed dock, once for each instrument (the dock may sit over nodes the file placed under its corner)
  if (!state.tidied.count(instrumentId) && state.dockMax.x > state.dockMin.x && !tiles.empty()) {
    state.tidied.insert(instrumentId);
    const float margin = 12;
    const float dockX = (state.dockMin.x - origin.x) / scale, dockY = (state.dockMin.y - origin.y) / scale;
    const float dockW = (state.dockMax.x - state.dockMin.x) / scale, dockH = (state.dockMax.y - state.dockMin.y) / scale;
    float dx = 0, dy = 0, lowest = 0;
    bool under = false;
    for (const auto& [id, t] : tiles) {
      const float x = (t.min.x - origin.x) / scale, y = (t.min.y - origin.y) / scale - 14, w = (t.max.x - t.min.x) / scale, h = (t.max.y - t.min.y) / scale + 14;   // (the name above it)
      lowest = std::max(lowest, y + h);
      if (x < dockX + dockW + margin && x + w > dockX - margin && y < dockY + dockH + margin && y + h > dockY - margin) {
        under = true;
        dx = std::max(dx, dockX + dockW + margin - x);
        dy = std::max(dy, dockY + dockH + margin - y);
      }
    }
    const float viewHeight = (dl->GetClipRectMax().y - dl->GetClipRectMin().y) / scale;
    if (under) {
      if (!(dy <= dx && lowest + dy <= viewHeight)) dy = 0; else dx = 0;
      std::vector<MoveNodes::Place> places;
      for (const auto& [id, t] : tiles) places.push_back({id, std::round((t.min.x - origin.x) / scale + dx), std::round((t.min.y - origin.y) / scale + dy)});
      doc.apply(std::make_unique<MoveNodes>(instrumentId, places));
      ImGui::EndChild();
      return true;   // (drawn again next frame, where the nodes now are)
    }
  }

  // what each port carries: a dual node whose audio input receives stereo runs as a pair, so its audio ports are stereo too
  std::set<std::string> wide;
  for (bool changed = true; changed;) {
    changed = false;
    for (const Connection& c : graph.connections) {
      const auto a = tiles.find(nodeOfAddress(c.from)), b = tiles.find(nodeOfAddress(c.to));
      if (a == tiles.end() || b == tiles.end() || !b->second.type->dual || wide.count(b->first)) continue;
      const PortKind from = portKind(*a->second.type, *a->second.node, portOfAddress(c.from));
      const bool fromStereo = from == PortKind::Stereo || (from == PortKind::Audio && wide.count(a->first));
      if (portKind(*b->second.type, *b->second.node, portOfAddress(c.to)) == PortKind::Audio && fromStereo) { wide.insert(b->first); changed = true; }
    }
  }
  auto kindOf = [&](const Tile& t, const std::string& port) {
    const PortKind k = portKind(*t.type, *t.node, port);
    return k == PortKind::Audio && wide.count(t.node->id) ? PortKind::Stereo : k;
  };
  auto indexOf = [](const std::vector<std::string>& names, const std::string& name) {
    const auto it = std::find(names.begin(), names.end(), name);
    return it == names.end() ? -1 : static_cast<int>(it - names.begin());
  };

  // the port under the mouse (ports win over tiles): reach is the port's radius plus a little
  struct Hit { std::string node, port; bool output = false; };
  Hit hover;
  if (windowHovered) {
    float best = (kPortRadius + 4) * scale;
    for (const auto& [id, t] : tiles) {
      for (int side = 0; side < 2; side++) {
        const std::vector<std::string>& names = side ? t.type->outputs : t.inputs;
        for (int i = 0; i < static_cast<int>(names.size()); i++) {
          const ImVec2 p = portPos(t, side == 1, i, scale);
          const float d = std::hypot(io.MousePos.x - p.x, io.MousePos.y - p.y);
          if (d <= best) { best = d; hover = {id, names[static_cast<std::size_t>(i)], side == 1}; }
        }
      }
    }
  }

  // wires, from an output port to an input port: a gentle S, coloured by what the signal is. The one nearest the mouse is lit; a click selects it.
  struct WireEnds { const Connection* c; ImVec2 p0, p1; PortKind kind; };
  std::vector<WireEnds> wires;
  for (const Connection& c : graph.connections) {
    const auto a = tiles.find(nodeOfAddress(c.from)), b = tiles.find(nodeOfAddress(c.to));
    if (a == tiles.end() || b == tiles.end()) continue;
    const std::string fromPort = portOfAddress(c.from), toPort = portOfAddress(c.to);
    wires.push_back({&c, portPos(a->second, true, indexOf(a->second.type->outputs, fromPort), scale), portPos(b->second, false, indexOf(b->second.inputs, toPort), scale), kindOf(a->second, fromPort)});
  }
  const bool dragging = !state.wiring.empty() || !state.wiringIn.empty() || state.rewire.started;
  const auto lifted = [&](const Connection& c) { return state.rewire.started && c.from == state.rewire.from && c.to == state.rewire.to; };
  // While a tile is dragged: the wire it is over (it is put into that wire when let go), and whether it is far out of its own chain (its
  // neighbours are then joined up). Only a node with an audio input and an audio output, and a wire that carries audio, take part.
  const Tile* dragTile = nullptr;
  std::string chainIn, chainOut;   // the dragged node's first input and first output, when it qualifies
  if (!state.held.empty()) {
    const auto found = tiles.find(state.held);
    if (found != tiles.end() && found->second.node->type != "out" && !found->second.type->inputs.empty() && !found->second.type->outputs.empty()) {
      const Tile& t = found->second;
      const std::string in = t.type->inputs.front(), out = t.type->outputs.front();
      const PortKind kIn = kindOf(t, in), kOut = kindOf(t, out);
      if ((kIn == PortKind::Audio || kIn == PortKind::Stereo) && (kOut == PortKind::Audio || kOut == PortKind::Stereo)) {
        dragTile = &t; chainIn = t.node->id + "." + in; chainOut = t.node->id + "." + out;
      }
    }
  }
  state.dropFrom.clear(); state.dropTo.clear(); state.dropPull = false;
  if (dragTile && state.dragMoved && !state.dragFree && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const ImVec2 centre = centreOf(*dragTile);
    float best = 16 * scale;
    for (const WireEnds& w : wires) {
      if (nodeOfAddress(w.c->from) == state.held || nodeOfAddress(w.c->to) == state.held) continue;
      if (w.kind != PortKind::Audio && w.kind != PortKind::Stereo) continue;
      const float arm = wireArm(w.p0, w.p1, scale);
      for (int k = 0; k <= 24; k++) {
        const ImVec2 pt = bezierAt(w.p0, w.p1, arm, static_cast<float>(k) / 24);
        const float d = std::hypot(centre.x - pt.x, centre.y - pt.y);
        if (d < best) { best = d; state.dropFrom = w.c->from; state.dropTo = w.c->to; }
      }
    }
    const WireEnds* feed = nullptr; const WireEnds* sink = nullptr;
    for (const WireEnds& w : wires) {
      if (!feed && w.c->to == chainIn) feed = &w;
      if (!sink && w.c->from == chainOut) sink = &w;
    }
    if (feed && sink && state.dropFrom.empty()) {
      const ImVec2 a = feed->p0, b = sink->p1;
      const float dx = b.x - a.x, dy = b.y - a.y, u = std::clamp(((centre.x - a.x) * dx + (centre.y - a.y) * dy) / std::max(1e-3f, dx * dx + dy * dy), 0.0f, 1.0f);
      const bool far = settings().graphPullOut && std::hypot(a.x + u * dx - centre.x, a.y + u * dy - centre.y) > 70 * scale;
      state.dropPull = far || state.dragForce;
      if (state.dropPull) drawWire(dl, a, b, kWarm, 1.0f * std::max(1.0f, scale), true, scale);
    }
    state.message = !state.dropFrom.empty() ? "Release to put " + state.held + " into this wire" : state.dropPull ? "Release to take " + state.held + " out of its chain" : std::string();
  }
  // A port with several wires: Tab chooses which one is lit, and a press on the port picks that one up.
  const Connection* litWire = nullptr;
  {
    const std::string key = hover.node.empty() ? std::string() : hover.node + "." + hover.port + (hover.output ? ">" : "<");
    if (key != state.hoverKey) { state.hoverKey = key; state.pickWire = 0; }
    std::vector<const Connection*> here;
    if (!key.empty()) for (const Connection& c : graph.connections) if ((hover.output ? c.from : c.to) == hover.node + "." + hover.port) here.push_back(&c);
    if (here.size() > 1 && windowHovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) state.pickWire = (state.pickWire + (io.KeyShift ? static_cast<int>(here.size()) - 1 : 1)) % static_cast<int>(here.size());
    if (here.size() > 1) litWire = here[static_cast<std::size_t>(state.pickWire) % here.size()];
  }
  const Connection* hoverWire = nullptr;
  if (windowHovered && hover.node.empty() && !dragging) {
    float best = 6 * scale;
    for (const WireEnds& w : wires) {
      const float arm = wireArm(w.p0, w.p1, scale);
      for (int k = 0; k <= 24; k++) {
        const ImVec2 pt = bezierAt(w.p0, w.p1, arm, static_cast<float>(k) / 24);
        const float d = std::hypot(io.MousePos.x - pt.x, io.MousePos.y - pt.y);
        if (d < best) { best = d; hoverWire = w.c; }
      }
    }
  }
  for (const WireEnds& w : wires) {
    if (lifted(*w.c)) continue;
    const bool chosenWire = w.c->from == state.wireFrom && w.c->to == state.wireTo;
    const float width = (w.kind == PortKind::Stereo ? 3.0f : 1.75f) * 0.65f * std::max(1.0f, scale) * (chosenWire || w.c == hoverWire || w.c == litWire || (w.c->from == state.dropFrom && w.c->to == state.dropTo && !state.dropFrom.empty()) ? 1.8f : 1.0f);
    drawWire(dl, w.p0, w.p1, chosenWire || (w.c->from == state.dropFrom && w.c->to == state.dropTo && !state.dropFrom.empty()) ? kWarm : kindColour(w.kind), width, settings().wireDashed && (w.kind == PortKind::Control || w.kind == PortKind::Param), scale);
  }
  std::string hoverText;   // what the thing under the mouse is, as in the web app's status line
  {
    const auto kindText = [](PortKind k) {
      switch (k) {
        case PortKind::Control: return "control: an envelope, lfo or trigger signal";
        case PortKind::Stereo: return "stereo audio: left and right travel on this one wire";
        case PortKind::Param: return "parameter control: a 0 to 1 signal mapped onto a parameter";
        default: return "audio: a mono signal";
      }
    };
    if (!hover.node.empty() && !dragging) {
      const auto t = tiles.find(hover.node);
      const std::string address = hover.node + "." + hover.port;
      int count = 0;
      for (const Connection& c : graph.connections) count += hover.output ? c.from == address : c.to == address;
      hoverText = std::string(hover.output ? "Output " : "Input ") + address + ", " + kindText(kindOf(t->second, hover.port));
      if (count) hoverText += count > 1 ? " - " + std::to_string(count) + " wires: Tab picks one, drag moves it (Shift+drag makes a new wire)" : " - drag to move its wire (Shift+drag makes a new wire)";
    } else if (hoverWire) {
      const auto a = tiles.find(nodeOfAddress(hoverWire->from));
      hoverText = "Wire " + hoverWire->from + " to " + hoverWire->to + ", " + kindText(kindOf(a->second, portOfAddress(hoverWire->from)));
    }
  }

  // the tiles themselves
  const ImU32 ink = kText;
  const ImU32 flat = IM_COL32(128, 128, 128, 51);   // the compact look: a flat grey fill, an outline only when chosen
  for (const GraphNode& n : graph.nodes) {
    const auto found = tiles.find(n.id);
    if (found == tiles.end()) continue;
    const Tile& t = found->second;
    const bool chosen = n.id == picked || state.group.count(n.id) > 0;
    const float w = t.max.x - t.min.x, h = t.max.y - t.min.y;
    const ImVec2 mid = centreOf(t);
    const float lineWidth = (chosen ? 2.0f : 1.0f) * std::max(1.0f, scale);
    if (t.shape == TileShape::Circle || t.shape == TileShape::Dot) {
      if (t.shape == TileShape::Dot) {
        dl->AddCircleFilled(mid, w / 2, kindColour(kindOf(t, t.type->outputs.empty() ? "" : t.type->outputs.front())), 20);
        dl->AddCircle(mid, w / 2, ink, 20, 1.0f);
      } else {
        dl->AddCircleFilled(mid, w / 2, kBg, 32);
        dl->AddCircleFilled(mid, w / 2, flat, 32);
        if (chosen) dl->AddCircle(mid, w / 2, kAccent, 32, lineWidth);
      }
    } else if (t.shape == TileShape::Speaker) {   // a small back and a cone that opens to the right
      const std::array<ImVec2, 6> poly = {ImVec2(t.min.x, t.min.y + 0.3f * h), ImVec2(t.min.x + 0.38f * w, t.min.y + 0.3f * h), ImVec2(t.max.x, t.min.y),
                                          ImVec2(t.max.x, t.max.y), ImVec2(t.min.x + 0.38f * w, t.min.y + 0.7f * h), ImVec2(t.min.x, t.min.y + 0.7f * h)};
      dl->AddConcavePolyFilled(poly.data(), 6, kBg);
      dl->AddConcavePolyFilled(poly.data(), 6, flat);
      if (chosen) dl->AddPolyline(poly.data(), 6, kAccent, ImDrawFlags_Closed, lineWidth);
    } else {
      const float rounding = (t.shape == TileShape::Rounded ? 10.0f : 2.0f) * scale;
      dl->AddRectFilled(t.min, t.max, kBg, rounding);
      dl->AddRectFilled(t.min, t.max, flat, rounding);
      if (chosen) dl->AddRect(t.min, t.max, kAccent, rounding, 0, lineWidth);
    }
    // the name sits above the shape (a route dot shows it only when chosen)
    if (t.shape != TileShape::Dot || chosen) {
      const float nameW = textWidth(nameSize, n.id.c_str());
      dl->AddText(font, nameSize, ImVec2(mid.x - nameW / 2, t.min.y - 4 * scale - nameSize), ink, n.id.c_str());
    }
    if (n.type == "sidechain" || n.type == "listen") {   // where it listens to
      const std::string where = n.source.empty() ? "<- (none)" : "<- " + n.source;
      dl->AddText(font, nameSize, ImVec2(mid.x - textWidth(nameSize, where.c_str()) / 2, t.max.y + 2 * scale), textAlpha(190), where.c_str());
    }
    if (t.compact) drawGlyph(dl, t.glyph, t.shape, ImVec2(t.min.x + t.layout.glyphX * scale, mid.y), scale, ink);

    // the ports
    for (int side = 0; side < 2 && t.shape != TileShape::Dot; side++) {
      const std::vector<std::string>& names = side ? t.type->outputs : t.inputs;
      for (int i = 0; i < static_cast<int>(names.size()); i++) {
        const std::string& name = names[static_cast<std::size_t>(i)];
        const ImVec2 p = portPos(t, side == 1, i, scale);
        const bool hot = hover.node == n.id && hover.port == name && hover.output == (side == 1);
        const float pulse = hot ? 0.4f + 1.0f * (0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 1000 / 260)) : 0;
        const float r = ((t.compact ? kPortRadius / 2 : kPortRadius) + pulse) * std::max(1.0f, scale);
        dl->AddCircleFilled(p, r, kindColour(kindOf(t, name)), 12);
        if (!t.compact) dl->AddCircle(p, r, ink, 12, 1.0f);
        const bool inStrip = side == 0 && t.compact && i >= static_cast<int>(t.faceInputs);   // a parameter dot under the tile: named by hover, not drawn
        std::string label;
        if (!inStrip) label = t.compact ? (side ? t.layout.outLabels : t.layout.inLabels)[static_cast<std::size_t>(i)] : (name[0] == '@' ? name.substr(1) : name);
        if (t.shape != TileShape::Circle && !label.empty()) {
          const float lw = textWidth(labelSize, label.c_str());
          dl->AddText(font, labelSize, ImVec2(side ? p.x - 8 * scale - lw : p.x + 8 * scale, p.y - labelSize / 2), textAlpha(190), label.c_str());
        }
      }
    }

    ImGui::SetCursorScreenPos(t.min);
    ImGui::PushID(n.id.c_str());
    ImGui::InvisibleButton("tile", ImVec2(std::max(1.0f, w), std::max(1.0f, h)));
    ImGui::PopID();
    if (ImGui::IsItemClicked() && hover.node.empty()) { if (!io.KeyShift && !state.group.count(n.id)) state.group.clear(); state.lassoing = false; state.lastPicked = picked; picked = n.id; state.held = n.id; state.dragFree = false; state.dragMoved = false; state.dragForce = io.KeyShift; state.grabX = io.MousePos.x - t.min.x; state.grabY = io.MousePos.y - t.min.y; }
    if (ImGui::IsItemHovered() && hover.node.empty()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (n.type != "out" && ImGui::BeginPopupContextItem("tilemenu")) {
      if (textButton("replace...")) { state.replacing = n.id; openInsertMenu(state.replaceMenu, "replacenode"); ImGui::CloseCurrentPopup(); }
      if (textButton("delete")) { doc.apply(std::make_unique<DeleteNode>(instrumentId, n.id)); if (picked == n.id) picked.clear(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); ImGui::EndChild(); return true; }
      ImGui::EndPopup();
    }
  }

  // Wiring, as in the web app. Pressing a port with wires (no Shift) picks up a wire by that end once the mouse has moved a little; pressing
  // a free input pulls a wire from it towards an output; pressing an output (or Shift+pressing any port) pulls a new wire from it.
  const auto portAt = [&](const std::string& address, bool output) {
    const auto t = tiles.find(nodeOfAddress(address));
    return t == tiles.end() ? ImVec2(0, 0) : portPos(t->second, output, indexOf(output ? t->second.type->outputs : t->second.inputs, portOfAddress(address)), scale);
  };
  // A handle of an equaliser's response picture: pressed, it is dragged (one undo step) and the click goes no further.
  bool onHandle = false;
  if (state.responseDrag >= 0 && state.responseDrag < static_cast<int>(state.response.list.size())) {
    const ResponseHandles::Handle& handle = state.response.list[static_cast<std::size_t>(state.responseDrag)];
    const auto found = tiles.find(handle.node);
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && found != tiles.end()) {
      onHandle = true;
      for (const auto& [name, value] : dragResponse(state.response, handle, *found->second.node, *found->second.type, io.MousePos)) {
        if (value != found->second.node->param(name, value + 1)) { doc.apply(std::make_unique<SetParam>(instrumentId, handle.node, name, value), !state.responseFresh); state.responseFresh = false; state.paramEdited = true; }
      }
    } else state.responseDrag = -1;
  } else if (windowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    float best = 10 * scale;
    for (std::size_t i = 0; i < state.response.list.size(); i++) {
      const ImVec2 d(state.response.list[i].pos.x - io.MousePos.x, state.response.list[i].pos.y - io.MousePos.y);
      if (std::sqrt(d.x * d.x + d.y * d.y) < best) { best = std::sqrt(d.x * d.x + d.y * d.y); state.responseDrag = static_cast<int>(i); state.responseFresh = true; onHandle = true; }
    }
  }
  const bool idle = state.wiring.empty() && state.wiringIn.empty() && !state.rewire.active && !onHandle;
  if (idle && windowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    state.message.clear();
    if (!hover.node.empty()) {
      const std::string address = hover.node + "." + hover.port;
      const Connection* first = nullptr;
      for (const Connection& c : graph.connections) if (!first && (hover.output ? c.from == address : c.to == address)) first = &c;
      if (litWire) first = litWire;   // (Tab chose among several)
      state.held.clear();
      state.wireFrom.clear(); state.wireTo.clear();
      if (first && !io.KeyShift) {
        state.rewire = {true, false, !hover.output, first->from, first->to, io.MousePos};
        picked = hover.node;
      } else if (!hover.output) {
        state.wiringIn = address;
        picked = hover.node;
      } else {
        state.wiring = address;
      }
    } else if (hoverWire) {
      state.wireFrom = hoverWire->from; state.wireTo = hoverWire->to;
    } else if (!state.held.empty()) {
      state.wireFrom.clear(); state.wireTo.clear();   // (a click on a tile)
    } else {
      state.wireFrom.clear(); state.wireTo.clear();
      state.group.clear();
      state.lassoing = true; state.lassoFrom = ImVec2(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
    }
  }
  const auto tryApply = [&](std::unique_ptr<Edit> edit) {
    try { doc.apply(std::move(edit)); moved = true; state.message.clear(); }
    catch (const std::exception& e) { state.message = e.what(); }
  };
  if (!state.wiring.empty()) {   // a new wire from an output: it lands on an input port
    const ImVec2 start = portAt(state.wiring, true);
    drawWire(dl, start, io.MousePos, kAccent, 1.75f * std::max(1.0f, scale) * 0.65f, false, scale);
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (!hover.node.empty() && !hover.output) tryApply(std::make_unique<Connect>(instrumentId, state.wiring, hover.node + "." + hover.port));
      state.wiring.clear();
    }
  }
  if (!state.wiringIn.empty()) {   // a new wire from a free input: it lands on an output port
    const ImVec2 end = portAt(state.wiringIn, false);
    drawWire(dl, io.MousePos, end, kAccent, 1.75f * std::max(1.0f, scale) * 0.65f, false, scale);
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (!hover.node.empty() && hover.output) tryApply(std::make_unique<Connect>(instrumentId, hover.node + "." + hover.port, state.wiringIn));
      state.wiringIn.clear();
    }
  }
  if (state.rewire.active) {   // a picked-up wire: dropped on a port of the same side it moves there, anywhere else it is unplugged, Esc puts it back
    auto& r = state.rewire;
    if (!r.started && ImGui::IsMouseDown(ImGuiMouseButton_Left) && std::hypot(io.MousePos.x - r.press.x, io.MousePos.y - r.press.y) > 4 * scale) {
      r.started = true;
      state.message = "Drop it on another port, or anywhere else to leave it unplugged (Esc puts it back)";
    }
    if (r.started) {
      const ImVec2 fixed = r.movingTo ? portAt(r.from, true) : portAt(r.to, false);
      if (r.movingTo) drawWire(dl, fixed, io.MousePos, kAccent, 1.75f * std::max(1.0f, scale) * 0.65f, false, scale);
      else drawWire(dl, io.MousePos, fixed, kAccent, 1.75f * std::max(1.0f, scale) * 0.65f, false, scale);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { r = {}; state.message.clear(); }
    else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (r.started) {
        const bool onWanted = !hover.node.empty() && hover.output == !r.movingTo;
        if (onWanted) {
          const std::string port = hover.node + "." + hover.port;
          if (r.movingTo) tryApply(std::make_unique<MoveWire>(instrumentId, r.from, r.to, r.from, port));
          else tryApply(std::make_unique<MoveWire>(instrumentId, r.from, r.to, port, r.to));
          if (state.message.empty()) state.message = "Wire moved to " + port;
        } else {
          tryApply(std::make_unique<Disconnect>(instrumentId, r.from, r.to));
          state.message = "Wire unplugged";
        }
      }
      r = {};
    }
  }
  // Delete cuts the selected wire
  if (!state.wireFrom.empty() && windowHovered && !io.WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
    tryApply(std::make_unique<Disconnect>(instrumentId, state.wireFrom, state.wireTo));
    state.wireFrom.clear(); state.wireTo.clear();
  }

  if (state.lassoing) {   // a lasso on empty space: the nodes it touches become the group
    const ImVec2 a(origin.x + state.lassoFrom.x, origin.y + state.lassoFrom.y), b = io.MousePos;
    const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      dl->AddRect(lo, hi, accentAlpha(200), 0, 0, 1.0f);
      dl->AddRectFilled(lo, hi, accentAlpha(25));
    } else {
      if (hi.x - lo.x >= 4 * scale || hi.y - lo.y >= 4 * scale)
        for (const auto& [id, t] : tiles) if (t.min.x < hi.x && t.max.x > lo.x && t.min.y < hi.y && t.max.y > lo.y && t.node->type != "out") state.group.insert(id);
      state.lassoing = false;
    }
  }
  // Keys, while the mouse is over the graph and no text box has the keyboard: Delete or Ctrl+X removes the picked node(s) (Alt+R joins the
  // wires round them first), Ctrl+D duplicates, Tab and Alt+arrows pick the next node.
  if (windowHovered && !io.WantTextInput && state.wireFrom.empty() && state.held.empty()) {
    std::vector<std::string> ids(state.group.begin(), state.group.end());
    if (ids.empty() && !picked.empty()) ids.push_back(picked);
    ids.erase(std::remove_if(ids.begin(), ids.end(), [&](const std::string& id) { const auto t = tiles.find(id); return t == tiles.end() || t->second.node->type == "out"; }), ids.end());
    const bool remove = ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || (io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_X, false));
    const bool join = io.KeyAlt && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false);
    if (io.KeyAlt && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {   // Alt+V: the pictures stay beside the node(s) (again: they show only while picked)
      std::vector<std::string> pictured;
      bool allKept = true;
      for (const std::string& id : ids) {
        const GraphNode* node = tiles.at(id).node;
        if (!hasEnvelopes(node->type) && !hasResponse(node->type) && !hasPicture(node->type)) continue;
        pictured.push_back(id);
        if (!node->keepView) allKept = false;
      }
      if (!pictured.empty()) {
        doc.apply(std::make_unique<SetKeepView>(instrumentId, pictured, !allKept));
        state.message = allKept ? "Its picture shows only while it is picked" : "Its picture stays on when it is not picked (Alt+V again to stop)";
      }
    }
    if ((remove || join) && !ids.empty()) {
      std::vector<std::unique_ptr<Edit>> edits;
      for (const std::string& id : ids) {
        if (join) {
          const Tile& t = tiles.at(id);
          std::vector<std::string> inputs;
          for (const std::string& in : t.type->inputs) if (kindOf(t, in) != PortKind::Control) inputs.push_back(id + "." + in);
          if (!inputs.empty() && !t.type->outputs.empty()) edits.push_back(std::make_unique<JoinAround>(instrumentId, inputs, id + "." + t.type->outputs.front()));
        }
        edits.push_back(std::make_unique<DeleteNode>(instrumentId, id));
      }
      std::unique_ptr<Edit> first = std::move(edits.front());
      edits.erase(edits.begin());
      tryApply(edits.empty() ? std::move(first) : std::make_unique<FollowedEdit>(std::move(first), std::move(edits)));
      if (state.message.empty()) { picked.clear(); state.group.clear(); state.message = ids.size() == 1 ? "Removed " + ids.front() : "Removed " + std::to_string(ids.size()) + " nodes"; }
    }
    if (io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_D, false) && !ids.empty()) {
      std::set<std::string> used;
      for (const GraphNode& n : graph.nodes) used.insert(n.id);
      std::vector<std::pair<std::string, std::string>> copies;
      for (const std::string& id : ids) {
        const std::string type = tiles.at(id).node->type;
        std::string fresh = type;
        for (int k = 1; used.count(fresh); k++) fresh = type + std::to_string(k);
        used.insert(fresh);
        copies.emplace_back(id, fresh);
      }
      tryApply(std::make_unique<DuplicateNodes>(instrumentId, copies));
      if (state.message.empty()) {
        state.group.clear();
        if (copies.size() > 1) for (const auto& c : copies) state.group.insert(c.second);
        picked = copies.back().second;
        state.message = copies.size() == 1 ? "Duplicated as " + picked : "Duplicated " + std::to_string(copies.size()) + " nodes";
      }
    }
    int way = 0;   // Tab and Shift+Tab step through the nodes left to right; Alt+arrows go to the nearest one that way
    const bool tab = ImGui::IsKeyPressed(ImGuiKey_Tab, false) && hover.node.empty();
    const ImGuiKey arrows[4] = {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow};
    if (io.KeyAlt && !io.KeyCtrl) for (int k = 0; k < 4; k++) if (ImGui::IsKeyPressed(arrows[k], false)) way = k + 1;
    if ((tab || way) && !graph.nodes.empty()) {
      std::vector<const Tile*> order;
      for (const auto& [id, t] : tiles) order.push_back(&t);
      std::sort(order.begin(), order.end(), [](const Tile* a, const Tile* b) { const ImVec2 ca = centreOf(*a), cb = centreOf(*b); return ca.x != cb.x ? ca.x < cb.x : ca.y < cb.y; });
      const auto here = std::find_if(order.begin(), order.end(), [&](const Tile* t) { return t->node->id == picked; });
      const Tile* next = nullptr;
      if (here == order.end()) next = order.front();
      else if (tab) next = order[(static_cast<std::size_t>(here - order.begin()) + (io.KeyShift ? order.size() - 1 : 1)) % order.size()];
      else {
        const ImVec2 c = centreOf(**here);
        float best = 1e30f;
        for (const Tile* t : order) {
          if (t == *here) continue;
          const float dx = centreOf(*t).x - c.x, dy = centreOf(*t).y - c.y;
          const float along = way == 1 ? -dx : way == 2 ? dx : way == 3 ? -dy : dy, across = way <= 2 ? std::fabs(dy) : std::fabs(dx);
          if (along <= 0) continue;
          if (along + 2 * across < best) { best = along + 2 * across; next = t; }
        }
      }
      if (next) { picked = next->node->id; state.group.clear(); }
    }
  }

  if (windowHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0)) {   // the middle button pans the whole graph, as in the web app
    ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);
    ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
  }
  // dragging the held tile; it lands on the grid unless Ctrl is held
  if (!state.held.empty()) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { state.dragFree = true; state.message = "Moving " + state.held + " only: its wires stay as they are"; }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (!state.dropFrom.empty()) tryApply(std::make_unique<InsertOnWire>(instrumentId, state.dropFrom, state.dropTo, chainIn, chainOut));
      else if (state.dropPull) tryApply(std::make_unique<PullOut>(instrumentId, chainIn, chainOut));
      if (!state.dropFrom.empty() || state.dropPull) { if (state.message.rfind("Release", 0) == 0) state.message.clear(); }
      else if (state.message.rfind("Moving", 0) == 0) state.message.clear();
      if (!state.dragMoved && state.dragForce) {   // Shift+click, no drag: the node joins (or leaves) the group
        if (state.group.empty() && !state.lastPicked.empty() && state.lastPicked != state.held) state.group.insert(state.lastPicked);
        if (!state.group.erase(state.held)) state.group.insert(state.held);
      }
      state.dropFrom.clear(); state.dropTo.clear(); state.dropPull = false; state.dragMoved = false;
      state.held.clear();
    } else {
      float nx = (io.MousePos.x - state.grabX - origin.x) / scale, ny = (io.MousePos.y - state.grabY - origin.y) / scale;
      if (!io.KeyCtrl) { nx = std::round(nx / kGrid) * kGrid; ny = std::round(ny / kGrid) * kGrid; }
      const GraphNode* held = nullptr;
      for (const GraphNode& n : graph.nodes) if (n.id == state.held) held = &n;
      if (held && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3) && (std::fabs(nx - held->x) > 0.5 || std::fabs(ny - held->y) > 0.5 || !held->placed)) {
        if (state.group.size() > 1 && state.group.count(state.held) && !state.dragForce) {   // the group moves by as much as the held tile
          const float dx = std::max(0.0f, nx) - static_cast<float>(held->x), dy = std::max(0.0f, ny) - static_cast<float>(held->y);
          std::vector<MoveNodes::Place> places;
          for (const GraphNode& n : graph.nodes) if (state.group.count(n.id)) places.push_back({n.id, std::max(0.0, n.x + dx), std::max(0.0, n.y + dy)});
          doc.apply(std::make_unique<MoveNodes>(instrumentId, places), true);
        } else {
          doc.apply(std::make_unique<MoveNode>(instrumentId, state.held, std::max(0.0f, nx), std::max(0.0f, ny)), true);
        }
        moved = true;
        state.dragMoved = true;
      }
    }
  }
  // right-click on empty space: the add-node menu (a cascading tree, as in the web app)
  const WireEnds* rightWire = nullptr;   // right-click on an audio wire: a node to put into it
  if (hoverWire && windowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hover.node.empty())
    for (const WireEnds& w : wires) if (w.c == hoverWire && (w.kind == PortKind::Audio || w.kind == PortKind::Stereo)) rightWire = &w;
  if (rightWire) {
    state.menuWireFrom = rightWire->c->from; state.menuWireTo = rightWire->c->to;
    state.addAt = ImVec2(((rightWire->p0.x + rightWire->p1.x) / 2 - origin.x) / scale - 30, ((rightWire->p0.y + rightWire->p1.y) / 2 - origin.y) / scale - 12);
    openInsertMenu(state.wireMenu, "wireinsert");
  }
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_None) && !ImGui::IsAnyItemHovered() && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) && hover.node.empty() && !hoverWire) {   // (a double-click on empty space too, as the web)
    state.addAt = ImVec2((io.MousePos.x - origin.x) / scale, (io.MousePos.y - origin.y) / scale);
    openInsertMenu(state.menu, "addnode");
  }
  {
    std::set<std::string> available;
    for (const std::string& name : types.typeNames()) if (name != "out" && name != "tempo") available.insert(name);
    const std::string name = drawInsertMenu(state.menu, "addnode", available);
    if (!name.empty()) {
      std::string id = name;
      for (int k = 1;; k++) {
        bool taken = false;
        for (const GraphNode& n : graph.nodes) taken |= n.id == id;
        if (!taken) break;
        id = name + std::to_string(k);
      }
      float x = std::round(state.addAt.x / kGrid) * kGrid, y = std::round(state.addAt.y / kGrid) * kGrid;
      doc.apply(std::make_unique<AddNode>(instrumentId, name, id, std::max(0.0f, x), std::max(0.0f, y)));
      picked = id;
      moved = true;
    }
  }
  if (!state.menuWireFrom.empty()) {   // the menu for a node to put into a wire: added at the wire and wired in as one undo step
    std::set<std::string> available;
    for (const std::string& name : types.typeNames()) {
      const NodeType* t = types.find(name);
      if (!t || name == "out" || name == "tempo" || t->inputs.empty() || t->outputs.empty()) continue;
      const GraphNode blank{};
      const PortKind in = portKind(*t, blank, t->inputs.front()), out = portKind(*t, blank, t->outputs.front());
      if ((in == PortKind::Audio || in == PortKind::Stereo) && (out == PortKind::Audio || out == PortKind::Stereo)) available.insert(name);
    }
    const std::string name = drawInsertMenu(state.wireMenu, "wireinsert", available);
    if (!name.empty()) {
      std::string id = name;
      for (int k = 1;; k++) {
        bool taken = false;
        for (const GraphNode& n : graph.nodes) taken |= n.id == id;
        if (!taken) break;
        id = name + std::to_string(k);
      }
      const NodeType* t = types.find(name);
      const float x = std::round(state.addAt.x / kGrid) * kGrid, y = std::round(state.addAt.y / kGrid) * kGrid;
      std::vector<std::unique_ptr<Edit>> follow;
      follow.push_back(std::make_unique<InsertOnWire>(instrumentId, state.menuWireFrom, state.menuWireTo, id + "." + t->inputs.front(), id + "." + t->outputs.front()));
      tryApply(std::make_unique<FollowedEdit>(std::make_unique<AddNode>(instrumentId, name, id, std::max(0.0f, x), std::max(0.0f, y)), std::move(follow)));
      if (state.message.empty()) picked = id;
      state.menuWireFrom.clear(); state.menuWireTo.clear();
    } else if (!ImGui::IsPopupOpen("wireinsert")) { state.menuWireFrom.clear(); state.menuWireTo.clear(); }
  }
  if (!state.replacing.empty()) {   // the replace menu: the type the node becomes
    std::set<std::string> available;
    for (const std::string& name : types.typeNames()) if (name != "out" && name != "tempo") available.insert(name);
    const std::string name = drawInsertMenu(state.replaceMenu, "replacenode", available);
    if (!name.empty()) {
      std::string sample;   // a node that plays samples starts on the first one of the song
      if (doc.project().samples.is_object())
        for (const auto& [id, value] : doc.project().samples.items()) if (!isPoolSample(id)) { sample = id; break; }
      auto edit = std::make_unique<ReplaceNode>(instrumentId, state.replacing, name, sample);
      const ReplaceNode* made = edit.get();
      tryApply(std::move(edit));
      if (state.message.empty()) { picked = made->newId(); state.message = state.replacing + " is now a " + name + (made->newId() != state.replacing ? " (" + made->newId() + ")" : std::string()) + "."; }
      state.replacing.clear();
    } else if (!ImGui::IsPopupOpen("replacenode")) state.replacing.clear();
  }
  // the pictures beside a node (its envelopes): for the picked node, and for every node told to keep them
  state.response.list.clear();
  const PictureTiming timing{60000.0 / (std::max(1.0, doc.project().song.bpm) * std::max(1, doc.project().song.ticksPerBeat)), std::max(1, doc.project().song.ticksPerBeat), &doc.project(), instrumentId, doc.revision()};
  for (const GraphNode& n : graph.nodes) {
    const auto found = tiles.find(n.id);
    if (found == tiles.end() || !(n.keepView || n.id == picked)) continue;
    if (hasResponse(n.type)) drawNodeResponse(dl, font, n, *found->second.type, found->second.min, found->second.max, dl->GetClipRectMin(), dl->GetClipRectMax(), scale, state.response);
    float below = 0;   // a node with a picture and envelopes: the envelopes go under the picture
    if (hasPicture(n.type)) below = drawNodePicture(dl, font, n, *found->second.type, found->second.min, found->second.max, dl->GetClipRectMin(), dl->GetClipRectMax(), scale, timing, state.response);
    if (!hasEnvelopes(n.type)) continue;
    drawNodeEnvelopes(dl, font, n, *found->second.type, ImVec2(found->second.min.x, found->second.min.y + below), ImVec2(found->second.max.x, found->second.max.y + below), dl->GetClipRectMin(), dl->GetClipRectMax(), scale);
  }
  {   // the line under the graph: a refused wire, else what the mouse is over
    const std::string& text = !state.message.empty() ? state.message : hoverText;
    if (!text.empty()) dl->AddText(font, nameSize, ImVec2(dl->GetClipRectMin().x + 8 * scale, dl->GetClipRectMax().y - nameSize - 6 * scale), dimAlpha(220), text.c_str());
  }
  ImGui::SetCursorScreenPos(ImVec2(origin.x + right + 40 * scale, origin.y + bottom + 40 * scale));
  ImGui::Dummy(ImVec2(1, 1));
  ImGui::EndChild();
  return moved;
}

}  // namespace wax::app

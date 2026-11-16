#include "app/param_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

#include "app/gate_window.h"
#include "app/shape_window.h"
#include "app/node_help.h"
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

// The hex a parameter's pattern cell holds for a value, `digits` wide.
std::string cellHex(const ParamSpec& spec, bool log, double value, int digits) {
  const double denom = digits == 1 ? 15 : digits == 4 ? 65535 : 255;
  double x;
  if (spec.isInt) x = value - spec.min;
  else if (log && spec.min > 0) x = (std::log(std::max(value, spec.min)) - std::log(spec.min)) / (std::log(spec.max) - std::log(spec.min)) * denom;
  else x = spec.max > spec.min ? (value - spec.min) / (spec.max - spec.min) * denom : 0;
  char text[16];
  std::snprintf(text, sizeof text, "%0*X", digits, static_cast<unsigned>(std::clamp(std::round(x), 0.0, denom)));
  return text;
}

// A floating panel's height follows its rows: what the last frame's content needed (an estimate on the first), at most two thirds of the
// graph area, and past that the window scrolls. The callback pins the height while the user can still drag the width.
void pinHeight(ImGuiSizeCallbackData* data) { data->DesiredSize.y = *static_cast<float*>(data->UserData); }

float wantedHeight(float contentHeight, int rows, float areaHeight, float scale) {
  const float content = contentHeight > 0 ? contentHeight : (rows + 2) * 20 * scale;
  return std::min(content, areaHeight * 2.0f / 3.0f);
}

// The window's content height, for the next frame (call just before End).
float measuredContent() { return ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y; }

// The keys of a panel, while the mouse is over it and no text box has the keyboard (the piano keys still play): Up/Down choose a row, Home/End
// the first and last, PageUp/PageDown jump eight, Left/Right change the chosen row (Shift: more, Ctrl: less), Enter opens its menu. Alt is the
// graph's (Alt+arrows pick a node).
struct RowKeys {
  int dir = 0;          // Left/Right: -1, +1
  float amount = 1;     // how many steps (5 with Shift, 0.2 with Ctrl)
  bool toEnd = false, toStart = false, menu = false;
};

RowKeys panelKeys(int& cursor, int rows) {
  RowKeys keys;
  const ImGuiIO& io = ImGui::GetIO();
  const bool active = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) && !io.WantTextInput && !io.KeyAlt && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
  cursor = std::clamp(cursor, 0, std::max(0, rows - 1));
  if (!active) return keys;
  const auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
  if (pressed(ImGuiKey_DownArrow)) cursor = std::min(rows - 1, cursor + 1);
  if (pressed(ImGuiKey_UpArrow)) cursor = std::max(0, cursor - 1);
  if (pressed(ImGuiKey_PageDown)) cursor = std::min(rows - 1, cursor + 8);
  if (pressed(ImGuiKey_PageUp)) cursor = std::max(0, cursor - 8);
  if (pressed(ImGuiKey_LeftArrow)) keys.dir = -1;
  if (pressed(ImGuiKey_RightArrow)) keys.dir = 1;
  keys.amount = io.KeyShift ? 5.0f : io.KeyCtrl ? 0.2f : 1.0f;
  keys.toStart = ImGui::IsKeyPressed(ImGuiKey_Home, false);
  keys.toEnd = ImGui::IsKeyPressed(ImGuiKey_End, false);
  keys.menu = ImGui::IsKeyPressed(ImGuiKey_Enter, false);
  return keys;
}

// One row of a panel at the cursor: the parameter's name (or `label`), then its bar. `wired`: a wire drives it. Returns true when the value changed.
bool drawRow(ParamPanelState& state, Document& doc, const Engine& engine, const Instrument* instrument, const std::string& instrumentId,
             const GraphNode* picked, const ParamSpec& spec, const std::string& label, bool dim, float total, float nameW, const RowKeys* keys = nullptr, bool marked = false) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const float rowH = ImGui::GetTextLineHeight() + 3 * scale;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bool changed = false;
  {
    const std::string address = instrumentId + "." + picked->id + "." + spec.name;
    const double value = engine.registry().slotOf(address) >= 0 ? engine.registry().get(address) : picked->param(spec.name, spec.def);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // a wide panel also has the row's buttons at its right: e (a pattern column or not), p (a port or not) and the column's width (1h, 2h, 4h)
    const float buttonW = 16 * scale;
    const bool buttons = total >= 330 * scale;
    const float buttonsW = buttons ? 3 * buttonW : 0;
    const ImVec2 barMin(p.x + nameW, p.y + 1), barMax(p.x + total - buttonsW, p.y + rowH - 1);

    // the name, as many characters as fit
    std::string name = label;
    while (name.size() > 1 && ImGui::CalcTextSize(name.c_str()).x > nameW - 6 * scale) name.pop_back();
    const Exposed* shownAs = nullptr;   // the pattern column this parameter is, if any
    for (const Exposed& e : instrument->exposed) if (e.node == picked->id && e.param == spec.name) shownAs = &e;
    const bool hasPort = std::find(picked->ports.begin(), picked->ports.end(), spec.name) != picked->ports.end();
    dl->AddText(ImVec2(p.x + 2 * scale + (hasPort ? 7 * scale : 0), p.y + 1.5f * scale), kDimText, name.c_str());
    if (hasPort) dl->AddCircleFilled(ImVec2(p.x + 4 * scale, p.y + rowH * 0.5f), 2.5f * scale, kAccent);   // a dot: it has a port

    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + nameW, p.y + rowH)) && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {   // the web's "?" hint: on the name
      const std::string hint = paramHint(picked->type, spec.name);
      char facts[96];
      std::snprintf(facts, sizeof facts, "range %g to %g, default %g%s", spec.min, spec.max, spec.def, spec.isInt ? ", whole numbers" : "");
      ImGui::BeginTooltip();
      ImGui::PushTextWrapPos(300 * scale);
      ImGui::TextUnformatted(spec.name.c_str());
      if (!hint.empty()) ImGui::TextUnformatted(hint.c_str());
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "%s", facts);
      ImGui::PopTextWrapPos();
      ImGui::EndTooltip();
    }
    ImGui::PushID(picked->id.c_str());
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
    } else if ((hovered && ImGui::GetIO().MouseWheel != 0) || (keys && (keys->dir != 0 || keys->toStart || keys->toEnd))) {   // the wheel, or the keys on the chosen row
      const bool keyed = keys && (keys->dir != 0 || keys->toStart || keys->toEnd) && !(hovered && ImGui::GetIO().MouseWheel != 0);
      const int way = keyed ? keys->dir : ImGui::GetIO().MouseWheel > 0 ? 1 : -1;
      const double step = 0.01 * (keyed ? keys->amount : 1.0);
      if (keyed && keys->toStart) next = spec.min;
      else if (keyed && keys->toEnd) next = spec.max;
      else {
        next = fromPos(spec, toPos(spec, value) + way * step);
        if (spec.isInt && next == std::round(value)) next = value + way;
      }
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
    dl->AddRectFilled(barMin, barMax, textAlpha(hovered ? 14 : 8));
    const float x = barMin.x + static_cast<float>(toPos(spec, shown)) * (barMax.x - barMin.x);
    dl->AddRectFilled(barMin, ImVec2(x, barMax.y), accentAlpha(dim ? 20 : 48));
    dl->AddRectFilled(ImVec2(std::min(x, barMax.x - 2 * scale), barMin.y), ImVec2(std::min(x, barMax.x - 2 * scale) + 2 * scale, barMax.y), dim ? kDimText : kAccent);
    dl->AddText(ImVec2(barMin.x + 4 * scale, p.y + 1.5f * scale), dim ? kDimText : kText, displayValue(picked->type, spec, shown).c_str());
    if (shownAs) {   // the pattern column: its label and the hex its cell holds for this value, at the right of the bar
      const int digits = shownAs->bits == 16 ? 4 : shownAs->bits == 4 ? 1 : 2;
      const std::string tag = shownAs->label + " " + cellHex(spec, shownAs->log, shown, digits);
      dl->AddText(ImVec2(barMax.x - ImGui::CalcTextSize(tag.c_str()).x - 4 * scale, p.y + 1.5f * scale), kAccent, tag.c_str());
    }

    if (keys && keys->menu) ImGui::OpenPopup("rowmenu");   // Enter on the chosen row
    if (marked) dl->AddRectFilled(ImVec2(p.x, p.y + 1), ImVec2(p.x + 2 * scale, p.y + rowH - 1), kAccent);   // the row the keys act on
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
      ImGui::Separator();
      if (textButton(shownAs ? "Remove pattern column" : "Expose as pattern column")) {
        doc.apply(std::make_unique<SetExposed>(instrumentId, picked->id, spec.name, shownAs == nullptr)); changed = true; ImGui::CloseCurrentPopup();
      } else if (shownAs) {   // the column's width: one, two or four hex digits
        for (const auto& [label, bits] : {std::pair<const char*, int>{"1hex", 4}, {"2hex", 8}, {"4hex", 16}}) {
          const std::string text = std::string(shownAs->bits == bits ? "\xe2\x97\x8f " : "\xe2\x97\x8b ") + "width " + label;
          if (textButton(text.c_str())) { doc.apply(std::make_unique<SetExposedBits>(instrumentId, picked->id, spec.name, bits)); changed = true; ImGui::CloseCurrentPopup(); break; }
        }
      }
      if (textButton(hasPort ? "Remove the port" : "Add a port (wire a node in)")) {
        doc.apply(std::make_unique<SetPort>(instrumentId, picked->id, spec.name, !hasPort)); changed = true; ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
    if (buttons) {
      const bool column = shownAs != nullptr;
      const int bits = column ? shownAs->bits : 0;
      const bool wasPort = hasPort;
      struct Button { const char* text; bool lit; };
      const Button faces[3] = {{"e", column}, {"p", wasPort}, {!column ? "--" : bits == 16 ? "4h" : bits == 4 ? "1h" : "2h", column}};
      for (int k = 0; k < 3; k++) {
        const ImVec2 lo(barMax.x + k * buttonW + 1, p.y + 1), hi(barMax.x + (k + 1) * buttonW - 1, p.y + rowH - 1);
        ImGui::SetCursorScreenPos(lo);
        ImGui::PushID(k);
        ImGui::InvisibleButton("b", ImVec2(hi.x - lo.x, hi.y - lo.y));
        const bool over = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        ImGui::PopID();
        dl->AddRectFilled(lo, hi, faces[k].lit ? withAlpha(kWarm, 50) : textAlpha(over ? 24 : 10));
        const ImVec2 size = ImGui::CalcTextSize(faces[k].text);
        dl->AddText(ImVec2((lo.x + hi.x - size.x) / 2, p.y + 1.5f * scale), faces[k].lit ? kWarm : kDimText, faces[k].text);
        if (!clicked) continue;
        if (k == 0) doc.apply(std::make_unique<SetExposed>(instrumentId, picked->id, spec.name, !column));
        else if (k == 1) doc.apply(std::make_unique<SetPort>(instrumentId, picked->id, spec.name, !wasPort));
        else if (column) doc.apply(std::make_unique<SetExposedBits>(instrumentId, picked->id, spec.name, bits == 4 ? 8 : bits == 8 ? 16 : 4));
        else continue;
        changed = true;
      }
    }
    ImGui::PopID();
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::Dummy(ImVec2(1, 0));
  }
  return changed;
}

bool drawParamPanel(ParamPanelState& state, Document& doc, const Engine& engine, const std::string& instrumentId, ImVec2 areaMin, ImVec2 areaMax) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const Instrument* instrument = findInstrument(doc.project(), instrumentId);
  if (state.instrument != instrumentId) { state.instrument = instrumentId; state.node.clear(); state.dragging = false; }
  const NodeRegistry& types = builtinNodes();
  if (!state.open) return false;

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
  if (picked) state.node = picked->id;

  // the window: first at the top right of the graph, then wherever the user puts it
  const float width = kNormalWidth * 1.25f * scale;
  if (!state.placed) {
    ImGui::SetNextWindowPos(ImVec2(std::max(areaMin.x, areaMax.x - width - 12 * scale), areaMin.y + 12 * scale), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
  }
  float height = wantedHeight(state.paramContent, picked ? static_cast<int>(type->params.size()) : 1, areaMax.y - areaMin.y, scale);
  ImGui::SetNextWindowSizeConstraints(ImVec2(kMinWidth * scale, 0), ImVec2(kMaxWidth * 3 * scale, FLT_MAX), pinHeight, &height);
  const std::string title = picked ? picked->id + "  " + picked->type + "###params" : "no parameters###params";
  ImGui::PushStyleColor(ImGuiCol_TitleBg, colour(kAccent));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, colour(kAccent));
  ImGui::PushStyleColor(ImGuiCol_Text, colour(kHiFg));
  const bool visible = ImGui::Begin(title.c_str(), &state.open, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleColor(3);
  state.placed = true;
  bool changed = false;
  if (!visible || !picked || !instrument) {
    if (visible && !picked) ImGui::TextColored(colour(kDimText), "no parameters");
    ImGui::End();
    return false;
  }
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);   // a small face: more rows fit

  if (!drawingsOf(picked->type).empty() && textButton("draw envelopes")) state.drawNode = picked->id;
  if (picked->type == "dvii" && textButton("edit voice")) state.dxNode = picked->id;
  if (hasGate(picked->type) && textButton("edit steps")) state.gateNode = picked->id;
  if (picked->type == "chord" && textButton("edit chords")) state.chordNode = picked->id;
  if (hasShape(picked->type) && textButton(picked->type == "osc" || picked->type == "westcoast" || picked->type == "fmsynth" ? "draw wave" : picked->type == "addsynth" ? "draw spectrum" : picked->type == "shiva" ? "edit taps" : "draw curve")) state.shapeNode = picked->id;

  std::set<std::string> wired;   // parameters a wire drives
  for (const Connection& c : instrument->graph.connections) {
    const std::string prefix = picked->id + ".@";
    if (c.to.rfind(prefix, 0) == 0) wired.insert(c.to.substr(prefix.size()));
  }

  const float rowH = ImGui::GetTextLineHeight() + 3 * scale;
  const float total = ImGui::GetContentRegionAvail().x;
  float nameW = 0;   // as wide as the longest name (and its port dot), not a share of the panel
  for (const ParamSpec& spec : type->params) if (spec.name != "bypass") nameW = std::max(nameW, ImGui::CalcTextSize(spec.name.c_str()).x);
  nameW = std::min(nameW + 14 * scale, total * 0.5f);
  int shownRows = 0;
  for (const ParamSpec& spec : type->params) if (spec.name != "bypass") shownRows++;
  const RowKeys keys = panelKeys(state.paramCursor, shownRows);
  int index = 0;
  for (const ParamSpec& spec : type->params) {
    if (spec.name == "bypass") continue;
    const bool chosen = index++ == state.paramCursor;
    if (drawRow(state, doc, engine, instrument, instrumentId, picked, spec, spec.name, wired.count(spec.name) > 0, total, nameW, chosen ? &keys : nullptr, chosen)) changed = true;
  }
  ImGui::PopFont();
  state.paramContent = measuredContent();
  ImGui::End();
  return changed;
}

}  // namespace wax::app

namespace wax::app {
namespace {

// A window of the exposed sliders of `instrumentId` (nothing is drawn when it exposes nothing). `closed` is set when its X was pressed; `pinState`:
// 0 no pin button, 1 "pin", 2 "unpin" (`pinPressed` is set when pressed).
bool drawExposedWindow(ParamPanelState& state, ExposedWindow& win, const char* windowId, const std::string& title, Document& doc, const Engine& engine,
                       const std::string& instrumentId, ImVec2 areaMin, ImVec2 areaMax, ImVec2 firstAt, bool* open, int pinState, bool* pinPressed) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const Instrument* instrument = findInstrument(doc.project(), instrumentId);
  win.min = win.max = ImVec2(0, 0);
  if (!instrument || instrument->exposed.empty()) return false;
  const NodeRegistry& types = builtinNodes();

  const float width = kNormalWidth * 1.1f * scale;
  // dragged about by its title, and wider or narrower by its edge; its height is its rows' (see pinHeight)
  if (!win.placed) {
    ImGui::SetNextWindowPos(firstAt, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
  }
  float height = wantedHeight(win.content, static_cast<int>(instrument->exposed.size()) + (pinState ? 1 : 0), areaMax.y - areaMin.y, scale);
  ImGui::SetNextWindowSizeConstraints(ImVec2(kMinWidth * scale, 0), ImVec2(kMaxWidth * 3 * scale, FLT_MAX), pinHeight, &height);
  ImGui::PushStyleColor(ImGuiCol_TitleBg, colour(kAccent));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, colour(kAccent));
  ImGui::PushStyleColor(ImGuiCol_Text, colour(kHiFg));
  const bool visible = ImGui::Begin((title + "###" + windowId).c_str(), open, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleColor(3);
  win.placed = true;
  win.min = ImGui::GetWindowPos();
  win.max = ImVec2(win.min.x + ImGui::GetWindowWidth(), win.min.y + ImGui::GetWindowHeight());
  bool changed = false;
  if (visible) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
    if (pinState && textButton(pinState == 2 ? "unpin" : "pin")) *pinPressed = true;
    const float total = ImGui::GetContentRegionAvail().x;
    float nameW = 0;   // as wide as the longest label (and its port dot), not a share of the panel
    for (const Exposed& e : instrument->exposed) nameW = std::max(nameW, ImGui::CalcTextSize((e.label.empty() ? e.param : e.label).c_str()).x);
    nameW = std::min(nameW + 14 * scale, total * 0.5f);
    const RowKeys keys = panelKeys(win.cursor, static_cast<int>(instrument->exposed.size()));
    int index = -1;
    for (const Exposed& e : instrument->exposed) {   // a row for each column the machine's patterns have
      index++;
      const GraphNode* node = nullptr;
      for (const GraphNode& n : instrument->graph.nodes) if (n.id == e.node) node = &n;
      const NodeType* type = node ? types.find(node->type) : nullptr;
      if (!type) continue;
      for (const ParamSpec& spec : type->params) {
        if (spec.name != e.param) continue;
        const std::string wiredTo = node->id + ".@" + spec.name;
        bool wired = false;
        for (const Connection& c : instrument->graph.connections) if (c.to == wiredTo) wired = true;
        if (drawRow(state, doc, engine, instrument, instrumentId, node, spec, e.label.empty() ? spec.name : e.label, wired, total, nameW, index == win.cursor ? &keys : nullptr, index == win.cursor)) changed = true;
      }
    }
    ImGui::PopFont();
    win.content = measuredContent();
  }
  ImGui::End();
  return changed;
}

}  // namespace

bool drawExposedDock(ParamPanelState& state, Document& doc, const Engine& engine, const std::string& instrumentId, ImVec2 areaMin, ImVec2 areaMax) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  return drawExposedWindow(state, state.dock, "exposed", "exposed", doc, engine, instrumentId, areaMin, areaMax, ImVec2(areaMin.x + 12 * scale, areaMin.y + 12 * scale), nullptr, 0, nullptr);
}

bool drawFloatPanels(ParamPanelState& state, Document& doc, const Engine& engine, const std::string& followInstrument, ImVec2 areaMin, ImVec2 areaMax) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  bool changed = false;
  int count = 0;
  std::string unpin;
  for (auto& [id, win] : state.pinned) {   // a window for each pinned machine
    bool open = true, pressed = false;
    const ImVec2 at(areaMin.x + (28 + 28 * count) * scale, areaMin.y + (28 + 28 * count) * scale);
    count++;
    if (drawExposedWindow(state, win, ("pin." + id).c_str(), id + " \xc2\xb7 parameters", doc, engine, id, areaMin, areaMax, at, &open, 2, &pressed)) changed = true;
    if (!open || pressed) unpin = id;
  }
  if (!unpin.empty()) state.pinned.erase(unpin);
  if (state.followOpen && !followInstrument.empty()) {   // the cursor lane's machine, with a pin
    bool open = true, pressed = false;
    const bool alreadyPinned = state.pinned.count(followInstrument) > 0;
    const ImVec2 at(areaMax.x - (kNormalWidth * 1.1f + 24) * scale, areaMin.y + 12 * scale);
    if (drawExposedWindow(state, state.follow, "follow", followInstrument + " \xc2\xb7 parameters", doc, engine, followInstrument, areaMin, areaMax, at, &open, alreadyPinned ? 2 : 1, &pressed)) changed = true;
    if (!open) state.followOpen = false;
    if (pressed) { if (alreadyPinned) state.pinned.erase(followInstrument); else state.pinned[followInstrument] = ExposedWindow(); }
  }
  return changed;
}

}  // namespace wax::app

#include "app/graph_toolbar.h"

#include <algorithm>
#include <cmath>

#include "app/config.h"
#include "app/theme.h"

namespace wax::app {

namespace {

constexpr float kButton = 22, kGap = 4, kPad = 4, kTop = 4;   // web pixels
constexpr int kSwitches = 6;

enum class Kind { Grid, Snap, Dashed, Straight, Pull, HideCc };

struct Switch {
  Kind kind;
  const char* name;
  const char* what;
};

const Switch kList[kSwitches] = {
    {Kind::Grid, "Grid", "the dot grid behind the nodes"},
    {Kind::Snap, "Snap", "nodes land on the grid dots when moved (hold Ctrl while dragging to move freely)"},
    {Kind::Dashed, "Dashed wires", "control and parameter wires drawn dashed, audio wires solid"},
    {Kind::Straight, "Straight short wires", "wires between close nodes go straight instead of in an S-curve"},
    {Kind::Pull, "Pull out", "dragging a node far out of its chain takes it out of the wires"},
    {Kind::HideCc, "Hide command parameters", "leaves out, in the parameter view, the parameters a command writes from a pattern"},
};

constexpr int kDefaultStraight = 60;   // what the switch turns the distance back on to

bool isOn(Kind kind) {
  const Settings& s = settings();
  switch (kind) {
    case Kind::Grid: return s.graphGrid;
    case Kind::Snap: return s.graphSnap;
    case Kind::Dashed: return s.wireDashed;
    case Kind::Straight: return s.wireStraight > 0;
    case Kind::Pull: return s.graphPullOut;
    case Kind::HideCc: return s.pvHideCc;
  }
  return false;
}

void flip(Kind kind) {
  Settings& s = settings();
  switch (kind) {
    case Kind::Grid: s.graphGrid = !s.graphGrid; break;
    case Kind::Snap: s.graphSnap = !s.graphSnap; break;
    case Kind::Dashed: s.wireDashed = !s.wireDashed; break;
    case Kind::Straight: s.wireStraight = s.wireStraight > 0 ? 0 : kDefaultStraight; break;
    case Kind::Pull: s.graphPullOut = !s.graphPullOut; break;
    case Kind::HideCc: s.pvHideCc = !s.pvHideCc; break;
  }
  saveSettings();
}

// The icons: thin strokes in a 16 by 16 box, as the web's.
void drawIcon(ImDrawList* dl, Kind kind, ImVec2 lo, float u, ImU32 ink) {   // u: pixels per icon unit
  const auto at = [&](float x, float y) { return ImVec2(lo.x + x * u, lo.y + y * u); };
  const float w = std::max(1.0f, 1.3f * u);
  switch (kind) {
    case Kind::Grid:
      for (float x : {3.0f, 8.0f, 13.0f}) for (float y : {3.0f, 8.0f, 13.0f}) dl->AddCircleFilled(at(x, y), 0.9f * u, ink);
      break;
    case Kind::Snap:   // a horseshoe magnet
      dl->PathArcTo(at(8, 5), 5 * u, 3.14159f, 6.28318f);
      dl->PathStroke(ink, 0, w);
      dl->AddLine(at(3, 5), at(3, 9), ink, w); dl->AddLine(at(13, 5), at(13, 9), ink, w);
      dl->AddRect(at(3, 9), at(6, 12), ink, 0, 0, w); dl->AddRect(at(10, 9), at(13, 12), ink, 0, 0, w);
      break;
    case Kind::Dashed:
      dl->AddLine(at(1.5f, 8), at(4, 8), ink, w); dl->AddLine(at(6.7f, 8), at(9.3f, 8), ink, w); dl->AddLine(at(12, 8), at(14.5f, 8), ink, w);
      break;
    case Kind::Straight:
      dl->AddLine(at(2, 12), at(14, 4), ink, w);
      dl->AddCircle(at(2.5f, 11.5f), 1.3f * u, ink, 0, w); dl->AddCircle(at(13.5f, 4.5f), 1.3f * u, ink, 0, w);
      break;
    case Kind::Pull:
      dl->AddLine(at(2, 8), at(6, 8), ink, w); dl->AddLine(at(10, 8), at(14, 8), ink, w);
      dl->AddRect(at(6, 3), at(10, 7), ink, 0.6f * u, 0, w); dl->AddLine(at(8, 9), at(8, 13), ink, w);
      break;
    case Kind::HideCc:   // two parentheses
      dl->PathArcTo(at(9, 8), 3.2f * u, 1.9f, 4.38f); dl->PathStroke(ink, 0, w);
      dl->PathArcTo(at(10, 8), 3.2f * u, -1.24f, 1.24f); dl->PathStroke(ink, 0, w);
      break;
  }
}

float width(float scale) { return (kPad * 2 + kSwitches * kButton + (kSwitches - 1) * kGap) * scale; }

}  // namespace

ToolbarBox graphToolbarBox(ImVec2 areaMin, ImVec2 areaMax, float scale) {
  ToolbarBox box;
  const float w = width(scale), h = (kButton + kPad * 2) * scale;
  if (areaMax.x - areaMin.x < w + 2 * kTop * scale) return box;   // no room: no toolbar
  box.min = ImVec2(std::round((areaMin.x + areaMax.x - w) / 2), areaMin.y + kTop * scale);
  box.max = ImVec2(box.min.x + w, box.min.y + h);
  return box;
}

void drawGraphToolbar(ImDrawList* dl, ImVec2 areaMin, ImVec2 areaMax, float scale, bool active) {
  const ToolbarBox box = graphToolbarBox(areaMin, areaMax, scale);
  if (box.max.x <= box.min.x) return;
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  dl->AddRectFilled(box.min, box.max, IM_COL32(24, 32, 36, 215), 5 * scale);
  dl->AddRect(box.min, box.max, lineAlpha(160), 5 * scale);
  for (int i = 0; i < kSwitches; i++) {
    const Switch& sw = kList[i];
    const ImVec2 lo(box.min.x + (kPad + i * (kButton + kGap)) * scale, box.min.y + kPad * scale), hi(lo.x + kButton * scale, lo.y + kButton * scale);
    const bool on = isOn(sw.kind), over = active && mouse.x >= lo.x && mouse.x <= hi.x && mouse.y >= lo.y && mouse.y <= hi.y;
    if (on) dl->AddRectFilled(lo, hi, accentAlpha(48), 3 * scale);
    if (over) dl->AddRect(lo, hi, accentAlpha(160), 3 * scale);
    const float unit = kButton * scale * 0.6f / 16;   // the icon fills about 60% of the button
    drawIcon(dl, sw.kind, ImVec2(lo.x + (kButton * scale - 16 * unit) / 2, lo.y + (kButton * scale - 16 * unit) / 2), unit, on ? kAccent : kDimText);
    if (over) {
      ImGui::SetTooltip("%s · %s · %s", sw.name, on ? "on" : "off", sw.what);
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) flip(sw.kind);
    }
  }
}

}  // namespace wax::app

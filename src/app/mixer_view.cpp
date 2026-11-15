#include "app/mixer_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "app/theme.h"

namespace wax::app {

namespace {

// The fader and the level share one scale in dB: kDbMin at the bottom, kDbMax at the top (nothing at all is the very bottom).
constexpr double kDbMin = -30, kDbMax = 6, kGainDefault = 0.8;
constexpr int kMajorDb[] = {6, 0, -6, -12, -24};
constexpr int kMinorDb[] = {3, -3, -9, -18};
constexpr float kStripWidth = 73, kScaleWidth = 32, kCarveWidth = 150;
constexpr ImU32 kRule = IM_COL32(60, 72, 84, 255);

double posOfGain(double g) { return g < 0.0316 ? 0 : std::clamp((20 * std::log10(g) - kDbMin) / (kDbMax - kDbMin), 0.0, 1.0); }
double gainAtPos(double p) { return p < 0.01 ? 0 : std::pow(10.0, (kDbMin + p * (kDbMax - kDbMin)) / 20); }
double posOfDb(double db) { return (db - kDbMin) / (kDbMax - kDbMin); }

std::string dbText(double g) {
  if (g < 0.001) return "-inf";
  char text[16];
  std::snprintf(text, sizeof text, "%.1f", 20 * std::log10(g));
  return text;
}

const GraphNode* outNode(const Instrument& instrument) {
  for (const GraphNode& node : instrument.graph.nodes) if (node.type == "out") return &node;
  return nullptr;
}

double current(const Engine& engine, const std::string& address, double fallback) {
  return engine.registry().slotOf(address) >= 0 ? engine.registry().get(address) : fallback;
}

struct Context {
  MixerState& state;
  Document& doc;
  const Engine& engine;
  bool changed = false;
  float scale = 1;

  void setParam(const std::string& instrument, const std::string& node, const std::string& param, double value, bool dragging) {
    const std::string address = instrument + "." + node + "." + param;
    const bool fold = dragging && state.draggingAddress == address;
    doc.apply(std::make_unique<SetParam>(instrument, node, param, value), fold);
    state.draggingAddress = dragging ? address : std::string();
    changed = true;
  }
};

// A small switch: a thin box with a word, filled when on. True when it was clicked.
bool button(const char* id, const char* text, bool on, float width, float scale) {
  const float height = ImGui::GetTextLineHeight() + 2 * scale;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, ImVec2(width, height));
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const bool hovered = ImGui::IsItemHovered();
  const ImU32 edge = on || hovered ? kAccent : IM_COL32(70, 100, 100, 255);
  if (on) draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), kAccent);
  draw->AddRect(at, ImVec2(at.x + width, at.y + height), edge);
  const float w = ImGui::CalcTextSize(text).x;
  draw->AddText(ImVec2(at.x + (width - w) * 0.5f, at.y + scale), on ? IM_COL32(16, 22, 26, 255) : IM_COL32(205, 214, 222, 255), text);
  return ImGui::IsItemClicked();
}

// A small bar with its value written in it, as the parameter panel of the web app draws them. Press to jump, drag, double-click resets.
void carveBar(Context& cx, const char* label, const char* param, double lo, double hi, double def, const char* format, bool percent) {
  const float scale = cx.scale;
  const float nameWidth = 46 * scale;
  const float height = ImGui::GetTextLineHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddText(at, kDimText, label);
  const float barWidth = std::max(30.0f, ImGui::GetContentRegionAvail().x - nameWidth - 4 * scale);
  const ImVec2 bar(at.x + nameWidth, at.y);
  ImGui::PushID(param);
  ImGui::SetCursorScreenPos(bar);
  ImGui::InvisibleButton("##bar", ImVec2(barWidth, height));
  const bool active = ImGui::IsItemActive();
  const double now = current(cx.engine, std::string("master.m.") + param, def);
  const bool on = current(cx.engine, "master.m.carve", 0) >= 0.5;
  if (active) {
    const double p = std::clamp((ImGui::GetIO().MousePos.x - bar.x) / barWidth, 0.0f, 1.0f);
    const double wanted = std::round((lo + p * (hi - lo)) * 100) / 100;
    if (wanted != now) cx.setParam("master", "m", param, wanted, true);
  } else if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && now != def) {
    cx.setParam("master", "m", param, def, false);
  }
  const double unit = std::clamp((current(cx.engine, std::string("master.m.") + param, def) - lo) / (hi - lo), 0.0, 1.0);
  const float x = bar.x + barWidth * static_cast<float>(unit);
  draw->AddRectFilled(bar, ImVec2(bar.x + barWidth, bar.y + height), IM_COL32(120, 134, 146, on ? 30 : 16));
  draw->AddRectFilled(bar, ImVec2(x, bar.y + height), IM_COL32(64, 190, 168, on ? 70 : 30));
  draw->AddRectFilled(ImVec2(x - scale, bar.y), ImVec2(x + scale, bar.y + height), on ? kAccent : kDimText);
  char text[24];
  const double value = current(cx.engine, std::string("master.m.") + param, def);
  if (percent) std::snprintf(text, sizeof text, "%d%%", static_cast<int>(std::lround(value * 100)));
  else std::snprintf(text, sizeof text, format, value);
  draw->AddText(ImVec2(bar.x + 4 * scale, bar.y), on ? IM_COL32(205, 214, 222, 255) : kDimText, text);
  ImGui::PopID();
  ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + height + 3 * scale));
}

void drawCarve(Context& cx) {
  const float scale = cx.scale;
  ImGui::BeginChild("carve", ImVec2(kCarveWidth * scale, 0), false, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
  const bool on = current(cx.engine, "master.m.carve", 0) >= 0.5;
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "CARVE");
  ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30 * scale + ImGui::GetCursorPosX());
  if (button("##carveOn", on ? "on" : "off", on, 30 * scale, scale)) cx.setParam("master", "m", "carve", on ? 0 : 1, false);
  ImGui::Separator();
  carveBar(cx, "Amt", "cvamt", 0, 1, 0.5, "", true);
  carveBar(cx, "Depth", "cvdepth", 0, 15, 6, "%.0f dB", false);
  carveBar(cx, "Focus", "cvfocus", 0, 1, 0.6, "", true);
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), on ? "+85 ms delay" : "off: no delay");
  ImGui::EndChild();
}

void drawScale(float trackTop, float trackHeight, float scale) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  for (const int db : kMajorDb) {
    const float y = trackTop + trackHeight * static_cast<float>(1 - posOfDb(db));
    char text[8];
    std::snprintf(text, sizeof text, db > 0 ? "+%d" : "%d", db);
    draw->AddText(ImVec2(at.x + 2 * scale, y - ImGui::GetTextLineHeight() * 0.5f), kDimText, text);
  }
}

}  // namespace

float mixerHeight() { return 178.0f * ImGui::GetStyle().FontScaleDpi; }

bool drawMixer(MixerState& state, Document& doc, Engine& engine, const std::string& selectedLane) {
  Context cx{state, doc, engine};
  cx.scale = ImGui::GetStyle().FontScaleDpi;
  const float scale = cx.scale;
  const float lineHeight = ImGui::GetTextLineHeight();
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) state.draggingAddress.clear();

  ImGui::BeginChild("mixer", ImVec2(0, mixerHeight()), false, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_HorizontalScrollbar);
  drawCarve(cx);
  ImGui::SameLine(0, 0);

  // the strips share one track height; the scale at the left lines up with their marks
  const float top = ImGui::GetCursorScreenPos().y;
  const float trackTop = top + lineHeight + 6 * scale;
  const float trackHeight = std::max(40.0f, mixerHeight() - lineHeight - 30 * scale);

  ImGui::BeginChild("scale", ImVec2(kScaleWidth * scale, 0), false, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
  drawScale(trackTop, trackHeight, scale);
  ImGui::EndChild();

  const Project& project = doc.project();
  for (const Lane& lane : project.lanes) {
    const Instrument* instrument = project.findInstrument(lane.instrument);
    if (!instrument || lane.instrument == kMaster) continue;
    const GraphNode* out = outNode(*instrument);
    ImGui::SameLine(0, 0);
    ImGui::PushID(lane.id.c_str());
    ImGui::BeginChild("strip", ImVec2(kStripWidth * scale, 0), false, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool selected = lane.id == selectedLane;
    if (selected) draw->AddLine(ImVec2(origin.x - 4 * scale, origin.y - 2 * scale), ImVec2(origin.x + kStripWidth * scale, origin.y - 2 * scale), kAccent, 2);
    draw->PushClipRect(origin, ImVec2(origin.x + (kStripWidth - 6) * scale, origin.y + lineHeight + 2 * scale), true);
    draw->AddText(origin, selected ? kAccent : kDimText, lane.id.c_str());
    draw->PopClipRect();
    if (!out) {
      draw->AddText(ImVec2(origin.x, origin.y + lineHeight + 6 * scale), kDimText, "no out");
    } else {
      const std::string base = lane.instrument + "." + out->id + ".";
      const double gain = current(engine, base + "gain", out->param("gain", 0.8));
      // the fader: a well, the gain's mark, ticks on the right
      const ImVec2 track(origin.x + 4 * scale, trackTop);
      const float trackWidth = 26 * scale;
      ImGui::SetCursorScreenPos(track);
      ImGui::InvisibleButton("##fader", ImVec2(trackWidth, trackHeight));
      if (ImGui::IsItemActive()) {
        const double p = std::clamp((track.y + trackHeight - ImGui::GetIO().MousePos.y) / trackHeight, 0.0f, 1.0f);
        const double wanted = std::round(gainAtPos(p) * 100) / 100;
        if (wanted != gain) cx.setParam(lane.instrument, out->id, "gain", wanted, true);
      } else if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && gain != kGainDefault) {
        cx.setParam(lane.instrument, out->id, "gain", kGainDefault, false);
      }
      const double shown = current(engine, base + "gain", gain);
      draw->AddRectFilled(ImVec2(track.x + 4 * scale, track.y), ImVec2(track.x + trackWidth - 4 * scale, track.y + trackHeight), IM_COL32(120, 134, 146, 26));
      for (const int db : kMajorDb) {
        const float y = track.y + trackHeight * static_cast<float>(1 - posOfDb(db));
        draw->AddLine(ImVec2(track.x + trackWidth - 5 * scale, y), ImVec2(track.x + trackWidth, y), IM_COL32(205, 214, 222, db == 0 ? 140 : 100));
      }
      for (const int db : kMinorDb) {
        const float y = track.y + trackHeight * static_cast<float>(1 - posOfDb(db));
        draw->AddLine(ImVec2(track.x + trackWidth - 3 * scale, y), ImVec2(track.x + trackWidth, y), IM_COL32(205, 214, 222, 90));
      }
      // the instrument's level, drawn in the well: green, then amber, then red, uncovered from the bottom; grey when it is not in the mix
      {
        const bool inMix = current(engine, base + "master", 1) >= 0.5;
        float& held = state.held[lane.instrument];
        held = std::max(engine.takeLevel(lane.instrument), held * 0.82f);
        const float h = trackHeight * static_cast<float>(posOfGain(held));
        const float x0 = track.x + 4 * scale, x1 = track.x + trackWidth - 4 * scale, bottom = track.y + trackHeight;
        const float amber = bottom - trackHeight * static_cast<float>(posOfDb(-3)), red = bottom - trackHeight * static_cast<float>(posOfDb(0));
        const float topY = bottom - h;
        const auto band = [&](float from, float to, ImU32 colour) {   // from and to are y values, from is the lower edge
          const float a = std::max(topY, to);
          if (from > a) draw->AddRectFilled(ImVec2(x0, a), ImVec2(x1, from), colour);
        };
        band(bottom, amber, inMix ? IM_COL32(64, 190, 120, 230) : IM_COL32(120, 134, 146, 110));
        band(amber, red, inMix ? IM_COL32(226, 190, 74, 230) : IM_COL32(120, 134, 146, 110));
        band(red, track.y, inMix ? IM_COL32(226, 90, 74, 230) : IM_COL32(120, 134, 146, 110));
        if (held >= 1.0f && inMix) draw->AddCircleFilled(ImVec2(track.x + trackWidth * 0.5f, track.y - 4 * scale), 2.5f * scale, IM_COL32(226, 90, 74, 255));
      }
      const float capY = track.y + trackHeight * static_cast<float>(1 - posOfGain(shown));
      draw->AddRectFilled(ImVec2(track.x + scale, capY - scale), ImVec2(track.x + trackWidth - scale, capY + scale), ImGui::IsItemActive() ? kWarm : kAccent);

      // the switches down the right of the track
      const float side = track.x + trackWidth + 5 * scale;
      const float sideWidth = (kStripWidth - 4 - 26 - 5 - 6) * scale;
      ImGui::SetCursorScreenPos(ImVec2(side, track.y));
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "%s", dbText(shown).c_str());
      ImGui::SetCursorScreenPos(ImVec2(side, track.y + lineHeight + 3 * scale));
      if (button("##mute", "M", lane.silenced, sideWidth, scale)) { doc.apply(std::make_unique<SetSilenced>(lane.id, !lane.silenced)); cx.changed = true; }
      ImGui::SetCursorScreenPos(ImVec2(side, track.y + 2 * (lineHeight + 5 * scale)));
      const bool mixOn = current(engine, base + "master", 1) >= 0.5;
      if (button("##mix", "mix", mixOn, sideWidth, scale)) cx.setParam(lane.instrument, out->id, "master", mixOn ? 0 : 1, false);
      ImGui::SetCursorScreenPos(ImVec2(side, track.y + 3 * (lineHeight + 5 * scale)));
      const bool leadOn = current(engine, base + "lead", 0) >= 0.5;
      if (button("##lead", "lead", leadOn, sideWidth, scale)) cx.setParam(lane.instrument, out->id, "lead", leadOn ? 0 : 1, false);
    }
    draw->AddLine(ImVec2(origin.x + kStripWidth * scale - 1, origin.y - 2 * scale), ImVec2(origin.x + kStripWidth * scale - 1, origin.y + mixerHeight() - 12 * scale), kRule);
    ImGui::EndChild();
    ImGui::PopID();
  }
  ImGui::EndChild();
  return cx.changed;
}

}  // namespace wax::app

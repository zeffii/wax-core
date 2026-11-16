#include "app/chord_window.h"

#include <algorithm>
#include <cstdio>

#include "app/theme.h"
#include "app/widgets.h"
#include "seq/chordname.h"

namespace wax::app {
namespace {

const GraphNode* findNode(const Project& project, const std::string& instrumentId, const std::string& nodeId) {
  for (const Instrument& instrument : project.instruments) {
    if (instrument.id != instrumentId) continue;
    for (const GraphNode& node : instrument.graph.nodes) if (node.id == nodeId) return &node;
  }
  return nullptr;
}

std::string hex2(int i) { char t[4]; std::snprintf(t, sizeof t, "%02X", i); return t; }

std::string noteName(int note) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return std::string(names[((note % 12) + 12) % 12]) + std::to_string(note / 12 - 1);
}

std::string slotText(const GraphNode& node, int slot) {
  if (node.data.is_object() && node.data.contains("chords") && node.data["chords"].is_object()) {
    const Json& chords = node.data["chords"];
    const std::string key = hex2(slot);
    if (chords.contains(key) && chords[key].is_string()) return chords[key].get<std::string>();
  }
  return {};
}

}  // namespace

void showChords(ChordWindow& window, const std::string& instrument, const std::string& node) {
  window.open = true; window.instrument = instrument; window.node = node; window.editing.fill(false);
}

bool drawChordWindow(ChordWindow& window, Document& doc, const Engine& engine) {
  if (!window.open) return false;
  const GraphNode* node = findNode(doc.project(), window.instrument, window.node);
  if (!node || node->type != "chord") { window.open = false; return false; }
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::SetNextWindowSize(ImVec2(520 * scale, 480 * scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(240 * scale, 90 * scale), ImGuiCond_FirstUseEver);
  const std::string title = "Chords  " + window.instrument + "." + window.node + "###chords";
  bool changed = false;
  if (ImGui::Begin(title.c_str(), &window.open, ImGuiWindowFlags_NoSavedSettings)) {
    const std::string address = window.instrument + "." + window.node + ".";
    const auto param = [&](const char* name, double def) { return engine.registry().slotOf(address + name) >= 0 ? engine.registry().get(address + name) : node->param(name, def); };
    const int now = static_cast<int>(param("chord", 0)), octave = static_cast<int>(param("octave", 4));

    for (int p = 0; p < 16; p++) {   // the pages: the first hex digit of the slots; lit when it has a chord, marked when the slot in use is on it
      bool used = false;
      for (int k = 0; k < 16 && !used; k++) used = !slotText(*node, p * 16 + k).empty();
      const std::string label = std::string(1, "0123456789ABCDEF"[p]) + "x" + (p == now / 16 ? "*" : "");
      if (textButton(label.c_str(), window.page == p || used)) { window.page = p; window.editing.fill(false); }
      if (p < 15) ImGui::SameLine(0, 8 * scale);
    }
    ImGui::Separator();
    for (int k = 0; k < 16; k++) {
      const int slot = window.page * 16 + k;
      char* buffer = window.text[static_cast<std::size_t>(k)].data();
      if (!window.editing[static_cast<std::size_t>(k)]) std::snprintf(buffer, window.text[0].size(), "%s", slotText(*node, slot).c_str());
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(slot == now ? kAccent : kDimText), "%s", hex2(slot).c_str());
      ImGui::SameLine(0, 10 * scale);
      ImGui::SetNextItemWidth(190 * scale);
      ImGui::PushID(k);
      ImGui::InputText("##box", buffer, window.text[0].size());
      window.editing[static_cast<std::size_t>(k)] = ImGui::IsItemActive();
      if (ImGui::IsItemDeactivatedAfterEdit()) {   // the box was left: the slot takes the text
        std::string typed = buffer;
        while (!typed.empty() && typed.front() == ' ') typed.erase(typed.begin());
        while (!typed.empty() && typed.back() == ' ') typed.pop_back();
        Json chords = node->data.is_object() && node->data.contains("chords") && node->data["chords"].is_object() ? node->data["chords"] : Json::object();
        if (typed.empty()) chords.erase(hex2(slot)); else chords[hex2(slot)] = typed;
        doc.apply(std::make_unique<SetNodeData>(window.instrument, window.node, "chords", chords, "chord " + hex2(slot)));
        changed = true;
      }
      ImGui::PopID();
      ImGui::SameLine(0, 10 * scale);
      const std::string name = buffer;
      if (name.find_first_not_of(' ') == std::string::npos) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "(plays the note as it is)");
      } else if (const auto chord = parseChord(name)) {
        std::string notes;
        for (const int n : chordNotes(*chord, octave)) notes += (notes.empty() ? "" : " ") + noteName(n);
        ImGui::TextUnformatted(notes.c_str());
      } else {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kWarm), "not a chord");
      }
    }
    ImGui::Separator();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "A root and a quality (F# 13sus, Dm7/C, Eb 9sus4, G Major 7th, Db 7#9(no5)), or exact notes after > (>G-4 A-5 C-5).");
  }
  ImGui::End();
  return changed;
}

}  // namespace wax::app

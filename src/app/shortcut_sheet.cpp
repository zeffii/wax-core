#include "app/shortcut_sheet.h"

#include "imgui.h"

#include "app/theme.h"

namespace wax::app {

void drawShortcutSheet(const char* id, const std::vector<Shortcut>& entries, float width, float height) {
  const float scale = ImGui::GetStyle().FontScaleDpi;
  ImGui::BeginChild(id, ImVec2(width, height), false, ImGuiWindowFlags_NoNav);
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kText), "Shortcuts");
  ImGui::Dummy(ImVec2(1, 3 * scale));
  for (const Shortcut& s : entries) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kWarm), "%s", s.keys);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kDimText));
    ImGui::PushTextWrapPos(0);
    ImGui::Indent(10 * scale);
    ImGui::TextUnformatted(s.what);
    ImGui::Unindent(10 * scale);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(1, 3 * scale));
  }
  ImGui::EndChild();
}

const std::vector<Shortcut>& patternShortcuts() {
  static const std::vector<Shortcut> list = {
      {"Enter", "switch editing on or off; the grid takes the keys while it is on"},
      {"Click", "click a cell to put the cursor there; click a row number to play from it"},
      {"Up / Down", "move a row"},
      {"Left / Right", "move a cell; crossing the ends does not wrap"},
      {"PgUp / PgDn", "a bar of rows up or down"},
      {"Shift+arrows", "select cells; Esc drops the selection"},
      {"Ctrl+C, X, V, A", "copy, cut, paste; select the whole lane. Ctrl+Z undoes, Ctrl+Y or Ctrl+Shift+Z redoes"},
      {"Ctrl+B, Ctrl+E, Ctrl+L", "start the loop at this row, end it with this row; Ctrl+L switches it off and on"},
      {"Z-M, Q-P", "play notes (and type them while editing)"},
      {"[ ]", "octave down / up"},
      {"Space", "on a parameter column: write the parameter's live value into the cell and move down (Shift+Space: every parameter column of the row)"},
      {"=", "note off"},
      {"`", "note cut"},
      {"0-9, A-F", "overwrite the character under the cursor (an empty cell fills with zeros)"},
      {"Delete, Backspace", "clear the cell"},
      {"Shift+Delete", "delete the row and pull the rows below up"},
      {"Insert", "push the rows below down from the cursor"},
      {"F5, F8, Space", "play from the start (or the loop); stop; play or stop"},
      {"F9", "follow the song: the grid scrolls with the playing row"},
      {"F1, F2, F3", "Song, Pattern and Instrument screens"},
      {"Ctrl+O, S, Shift+S", "open, save, save as"},
  };
  return list;
}

}  // namespace wax::app

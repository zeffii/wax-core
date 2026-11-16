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
      {"Left / Right", "move a cell; past the last column it goes on into the next lane (All lanes view)"},
      {"PgUp / PgDn", "a bar of rows up or down"},
      {"Shift+arrows", "select cells; Esc drops the selection"},
      {"Esc", "drops the selection; with none, leaves solo-edit if on; then back to the sequence (Song screen)"},
      {"Tab / Esc", "in solo-edit: Tab switches between the pattern alone and the whole song with the rest locked; Esc (with no selection) leaves it"},
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

const std::vector<Shortcut>& songShortcuts() {
  static const std::vector<Shortcut> list = {
      {"Enter", "switch editing on; with it on, Enter opens the pattern under the cursor in the Pattern screen (a new one when none is there)"},
      {"Esc", "switch editing off"},
      {"Click", "click a cell to put the cursor there; click a tick number to play from that row"},
      {"Up / Down", "move a row (Ctrl+wheel sets how many ticks a row is)"},
      {"Left / Right, Tab", "move to the previous or next lane"},
      {"PgUp / PgDn", "sixteen rows up or down"},
      {"Home / End", "the first or last row"},
      {"Space", "place the outlined pattern from the list here and move on to where it ends"},
      {"I", "insert a new empty pattern here, over any that plays (Enter makes one only on empty ground)"},
      {"N", "clone: this placement gets its own copy of the pattern"},
      {"S", "slice the pattern in two at the cursor"},
      {". ,", "move the pattern a row later or earlier (Shift: sixteen rows)"},
      {"+ -", "make the pattern sixteen ticks longer or shorter"},
      {"Shift+arrows", "select a block of lanes and rows (PgUp, PgDn, Home, End too); any other key drops it"},
      {"G", "glue the selected block into one new pattern per lane (asks about lanes that play nothing there)"},
      {"Delete", "remove the pattern here (it stays in the list); on empty ground pull the patterns below up; with a block selected, remove the patterns starting in it"},
      {"Ctrl+C / Ctrl+X", "copy / cut the selected block (without one, the pattern under the cursor)"},
      {"Ctrl+V", "paste at the cursor (or where the selection is); a pattern goes back into its own lane as the same pattern, into another lane with the same columns as a copy"},
      {"Ctrl+D", "copy the block to the rows right below it; again for another copy"},
      {"Ctrl+B, Ctrl+E, Ctrl+L", "start the loop at this row, end it where this row starts; Ctrl+L switches it off and on"},
      {"Ctrl+Enter", "open the pattern under the cursor on its own (solo-edit): nothing else can be edited or pasted into"},
      {"L / Shift+L", "the loop on or off / a loop round the pattern under the cursor (again to remove it)"},
      {"Alt+S", "solo the lane under the cursor (the others let go); again to bring everyone back"},
      {"Alt+M", "silence the lane under the cursor whatever its marks say; again to bring it back"},
      {"W", "set a waypoint: remember the arrangement as it is"},
      {"Shift+W", "go back to the waypoint (one undo step)"},
      {"Insert", "push the patterns from here down sixteen ticks"},
      {"M, O", "put a mute marker here (the lane is silent from the first until the next), or a note-off marker; again takes it away"},
      {"K, Shift+K", "step the pattern's colour forward or back"},
      {"Ctrl+Up / Down", "step through the pattern list"},
      {"H", "(while editing) show or hide the pattern names in the cells (the number stays; the status bar has the name)"},
      {"Ctrl+Z, Ctrl+Y", "undo, redo"},
      {"F5, F8", "play from the start (or the loop); stop"},
      {"F9", "follow the song: the sequence scrolls with the playing row"},
      {"F1, F2, F3", "Song, Pattern and Instrument screens"},
      {"Ctrl+O, S, Shift+S", "open, save, save as"},
  };
  return list;
}

}  // namespace wax::app

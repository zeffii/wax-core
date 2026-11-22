// The Info screen.
#include "wax_app/app_state.h"

namespace wax::tool {

// The Info screen: plain text saved with the project.
void drawInfoScreen(FrameContext& f) {
  [[maybe_unused]] Session* session = f.session;
  [[maybe_unused]] Player& player = f.player;
  [[maybe_unused]] Document& doc = f.doc;
  [[maybe_unused]] CellEditor& editor = f.editor;
  [[maybe_unused]] const Song& song = f.song;
  [[maybe_unused]] std::string& selectedLane = f.selectedLane;
  [[maybe_unused]] std::string& laneInstrument = f.laneInstrument;
  [[maybe_unused]] const bool playing = f.playing;
  [[maybe_unused]] const int tick = f.tick;
  [[maybe_unused]] bool& follow = f.follow;
  [[maybe_unused]] int& playFromRequest = f.playFromRequest;
  [[maybe_unused]] const float scale = f.scale;
  [[maybe_unused]] const Options& options = f.options;
  [[maybe_unused]] std::map<ImGuiKey, int>& held = f.held;
  [[maybe_unused]] int& octave = f.octave;
  [[maybe_unused]] const int perBeat = f.perBeat;
  [[maybe_unused]] const int perBar = f.perBar;
  [[maybe_unused]] const float statusH = f.statusH;
  [[maybe_unused]] const bool mixerShown = f.mixerShown;
  // Song info: plain text saved with the project (notes, credits, greetings). One undo step each time the box is left.
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "Song info: notes, credits, greetings. Plain text, saved with the project.");
  const std::string stored = song.raw.is_object() && song.raw.contains("info") && song.raw["info"].is_string() ? song.raw["info"].get<std::string>() : std::string();
  if (!session->infoFocused) std::snprintf(session->infoText.data(), session->infoText.size(), "%s", stored.c_str());
  ImGui::InputTextMultiline("##info", session->infoText.data(), session->infoText.size(), ImVec2(-1, -1));
  session->infoFocused = ImGui::IsItemActive();
  if (ImGui::IsItemDeactivatedAfterEdit() && std::string(session->infoText.data()) != stored) doc.apply(std::make_unique<SetSongInfo>(session->infoText.data()));
}

}  // namespace wax::tool

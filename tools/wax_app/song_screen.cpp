// The Song screen.
#include "wax_app/app_state.h"

namespace wax::tool {

// The Song screen: the arrangement as lanes of placed patterns, curves over them, and the caret in the sequence.
void drawSongScreen(FrameContext& f) {
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
  if (!selectedLane.empty() && editor.lane() != selectedLane) editor.setLane(selectedLane, player.engine().laneColumns(selectedLane));   // (one cell is always active: the cursor is in the lane shown)
  const float songSheetW = session->showKeys ? 270 * scale : 0;
  ImGui::BeginChild("seqmain", ImVec2(-songSheetW, 0), false, ImGuiWindowFlags_NoNav);
  const app::CurveView::Targets curveTargetsOf = [&](const Lane& lane) { return curveTargets(player.engine().laneColumns(lane.id), lane.instrument, player.engine().registry()); };
  std::map<std::string, app::MinimapLane> minimapLanes;   // what the pictures of the patterns read (only when they are on)
  if (app::settings().minimapOn)
    for (const Lane& lane : doc.project().lanes) minimapLanes[lane.id] = {&session->player->engine().laneColumns(lane.id), &session->rows[lane.id]};
  const app::SequenceClick hit = app::drawSequenceView(doc.project(), selectedLane, playing ? tick : -1, selectedLane.empty() ? -1 : editor.tick(), editor.tick(), follow && playing && !session->edit, session->curveView, curveTargetsOf, session->edit ? app::selectedBlock(doc.project(), session->song, editor.lane(), editor.tick(), app::sequenceRowTicks(doc.project().song)) : app::SequenceBlock(), &minimapLanes, playing ? session->heard.frac : 0.0f);
  if (hit.curve.any) {   // a curve edited: one undo step per gesture, the lane rebuilt in the engine
    doc.apply(std::make_unique<SetCurves>(hit.curve.lane, hit.curve.curves, hit.curve.label), hit.curve.fold);
    const std::vector<std::string> problems = syncLanes(doc, player.engine());
    session->editStatus = problems.empty() ? std::string() : problems.front();
  }
  if (hit.playFrom >= 0) { playFromRequest = hit.playFrom; session->pausedAt = -1; }   // a tick number clicked: play from there
  if (session->song.glueAsk) { ImGui::OpenPopup("glue"); session->song.glueAsk = false; }
  if (ImGui::BeginPopup("glue")) {   // G over several lanes with some empty there
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "Some of these lanes play nothing there.");
    const auto glue = [&](bool fill) {
      const int n = app::glueBlock(doc, player.engine(), session->song.glueBlock, fill);
      if (n) { const std::vector<std::string> problems = syncLanes(doc, player.engine()); (void)problems; }
      session->editStatus = n == 0 ? "Nothing to merge there." : n == 1 ? "Merged into one new pattern." : "Merged into " + std::to_string(n) + " new patterns, one per lane.";
      session->song.selecting = false;
      ImGui::CloseCurrentPopup();
    };
    if (app::textButton("Empty patterns too")) glue(true);
    ImGui::SameLine();
    if (app::textButton("Only where there is something")) glue(false);
    ImGui::SameLine();
    if (app::textButton("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (!hit.lane.empty()) {
    session->song.selecting = false;
    session->handoff.openLane(hit.lane, hit.tick); selectedLane = hit.lane;
    editor.setLane(hit.lane, player.engine().laneColumns(hit.lane));   // (the keys carry on from the click)
    editor.moveTo(hit.tick, 0);
    session->edit = true;   // a click puts the caret there, ready to move about
    if (hit.doubleClicked && enterPatternUnderCaret(session, selectedLane)) { const std::vector<std::string> problems = syncLanes(doc, player.engine()); session->editStatus = problems.empty() ? std::string() : problems.front(); }   // (a double-click is Enter on that pattern)
  }
  if (!hit.dragLane.empty() && (hit.dragLane != editor.lane() || hit.dragTick != editor.tick() || session->song.selecting)) {   // a drag from the click selects a block, as Shift+arrows do
    if (!session->song.selecting) { session->song.selecting = true; session->song.anchorLane = editor.lane(); session->song.anchorTick = editor.tick(); }
    if (hit.dragLane != editor.lane()) editor.setLane(hit.dragLane, player.engine().laneColumns(hit.dragLane));
    editor.moveTo(hit.dragTick, 0);
    selectedLane = hit.dragLane;
    session->edit = true;
  }
  ImGui::EndChild();
  if (session->showKeys) { ImGui::SameLine(0, 8 * scale); app::drawShortcutSheet("songkeys", app::songShortcuts(), 0, 0); }
}

}  // namespace wax::tool

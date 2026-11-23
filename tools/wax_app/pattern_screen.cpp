// The Pattern screen.
#include "wax_app/app_state.h"

namespace wax::tool {

// The Pattern screen: the lanes as tables of rows the caret moves in, and the lane tabs.
void drawPatternScreen(FrameContext& f) {
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
  const float sheetW = session->showKeys ? 270 * scale : 0;
  ImGui::BeginChild("patmain", ImVec2(-sheetW, 0), false, ImGuiWindowFlags_NoNav);
  {   // the toolbar along the top of the window; the grid starts below it
    const ImVec2 areaMin = ImGui::GetWindowPos();
    const float taken = app::drawPatternToolbar(ImGui::GetWindowDrawList(), areaMin, ImVec2(areaMin.x + ImGui::GetWindowWidth(), areaMin.y + ImGui::GetWindowHeight()), scale, follow);
    ImGui::Dummy(ImVec2(0, taken));
  }
  // the grid of the lanes shown (one, picked by its tab, or all of them side by side), the cursor in lane `cursorLane` of them
  const auto drawGrid = [&](const std::vector<const Lane*>& shown, int cursorLane) {
    std::vector<app::PatternLane> grids;
    for (const Lane* shownLane : shown) {
      app::PatternLane grid;
      grid.id = shownLane->id;
      grid.rows = &session->rows.of(shownLane->id);
      grid.labels = &session->player->engine().columnLabels(shownLane->id);
      if (app::settings().tintPatterns) {   // each pattern's rows in the pattern's colour, faintly
        grid.tint.assign(static_cast<std::size_t>(song.length), 0);
        for (const Placement& placement : shownLane->placements)
          for (const Pattern& pat : shownLane->patterns) {
            if (pat.id != placement.pattern) continue;
            const ImU32 colour = app::patternColour(pat, 34);
            const int end = std::min({song.length, placement.start + placement.length, placement.start + static_cast<int>(pat.rows.size())});
            for (int t = std::max(0, placement.start); t < end; t++) grid.tint[static_cast<std::size_t>(t)] = colour;
          }
      }
      {   // where each pattern starts and ends; on the cursor's lane the patterns other than the one under the cursor are dimmed
        grid.marks.assign(static_cast<std::size_t>(song.length), 0);
        grid.edge.assign(static_cast<std::size_t>(song.length), 0);
        const Placement* under = shownLane->id == editor.lane() ? seq::placementAt(*shownLane, editor.tick()) : nullptr;
        for (int t = 0; t < song.length; t++) {
          const Placement* owner = seq::placementAt(*shownLane, t);
          if (!owner) continue;
          const Pattern* pat = seq::patternOf(*shownLane, owner->pattern);
          unsigned char& mark = grid.marks[static_cast<std::size_t>(t)];
          if (t == owner->start) { mark |= app::PatternLane::kStart; if (pat) grid.edge[static_cast<std::size_t>(t)] = app::patternColour(*pat, 95); }
          if (under && owner != under) mark |= app::PatternLane::kDim;
        }
        if (session->solo.on && app::settings().soloHatch)   // solo-edit with the song shown: everything but the pattern is out of reach
          for (int t = 0; t < song.length; t++) if (shownLane->id != session->solo.lane || t < session->solo.lo || t >= session->solo.hi) grid.marks[static_cast<std::size_t>(t)] |= app::PatternLane::kLocked;
      }
      grids.push_back(std::move(grid));
    }
    const Lane& cursorOwner = *shown[static_cast<std::size_t>(cursorLane)];
    app::CursorView cursor;
    cursor.lane = cursorLane;
    cursor.show = session->edit;
    cursor.hideEmpty = app::settings().hideEmpty;
    if (session->solo.on && !app::settings().soloHatch) { cursor.rowFirst = session->solo.lo; cursor.rowEnd = session->solo.hi; }   // the pattern on its own
    cursor.tick = editor.tick();
    cursor.cell = editor.cell();
    cursor.character = editor.character();
    const app::Handoff::Scroll scroll = session->handoff.takeScroll();
    cursor.scrollTo = scroll.toCursor; cursor.revealFrom = scroll.revealFrom; cursor.revealTo = scroll.revealTo;
    const CellEditor::Selection sel = editor.selection();
    { const LoopRange lr = loopOf(song); cursor.loop = song.raw.is_object() && song.raw.contains("loop") && song.raw["loop"].is_object(); cursor.loopOn = lr.on; cursor.loopStart = lr.start; cursor.loopEnd = lr.end; }
    cursor.playFrac = playing ? session->heard.frac : 0.0f;
    cursor.info = app::describeColumn(doc.project(), cursorOwner, session->player->engine().laneColumns(cursorOwner.id), session->rows.of(cursorOwner.id), editor.tick(), editor.cell());
    cursor.selected = sel.active; cursor.tick0 = sel.tick0; cursor.tick1 = sel.tick1; cursor.cell0 = sel.cell0; cursor.cell1 = sel.cell1;
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * app::settings().patternText);   // Text size
    const app::PatternViewResult result = app::drawPatternView(grids, song.ticksPerBeat, song.beatsPerBar, playing ? tick : -1, follow && playing && !session->edit, cursor);
    ImGui::PopFont();
    if (result.clickedTick >= 0) playFromRequest = result.clickedTick;
    if (result.pressed && follow && playing) { follow = false; session->editStatus = "Follow off while you edit: F9 follows again."; }   // (at the press, so the rows stop moving before the click lands)
    if (result.cell >= 0 && result.lane >= 0) {
      const Lane& clicked = *shown[static_cast<std::size_t>(result.lane)];
      if (session->solo.on && clicked.id != editor.lane()) return;   // (solo-edit keeps to its lane)
      if (clicked.id != editor.lane()) { selectedLane = clicked.id; editor.setLane(clicked.id, session->player->engine().laneColumns(clicked.id)); }
      editor.moveTo(result.cellTick, result.cell);
      session->edit = true;   // a click puts the caret there, ready to type
    }
    if (result.dragCell >= 0 && result.dragLane >= 0 && !(result.dragTick == editor.tick() && result.dragCell == editor.cell() && !editor.selection().active) &&
        shown[static_cast<std::size_t>(result.dragLane)]->id == editor.lane()) {   // a drag from a click selects a rectangle (within the lane the click was in)
      editor.selectTo(result.dragTick, result.dragCell);
      session->edit = true;
    }
    if (result.menuCell >= 0 && result.menuLane >= 0) {   // right-click: the caret goes to the cell (a selection the click is inside stays), and a menu offers the usual things
      const Lane& clicked = *shown[static_cast<std::size_t>(result.menuLane)];
      if (!(session->solo.on && clicked.id != editor.lane())) {
        const CellEditor::Selection block = editor.selection();
        const bool inside = block.active && clicked.id == editor.lane() && result.menuTick >= std::min(block.tick0, block.tick1) && result.menuTick <= std::max(block.tick0, block.tick1) &&
                            result.menuCell >= std::min(block.cell0, block.cell1) && result.menuCell <= std::max(block.cell0, block.cell1);
        if (!inside) {
          if (clicked.id != editor.lane()) { selectedLane = clicked.id; editor.setLane(clicked.id, session->player->engine().laneColumns(clicked.id)); }
          editor.moveTo(result.menuTick, result.menuCell);
        }
        session->edit = true;
        ImGui::OpenPopup("patternmenu");
      }
    }
    drawPatternMenu(session, doc, player);
  };
  if (app::settings().patternAll) {
    std::vector<const Lane*> shown;
    int cursorLane = 0;
    for (const Lane& lane : doc.project().lanes) {
      if (session->solo.on && !app::settings().soloHatch && lane.id != session->solo.lane) continue;   // (the pattern on its own: one lane)
      if (lane.id == selectedLane) cursorLane = static_cast<int>(shown.size());
      shown.push_back(&lane);
    }
    if (!shown.empty()) {
      if (session->handoff.laneRequested()) {   // a lane asked for (a click in the Song screen)
        for (std::size_t i = 0; i < shown.size(); i++) if (session->handoff.isRequested(shown[i]->id)) cursorLane = static_cast<int>(i);
      }
      const Lane& owner = *shown[static_cast<std::size_t>(cursorLane)];
      selectedLane = owner.id;
      laneInstrument = owner.instrument;
      editor.setLane(owner.id, session->player->engine().laneColumns(owner.id));
      if (session->handoff.laneRequested()) {
        if (session->handoff.requestedTick() >= 0) { editor.moveTo(session->handoff.requestedTick(), editor.cell()); session->handoff.scrollToCursor(); }
        session->handoff.landed();
      }
      drawGrid(shown, cursorLane);
    }
  } else if (ImGui::BeginTabBar("lanes")) {
    for (const Lane& lane : doc.project().lanes) {
      if (session->solo.on && lane.id != session->solo.lane) continue;   // (solo-edit: its lane only)
      const std::string label = lane.id + (lane.instrument == lane.id ? "" : "  " + lane.instrument);
      ImGuiTabItemFlags tabFlags = ImGuiTabItemFlags_None;
      if (!session->pickedTab && lane.id != kMaster) { tabFlags |= ImGuiTabItemFlags_SetSelected; session->pickedTab = true; }
      if (session->handoff.isRequested(lane.id)) tabFlags |= ImGuiTabItemFlags_SetSelected;
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);   // lane names in a small face
      const bool tabOpen = ImGui::BeginTabItem(label.c_str(), nullptr, tabFlags);
      ImGui::PopFont();
      if (tabOpen) {
        selectedLane = lane.id;
        laneInstrument = lane.instrument;
        editor.setLane(lane.id, session->player->engine().laneColumns(lane.id));
        if (session->handoff.isRequested(lane.id)) {
          if (session->handoff.requestedTick() >= 0) { editor.moveTo(session->handoff.requestedTick(), editor.cell()); session->handoff.scrollToCursor(); }
          session->handoff.landed();
        }
        drawGrid({&lane}, 0);
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }
  ImGui::EndChild();
  if (session->showKeys) { ImGui::SameLine(0, 8 * scale); app::drawShortcutSheet("patkeys", app::patternShortcuts(), 0, 0); }
}

}  // namespace wax::tool

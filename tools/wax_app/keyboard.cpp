// The computer keyboard.
#include "wax_app/app_state.h"

namespace wax::tool {

KeyFocus keyFocusOf(const Session& session) {
  KeyFocus focus;
  focus.sequenceCaret = session.edit && session.screen == Screen::Song;
  focus.gridCaret = session.edit && session.screen == Screen::Pattern;
  focus.gridEditing = focus.gridCaret && !session.envelopes.wantsKeys();
  focus.pianoKeys = session.screen != Screen::Sample && !focus.sequenceCaret;
  return focus;
}

// Enter on the Song screen (and a double-click on a pattern there): into the pattern under the caret, a new one where none is. The Pattern
// screen then shows that pattern whole. Returns true when a pattern was made (the song changed).
bool enterPatternUnderCaret(Session* session, std::string& selectedLane) {
  Document& doc = *session->doc;
  CellEditor& editor = *session->editor;
  bool edited = false;
  if (!editor.patternHere()) edited |= editor.createPatternHere();
  if (!editor.patternHere()) { session->editStatus = "No pattern here, and one cannot start here."; return edited; }
  selectedLane = editor.lane();
  session->screen = Screen::Pattern;
  session->handoff.forgetReveal();
  for (const Lane& lane : doc.project().lanes) {   // the pattern under the caret: its rows are what the Pattern screen shows
    if (lane.id != editor.lane()) continue;
    if (const Placement* p = seq::placementAt(lane, editor.tick())) {
      const Pattern* pat = seq::patternOf(lane, p->pattern);
      session->handoff.revealRows(p->start, p->start + std::min(p->length, pat ? static_cast<int>(pat->rows.size()) : p->length));
    }
  }
  session->handoff.openLane(selectedLane, editor.tick());
  return edited;
}

// Starts solo-edit on the pattern under the caret (its rows alone can be changed). False where no pattern plays at the caret.
static bool beginSoloUnderCaret(Session* session, int songLength) {
  const Project& project = session->doc->project();
  CellEditor& editor = *session->editor;
  const Lane* lane = nullptr;
  for (const Lane& l : project.lanes) if (l.id == editor.lane()) lane = &l;
  const Placement* at = lane ? seq::placementAt(*lane, editor.tick()) : nullptr;
  const Pattern* pattern = at ? seq::patternOf(*lane, at->pattern) : nullptr;
  if (!at || !pattern) return false;
  const int lo = at->start, hi = std::min(songLength, at->start + std::min(at->length, static_cast<int>(pattern->rows.size())));
  session->solo.begin(editor, lane->id, lo, hi);
  return true;
}

// The least time a note played from the keys sounds on the machine they play (Instrument::previewMs; 0: it stops with the key).
static int previewMsOf(const Session& session) {
  std::string machine = session.screen == Screen::Instrument ? session.instrument : std::string();
  if (machine.empty()) for (const Lane& lane : session.doc->project().lanes) if (lane.id == session.editor->lane()) machine = lane.instrument;
  for (const Instrument& instrument : session.doc->project().instruments) if (instrument.id == machine) return instrument.previewMs;
  return 0;
}

// The computer keyboard: the transport keys, Enter and the caret, the editing keys of the Song and Pattern screens, and the piano keys.
// Returns true when the song was changed (the engine is brought up to date afterwards).
bool handleKeyboard(Session* session, KeyboardEnv& env) {
  Player& player = *session->player;
  Document& doc = *session->doc;
  CellEditor& editor = *session->editor;
  const Song& song = doc.project().song;
  int& octave = env.octave;
  bool& follow = env.follow;
  int& playFromRequest = env.playFromRequest;
  std::string& selectedLane = env.selectedLane;
  std::map<ImGuiKey, int>& held = env.held;
  bool& requestSave = env.requestSave;
  const bool playing = env.playing;
  // The computer keyboard plays notes unless a text box has it. With Enter pressed it also types into the cell under the cursor.
  ImGuiIO& io = ImGui::GetIO();
  if (session->screen == Screen::Song || session->screen == Screen::Pattern) session->edit = true;   // the caret is always there to move, on both editors (Esc only clears a selection or goes back)
  editor.setStep(app::settings().editStep);   // (rows down after a value is entered)
  bool edited = false;
  const auto focusNow = [&] { return keyFocusOf(*session); };   // (asked at each key, as Enter and the arrow keys change it within the frame)
  const bool ctrl = io.KeyCtrl;
  const bool menuOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);   // a menu has the keys while it is open
  if (!io.WantTextInput && !menuOpen) {
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) octave = std::max(0, octave - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) octave = std::min(9, octave + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Pause, false)) {   // pause, or carry on from the pause
      if (player.playing()) { session->pausedAt = player.tick(); player.ring().push({LiveCommand::Stop, 0, 1}); }
      else if (session->pausedAt >= 0) { player.ring().push({LiveCommand::Play, session->pausedAt, 1}); session->pausedAt = -1; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) { player.ring().push({LiveCommand::Play, std::max(0, session->pausedAt), 1}); session->pausedAt = -1; }
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) { playFromRequest = editor.tick(); session->pausedAt = -1; }   // from the cursor
    if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) player.ring().push({LiveCommand::Row, editor.tick(), 1});   // the row under the cursor, once, all lanes together
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) { player.ring().push({LiveCommand::Stop, 0, 1}); session->pausedAt = -1; }
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) follow = !follow;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && session->screen != Screen::Sample && session->screen != Screen::Song && !focusNow().gridCaret) {   // (on the Song screen Space places a pattern, editing or not, as the web; editing the Pattern screen it writes the live value)
      if (playing) player.ring().push({LiveCommand::Stop, 0, 1});
      else player.ring().push({LiveCommand::Play, 0, 1});
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      if (ctrl && !io.KeyShift && !io.KeyAlt) {   // Ctrl+Enter in the sequence: the pattern under the cursor on its own (solo-edit); where none is: "No pattern here."
        if (session->screen == Screen::Song) {
          if (!beginSoloUnderCaret(session, song.length)) session->editStatus = "No pattern here.";
          else {
            selectedLane = editor.lane(); session->handoff.openLane(selectedLane, std::clamp(editor.tick(), session->solo.lo, session->solo.hi - 1));
            session->screen = Screen::Pattern;
            session->edit = true;
            session->editStatus = "Solo-edit: only this pattern can be changed. Tab: whole song or just the pattern. Esc leaves.";
          }
        }
      } else if (io.KeyAlt && !ctrl && !io.KeyShift && session->screen == Screen::Pattern) {   // Alt+Enter on the Pattern screen: solo-edit the pattern under the caret, or leave it (as the web)
        if (session->solo.on) { session->solo.end(editor); session->editStatus = "Solo-edit ended."; }
        else if (beginSoloUnderCaret(session, song.length)) session->editStatus = "Solo-edit: only this pattern can be changed. Tab: whole song or just the pattern. Esc leaves.";
        else session->editStatus = "No pattern here to solo-edit.";
      } else if (focusNow().sequenceCaret) {   // in the sequence, Enter goes into the pattern under the cursor (a new one where none is)
        edited |= enterPatternUnderCaret(session, selectedLane);
      } else if (session->screen == Screen::Pattern && !session->solo.on && !editor.patternHere()) {   // Enter on the Pattern screen where no pattern plays: a new one (as the web), at the caret's block of 16 rows or the nearest free one
        edited |= editor.createPatternHere();
        session->edit = true;
        session->editStatus = editor.patternHere() ? "New pattern here." : "No pattern here, and one cannot start here.";
      } else if (session->screen == Screen::Pattern) {   // Enter where a pattern already is does nothing (as the web)
        if (!session->solo.on) session->editStatus = "A pattern is already here.";
      } else session->edit = !session->edit;
    }
    if (session->solo.on && session->screen != Screen::Pattern) { session->solo.end(editor); }   // (leaving the Pattern screen ends solo-edit)
    app::SongKeysEnv songEnv{doc, editor, player.engine(), selectedLane, session->editStatus, session->handoff, edited, session->edit};
    if (session->screen == Screen::Song && !session->edit && !io.WantTextInput && !ctrl && !io.KeyAlt &&   // an arrow key in the sequence puts the caret in play, and moves it
        (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow))) session->edit = true;
    if (focusNow().sequenceCaret) app::songCursorKeys(session->song, songEnv);   // the Song screen cursor, selection and edits
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) { if (io.KeyShift) doc.redo(); else doc.undo(); edited = true; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) { doc.redo(); edited = true; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) gRequest = DialogRequest::Open;
    if (ctrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false)) gRequest = DialogRequest::SaveAs;
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) requestSave = true;
    if (requestSave) {
      requestSave = false;
      if (session->path.empty()) gRequest = DialogRequest::SaveAs;   // a new song has no file yet
      else try {
        saveProject(doc.project(), session->path);
        doc.markSaved();
        app::rememberRecent(session->path);
        session->editStatus = "saved " + session->path;
      } catch (const std::exception& e) {
        session->editStatus = e.what();
      }
    }
    if (session->screen == Screen::Song || session->screen == Screen::Pattern) app::laneSoloKeys(songEnv);   // Alt+S solo, Alt+M silence the cursor lane
    if (io.KeyAlt && !ctrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_B, false)) session->showBrowser = !session->showBrowser;   // the browser
    if (io.KeyShift && !ctrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_I, false)) session->wantTree = true;   // the machine tree
    if (ctrl && !io.KeyShift && !io.KeyAlt && (session->screen == Screen::Pattern || session->screen == Screen::Song)) {   // the song loop: start and end at the cursor row, switch
      const auto setPoint = [&](const char* which, int at) {
        std::string message;
        Json loop = loopWithPoint(song, which, at, message);
        doc.apply(std::make_unique<SetLoop>(std::move(loop), message));
        syncLoop(doc, player.engine());
        session->editStatus = message;
      };
      const int rowTicks = session->screen == Screen::Song ? app::sequenceRowTicks(song) : 1;   // (on the Song screen a row is a stretch of ticks: the loop starts where it starts, ends where it starts)
      const int rowStart = editor.tick() / rowTicks * rowTicks;
      if (ImGui::IsKeyPressed(ImGuiKey_B, false)) setPoint("start", rowStart);
      if (ImGui::IsKeyPressed(ImGuiKey_E, false)) setPoint("end", session->screen == Screen::Song ? rowStart : editor.tick() + 1);
      if (ImGui::IsKeyPressed(ImGuiKey_L, false)) {
        Json loop = loopToggled(song);
        if (loop.is_null()) session->editStatus = "No loop to switch: Ctrl+B and Ctrl+E set one.";
        else {
          const bool off = loop.value("off", false);
          doc.apply(std::make_unique<SetLoop>(std::move(loop), off ? "Loop off" : "Loop on"));
          syncLoop(doc, player.engine());
          session->editStatus = off ? "Loop off (its points are kept)." : "Loop on.";
        }
      }
    }
    if (focusNow().sequenceCaret) app::songBlockKeys(session->song, songEnv);   // Ctrl+C, X, V, D on blocks of the sequence
    if (ctrl && !io.KeyAlt && session->screen == Screen::Pattern && ImGui::IsKeyPressed(ImGuiKey_K, false)) {   // Ctrl+K: the next colour for the pattern under the cursor; Ctrl+Shift+K the previous one (as the web)
      const Lane* current = nullptr;
      for (const Lane& l : doc.project().lanes) if (l.id == selectedLane) current = &l;
      Lane copy = current ? *current : Lane{};
      if (current && seq::colourPattern(copy, session->solo.on ? session->solo.lo : editor.tick(), io.KeyShift ? -1 : 1)) {
        doc.apply(std::make_unique<SetLaneSequence>(copy, "change the colour"));
        edited = true;
        session->editStatus = "Pattern colour changed.";
      } else session->editStatus = "No pattern here.";
    }
    if (ctrl && !io.KeyAlt && !io.KeyShift && session->screen == Screen::Pattern && ImGui::IsKeyPressed(ImGuiKey_G, false)) {   // Ctrl+G: this placement gets its own copy of its pattern, so editing it no longer changes the others (as the web)
      const Lane* current = nullptr;
      for (const Lane& l : doc.project().lanes) if (l.id == selectedLane) current = &l;
      Lane copy = current ? *current : Lane{};
      if (current && seq::clonePlacement(copy, session->solo.on ? session->solo.lo : editor.tick())) {
        doc.apply(std::make_unique<SetLaneSequence>(copy, "clone pattern"));
        edited = true;
        editor.clearSelection();
        session->editStatus = "Cloned: this placement now has its own copy.";
      } else session->editStatus = "No pattern here.";
    }
    bool gluedHere = false;   // (G was this command: it is not also the piano key)
    if (!ctrl && !io.KeyAlt && !io.KeyShift && session->screen == Screen::Pattern && focusNow().gridEditing && !session->solo.on && !editor.patternHere() && ImGui::IsKeyPressed(ImGuiKey_G, false)) {
      // G where no pattern plays (as in the sequence): the selected rows become a new pattern there; with nothing selected, a new pattern of the usual length as Enter makes one
      gluedHere = true;
      const CellEditor::Selection range = editor.selection();
      if (!range.active) edited |= editor.createPatternHere();
      else {
        app::SequenceBlock block;
        for (std::size_t i = 0; i < doc.project().lanes.size(); i++) if (doc.project().lanes[i].id == editor.lane()) block.first = block.last = static_cast<int>(i);
        block.from = range.tick0; block.to = range.tick1 + 1;
        if (block.first >= 0 && app::glueBlock(doc, player.engine(), block, true) > 0) { edited = true; editor.clearSelection(); }
      }
      session->editStatus = editor.patternHere() ? "New pattern here." : "No pattern here, and one cannot start here.";
    }
    if (ctrl && focusNow().gridCaret && !io.KeyAlt) {   // block editing: copy, cut, paste, select all
      if (ImGui::IsKeyPressed(ImGuiKey_C, false)) ImGui::SetClipboardText(editor.copy().c_str());
      if (ImGui::IsKeyPressed(ImGuiKey_X, false)) { ImGui::SetClipboardText(editor.copy().c_str()); edited |= editor.cut(); }
      if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        const char* text = ImGui::GetClipboardText();
        edited |= (text && *text) ? editor.paste(text) : editor.paste();
      }
      if (ImGui::IsKeyPressed(ImGuiKey_A, false)) editor.selectAll();
    }
    const bool typing = focusNow().gridEditing && !ctrl && !io.KeyAlt;   // (only the pattern editor types into the grid: on the Instrument, Info and Config screens the keys only play; the sequence has its own keys, above)
    if (session->screen == Screen::Pattern && !ctrl && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !editor.selection().active && !session->envelopes.wantsKeys()) {   // Esc with nothing selected: back to the sequence (as the web)
      if (session->solo.on) { session->solo.end(editor); session->editStatus = "Solo-edit ended."; }
      else { session->screen = Screen::Song; session->handoff.scrollToCursor(); }
    }
    const bool editing = focusNow().gridEditing;   // (typing, and the Ctrl keys that edit)
      editor.setWrapRows(app::settings().rowWrap);
      const auto step = [&](int rows, int cells) {
        if (io.KeyShift) { editor.extend(rows, cells); return; }
        const int cellBefore = editor.cell();
        editor.move(rows, cells);
        if (!cells || editor.cell() != cellBefore || session->screen != Screen::Pattern || !app::settings().patternAll) return;
        // Left or Right at the first or last column of a lane: on into the next lane (when all lanes are shown), the cursor at the same row
        std::vector<std::string> order;
        for (const Lane& l : doc.project().lanes) if (l.id != kMaster) order.push_back(l.id);
        const auto at = std::find(order.begin(), order.end(), editor.lane());
        if (at == order.end() || order.size() < 2) return;
        std::ptrdiff_t next = (at - order.begin()) + cells;
        if (next < 0 || next >= static_cast<std::ptrdiff_t>(order.size())) {
          if (!app::settings().laneWrap) return;   // the caret stops at the first and last column
          next = (next + static_cast<std::ptrdiff_t>(order.size())) % static_cast<std::ptrdiff_t>(order.size());
        }
        const std::string lane = order[static_cast<std::size_t>(next)];
        const int rowTick = editor.tick();
        selectedLane = lane;
        editor.setLane(lane, player.engine().laneColumns(lane));
        const int columns = static_cast<int>(player.engine().laneColumns(lane).size());
        editor.moveTo(rowTick, cells > 0 ? 0 : std::max(0, columns - 1));
        if (cells < 0) editor.lastCharacter();
        if (cells > 0) editor.move(0, 1), editor.move(0, -1);   // (the first cell that is not a spacer)
        else editor.move(0, -1), editor.move(0, 1);              // (the last one)
        session->handoff.openLane(lane);
      };
    if (editing && ctrl) {   // the Ctrl keys of the pattern editor (the web's): by column, transposing, pulling rows up, pushing them down, rotating
      const int before = editor.tick() * 4096 + editor.cell();
      if (!io.KeyAlt && !io.KeyShift) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step(0, -1);    // Ctrl+Left/Right: the first character of the next or previous column
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step(0, 1);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) edited |= editor.shiftSelection(CellEditor::Shift::PullUp);   // the selection goes, the rows below move up
        if (ImGui::IsKeyPressed(ImGuiKey_Insert)) edited |= editor.shiftSelection(CellEditor::Shift::PushDown);   // the rows from the selection down move down
      }
      if (!io.KeyAlt) {   // (Shift: an octave for notes, a digit for values)
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) edited |= editor.transpose(true, 1, io.KeyShift);       // Ctrl+Up/Down: the notes under the cursor or in the selection by one
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) edited |= editor.transpose(true, -1, io.KeyShift);
        if (ImGui::IsKeyPressed(ImGuiKey_Period)) edited |= editor.transpose(false, 1, io.KeyShift);
        if (ImGui::IsKeyPressed(ImGuiKey_Comma)) edited |= editor.transpose(false, -1, io.KeyShift);
      } else if (editor.selection().active) {   // Ctrl+Alt+Up/Down: the selected rows rotate
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) edited |= editor.shiftSelection(CellEditor::Shift::RotateUp);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) edited |= editor.shiftSelection(CellEditor::Shift::RotateDown);
      } else {   // Ctrl+Alt+Up/Down with nothing selected: one more or one fewer Note + Vol pair for the machine of this lane (as the web)
        const int direction = ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? -1 : 0;
        if (direction != 0) session->editStatus = changeNoteColumns(session, doc, player, laneMachine(doc, editor.lane()), direction);
      }
      if (before != editor.tick() * 4096 + editor.cell() || edited) session->handoff.scrollToCursor();
    }
    if (typing) {
      const int before = editor.tick() * 4096 + editor.cell();
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) step(-1, 0);
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) step(1, 0);
      // Left and Right step one character, on into the next cell and column at the ends (and the next lane at the lane's ends); Shift extends a selection by cells.
      const auto sideways = [&](int d) {
        if (!io.KeyShift && editor.moveChar(d)) return;
        step(0, d);
      };
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) sideways(-1);
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) sideways(1);
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {   // Esc: drops a selection; then leaves solo-edit; then goes back to the sequence, as in the web
        if (editor.selection().active) editor.clearSelection();
        else if (session->solo.on) { session->solo.end(editor); session->editStatus = "Solo-edit ended."; }   // (the web: solo first, then back)
        else if (session->screen == Screen::Pattern) { session->screen = Screen::Song; session->handoff.scrollToCursor(); }
      }
      if (session->solo.on && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {   // Tab: the pattern on its own, or the whole song with the rest locked
        app::settings().soloHatch = !app::settings().soloHatch;
        app::saveSettings();
        session->handoff.scrollToCursor();
        session->editStatus = app::settings().soloHatch ? "Solo-edit: the song, with the rest locked." : "Solo-edit: the pattern on its own.";
      }
      const int bar = std::max(1, song.ticksPerBeat) * std::max(1, song.beatsPerBar);
      if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) editor.move(-bar, 0);
      if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) editor.move(bar, 0);
      if (ImGui::IsKeyPressed(ImGuiKey_Delete)) edited |= io.KeyShift ? editor.deleteRow() : editor.clearCell(true);
      if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) edited |= editor.clearCell(false);
      if (ImGui::IsKeyPressed(ImGuiKey_Insert)) edited |= editor.insertRow();
      if (session->screen == Screen::Pattern && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {   // the live value of a parameter column (Ctrl+Space: the whole row's) into the grid
        std::string instrumentId;
        for (const Lane& l : doc.project().lanes) if (l.id == editor.lane()) instrumentId = l.instrument;
        const bool whole = io.KeyShift;
        const bool wrote = editor.writeLive([&](const Column& column) -> std::optional<double> {
          const std::string address = instrumentId + "." + column.node + "." + column.param;
          if (player.engine().registry().slotOf(address) < 0) return std::nullopt;
          return player.engine().registry().get(address);
        }, whole);
        edited |= wrote;
        session->editStatus = wrote ? std::string() : whole ? "This lane has no parameter columns with a live value." : "Only parameter columns have a live value; move to one.";
      }
      if (ImGui::IsKeyPressed(ImGuiKey_1, false) && !io.KeyShift && !ctrl && !io.KeyAlt && editor.character() == 0) edited |= editor.typeNoteOff();   // 1: a note off in this cell (as the web; Shift+1 is the whole row, below)
      if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_1, false)) {   // Shift+1 (!): a note off in every note column of this row (as the web)
        if (editor.typeNoteOffRow()) { edited = true; session->editStatus = "Note off in " + std::to_string(editor.noteOffColumns()) + (editor.noteOffColumns() == 1 ? " note column." : " note columns."); }
        else session->editStatus = "This lane has no note columns, or no pattern could be made here.";
      }
      if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) edited |= editor.typeNoteCut();
      if (editor.onHexCell() || editor.character() >= 1) {   // typing changes the character the cursor is on (a note's first character takes whole notes, below)
        // The characters the window was sent; when it was sent none (no text box has the keyboard, so the platform may not send text at all), the keys themselves.
        std::string typed;
        for (int i = 0; i < io.InputQueueCharacters.Size; i++) if (io.InputQueueCharacters[i] < 128) typed += static_cast<char>(io.InputQueueCharacters[i]);
        if (io.InputQueueCharacters.Size == 0) {
          for (int d = 0; d < 10 && !io.KeyShift; d++)
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + d), false) || ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + d), false)) typed += static_cast<char>('0' + d);
          for (int d = 0; d < 6; d++)
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + d), false)) typed += static_cast<char>('A' + d);
          if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false)) typed += '-';
          if ((io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_3, false)) || ImGui::IsKeyPressed(ImGuiKey_Backslash, false)) typed += '#';   // (the # key of a UK keyboard, or Shift+3)
        }
        for (const char c : typed) edited |= editor.typeCharacter(c);
      }
      if (before != editor.tick() * 4096 + editor.cell() || edited) session->handoff.scrollToCursor();
    }
    const bool hexKeys = typing && (editor.onHexCell() || (editor.onNoteCell() && editor.character() >= 1));   // there the keys are characters, not piano keys
    if (!hexKeys && focusNow().pianoKeys) {   // (on the Sample screen the keys play the sample)
      for (int i = 0; i < static_cast<int>(sizeof kPianoKeys / sizeof kPianoKeys[0]); i++) {
        const ImGuiKey key = kPianoKeys[i];
        if (ImGui::IsKeyPressed(key, false) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift && !(gluedHere && key == ImGuiKey_G)) {   // (Shift+letter is a command, e.g. Shift+I: the machine menu)
          const int note = 12 * (octave + 1) + i;
          if (note > 127) { session->editStatus = "Note out of range."; continue; }   // (as the web: nothing is entered)
          const bool entering = typing && editor.onNoteCell();   // the key puts a note in the grid
          held[key] = note;
          session->releaseAt.erase(std::remove_if(session->releaseAt.begin(), session->releaseAt.end(), [&](const std::pair<double, int>& due) { return due.second == note; }), session->releaseAt.end());   // (a note struck again keeps its new life)
          session->heldSince[key] = {ImGui::GetTime(), previewMsOf(*session)};
          if (!entering || app::settings().notePreview) player.ring().push({LiveCommand::NoteOn, note, 0.8});   // (Note preview off: entering is silent)
          if (entering) edited |= editor.typeNote(note);
        }
      }
    }
  }
  return edited;
}

}  // namespace wax::tool

// wax-app [song.wax] [--rate hz] [--threads n] [--midi n] [--null]
//         [--screenshot out.bmp [--frames n] [--play-from tick]]
// The window: the transport, the lanes of the song as pattern tables that follow the playhead, level meters, and
// keyjazz from the computer keyboard or a MIDI controller. Drop a .wax file on the window to open it.
// Without a sound card (or with --null) it still runs; --screenshot draws some frames, saves the last, and quits,
// which is how the window is looked at on a machine without a screen (SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software).
#include "wax_app/app_state.h"

namespace wax::tool {

void SDLCALL dialogDone(void* userdata, const char* const* files, int) {
  if (!files || !files[0]) return;   // cancelled or failed
  std::lock_guard<std::mutex> lock(gAnswer.mutex);
  gAnswer.path = files[0];
  gAnswer.kind = userdata ? *static_cast<DialogKind*>(userdata) : DialogKind::Open;
  gAnswer.ready = true;
}

void drawMeter(const char* id, float level, ImVec2 size) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, size);
  draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), app::kField, 2);
  const float db = level > 1e-5f ? 20.0f * std::log10(level) : -100.0f;
  const float fill = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
  draw->AddRectFilled(origin, ImVec2(origin.x + size.x * fill, origin.y + size.y),
                      db > -0.5f ? IM_COL32(220, 90, 70, 255) : app::kAccent, 2);
}

// Where the automatic backups go: a folder of the user's preferences.
std::filesystem::path backupFolder() {
  char* dir = SDL_GetPrefPath("wax", "wax-app");
  const std::filesystem::path path = dir ? std::filesystem::path(dir) / "backups" : std::filesystem::path();
  if (dir) SDL_free(dir);
  return path;
}

// The chosen theme is kept between runs in the user's preferences folder.
std::string themeFile() {
  char* dir = SDL_GetPrefPath("wax", "wax-app");
  const std::string path = dir ? std::string(dir) + "theme.txt" : std::string();
  if (dir) SDL_free(dir);
  return path;
}

void saveTheme() {
  const std::string path = themeFile();
  if (path.empty()) return;
  std::ofstream(path, std::ios::trunc) << app::themeName(app::themeIndex()) << "\n";
}

void loadTheme() {
  const std::string path = themeFile();
  std::ifstream file(path);
  std::string name;
  if (file && std::getline(file, name)) app::setThemeByName(name.c_str());
}

// The first screen: the Wax logo with a thin loader bar along the bottom filling over a moment, then fading away (as the web app's splash).
// `seconds` is the time since the window opened. Drawn over everything; returns false once it is gone.
bool drawSplash(double seconds, const std::string& build) {
  constexpr double kShow = 0.9, kFade = 0.3;
  if (seconds >= kShow + kFade) return false;
  const double fade = seconds <= kShow ? 1.0 : 1.0 - (seconds - kShow) / kFade;
  const int alpha = static_cast<int>(255 * fade);
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  const ImVec2 min = viewport->WorkPos, size = viewport->WorkSize;
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  draw->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y), app::withAlpha(app::kBg, alpha));
  const float scale = ImGui::GetStyle().FontScaleDpi;
  const float width = std::min(size.x * 0.46f, 520.0f * scale), aspect = 820.0f / 2320.0f;
  app::drawIcon("wax-logo", ImVec2(min.x + (size.x - width) / 2, min.y + (size.y - width * aspect) / 2), width, aspect, app::withAlpha(app::kText, alpha));
  const float bar = static_cast<float>(std::min(1.0, seconds / kShow));
  draw->AddRectFilled(ImVec2(min.x, min.y + size.y - 3 * scale), ImVec2(min.x + size.x, min.y + size.y), app::withAlpha(app::kText, static_cast<int>(30 * fade)));
  draw->AddRectFilled(ImVec2(min.x, min.y + size.y - 3 * scale), ImVec2(min.x + size.x * bar, min.y + size.y), app::withAlpha(app::kAccent, alpha));
  if (!build.empty()) {
    const ImVec2 text = ImGui::CalcTextSize(build.c_str());
    draw->AddText(ImVec2(min.x + size.x - text.x - 12 * scale, min.y + size.y - text.y - 10 * scale), app::withAlpha(app::kDimText, static_cast<int>(alpha * 0.7f)), build.c_str());
  }
  return true;
}

// Renders the song as it is now to a WAV file on another thread; the answer (a message for the status line) arrives through `exporting`.
void startExport(Session& session, const std::string& path) {
  if (session.exporting.valid() && session.exporting.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { session.editStatus = "an export is already running"; return; }
  session.editStatus = "exporting " + path + " ...";
  session.exporting = std::async(std::launch::async, [project = session.doc->project(), path]() -> std::string {
    try {
      RenderOptions render;
      render.threads = 2;
      const RenderResult result = renderSong(project, render);
      writeWav(path, result.audio);
      return "exported " + path + (result.warnings.empty() ? std::string() : "  (" + result.warnings.front() + ")");
    } catch (const std::exception& e) {
      return std::string("export failed: ") + e.what();
    }
  });
}

// A new song: nothing in it but the master lane (the tempo node and the output). Its master shows four columns: tempo, swing, ticks per beat and
// volume; the limiter's are exposed from the parameter panel when wanted. Save asks for a file.
Project blankProject() {
  static const char* const kBlank = R"({
    "format": "webtracker", "version": 1,
    "song": {"name": "untitled", "bpm": 120, "ticksPerBeat": 4, "length": 256},
    "instruments": {"master": {
      "graph": {"nodes": [
        {"id": "m", "type": "tempo", "x": 40, "y": 108, "params": {"bpm": 120, "swing": 50, "tpb": 4, "volume": 1}},
        {"id": "out", "type": "out", "x": 240, "y": 108}], "connections": []},
      "noteColumns": 0,
      "exposed": [
        {"node": "m", "param": "bpm", "label": "BPM", "bits": 16}, {"node": "m", "param": "swing", "label": "Swg"},
        {"node": "m", "param": "tpb", "label": "TPB"}, {"node": "m", "param": "volume", "label": "Vol"}],
      "commands": []}},
    "samples": {},
    "lanes": [{"id": "master", "instrument": "master", "patterns": {}, "placements": []}]
  })";
  return parseProject(kBlank);
}

// The pattern editor's right-click menu (the web's items, with this app's keys). What is not in this app yet is a stub that says so.
void drawPatternMenu(Session* session, Document& doc, Player& player) {
  if (!ImGui::BeginPopup("patternmenu")) return;
  CellEditor& editor = *session->editor;
  const std::vector<Column> columns = player.engine().laneColumns(editor.lane());
  const CellEditor::Selection sel = editor.selection();
  const int from = sel.active ? std::min(sel.cell0, sel.cell1) : editor.cell(), to = sel.active ? std::max(sel.cell0, sel.cell1) : editor.cell();
  bool hasNotes = false;
  for (int c = from; c <= to && c < static_cast<int>(columns.size()); c++) hasNotes |= columns[static_cast<std::size_t>(c)].kind == ColumnKind::Note;
  const char* up = hasNotes ? "Transpose up" : "Values up";
  const char* down = hasNotes ? "Transpose down" : "Values down";
  const float width = app::menuWidth({{"Cut", "Ctrl+X"}, {"Paste", "Ctrl+V"}, {"Transpose down", "Alt+Up"}, {"Values down", "Ctrl+,"}, {"Down an octave", "Alt+Shift+Down"}, {"Interpolate...", "Ctrl+I"}, {"Chord palette...", "Alt+C"}, {"Machine help", "Ctrl+Alt+H"}});
  bool changed = false;
  const auto stub = [&](const char* what) { session->editStatus = std::string(what) + " is not in this app yet."; ImGui::CloseCurrentPopup(); };
  if (app::menuRow("Cut", "Ctrl+X", width)) { ImGui::SetClipboardText(editor.copy().c_str()); changed |= editor.cut(); ImGui::CloseCurrentPopup(); }
  if (app::menuRow("Copy", "Ctrl+C", width)) { ImGui::SetClipboardText(editor.copy().c_str()); ImGui::CloseCurrentPopup(); }
  if (app::menuRow("Paste", "Ctrl+V", width)) {
    const char* text = ImGui::GetClipboardText();
    changed |= (text && *text) ? editor.paste(text) : editor.paste();
    ImGui::CloseCurrentPopup();
  }
  if (app::menuRow("Clear", "Delete", width)) { changed |= editor.clearCell(false); ImGui::CloseCurrentPopup(); }
  ImGui::Separator();
  if (app::menuRow(up, hasNotes ? "Alt+Up" : "Ctrl+.", width)) { changed |= editor.transpose(hasNotes, 1, false); ImGui::CloseCurrentPopup(); }
  if (app::menuRow(down, hasNotes ? "Alt+Down" : "Ctrl+,", width)) { changed |= editor.transpose(hasNotes, -1, false); ImGui::CloseCurrentPopup(); }
  if (hasNotes) {
    if (app::menuRow("Up an octave", "Alt+Shift+Up", width)) { changed |= editor.transpose(true, 1, true); ImGui::CloseCurrentPopup(); }
    if (app::menuRow("Down an octave", "Alt+Shift+Down", width)) { changed |= editor.transpose(true, -1, true); ImGui::CloseCurrentPopup(); }
  }
  ImGui::Separator();
  if (app::menuRow("Interpolate...", "Ctrl+I", width)) stub("Interpolate");
  if (app::menuRow("Randomize...", "Ctrl+R", width)) stub("Randomize");
  if (hasNotes && app::menuRow("Chord palette...", "Alt+C", width)) stub("The chord palette");
  ImGui::Separator();
  const int at = editor.cell();
  if (at >= 0 && at < static_cast<int>(columns.size()) && columns[static_cast<std::size_t>(at)].kind == ColumnKind::Cmd && app::menuRow("What does this command do?", "", width)) stub("Command help");
  if (app::menuRow("Machine help", "Ctrl+Alt+H", width)) stub("Machine help");
  ImGui::EndPopup();
  if (changed) {
    const std::vector<std::string> problems = syncLanes(doc, player.engine());
    session->editStatus = problems.empty() ? std::string() : problems.front();
  }
}

// The id of the machine a lane plays (empty when there is no such lane).
std::string laneMachine(const Document& doc, const std::string& laneId) {
  for (const Lane& l : doc.project().lanes) if (l.id == laneId) return l.instrument;
  return {};
}

// Ctrl+Alt+Up/Down: one more (+1) or one fewer (-1) Note + Vol pair for a machine (on the Pattern screen the machine of the lane the cursor is in, on
// the Instrument screen the machine on show). The last pair only goes when it is empty everywhere in the machine's lanes: nothing written is ever
// thrown away by a key. Returns the message for the status line.
std::string changeNoteColumns(Session* session, Document& doc, Player& player, const std::string& machineId, int direction) {
  const Instrument* machine = nullptr;
  for (const Instrument& i : doc.project().instruments) if (i.id == machineId) machine = &i;
  std::vector<std::string> laneIds;   // the lanes that play it
  for (const Lane& l : doc.project().lanes) if (l.instrument == machineId) laneIds.push_back(l.id);
  if (!machine || laneIds.empty()) return machineId + " has no lane to hold note columns.";
  const std::vector<Column> columns = player.engine().laneColumns(laneIds.front());
  std::vector<std::size_t> notes;
  for (std::size_t i = 0; i < columns.size(); i++) if (columns[i].kind == ColumnKind::Note) notes.push_back(i);
  if (notes.empty()) return machineId + " has no note columns.";
  if (direction > 0 && machine->noteColumns >= 8) return "A machine has at most eight note columns.";
  if (direction < 0) {
    if (notes.size() < 2) return "A machine keeps at least one note column.";
    const std::size_t last = notes.back();
    const bool hasVel = last + 1 < columns.size() && columns[last + 1].kind == ColumnKind::Vel;
    for (const std::string& laneId : laneIds) {
      for (const std::vector<std::string>& row : session->rows[laneId]) {
        for (const std::size_t c : {last, last + 1}) {
          if (c == last + 1 && !hasVel) continue;
          if (c < row.size() && row[c].find_first_not_of('.') != std::string::npos) return "The last note column has notes in it: clear them first.";
        }
      }
    }
  }
  doc.apply(std::make_unique<SetNoteColumns>(machine->id, machine->noteColumns + direction));
  const std::vector<std::string> problems = syncLanes(doc, player.engine());
  return problems.empty() ? machine->id + ": " + std::to_string(machine->noteColumns) + (machine->noteColumns == 1 ? " note column." : " note columns.") : problems.front();
}

void drawFrame(Session* session, const Options& options, const std::string& status, float& meterL, float& meterR, std::map<ImGuiKey, int>& held,
               int& octave, std::string& selectedLane, bool& follow, int& playFromRequest) {
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("wax", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

  if (!session) {
    ImGui::TextUnformatted("Drop a .wax, .xm or .it file on the window to open it.");
    if (!status.empty()) ImGui::TextColored(ImVec4(0.89f, 0.64f, 0.29f, 1), "%s", status.c_str());
    ImGui::End();
    return;
  }
  Player& player = *session->player;
  Document& doc = *session->doc;
  CellEditor& editor = *session->editor;
  const Song& song = doc.project().song;
  if (session->rowsRevision != doc.revision()) {   // the table shows the song as it is edited
    for (const Lane& lane : doc.project().lanes) session->rows[lane.id] = app::flattenLane(lane, song.length);
    session->rowsRevision = doc.revision();
  }

  bool requestSave = false;   // the File menu or Ctrl+S

  const HeaderState where = drawHeader(session, options, follow, requestSave, meterL, meterR, playFromRequest);
  const bool playing = where.playing;
  const int tick = where.tick, perBeat = where.perBeat, perBar = where.perBar;

  ImGuiIO& io = ImGui::GetIO();
  const bool ctrl = io.KeyCtrl;
  KeyboardEnv keyboard{octave, follow, playFromRequest, selectedLane, held, requestSave, playing};
  const bool edited = handleKeyboard(session, keyboard);
  if (edited) {
    const std::vector<std::string> problems = syncLanes(doc, player.engine());
    session->editStatus = problems.empty() ? std::string() : problems.front();
  }
  // the cursor's columns follow the lane as it is now (a Note + Vol pair added or taken away, an exposed parameter, an undo): the editor keeps its own copy
  if (!editor.lane().empty()) editor.setLane(editor.lane(), player.engine().laneColumns(editor.lane()));
  for (auto it = held.begin(); it != held.end();) {   // a held key lets go when it is up (or the text box took the keyboard)
    if (!ImGui::IsKeyDown(it->first) || io.WantTextInput) {
      player.ring().push({LiveCommand::NoteOff, it->second, 0});
      it = held.erase(it);
    } else {
      ++it;
    }
  }

  // The screens, as in the web app's rail: Song (the arrangement), Pattern, Instrument, Info, Config. F1, F2, F3, F10 and F11 switch.
  const float scale = ImGui::GetStyle().FontScaleDpi;
  if (!io.WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
      if (io.KeyShift) {   // Shift+F1: the help of the node picked in the graph
        for (const Instrument& inst : doc.project().instruments) {
          if (inst.id != session->panel.instrument) continue;
          for (const GraphNode& n : inst.graph.nodes) if (n.id == session->panel.node) app::showNodeHelp(session->nodeHelp, n.type);
        }
      } else {
        session->screen = Screen::Song;
      }
    }
    if (session->screen == Screen::Instrument && ctrl && io.KeyAlt && !io.KeyShift) {   // Ctrl+Alt+Up/Down: a Note + Vol pair more or fewer for the machine on show
      const int direction = ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? -1 : 0;
      if (direction != 0) session->editStatus = changeNoteColumns(session, doc, player, session->panel.instrument, direction);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) session->screen = Screen::Pattern;
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) session->screen = Screen::Instrument;
    if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) session->screen = Screen::Sample;
    if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) session->screen = Screen::Info;
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) session->screen = Screen::Config;
  }
  const bool mixerShown = session->showMixer && (session->screen == Screen::Song || session->screen == Screen::Pattern || session->screen == Screen::Instrument);
  const float statusH = ImGui::GetTextLineHeightWithSpacing() + 6 * scale;
  const float below = -(statusH + (mixerShown ? app::mixerHeight() + app::mixerGripHeight() : 0.0f));
  drawRail(session, scale, below);
  ImGui::SameLine();
  const float railGap = gFileX > 0 ? std::max(0.0f, gFileX - ImGui::GetCursorScreenPos().x) : 0.0f;   // the browser's tabs start where the File word does
  ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + railGap, ImGui::GetCursorScreenPos().y));
  // The browser, down the left of every screen as in the web app: tabs for the lists (patterns, machines, samples), and under them
  // the options of the screen being shown (the Song screen's lane tools).
  const Lane* listed = nullptr;
  for (const Lane& l : doc.project().lanes) if (l.id == selectedLane) listed = &l;
  if (!listed) for (const Lane& l : doc.project().lanes) if (l.id != kMaster) { listed = &l; selectedLane = l.id; break; }   // none picked yet: the first lane
  app::PatternListResult listed2;
  std::string laneInstrument;
  FrameContext frame{session, options, player, doc, editor, song, selectedLane, laneInstrument, playing, tick, follow, playFromRequest, held, octave, perBeat, perBar, scale, statusH, mixerShown};
  if (session->showBrowser) drawBrowser(frame, listed, listed2, railGap, below);
    bool listEdited = false;
    if (listed) editor.setLane(listed->id, player.engine().laneColumns(listed->id));
    if (listed2.create) listEdited |= editor.createPatternHere();
    if (listed && !listed2.rename.empty()) { doc.apply(std::make_unique<RenamePattern>(listed->id, session->patternList.picked, listed2.rename)); listEdited = true; }
    if (listed && !listed2.colourFor.empty()) {
      doc.apply(std::make_unique<SetPatternColor>(listed->id, listed2.colourFor, listed2.colour));
      listEdited = true;
    }
    if (listed && listed2.clone) {
      const std::string copyId = "clone-" + std::to_string(static_cast<long long>(ImGui::GetTime() * 1000)) + "-" + std::to_string(doc.revision());
      doc.apply(std::make_unique<ClonePattern>(listed->id, session->patternList.picked, copyId));
      session->patternList.picked = copyId; listEdited = true;
    }
    std::string toPlace = listed2.place;
    if (session->screen == Screen::Song && ImGui::IsKeyPressed(ImGuiKey_Space, false) && !ImGui::GetIO().WantTextInput) toPlace = session->patternList.picked;   // Space: the outlined pattern here
    if (!toPlace.empty()) {
      const int before = editor.tick();
      if (editor.placePatternHere(toPlace)) {
        listEdited = true;
        if (listed) for (const Pattern& pat : listed->patterns) if (pat.id == toPlace) editor.moveTo(std::min(before + static_cast<int>(pat.rows.size()), song.length - 1), editor.cell());   // on to where it ends
      }
    }
    if ((session->screen == Screen::Song || session->screen == Screen::Pattern) && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyShift && !ImGui::GetIO().WantTextInput) {   // Ctrl+Up / Ctrl+Down step through the pattern list (on the Pattern screen too)
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) app::stepPatternList(session->patternList, listed, -1);
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) app::stepPatternList(session->patternList, listed, 1);
    }
    if (listEdited) { const std::vector<std::string> problems = syncLanes(doc, player.engine()); session->editStatus = problems.empty() ? std::string() : problems.front(); }
  ImGui::BeginChild("screen", ImVec2(0, below), false);
  for (const Lane& lane : doc.project().lanes) if (lane.id == selectedLane) laneInstrument = lane.instrument;   // (after the browser: it may have picked another lane)
  switch (session->screen) {
    case Screen::Song: drawSongScreen(frame); break;
    case Screen::Pattern: drawPatternScreen(frame); break;
    case Screen::Instrument: drawInstrumentScreen(frame); break;
    case Screen::Sample: app::drawSampleScreen(session->sampleView, doc, player.preview(), scale); break;
    case Screen::Info: drawInfoScreen(frame); break;
    case Screen::Config: drawConfigScreen(frame); break;
  }
  ImGui::EndChild();
  // The keys play the machine being worked on: the lane's machine, or the one picked in the browser.
  if (!laneInstrument.empty() && laneInstrument != session->keyjazzInstrument &&
      std::find(session->playable.begin(), session->playable.end(), laneInstrument) != session->playable.end()) {
    // Only the keys held down are let go (on the machine they were played on): AllOff would also cut what the song is playing on it.
    for (const auto& [key, note] : held) player.ring().push({LiveCommand::NoteOff, note, 0});
    held.clear();
    session->keyjazzInstrument = laneInstrument;
    player.setInstrument(laneInstrument);
  }
  if (session->screen == Screen::Song || session->screen == Screen::Pattern) {   // the floating parameter windows: the cursor lane's machine, and the pinned ones
    const ImGuiViewport* view = ImGui::GetMainViewport();
    const ImVec2 areaMin(view->WorkPos.x + 60 * scale, view->WorkPos.y + 40 * scale), areaMax(view->WorkPos.x + view->WorkSize.x, view->WorkPos.y + view->WorkSize.y - 40 * scale);
    if (app::drawFloatPanels(session->panel, doc, player.engine(), laneInstrument, areaMin, areaMax)) {
      syncParams(doc, player.engine());
      const std::vector<std::string> problems = syncLanes(doc, player.engine());
      if (!problems.empty()) session->editStatus = problems.front();
    }
  }
  if (!session->panel.drawNode.empty()) { app::showEnvelopes(session->envelopes, laneInstrument, session->panel.drawNode); session->panel.drawNode.clear(); }
  ImGui::Separator();
  {
    const std::vector<std::string> sampleProblems = syncSamples(doc, player.engine());   // (after any edit or undo of a sample)
    if (!sampleProblems.empty()) session->editStatus = sampleProblems.front();
  }
  if (app::drawEnvelopeWindow(session->envelopes, doc, player.engine())) syncData(doc, player.engine());
  app::drawNodeHelp(session->nodeHelp);
  if (!session->panel.shapeNode.empty()) { app::showShape(session->shape, laneInstrument, session->panel.shapeNode); session->panel.shapeNode.clear(); }
  if (app::drawShapeWindow(session->shape, doc)) syncData(doc, player.engine());
  if (!session->panel.tableNode.empty()) { app::showWavetable(session->table, laneInstrument, session->panel.tableNode); session->panel.tableNode.clear(); }
  if (app::drawWavetableWindow(session->table, doc, player.engine())) syncData(doc, player.engine());
  if (!session->panel.chordNode.empty()) { app::showChords(session->chords, laneInstrument, session->panel.chordNode); session->panel.chordNode.clear(); }
  if (app::drawChordWindow(session->chords, doc, player.engine())) syncData(doc, player.engine());
  if (!session->panel.gateNode.empty()) { app::showGate(session->gate, laneInstrument, session->panel.gateNode); session->panel.gateNode.clear(); }
  if (app::drawGateWindow(session->gate, doc, player.engine())) syncData(doc, player.engine());
  if (!session->panel.vectorNode.empty()) { app::showVector(session->vector, laneInstrument, session->panel.vectorNode); session->panel.vectorNode.clear(); }
  if (!session->panel.genexprNode.empty()) { app::showGenExpr(session->genexpr, laneInstrument, session->panel.genexprNode); session->panel.genexprNode.clear(); }
  if (app::drawGenExprWindow(session->genexpr, doc, player.engine())) syncData(doc, player.engine());
  if (app::drawVectorWindow(session->vector, doc, player.engine())) { syncData(doc, player.engine()); syncParams(doc, player.engine()); }
  if (!session->panel.dxNode.empty()) { app::showDx(session->dx, laneInstrument, session->panel.dxNode); session->panel.dxNode.clear(); }
  if (app::drawDxWindow(session->dx, doc, player.engine())) { syncData(doc, player.engine()); syncParams(doc, player.engine()); }
  if (session->mixer.hide) { session->showMixer = false; session->mixer.hide = false; }   // (the grip was pulled right down)
  if (mixerShown && app::drawMixer(session->mixer, doc, player.engine(), selectedLane)) {
    syncParams(doc, player.engine());
    const std::vector<std::string> problems = syncLanes(doc, player.engine());
    if (!problems.empty()) session->editStatus = problems.front();
  }
  {   // the wiring changed (an edit, undo or redo): the engine takes the new graph; when it cannot be compiled the change is taken back
    const std::vector<std::string> wiring = syncGraphs(doc, player.engine());
    if (!wiring.empty()) {
      session->editStatus = wiring.front();
      doc.undo();
      syncGraphs(doc, player.engine());
    }
  }
  // The machine tree: a new instrument of the kind picked, with a blank lane of its own
  if (session->wantTree) { session->wantTree = false; app::openMachineTree(); }
  {
    std::vector<std::string> sampleIds;   // the project's own samples (not the tap pool's slots): what a machine that plays samples is given
    if (doc.project().samples.is_object()) for (const auto& [id, sample] : doc.project().samples.items()) if (!isPoolSample(id)) sampleIds.push_back(id);
    const std::string kind = app::drawMachineTree(!sampleIds.empty());
    const Json definition = kind.empty() ? Json() : app::machineJson(kind, sampleIds);
    if (!definition.is_null()) {
      std::vector<std::string> instrumentIds, laneIds;
      for (const Instrument& i : doc.project().instruments) instrumentIds.push_back(i.id);
      for (const Lane& l : doc.project().lanes) laneIds.push_back(l.id);
      const std::string instrumentId = app::uniqueName(kind, instrumentIds);
      std::string laneBase = instrumentId;   // a second lane for 'clap2' is 'clap3', not 'clap22'
      if (std::find(laneIds.begin(), laneIds.end(), laneBase) != laneIds.end()) { while (!laneBase.empty() && std::isdigit(static_cast<unsigned char>(laneBase.back()))) laneBase.pop_back(); if (laneBase.empty()) laneBase = instrumentId; }
      const std::string laneId = app::uniqueName(laneBase, laneIds);
      doc.apply(std::make_unique<AddInstrument>(instrumentId, definition, laneId));
      std::vector<std::string> problems = syncGraphs(doc, player.engine());   // the machine first (it gives its parameters their slots), then its lane
      for (const std::string& p : syncLanes(doc, player.engine())) problems.push_back(p);
      session->editStatus = problems.empty() ? "Added " + instrumentId + "." : problems.front();
      const Instrument& added = doc.project().instruments.back();
      if (added.id != kMaster && !added.isMeta) session->playable.push_back(added.id);
      session->handoff.openLane(laneId);
      session->instrument = instrumentId;
    }
  }

  // The status bar, as the web's footer: what the keyboard is doing, the cursor, the last message, undo and redo; at the right the audio
  // load, the mixer toggle, the limiter, panic and the theme.
  {
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - statusH + 3 * scale);
    ImDrawList* bar = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    bar->AddLine(ImVec2(ImGui::GetWindowPos().x, at.y - 3 * scale), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), at.y - 3 * scale), app::lineAlpha(120));
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);   // a smaller face, so more fits along the bar
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kText), "%d.%d  %d/%d", tick / perBar + 1, tick % perBar / perBeat + 1, tick, song.length);   // bar.beat  tick/length
    ImGui::SameLine(0, 14 * scale);
    if (session->edit) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kWarm), "EDIT");
    else ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "Enter: edit");
    ImGui::SameLine(0, 14 * scale);
    if (session->screen == Screen::Song) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "%s", app::describeSongCursor(doc.project(), selectedLane, editor.tick()).c_str());
    else {
      std::string where;   // which pattern the cursor is in, and which placement of it ("01 pat01 - 2nd of 3 in this lane - row 3/16")
      for (const Lane& l : doc.project().lanes) {
        if (l.id != editor.lane()) continue;
        const Placement* under = seq::placementAt(l, editor.tick());
        const Pattern* pat = under ? seq::patternOf(l, under->pattern) : nullptr;
        if (!pat) { where = "  no pattern"; break; }
        int nth = 0, total = 0;
        for (const Placement& p : l.placements) if (p.pattern == under->pattern) { total++; if (p.start <= under->start) nth++; }
        char text[160];
        std::snprintf(text, sizeof text, "  %02X \"%s\" \xc2\xb7 %d of %d in this lane \xc2\xb7 row %d/%d", pat->index, pat->name.c_str(), nth, total, editor.tick() - under->start + 1, std::min(under->length, static_cast<int>(pat->rows.size())));
        where = text;
        break;
      }
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "%s  tick %d  octave %d%s", selectedLane.c_str(), editor.tick(), octave, where.c_str());
    }
    ImGui::SameLine(0, 14 * scale);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "undo: %s  redo: %s", doc.canUndo() ? doc.undoLabel().c_str() : "-", doc.canRedo() ? doc.redoLabel().c_str() : "-");
    if (!session->editStatus.empty()) {
      ImGui::SameLine(0, 14 * scale);
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kWarm), "%s", session->editStatus.c_str());
    }
    static float rightWidth = 520 * scale;   // how wide the right-hand group was last frame, so it can end at the window's edge
    ImGui::SameLine(std::max(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 20 * scale, ImGui::GetWindowWidth() - rightWidth - 10 * scale));   // (flush right, but never over what was written)
    const float rightStart = ImGui::GetCursorPosX();
    if (session->audio) {
      const double load = session->audio->load();
      // The numbers are right-aligned in cells as wide as their largest value, so what follows them stays where it is.
      const ImVec4 colour = ImGui::ColorConvertU32ToFloat4(load > 0.7 ? app::kWarm : app::kDimText);
      const auto cell = [&](const char* label, const char* widest, const char* value) {
        const float width = ImGui::CalcTextSize(widest).x;
        ImGui::TextColored(colour, "%s", label);
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + width - ImGui::CalcTextSize(value).x);
        ImGui::TextColored(colour, "%s", value);
      };
      char cpu[16], late[16], cores[24];
      std::snprintf(cpu, sizeof cpu, "%.0f%%", 100 * load);
      std::snprintf(cores, sizeof cores, "%.1f/%d", session->audio->coresBusy(), session->audio->coresAvailable());
      std::snprintf(late, sizeof late, "%ld", session->audio->lateBuffers());
      cell("cpu ", "100%", cpu);
      ImGui::SameLine(0, 0);
      cell("  cores ", "00.0/00", cores);
      ImGui::SameLine(0, 0);
      cell("  late ", "0000", late);
      ImGui::SameLine(0, 14 * scale);
    }
    if (app::iconButton("browser-toggle", "Browser: the patterns and machines, down the left of every screen (Alt+B)", session->showBrowser, 16)) session->showBrowser = !session->showBrowser;
    ImGui::SameLine(0, 10 * scale);
    if (app::iconButton("mixer-toggle", "Mixer: a fader for each lane, along the bottom of the Song, Pattern and Instrument screens", session->showMixer, 16)) session->showMixer = !session->showMixer;
    ImGui::SameLine(0, 10 * scale);
    if (app::textButton("params", session->panel.followOpen)) session->panel.followOpen = !session->panel.followOpen;   // the floating parameter window of the cursor lane's machine (Song and Pattern screens)
    ImGui::SameLine(0, 10 * scale);
    const double limitOn = player.engine().registry().slotOf("master.m.limit") >= 0 ? player.engine().registry().get("master.m.limit") : 0;
    if (app::textButton("Lim", limitOn > 0.5)) { doc.apply(std::make_unique<SetParam>("master", "m", "limit", limitOn > 0.5 ? 0.0 : 1.0)); syncParams(doc, player.engine()); }
    ImGui::SameLine(0, 10 * scale);
    if (app::textButton("panic")) player.ring().push({LiveCommand::AllOff, 0, 0});
    ImGui::SameLine(0, 14 * scale);
    ImGui::SetNextItemWidth(110 * scale);
    if (ImGui::BeginCombo("##theme", app::themeName(app::themeIndex()), ImGuiComboFlags_HeightLarge)) {
      for (int i = 0; i < app::themeCount(); i++) {
        if (ImGui::Selectable(app::themeName(i), i == app::themeIndex())) { app::setTheme(i); saveTheme(); }
      }
      ImGui::EndCombo();
    }
    rightWidth = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x - rightStart;
    ImGui::PopFont();
  }
  ImGui::End();
}

// "enter,down,ctrl+z,4,7" -> key events, one key every two frames (down, then up).
ImGuiKey keyNamed(const std::string& name) {
  static const std::map<std::string, ImGuiKey> named = {
      {"enter", ImGuiKey_Enter}, {"up", ImGuiKey_UpArrow}, {"down", ImGuiKey_DownArrow}, {"left", ImGuiKey_LeftArrow},
      {"right", ImGuiKey_RightArrow}, {"delete", ImGuiKey_Delete}, {"backspace", ImGuiKey_Backspace}, {"insert", ImGuiKey_Insert},
      {"equal", ImGuiKey_Equal}, {"pagedown", ImGuiKey_PageDown}, {"pageup", ImGuiKey_PageUp}, {"space", ImGuiKey_Space},
      {"tab", ImGuiKey_Tab}, {"escape", ImGuiKey_Escape}, {"f1", ImGuiKey_F1}, {"f2", ImGuiKey_F2}, {"f3", ImGuiKey_F3}, {"f4", ImGuiKey_F4},
      {"f5", ImGuiKey_F5}, {"f6", ImGuiKey_F6}, {"f7", ImGuiKey_F7}, {"f8", ImGuiKey_F8}, {"f9", ImGuiKey_F9}, {"insert", ImGuiKey_Insert}};
  const auto found = named.find(name);
  if (found != named.end()) return found->second;
  if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (name[0] - '0'));
  if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') return static_cast<ImGuiKey>(ImGuiKey_A + (name[0] - 'a'));
  return ImGuiKey_None;
}

// "100,200;150,220" -> a click at each place, one every six frames: the pointer moves, goes down two frames later and up two after that.
void feedClicks(const std::string& script, int frame) {
  std::vector<std::pair<float, float>> places;
  std::vector<int> buttons;   // 0 left, 1 right ("r:x,y")
  std::stringstream stream(script);
  for (std::string item; std::getline(stream, item, ';');) {
    float x = 0, y = 0;
    const bool right = item.rfind("r:", 0) == 0;
    if (std::sscanf(item.c_str() + (right ? 2 : 0), "%f,%f", &x, &y) == 2) { places.emplace_back(x, y); buttons.push_back(right ? 1 : 0); }
  }
  const int index = frame / 6, phase = frame % 6;
  if (index < 0 || index >= static_cast<int>(places.size())) return;
  ImGuiIO& io = ImGui::GetIO();
  if (phase == 0) io.AddMousePosEvent(places[static_cast<std::size_t>(index)].first, places[static_cast<std::size_t>(index)].second);
  if (phase == 2) io.AddMouseButtonEvent(buttons[static_cast<std::size_t>(index)], true);
  if (phase == 4) io.AddMouseButtonEvent(buttons[static_cast<std::size_t>(index)], false);
}

// "x1,y1;x2,y2" -> the mouse goes to the first place, presses, moves to the second over six frames and stays pressed (a screenshot then shows
// a drag in progress).
void feedDrag(const std::string& script, int frame) {
  float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  if (std::sscanf(script.c_str(), "%f,%f;%f,%f", &x1, &y1, &x2, &y2) != 4) return;
  ImGuiIO& io = ImGui::GetIO();
  if (frame == 0) io.AddMousePosEvent(x1, y1);
  if (frame == 2) io.AddMouseButtonEvent(0, true);
  if (frame >= 4 && frame <= 10) { const float t = std::min(1.0f, (frame - 4) / 6.0f); io.AddMousePosEvent(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t); }
}

void feedKeys(const std::string& script, int frame) {
  std::vector<std::string> keys;
  std::stringstream stream(script);
  for (std::string item; std::getline(stream, item, ',');) keys.push_back(item);
  ImGuiIO& io = ImGui::GetIO();
  const int index = frame / 2;
  if (index < 0 || index >= static_cast<int>(keys.size())) return;
  std::string name = keys[static_cast<std::size_t>(index)];
  bool ctrl = false, shift = false, alt = false;
  for (;;) {
    if (name.rfind("alt+", 0) == 0) { alt = true; name = name.substr(4); }
    else if (name.rfind("ctrl+", 0) == 0) { ctrl = true; name = name.substr(5); }
    else if (name.rfind("shift+", 0) == 0) { shift = true; name = name.substr(6); }
    else break;
  }
  const bool down = frame % 2 == 0;
  io.AddKeyEvent(ImGuiMod_Ctrl, ctrl && down);
  io.AddKeyEvent(ImGuiMod_Shift, shift && down);
  io.AddKeyEvent(ImGuiMod_Alt, alt && down);
  io.AddKeyEvent(keyNamed(name), down);
  if (down && name.size() == 1 && !ctrl && !alt) io.AddInputCharacter(static_cast<unsigned>(name[0]));   // (a text box that has the keyboard takes it as typing)
}

bool parse(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    const bool more = i + 1 < argc;
    if (a == "--rate" && more) o.rate = std::atof(argv[++i]);
    else if (a == "--threads" && more) o.threads = std::max(1, std::atoi(argv[++i]));
    else if (a == "--buffer" && more) o.buffer = std::max(0, std::atoi(argv[++i]));
    else if (a == "--midi" && more) o.midiPort = std::atoi(argv[++i]);
    else if (a == "--screenshot" && more) o.screenshot = argv[++i];
    else if (a == "--press" && more) o.press = argv[++i];
    else if (a == "--clicks" && more) o.clicks = argv[++i];
    else if (a == "--drag" && more) o.drag = argv[++i];
    else if (a == "--envelope" && more) o.envelope = argv[++i];
    else if (a == "--dx" && more) o.dxnode = argv[++i];
    else if (a == "--vector" && more) o.vectornode = argv[++i];
    else if (a == "--genexpr" && more) o.genexprnode = argv[++i];
    else if (a == "--screen" && more) o.screen = argv[++i];
    else if (a == "--theme" && more) o.theme = argv[++i];
    else if (a == "--frames" && more) o.frames = std::max(1, std::atoi(argv[++i]));
    else if (a == "--play-from" && more) o.playFrom = std::atoi(argv[++i]);
    else if (a == "--null") o.useNull = true;
    else if (a == "--all-lanes") o.allLanes = true;
    else if (a == "--splash") { o.splash = true; o.splashForced = true; }
    else if (a == "--no-splash") o.splash = false;
    else if (a == "--no-play") o.noPlay = true;
    else if (!a.empty() && a[0] != '-' && o.path.empty()) o.path = a;
    else return false;
  }
  return true;
}

}  // namespace wax::tool

using namespace wax::tool;
using namespace wax;

int main(int argc, char** argv) {
  Options options;
  std::vector<std::string> utf8Args = app::commandLineUtf8();   // (Windows: a double-clicked file's path, with its accents)
  std::vector<char*> utf8Pointers;
  if (!utf8Args.empty()) {
    for (std::string& arg : utf8Args) utf8Pointers.push_back(arg.data());
    argc = static_cast<int>(utf8Pointers.size());
    argv = utf8Pointers.data();
  }
  if (!parse(argc, argv, options)) {
    std::printf("usage: wax-app [song.wax] [--rate hz] [--threads n] [--midi n] [--null] [--screenshot out.bmp [--frames n] [--play-from tick]]\n");
    return 1;
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::printf("SDL: %s\n", SDL_GetError());
    return 2;
  }
  app::loadSettings();   // (before the window: it opens at the size it had when it was last closed)
  SDL_Window* window = SDL_CreateWindow("Wax", app::settings().windowWidth, app::settings().windowHeight, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (app::settings().windowMaximised ? SDL_WINDOW_MAXIMIZED : 0));
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (renderer) { app::iconsInit(renderer); app::setWindowIcon(window, "wax-icon"); }
  if (!renderer) {
    std::printf("SDL: %s\n", SDL_GetError());
    return 2;
  }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  float scale = SDL_GetWindowDisplayScale(window);
  if (scale <= 0) scale = 1;
  app::loadFonts();
  app::applyTheme(scale);
  loadTheme();
  if (!options.theme.empty()) app::setThemeByName(options.theme.c_str());
  if (options.allLanes) app::settings().patternAll = true;
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  std::unique_ptr<Session> session;
  std::string status;
  const auto showScreenOption = [&] {   // --screen: the screen a headless run starts on
    if (options.screen == "song") session->screen = Screen::Song;
    else if (options.screen == "instrument") session->screen = Screen::Instrument;
    else if (options.screen == "sample") session->screen = Screen::Sample;
    else if (options.screen == "info") session->screen = Screen::Info;
    else if (options.screen == "config") session->screen = Screen::Config;
  };
  const auto open = [&](const std::string& path) {
    session.reset();   // the old sound stops before the new song is built
    try {
      const auto endsWith = [&](const char* suffix, const char* upper) { const std::size_t n = std::char_traits<char>::length(suffix); return path.size() > n && (path.compare(path.size() - n, n, suffix) == 0 || path.compare(path.size() - n, n, upper) == 0); };
      if (endsWith(".xm", ".XM")) session = std::make_unique<Session>(app::projectFromXmFile(path), std::string(), options);   // a tracker module opens as a new song
      else if (endsWith(".it", ".IT")) session = std::make_unique<Session>(app::projectFromItFile(path), std::string(), options);
      else session = std::make_unique<Session>(path, options);
      showScreenOption();
      std::string title = "Wax - " + session->project.song.name;
      SDL_SetWindowTitle(window, title.c_str());
      status.clear();
      app::rememberRecent(path);
    } catch (const std::exception& e) {
      status = std::string("Could not open ") + path + ": " + e.what();
      SDL_SetWindowTitle(window, "Wax");
    }
  };
  if (!options.path.empty()) open(options.path);
  else {   // no file given: a new, empty song (File > Open, or a file dropped on the window, replaces it)
    try { session = std::make_unique<Session>(blankProject(), std::string(), options); SDL_SetWindowTitle(window, "Wax - untitled"); showScreenOption(); }
    catch (const std::exception& e) { status = std::string("Could not start a new song: ") + e.what(); }
  }

  float meterL = 0, meterR = 0;
  bool titleUnsaved = false;
  std::map<ImGuiKey, int> held;
  int octave = 3, playFromRequest = -1, frame = 0;
  std::string selectedLane;
  bool follow = app::settings().follow, running = true;
  bool showSplash = options.splash && (options.splashForced || (options.screenshot.empty() && !options.useNull));
  if (options.noPlay) {}
  else if (session && !options.screenshot.empty() && options.playFrom >= 0)
    session->player->ring().push({LiveCommand::Play, options.playFrom, 1});
  else if (session && !options.screenshot.empty())
    session->player->ring().push({LiveCommand::Play, 0, 1});

  while (running) {
    for (SDL_Event event; SDL_PollEvent(&event);) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) running = false;
      if (event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_MAXIMIZED || event.type == SDL_EVENT_WINDOW_RESTORED) {   // remembered (saved when the app closes)
        app::Settings& cfg = app::settings();
        cfg.windowMaximised = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
        int w = 0, h = 0;
        if (!cfg.windowMaximised && SDL_GetWindowSize(window, &w, &h) && w >= 640 && h >= 400) { cfg.windowWidth = w; cfg.windowHeight = h; }
      }
      if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) open(event.drop.data);
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    if (!options.clicks.empty() && frame >= 4) feedClicks(options.clicks, frame - 4);
    if (!options.drag.empty() && frame >= 4) feedDrag(options.drag, frame - 4);
    if (!options.dxnode.empty() && frame == 3 && session) {
      const std::size_t dot = options.dxnode.find('.');
      app::showDx(session->dx, options.dxnode.substr(0, dot), options.dxnode.substr(dot + 1));
    }
    if (!options.genexprnode.empty() && frame == 3 && session) {
      const std::size_t dot = options.genexprnode.find('.');
      app::showGenExpr(session->genexpr, options.genexprnode.substr(0, dot), options.genexprnode.substr(dot + 1));
    }
    if (!options.vectornode.empty() && frame == 3 && session) {
      const std::size_t dot = options.vectornode.find('.');
      app::showVector(session->vector, options.vectornode.substr(0, dot), options.vectornode.substr(dot + 1));
    }
    if (!options.envelope.empty() && frame == 3 && session) {
      const std::size_t dot = options.envelope.find('.');
      app::showEnvelopes(session->envelopes, options.envelope.substr(0, dot), options.envelope.substr(dot + 1));
    }
    if (!options.press.empty() && frame >= 4) feedKeys(options.press, frame - 4);   // (the first frames let the window settle)
    ImGui::NewFrame();
    drawFrame(session.get(), options, status, meterL, meterR, held, octave, selectedLane, follow, playFromRequest);
    if (showSplash) showSplash = drawSplash(SDL_GetTicks() / 1000.0, "native");
    if (!gOpenBackup.empty()) {   // a snapshot from the File menu: opened as an untitled song, so Save cannot overwrite the snapshot
      const std::string file = gOpenBackup;
      gOpenBackup.clear();
      open(file);
      if (session) { session->path.clear(); SDL_SetWindowTitle(window, ("Wax - " + session->project.song.name + " (backup)").c_str()); }
    }
    if (!gOpenRecent.empty()) {   // a song from the recent list: if it has moved since it was saved that is a warning, not an error
      const std::string file = gOpenRecent;
      gOpenRecent.clear();
      std::error_code ignored;
      if (!std::filesystem::exists(pathFromUtf8(file), ignored)) {
        const std::string warning = "Warning: " + file + " is not there any more (moved, renamed or deleted since it was last saved). Use File > Open to find it";
        if (session) session->editStatus = warning; else status = warning;
      }
      else open(file);
    }
    if (gRequest == DialogRequest::ClearArrangement || gRequest == DialogRequest::ClearPatterns) {   // New From Existing: the instruments and samples stay
      const bool patterns = gRequest == DialogRequest::ClearPatterns;
      gRequest = DialogRequest::None;
      if (session) {
        try {
          Project kept = parseProject(serializeProject(session->doc->project()));   // (a copy, as the web app does: it is not an undo step)
          if (!backupFolder().empty()) writeBackup(backupFolder(), kept.song.name, serializeProject(kept), app::settings().backupKeep, std::time(nullptr));   // (the web app snapshots 'before clear')
          for (Lane& lane : kept.lanes) { lane.placements.clear(); if (patterns) lane.patterns.clear(); }
          const std::string keptPath = session->path;
          session.reset();
          session = std::make_unique<Session>(std::move(kept), keptPath, options);
          session->editStatus = patterns ? "Cleared every pattern and the arrangement." : "Cleared the arrangement.";
        } catch (const std::exception& e) { status = std::string("Could not clear: ") + e.what(); }
      }
    }
    if (gRequest == DialogRequest::New) {   // start over with an empty song (the old one is replaced: unsaved changes are lost)
      gRequest = DialogRequest::None;
      {
        session.reset();
        try { session = std::make_unique<Session>(blankProject(), std::string(), options); status.clear(); SDL_SetWindowTitle(window, "Wax - untitled"); }
        catch (const std::exception& e) { status = std::string("Could not start a new song: ") + e.what(); }
      }
    }
    if (gRequest != DialogRequest::None) {
      static const SDL_DialogFileFilter filters[] = {{"Wax songs", "wax"}, {"All files", "*"}};
      static const SDL_DialogFileFilter openFilters[] = {{"Wax songs and tracker modules", "wax;xm;it"}, {"All files", "*"}};
      static const SDL_DialogFileFilter wavFilter[] = {{"WAV audio", "wav"}};
      static DialogKind saveKind = DialogKind::Save, exportKind = DialogKind::Export;
      if (gRequest == DialogRequest::Open) SDL_ShowOpenFileDialog(dialogDone, nullptr, window, openFilters, 2, nullptr, false);
      else if (session && gRequest == DialogRequest::Export) {
        std::string suggestion = session->path.empty() ? session->project.song.name + ".wav" : session->path;
        if (suggestion.size() > 4 && suggestion.substr(suggestion.size() - 4) == ".wax") suggestion = suggestion.substr(0, suggestion.size() - 4) + ".wav";
        SDL_ShowSaveFileDialog(dialogDone, &exportKind, window, wavFilter, 1, suggestion.c_str());
      } else if (session) SDL_ShowSaveFileDialog(dialogDone, &saveKind, window, filters, 2, session->path.c_str());
      gRequest = DialogRequest::None;
    }
    {
      std::string chosen;
      DialogKind kind = DialogKind::Open;
      {
        std::lock_guard<std::mutex> lock(gAnswer.mutex);
        if (gAnswer.ready) { chosen = gAnswer.path; kind = gAnswer.kind; gAnswer.ready = false; }
      }
      if (!chosen.empty()) {
        if (kind == DialogKind::Open) open(chosen);
        else if (kind == DialogKind::Export && session) {
          if (chosen.size() < 4 || chosen.substr(chosen.size() - 4) != ".wav") chosen += ".wav";
          startExport(*session, chosen);
        } else if (session) {
          if (chosen.size() < 4 || chosen.substr(chosen.size() - 4) != ".wax") chosen += ".wax";
          try {
            saveProject(session->doc->project(), chosen);
            session->path = chosen;
            session->doc->markSaved();
            app::rememberRecent(chosen);
            session->editStatus = "saved " + chosen;
          } catch (const std::exception& e) {
            session->editStatus = e.what();
          }
        }
      }
    }
    if (session && session->exporting.valid() && session->exporting.wait_for(std::chrono::seconds(0)) == std::future_status::ready) session->editStatus = session->exporting.get();
    if (session) {   // an asterisk in the title while the song has changes that are not saved
      const bool unsaved = session->doc->modified();
      if (unsaved != titleUnsaved) {
        titleUnsaved = unsaved;
        const std::string title = "Wax - " + session->project.song.name + (unsaved ? " *" : "");
        SDL_SetWindowTitle(window, title.c_str());
      }
    }
    if (session && playFromRequest >= 0) {
      session->player->ring().push({LiveCommand::Play, playFromRequest, 1});
      playFromRequest = -1;
    }
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 22, 27, 32, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

    frame++;
    if (!options.screenshot.empty() && frame >= options.frames) {
      SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr);
      if (shot) {
        SDL_SaveBMP(shot, options.screenshot.c_str());
        SDL_DestroySurface(shot);
        std::printf("saved %s\n", options.screenshot.c_str());
      } else {
        std::printf("SDL: %s\n", SDL_GetError());
      }
      running = false;
    }
    SDL_RenderPresent(renderer);
    if (!options.screenshot.empty()) SDL_Delay(120);   // lets the audio thread play some blocks between frames
  }

  session.reset();
  app::saveSettings();   // (the window size is kept in the settings)
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  app::iconsShutdown();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

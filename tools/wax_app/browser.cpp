// The browser down the left of every screen.
#include "wax_app/app_state.h"

namespace wax::tool {

// The browser down the left of every screen: the lists (Patterns, Machines, Samples) and the options under them.
void drawBrowser(FrameContext& f, const Lane* listed, app::PatternListResult& listed2, float railGap, float below) {
  Session* session = f.session;
  [[maybe_unused]] Player& player = f.player;
  [[maybe_unused]] Document& doc = f.doc;
  [[maybe_unused]] CellEditor& editor = f.editor;
  [[maybe_unused]] const Song& song = f.song;
  [[maybe_unused]] std::string& selectedLane = f.selectedLane;
  [[maybe_unused]] const bool playing = f.playing;
  [[maybe_unused]] const Options& options = f.options;
  [[maybe_unused]] const float scale = f.scale;
  [[maybe_unused]] const float statusH = f.statusH;
  [[maybe_unused]] const bool mixerShown = f.mixerShown;
  [[maybe_unused]] bool& follow = f.follow;
  ImGui::BeginChild("browser", ImVec2(session->browserWidth * scale - railGap, below), false, ImGuiWindowFlags_NoNav);
  {   // the tabs
    const char* tabs[] = {"Patterns", "Machines", "Samples"};
    ImDrawList* tabLines = ImGui::GetWindowDrawList();
    const float left = ImGui::GetCursorScreenPos().x, right = left + ImGui::GetContentRegionAvail().x;
    float baseline = 0;
    for (int i = 0; i < 3; i++) {
      if (i) ImGui::SameLine(0, 8 * scale);
      if (app::textButton(tabs[i], session->browserTab == i)) session->browserTab = i;
      const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
      baseline = hi.y + 1 * scale;
      if (session->browserTab == i) tabLines->AddRectFilled(ImVec2(lo.x, baseline - 1 * scale), ImVec2(hi.x, baseline + 1 * scale), app::kAccent);   // the open tab: underlined
    }
    tabLines->AddLine(ImVec2(left, baseline + 1 * scale), ImVec2(right, baseline + 1 * scale), app::textAlpha(55));   // the line the tabs sit on
    ImGui::Dummy(ImVec2(0, 3 * scale));
  }
  if (session->screen != session->browserScreen) {   // each screen shows the list that goes with it
    session->browserScreen = session->screen;
    if (session->screen == Screen::Instrument) session->browserTab = 1;
    else if (session->screen == Screen::Sample) session->browserTab = 2;
    else if (session->screen == Screen::Song || session->screen == Screen::Pattern) session->browserTab = 0;
  }
  const bool hasOptions = (session->screen == Screen::Song || session->screen == Screen::Pattern || session->screen == Screen::Instrument) && session->browserTab != 2;   // (the Samples tab takes the whole panel)
  const float panelH = ImGui::GetContentRegionAvail().y;
  float optionsH = hasOptions ? (session->browserOptions > 0 ? session->browserOptions * scale : panelH * 0.5f) : 0.0f;
  optionsH = std::clamp(optionsH, 0.0f, std::max(0.0f, panelH - 120 * scale));
  ImGui::BeginChild("browserlist", ImVec2(0, panelH - optionsH - (hasOptions ? 7 * scale : 0)), false, ImGuiWindowFlags_NoNav);
  if (session->browserTab == 0) {
    listed2 = app::drawPatternList(session->patternList, listed, ImGui::GetContentRegionAvail().y);
  } else if (session->browserTab == 1) {
    if (app::textButton("+ add")) session->wantTree = true;
    for (const Instrument& inst : doc.project().instruments) {
      if (app::textButton(inst.id.c_str(), inst.id == session->instrument)) session->instrument = inst.id;
    }
  } else {   // the project's samples, then the folders of the computer to load more from
    const ImVec4 dim = ImGui::ColorConvertU32ToFloat4(app::kDimText);
    ImGui::TextColored(dim, "samples in this song");
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
    bool any = false;
    if (doc.project().samples.is_object())
      for (const auto& [id, sample] : doc.project().samples.items()) {
        if (isPoolSample(id)) continue;
        any = true;
        const double bytes = sample.value("data", std::string()).size() * 0.75, perFrame = sample.value("encoding", std::string("pcm16")) == "pcm16" ? 2.0 : 4.0;
        ImGui::Text("%-24s %5.2fs %s", id.c_str(), bytes / perFrame / std::max(1.0, sample.value("rate", 44100.0)), sample.contains("right") ? "stereo" : "mono");
      }
    if (!any) ImGui::TextColored(dim, "none yet: pick a file below");
    ImGui::PopFont();
    ImGui::Separator();
    const app::SampleBrowserResult picked = app::drawSampleBrowser(session->sampleBrowser, player.preview(), std::filesystem::path(options.path).parent_path(), app::settings().sampleAskMB, session->screen == Screen::Sample ? session->sampleView.selected : std::string());
    if (picked.load) {
      try {
        const StereoAudio audio = readWav(picked.file.string());
        const bool stereo = audio.left != audio.right;
        const Json encoded = encodeSample(audio.sampleRate, audio.left, stereo ? audio.right : std::vector<float>());
        if (picked.replace && doc.project().samples.contains(session->sampleView.selected)) {   // new audio in an existing sample: its levels stay, its loops and markers were places in the old audio
          const std::string id = session->sampleView.selected;
          EditableSample next = readSample(encoded);
          const EditableSample old = readSample(doc.project().samples[id]);
          for (const char* key : {"volume", "globalVolume", "pan"}) if (old.props.contains(key)) next.props[key] = old.props[key];
          doc.apply(std::make_unique<SetSample>(id, writeSample(next, doc.project().samples[id]), "Replace sample " + id));
          session->sampleView.selFrom = session->sampleView.selTo = session->sampleView.head = 0;
          session->editStatus = "Replaced " + id + ".";
        } else {
          const std::string id = uniqueSampleId(doc.project().samples, sampleIdFromName(picked.file.filename().string()));
          doc.apply(std::make_unique<AddSample>(id, encodeSample(audio.sampleRate, audio.left, stereo ? audio.right : std::vector<float>())));
          session->editStatus = "Loaded " + id + ". Add a Sampler from Shift+I to play it.";
        }
      } catch (const std::exception& e) {
        session->editStatus = e.what();
      }
    }
  }
  ImGui::EndChild();
  if (hasOptions) {   // the bar that sets the height of the options, then the options
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##optsplit", ImVec2(-1, 7 * scale));
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) session->browserOptions = std::clamp(optionsH / scale - ImGui::GetIO().MouseDelta.y / scale, 60.0f, 600.0f);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) session->browserOptions = 0;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y + 3 * scale), ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + 3 * scale), hot ? app::kAccent : app::lineAlpha(90), 1.0f);
    ImGui::BeginChild("browseroptions", ImVec2(0, 0), false, ImGuiWindowFlags_NoNav);
    // a button goes on the same line when it fits, else it wraps to the next (the browser is narrow)
    const auto fits = [&](const char* label) { return ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - ImGui::GetItemRectMax().x > ImGui::CalcTextSize(label).x + 24 * scale; };   // room to the right of the last item
    if (session->screen == Screen::Pattern) {   // how the grid is shown
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "Pattern view");
      if (app::checkbox("All lanes side by side", &app::settings().patternAll)) app::saveSettings();
      if (app::checkbox("Tint patterns by colour", &app::settings().tintPatterns)) app::saveSettings();
      if (app::checkbox("Note preview", &app::settings().notePreview)) app::saveSettings();
      ImGui::SetNextItemWidth(120 * ImGui::GetStyle().FontScaleDpi);
      if (ImGui::SliderFloat("Text size", &app::settings().patternText, 0.6f, 2.0f, "%.2fx")) app::saveSettings();
      if (app::checkbox("Hide empty columns", &app::settings().hideEmpty)) app::saveSettings();
      if (app::checkbox("Solo-edit shows the song", &app::settings().soloHatch)) app::saveSettings();   // (the rest hatched and locked; off: the pattern alone)
      if (app::checkbox("Follow the playing row", &follow)) { app::settings().follow = follow; app::saveSettings(); }
    }
    if (session->screen == Screen::Song) {
  if (app::checkbox("Show pattern names (H)", &app::settings().seqNames)) app::saveSettings();
  {   // the lane under the cursor: rename, move, duplicate, remove (the web's lane tools)
    const Lane* current = nullptr;
    for (const Lane& l : doc.project().lanes) if (l.id == selectedLane) current = &l;
    if (current && current->id != kMaster) {
      const std::string curId = current->id, curMachine = current->instrument;   // (the edits below move the lanes about: a pointer into them would go stale)
      if (session->laneNameFor != curId) { std::snprintf(session->laneName, sizeof session->laneName, "%s", curId.c_str()); session->laneNameFor = curId; }
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "lane");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(110 * scale);
      ImGui::InputText("##lanename", session->laneName, sizeof session->laneName);
      std::vector<std::string> laneIds;
      for (const Lane& l : doc.project().lanes) laneIds.push_back(l.id);
      bool laneEdited = false;
      const std::string wanted = session->laneName;
      ImGui::AlignTextToFramePadding();
      if (app::textButton("Rename") && !wanted.empty() && wanted != curId && std::find(laneIds.begin(), laneIds.end(), wanted) == laneIds.end()) {
        doc.apply(std::make_unique<RenameLane>(curId, wanted)); selectedLane = wanted; session->handoff.openLane(wanted); session->laneNameFor = wanted; laneEdited = true;
      }
      if (fits("<")) ImGui::SameLine(0, 8 * scale);
      if (app::textButton("<")) { doc.apply(std::make_unique<MoveLane>(curId, -1)); }
      if (fits(">")) ImGui::SameLine(0, 4 * scale);
      if (app::textButton(">")) { doc.apply(std::make_unique<MoveLane>(curId, 1)); }
      if (fits("Duplicate")) ImGui::SameLine(0, 8 * scale);
      if (app::textButton("Duplicate") && !laneEdited) {
        const std::string copy = app::uniqueName(curId, laneIds);
        doc.apply(std::make_unique<DuplicateLane>(curId, copy)); selectedLane = copy; session->handoff.openLane(copy); laneEdited = true;
      }
      int shares = 0;
      for (const Lane& l : doc.project().lanes) shares += l.instrument == curMachine;
      if (app::textButton("Remove") && !laneEdited) { const std::string gone = curId; doc.apply(std::make_unique<RemoveLane>(gone, false)); session->handoff.dropLaneRequest(); selectedLane.clear(); laneEdited = true; }
      if (shares == 1 && !laneEdited) {
        if (fits("Remove + machine")) ImGui::SameLine(0, 8 * scale);
        if (app::textButton("Remove + machine")) {
          const std::string gone = curId, machine = curMachine;
          doc.apply(std::make_unique<RemoveLane>(gone, true));
          session->playable.erase(std::remove(session->playable.begin(), session->playable.end(), machine), session->playable.end());
          if (session->instrument == machine) session->instrument.clear();
          session->handoff.dropLaneRequest(); selectedLane.clear(); laneEdited = true;
        }
      }
      if (!laneEdited) {   // a lane with its own copy of the machine, to go on changing one while the other keeps the sound
        std::vector<std::string> machineIds;
        for (const Instrument& i : doc.project().instruments) machineIds.push_back(i.id);
        if (fits("Clone with machine")) ImGui::SameLine(0, 8 * scale);
        if (app::textButton("Clone with machine")) {
          const std::string machineCopy = app::uniqueName(curMachine, machineIds), laneCopy = app::uniqueName(curId, laneIds);
          doc.apply(std::make_unique<CloneLaneWithMachine>(curId, machineCopy, laneCopy, session->clonePatterns, session->cloneCurves));
          std::vector<std::string> problems = syncGraphs(doc, player.engine());   // the machine first, then its lane
          laneEdited = true;
          session->playable.push_back(machineCopy);
          selectedLane = laneCopy; session->handoff.openLane(laneCopy);
          session->editStatus = problems.empty() ? std::string() : problems.front();
        }
        if (fits("patterns")) ImGui::SameLine(0, 8 * scale);
        app::checkbox("patterns", &session->clonePatterns);
        if (fits("automation")) ImGui::SameLine(0, 8 * scale);
        app::checkbox("automation", &session->cloneCurves);
      }
      if (laneEdited) { const std::vector<std::string> problems = syncLanes(doc, player.engine()); session->editStatus = problems.empty() ? std::string() : problems.front(); }
      if (!laneEdited) { /* a move changes order only */ }
    }
  }
  {   // insert silence at the cursor row: everything from there on moves later (the web's Insert box)
    const auto insert = [&](int ticks) {
      doc.apply(std::make_unique<InsertSilence>(editor.tick(), ticks, app::settings().insertCut));
      const std::vector<std::string> problems = syncLanes(doc, player.engine());
      syncLoop(doc, player.engine());
      session->editStatus = problems.empty() ? std::string() : problems.front();
    };
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "insert silence");
    ImGui::SameLine();
    char text[16]; std::snprintf(text, sizeof text, "%d", session->silenceTicks);
    const int step = app::spinBox("##silence", text, 56 * scale);
    session->silenceTicks = std::clamp(session->silenceTicks + step * 16, 1, 65536);
    if (fits("Insert")) ImGui::SameLine(0, 8 * scale);
    if (app::textButton("Insert")) insert(session->silenceTicks);
    for (const int preset : {16, 32, 48, 64, 128, 256, 512}) {
      char label[16]; std::snprintf(label, sizeof label, "%d", preset);
      if (fits(label)) ImGui::SameLine(0, 6 * scale);
      if (app::textButton(label)) { session->silenceTicks = preset; insert(preset); }
    }
    if (app::checkbox("cut patterns across the cursor", &app::settings().insertCut)) app::saveSettings();
    {   // an empty lane for a machine the song already has
      std::vector<std::string> machines;
      for (const Instrument& i : doc.project().instruments) if (i.id != kMaster && !i.isMeta) machines.push_back(i.id);
      if (!machines.empty()) {
        session->addLaneFor = std::clamp(session->addLaneFor, 0, static_cast<int>(machines.size()) - 1);
        if (app::textButton("Add lane for")) {
          std::vector<std::string> laneIds;
          for (const Lane& l : doc.project().lanes) laneIds.push_back(l.id);
          const std::string machine = machines[static_cast<std::size_t>(session->addLaneFor)];
          std::string base = machine;   // a second lane for 'clap2' is 'clap3', not 'clap22'
          if (std::find(laneIds.begin(), laneIds.end(), base) != laneIds.end()) { while (!base.empty() && std::isdigit(static_cast<unsigned char>(base.back()))) base.pop_back(); if (base.empty()) base = machine; }
          const std::string laneId = app::uniqueName(base, laneIds);
          doc.apply(std::make_unique<AddLane>(machine, laneId));
          const std::vector<std::string> problems = syncLanes(doc, player.engine());
          session->editStatus = problems.empty() ? std::string() : problems.front();
          session->handoff.openLane(laneId);
        }
        ImGui::SameLine(0, 8 * scale);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##addlanefor", machines[static_cast<std::size_t>(session->addLaneFor)].c_str())) {
          for (int i = 0; i < static_cast<int>(machines.size()); i++) if (ImGui::Selectable(machines[static_cast<std::size_t>(i)].c_str(), i == session->addLaneFor)) session->addLaneFor = i;
          ImGui::EndCombo();
        }
      }
    }
  }
    }
    if (session->screen == Screen::Instrument) {   // the instrument under the cursor: voices, sleep, order
      const Instrument* current = nullptr;
      for (const Instrument& i : doc.project().instruments) if (i.id == session->instrument) current = &i;
      if (current) {
        const std::string name = current->id;
        int voices = current->graph.voices;
        bool never = current->graph.neverSleeps;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kText), "%s", name.c_str());
        if (!session->panel.open && app::textButton("Show parameters")) session->panel.open = true;   // (its close button hid the window)
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "voices");
        ImGui::SameLine();
        char text[16]; std::snprintf(text, sizeof text, "%d", voices);
        const int step = name == kMaster ? 0 : app::spinBox("##voices", text, 56 * scale);
        bool setup = false;
        if (step && voices + step >= 1 && voices + step <= 16) { voices += step; setup = true; }
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "tracks");
        if (current->noteColumns > 0) {   // (a meta machine has none)
          ImGui::SameLine();
          char tracks[16]; std::snprintf(tracks, sizeof tracks, "%d", current->noteColumns);
          const int more = app::spinBox("##tracks", tracks, 56 * scale);
          if (more && current->noteColumns + more >= 1 && current->noteColumns + more <= 8) {
            doc.apply(std::make_unique<SetNoteColumns>(name, current->noteColumns + more));
            const std::vector<std::string> problems = syncLanes(doc, player.engine());
            session->editStatus = problems.empty() ? std::string() : problems.front();
          }
        }
        bool sleeps = !never;
        if (app::checkbox("sleep when quiet", &sleeps)) { never = !sleeps; setup = true; }
        if (setup) {
          doc.apply(std::make_unique<SetInstrumentSetup>(name, voices, never));
          const std::vector<std::string> problems = syncGraphs(doc, player.engine());
          session->editStatus = problems.empty() ? std::string() : problems.front();
        }
        if (app::textButton("Move earlier")) doc.apply(std::make_unique<MoveInstrument>(name, -1));
        ImGui::SameLine(0, 8 * scale);
        if (app::textButton("Move later")) doc.apply(std::make_unique<MoveInstrument>(name, 1));
        if (name != kMaster) {   // rename, duplicate, remove (each refused with a message when something still uses the machine)
          std::vector<std::string> ids;
          for (const Instrument& i : doc.project().instruments) ids.push_back(i.id);
          const auto attempt = [&](std::unique_ptr<Edit> edit, const std::string& then) {
            try {
              doc.apply(std::move(edit));
              if (!then.empty()) session->instrument = then;
              const std::vector<std::string> problems = syncGraphs(doc, player.engine());
              const std::vector<std::string> laneProblems = syncLanes(doc, player.engine());
              session->editStatus = !problems.empty() ? problems.front() : !laneProblems.empty() ? laneProblems.front() : std::string();
            } catch (const std::exception& e) { session->editStatus = e.what(); }
          };
          if (!session->renamingMachine) std::snprintf(session->machineName, sizeof session->machineName, "%s", name.c_str());
          ImGui::SetNextItemWidth(-1);
          ImGui::InputText("##machinename", session->machineName, sizeof session->machineName);
          session->renamingMachine = ImGui::IsItemActive();
          if (ImGui::IsItemDeactivatedAfterEdit() && name != session->machineName) attempt(std::make_unique<RenameInstrument>(name, session->machineName), session->machineName);
          if (app::textButton("Duplicate")) { const std::string copy = app::uniqueName(name, ids); attempt(std::make_unique<DuplicateInstrument>(name, copy), copy); }
          ImGui::SameLine(0, 8 * scale);
          if (app::textButton("Remove")) {
            std::string next = kMaster;
            for (const Instrument& i : doc.project().instruments) if (i.id != name && i.id != kMaster) { next = i.id; break; }
            attempt(std::make_unique<RemoveInstrument>(name), next);
          }
        }
        {   // presets: snapshots of every parameter, recalled by a click or by command 00 in a pattern
          ImGui::Spacing();
          ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "presets");
          current = nullptr;   // (an edit above may have moved the list of machines)
          for (const Instrument& i : doc.project().instruments) if (i.id == session->instrument || i.id == name) current = &i;
          if (current) {
          const Json presets = current->raw.is_object() && current->raw.contains("presets") ? current->raw["presets"] : Json::array();
          bool selectedExists = false;
          for (const Json& p : presets) selectedExists |= p.value("id", -1) == session->selectedPreset;
          if (!selectedExists) session->selectedPreset = -1;
          for (const Json& p : presets) {
            const int id = p.value("id", 0);
            const std::string label = presetLabel(id) + "  " + p.value("name", std::string());
            if (app::textButton(label.c_str(), id == session->selectedPreset)) {
              session->selectedPreset = id;
              std::snprintf(session->presetName, sizeof session->presetName, "%s", p.value("name", std::string()).c_str());
              doc.apply(std::make_unique<RecallPreset>(name, id), false);
              syncParams(doc, player.engine());
              session->editStatus = "Recalled preset " + presetLabel(id) + ".";
            }
          }
          ImGui::SetNextItemWidth(-1);
          ImGui::InputTextWithHint("##presetname", "preset name", session->presetName, sizeof session->presetName);
          const auto liveValue = [&](const std::string& address) -> std::optional<double> {
            const ParamRegistry& registry = player.engine().registry();
            return registry.slotOf(address) >= 0 ? std::optional<double>(registry.get(address)) : std::nullopt;
          };
          const auto setPresets = [&](Json list, const char* what) {
            doc.apply(std::make_unique<SetPresets>(name, std::move(list), what));
            const std::vector<std::string> problems = syncLanes(doc, player.engine());
            session->editStatus = problems.empty() ? std::string() : problems.front();
          };
          try {
            if (app::textButton("Save")) {
              int id = 0;
              setPresets(presetsWithAdded(*current, session->presetName, capturePreset(*current, liveValue), id, capturePresetData(*current)), "Save preset");
              session->selectedPreset = id;
            }
            ImGui::SameLine(0, 8 * scale);
            if (app::textButton("Overwrite")) {
              if (session->selectedPreset < 0) session->editStatus = "Click a preset to select it first.";
              else setPresets(presetsWithValues(*current, session->selectedPreset, capturePreset(*current, liveValue), capturePresetData(*current)), "Overwrite preset");
            }
            if (app::textButton("Rename")) {
              if (session->selectedPreset < 0) session->editStatus = "Click a preset to select it first.";
              else setPresets(presetsWithName(*current, session->selectedPreset, session->presetName), "Rename preset");
            }
            ImGui::SameLine(0, 8 * scale);
            if (app::textButton("Delete")) {
              if (session->selectedPreset < 0) session->editStatus = "Click a preset to select it first.";
              else { setPresets(presetsWithout(*current, session->selectedPreset), "Remove preset"); session->selectedPreset = -1; }
            }
          } catch (const std::exception& e) { session->editStatus = e.what(); }
          }
        }
      }
    }
    ImGui::EndChild();
  }
  {   // the grip on the right edge: drag to change the width, all the way left closes it
    // It is a window of its own, straddling the edge, so that it can be picked up from a few pixels either side (a button inside the panel
    // would only reach the panel's own pixels, and the panels beside it would take the rest).
    const ImVec2 window = ImGui::GetWindowPos();
    const float height = ImGui::GetWindowHeight(), edge = window.x + ImGui::GetWindowWidth(), reach = 7 * scale;
    ImGui::SetNextWindowPos(ImVec2(edge - reach, window.y));
    ImGui::SetNextWindowSize(ImVec2(reach * 2, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags gripFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoScrollWithMouse;
    const bool gripOpen = ImGui::Begin("##browsergrip", nullptr, gripFlags);
    ImGui::PopStyleVar(2);
    if (gripOpen) {
      ImGui::InvisibleButton("##grip", ImVec2(reach * 2, height));
      const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
      if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      if (ImGui::IsItemActive()) {
        session->browserWidth += ImGui::GetIO().MouseDelta.x / scale;
        if (session->browserWidth < 90) { session->browserWidth = 260; session->showBrowser = false; }
        session->browserWidth = std::clamp(session->browserWidth, 160.0f, 600.0f);
      }
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) session->browserWidth = 260;
      // always visible, like the mixer's: a thin accent line the height of the panel, with a longer handle in the middle that thickens when hot
      const float lineX = std::floor(edge - 3 * scale) + 0.5f, mid = window.y + height * 0.5f, half = 28 * scale;
      ImDrawList* draw = ImGui::GetForegroundDrawList();
      draw->AddLine(ImVec2(lineX, window.y), ImVec2(lineX, window.y + height), app::kAccent, 1.0f);
      draw->AddLine(ImVec2(lineX, mid - half), ImVec2(lineX, mid + half), app::kAccent, hot ? 6.0f : 4.0f);
    }
    ImGui::End();
  }
  ImGui::EndChild();
  ImGui::SameLine(0, 0);
}

}  // namespace wax::tool

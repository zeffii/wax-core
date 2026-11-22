// The Config screen.
#include "wax_app/app_state.h"

namespace wax::tool {

// The Config screen: the settings, in groups as in the web app.
void drawConfigScreen(FrameContext& f) {
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
  // Config: the settings, in groups as in the web app; each change is kept at once.
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "Settings are kept on this computer (config.json in the preferences folder).");
  app::Settings& cfg = app::settings();
  bool changed = false;
  const auto group = [&](const char* title, const char* blurb) {
    ImGui::Spacing();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kAccent), "%s", title);
    ImGui::SameLine(0, 12 * scale);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "%s", blurb);
  };
  const auto colour = [&](const char* label, ImU32& value) {
    float rgb[3] = {(value & 255) / 255.0f, ((value >> 8) & 255) / 255.0f, ((value >> 16) & 255) / 255.0f};
    ImGui::PushID(label);
    if (ImGui::ColorEdit3("##c", rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
      value = IM_COL32(static_cast<int>(rgb[0] * 255 + 0.5f), static_cast<int>(rgb[1] * 255 + 0.5f), static_cast<int>(rgb[2] * 255 + 0.5f), 255);
      changed = true;
    }
    ImGui::PopID();
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
  };
  group("Look", "Colours. Applied at once.");
  ImGui::SetNextItemWidth(180 * scale);
  if (ImGui::BeginCombo("Colour theme", app::themeName(app::themeIndex()), ImGuiComboFlags_HeightLarge)) {
    for (int i = 0; i < app::themeCount(); i++) if (ImGui::Selectable(app::themeName(i), i == app::themeIndex())) { app::setTheme(i); saveTheme(); }
    ImGui::EndCombo();
  }
  group("Audio", "The sound card's buffer: smaller is quicker to respond, larger is safer against crackle.");
  if (session->audio) {
    static const int sizes[] = {0, 64, 128, 256, 512, 1024, 2048, 4096};
    const int now = session->audio->bufferFrames();
    char label[64];
    std::snprintf(label, sizeof label, "buffer %d (%.1f ms)", now, 1000.0 * now / session->audio->sampleRate());
    ImGui::SetNextItemWidth(260 * scale);
    if (ImGui::BeginCombo("Audio buffer", label)) {
      for (const int size : sizes) {
        char item[32];
        if (size == 0) std::snprintf(item, sizeof item, "card's own");
        else std::snprintf(item, sizeof item, "%d (%.1f ms)", size, 1000.0 * size / session->audio->sampleRate());
        if (ImGui::Selectable(item, size == now)) {
          try { session->openAudio(options, size); }
          catch (const std::exception& e) { session->audioName = std::string("no sound: ") + e.what(); }
        }
      }
      ImGui::EndCombo();
    }
  }
  group("Engine", "How many cores render the sound. The sound is the same on any number.");
  if (session->audio) {
    const int available = std::max(1u, std::thread::hardware_concurrency());
    const int inUse = session->player->engine().runtime().threads();
    const ImVec4 dim = ImGui::ColorConvertU32ToFloat4(app::kDimText);
    ImGui::TextColored(dim, "cores available: %d   in use: %d", available, inUse);
    ImGui::SetNextItemWidth(220 * scale);
    ImGui::SliderInt("Render threads", &cfg.threads, 1, available);
    if (ImGui::IsItemDeactivatedAfterEdit()) {   // (applied when let go: it restarts the sound for a moment)
      changed = true;
      if (cfg.threads != inUse) session->setThreads(options, cfg.threads);
    }
    ImGui::TextColored(dim, "it may not be in your best interest to use all cores");
  }
  group("Playing", "How the screens behave while the song runs.");
  if (app::checkbox("Follow mode (the grids scroll with the playing row)", &follow)) { cfg.follow = follow; changed = true; }
  group("Graph", "Wire colours by kind of signal, and dashed control wires.");
  colour("Wire colour: audio", cfg.wireAudio);
  colour("Wire colour: control", cfg.wireControl);
  colour("Wire colour: stereo", cfg.wireStereo);
  colour("Wire colour: parameter", cfg.wireParam);
  if (app::checkbox("Control wires are dashed", &cfg.wireDashed)) changed = true;
  if (app::checkbox("Graph: faint grid of dots", &cfg.graphGrid)) changed = true;
  ImGui::SetNextItemWidth(180 * scale);
  if (ImGui::SliderInt("Graph: grid spacing (pixels)", &cfg.graphGridSize, 8, 96)) changed = true;
  ImGui::SetNextItemWidth(180 * scale);
  if (ImGui::SliderInt("Graph: brighter dot every (0: none)", &cfg.graphGridMajor, 0, 64)) changed = true;
  if (app::checkbox("Graph: nodes snap to the grid (Ctrl while dragging moves freely)", &cfg.graphSnap)) changed = true;
  ImGui::SetNextItemWidth(180 * scale);
  if (ImGui::SliderInt("Graph: wires between close nodes go straight below (pixels; 0: always curved)", &cfg.wireStraight, 0, 400)) changed = true;
  if (app::checkbox("Graph: dragging a node far out of its chain takes it out", &cfg.graphPullOut)) changed = true;
  if (app::checkbox("Parameter view: hide command parameters", &cfg.pvHideCc)) changed = true;
  group("Song screen", "The sequence editor.");
  if (app::checkbox("Song overview (a strip at the right showing the whole song scaled to fit: click or drag to scroll)", &cfg.seqOverview)) changed = true;
  if (app::checkbox("Pattern minimaps (each cell shows a small picture of its pattern)", &cfg.minimapOn)) changed = true;
  ImGui::BeginDisabled(!cfg.minimapOn);
  ImGui::Indent(16 * scale);
  if (app::checkbox("Note lengths (a thin line from each note to the next)", &cfg.minimapLengths)) changed = true;
  if (app::checkbox("Note off and kill (cyan for ===, magenta for ^^^)", &cfg.minimapCaps)) changed = true;
  if (app::checkbox("Parameters (a narrow column each at the right edge)", &cfg.minimapParams)) changed = true;
  ImGui::Unindent(16 * scale);
  ImGui::EndDisabled();
  group("Editing", "How the cursor moves.");
  if (app::checkbox("Sequence editor: show pattern names (off: just the two-digit number; H toggles)", &cfg.seqNames)) changed = true;
  if (app::checkbox("Up and Down wrap round the first and last row", &cfg.rowWrap)) changed = true;
  if (app::checkbox("Left and Right wrap round the first and last lane (All lanes view)", &cfg.laneWrap)) changed = true;
  if (app::fileAssociationSupported()) {
    group("Files", "Double-clicking a .wax file in the file manager.");
    static std::string associationNote;   // what the last press of the button did
    if (app::isOpenerOfWaxFiles()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), ".wax files open in this Wax when double-clicked.");
    else if (app::textButton("Open .wax files with this Wax")) associationNote = app::registerAsOpenerOfWaxFiles();
    if (!associationNote.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "%s", associationNote.c_str());
  }
  group("Backup", "Snapshots of the song in the preferences folder; File > Backups opens one.");
  if (app::checkbox("Automatic backups", &cfg.backupOn)) changed = true;
  ImGui::SetNextItemWidth(220 * scale);
  if (ImGui::SliderInt("Back up every (minutes)", &cfg.backupMinutes, 1, 60)) changed = true;
  ImGui::SetNextItemWidth(220 * scale);
  if (ImGui::SliderInt("Snapshots kept", &cfg.backupKeep, 5, 200)) changed = true;
  group("Sample", "The Sample screen.");
  ImGui::SetNextItemWidth(220 * scale);
  if (ImGui::SliderInt("Auto-slice: sensitivity (%)", &cfg.sliceSensitivity, 0, 100)) changed = true;
  ImGui::SetNextItemWidth(220 * scale);
  if (ImGui::SliderInt("Auto-slice: shortest gap (ms)", &cfg.sliceMinGap, 5, 500)) changed = true;
  if (app::checkbox("Auto-slice: replace the markers in range", &cfg.sliceReplace)) changed = true;
  if (app::checkbox("Auto-slice: operate on the whole sample", &cfg.sliceWhole)) changed = true;
  if (app::checkbox("Waveform: hide the loop region", &cfg.waveHideLoop)) changed = true;
  if (app::checkbox("Waveform: hide the selection", &cfg.waveHideSel)) changed = true;
  if (app::checkbox("Waveform: hide the sustain loop region", &cfg.waveHideSus)) changed = true;
  {
    static const struct { const char* id; const char* label; } modes[] = {{"both", "both (relative and absolute)"}, {"relative", "relative: 0000 to FFFF through the sample"}, {"absolute", "absolute: the frame number"}};
    const char* shown = modes[0].label;
    for (const auto& mode : modes) if (cfg.hexMode == mode.id) shown = mode.label;
    ImGui::SetNextItemWidth(330 * scale);
    if (ImGui::BeginCombo("Slice markers: hex value", shown)) {
      for (const auto& mode : modes) if (ImGui::Selectable(mode.label, cfg.hexMode == mode.id)) { cfg.hexMode = mode.id; changed = true; }
      ImGui::EndCombo();
    }
  }
  ImGui::SetNextItemWidth(220 * scale);
  if (ImGui::SliderInt("Sample browser: ask before reading files over (MB)", &cfg.sampleAskMB, 1, 2000)) changed = true;
  ImGui::Spacing();
  if (app::textButton("Reset to defaults")) { app::resetSettings(); follow = cfg.follow; changed = true; }
  if (changed) app::saveSettings();
}

}  // namespace wax::tool

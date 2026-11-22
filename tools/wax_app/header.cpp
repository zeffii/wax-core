// The header and the rail.
#include "wax_app/app_state.h"

namespace wax::tool {

// The header: the File menu, the transport, the project name, the song settings and the meters, with the line under them. Returns where the
// song is (the tick to show as the playhead, and the beat and bar lengths in ticks).
HeaderState drawHeader(Session* session, bool& follow, bool& requestSave, float& meterL, float& meterR) {
  Player& player = *session->player;
  Document& doc = *session->doc;
  const Song& song = doc.project().song;
  {   // the File menu, then the transport, the project name and the song settings (the web app's header LCD) all on one line
    const float hs = ImGui::GetStyle().FontScaleDpi;
    ImGui::AlignTextToFramePadding();   // the words in this row sit on the same baseline as the text inside the boxes
    const float lineY = ImGui::GetCursorPosY();
    {   // the small Wax mark at the top left, in the rail's width; the File button beside it starts right of the rail's line
      const float width = 34 * hs, aspect = 820.0f / 2320.0f, frame = ImGui::GetFrameHeight();
      const ImVec2 at = ImGui::GetCursorScreenPos();
      app::drawIcon("wax-logo", ImVec2(at.x + 3 * hs, at.y + (frame - width * aspect) / 2), width, aspect, app::kText, ImGui::GetWindowDrawList());
      ImGui::Dummy(ImVec2(40 * hs + ImGui::GetStyle().ItemSpacing.x / 2 + kFileGap * hs, frame));   // (File sits midway between the rail's line and the Play button: the same gap each side)
      ImGui::SameLine(0, 0);
      ImGui::SetCursorPosY(lineY);
    }
    const float iconsY = lineY + ImGui::GetStyle().FramePadding.y;   // (a text button does not apply the baseline offset itself)
    ImGui::SetCursorPosY(iconsY);
    if (gIconCentre > 0) ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, gIconCentre - ImGui::GetTextLineHeight() / 2));
    if (app::textButton("File")) ImGui::OpenPopup("filemenu");
    gFileX = ImGui::GetItemRectMin().x;
    ImGui::SameLine(0, 0); ImGui::SetCursorPosY(lineY);
    if (ImGui::BeginPopup("filemenu")) {
      // Rows in the normal text colour (a grey row would look unavailable), the shortcuts dim and right-aligned.
      const float menuWidth = app::menuWidth({{"Open...", "Ctrl+O"}, {"New", ""}, {"New From Existing", ""}, {"Save", "Ctrl+S"}, {"Save as...", "Ctrl+Shift+S"}, {"Export WAV...", ""}, {"Backups...", ""}, {"Recent", ""}});
      if (app::menuRow("Open...", "Ctrl+O", menuWidth)) { gRequest = DialogRequest::Open; ImGui::CloseCurrentPopup(); }
      if (app::menuRow("New", "", menuWidth)) { gRequest = DialogRequest::New; ImGui::CloseCurrentPopup(); }
      if (ImGui::BeginMenu("New From Existing")) {   // keep the instruments and samples, clear part of the song (as the web app's)
        if (ImGui::MenuItem("Clear Arrangement")) { gRequest = DialogRequest::ClearArrangement; ImGui::CloseCurrentPopup(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove every pattern placement; the patterns stay");
        if (ImGui::MenuItem("Clear Patterns and Arrangement")) { gRequest = DialogRequest::ClearPatterns; ImGui::CloseCurrentPopup(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove every pattern and the arrangement; instruments and samples stay");
        ImGui::EndMenu();
      }
      if (app::menuRow("Save", "Ctrl+S", menuWidth)) { requestSave = true; ImGui::CloseCurrentPopup(); }
      if (app::menuRow("Save as...", "Ctrl+Shift+S", menuWidth)) { gRequest = DialogRequest::SaveAs; ImGui::CloseCurrentPopup(); }
      if (app::menuRow("Export WAV...", "", menuWidth)) { gRequest = DialogRequest::Export; ImGui::CloseCurrentPopup(); }
      if (app::menuRow("Backups...", "", menuWidth)) { ImGui::CloseCurrentPopup(); session->backupsOpen = true; }
      if (!app::settings().recentFiles.empty() && ImGui::BeginMenu("Recent")) {   // the recent songs, a submenu; one that has moved shows dim (opening it only warns)
        for (const std::string& file : app::settings().recentFiles) {
          std::error_code ignored;
          const bool there = std::filesystem::exists(pathFromUtf8(file), ignored);
          const std::string name = pathFromUtf8(file).filename().generic_string();
          ImGui::PushID(file.c_str());
          if (!there) ImGui::PushStyleColor(ImGuiCol_Text, app::kDimText);
          if (ImGui::MenuItem((there ? name : name + "  (moved?)").c_str())) { gOpenRecent = file; ImGui::CloseCurrentPopup(); }
          if (!there) ImGui::PopStyleColor();
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", file.c_str());
          ImGui::PopID();
        }
        ImGui::EndMenu();
      }
      ImGui::EndPopup();
    }
    ImGui::SameLine(0, kFileGap * hs);
    ImGui::SetCursorPosY(iconsY);   // (the icons keep their own height whatever File's was)
  }
  {   // automatic backups: a snapshot every few minutes while the song has changes
    const std::time_t now = std::time(nullptr);
    const app::Settings& cfg = app::settings();
    if (session->backupAt == 0) { session->backupAt = now; session->backupRevision = doc.revision(); }
    if (cfg.backupOn && doc.revision() != session->backupRevision && now - session->backupAt >= cfg.backupMinutes * 60 && !backupFolder().empty()) {
      try {
        writeBackup(backupFolder(), doc.project().song.name, serializeProject(doc.project()), cfg.backupKeep, now);
        session->backupRevision = doc.revision();
      } catch (const std::exception& e) {
        session->editStatus = std::string("backup failed: ") + e.what();
      }
      session->backupAt = now;
    }
  }
  if (session->backupsOpen) { ImGui::OpenPopup("backups"); session->backupsOpen = false; }
  if (ImGui::BeginPopup("backups")) {   // the snapshots, newest first; one opens as an untitled song (saving it asks for a file)
    const std::vector<BackupInfo> list = listBackups(backupFolder());
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "Automatic backups (every %d min while the song changes, %d kept)", app::settings().backupMinutes, app::settings().backupKeep);
    if (list.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "none yet");
    int shown = 0;
    for (const BackupInfo& info : list) {
      if (++shown > 20) break;
      char when[32];
      std::tm parts{};
#ifdef _WIN32
      localtime_s(&parts, &info.time);
#else
      localtime_r(&info.time, &parts);
#endif
      std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &parts);
      char label[160];
      std::snprintf(label, sizeof label, "%s   %s   %.0f KB###%s", when, info.name.c_str(), info.size / 1024.0, info.file.string().c_str());
      if (app::textButton(label)) { gOpenBackup = info.file.string(); ImGui::CloseCurrentPopup(); }
    }
    ImGui::EndPopup();
  }
  // The transport.
  const bool playing = player.playing();
  if (app::iconButton("play", playing ? "Stop (F8)" : session->pausedAt >= 0 ? "Carry on from the pause (F5)" : "Play (F5)", playing, 20 * app::kTickScale, true)) {
    if (playing) { player.ring().push({LiveCommand::Stop, 0, 1}); session->pausedAt = -1; }
    else { player.ring().push({LiveCommand::Play, std::max(0, session->pausedAt), 1}); session->pausedAt = -1; }
  }
  gIconCentre = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) / 2;
  const float transportTop = ImGui::GetItemRectMin().y;
  ImGui::SameLine();
  ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, transportTop));   // (the whole row of icons at Play's height)
  if (app::iconButton("pause", "Pause (Pause key): stop here; Play (or Pause again) carries on from the same place", session->pausedAt >= 0, 20 * app::kTickScale, true)) {
    if (playing) { session->pausedAt = player.tick(); player.ring().push({LiveCommand::Stop, 0, 1}); }
    else if (session->pausedAt >= 0) { player.ring().push({LiveCommand::Play, session->pausedAt, 1}); session->pausedAt = -1; }
  }
  ImGui::SameLine();
  ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, transportTop));   // (the whole row of icons at Play's height)
  if (app::iconButton("follow", "Follow the song (F9): the grids scroll with the playing row", follow, 20 * app::kTickScale, true)) follow = !follow;
  ImGui::SameLine();
  ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, transportTop));   // (the whole row of icons at Play's height)
  {
    const LoopRange loop = loopOf(song);
    const bool hasLoop = song.raw.is_object() && song.raw.contains("loop") && song.raw["loop"].is_object();
    if (app::iconButton("loopbtn", hasLoop ? (loop.on ? "Loop on (Ctrl+L): Ctrl+B sets its start, Ctrl+E its end" : "Loop off (Ctrl+L)") : "No loop: Ctrl+B sets its start, Ctrl+E its end", hasLoop && loop.on, 20 * app::kTickScale, true) && hasLoop) {
      doc.apply(std::make_unique<SetLoop>(loopToggled(song), loop.on ? "Loop off" : "Loop on"));
      syncLoop(doc, player.engine());
    }
  }
  ImGui::SameLine(0, 24 * ImGui::GetStyle().FontScaleDpi);
  const double tickSeconds = 60.0 / (std::max(1.0, song.bpm) * std::max(1, song.ticksPerBeat));
  session->heard.update(playing, player.tick(), ImGui::GetTime(), session->audio ? session->audio->latencySeconds() : 0.0, tickSeconds);
  app::keepPins(session->panel, session->doc->project());   // the pinned parameter windows are remembered between songs and sessions
  const int tick = playing ? session->heard.tick : player.tick();
  const int perBeat = std::max(1, song.ticksPerBeat), perBar = perBeat * std::max(1, song.beatsPerBar);
  {   // the project name: it is the file name when you save
    if (!session->nameFocused) std::snprintf(session->projectName, sizeof session->projectName, "%s", song.name.c_str());
    ImGui::SetNextItemWidth(150 * ImGui::GetStyle().FontScaleDpi);
    ImGui::InputTextWithHint("##projectname", "untitled", session->projectName, sizeof session->projectName);
    session->nameFocused = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit() && session->projectName != song.name) doc.apply(std::make_unique<SetSongName>(session->projectName));
    ImGui::SameLine(0, 16 * ImGui::GetStyle().FontScaleDpi);
  }

  // The song settings: bpm, swing, tpb and bar are spin boxes; len and time are read out.
  {
    const float hs = ImGui::GetStyle().FontScaleDpi;
    // bpm, swing, tpb and bar are set seldom, so each is a spin box (one fixed step per arrow click, one undo per click).
    const ImU32 dim = app::kDimText;
    auto stepper = [&](const char* id, const char* label, const char* text, int direction) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(dim), "%s", label);
      ImGui::SameLine();
      const int clicked = app::spinBox(id, text, 58 * hs);
      ImGui::SameLine(0, 16 * hs);
      return clicked * direction;
    };
    const NodeType* tempo = builtinNodes().find("tempo");
    auto tempoValue = [&](const char* param) {
      const ParamSpec* spec = nullptr;
      for (const ParamSpec& p : tempo->params) if (p.name == param) spec = &p;
      const std::string address = std::string("master.m.") + param;
      return player.engine().registry().slotOf(address) >= 0 ? player.engine().registry().get(address) : spec->def;
    };
    auto setTempo = [&](const char* param, double value) {
      doc.apply(std::make_unique<SetParam>("master", "m", param, value), false);
      syncParams(doc, player.engine());
    };
    char text[32];
    const double bpm = tempoValue("bpm"), swing = tempoValue("swing"), tpb = tempoValue("tpb");
    std::snprintf(text, sizeof text, "%5.1f", bpm);
    if (const int d = stepper("lcd_bpm", "bpm", text, 1)) setTempo("bpm", std::clamp(bpm + d, 20.0, 400.0));
    std::snprintf(text, sizeof text, "%2d", static_cast<int>(swing));
    if (const int d = stepper("lcd_swing", "swing", text, 1)) setTempo("swing", std::clamp(swing + d, 50.0, 75.0));
    std::snprintf(text, sizeof text, "%2d", static_cast<int>(tpb));
    if (const int d = stepper("lcd_tpb", "tpb", text, 1)) setTempo("tpb", std::clamp(tpb + d, 1.0, 24.0));
    std::snprintf(text, sizeof text, "%2d", song.beatsPerBar);
    if (const int d = stepper("lcd_bar", "bar", text, 1)) doc.apply(std::make_unique<SetBeatsPerBar>(std::clamp(song.beatsPerBar + d, 1, 16)), false);
    const int seconds = static_cast<int>(song.length * 60.0 / std::max(1, song.ticksPerBeat) / std::max(20.0, song.bpm));
    std::snprintf(text, sizeof text, "%d", song.length);
    if (const int d = stepper("lcd_len", "len", text, 16)) {   // a row (16 ticks) a click
      if (song.length + d >= 16 && song.length + d <= 65536) {
        doc.apply(std::make_unique<SetSongLength>(song.length + d), false);
        syncLanes(doc, player.engine());
        syncLoop(doc, player.engine());
      }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "time %d:%02d", seconds / 60, seconds % 60);
  }
  ImGui::SameLine(ImGui::GetWindowWidth() - 260 * ImGui::GetStyle().FontScaleDpi);
  meterL = std::max(player.levelLeft(), meterL * 0.9f);   // falls back a little each frame
  meterR = std::max(player.levelRight(), meterR * 0.9f);
  ImGui::BeginGroup();
  drawMeter("##l", meterL, ImVec2(200 * ImGui::GetStyle().FontScaleDpi, 8));
  drawMeter("##r", meterR, ImVec2(200 * ImGui::GetStyle().FontScaleDpi, 8));
  ImGui::EndGroup();
  {   // the header's line starts at the rail's line (which runs up to the top of the window) and goes on to the right edge
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float from = railLineX(at.x, scale), y = std::floor(at.y) + 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(from, y), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), y), ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
    ImGui::Dummy(ImVec2(0, 1));
  }
  return {playing, tick, perBeat, perBar};
}

// The rail down the left: the screens' icons, and the shortcut sheet and settings at the foot; with the thin line to the right of it.
void drawRail(Session* session, float scale, float below) {
  ImGui::BeginChild("rail", ImVec2(40 * scale, below), false);
  {
    const struct { const char* label; Screen screen; } entries[] = {{"Song", Screen::Song}, {"Pattern", Screen::Pattern}, {"Instrument", Screen::Instrument}, {"Sample", Screen::Sample}, {"Info", Screen::Info}, {"Config", Screen::Config}};
    const struct { const char* icon; const char* tip; Screen screen; } icons[] = {
        {"arrange", "Song: the arrangement (F1)", Screen::Song}, {"patterns", "Pattern (F2)", Screen::Pattern},
        {"instruments", "Instrument (F3)", Screen::Instrument}, {"samples", "Sample (F4)", Screen::Sample}, {"info", "Song info (F10)", Screen::Info}};
    const float centred = (40 - 24) * 0.5f * scale;   // the icons sit centred under the logo
    for (const auto& entry : icons) { ImGui::SetCursorPosX(centred); if (app::iconButton(entry.icon, entry.tip, session->screen == entry.screen, 24)) session->screen = entry.screen; }
    // the other domain, at the bottom of the rail (above the mixer when it shows): the shortcut sheet and the settings
    const float iconStep = 24 * scale + ImGui::GetStyle().ItemSpacing.y;
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - 2 * iconStep));
    ImGui::SetCursorPosX(centred);
    if (app::iconButton("keys-toggle", "Shortcuts: the keys of this screen, beside it", session->showKeys, 24)) session->showKeys = !session->showKeys;
    ImGui::SetCursorPosX(centred);
    if (app::iconButton("config", "Config (F11)", session->screen == Screen::Config, 24)) session->screen = Screen::Config;
    (void)entries;
  }
  ImGui::EndChild();
  {   // a thin line between the rail and what is to the right of it
    const ImVec2 railMin = ImGui::GetItemRectMin(), railMax = ImGui::GetItemRectMax();
    const float x = railLineX(railMin.x, scale);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, ImGui::GetWindowPos().y), ImVec2(x, railMax.y), app::lineAlpha(255), 1.0f);   // (from the top of the window)
  }
}

}  // namespace wax::tool

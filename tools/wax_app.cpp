// wax-app [song.wax] [--rate hz] [--threads n] [--midi n] [--null]
//         [--screenshot out.bmp [--frames n] [--play-from tick]]
// The window: the transport, the lanes of the song as pattern tables that follow the playhead, level meters, and
// keyjazz from the computer keyboard or a MIDI controller. Drop a .wax file on the window to open it.
// Without a sound card (or with --null) it still runs; --screenshot draws some frames, saves the last, and quits,
// which is how the window is looked at on a machine without a screen (SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software).
#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <fstream>
#include <map>
#include <sstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "app/sequence_view.h"
#include "app/envelope_window.h"
#include "app/config.h"
#include "app/sample_browser.h"
#include "app/sample_view.h"
#include "app/curve_view.h"
#include "app/dx_window.h"
#include "app/graph_view.h"
#include "app/machine_tree.h"
#include "app/machines.h"
#include "app/pattern_colour.h"
#include "app/pattern_list.h"
#include "app/shortcut_sheet.h"
#include "app/widgets.h"
#include "app/icons.h"
#include "app/mixer_view.h"
#include "app/param_panel.h"
#include "app/pattern_view.h"
#include "app/theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui.h"
#include "edit/cell_editor.h"
#include "edit/document.h"
#include "edit/presets.h"
#include "app/column_info.h"
#include "edit/sync.h"
#include "seq/loop.h"
#include "live/audio_out.h"
#include "project/save.h"
#include "live/player.h"
#include "engine/offline.h"
#include "project/project.h"
#include "io/wav.h"
#include "project/samples.h"

namespace {

using namespace wax;

// File dialogs answer on another thread; the window picks the answer up between frames.
enum class DialogKind { Open, Save, Export };
struct DialogAnswer {
  std::mutex mutex;
  std::string path;
  DialogKind kind = DialogKind::Open;
  bool ready = false;
};
DialogAnswer gAnswer;
enum class DialogRequest { None, Open, SaveAs, Export, New };
DialogRequest gRequest = DialogRequest::None;   // set by the keys, carried out by the main loop (it has the window)

void SDLCALL dialogDone(void* userdata, const char* const* files, int) {
  if (!files || !files[0]) return;   // cancelled or failed
  std::lock_guard<std::mutex> lock(gAnswer.mutex);
  gAnswer.path = files[0];
  gAnswer.kind = userdata ? *static_cast<DialogKind*>(userdata) : DialogKind::Open;
  gAnswer.ready = true;
}

struct Options {
  std::string path, screenshot, press, clicks, envelope, dxnode, screen, theme;   // clicks: "x,y;x,y" mouse clicks, one every 6 frames; envelope: "instrument.node" opens its envelope window   // press: keys to press one after another (for checking the window without a keyboard)
  double rate = 48000;
  int threads = 1, midiPort = 0, frames = 3, playFrom = -1, buffer = 0;
  bool useNull = false;
  bool splashForced = false;
  bool splash = true;   // the Wax logo for a moment at start (not when taking a screenshot, unless --splash is given)
};

// One song, loaded: the project, the engine that plays it, the sound card and the MIDI input.
enum class Screen { Song, Pattern, Instrument, Sample, Info, Config };

struct Session {
  Project project;
  std::unique_ptr<Player> player;
  std::unique_ptr<AudioOut> audio;
  std::unique_ptr<RtMidiIn> midi;
  std::string midiName = "none";
  std::string keyjazzInstrument;
  app::SampleView sampleView;   // the Sample screen: the sample chosen, its selection, the view of its waveform
  app::SampleBrowserState sampleBrowser;   // the Samples tab: the folder shown and the file picked
  app::CurveView curveView;   // the lane curves drawn over the Song screen: the one each lane shows, what is dragged
  std::vector<std::string> playable;                                       // the machines a keyboard can play
  std::map<std::string, std::vector<std::vector<std::string>>> rows;      // lane id -> flattened rows
  std::string audioName;
  bool pickedTab = false;   // the first lane that is not the master is shown at first
  std::unique_ptr<Document> doc;         // the song as edited: every change goes through it, so it can be undone
  std::unique_ptr<CellEditor> editor;
  bool edit = false;                     // Enter: the keyboard types into cells
  int rowsRevision = -1;                 // the revision of `doc` that `rows` shows
  bool scrollToCursor = false;
  std::string editStatus;
  app::ParamPanelState panel;
  app::MixerState mixer;
  app::EnvelopeWindow envelopes;
  app::DxWindow dx;
  app::GraphViewState graphView;
  app::PatternListState patternList;     // the Song screen's pattern list
  char laneName[40] = {0};               // the rename box on the Song screen
  std::string laneNameFor;               // the lane that text belongs to
  int silenceTicks = 128;                // the length of silence Insert puts in
  bool wantTree = false;                 // open the machine tree (Shift+I, or the Instrument screen's add button)
  bool showKeys = true;                  // the shortcut sheet beside the Pattern grid
  std::future<std::string> exporting;     // a render to a WAV file in the background: its answer is the message to show
  std::vector<char> infoText = std::vector<char>(65536, 0);   // the Info screen's text while it is being edited (up to 64 KB)
  bool infoFocused = false;
  int pausedAt = -1;                     // the tick the song was paused at (-1: not paused)
  char projectName[64] = {0};            // the header's project name box
  bool nameFocused = false;
  bool showBrowser = true;               // the browser down the left of every screen (Alt+B)
  float browserWidth = 260;              // in web pixels
  float browserOptions = 0;              // height of the options under the lists in web pixels (0: half the panel)
  Screen browserScreen = Screen::Song;   // the screen the browser last matched its tab to
  int browserTab = 0;                    // 0 Patterns, 1 Machines, 2 Samples
  bool clonePatterns = true, cloneCurves = true;   // what Clone with machine brings along
  int addLaneFor = 0;                     // the machine chosen under "Add lane for"
  int selectedPreset = -1;                // the Instrument screen's preset list
  char presetName[48] = {};
  char machineName[64] = {};              // the Instrument screen's rename box
  bool renamingMachine = false;
  bool showMixer = true;                 // the mixer under the Song and Pattern screens (the status bar's toggle)
  Screen screen = Screen::Pattern;       // the screen shown (the rail at the left)
  std::string instrument;                // the instrument shown on the Instrument screen
  std::string wantLane;                  // a lane the arrangement asked for; its tab opens next frame
  int wantTick = -1;                     // and the cursor goes here
  std::string path;                      // the file this song was opened from, where Ctrl+S saves it

  Session(const std::string& filePath, const Options& options) : Session(loadProject(filePath), filePath, options) {}
  Session(Project loaded, const std::string& filePath, const Options& options) : project(std::move(loaded)), path(filePath) {
    for (const Instrument& inst : project.instruments) if (inst.id != kMaster && !inst.isMeta) playable.push_back(inst.id);
    keyjazzInstrument = playable.empty() ? std::string() : playable.front();
    player = std::make_unique<Player>(project, options.rate, options.threads, keyjazzInstrument, true);
    doc = std::make_unique<Document>(project);
    editor = std::make_unique<CellEditor>(*doc);
    try {
      openAudio(options, options.buffer);
    } catch (const std::exception& e) {
      audioName = std::string("no sound: ") + e.what();
    }
    try {
      midi = std::make_unique<RtMidiIn>();
      if (midi->getPortCount() > static_cast<unsigned>(options.midiPort)) {
        midi->openPort(options.midiPort);
        midi->setCallback(&midiCallback, player.get());
        midiName = midi->getPortName(options.midiPort);
      }
    } catch (const RtMidiError&) {
      midi.reset();
    }
  }
  // (Re)opens the sound card with a buffer of this many frames (0: the card's own choice). Notes in the air are let go.
  void openAudio(const Options& options, int bufferFrames) {
    audio.reset();
    player->ring().push({LiveCommand::AllOff, 0, 0});
    audio = std::make_unique<AudioOut>(*player, options.rate, options.useNull, bufferFrames);
    audioName = audio->deviceName() + " (" + audio->backendName() + ")";
    audio->start();
  }
  ~Session() {
    if (midi) midi->closePort();   // the callback must not outlive the player
    midi.reset();
    audio.reset();
  }
};

// The computer keyboard as a piano: two rows, the lower from C of the octave, the upper an octave higher.
const ImGuiKey kPianoKeys[] = {
    ImGuiKey_Z, ImGuiKey_S, ImGuiKey_X, ImGuiKey_D, ImGuiKey_C, ImGuiKey_V, ImGuiKey_G, ImGuiKey_B, ImGuiKey_H, ImGuiKey_N,
    ImGuiKey_J, ImGuiKey_M, ImGuiKey_Comma,
    ImGuiKey_Q, ImGuiKey_2, ImGuiKey_W, ImGuiKey_3, ImGuiKey_E, ImGuiKey_R, ImGuiKey_5, ImGuiKey_T, ImGuiKey_6, ImGuiKey_Y,
    ImGuiKey_7, ImGuiKey_U, ImGuiKey_I};

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

// A project with nothing in it but the master lane: the master's own machine, no samples, patterns or arrangement.
Project emptyProject(const Project& from) {
  Json file = Json::parse(serializeProject(from));
  file["song"]["name"] = "untitled";
  file["song"]["length"] = 256;
  file["song"].erase("loop");
  file["samples"] = Json::object();
  Json instruments = Json::object();
  instruments[std::string(kMaster)] = file["instruments"][std::string(kMaster)];
  file["instruments"] = instruments;
  Json lanes = Json::array();
  for (Json lane : file["lanes"]) {
    if (lane.value("id", std::string()) != kMaster) continue;
    lane["patterns"] = Json::object();
    lane["placements"] = Json::array();
    lane.erase("mutes"); lane.erase("offs");
    lanes.push_back(lane);
  }
  file["lanes"] = lanes;
  return parseProject(file.dump());
}

void drawFrame(Session* session, const Options& options, const std::string& status, float& meterL, float& meterR, std::map<ImGuiKey, int>& held,
               int& octave, std::string& selectedLane, bool& follow, int& playFromRequest) {
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  ImGui::Begin("wax", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

  if (!session) {
    ImGui::TextUnformatted("Drop a .wax file on the window to open it.");
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

  {   // the File menu, then the transport, the project name and the song settings (the web app's header LCD) all on one line
    const float hs = ImGui::GetStyle().FontScaleDpi;
    ImGui::AlignTextToFramePadding();   // the words in this row sit on the same baseline as the text inside the boxes
    const float lineY = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(lineY + ImGui::GetStyle().FramePadding.y);   // (a text button does not apply the baseline offset itself)
    if (app::textButton("File")) ImGui::OpenPopup("filemenu");
    ImGui::SameLine(0, 0); ImGui::SetCursorPosY(lineY);
    if (ImGui::BeginPopup("filemenu")) {
      if (app::textButton("Open...  Ctrl+O")) { gRequest = DialogRequest::Open; ImGui::CloseCurrentPopup(); }
      if (app::textButton("New")) { gRequest = DialogRequest::New; ImGui::CloseCurrentPopup(); }
      if (app::textButton("Save  Ctrl+S")) { requestSave = true; ImGui::CloseCurrentPopup(); }
      if (app::textButton("Save as...  Ctrl+Shift+S")) { gRequest = DialogRequest::SaveAs; ImGui::CloseCurrentPopup(); }
      if (app::textButton("Export WAV...")) { gRequest = DialogRequest::Export; ImGui::CloseCurrentPopup(); }
      ImGui::EndPopup();
    }
    ImGui::SameLine(0, 18 * hs);
  }
  // The transport.
  const bool playing = player.playing();
  if (app::iconButton("play", playing ? "Stop (F8)" : session->pausedAt >= 0 ? "Carry on from the pause (F5)" : "Play (F5)", playing, 20)) {
    if (playing) { player.ring().push({LiveCommand::Stop, 0, 1}); session->pausedAt = -1; }
    else { player.ring().push({LiveCommand::Play, std::max(0, session->pausedAt), 1}); session->pausedAt = -1; }
  }
  ImGui::SameLine();
  if (app::iconButton("pause", "Pause (Pause key): stop here; Play (or Pause again) carries on from the same place", session->pausedAt >= 0, 20)) {
    if (playing) { session->pausedAt = player.tick(); player.ring().push({LiveCommand::Stop, 0, 1}); }
    else if (session->pausedAt >= 0) { player.ring().push({LiveCommand::Play, session->pausedAt, 1}); session->pausedAt = -1; }
  }
  ImGui::SameLine();
  if (app::iconButton("follow", "Follow the song (F9): the grids scroll with the playing row", follow, 20)) follow = !follow;
  ImGui::SameLine();
  {
    const LoopRange loop = loopOf(song);
    const bool hasLoop = song.raw.is_object() && song.raw.contains("loop") && song.raw["loop"].is_object();
    if (app::iconButton("loopbtn", hasLoop ? (loop.on ? "Loop on (Ctrl+L): Ctrl+B sets its start, Ctrl+E its end" : "Loop off (Ctrl+L)") : "No loop: Ctrl+B sets its start, Ctrl+E its end", hasLoop && loop.on, 20) && hasLoop) {
      doc.apply(std::make_unique<SetLoop>(loopToggled(song), loop.on ? "Loop off" : "Loop on"));
      syncLoop(doc, player.engine());
    }
  }
  ImGui::SameLine(0, 24 * ImGui::GetStyle().FontScaleDpi);
  const int tick = player.tick();
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
  ImGui::Separator();

  // The computer keyboard plays notes unless a text box has it. With Enter pressed it also types into the cell under the cursor.
  ImGuiIO& io = ImGui::GetIO();
  bool edited = false;
  const bool ctrl = io.KeyCtrl;
  const bool menuOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);   // a menu has the keys while it is open
  if (!io.WantTextInput && !menuOpen) {
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) octave = std::max(0, octave - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) octave = std::min(8, octave + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Pause, false)) {   // pause, or carry on from the pause
      if (player.playing()) { session->pausedAt = player.tick(); player.ring().push({LiveCommand::Stop, 0, 1}); }
      else if (session->pausedAt >= 0) { player.ring().push({LiveCommand::Play, session->pausedAt, 1}); session->pausedAt = -1; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) { player.ring().push({LiveCommand::Play, std::max(0, session->pausedAt), 1}); session->pausedAt = -1; }
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) { player.ring().push({LiveCommand::Stop, 0, 1}); session->pausedAt = -1; }
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) follow = !follow;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && session->screen != Screen::Sample && !(session->edit && (session->screen == Screen::Song || session->screen == Screen::Pattern))) {   // (editing, Space places a pattern on the Song screen and writes the live value on the Pattern screen)
      if (playing) player.ring().push({LiveCommand::Stop, 0, 1});
      else player.ring().push({LiveCommand::Play, 0, 1});
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) session->edit = !session->edit;
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
        session->editStatus = "saved " + session->path;
      } catch (const std::exception& e) {
        session->editStatus = e.what();
      }
    }
    if (io.KeyAlt && !ctrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_B, false)) session->showBrowser = !session->showBrowser;   // the browser
    if (io.KeyShift && !ctrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_I, false)) session->wantTree = true;   // the machine tree
    if (ctrl && !io.KeyShift && !io.KeyAlt && session->screen == Screen::Pattern) {   // the song loop: start and end at the cursor row, switch
      const auto setPoint = [&](const char* which, int at) {
        std::string message;
        Json loop = loopWithPoint(song, which, at, message);
        doc.apply(std::make_unique<SetLoop>(std::move(loop), message));
        syncLoop(doc, player.engine());
        session->editStatus = message;
      };
      if (ImGui::IsKeyPressed(ImGuiKey_B, false)) setPoint("start", editor.tick());
      if (ImGui::IsKeyPressed(ImGuiKey_E, false)) setPoint("end", editor.tick() + 1);
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
    if (ctrl && session->edit && session->screen == Screen::Pattern && !io.KeyAlt) {   // block editing: copy, cut, paste, select all
      if (ImGui::IsKeyPressed(ImGuiKey_C, false)) ImGui::SetClipboardText(editor.copy().c_str());
      if (ImGui::IsKeyPressed(ImGuiKey_X, false)) { ImGui::SetClipboardText(editor.copy().c_str()); edited |= editor.cut(); }
      if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
        const char* text = ImGui::GetClipboardText();
        edited |= (text && *text) ? editor.paste(text) : editor.paste();
      }
      if (ImGui::IsKeyPressed(ImGuiKey_A, false)) editor.selectAll();
    }
    const bool typing = session->edit && !ctrl && !io.KeyAlt && !session->envelopes.wantsKeys();
    if (typing) {
      const int before = editor.tick() * 4096 + editor.cell();
      const auto step = [&](int rows, int cells) { if (io.KeyShift) editor.extend(rows, cells); else editor.move(rows, cells); };
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) step(-1, 0);
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) step(1, 0);
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step(0, -1);
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step(0, 1);
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) editor.clearSelection();
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
      if (ImGui::IsKeyPressed(ImGuiKey_Equal, false)) edited |= editor.typeNoteOff();
      if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) edited |= editor.typeNoteCut();
      if (editor.onHexCell()) {
        for (int d = 0; d < 16; d++) {
          const ImGuiKey key = d < 10 ? static_cast<ImGuiKey>(ImGuiKey_0 + d) : static_cast<ImGuiKey>(ImGuiKey_A + (d - 10));
          if (ImGui::IsKeyPressed(key, false)) edited |= editor.typeHexDigit(d);
        }
      }
      if (before != editor.tick() * 4096 + editor.cell() || edited) session->scrollToCursor = true;
    }
    const bool hexKeys = typing && editor.onHexCell();   // on a hex cell the letters are digits, not piano keys
    if (!hexKeys && session->screen != Screen::Sample) {   // (on the Sample screen the keys play the sample)
      for (int i = 0; i < static_cast<int>(sizeof kPianoKeys / sizeof kPianoKeys[0]); i++) {
        const ImGuiKey key = kPianoKeys[i];
        if (ImGui::IsKeyPressed(key, false) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {   // (Shift+letter is a command, e.g. Shift+I: the machine menu)
          const int note = std::clamp(12 * (octave + 1) + i, 0, 127);
          const bool entering = typing && editor.onNoteCell();   // the key puts a note in the grid
          held[key] = note;
          if (!entering || app::settings().notePreview) player.ring().push({LiveCommand::NoteOn, note, 0.8});   // (Note preview off: entering is silent)
          if (entering) edited |= editor.typeNote(note);
        }
      }
    }
  }
  if (edited) {
    const std::vector<std::string> problems = syncLanes(doc, player.engine());
    session->editStatus = problems.empty() ? std::string() : problems.front();
  }
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
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) session->screen = Screen::Song;
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) session->screen = Screen::Pattern;
    if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) session->screen = Screen::Instrument;
    if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) session->screen = Screen::Sample;
    if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) session->screen = Screen::Info;
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) session->screen = Screen::Config;
  }
  const bool mixerShown = session->showMixer && (session->screen == Screen::Song || session->screen == Screen::Pattern);
  const float statusH = ImGui::GetTextLineHeightWithSpacing() + 6 * scale;
  const float below = -(statusH + (mixerShown ? app::mixerHeight() + 10 : 0.0f));
  ImGui::BeginChild("rail", ImVec2(40 * scale, below), false);
  {
    const struct { const char* label; Screen screen; } entries[] = {{"Song", Screen::Song}, {"Pattern", Screen::Pattern}, {"Instrument", Screen::Instrument}, {"Sample", Screen::Sample}, {"Info", Screen::Info}, {"Config", Screen::Config}};
    const struct { const char* icon; const char* tip; Screen screen; } icons[] = {
        {"arrange", "Song: the arrangement (F1)", Screen::Song}, {"patterns", "Pattern (F2)", Screen::Pattern},
        {"instruments", "Instrument (F3)", Screen::Instrument}, {"samples", "Sample (F4)", Screen::Sample}, {"info", "Song info (F10)", Screen::Info}, {"config", "Config (F11)", Screen::Config}};
    for (const auto& entry : icons) if (app::iconButton(entry.icon, entry.tip, session->screen == entry.screen, 24)) session->screen = entry.screen;
    (void)entries;
  }
  ImGui::EndChild();
  ImGui::SameLine();
  // The browser, down the left of every screen as in the web app: tabs for the lists (patterns, machines, samples), and under them
  // the options of the screen being shown (the Song screen's lane tools).
  const Lane* listed = nullptr;
  for (const Lane& l : doc.project().lanes) if (l.id == selectedLane) listed = &l;
  if (!listed) for (const Lane& l : doc.project().lanes) if (l.id != kMaster) { listed = &l; selectedLane = l.id; break; }   // none picked yet: the first lane
  app::PatternListResult listed2;
  if (session->showBrowser) {
    ImGui::BeginChild("browser", ImVec2(session->browserWidth * scale, below), false, ImGuiWindowFlags_NoNav);
    {   // the tabs
      const char* tabs[] = {"Patterns", "Machines", "Samples"};
      for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine(0, 8 * scale);
        if (app::textButton(tabs[i], session->browserTab == i)) session->browserTab = i;
      }
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
      const app::SampleBrowserResult picked = app::drawSampleBrowser(session->sampleBrowser, player.preview(), std::filesystem::path(options.path).parent_path(), 10, session->screen == Screen::Sample ? session->sampleView.selected : std::string());
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
        if (ImGui::Checkbox("All lanes side by side", &app::settings().patternAll)) app::saveSettings();
        if (ImGui::Checkbox("Tint patterns by colour", &app::settings().tintPatterns)) app::saveSettings();
        if (ImGui::Checkbox("Note preview", &app::settings().notePreview)) app::saveSettings();
        ImGui::SetNextItemWidth(120 * ImGui::GetStyle().FontScaleDpi);
        if (ImGui::SliderFloat("Text size", &app::settings().patternText, 0.6f, 2.0f, "%.2fx")) app::saveSettings();
        if (ImGui::Checkbox("Hide empty columns", &app::settings().hideEmpty)) app::saveSettings();
        if (ImGui::Checkbox("Follow the playing row", &follow)) { app::settings().follow = follow; app::saveSettings(); }
      }
      if (session->screen == Screen::Song) {
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
          doc.apply(std::make_unique<RenameLane>(curId, wanted)); selectedLane = wanted; session->wantLane = wanted; session->laneNameFor = wanted; laneEdited = true;
        }
        if (fits("<")) ImGui::SameLine(0, 8 * scale);
        if (app::textButton("<")) { doc.apply(std::make_unique<MoveLane>(curId, -1)); }
        if (fits(">")) ImGui::SameLine(0, 4 * scale);
        if (app::textButton(">")) { doc.apply(std::make_unique<MoveLane>(curId, 1)); }
        if (fits("Duplicate")) ImGui::SameLine(0, 8 * scale);
        if (app::textButton("Duplicate") && !laneEdited) {
          const std::string copy = app::uniqueName(curId, laneIds);
          doc.apply(std::make_unique<DuplicateLane>(curId, copy)); selectedLane = copy; session->wantLane = copy; laneEdited = true;
        }
        int shares = 0;
        for (const Lane& l : doc.project().lanes) shares += l.instrument == curMachine;
        if (app::textButton("Remove") && !laneEdited) { const std::string gone = curId; doc.apply(std::make_unique<RemoveLane>(gone, false)); session->wantLane.clear(); selectedLane.clear(); laneEdited = true; }
        if (shares == 1 && !laneEdited) {
          if (fits("Remove + machine")) ImGui::SameLine(0, 8 * scale);
          if (app::textButton("Remove + machine")) {
            const std::string gone = curId, machine = curMachine;
            doc.apply(std::make_unique<RemoveLane>(gone, true));
            session->playable.erase(std::remove(session->playable.begin(), session->playable.end(), machine), session->playable.end());
            if (session->instrument == machine) session->instrument.clear();
            session->wantLane.clear(); selectedLane.clear(); laneEdited = true;
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
            selectedLane = laneCopy; session->wantLane = laneCopy;
            session->editStatus = problems.empty() ? std::string() : problems.front();
          }
          if (fits("patterns")) ImGui::SameLine(0, 8 * scale);
          ImGui::Checkbox("patterns", &session->clonePatterns);
          if (fits("automation")) ImGui::SameLine(0, 8 * scale);
          ImGui::Checkbox("automation", &session->cloneCurves);
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
      if (ImGui::Checkbox("cut patterns across the cursor", &app::settings().insertCut)) app::saveSettings();
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
            session->wantLane = laneId;
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
          if (ImGui::Checkbox("sleep when quiet", &sleeps)) { never = !sleeps; setup = true; }
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
      const ImVec2 window = ImGui::GetWindowPos();
      const float x = window.x + ImGui::GetWindowWidth() - 5 * scale;
      ImGui::SetCursorScreenPos(ImVec2(x, window.y));
      ImGui::InvisibleButton("##browsergrip", ImVec2(5 * scale, ImGui::GetWindowHeight()));
      const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
      if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      if (ImGui::IsItemActive()) {
        session->browserWidth += ImGui::GetIO().MouseDelta.x / scale;
        if (session->browserWidth < 90) { session->browserWidth = 260; session->showBrowser = false; }
        session->browserWidth = std::clamp(session->browserWidth, 160.0f, 600.0f);
      }
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) session->browserWidth = 260;
      ImGui::GetWindowDrawList()->AddLine(ImVec2(x + 2 * scale, window.y), ImVec2(x + 2 * scale, window.y + ImGui::GetWindowHeight()), hot ? app::kAccent : app::lineAlpha(90), 1.0f);
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
  }
    bool listEdited = false;
    if (listed) editor.setLane(listed->id, player.engine().laneColumns(listed->id));
    if (listed2.create) listEdited |= editor.createPatternHere();
    if (listed && !listed2.rename.empty()) { doc.apply(std::make_unique<RenamePattern>(listed->id, session->patternList.picked, listed2.rename)); listEdited = true; }
    if (listed && listed2.colorStep != 0) {
      for (const Pattern& pat : listed->patterns) if (pat.id == session->patternList.picked) {
        doc.apply(std::make_unique<SetPatternColor>(listed->id, pat.id, app::steppedPatternColor(pat, listed2.colorStep)));
        listEdited = true;
        break;
      }
    }
    if (listed && listed2.clone) {
      const std::string copyId = "clone-" + std::to_string(static_cast<long long>(ImGui::GetTime() * 1000)) + "-" + std::to_string(doc.revision());
      doc.apply(std::make_unique<ClonePattern>(listed->id, session->patternList.picked, copyId));
      session->patternList.picked = copyId; listEdited = true;
    }
    std::string toPlace = listed2.place;
    if (session->screen == Screen::Song && session->edit && ImGui::IsKeyPressed(ImGuiKey_Space, false) && !ImGui::GetIO().WantTextInput) toPlace = session->patternList.picked;   // Space: the outlined pattern here
    if (!toPlace.empty()) {
      const int before = editor.tick();
      if (editor.placePatternHere(toPlace)) {
        listEdited = true;
        if (listed) for (const Pattern& pat : listed->patterns) if (pat.id == toPlace) editor.moveTo(std::min(before + static_cast<int>(pat.rows.size()), song.length - 1), editor.cell());   // on to where it ends
      }
    }
    if (session->screen == Screen::Song && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput) {   // Ctrl+Up / Ctrl+Down step through the list
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) app::stepPatternList(session->patternList, listed, -1);
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) app::stepPatternList(session->patternList, listed, 1);
    }
    if (listEdited) { const std::vector<std::string> problems = syncLanes(doc, player.engine()); session->editStatus = problems.empty() ? std::string() : problems.front(); }
  ImGui::BeginChild("screen", ImVec2(0, below), false);
  std::string laneInstrument;
  for (const Lane& lane : doc.project().lanes) if (lane.id == selectedLane) laneInstrument = lane.instrument;
  if (session->screen == Screen::Song) {
    ImGui::BeginChild("seqmain", ImVec2(0, 0), false, ImGuiWindowFlags_NoNav);
    const app::CurveView::Targets curveTargetsOf = [&](const Lane& lane) { return curveTargets(player.engine().laneColumns(lane.id), lane.instrument, player.engine().registry()); };
    const app::SequenceClick hit = app::drawSequenceView(doc.project(), selectedLane, playing ? tick : -1, session->edit ? editor.tick() : -1, follow && playing && !session->edit, session->curveView, curveTargetsOf);
    if (hit.curve.any) {   // a curve edited: one undo step per gesture, the lane rebuilt in the engine
      doc.apply(std::make_unique<SetCurves>(hit.curve.lane, hit.curve.curves, hit.curve.label), hit.curve.fold);
      const std::vector<std::string> problems = syncLanes(doc, player.engine());
      session->editStatus = problems.empty() ? std::string() : problems.front();
    }
    if (!hit.lane.empty()) { session->wantLane = hit.lane; session->wantTick = hit.tick; selectedLane = hit.lane; }
    ImGui::EndChild();
  } else if (session->screen == Screen::Pattern) {
    const float sheetW = session->showKeys ? 270 * scale : 0;
    ImGui::BeginChild("patmain", ImVec2(-sheetW, 0), false, ImGuiWindowFlags_NoNav);
    // the grid of the lanes shown (one, picked by its tab, or all of them side by side), the cursor in lane `cursorLane` of them
    const auto drawGrid = [&](const std::vector<const Lane*>& shown, int cursorLane) {
      std::vector<app::PatternLane> grids;
      for (const Lane* shownLane : shown) {
        app::PatternLane grid;
        grid.id = shownLane->id;
        grid.rows = &session->rows[shownLane->id];
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
        grids.push_back(std::move(grid));
      }
      const Lane& cursorOwner = *shown[static_cast<std::size_t>(cursorLane)];
      app::CursorView cursor;
      cursor.lane = cursorLane;
      cursor.show = session->edit;
      cursor.hideEmpty = app::settings().hideEmpty;
      cursor.tick = editor.tick();
      cursor.cell = editor.cell();
      cursor.scrollTo = session->scrollToCursor;
      cursor.playhead = app::settings().playhead.c_str();
      const CellEditor::Selection sel = editor.selection();
      { const LoopRange lr = loopOf(song); cursor.loop = song.raw.is_object() && song.raw.contains("loop") && song.raw["loop"].is_object(); cursor.loopOn = lr.on; cursor.loopStart = lr.start; cursor.loopEnd = lr.end; }
      {   // how far through the playing row we are: the time since the tick changed over one tick's length
        static int lastTick = -1; static double changedAt = 0;
        if (tick != lastTick) { lastTick = tick; changedAt = ImGui::GetTime(); }
        const double tickSeconds = 60.0 / (std::max(1.0, song.bpm) * std::max(1, song.ticksPerBeat));
        cursor.playFrac = playing ? static_cast<float>(std::clamp((ImGui::GetTime() - changedAt) / tickSeconds, 0.0, 1.0)) : 0.0f;
      }
      cursor.info = app::describeColumn(doc.project(), cursorOwner, session->player->engine().laneColumns(cursorOwner.id), session->rows[cursorOwner.id], editor.tick(), editor.cell());
      cursor.selected = sel.active; cursor.tick0 = sel.tick0; cursor.tick1 = sel.tick1; cursor.cell0 = sel.cell0; cursor.cell1 = sel.cell1;
      session->scrollToCursor = false;
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * app::settings().patternText);   // Text size
      const app::PatternViewResult result = app::drawPatternView(grids, song.ticksPerBeat, song.beatsPerBar, playing ? tick : -1, follow && playing && !session->edit, cursor);
      ImGui::PopFont();
      if (result.clickedTick >= 0) playFromRequest = result.clickedTick;
      if (result.cell >= 0 && result.lane >= 0) {
        const Lane& clicked = *shown[static_cast<std::size_t>(result.lane)];
        if (clicked.id != editor.lane()) { selectedLane = clicked.id; editor.setLane(clicked.id, session->player->engine().laneColumns(clicked.id)); }
        editor.moveTo(result.cellTick, result.cell);
      }
    };
    if (app::settings().patternAll) {
      std::vector<const Lane*> shown;
      int cursorLane = 0;
      for (const Lane& lane : doc.project().lanes) { if (lane.id == selectedLane) cursorLane = static_cast<int>(shown.size()); shown.push_back(&lane); }
      if (!shown.empty()) {
        if (!session->wantLane.empty()) {   // a lane asked for (a click in the Song screen)
          for (std::size_t i = 0; i < shown.size(); i++) if (shown[i]->id == session->wantLane) cursorLane = static_cast<int>(i);
        }
        const Lane& owner = *shown[static_cast<std::size_t>(cursorLane)];
        selectedLane = owner.id;
        laneInstrument = owner.instrument;
        editor.setLane(owner.id, session->player->engine().laneColumns(owner.id));
        if (!session->wantLane.empty()) {
          if (session->wantTick >= 0) { editor.moveTo(session->wantTick, editor.cell()); session->scrollToCursor = true; }
          session->wantLane.clear();
          session->wantTick = -1;
        }
        drawGrid(shown, cursorLane);
      }
    } else if (ImGui::BeginTabBar("lanes")) {
      for (const Lane& lane : doc.project().lanes) {
        const std::string label = lane.id + (lane.instrument == lane.id ? "" : "  " + lane.instrument);
        ImGuiTabItemFlags tabFlags = ImGuiTabItemFlags_None;
        if (!session->pickedTab && lane.id != kMaster) { tabFlags |= ImGuiTabItemFlags_SetSelected; session->pickedTab = true; }
        if (lane.id == session->wantLane) tabFlags |= ImGuiTabItemFlags_SetSelected;
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);   // lane names in a small face
        const bool tabOpen = ImGui::BeginTabItem(label.c_str(), nullptr, tabFlags);
        ImGui::PopFont();
        if (tabOpen) {
          selectedLane = lane.id;
          laneInstrument = lane.instrument;
          editor.setLane(lane.id, session->player->engine().laneColumns(lane.id));
          if (lane.id == session->wantLane) {
            if (session->wantTick >= 0) { editor.moveTo(session->wantTick, editor.cell()); session->scrollToCursor = true; }
            session->wantLane.clear();
            session->wantTick = -1;
          }
          drawGrid({&lane}, 0);
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
    }
    ImGui::EndChild();
    if (session->showKeys) { ImGui::SameLine(0, 8 * scale); app::drawShortcutSheet("patkeys", app::patternShortcuts(), 0, 0); }
  } else if (session->screen == Screen::Instrument) {
    // the instruments as flat text on the left, the graph in the middle, the parameter dock at the right
    if (session->instrument.empty()) session->instrument = !laneInstrument.empty() ? laneInstrument : session->playable.empty() ? std::string(kMaster) : session->playable.front();
    laneInstrument = session->instrument;
    ImGui::BeginChild("instmain", ImVec2(0, 0), false);
    if (session->panel.instrument != laneInstrument) { session->panel.instrument = laneInstrument; session->panel.node.clear(); }
    app::drawGraphView(session->graphView, doc, laneInstrument, session->panel.node, 0);
    const ImVec2 graphMin = ImGui::GetWindowPos(), graphMax(graphMin.x + ImGui::GetWindowWidth(), graphMin.y + ImGui::GetWindowHeight());
    ImGui::EndChild();
    if (app::drawParamPanel(session->panel, doc, player.engine(), laneInstrument, graphMin, graphMax)) {   // the parameters float over the graph
      syncParams(doc, player.engine());
      const std::vector<std::string> graphProblems = syncGraphs(doc, player.engine()), laneProblems = syncLanes(doc, player.engine());
      session->editStatus = !graphProblems.empty() ? graphProblems.front() : !laneProblems.empty() ? laneProblems.front() : std::string();
    }
  } else if (session->screen == Screen::Sample) {
    app::drawSampleScreen(session->sampleView, doc, player.preview(), scale);
  } else if (session->screen == Screen::Info) {
    // Song info: plain text saved with the project (notes, credits, greetings). One undo step each time the box is left.
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "Song info: notes, credits, greetings. Plain text, saved with the project.");
    const std::string stored = song.raw.is_object() && song.raw.contains("info") && song.raw["info"].is_string() ? song.raw["info"].get<std::string>() : std::string();
    if (!session->infoFocused) std::snprintf(session->infoText.data(), session->infoText.size(), "%s", stored.c_str());
    ImGui::InputTextMultiline("##info", session->infoText.data(), session->infoText.size(), ImVec2(-1, -1));
    session->infoFocused = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit() && std::string(session->infoText.data()) != stored) doc.apply(std::make_unique<SetSongInfo>(session->infoText.data()));
  } else {
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
    group("Playing", "How the screens behave while the song runs.");
    if (ImGui::Checkbox("Follow mode (the grids scroll with the playing row)", &follow)) { cfg.follow = follow; changed = true; }
    {
      static const struct { const char* id; const char* label; } looks[] = {{"marker", "a > in the tick column and a tinted row"}, {"line", "a thin line gliding through the playing row"}, {"band", "the tinted row alone"}};
      const char* shown = looks[1].label;
      for (const auto& look : looks) if (cfg.playhead == look.id) shown = look.label;
      ImGui::SetNextItemWidth(430 * scale);
      if (ImGui::BeginCombo("Playhead", shown)) {
        for (const auto& look : looks) if (ImGui::Selectable(look.label, cfg.playhead == look.id)) { cfg.playhead = look.id; changed = true; }
        ImGui::EndCombo();
      }
    }
    group("Graph", "Wire colours by kind of signal, and dashed control wires.");
    colour("Wire colour: audio", cfg.wireAudio);
    colour("Wire colour: control", cfg.wireControl);
    colour("Wire colour: stereo", cfg.wireStereo);
    colour("Wire colour: parameter", cfg.wireParam);
    if (ImGui::Checkbox("Control wires are dashed", &cfg.wireDashed)) changed = true;
    ImGui::Spacing();
    if (app::textButton("Reset to defaults")) { app::resetSettings(); follow = cfg.follow; changed = true; }
    if (changed) app::saveSettings();
  }
  ImGui::EndChild();
  // The keys play the machine being worked on: the lane's machine, or the one picked in the browser.
  if (!laneInstrument.empty() && laneInstrument != session->keyjazzInstrument &&
      std::find(session->playable.begin(), session->playable.end(), laneInstrument) != session->playable.end()) {
    player.ring().push({LiveCommand::AllOff, 0, 0});
    session->keyjazzInstrument = laneInstrument;
    player.setInstrument(laneInstrument);
  }
  if (!session->panel.drawNode.empty()) { app::showEnvelopes(session->envelopes, laneInstrument, session->panel.drawNode); session->panel.drawNode.clear(); }
  ImGui::Separator();
  {
    const std::vector<std::string> sampleProblems = syncSamples(doc, player.engine());   // (after any edit or undo of a sample)
    if (!sampleProblems.empty()) session->editStatus = sampleProblems.front();
  }
  if (app::drawEnvelopeWindow(session->envelopes, doc, player.engine())) syncData(doc, player.engine());
  if (!session->panel.dxNode.empty()) { app::showDx(session->dx, laneInstrument, session->panel.dxNode); session->panel.dxNode.clear(); }
  if (app::drawDxWindow(session->dx, doc, player.engine())) { syncData(doc, player.engine()); syncParams(doc, player.engine()); }
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
      session->wantLane = laneId;
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
    else ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "%s  tick %d  octave %d", selectedLane.c_str(), editor.tick(), octave);
    ImGui::SameLine(0, 14 * scale);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "undo: %s  redo: %s", doc.canUndo() ? doc.undoLabel().c_str() : "-", doc.canRedo() ? doc.redoLabel().c_str() : "-");
    if (!session->editStatus.empty()) {
      ImGui::SameLine(0, 14 * scale);
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kWarm), "%s", session->editStatus.c_str());
    }
    ImGui::SameLine(std::max(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 20 * scale, ImGui::GetWindowWidth() - 520 * scale));   // (to the right of what was written, or at the right edge)
    if (session->audio) {
      const double load = session->audio->load();
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(load > 0.7 ? app::kWarm : app::kDimText), "cpu %.0f%%  late %ld", 100 * load, session->audio->lateBuffers());
      ImGui::SameLine(0, 14 * scale);
    }
    if (app::iconButton("browser-toggle", "Browser: the patterns and machines, down the left of every screen (Alt+B)", session->showBrowser, 16)) session->showBrowser = !session->showBrowser;
    ImGui::SameLine(0, 10 * scale);
    if (app::iconButton("keys-toggle", "Shortcuts: the keys of the Pattern screen, beside the grid", session->showKeys, 16)) session->showKeys = !session->showKeys;
    ImGui::SameLine(0, 10 * scale);
    if (app::iconButton("mixer-toggle", "Mixer: a fader for each lane, along the bottom of the Song and Pattern screens", session->showMixer, 16)) session->showMixer = !session->showMixer;
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
    ImGui::PopFont();
  }
  ImGui::End();
}

// "enter,down,ctrl+z,4,7" -> key events, one key every two frames (down, then up).
ImGuiKey keyNamed(const std::string& name) {
  static const std::map<std::string, ImGuiKey> named = {
      {"enter", ImGuiKey_Enter}, {"up", ImGuiKey_UpArrow}, {"down", ImGuiKey_DownArrow}, {"left", ImGuiKey_LeftArrow},
      {"right", ImGuiKey_RightArrow}, {"delete", ImGuiKey_Delete}, {"backspace", ImGuiKey_Backspace}, {"insert", ImGuiKey_Insert},
      {"equal", ImGuiKey_Equal}, {"pagedown", ImGuiKey_PageDown}, {"pageup", ImGuiKey_PageUp}, {"space", ImGuiKey_Space}};
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

void feedKeys(const std::string& script, int frame) {
  std::vector<std::string> keys;
  std::stringstream stream(script);
  for (std::string item; std::getline(stream, item, ',');) keys.push_back(item);
  ImGuiIO& io = ImGui::GetIO();
  const int index = frame / 2;
  if (index < 0 || index >= static_cast<int>(keys.size())) return;
  std::string name = keys[static_cast<std::size_t>(index)];
  bool ctrl = false, shift = false;
  for (;;) {
    if (name.rfind("ctrl+", 0) == 0) { ctrl = true; name = name.substr(5); }
    else if (name.rfind("shift+", 0) == 0) { shift = true; name = name.substr(6); }
    else break;
  }
  const bool down = frame % 2 == 0;
  io.AddKeyEvent(ImGuiMod_Ctrl, ctrl && down);
  io.AddKeyEvent(ImGuiMod_Shift, shift && down);
  io.AddKeyEvent(keyNamed(name), down);
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
    else if (a == "--envelope" && more) o.envelope = argv[++i];
    else if (a == "--dx" && more) o.dxnode = argv[++i];
    else if (a == "--screen" && more) o.screen = argv[++i];
    else if (a == "--theme" && more) o.theme = argv[++i];
    else if (a == "--frames" && more) o.frames = std::max(1, std::atoi(argv[++i]));
    else if (a == "--play-from" && more) o.playFrom = std::atoi(argv[++i]);
    else if (a == "--null") o.useNull = true;
    else if (a == "--splash") { o.splash = true; o.splashForced = true; }
    else if (a == "--no-splash") o.splash = false;
    else if (!a.empty() && a[0] != '-' && o.path.empty()) o.path = a;
    else return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse(argc, argv, options)) {
    std::printf("usage: wax-app [song.wax] [--rate hz] [--threads n] [--midi n] [--null] [--screenshot out.bmp [--frames n] [--play-from tick]]\n");
    return 1;
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::printf("SDL: %s\n", SDL_GetError());
    return 2;
  }
  SDL_Window* window = SDL_CreateWindow("Wax", 1280, 800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (renderer) app::iconsInit(renderer);
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
  app::applyTheme(scale);
  loadTheme();
  app::loadSettings();
  if (!options.theme.empty()) app::setThemeByName(options.theme.c_str());
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  std::unique_ptr<Session> session;
  std::string status;
  const auto open = [&](const std::string& path) {
    session.reset();   // the old sound stops before the new song is built
    try {
      session = std::make_unique<Session>(path, options);
      if (options.screen == "song") session->screen = Screen::Song;
      else if (options.screen == "instrument") session->screen = Screen::Instrument;
      else if (options.screen == "sample") session->screen = Screen::Sample;
      else if (options.screen == "info") session->screen = Screen::Info;
      else if (options.screen == "config") session->screen = Screen::Config;
      std::string title = "Wax - " + session->project.song.name;
      SDL_SetWindowTitle(window, title.c_str());
      status.clear();
    } catch (const std::exception& e) {
      status = std::string("Could not open ") + path + ": " + e.what();
      SDL_SetWindowTitle(window, "Wax");
    }
  };
  if (!options.path.empty()) open(options.path);

  float meterL = 0, meterR = 0;
  bool titleUnsaved = false;
  std::map<ImGuiKey, int> held;
  int octave = 4, playFromRequest = -1, frame = 0;
  std::string selectedLane;
  bool follow = app::settings().follow, running = true;
  bool showSplash = options.splash && (options.splashForced || (options.screenshot.empty() && !options.useNull));
  if (session && !options.screenshot.empty() && options.playFrom >= 0)
    session->player->ring().push({LiveCommand::Play, options.playFrom, 1});
  else if (session && !options.screenshot.empty())
    session->player->ring().push({LiveCommand::Play, 0, 1});

  while (running) {
    for (SDL_Event event; SDL_PollEvent(&event);) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) running = false;
      if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) open(event.drop.data);
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    if (!options.clicks.empty() && frame >= 4) feedClicks(options.clicks, frame - 4);
    if (!options.dxnode.empty() && frame == 3 && session) {
      const std::size_t dot = options.dxnode.find('.');
      app::showDx(session->dx, options.dxnode.substr(0, dot), options.dxnode.substr(dot + 1));
    }
    if (!options.envelope.empty() && frame == 3 && session) {
      const std::size_t dot = options.envelope.find('.');
      app::showEnvelopes(session->envelopes, options.envelope.substr(0, dot), options.envelope.substr(dot + 1));
    }
    if (!options.press.empty() && frame >= 4) feedKeys(options.press, frame - 4);   // (the first frames let the window settle)
    ImGui::NewFrame();
    drawFrame(session.get(), options, status, meterL, meterR, held, octave, selectedLane, follow, playFromRequest);
    if (showSplash) showSplash = drawSplash(SDL_GetTicks() / 1000.0, "native");
    if (gRequest == DialogRequest::New) {   // start over with an empty song (the old one is replaced: unsaved changes are lost)
      gRequest = DialogRequest::None;
      if (session) {
        Project blank = emptyProject(session->doc->project());
        session.reset();
        try { session = std::make_unique<Session>(std::move(blank), std::string(), options); status.clear(); SDL_SetWindowTitle(window, "Wax - untitled"); }
        catch (const std::exception& e) { status = std::string("Could not start a new song: ") + e.what(); }
      }
    }
    if (gRequest != DialogRequest::None) {
      static const SDL_DialogFileFilter filters[] = {{"Wax songs", "wax"}, {"All files", "*"}};
      static const SDL_DialogFileFilter wavFilter[] = {{"WAV audio", "wav"}};
      static DialogKind saveKind = DialogKind::Save, exportKind = DialogKind::Export;
      if (gRequest == DialogRequest::Open) SDL_ShowOpenFileDialog(dialogDone, nullptr, window, filters, 2, nullptr, false);
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
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  app::iconsShutdown();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

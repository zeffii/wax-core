// The window's state and what its parts share: the options, the session (one song, loaded), the screens' common frame context, and the
// functions the screens' files call. The parts: wax_app.cpp (the frame, the header and rail, the main loop), keyboard.cpp, browser.cpp,
// and one file per screen.
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <functional>
#include <future>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "app/minimap.h"
#include "app/pattern_toolbar.h"
#include "app/sequence_toolbar.h"
#include "app/handoff.h"
#include "app/sequence_view.h"
#include "app/it_import.h"
#include "app/xm_import.h"
#include "app/song_keys.h"
#include "edit/sequence_ops.h"
#include "app/envelope_window.h"
#include "app/node_help.h"
#include "app/config.h"
#include "app/file_association.h"
#include "app/sample_browser.h"
#include "app/sample_view.h"
#include "project/backups.h"
#include "app/curve_view.h"
#include "app/dx_window.h"
#include "app/chord_window.h"
#include "app/wavetable_window.h"
#include "app/gate_window.h"
#include "app/fonts.h"
#include "app/genexpr_window.h"
#include "app/vector_window.h"
#include "app/shape_window.h"
#include "app/graph_view.h"
#include "app/node_strip.h"
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
#include "seq/cells.h"
#include "seq/loop.h"
#include "live/audio_out.h"
#include "project/save.h"
#include "live/player.h"
#include "engine/offline.h"
#include "project/project.h"
#include "io/wav.h"
#include "project/samples.h"


namespace wax::tool {

using namespace wax;

// File dialogs answer on another thread; the window picks the answer up between frames.
enum class DialogKind { Open, Save, Export };
struct DialogAnswer {
  std::mutex mutex;
  std::string path;
  DialogKind kind = DialogKind::Open;
  bool ready = false;
};
inline DialogAnswer gAnswer;
enum class DialogRequest { None, Open, SaveAs, Export, New, ClearArrangement, ClearPatterns };
inline float gFileX = 0;          // the screen x of the File word, as drawn last frame: the browser's tabs start at the same x
inline float gIconCentre = 0;      // the screen y of the middle of the transport icons, as drawn last frame: the File word is centred on it
inline constexpr float kFileGap = 18;   // the space either side of the File word, in web pixels: from the rail's line, and to the Play button
inline std::string gOpenRecent;   // a song the File menu's recent list asked to open
inline std::string gOpenBackup;   // a snapshot the File menu asked to open (the main loop does it: it replaces the session)
inline DialogRequest gRequest = DialogRequest::None;   // set by the keys, carried out by the main loop (it has the window)

struct Options {
  std::string path, screenshot, press, clicks, drag, envelope, dxnode, vectornode, genexprnode, screen, theme;   // clicks: "x,y;x,y" mouse clicks, one every 6 frames; envelope: "instrument.node" opens its envelope window   // press: keys to press one after another (for checking the window without a keyboard)
  double rate = 48000;
  int threads = 0, midiPort = 0, frames = 3, playFrom = -1, buffer = 0;
  bool useNull = false;
  bool allLanes = false;   // --all-lanes: the Pattern screen shows every lane side by side
  bool splashForced = false;
  bool noPlay = false;   // --no-play: a screenshot run does not start the song (the picture does not depend on the timing of the audio)
  bool splash = true;   // the Wax logo for a moment at start (not when taking a screenshot, unless --splash is given)
};

// One song, loaded: the project, the engine that plays it, the sound card and the MIDI input.
enum class Screen { Song, Pattern, Instrument, Sample, Info, Config };

// A path from UTF-8 text (std::filesystem::u8path is deprecated since C++20).
inline std::filesystem::path pathFromUtf8(const std::string& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }

// The x of the thin line to the right of the rail (the rail is 40 units wide and starts at `left`).
inline float railLineX(float left, float scale) { return std::floor(left + 40 * scale + ImGui::GetStyle().ItemSpacing.x / 2) + 0.5f; }

// Where the song is as it is HEARD. The audio thread reports a tick when it has rendered it, which is the device's buffer ahead of the
// speakers, so the display looks the tick up as it was one latency ago (and glides through the row from the moment that tick began).
struct AudibleClock {
  struct Stamp { double time; int tick; };
  std::vector<Stamp> history;
  int tick = 0;
  float frac = 0;   // how far through the audible row (0..1)
  void update(bool playing, int reported, double now, double latency, double tickSeconds) {
    if (!playing) { history.clear(); tick = reported; frac = 0; return; }
    if (history.empty() || history.back().tick != reported) history.push_back({now, reported});
    const double heard = now - latency;
    std::size_t at = 0;
    for (std::size_t i = 0; i < history.size(); i++) if (history[i].time <= heard) at = i;
    history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(at));   // (older stamps are never asked for again)
    tick = history.front().tick;
    frac = static_cast<float>(std::clamp((heard - history.front().time) / tickSeconds, 0.0, 1.0));
  }
};

struct Session {
  Project project;
  std::unique_ptr<Player> player;
  std::unique_ptr<AudioOut> audio;
  std::unique_ptr<RtMidiIn> midi;
  std::string midiName = "none";
  std::string keyjazzInstrument;
  struct HeldKey { double since = 0; int previewMs = 0; };
  std::map<ImGuiKey, HeldKey> heldSince;     // the piano keys held: when each went down (seconds) and the machine's least preview time
  std::vector<std::pair<double, int>> releaseAt;   // notes whose key was let go before the machine's least preview time: when to release each (seconds), and the note
  app::SampleView sampleView;   // the Sample screen: the sample chosen, its selection, the view of its waveform
  app::SampleBrowserState sampleBrowser;   // the Samples tab: the folder shown and the file picked
  app::CurveView curveView;   // the lane curves drawn over the Song screen: the one each lane shows, what is dragged
  std::vector<std::string> playable;                                       // the machines a keyboard can play
  std::map<std::string, std::vector<std::vector<std::string>>> rows;      // lane id -> flattened rows
  std::string audioName;
  bool pickedTab = false;   // the first lane that is not the master is shown at first
  std::unique_ptr<Document> doc;         // the song as edited: every change goes through it, so it can be undone
  std::unique_ptr<CellEditor> editor;
  // Ctrl+Enter in the sequence: one pattern edited on its own (rows lo..hi-1 of one lane). The editor is told in the same call, so the two
  // never disagree about whether solo-edit is on.
  struct SoloEdit {
    bool on = false;
    int lo = 0, hi = 0;
    std::string lane;
    void begin(CellEditor& editor, const std::string& laneId, int firstRow, int endRow) { on = true; lo = firstRow; hi = endRow; lane = laneId; editor.setSolo(lo, hi); }
    void end(CellEditor& editor) { on = false; lo = hi = 0; lane.clear(); editor.clearSolo(); }
  } solo;
  bool edit = false;                     // Enter: the keyboard types into cells
  bool backupsOpen = false;              // the Backups list is showing
  int backupRevision = 0;                // the revision of `doc` the last snapshot (or the opening) holds
  std::time_t backupAt = 0;              // when the last snapshot was made (or the song opened)
  app::MinimapCache minimapCache;        // the Song screen's pattern pictures, kept between frames
  std::map<std::string, int> rowsVersion;   // lane id -> counts the times its rows changed (the pattern pictures are painted again when it moves)
  int rowsRevision = -1;                 // the revision of `doc` that `rows` shows
  app::Handoff handoff;                  // what the Song screen (and others) ask of the Pattern screen: lane tab, cursor, scroll
  std::string editStatus;
  app::ParamPanelState panel;
  app::MixerState mixer;
  app::EnvelopeWindow envelopes;
  app::NodeHelpWindow nodeHelp;           // Shift+F1 on the Instrument screen: the help of the picked node
  app::DxWindow dx;
  app::GraphViewState graphView;
  app::NodeStripState strip;   // the node UIs along the bottom of the graph
  app::GateWindow gate;
  app::VectorWindow vector;
  app::GenExprWindow genexpr;
  app::ChordWindow chords;
  app::WavetableWindow table;
  app::ShapeWindow shape;
  app::PatternListState patternList;     // the Song screen's pattern list
  char laneName[40] = {0};               // the rename box on the Song screen
  std::string laneNameFor;               // the lane that text belongs to
  int silenceTicks = 128;                // the length of silence Insert puts in
  bool wantTree = false;                 // open the machine tree (Shift+I, or the Instrument screen's add button)
  bool showKeys = false;                  // the shortcut sheet beside the Pattern grid
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
  bool showMixer = true;                 // the mixer under the Song, Pattern and Instrument screens (the status bar's toggle)
  Screen screen = Screen::Pattern;       // the screen shown (the rail at the left)
  std::string instrument;                // the instrument shown on the Instrument screen
  app::SongEdit song;                    // the Song screen's keyboard state: the selected block, the copied block, the waypoint
  AudibleClock heard;                    // where the song is as it is heard (the display follows this, not the audio thread's tick)
  std::string path;                      // the file this song was opened from, where Ctrl+S saves it

  int audioBuffer = 0;                   // the sound card buffer asked for (0: the card's own)
  Session(const std::string& filePath, const Options& options) : Session(loadProject(filePath), filePath, options) {}
  Session(Project loaded, const std::string& filePath, const Options& options) : project(std::move(loaded)), path(filePath) {
    repairNoteColumnRows(project);   // (a song saved after a note column was added without its cells reaches the pattern editor whole)
    for (const Instrument& inst : project.instruments) if (inst.id != kMaster && !inst.isMeta) playable.push_back(inst.id);
    keyjazzInstrument = playable.empty() ? std::string() : playable.front();
    player = std::make_unique<Player>(project, options.rate, options.threads > 0 ? options.threads : app::settings().threads, keyjazzInstrument, true);
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
    audioBuffer = bufferFrames;
    audio = std::make_unique<AudioOut>(*player, options.rate, options.useNull, bufferFrames);
    audioName = audio->deviceName() + " (" + audio->backendName() + ")";
    audio->start();
  }
  // Gives the engine another number of render threads: the sound card's callback is stopped while the pool is rebuilt, then started again.
  void setThreads(const Options& options, int threads) {
    audio.reset();
    player->ring().push({LiveCommand::AllOff, 0, 0});
    player->engine().runtime().setThreads(threads);
    try { openAudio(options, audioBuffer); }
    catch (const std::exception& e) { audioName = std::string("no sound: ") + e.what(); }
  }
  ~Session() {
    if (midi) midi->closePort();   // the callback must not outlive the player
    midi.reset();
    audio.reset();
  }
};

// The computer keyboard as a piano, the web's piano.js: two rows, the lower from C of the octave, the upper an octave higher (to the P key, two octaves up).
inline const ImGuiKey kPianoKeys[] = {
    ImGuiKey_Z, ImGuiKey_S, ImGuiKey_X, ImGuiKey_D, ImGuiKey_C, ImGuiKey_V, ImGuiKey_G, ImGuiKey_B, ImGuiKey_H, ImGuiKey_N,
    ImGuiKey_J, ImGuiKey_M,
    ImGuiKey_Q, ImGuiKey_2, ImGuiKey_W, ImGuiKey_3, ImGuiKey_E, ImGuiKey_R, ImGuiKey_5, ImGuiKey_T, ImGuiKey_6, ImGuiKey_Y,
    ImGuiKey_7, ImGuiKey_U, ImGuiKey_I, ImGuiKey_9, ImGuiKey_O, ImGuiKey_0, ImGuiKey_P};

// Who the computer keyboard goes to this frame, decided once from the screen and the editing state (Enter makes the caret active). The key
// handlers below ask this, and do not work it out again each in their own way.
struct KeyFocus {
  bool sequenceCaret = false;   // the Song screen with its caret active: arrows, selection, block copy and the edits at the caret
  bool gridCaret = false;       // the Pattern screen with its caret active (the Ctrl keys that copy and paste cells)
  bool gridEditing = false;     // the same, and the envelope window is not taking the keys: typing and the Ctrl keys that edit
  bool pianoKeys = false;       // letters play notes (the Sample screen plays the sample; the sequence caret takes the keys as commands)
};
// The computer keyboard: the transport keys, Enter and the caret, the editing keys of the Song and Pattern screens, and the piano keys.
// Returns true when the song was changed (the engine is brought up to date afterwards).
struct KeyboardEnv {
  int& octave;
  bool& follow;
  int& playFromRequest;
  std::string& selectedLane;
  std::map<ImGuiKey, int>& held;
  bool& requestSave;   // Ctrl+S (the File menu sets it too)
  bool playing;
};
// What the screens share while one frame is drawn.
struct FrameContext {
  Session* session;
  const Options& options;
  Player& player;
  Document& doc;
  CellEditor& editor;
  const Song& song;
  std::string& selectedLane;   // the lane whose tab is shown
  std::string& laneInstrument;   // the machine of that lane
  bool playing;
  int tick;   // the tick to show as the playhead
  bool& follow;
  int& playFromRequest;
  std::map<ImGuiKey, int>& held;   // the piano keys held down
  int& octave;
  int perBeat, perBar;
  float scale;
  float statusH;
  bool mixerShown;
};

// Where the song is, as the header works it out each frame.
struct HeaderState {
  bool playing;
  int tick;      // the tick to show as the playhead (as it is heard)
  int perBeat, perBar;   // ticks
};

// The functions the parts call in each other.
void SDLCALL dialogDone(void* userdata, const char* const* files, int count);
void drawMeter(const char* id, float level, ImVec2 size);
std::filesystem::path backupFolder();
std::string themeFile();
void saveTheme();
void loadTheme();
bool drawSplash(double seconds, const std::string& build);
void startExport(Session& session, const std::string& path);
Project blankProject();
void drawPatternMenu(Session* session, Document& doc, Player& player);
std::string laneMachine(const Document& doc, const std::string& laneId);
std::string changeNoteColumns(Session* session, Document& doc, Player& player, const std::string& machineId, int direction);
KeyFocus keyFocusOf(const Session& session);
bool enterPatternUnderCaret(Session* session, std::string& selectedLane);   // (keyboard.cpp) Enter, or a double-click, on a pattern in the sequence
bool handleKeyboard(Session* session, KeyboardEnv& env);   // (keyboard.cpp)
void drawSongScreen(FrameContext& f);   // (song_screen.cpp)
void drawPatternScreen(FrameContext& f);   // (pattern_screen.cpp)
void drawInstrumentScreen(FrameContext& f);   // (instrument_screen.cpp)
void drawInfoScreen(FrameContext& f);   // (info_screen.cpp)
void drawConfigScreen(FrameContext& f);   // (config_screen.cpp)
HeaderState drawHeader(Session* session, bool& follow, bool& requestSave, float& meterL, float& meterR);   // (header.cpp)
void drawRail(Session* session, float scale, float below);   // (header.cpp)
void drawBrowser(FrameContext& f, const Lane* listed, app::PatternListResult& listed2, float railGap, float below);   // (browser.cpp)

}  // namespace wax::tool

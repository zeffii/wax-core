// wax-app [song.wax] [--rate hz] [--threads n] [--midi n] [--null]
//         [--screenshot out.bmp [--frames n] [--play-from tick]]
// The window: the transport, the lanes of the song as pattern tables that follow the playhead, level meters, and
// keyjazz from the computer keyboard or a MIDI controller. Drop a .wax file on the window to open it.
// Without a sound card (or with --null) it still runs; --screenshot draws some frames, saves the last, and quits,
// which is how the window is looked at on a machine without a screen (SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software).
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app/pattern_view.h"
#include "app/theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui.h"
#include "live/audio_out.h"
#include "live/player.h"
#include "project/project.h"

namespace {

using namespace wax;

struct Options {
  std::string path, screenshot;
  double rate = 48000;
  int threads = 1, midiPort = 0, frames = 3, playFrom = -1;
  bool useNull = false;
};

// One song, loaded: the project, the engine that plays it, the sound card and the MIDI input.
struct Session {
  Project project;
  std::unique_ptr<Player> player;
  std::unique_ptr<AudioOut> audio;
  std::unique_ptr<RtMidiIn> midi;
  std::string midiName = "none";
  std::string keyjazzInstrument;
  std::vector<std::string> playable;                                       // the machines a keyboard can play
  std::map<std::string, std::vector<std::vector<std::string>>> rows;      // lane id -> flattened rows
  std::string audioName;
  bool pickedTab = false;   // the first lane that is not the master is shown at first

  Session(const std::string& path, const Options& options) : project(loadProject(path)) {
    for (const Instrument& inst : project.instruments) if (inst.id != kMaster && !inst.isMeta) playable.push_back(inst.id);
    keyjazzInstrument = playable.empty() ? std::string() : playable.front();
    player = std::make_unique<Player>(project, options.rate, options.threads, keyjazzInstrument, true);
    for (const Lane& lane : project.lanes) rows[lane.id] = app::flattenLane(lane, project.song.length);
    try {
      audio = std::make_unique<AudioOut>(*player, options.rate, options.useNull);
      audioName = audio->deviceName() + " (" + audio->backendName() + ")";
      audio->start();
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
  draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(30, 37, 44, 255), 2);
  const float db = level > 1e-5f ? 20.0f * std::log10(level) : -100.0f;
  const float fill = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
  draw->AddRectFilled(origin, ImVec2(origin.x + size.x * fill, origin.y + size.y),
                      db > -0.5f ? IM_COL32(220, 90, 70, 255) : app::kAccent, 2);
}

void drawFrame(Session* session, const std::string& status, float& meterL, float& meterR, std::map<ImGuiKey, int>& held,
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
  const Song& song = session->project.song;

  // The transport.
  const bool playing = player.playing();
  if (ImGui::Button(playing ? "Stop" : "Play", ImVec2(70, 0))) {
    if (playing) player.ring().push({LiveCommand::Stop, 0, 1});
    else player.ring().push({LiveCommand::Play, 0, 1});
  }
  ImGui::SameLine();
  ImGui::Checkbox("Follow", &follow);
  ImGui::SameLine(0, 24 * ImGui::GetStyle().FontScaleDpi);
  const int tick = player.tick();
  const int perBeat = std::max(1, song.ticksPerBeat), perBar = perBeat * std::max(1, song.beatsPerBar);
  ImGui::Text("%s   bar %d  beat %d  tick %d / %d   %.0f bpm", song.name.c_str(), tick / perBar + 1, tick % perBar / perBeat + 1,
              tick, song.length, song.bpm);
  ImGui::SameLine(ImGui::GetWindowWidth() - 260 * ImGui::GetStyle().FontScaleDpi);
  meterL = std::max(player.levelLeft(), meterL * 0.9f);   // falls back a little each frame
  meterR = std::max(player.levelRight(), meterR * 0.9f);
  ImGui::BeginGroup();
  drawMeter("##l", meterL, ImVec2(200 * ImGui::GetStyle().FontScaleDpi, 8));
  drawMeter("##r", meterR, ImVec2(200 * ImGui::GetStyle().FontScaleDpi, 8));
  ImGui::EndGroup();

  // Keyjazz.
  if (!session->playable.empty()) {
    ImGui::SetNextItemWidth(160);
    if (ImGui::BeginCombo("##keys", session->keyjazzInstrument.c_str())) {
      for (const std::string& id : session->playable)
        if (ImGui::Selectable(id.c_str(), id == session->keyjazzInstrument)) {
          player.ring().push({LiveCommand::AllOff, 0, 0});
          session->keyjazzInstrument = id;
          player.setInstrument(id);
        }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(app::kDimText), "keys: Z-M, Q-I   octave %d ([ ])   midi: %s   audio: %s",
                       octave, session->midiName.c_str(), session->audioName.c_str());
  }
  ImGui::Separator();

  // The computer keyboard plays notes unless a text box has it.
  ImGuiIO& io = ImGui::GetIO();
  if (!io.WantTextInput) {
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) octave = std::max(0, octave - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) octave = std::min(8, octave + 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
      if (playing) player.ring().push({LiveCommand::Stop, 0, 1});
      else player.ring().push({LiveCommand::Play, 0, 1});
    }
    for (int i = 0; i < static_cast<int>(sizeof kPianoKeys / sizeof kPianoKeys[0]); i++) {
      const ImGuiKey key = kPianoKeys[i];
      if (ImGui::IsKeyPressed(key, false) && !io.KeyCtrl && !io.KeyAlt) {
        const int note = std::clamp(12 * (octave + 1) + i, 0, 127);
        held[key] = note;
        player.ring().push({LiveCommand::NoteOn, note, 0.8});
      }
    }
  }
  for (auto it = held.begin(); it != held.end();) {   // a held key lets go when it is up (or the text box took the keyboard)
    if (!ImGui::IsKeyDown(it->first) || io.WantTextInput) {
      player.ring().push({LiveCommand::NoteOff, it->second, 0});
      it = held.erase(it);
    } else {
      ++it;
    }
  }

  // The lanes.
  if (ImGui::BeginTabBar("lanes")) {
    for (const Lane& lane : session->project.lanes) {
      const std::string label = lane.id + (lane.instrument == lane.id ? "" : "  " + lane.instrument);
      ImGuiTabItemFlags tabFlags = ImGuiTabItemFlags_None;
      if (!session->pickedTab && lane.id != kMaster) { tabFlags |= ImGuiTabItemFlags_SetSelected; session->pickedTab = true; }
      if (ImGui::BeginTabItem(label.c_str(), nullptr, tabFlags)) {
        selectedLane = lane.id;
        const app::PatternViewResult result = app::drawPatternView(
            session->rows[lane.id], session->player->engine().columnLabels(lane.id), song.ticksPerBeat, song.beatsPerBar,
            playing ? tick : -1, follow && playing);
        if (result.clickedTick >= 0) playFromRequest = result.clickedTick;
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }
  ImGui::End();
}

bool parse(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    const bool more = i + 1 < argc;
    if (a == "--rate" && more) o.rate = std::atof(argv[++i]);
    else if (a == "--threads" && more) o.threads = std::max(1, std::atoi(argv[++i]));
    else if (a == "--midi" && more) o.midiPort = std::atoi(argv[++i]);
    else if (a == "--screenshot" && more) o.screenshot = argv[++i];
    else if (a == "--frames" && more) o.frames = std::max(1, std::atoi(argv[++i]));
    else if (a == "--play-from" && more) o.playFrom = std::atoi(argv[++i]);
    else if (a == "--null") o.useNull = true;
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
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  std::unique_ptr<Session> session;
  std::string status;
  const auto open = [&](const std::string& path) {
    session.reset();   // the old sound stops before the new song is built
    try {
      session = std::make_unique<Session>(path, options);
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
  std::map<ImGuiKey, int> held;
  int octave = 4, playFromRequest = -1, frame = 0;
  std::string selectedLane;
  bool follow = true, running = true;
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
    ImGui::NewFrame();
    drawFrame(session.get(), status, meterL, meterR, held, octave, selectedLane, follow, playFromRequest);
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
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

// wax-play song.wax [options]
// Plays a song through the sound card, with no window: the same engine wax-render uses, driven by an audio callback.
// A MIDI keyboard plays the chosen instrument (keyjazz) while the song plays or stops. Commands from the keyboard
// of the terminal, one per line:  p play   s stop   q quit   n 60 [vel] note on   f 60 note off   i name instrument
//
//   --rate hz          sample rate (default 48000)
//   --threads n        render the instruments of a block on n threads
//   --instrument id    the machine the MIDI keyboard plays (default: the first that is not the master)
//   --midi n           MIDI input port (default: the first); --list-midi shows them
//   --no-midi          do not open a MIDI input
//   --loop             play the song over and over (default: once)
//   --no-play          wait for the p command instead of starting at once
//   --seconds s        quit after s seconds
//   --null             use miniaudio's null device (no sound card; time still runs): for tests and servers
//   --dry out.wav      no device at all: run the audio callback by hand for --seconds and write the result
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <memory>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "engine/engine.h"
#include "live/player.h"
#include "io/wav.h"
#include "miniaudio/miniaudio.h"
#include "project/project.h"
#include "rtmidi/RtMidi.h"

namespace {

using namespace wax;

void dataCallback(ma_device* device, void* output, const void*, ma_uint32 frames) {
  static_cast<Player*>(device->pUserData)->fill(static_cast<float*>(output), static_cast<int>(frames));
}

void usage() {
  std::printf("usage: wax-play song.wax [--rate hz] [--threads n] [--instrument id] [--midi n | --no-midi | --list-midi]\n"
              "                [--loop] [--no-play] [--seconds s] [--null] [--dry out.wav]\n");
}

}  // namespace

int main(int argc, char** argv) {
  std::string path, instrument, dryPath;
  double rate = 48000, seconds = 0;
  int threads = 1, midiPort = 0;
  bool loop = false, noPlay = false, useNull = false, noMidi = false, listMidi = false;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    const bool more = i + 1 < argc;
    if (a == "--rate" && more) rate = std::atof(argv[++i]);
    else if (a == "--threads" && more) threads = std::max(1, std::atoi(argv[++i]));
    else if (a == "--instrument" && more) instrument = argv[++i];
    else if (a == "--midi" && more) midiPort = std::atoi(argv[++i]);
    else if (a == "--seconds" && more) seconds = std::atof(argv[++i]);
    else if (a == "--dry" && more) dryPath = argv[++i];
    else if (a == "--no-midi") noMidi = true;
    else if (a == "--list-midi") listMidi = true;
    else if (a == "--loop") loop = true;
    else if (a == "--no-play") noPlay = true;
    else if (a == "--null") useNull = true;
    else if (!a.empty() && a[0] != '-' && path.empty()) path = a;
    else { usage(); return 1; }
  }

  if (listMidi) {
    try {
      RtMidiIn in;
      const unsigned n = in.getPortCount();
      if (n == 0) std::printf("no MIDI input ports\n");
      for (unsigned i = 0; i < n; i++) std::printf("%u: %s\n", i, in.getPortName(i).c_str());
    } catch (const RtMidiError& e) { std::printf("MIDI: %s\n", e.getMessage().c_str()); }
    return 0;
  }
  if (path.empty()) { usage(); return 1; }

  try {
    const Project project = loadProject(path);
    if (instrument.empty()) {
      for (const Instrument& inst : project.instruments)
        if (inst.id != "master" && !inst.isMeta) { instrument = inst.id; break; }
    }
    Player player(project, rate, threads, instrument, loop);
    for (const std::string& w : player.engine().warnings()) std::printf("note: %s\n", w.c_str());

    if (!dryPath.empty()) {   // no device: the callback by hand, in odd sizes, like a sound card's
      if (seconds <= 0) seconds = 10;
      StereoAudio audio;
      audio.sampleRate = rate;
      const long total = static_cast<long>(seconds * rate);
      std::vector<float> chunk(2 * 441);
      if (!noPlay) player.ring().push({LiveCommand::Play, 0, 1});
      long done = 0;
      int size = 97;
      while (done < total) {
        const int n = static_cast<int>(std::min<long>(size, total - done));
        player.fill(chunk.data(), n);
        for (int i = 0; i < n; i++) { audio.left.push_back(chunk[2 * i]); audio.right.push_back(chunk[2 * i + 1]); }
        done += n;
        size = 97 + (size * 7 + 13) % 300;
      }
      writeWav(dryPath, audio);
      std::printf("%.2f s, peak %.6f -> %s\n", seconds, player.peak(), dryPath.c_str());
      return 0;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = static_cast<ma_uint32>(rate);
    config.dataCallback = dataCallback;
    config.pUserData = &player;
    ma_context context;
    ma_backend nullBackend[] = {ma_backend_null};
    if (ma_context_init(useNull ? nullBackend : nullptr, useNull ? 1 : 0, nullptr, &context) != MA_SUCCESS) {
      std::printf("no audio backend could be started\n");
      return 2;
    }
    ma_device device;
    if (ma_device_init(&context, &config, &device) != MA_SUCCESS) {
      std::printf("no audio device could be opened\n");
      ma_context_uninit(&context);
      return 2;
    }
    std::printf("playing %s on %s (%s), %u Hz; keyjazz plays '%s'\n", project.song.name.c_str(), device.playback.name,
                ma_get_backend_name(context.backend), device.sampleRate, instrument.c_str());

    std::unique_ptr<RtMidiIn> midi;
    if (!noMidi) {
      try {
        midi = std::make_unique<RtMidiIn>();
        if (midi->getPortCount() > static_cast<unsigned>(midiPort)) {
          midi->openPort(midiPort);
          midi->setCallback(&midiCallback, &player);
          std::printf("MIDI in: %s\n", midi->getPortName(midiPort).c_str());
        } else {
          std::printf("no MIDI input\n");
        }
      } catch (const RtMidiError& e) { std::printf("MIDI: %s\n", e.getMessage().c_str()); }
    }

    if (!noPlay) player.ring().push({LiveCommand::Play, 0, 1});
    ma_device_start(&device);

    std::atomic<bool> quit{false};
    std::thread terminal([&] {
      std::string line;
      while (!quit && std::getline(std::cin, line)) {
        char cmd = 0; int note = 60; double vel = 100;
        char name[256] = {};
        const int got = std::sscanf(line.c_str(), " %c", &cmd);
        if (got != 1) continue;
        if (cmd == 'q') quit = true;
        else if (cmd == 'p') player.ring().push({LiveCommand::Play, 0, 1});
        else if (cmd == 's') player.ring().push({LiveCommand::Stop, 0, 1});
        else if (cmd == 'n') { std::sscanf(line.c_str(), " n %d %lf", &note, &vel); player.ring().push({LiveCommand::NoteOn, note, std::min(1.0, vel / 127.0)}); }
        else if (cmd == 'f') { std::sscanf(line.c_str(), " f %d", &note); player.ring().push({LiveCommand::NoteOff, note, 0}); }
        else if (cmd == 'i' && std::sscanf(line.c_str(), " i %255s", name) == 1) player.setInstrument(name);
      }
      quit = true;
    });
    terminal.detach();

    const auto started = std::chrono::steady_clock::now();
    while (!quit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (seconds > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >= seconds) break;
      if (!loop && !noPlay && seconds <= 0 && !player.engine().sequencer().playing() &&
          std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() > 1) {
        // The song has ended: let the tail ring out for a few seconds, then leave.
        static auto ended = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - ended).count() > 6) break;
      }
    }
    ma_device_uninit(&device);
    ma_context_uninit(&context);
    std::printf("peak %.6f\n", player.peak());
  } catch (const std::exception& e) {
    std::printf("error: %s\n", e.what());
    return 1;
  }
  return 0;
}

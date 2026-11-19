// FastTracker 2 module files (.xm) opened as a project: a lane on an `xmchan` machine for each channel, every sample of every
// instrument, and the arrangement played out. Port of webtracker src/project/xm.js; see nodes/xmchan.cpp for what the lanes play.
// As for MOD files in the web app, the song runs at 24 ticks to the beat, so the BPM is the XM BPM and one song tick is one XM tick;
// a row becomes `speed` ticks long.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "project/project.h"

namespace wax::app {

constexpr int kXmTicksPerBeat = 24;

struct XmCell {
  int note = 0, ins = 0, vol = 0, effect = 0, param = 0;
};

// An envelope as the file has it: ticks and values (0..64) as (x, y) pairs, and the sustain and loop points as point numbers or -1.
struct XmEnvelope {
  std::vector<int> pts;
  int sus = -1, ls = -1, le = -1;
};

struct XmSample {
  std::string name;
  std::vector<float> data;          // -1..1
  int volume = 64, finetune = 0, pan = 128, relNote = 0;
  int loopType = 0;                 // 0 none, 1 forward, 2 ping-pong
  int loopStart = 0, loopEnd = 0;   // in samples
};

struct XmInstrument {
  std::string name;
  std::vector<int> map = std::vector<int>(96, 0);   // the sample each note plays
  bool hasVol = false, hasPan = false;
  XmEnvelope vol, pan;
  int fade = 0;
  int vib[4] = {0, 0, 0, 0};        // type, sweep, depth, rate
  std::vector<XmSample> samples;
};

struct XmModule {
  std::string title;
  int channels = 0;
  bool linear = true;
  int speed = 6, bpm = 125, restart = 0;
  std::vector<int> orders;
  std::vector<std::vector<std::vector<XmCell>>> patterns;   // [pattern][row][channel]
  std::vector<XmInstrument> instruments;
};

// One row of the song as it sounds: a row can come up more than once (loops), and a jump leaves some out.
struct XmStep {
  int pattern = 0, row = 0, speed = 6, delay = 0;
  int tempo = 0;    // the BPM set on this row, or 0
  int gv = 64;      // the global volume (0..64) on the row's first tick
  int gslide = 0;   // what H adds to it on each later tick
};

// Throws std::runtime_error with a message for the user when the bytes are not a module this build can read.
XmModule parseXm(const std::vector<std::uint8_t>& bytes);

// Plays the song out in order, following jumps (B), breaks (D), pattern loops (E6x), pattern delays (EEx), the speed and BPM (F) and the
// global volume (G, H).
std::vector<XmStep> playOut(const XmModule& xm, int maxRows = 40000);

// The .wax file (as the web app writes it) for a module. `name` is the song's name when the module has no title.
Json xmToProjectFile(const XmModule& xm, const std::string& name);

// The module in this file, read and made into a project. Throws std::runtime_error with a message for the user.
Project projectFromXmFile(const std::string& path);

}  // namespace wax::app

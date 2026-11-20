// Impulse Tracker module files (.it) opened as a project: a lane on an `itchan` machine for each channel that sounds, every sample,
// and the arrangement played out. Port of webtracker src/project/it.js; see nodes/itchan.cpp for what the lanes play, and the web file
// for what is left to the arrangement and what is not played. As for XM files, the song runs at 24 ticks to the beat, so the BPM is the
// IT tempo and one song tick is one IT tick; a row becomes `speed` ticks long.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "project/project.h"

namespace wax::app {

constexpr int kItTicksPerBeat = 24;

// A cell as the file has it: note -1 / vol -1 / ins 0 / effect 0 when empty (note 0 is C-0, 254 the cut, 255 the key off).
struct ItCell {
  int note = -1, ins = 0, vol = -1, effect = 0, param = 0;
};

// An envelope: ticks and values (0..64, panning and pitch moved up by 32) as (x, y) pairs, and the loop and sustain points.
struct ItEnvelope {
  bool on = false;
  std::vector<int> pts;
  int ls = -1, le = -1, ss = -1, se = -1;
};

struct ItSample {
  bool valid = false;               // false: the file has no sample header for this slot
  std::string name;
  std::vector<float> data, right;   // -1..1; `right` stays empty for a mono sample
  int volume = 64, globalVolume = 64, pan = 0;   // pan: bit 7 set means the sample sets the panning (0..64 in the rest)
  std::uint32_t length = 0, loopStart = 0, loopEnd = 0, susStart = 0, susEnd = 0, rate = 8363;
  int vib[4] = {0, 0, 0, 0};        // speed, depth, rate, type
  bool loop = false, sus = false, pingpong = false, susPingpong = false;
};

struct ItInstrument {
  bool valid = false;               // false: the file has no instrument header for this slot
  int nna = 0, dct = 0, dca = 0, fade = 0, pps = 0, ppc = 60, gbv = 128;
  int pan = -1, ifc = -1, ifr = -1;   // -1: none
  std::vector<int> map, notes;        // for each of the 120 notes: the sample (0-based, -1 none) and the note it plays
  ItEnvelope vol, pan2, pit;
  bool filter = false;                // the pitch envelope moves the filter cutoff
};

struct ItModule {
  std::string title;
  int flags = 0, speed = 6, bpm = 125, gv = 128;
  std::vector<int> chanPan, chanVol, orders;
  std::vector<std::vector<std::vector<ItCell>>> patterns;   // [pattern][row][channel] (64 channels)
  std::vector<ItInstrument> instruments;
  std::vector<ItSample> samples;
  bool useInstruments = false, hasStereo = false;
};

// One row of the song as it sounds.
struct ItStep {
  int pattern = 0, row = 0, speed = 6, ticks = 6;   // ticks: the speed, times the pattern delay, plus any tick delay
  int tempo = 125;   // on the row's first tick
  int tslide = 0;    // what T0x / T1x adds on each later tick
  int gv = 128;      // the global volume (0..128) on the first tick
  int gslide = 0;    // what W adds on each later tick
};

// IT 2.14 / 2.15 sample decompression: `count` values (8 or 16 bits) from the compressed blocks at `at`.
std::vector<int> decompressIt(const std::vector<std::uint8_t>& bytes, std::size_t at, std::size_t count, bool sixteen, bool second);

// Throws std::runtime_error with a message for the user when the bytes are not a module this build can read.
ItModule parseIt(const std::vector<std::uint8_t>& bytes);

std::vector<ItStep> playOut(const ItModule& it, int maxRows = 40000);

// The .wax file (as the web app writes it) for a module. `name` is the song's name when the module has no title.
Json itToProjectFile(const ItModule& it, const std::string& name);

// The module in this file, read and made into a project. Throws std::runtime_error with a message for the user.
Project projectFromItFile(const std::string& path);

}  // namespace wax::app

// The settings screen's values, kept between runs in the user's preferences folder (config.json). The views read them from settings();
// the Config screen changes them. Each is the web app's setting of the same name where there is one.
#pragma once

#include <string>
#include <vector>

#include "imgui.h"

namespace wax::app {

struct Settings {
  // wire colours by kind of signal (the web's wireAudio, wireControl, wireStereo, wireParam) and whether control wires are dashed
  ImU32 wireAudio = IM_COL32(0x5e, 0x94, 0xc9, 255);
  ImU32 wireControl = IM_COL32(0xe0, 0x94, 0x29, 255);
  ImU32 wireStereo = IM_COL32(0xb1, 0x77, 0xc5, 255);
  ImU32 wireParam = IM_COL32(0xf4, 0x85, 0x7b, 255);
  bool wireDashed = true;
  // how the playing row is shown in the pattern editor: "marker" (a > by the tick and a tinted row), "line" (a thin line gliding through the
  // row, with the tint) or "band" (the tinted row alone)
  std::string playhead = "line";
  bool follow = true;   // the grids scroll with the playing row
  bool insertCut = false;   // Insert silence cuts a pattern that runs across the cursor row (off: it stays whole)
  bool patternAll = false;     // the Pattern screen shows every lane side by side (off: one lane, picked by its tab)
  static constexpr bool kNotePreviewDefault = true;   // never off by default
  bool notePreview = kNotePreviewDefault;   // a note typed into the grid sounds as it goes in
  float patternText = 1.0f;    // the Pattern grid's text size, as a multiple of the normal size
  bool seqNames = false;       // the Song screen's pattern cells show the pattern's name after its number (off: just the two-digit number; H toggles)
  bool hideEmpty = false;      // the Pattern screen leaves out columns with nothing in them
  bool tintPatterns = true;    // the Pattern screen tints each pattern's rows with the pattern's colour
  // editing (the web's rowWrap and laneWrap): Up/Down wrap round the first and last row; Left/Right go past the last column to the next lane
  // and, with laneWrap, from the last lane back to the first
  bool rowWrap = false, laneWrap = false;
  int threads = 1;   // threads that render the instruments (the calling thread counts); 1: all on one. The sound is the same whatever the number
  bool graphPullOut = false;   // the web's graphPullOut: dragging a node far out of its chain takes it out (Shift+drag always does)
  // the Sample screen (the web's sliceSensitivity, sliceMinGap, sliceReplace, sliceWhole, waveHide*, hexMode, sampleAskMB)
  int sliceSensitivity = 50, sliceMinGap = 30;
  bool sliceReplace = true, sliceWhole = false;
  bool waveHideLoop = false, waveHideSel = false, waveHideSus = false;
  std::string hexMode = "both";   // what a slice marker shows: "relative" (0000..FFFF through the sample), "absolute" (its frame) or "both"
  int sampleAskMB = 10;           // the sample browser asks before reading files over this many megabytes
  // automatic backups (the web's backupOn, backupMinutes, backupKeep)
  bool backupOn = true;
  int backupMinutes = 3, backupKeep = 30;
  std::vector<std::string> recentFiles;   // the File menu's recent songs, newest first (at most kRecentMax)
  static constexpr int kRecentMax = 10;
  int arrangementRow = 0;   // the Song screen's row size in ticks (Ctrl+wheel); 0: a bar
};

Settings& settings();
void loadSettings();
void saveSettings();
void resetSettings();
void rememberRecent(const std::string& path);   // puts a song at the top of the recent list and saves the settings

}  // namespace wax::app

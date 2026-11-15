// The settings screen's values, kept between runs in the user's preferences folder (config.json). The views read them from settings();
// the Config screen changes them. Each is the web app's setting of the same name where there is one.
#pragma once

#include <string>

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
  int arrangementRow = 0;   // the Song screen's row size in ticks (Ctrl+wheel); 0: a bar
};

Settings& settings();
void loadSettings();
void saveSettings();
void resetSettings();

}  // namespace wax::app

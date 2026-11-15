// The mixer, as in the web app: a strip for every lane that makes sound, each a fader (the gain of the instrument's Out node, in dB),
// the lane's M switch, and the Out node's mix and lead switches; the carve block at the left. Mouse only: it never takes the keyboard.
// (Solo and the test gain come later.)
#pragma once

#include <map>
#include <string>

#include "edit/document.h"
#include "engine/engine.h"

namespace wax::app {

struct MixerState {
  std::string draggingAddress;   // the parameter a drag is changing: its steps are one undo
  std::map<std::string, float> held;   // per instrument, the level drawn: it rises at once and falls away
};

// The height the dock wants.
float mixerHeight();

// Draws it. Edits go through `doc`; the caller then runs syncLanes and syncParams. Returns true when something changed.
bool drawMixer(MixerState& state, Document& doc, Engine& engine, const std::string& selectedLane);

}  // namespace wax::app

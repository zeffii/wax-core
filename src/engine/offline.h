// Rendering a song to sound, offline: the same runtime and sequencer a live player uses, run as fast as the computer
// allows. Port of renderSong() in webtracker src/audio/export.js and of the engine set-up in tools/wax-mcp/core.mjs.
#pragma once

#include <string>
#include <vector>

#include "io/wav.h"
#include "project/project.h"

namespace wax {

struct RenderOptions {
  double sampleRate = 44100;
  double tailSeconds = 6;       // rendered after the last row, so reverbs and delays can die out
  double maxSeconds = 7200;
  int threads = 1;              // render the instruments of a block side by side on this many threads; the sound is the same
};

struct RenderResult {
  StereoAudio audio;
  double peak = 0;
  std::vector<std::string> warnings;   // what the song uses that this engine does not do yet
};

// The song is played once from the top; when its last row has sounded the notes are released and the tail is rendered.
// Throws std::runtime_error when the project cannot be played (an unknown node type, a pattern that cannot be read).
RenderResult renderSong(const Project& project, const RenderOptions& options = {});

}  // namespace wax

// The sample browser: walk the folders of the computer, hear each sound file as it is picked, and load one into the project as a new sample.
// The Samples tab of the browser panel. A port of the web app's sample browser (src/ui/samplebrowser.js), without its folder-handle plumbing
// (a native window can read any folder). Mouse only here; a click on a file plays it, a double-click on a folder opens it, a double-click on a
// file loads it. Files over the size limit are not read until asked for.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "live/preview.h"

namespace wax::app {

struct SampleBrowserState {
  struct Entry {
    std::string name;
    bool folder = false;
    std::uintmax_t size = 0;
  };
  std::filesystem::path folder;   // the folder shown (empty until first drawn)
  std::vector<Entry> entries;     // folders first, then the sound files, each by name
  int selected = -1;
  std::string error;              // what went wrong with the last file, or empty

  // what the picked file is, once read
  std::shared_ptr<const PreviewClip> clip;
  std::vector<float> peaks;       // the waveform, as pairs of lowest and highest per column
  double rate = 0;
  bool stereo = false;
  bool tooBig = false;            // over the size limit and not yet asked for
};

struct SampleBrowserResult {
  bool load = false;                // the picked file should be loaded into the project
  std::filesystem::path file;
};

// Draws the browser in `height` pixels of the current window. `start` is the folder to open at first. `limitMB`: files over this size wait for a click.
SampleBrowserResult drawSampleBrowser(SampleBrowserState& state, Preview& preview, const std::filesystem::path& start, double limitMB);

// The sound files this reads: .wav (16, 24 or 32-bit float).
bool isSoundFile(const std::filesystem::path& file);

}  // namespace wax::app

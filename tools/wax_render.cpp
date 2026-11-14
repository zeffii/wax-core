// wax-render song.wax [out.wav] [--tail seconds]
// Renders a song offline to a 32-bit float WAV, the way webtracker's tools/render-ref.mjs does, so the two can be
// compared with wax-compare.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include "engine/offline.h"
#include "io/wav.h"
#include "project/project.h"

int main(int argc, char** argv) {
  std::vector<std::string> paths;
  wax::RenderOptions options;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--tail" && i + 1 < argc) options.tailSeconds = std::atof(argv[++i]);
    else paths.push_back(arg);
  }
  if (paths.empty()) {
    std::printf("usage: wax-render song.wax [out.wav] [--tail seconds]\n");
    return 1;
  }
  const std::string in = paths[0];
  const std::string out = paths.size() > 1 ? paths[1] : in.substr(0, in.rfind('.')) + ".native.wav";
  try {
    const wax::Project project = wax::loadProject(in);
    const auto started = std::chrono::steady_clock::now();
    const wax::RenderResult result = wax::renderSong(project, options);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    wax::writeWav(out, result.audio);
    const double seconds = static_cast<double>(result.audio.frames()) / result.audio.sampleRate;
    std::printf("%s: %.3f s, peak %.6f, rendered in %.2f s (%.0f times faster than it plays) -> %s\n",
                project.song.name.c_str(), seconds, result.peak, took, took > 0 ? seconds / took : 0.0, out.c_str());
    for (const std::string& warning : result.warnings) std::printf("  note: %s\n", warning.c_str());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "wax-render: %s\n", e.what());
    return 2;
  }
}

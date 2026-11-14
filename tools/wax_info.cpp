// wax-info song.wax
// What a song is made of, and how much of it the native engine can play so far: every node type the song uses,
// marked as ported or not.
#include <cstdio>
#include <exception>

#include "core/node_registry.h"
#include "project/project.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: wax-info song.wax\n");
    return 1;
  }
  try {
    const wax::Project project = wax::loadProject(argv[1]);
    const wax::NodeRegistry& nodes = wax::builtinNodes();

    std::printf("%s\n", project.song.name.c_str());
    std::printf("  %g bpm, %d ticks per beat, %d ticks long\n", project.song.bpm, project.song.ticksPerBeat, project.song.length);
    std::printf("  %zu instruments, %zu lanes\n\n", project.instruments.size(), project.lanes.size());

    for (const wax::Instrument& instrument : project.instruments) {
      std::printf("  %-12s %zu nodes, %d voice%s\n", instrument.id.c_str(), instrument.graph.nodes.size(), instrument.graph.voices, instrument.graph.voices == 1 ? "" : "s");
    }

    const auto types = project.nodeTypes();
    std::size_t ported = 0;
    std::printf("\n  node types:\n");
    for (const std::string& type : types) {
      const bool known = nodes.find(type) != nullptr;
      ported += known;
      std::printf("    [%c] %s\n", known ? 'x' : ' ', type.c_str());
    }
    std::printf("\n  %zu of %zu node types ported%s\n", ported, types.size(), ported == types.size() ? ": this song can be played" : "");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "wax-info: %s\n", e.what());
    return 2;
  }
}

#include "engine/offline.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/js_math.h"
#include "core/node_registry.h"
#include "engine/runtime.h"
#include "engine/sequencer.h"
#include "graph/compile.h"
#include "seq/lane.h"

namespace wax {
namespace {

constexpr int kBlock = 128;

// The parameter slots of the master lane's tempo node, once the master instrument is compiled.
MasterSlots findMasterSlots(const ParamRegistry& registry) {
  const auto slot = [&](const char* name) { return registry.slotOf(std::string("master.m.") + name); };
  MasterSlots m;
  m.bpm = slot("bpm"); m.swing = slot("swing"); m.tpb = slot("tpb"); m.volume = slot("volume");
  m.limit = slot("limit"); m.ceiling = slot("ceiling"); m.boost = slot("boost"); m.release = slot("release");
  m.carve = slot("carve"); m.cvamt = slot("cvamt"); m.cvdepth = slot("cvdepth"); m.cvfocus = slot("cvfocus");
  return m;
}

// Which machines each meta machine plays (its note and trigger columns), for the sidechains that listen to one.
std::map<std::string, std::vector<std::string>> metaSourcesOf(const Project& project) {
  std::map<std::string, std::vector<std::string>> map;
  for (const Instrument& instrument : project.instruments) {
    std::vector<std::string> played;
    for (const MetaColumn& c : instrument.metaColumns) {
      const bool plays = c.role == "note" || c.role == "rnote" || c.role == "trig";
      if (!plays || c.instrument.empty() || c.instrument == instrument.id || !project.findInstrument(c.instrument)) continue;
      if (std::find(played.begin(), played.end(), c.instrument) == played.end()) played.push_back(c.instrument);
    }
    if (!played.empty()) map[instrument.id] = played;
  }
  return map;
}

}  // namespace

RenderResult renderSong(const Project& project, const RenderOptions& options) {
  if (project.song.length <= 0) throw std::runtime_error("There is no song to render.");
  const NodeRegistry& nodeTypes = builtinNodes();
  RenderResult result;

  // Every instrument is compiled, in the order of the file (the master first), which gives the parameters their slots.
  ParamRegistry registry;
  GraphRuntime runtime(options.sampleRate, registry.values(), nodeTypes, kBlock);
  runtime.setDelayComp(false);
  for (const Instrument& instrument : project.instruments) {
    try {
      runtime.load(compileGraph(instrument.graph, instrument.id, nodeTypes, registry));
    } catch (const std::runtime_error& e) {
      throw std::runtime_error("instrument \"" + instrument.id + "\": " + e.what());
    }
  }
  runtime.setMetaSources(metaSourcesOf(project));

  Sequencer sequencer(options.sampleRate, runtime);
  SongSettings song;
  song.bpm = project.song.bpm;
  song.ticksPerBeat = project.song.ticksPerBeat;
  song.length = project.song.length;
  song.swing = project.song.swing;
  song.beatsPerBar = project.song.beatsPerBar;
  song.master = findMasterSlots(registry);
  sequencer.setSong(song);
  for (const PreparedLane& lane : prepareSong(project, registry)) sequencer.setLane(lane);

  if (project.song.raw.contains("nodeseq") && !project.song.raw["nodeseq"].value("nodes", Json::array()).empty()) result.warnings.push_back("the Node Sequencer is not ported yet: what it plays is missing");
  for (const Lane& lane : project.lanes) {
    if (!lane.raw.value("curves", Json::array()).empty()) { result.warnings.push_back("lane curves (automation) are not ported yet: they are not applied"); break; }
  }

  // With delay compensation on, the whole mix comes out late by `lag` samples: the start is trimmed off below.
  const int lag = runtime.alignment() + runtime.carveLatency();
  const std::size_t limit = static_cast<std::size_t>(std::floor(options.sampleRate * options.maxSeconds));
  double tailLeft = jsRound(options.tailSeconds * options.sampleRate) + lag;
  std::vector<float> left, right;
  float blockLeft[kBlock], blockRight[kBlock];
  sequencer.play(0, true);
  while (left.size() < limit) {
    if (!sequencer.playing() && tailLeft <= 0) break;
    sequencer.process(blockLeft, blockRight, kBlock);
    if (!sequencer.playing()) tailLeft -= kBlock;
    for (int i = 0; i < kBlock; i++) result.peak = std::max({result.peak, static_cast<double>(std::abs(blockLeft[i])), static_cast<double>(std::abs(blockRight[i]))});
    left.insert(left.end(), blockLeft, blockLeft + kBlock);
    right.insert(right.end(), blockRight, blockRight + kBlock);
  }
  if (runtime.sawUnported()) result.warnings.push_back("the mixer's carve stage is not ported yet: the song switches it on");

  const std::size_t skip = std::min(static_cast<std::size_t>(lag), left.size());
  result.audio.sampleRate = static_cast<int>(options.sampleRate);
  result.audio.left.assign(left.begin() + static_cast<std::ptrdiff_t>(skip), left.end());
  result.audio.right.assign(right.begin() + static_cast<std::ptrdiff_t>(skip), right.end());
  return result;
}

}  // namespace wax

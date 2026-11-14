#include "engine/engine.h"

#include <stdexcept>

#include "core/node_registry.h"
#include "project/samples.h"
#include "seq/lane.h"

namespace wax {
namespace {

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

// What the names in the Node Sequencer's graph mean on this machine: a `set` node's parameter slot and range, and the
// slots of the eight group columns. Port of resolveNodeseq() in webtracker src/nodeseq/resolve.js.
nodeseq::Resolve resolveNodeseq(const Json& graph, const Project& project, const ParamRegistry& registry) {
  nodeseq::Resolve resolve;
  for (const Json& node : graph.value("nodes", Json::array())) {
    if (node.value("type", std::string()) != "set") continue;
    const Json data = node.value("data", Json::object());
    const std::string target = data.is_object() ? data.value("target", std::string()) : std::string();
    const int slot = target.empty() ? -1 : registry.slotOf(target);
    if (slot < 0) continue;
    // the target is "instrument.node.param"; the column runs on a log scale when the instrument shows it that way
    const std::size_t first = target.find('.'), second = target.find('.', first + 1);
    const std::string instrumentId = target.substr(0, first), nodeId = target.substr(first + 1, second - first - 1), param = target.substr(second + 1);
    bool shownAsLog = false;
    if (const Instrument* instrument = project.findInstrument(instrumentId)) {
      for (const Exposed& e : instrument->exposed) if (e.node == nodeId && e.param == param) { shownAsLog = e.log; break; }
    }
    const ParamMeta& meta = registry.meta(slot);
    resolve.slots[node.value("id", std::string())] = {slot, meta.min, meta.max, shownAsLog && meta.min > 0, meta.isInt};
  }
  for (std::size_t g = 0; g < resolve.groupSlots.size(); g++) resolve.groupSlots[g] = registry.slotOf("graph.g.g" + std::to_string(g + 1));
  return resolve;
}

}  // namespace

Engine::Engine(const Project& project, double sampleRate, int threads)
    : registry_(), runtime_(sampleRate, registry_.values(), builtinNodes(), kEngineBlock), sequencer_(sampleRate, runtime_) {
  const NodeRegistry& nodeTypes = builtinNodes();
  runtime_.setDelayComp(false);
  runtime_.setThreads(threads);
  for (const Instrument& instrument : project.instruments) instrumentIds_.push_back(instrument.id);
  // Every instrument is compiled, in the order of the file (the master first), which gives the parameters their slots.
  const auto samples = decodeSamples(project.samples);
  for (const Instrument& instrument : project.instruments) {
    try {
      runtime_.load(compileGraph(graphWithSamplerStereo(instrument, samples), instrument.id, nodeTypes, registry_));
    } catch (const std::runtime_error& e) {
      throw std::runtime_error("instrument \"" + instrument.id + "\": " + e.what());
    }
    for (const GraphNode& node : instrument.graph.nodes) {   // the samples each node plays, one per slot
      const std::vector<std::string> ids = sampleSlots(instrument, node.id);
      for (std::size_t slot = 0; slot < ids.size(); slot++) {
        const auto found = samples.find(ids[slot]);
        if (found == samples.end()) continue;
        auto message = std::make_shared<SampleMessage>(*found->second);
        message->slot = static_cast<int>(slot);
        runtime_.sendSample(instrument.id, node.id, message);
      }
    }
  }
  runtime_.setMetaSources(metaSourcesOf(project));

  SongSettings song;
  song.bpm = project.song.bpm;
  song.ticksPerBeat = project.song.ticksPerBeat;
  song.length = project.song.length;
  song.swing = project.song.swing;
  song.beatsPerBar = project.song.beatsPerBar;
  song.master = findMasterSlots(registry_);
  sequencer_.setSong(song);
  for (const PreparedLane& lane : prepareSong(project, registry_)) {
    sequencer_.setLane(lane);
    std::vector<std::string>& names = labels_[lane.id];
    for (const Column& column : lane.columns) {
      switch (column.kind) {
        case ColumnKind::Note: names.push_back("N" + std::to_string(column.nc + 1)); break;
        case ColumnKind::Vel: names.push_back("V" + std::to_string(column.nc + 1)); break;
        case ColumnKind::Trig: names.push_back("T" + std::to_string(column.tc + 1)); break;
        case ColumnKind::Param: names.push_back(column.param); break;
        case ColumnKind::Cmd: names.push_back("CMD"); break;
        case ColumnKind::Spacer: names.push_back(""); break;
      }
    }
  }

  if (project.song.raw.contains("nodeseq") && project.song.raw["nodeseq"].is_object()) {
    const Json& graph = project.song.raw["nodeseq"];
    sequencer_.setNodeseq(graph, resolveNodeseq(graph, project, registry_));
    for (const std::string& type : sequencer_.nodeseq().unknownTypes()) {
      warnings_.push_back("the Node Sequencer's \"" + type + "\" node is not in this engine: what it does is missing");
    }
  }

}

const std::vector<std::string>& Engine::columnLabels(const std::string& laneId) const {
  static const std::vector<std::string> none;
  const auto found = labels_.find(laneId);
  return found == labels_.end() ? none : found->second;
}

}  // namespace wax

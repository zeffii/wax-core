#include "project/project.h"

#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#include "project/master.h"

namespace wax {
namespace {

constexpr const char* kFormat = "webtracker";
constexpr int kVersion = 1;

std::runtime_error problem(const std::string& message) { return std::runtime_error(message); }

std::string text(const Json& object, const char* key) { return object.value(key, std::string()); }

Song readSong(const Json& data) {
  Song song;
  song.raw = data;
  song.name = data.value("name", song.name);
  song.bpm = data.value("bpm", song.bpm);
  song.ticksPerBeat = data.value("ticksPerBeat", song.ticksPerBeat);
  song.length = data.value("length", song.length);
  song.swing = data.value("swing", song.swing);
  song.beatsPerBar = data.value("beatsPerBar", song.beatsPerBar);
  return song;
}

GraphNode readNode(const Json& n) {
  GraphNode node;
  node.id = n.at("id").get<std::string>();
  node.type = n.at("type").get<std::string>();
  if (node.type == "busin") node.type = "bus-in";   // build 748 called it busin
  node.params = n.value("params", Json::object());
  node.ports = n.value("ports", std::vector<std::string>());
  node.stereo = n.value("stereo", false);
  node.alias = text(n, "alias");
  node.source = text(n, "source");
  node.parts = n.value("parts", Json());
  node.data = n.value("data", Json());
  if (n.contains("x") && n["x"].is_number() && n.contains("y") && n["y"].is_number()) { node.x = n["x"].get<double>(); node.y = n["y"].get<double>(); node.placed = true; }
  return node;
}

Graph readGraph(const Json& data) {
  Graph graph;
  for (const Json& n : data.value("nodes", Json::array())) graph.nodes.push_back(readNode(n));
  for (const Json& c : data.value("connections", Json::array())) {
    graph.connections.push_back({c.at(0).get<std::string>(), c.at(1).get<std::string>()});
  }
  graph.voices = data.value("voices", 1);
  graph.neverSleeps = text(data, "sleep") == "never";
  return graph;
}

Instrument readInstrument(const std::string& id, const Json& data) {
  Instrument instrument;
  instrument.id = id;
  instrument.raw = data;
  instrument.graph = readGraph(data.value("graph", Json::object()));
  instrument.noteColumns = data.value("noteColumns", 1);
  instrument.noVel = data.value("noVel", false);

  for (const Json& e : data.value("exposed", Json::array())) {
    instrument.exposed.push_back({text(e, "node"), text(e, "param"), e.value("bits", 8), text(e, "scale") == "log"});
  }
  for (const Json& c : data.value("commands", Json::array())) {
    Command command;
    command.id = c.value("id", 0);
    command.node = text(c, "node");
    command.param = text(c, "param");
    command.log = text(c, "scale") == "log";
    if (c.contains("fixed") && c["fixed"].is_number()) command.fixed = c["fixed"].get<double>();
    command.reset = c.value("reset", 0);
    command.setting = text(c, "setting");
    instrument.commands.push_back(std::move(command));
  }
  const Json triggers = data.value("triggers", Json::array());
  for (std::size_t i = 0; i < triggers.size(); i++) {
    instrument.triggers.push_back({triggers[i].value("note", static_cast<int>(i))});
  }
  if (data.contains("meta")) {
    instrument.isMeta = true;
    for (const Json& m : data["meta"].value("columns", Json::array())) {
      MetaColumn column;
      column.id = text(m, "id");
      column.instrument = text(m, "instrument");
      column.role = text(m, "role");
      column.nc = m.value("nc", 0);
      column.tc = m.value("tc", 0);
      column.node = text(m, "node");
      column.param = text(m, "param");
      column.bits = m.value("bits", 0);
      column.root = m.value("root", 60);
      instrument.metaColumns.push_back(std::move(column));
    }
  }
  return instrument;
}

Lane readLane(const Json& data) {
  Lane lane;
  lane.id = data.at("id").get<std::string>();
  lane.instrument = data.at("instrument").get<std::string>();
  lane.raw = data;
  const Json patterns = data.value("patterns", Json::object());   // (named, so it outlives the loop over its items)
  for (const auto& [id, p] : patterns.items()) {
    Pattern pattern;
    pattern.id = id;
    pattern.name = p.value("name", id);
    pattern.index = p.value("index", 0);
    pattern.rows = p.value("rows", std::vector<std::string>());
    lane.patterns.push_back(std::move(pattern));
  }
  for (const Json& p : data.value("placements", Json::array())) {
    lane.placements.push_back({p.at("puid").get<std::string>(), p.value("start", 0), p.value("length", 0)});
  }
  lane.mutes = data.value("mutes", std::vector<int>());
  lane.offs = data.value("offs", std::vector<int>());
  lane.silenced = data.value("silenced", false);
  return lane;
}

}  // namespace

double GraphNode::param(const std::string& name, double fallback) const {
  const auto found = params.find(name);
  if (found == params.end()) return fallback;
  if (found->is_number()) return found->get<double>();
  if (found->is_boolean()) return found->get<bool>() ? 1.0 : 0.0;
  return fallback;
}

const Instrument* Project::findInstrument(const std::string& id) const {
  for (const Instrument& instrument : instruments) if (instrument.id == id) return &instrument;
  return nullptr;
}

std::vector<std::string> Project::nodeTypes() const {
  std::set<std::string> types;
  for (const Instrument& instrument : instruments) for (const GraphNode& node : instrument.graph.nodes) types.insert(node.type);
  return {types.begin(), types.end()};
}

Instrument instrumentFromJson(const std::string& id, const Json& data) { return readInstrument(id, data); }

Lane blankLane(const std::string& laneId, const std::string& instrumentId) {
  Json data = Json::object();
  data["id"] = laneId;
  data["instrument"] = instrumentId;
  data["patterns"] = Json::object();
  data["placements"] = Json::array();
  data["mutes"] = Json::array();
  data["offs"] = Json::array();
  data["curves"] = Json::array();
  return readLane(data);
}

Project parseProject(const std::string& fileText) {
  Json data;
  try {
    data = Json::parse(fileText);
  } catch (const Json::parse_error&) {
    throw problem("Not a webtracker file (invalid JSON).");
  }
  if (!data.is_object() || text(data, "format") != kFormat) throw problem("Not a webtracker file.");
  if (const int version = data.value("version", 0); version != kVersion) {
    throw problem("Unsupported file version " + std::to_string(version) + "; this build reads version " + std::to_string(kVersion) + ".");
  }

  Project project;
  try {
    if (!data.contains("instruments")) data["instruments"] = Json::object();
    ensureMaster(data);
    project.song = readSong(data.at("song"));
    const Json& instruments = data.at("instruments");
    for (const auto& [id, def] : instruments.items()) project.instruments.push_back(readInstrument(id, def));
    for (const Json& lane : data.at("lanes")) project.lanes.push_back(readLane(lane));
    project.samples = data.value("samples", Json::object());
    project.file = data;
  } catch (const Json::exception& e) {
    throw problem(std::string("The file is damaged: ") + e.what());
  }

  for (const Lane& lane : project.lanes) {
    if (!project.findInstrument(lane.instrument)) {
      throw problem("Lane \"" + lane.id + "\" uses unknown instrument \"" + lane.instrument + "\".");
    }
  }
  return project;
}

Project loadProject(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw problem("Cannot open \"" + path + "\".");
  std::ostringstream contents;
  contents << file.rdbuf();
  return parseProject(contents.str());
}

}  // namespace wax

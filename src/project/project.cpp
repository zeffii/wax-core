#include "project/project.h"

#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace wax {
namespace {

constexpr const char* kFormat = "webtracker";
constexpr int kVersion = 1;

std::runtime_error problem(const std::string& message) { return std::runtime_error(message); }

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

Graph readGraph(const Json& data) {
  Graph graph;
  for (const Json& n : data.value("nodes", Json::array())) {
    GraphNode node;
    node.id = n.at("id").get<std::string>();
    node.type = n.at("type").get<std::string>();
    if (node.type == "busin") node.type = "bus-in";   // build 748 called it busin
    node.params = n.value("params", Json::object());
    node.raw = n;
    graph.nodes.push_back(std::move(node));
  }
  for (const Json& c : data.value("connections", Json::array())) {
    graph.connections.push_back({c.at(0).get<std::string>(), c.at(1).get<std::string>()});
  }
  graph.voices = data.value("voices", 1);
  graph.neverSleeps = data.value("sleep", std::string()) == "never";
  return graph;
}

Instrument readInstrument(const std::string& id, const Json& data) {
  Instrument instrument;
  instrument.id = id;
  instrument.raw = data;
  instrument.graph = readGraph(data.value("graph", Json::object()));
  instrument.noteColumns = data.value("noteColumns", 1);
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
  return lane;
}

}  // namespace

const Instrument* Project::findInstrument(const std::string& id) const {
  for (const Instrument& instrument : instruments) if (instrument.id == id) return &instrument;
  return nullptr;
}

std::vector<std::string> Project::nodeTypes() const {
  std::set<std::string> types;
  for (const Instrument& instrument : instruments) for (const GraphNode& node : instrument.graph.nodes) types.insert(node.type);
  return {types.begin(), types.end()};
}

Project parseProject(const std::string& text) {
  Json data;
  try {
    data = Json::parse(text);
  } catch (const Json::parse_error&) {
    throw problem("Not a webtracker file (invalid JSON).");
  }
  if (!data.is_object() || data.value("format", std::string()) != kFormat) throw problem("Not a webtracker file.");
  if (const int version = data.value("version", 0); version != kVersion) {
    throw problem("Unsupported file version " + std::to_string(version) + "; this build reads version " + std::to_string(kVersion) + ".");
  }

  Project project;
  try {
    project.song = readSong(data.at("song"));
    const Json& instruments = data.at("instruments");
    for (const auto& [id, def] : instruments.items()) project.instruments.push_back(readInstrument(id, def));
    for (const Json& lane : data.value("lanes", Json::array())) project.lanes.push_back(readLane(lane));
    project.samples = data.value("samples", Json::object());
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
  std::ostringstream text;
  text << file.rdbuf();
  return parseProject(text.str());
}

}  // namespace wax

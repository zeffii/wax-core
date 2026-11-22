#include "project/save.h"
#include "project/samples.h"
#include "core/js_math.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace wax {
namespace {

Json laneJson(const Lane& lane) {
  Json out = lane.raw.is_object() ? lane.raw : Json::object();
  out["id"] = lane.id;
  out["instrument"] = lane.instrument;
  if (lane.silenced) out["silenced"] = true;
  else out.erase("silenced");

  const Json oldPatterns = out.value("patterns", Json::object());
  Json patterns = Json::object();
  for (const Pattern& pattern : lane.patterns) {
    Json p = oldPatterns.contains(pattern.id) ? oldPatterns[pattern.id] : Json::object();
    p["name"] = pattern.name;
    p["index"] = pattern.index;
    p.erase("length");
    if (pattern.color.is_null()) p.erase("color"); else p["color"] = pattern.color;
    p["rows"] = pattern.rows;
    patterns[pattern.id] = std::move(p);
  }
  out["patterns"] = std::move(patterns);

  const Json oldPlacements = out.value("placements", Json::array());
  Json placements = Json::array();
  for (const Placement& placement : lane.placements) {
    Json p = Json::object();
    for (const Json& old : oldPlacements) {
      if (old.value("puid", std::string()) == placement.pattern && old.value("start", -1) == placement.start) { p = old; break; }
    }
    p["puid"] = placement.pattern;
    p["start"] = placement.start;
    p["length"] = placement.length;
    placements.push_back(std::move(p));
  }
  out["placements"] = std::move(placements);
  for (const char* key : {"mutes", "offs"}) {   // written when there are some, or the file had the list
    const std::vector<int>& ticks = key[0] == 'm' ? lane.mutes : lane.offs;
    if (!ticks.empty() || out.contains(key)) out[key] = ticks;
  }
  return out;
}

}  // namespace

std::string serializeProject(const Project& project) {
  Json out = project.file.is_object() ? project.file : Json::object();
  Json lanes = Json::array();
  for (const Lane& lane : project.lanes) lanes.push_back(laneJson(lane));
  out["lanes"] = std::move(lanes);
  if (project.samples.is_object()) {   // the samples as edited (the tap pool's slots are not kept, as in the web app)
    Json samples = Json::object();
    for (const auto& [id, sample] : project.samples.items()) if (!isPoolSample(id)) samples[id] = sample;
    out["samples"] = std::move(samples);
  }
  // instruments taken away while editing (with their lane) leave the file too
  if (out.contains("instruments") && out["instruments"].is_object()) {
    std::vector<std::string> gone;
    for (const auto& [id, def] : out["instruments"].items()) {
      bool kept = false;
      for (const Instrument& i : project.instruments) kept |= i.id == id;
      if (!kept) gone.push_back(id);
    }
    for (const std::string& id : gone) out["instruments"].erase(id);
  }
  // instruments added while editing: written as they were made
  for (const Instrument& instrument : project.instruments) {
    if (!out.contains("instruments") || !out["instruments"].is_object()) out["instruments"] = Json::object();
    if (!out["instruments"].contains(instrument.id) && instrument.raw.is_object()) out["instruments"][instrument.id] = instrument.raw;
  }
  // node parameters as edited
  for (const Instrument& instrument : project.instruments) {
    if (!out.contains("instruments") || !out["instruments"].contains(instrument.id)) continue;
    Json& definition = out["instruments"][instrument.id];
    if (instrument.raw.is_object() && instrument.raw.contains("presets")) definition["presets"] = instrument.raw["presets"];   // presets as edited
    else definition.erase("presets");
    if (instrument.previewMs > 0) definition["previewMs"] = instrument.previewMs; else definition.erase("previewMs");
    if (instrument.noteColumns != definition.value("noteColumns", 1) && instrument.noteColumns > 0) definition["noteColumns"] = instrument.noteColumns;   // tracks as edited
    {   // the parameters shown as pattern columns, as edited: entries the file has keep what it holds about them
      Json shown = Json::array();
      const Json fileExposed = definition.contains("exposed") ? definition["exposed"] : Json::array();
      for (const Exposed& e : instrument.exposed) {
        Json entry = Json::object();
        for (const Json& candidate : fileExposed) if (candidate.value("node", std::string()) == e.node && candidate.value("param", std::string()) == e.param) entry = candidate;
        entry["node"] = e.node;
        entry["param"] = e.param;
        if (!e.label.empty()) entry["label"] = e.label;
        if (!e.longName.empty()) entry["long"] = e.longName;
        if (e.bits != 8) entry["bits"] = e.bits; else entry.erase("bits");
        if (e.log) entry["scale"] = "log"; else entry.erase("scale");
        shown.push_back(std::move(entry));
      }
      if (shown != fileExposed) definition["exposed"] = std::move(shown);
    }
    Json& graph = definition["graph"];
    if (graph.is_object()) {   // voices and sleep as edited
      if (instrument.graph.voices != 1 || graph.contains("voices")) graph["voices"] = instrument.graph.voices;
      if (instrument.graph.neverSleeps) graph["sleep"] = "never"; else graph.erase("sleep");
    }
    if (!graph.is_object()) continue;
    // the nodes as edited: those the file has keep what it holds about them (and their place in the list), new ones are written out,
    // deleted ones leave
    const Json fileNodes = graph.contains("nodes") ? graph["nodes"] : Json::array();
    Json nodes = Json::array();
    for (const GraphNode& edited : instrument.graph.nodes) {
      Json node = Json::object();
      bool inFile = false;
      for (const Json& candidate : fileNodes) if (candidate.value("id", std::string()) == edited.id) { node = candidate; inFile = true; }
      if (!inFile) { node["id"] = edited.id; node["type"] = edited.type; }
      if (edited.params.is_object() && (!edited.params.empty() || node.contains("params"))) node["params"] = edited.params;
      if (!edited.ports.empty()) node["ports"] = edited.ports; else node.erase("ports");
      if (!inFile) {
        if (!edited.alias.empty()) node["alias"] = edited.alias;
        if (edited.stereo) node["stereo"] = true;
      }
      if (!edited.source.empty()) node["source"] = edited.source;
      if (!edited.parts.is_null()) node["parts"] = edited.parts;
      if (edited.keepView) node["keepView"] = true; else node.erase("keepView");
      if (edited.placed) { node["x"] = edited.x; node["y"] = edited.y; }
      if (edited.data.is_object() && !(node.contains("data") && node["data"] == edited.data)) node["data"] = edited.data;
      nodes.push_back(std::move(node));
    }
    graph["nodes"] = std::move(nodes);
    // the wiring as edited (left as the file has it when nothing changed)
    Json wires = Json::array();
    for (const Connection& c : instrument.graph.connections) wires.push_back(Json::array({c.from, c.to}));
    if (!graph.contains("connections") || graph["connections"] != wires) graph["connections"] = std::move(wires);
  }
  // the instruments in the order of the list (moving one changes it)
  if (out.contains("instruments") && out["instruments"].is_object()) {
    Json ordered = Json::object();
    for (const Instrument& instrument : project.instruments) if (out["instruments"].contains(instrument.id)) ordered[instrument.id] = out["instruments"][instrument.id];
    for (const auto& [id, def] : out["instruments"].items()) if (!ordered.contains(id)) ordered[id] = def;
    out["instruments"] = ordered;
  }
  // the song's tempo settings are the master tempo node's parameters as edited (the header's bpm, swing and ticks per beat);
  // a setting is written only when it differs from what the file holds, so a song that was not touched is saved as it was
  for (const Instrument& instrument : project.instruments) {
    if (instrument.id != "master" || !out.contains("song")) continue;
    Json& song = out["song"];
    for (const GraphNode& node : instrument.graph.nodes) {
      if (node.id != "m" || !node.params.is_object()) continue;
      const auto number = [&](const char* name) { return node.params.contains(name) && node.params[name].is_number() ? node.params[name].get<double>() : 0.0; };
      if (node.params.contains("bpm") && number("bpm") != song.value("bpm", 120.0)) song["bpm"] = number("bpm");
      if (node.params.contains("swing") && number("swing") != jsRound(song.value("swing", 0.5) * 100)) song["swing"] = number("swing") / 100.0;
      if (node.params.contains("tpb") && number("tpb") != song.value("ticksPerBeat", 4)) song["ticksPerBeat"] = number("tpb");
    }
    if (project.song.name != song.value("name", project.song.name)) song["name"] = project.song.name;
    if (project.song.length != song.value("length", project.song.length)) song["length"] = project.song.length;
    if (project.song.beatsPerBar != song.value("beatsPerBar", 4)) song["beatsPerBar"] = project.song.beatsPerBar;
    // the loop as edited (set, switched off, or taken away)
    if (project.song.raw.is_object() && project.song.raw.contains("info") && project.song.raw["info"].is_string()) song["info"] = project.song.raw["info"];
    else song.erase("info");
    if (project.song.raw.is_object() && project.song.raw.contains("loop")) song["loop"] = project.song.raw["loop"];
    else song.erase("loop");
  }
  return out.dump(1);
}

void saveProject(const Project& project, const std::string& path) {
  const std::string text = serializeProject(project);
  const std::string temporary = path + ".saving";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("Cannot write \"" + path + "\".");
    file << text;
    file.flush();
    if (!file) throw std::runtime_error("Could not write \"" + path + "\" (is the disk full?).");
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    throw std::runtime_error("Cannot replace \"" + path + "\".");
  }
}

}  // namespace wax

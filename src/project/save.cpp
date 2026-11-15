#include "project/save.h"

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
  return out;
}

}  // namespace

std::string serializeProject(const Project& project) {
  Json out = project.file.is_object() ? project.file : Json::object();
  Json lanes = Json::array();
  for (const Lane& lane : project.lanes) lanes.push_back(laneJson(lane));
  out["lanes"] = std::move(lanes);
  // node parameters as edited
  for (const Instrument& instrument : project.instruments) {
    if (!out.contains("instruments") || !out["instruments"].contains(instrument.id)) continue;
    Json& graph = out["instruments"][instrument.id]["graph"];
    if (!graph.is_object() || !graph.contains("nodes")) continue;
    for (Json& node : graph["nodes"]) {
      for (const GraphNode& edited : instrument.graph.nodes) {
        if (edited.id != node.value("id", std::string())) continue;
        if (edited.params.is_object() && (!edited.params.empty() || node.contains("params"))) node["params"] = edited.params;
        if (edited.placed) { node["x"] = edited.x; node["y"] = edited.y; }
        if (edited.data.is_object() && !(node.contains("data") && node["data"] == edited.data)) node["data"] = edited.data;
      }
    }
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

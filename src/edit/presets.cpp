#include "edit/presets.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <stdexcept>

#include "core/node_registry.h"

namespace wax {

std::string presetLabel(int id) {
  char text[16];
  std::snprintf(text, sizeof text, "%02X", id);
  return text;
}

Json capturePreset(const Instrument& instrument, const std::function<std::optional<double>(const std::string&)>& live) {
  Json values = Json::object();
  for (const GraphNode& node : instrument.graph.nodes) {
    const NodeType* type = builtinNodes().find(node.type);
    if (!type) continue;
    for (const ParamSpec& spec : type->params) {
      const std::optional<double> now = live(instrument.id + "." + node.id + "." + spec.name);
      values[node.id + "." + spec.name] = now ? *now : node.param(spec.name, spec.def);
    }
  }
  return values;
}

namespace {
Json listOf(const Instrument& instrument) {
  return instrument.raw.is_object() && instrument.raw.contains("presets") ? instrument.raw["presets"] : Json::array();
}

Json& presetNamed(Json& list, int id) {
  for (Json& p : list) if (p.value("id", -1) == id) return p;
  throw std::out_of_range("No preset number " + std::to_string(id) + ".");
}
}  // namespace

Json presetsWithAdded(const Instrument& instrument, const std::string& name, const Json& values, int& id) {
  Json list = listOf(instrument);
  std::set<int> used;
  for (const Json& p : list) used.insert(p.value("id", -1));
  id = 0;
  while (used.count(id)) id++;
  Json preset = Json::object();
  preset["id"] = id;
  preset["name"] = name.empty() ? "preset " + presetLabel(id) : name.substr(0, 32);
  preset["values"] = values;
  list.push_back(preset);
  std::stable_sort(list.begin(), list.end(), [](const Json& a, const Json& b) { return a.value("id", 0) < b.value("id", 0); });
  return list;
}

Json presetsWithValues(const Instrument& instrument, int id, const Json& values) {
  Json list = listOf(instrument);
  presetNamed(list, id)["values"] = values;
  return list;
}

Json presetsWithName(const Instrument& instrument, int id, const std::string& name) {
  if (name.find_first_not_of(' ') == std::string::npos) throw std::runtime_error("A preset needs a name.");
  Json list = listOf(instrument);
  presetNamed(list, id)["name"] = name.substr(0, 32);
  return list;
}

Json presetsWithout(const Instrument& instrument, int id) {
  Json list = listOf(instrument);
  presetNamed(list, id);
  Json kept = Json::array();
  for (const Json& p : list) if (p.value("id", -1) != id) kept.push_back(p);
  return kept;
}

}  // namespace wax

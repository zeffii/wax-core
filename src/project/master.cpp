#include "project/master.h"

#include <string>

#include "core/js_math.h"

namespace wax {
namespace {

Json column(const char* param, const char* label, int bits = 0, const char* scale = nullptr) {
  Json c = {{"node", "m"}, {"param", param}, {"label", label}};
  if (bits) c["bits"] = bits;
  if (scale) c["scale"] = scale;
  return c;
}

// The limiter's columns: on or off, the ceiling, the gain into it, and the release.
Json limiterColumns() {
  return Json::array({column("limit", "Lim", 4), column("ceiling", "Ceil"), column("boost", "Gain"), column("release", "Rel", 0, "log")});
}

// The carve stage's columns: on or off, how strongly, how deep (dB), and how narrow the bands.
Json carveColumns() {
  return Json::array({column("carve", "Crv", 4), column("cvamt", "CAmt"), column("cvdepth", "CDep"), column("cvfocus", "CFoc")});
}

// The tempo node's parameters as the song's header sets them.
Json tempoParams(const Json& song) {
  return {
    {"bpm", song.value("bpm", 120.0)},
    {"swing", jsRound(song.value("swing", 0.5) * 100)},
    {"tpb", song.value("ticksPerBeat", 4)},
    {"volume", 1},
  };
}

Json masterInstrument(const Json& song) {
  Json exposed = Json::array({column("bpm", "BPM", 16), column("swing", "Swg"), column("tpb", "TPB"), column("volume", "Vol")});
  for (const Json& c : limiterColumns()) exposed.push_back(c);
  for (const Json& c : carveColumns()) exposed.push_back(c);
  return {
    {"graph", {
      {"nodes", Json::array({
        {{"id", "m"}, {"type", "tempo"}, {"params", tempoParams(song)}},
        {{"id", "bus"}, {"type", "bus-in"}},                             // the whole mix: wire it through effects into out
        {{"id", "out"}, {"type", "out"}, {"params", {{"gain", 1}}}},     // unity: the bus passes through unchanged
      })},
      {"connections", Json::array()},
    }},
    {"noteColumns", 0},
    {"exposed", exposed},
    {"commands", Json::array()},
  };
}

bool exposes(const Json& instrument, const char* param) {
  for (const Json& e : instrument.value("exposed", Json::array())) if (e.value("param", std::string()) == param) return true;
  return false;
}

Json* findNode(Json& instrument, const std::string& key, const std::string& value) {
  for (Json& n : instrument["graph"]["nodes"]) if (n.value(key, std::string()) == value) return &n;
  return nullptr;
}

}  // namespace

void ensureMaster(Json& data) {
  const Json& song = data.at("song");
  Json& instruments = data["instruments"];
  const bool kept = instruments.contains("master");
  Json master = kept ? instruments["master"] : masterInstrument(song);

  if (kept) {
    // A saved master machine starts at the song's own tempo, and gets what later builds added to it.
    if (Json* tempo = findNode(master, "id", "m")) {
      if (!tempo->contains("params")) (*tempo)["params"] = Json::object();
      const Json fromSong = tempoParams(song);   // (named, so it outlives the loop over its items)
      for (const auto& [name, value] : fromSong.items()) (*tempo)["params"][name] = value;
    }
    if (!findNode(master, "type", "bus-in")) {
      const char* id = findNode(master, "id", "bus") ? "bus-in" : "bus";
      master["graph"]["nodes"].push_back({{"id", id}, {"type", "bus-in"}});
    }
    if (Json* out = findNode(master, "id", "out")) {
      if (!out->contains("params")) (*out)["params"] = Json::object();
      if (!(*out)["params"].contains("gain")) (*out)["params"]["gain"] = 1;
    }
    if (!master.contains("exposed")) master["exposed"] = Json::array();
    if (!exposes(master, "limit")) for (const Json& c : limiterColumns()) master["exposed"].push_back(c);
    if (!exposes(master, "carve")) for (const Json& c : carveColumns()) master["exposed"].push_back(c);
  }

  // The master comes first among the instruments, the others keep their order.
  Json ordered = Json::object();
  ordered["master"] = master;
  for (const auto& [id, def] : instruments.items()) if (id != "master") ordered[id] = def;
  instruments = ordered;

  // And its lane first among the lanes (an empty one when the file has none).
  if (!data.contains("lanes")) data["lanes"] = Json::array();
  Json& lanes = data["lanes"];
  Json masterLane = {{"id", "master"}, {"instrument", "master"}, {"patterns", Json::object()}, {"placements", Json::array()}};
  Json others = Json::array();
  for (const Json& lane : lanes) {
    if (lane.value("id", std::string()) == "master") masterLane = lane;
    else others.push_back(lane);
  }
  lanes = Json::array({masterLane});
  for (const Json& lane : others) lanes.push_back(lane);
}

}  // namespace wax

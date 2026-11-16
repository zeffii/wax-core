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
  // A master the file does not have shows tempo, swing, ticks per beat and volume as pattern columns; the limiter's and the carve stage's
  // columns are there only when the song exposes them (from the parameter panel).
  Json exposed = Json::array({column("bpm", "BPM", 16), column("swing", "Swg"), column("tpb", "TPB"), column("volume", "Vol")});
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
    // A saved master machine starts at the song's own tempo, and gets the bus and the output gain that later builds added to it. Its columns are
    // as the song saved them (the limiter's and carve stage's are not added back).
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

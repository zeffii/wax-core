#include "project/samples.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace wax {
namespace {

std::vector<std::uint8_t> fromBase64(const std::string& text) {
  std::vector<std::uint8_t> out;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char c : text) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else continue;   // padding, line breaks
    buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xff));
    }
  }
  return out;
}

std::vector<float> decodeChannel(const std::string& text, const std::string& encoding) {
  const std::vector<std::uint8_t> bytes = fromBase64(text);
  std::vector<float> out;
  if (encoding == "pcm16") {
    out.resize(bytes.size() / 2);
    for (std::size_t i = 0; i < out.size(); i++) {
      const std::int16_t v = static_cast<std::int16_t>(static_cast<std::uint16_t>(bytes[2 * i]) | (static_cast<std::uint16_t>(bytes[2 * i + 1]) << 8));
      out[i] = static_cast<float>(v / 32767.0);
    }
  } else {
    out.resize(bytes.size() / 4);
    if (!out.empty()) std::memcpy(out.data(), bytes.data(), out.size() * 4);   // little-endian floats, as on every machine this runs on
  }
  return out;
}

double numberOr(const Json& j, const char* key, double fallback) {
  return j.contains(key) && j[key].is_number() ? j[key].get<double>() : fallback;
}

bool boolOr(const Json& j, const char* key, bool fallback) {
  return j.contains(key) && j[key].is_boolean() ? j[key].get<bool>() : fallback;
}

}  // namespace

std::map<std::string, std::shared_ptr<const SampleMessage>> decodeSamples(const Json& samples) {
  std::map<std::string, std::shared_ptr<const SampleMessage>> out;
  if (samples.is_object()) {
    for (const auto& [id, s] : samples.items()) {
      if (!s.is_object()) continue;
      auto message = std::make_shared<SampleMessage>();
      const std::string encoding = s.value("encoding", std::string("pcm16"));
      message->rate = numberOr(s, "rate", 44100);
      if (s.contains("data") && s["data"].is_string()) message->data = decodeChannel(s["data"].get<std::string>(), encoding);
      if (s.contains("right") && s["right"].is_string()) message->right = decodeChannel(s["right"].get<std::string>(), encoding);
      message->volume = numberOr(s, "volume", 1);
      message->globalVolume = numberOr(s, "globalVolume", 1);
      message->pan = numberOr(s, "pan", 0);
      if (s.contains("slices") && s["slices"].is_array()) {
        for (const auto& v : s["slices"]) if (v.is_number()) message->slices.push_back(v.get<double>());
      }
      message->loopOn = boolOr(s, "loopOn", true);
      message->loopStart = numberOr(s, "loopStart", 0);
      message->loopEnd = numberOr(s, "loopEnd", 0);
      message->loopXfade = numberOr(s, "loopXfade", 0);
      message->susOn = boolOr(s, "susOn", true);
      message->susStart = numberOr(s, "susStart", 0);
      message->susEnd = numberOr(s, "susEnd", 0);
      message->susXfade = numberOr(s, "susXfade", 0);
      out[id] = message;
    }
  }
  for (int i = 1; i <= 16; i++) {   // the tap pool: always there, a single silent frame until something records into it
    char name[8];
    std::snprintf(name, sizeof name, "tap%02d", i);
    if (!out.count(name)) {
      auto message = std::make_shared<SampleMessage>();
      message->data.assign(1, 0.0f);
      out[name] = message;
    }
  }
  return out;
}

std::vector<std::string> sampleSlots(const Instrument& instrument, const std::string& nodeId) {
  std::vector<std::string> ids;
  if (!instrument.raw.is_object() || !instrument.raw.contains("samples") || !instrument.raw["samples"].is_object()) return ids;
  const Json& samples = instrument.raw["samples"];
  if (!samples.contains(nodeId)) return ids;
  const Json& v = samples[nodeId];
  if (v.is_array()) {
    for (const auto& id : v) ids.push_back(id.is_string() ? id.get<std::string>() : std::string());
  } else if (v.is_string()) {
    ids.push_back(v.get<std::string>());
  }
  return ids;
}

Graph graphWithSamplerStereo(const Instrument& instrument, const std::map<std::string, std::shared_ptr<const SampleMessage>>& samples) {
  Graph graph = instrument.graph;
  for (GraphNode& node : graph.nodes) {
    if (node.type != "sampler") continue;
    bool stereo = false;
    for (const std::string& id : sampleSlots(instrument, node.id)) {
      const auto found = samples.find(id);
      if (found != samples.end() && !found->second->right.empty()) stereo = true;
    }
    node.stereo = stereo;
  }
  return graph;
}

}  // namespace wax

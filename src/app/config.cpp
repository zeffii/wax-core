#include <algorithm>
#include "app/config.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "nlohmann/json.hpp"

namespace wax::app {

namespace {

using Json = nlohmann::json;

std::string configPath() {
  char* dir = SDL_GetPrefPath("wax", "wax-app");
  const std::string path = dir ? std::string(dir) + "config.json" : std::string();
  if (dir) SDL_free(dir);
  return path;
}

std::string hexOf(ImU32 colour) {
  char text[16];
  std::snprintf(text, sizeof text, "#%02x%02x%02x", colour & 255, (colour >> 8) & 255, (colour >> 16) & 255);
  return text;
}

ImU32 colourOf(const Json& value, ImU32 fallback) {
  if (!value.is_string() || value.get<std::string>().size() != 7 || value.get<std::string>()[0] != '#') return fallback;
  const unsigned long v = std::strtoul(value.get<std::string>().c_str() + 1, nullptr, 16);
  return IM_COL32((v >> 16) & 255, (v >> 8) & 255, v & 255, 255);
}

Settings gSettings;

}  // namespace

Settings& settings() { return gSettings; }

void loadSettings() {
  std::ifstream file(configPath());
  if (!file) return;
  const Json data = Json::parse(file, nullptr, false);
  if (!data.is_object()) return;
  Settings& s = gSettings;
  s.wireAudio = colourOf(data.value("wireAudio", Json()), s.wireAudio);
  s.wireControl = colourOf(data.value("wireControl", Json()), s.wireControl);
  s.wireStereo = colourOf(data.value("wireStereo", Json()), s.wireStereo);
  s.wireParam = colourOf(data.value("wireParam", Json()), s.wireParam);
  s.wireDashed = data.value("wireDashed", s.wireDashed);
  const std::string playhead = data.value("playhead", s.playhead);
  if (playhead == "marker" || playhead == "line" || playhead == "band") s.playhead = playhead;
  s.follow = data.value("follow", s.follow);
  s.arrangementRow = data.value("arrangementRow", s.arrangementRow);
  s.insertCut = data.value("insertCut", s.insertCut);
  s.patternAll = data.value("patternAll", s.patternAll);
  s.hideEmpty = data.value("hideEmpty", s.hideEmpty);
  s.patternText = std::clamp(data.value("patternText", s.patternText), 0.6f, 2.0f);
  s.notePreview = data.value("notePreview", s.notePreview);
  s.tintPatterns = data.value("tintPatterns", s.tintPatterns);
}

void saveSettings() {
  const std::string path = configPath();
  if (path.empty()) return;
  const Settings& s = gSettings;
  Json data = {{"wireAudio", hexOf(s.wireAudio)}, {"wireControl", hexOf(s.wireControl)}, {"wireStereo", hexOf(s.wireStereo)},
               {"wireParam", hexOf(s.wireParam)}, {"wireDashed", s.wireDashed}, {"playhead", s.playhead}, {"follow", s.follow}, {"arrangementRow", s.arrangementRow}, {"insertCut", s.insertCut}, {"patternAll", s.patternAll}, {"hideEmpty", s.hideEmpty}, {"patternText", s.patternText}, {"notePreview", s.notePreview}, {"tintPatterns", s.tintPatterns}};
  std::ofstream(path, std::ios::trunc) << data.dump(1) << "\n";
}

void resetSettings() { gSettings = Settings(); }

}  // namespace wax::app

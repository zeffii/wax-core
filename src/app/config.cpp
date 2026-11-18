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
  s.follow = data.value("follow", s.follow);
  s.arrangementRow = data.value("arrangementRow", s.arrangementRow);
  s.insertCut = data.value("insertCut", s.insertCut);
  s.patternAll = data.value("patternAll", s.patternAll);
  s.hideEmpty = data.value("hideEmpty", s.hideEmpty);
  s.soloHatch = data.value("soloHatch", s.soloHatch);
  s.seqNames = data.value("seqNames", s.seqNames);
  s.windowWidth = std::clamp(data.value("windowWidth", s.windowWidth), 640, 16000);
  s.windowHeight = std::clamp(data.value("windowHeight", s.windowHeight), 400, 16000);
  s.windowMaximised = data.value("windowMaximised", s.windowMaximised);
  s.mixerHeight = std::clamp(data.value("mixerHeight", s.mixerHeight), 60.0f, 2000.0f);
  s.patternText = std::clamp(data.value("patternText", s.patternText), 0.6f, 2.0f);
  s.notePreview = data.value("notePreview", s.notePreview);
  s.tintPatterns = data.value("tintPatterns", s.tintPatterns);
  s.seqOverview = data.value("seqOverview", s.seqOverview);
  s.minimapOn = data.value("minimapOn", s.minimapOn);
  s.minimapLengths = data.value("minimapLengths", s.minimapLengths);
  s.minimapCaps = data.value("minimapCaps", s.minimapCaps);
  s.minimapParams = data.value("minimapParams", s.minimapParams);
  s.pinnedMachines.clear();
  if (data.contains("pinnedMachines") && data["pinnedMachines"].is_array())
    for (const auto& id : data["pinnedMachines"]) if (id.is_string()) s.pinnedMachines.push_back(id.get<std::string>());
  s.backupOn = data.value("backupOn", s.backupOn);
  s.backupMinutes = std::clamp(data.value("backupMinutes", s.backupMinutes), 1, 60);
  s.backupKeep = std::clamp(data.value("backupKeep", s.backupKeep), 5, 200);
  s.rowWrap = data.value("rowWrap", s.rowWrap);
  s.graphPullOut = data.value("graphPullOut", s.graphPullOut);
  s.threads = std::clamp(data.value("threads", s.threads), 1, 64);
  s.laneWrap = data.value("laneWrap", s.laneWrap);
  s.sliceSensitivity = std::clamp(data.value("sliceSensitivity", s.sliceSensitivity), 0, 100);
  s.sliceMinGap = std::clamp(data.value("sliceMinGap", s.sliceMinGap), 5, 500);
  s.sliceReplace = data.value("sliceReplace", s.sliceReplace);
  s.sliceWhole = data.value("sliceWhole", s.sliceWhole);
  s.waveHideLoop = data.value("waveHideLoop", s.waveHideLoop);
  s.waveHideSel = data.value("waveHideSel", s.waveHideSel);
  s.waveHideSus = data.value("waveHideSus", s.waveHideSus);
  const std::string hex = data.value("hexMode", s.hexMode);
  if (hex == "both" || hex == "relative" || hex == "absolute") s.hexMode = hex;
  s.sampleAskMB = std::clamp(data.value("sampleAskMB", s.sampleAskMB), 1, 2000);
  s.recentFiles.clear();
  if (data.contains("recentFiles") && data["recentFiles"].is_array())
    for (const Json& f : data["recentFiles"]) if (f.is_string() && (int)s.recentFiles.size() < Settings::kRecentMax) s.recentFiles.push_back(f.get<std::string>());
}

void saveSettings() {
  const std::string path = configPath();
  if (path.empty()) return;
  const Settings& s = gSettings;
  Json data = {{"wireAudio", hexOf(s.wireAudio)}, {"wireControl", hexOf(s.wireControl)}, {"wireStereo", hexOf(s.wireStereo)},
               {"wireParam", hexOf(s.wireParam)}, {"wireDashed", s.wireDashed}, {"follow", s.follow}, {"arrangementRow", s.arrangementRow}, {"insertCut", s.insertCut}, {"patternAll", s.patternAll}, {"hideEmpty", s.hideEmpty}, {"soloHatch", s.soloHatch}, {"seqNames", s.seqNames}, {"patternText", s.patternText}, {"mixerHeight", s.mixerHeight}, {"windowWidth", s.windowWidth}, {"windowHeight", s.windowHeight}, {"windowMaximised", s.windowMaximised}, {"notePreview", s.notePreview}, {"tintPatterns", s.tintPatterns}, {"seqOverview", s.seqOverview}, {"minimapOn", s.minimapOn}, {"minimapLengths", s.minimapLengths}, {"minimapCaps", s.minimapCaps}, {"minimapParams", s.minimapParams}, {"pinnedMachines", s.pinnedMachines},
               {"backupOn", s.backupOn}, {"backupMinutes", s.backupMinutes}, {"backupKeep", s.backupKeep}, {"rowWrap", s.rowWrap}, {"graphPullOut", s.graphPullOut}, {"threads", s.threads}, {"laneWrap", s.laneWrap}, {"sliceSensitivity", s.sliceSensitivity}, {"sliceMinGap", s.sliceMinGap}, {"sliceReplace", s.sliceReplace}, {"sliceWhole", s.sliceWhole},
               {"waveHideLoop", s.waveHideLoop}, {"waveHideSel", s.waveHideSel}, {"waveHideSus", s.waveHideSus}, {"hexMode", s.hexMode}, {"sampleAskMB", s.sampleAskMB}, {"recentFiles", s.recentFiles}};
  std::ofstream(path, std::ios::trunc) << data.dump(1) << "\n";
}

void resetSettings() {
  const std::vector<std::string> recent = gSettings.recentFiles;   // (a reset of the settings keeps the history of songs)
  gSettings = Settings();
  gSettings.recentFiles = recent;
}

void rememberRecent(const std::string& path) {
  if (path.empty()) return;
  std::vector<std::string>& list = gSettings.recentFiles;
  list.erase(std::remove(list.begin(), list.end(), path), list.end());
  list.insert(list.begin(), path);
  if ((int)list.size() > Settings::kRecentMax) list.resize(Settings::kRecentMax);
  saveSettings();
}

}  // namespace wax::app

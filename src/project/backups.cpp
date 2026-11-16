#include "project/backups.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace wax {

namespace {

constexpr std::size_t kStampLength = 16;   // "-YYYYMMDD-HHMMSS"

std::string safeName(const std::string& name) {
  std::string out;
  for (const char c : name) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
  if (out.empty()) out = "song";
  return out.substr(0, 40);
}

std::string stampOf(std::time_t time) {
  std::tm parts{};
#ifdef _WIN32
  localtime_s(&parts, &time);
#else
  localtime_r(&time, &parts);
#endif
  char text[24];
  std::snprintf(text, sizeof text, "-%04d%02d%02d-%02d%02d%02d", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
  return text;
}

// Reads the stamp at the end of a snapshot's file name back into a time; 0 when the name is not one of ours.
std::time_t timeOf(const std::string& stem) {
  if (stem.size() < kStampLength) return 0;
  const std::string stamp = stem.substr(stem.size() - kStampLength);
  std::tm parts{};
  if (std::sscanf(stamp.c_str(), "-%4d%2d%2d-%2d%2d%2d", &parts.tm_year, &parts.tm_mon, &parts.tm_mday, &parts.tm_hour, &parts.tm_min, &parts.tm_sec) != 6) return 0;
  parts.tm_year -= 1900;
  parts.tm_mon -= 1;
  parts.tm_isdst = -1;
  return std::mktime(&parts);
}

}  // namespace

fs::path writeBackup(const fs::path& folder, const std::string& songName, const std::string& text, int keep, std::time_t now) {
  std::error_code error;
  fs::create_directories(folder, error);
  const fs::path file = folder / (safeName(songName) + stampOf(now) + ".wax");
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write the backup " + file.string());
    out << text;
  }
  std::vector<BackupInfo> all = listBackups(folder);
  for (std::size_t i = static_cast<std::size_t>(std::max(1, keep)); i < all.size(); i++) fs::remove(all[i].file, error);
  return file;
}

std::vector<BackupInfo> listBackups(const fs::path& folder) {
  std::vector<BackupInfo> out;
  std::error_code error;
  if (!fs::is_directory(folder, error)) return out;
  for (const fs::directory_entry& entry : fs::directory_iterator(folder, error)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".wax") continue;
    const std::string stem = entry.path().stem().string();
    const std::time_t time = timeOf(stem);
    if (!time) continue;
    BackupInfo info;
    info.file = entry.path();
    info.name = stem.substr(0, stem.size() - kStampLength);
    info.time = time;
    info.size = entry.file_size(error);
    out.push_back(info);
  }
  std::sort(out.begin(), out.end(), [](const BackupInfo& a, const BackupInfo& b) { return a.time != b.time ? a.time > b.time : a.file > b.file; });
  return out;
}

}  // namespace wax

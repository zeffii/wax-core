// Automatic backups: snapshots of the song kept in a folder, newest kept, oldest dropped. The window writes one every few minutes while the
// song has changes (the web app's Backup settings) and the File menu lists them so one can be opened again.
#pragma once

#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace wax {

struct BackupInfo {
  std::filesystem::path file;
  std::string name;        // the song's name, as it was when the snapshot was made
  std::time_t time = 0;    // when
  std::uintmax_t size = 0;
};

// Writes `text` (a song as serializeProject makes it) into `folder` as "<name>-YYYYMMDD-HHMMSS.wax" (the name made safe for a file name), creates the
// folder when needed, and then deletes the oldest snapshots (of all songs) beyond `keep`. Returns the file written. Throws std::runtime_error.
std::filesystem::path writeBackup(const std::filesystem::path& folder, const std::string& songName, const std::string& text, int keep, std::time_t now);

// The snapshots in the folder, newest first (an empty list when there is no folder).
std::vector<BackupInfo> listBackups(const std::filesystem::path& folder);

}  // namespace wax

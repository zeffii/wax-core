// Writing a project back to a .wax file. The file as it was read is kept in Project::file; saving puts the lanes as edited
// (patterns and placements) into a copy of it, so every other part of the file (instruments, samples, the song's own
// settings, curves, anything a newer build added) is written back as it was. Same layout as the web app (JSON, indent 1).
#pragma once

#include <string>

#include "project/project.h"

namespace wax {

std::string serializeProject(const Project& project);

// Writes the file next to its final place first and then moves it over, so a crash never leaves half a song.
// Throws std::runtime_error with a message for the user when the file cannot be written.
void saveProject(const Project& project, const std::string& path);

}  // namespace wax

#include "tests/support.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

#include "engine/offline.h"
#include "project/project.h"

namespace tests {

int failures = 0;

void check(bool ok, const std::string& what) {
  if (ok) return;
  failures++;
  std::printf("FAILED: %s\n", what.c_str());
}

bool throws(const std::function<void()>& action) {
  try { action(); } catch (const std::exception&) { return true; }
  return false;
}

namespace {

// The registry is made on first use: tests in other files register while the program starts, in no fixed order.
std::vector<Entry>& entries() {
  static std::vector<Entry> list;
  return list;
}

// "testLiveCellText" -> "live-cell-text"
std::string wordsOf(const std::string& functionName) {
  std::string name = functionName.rfind("test", 0) == 0 ? functionName.substr(4) : functionName;
  std::string words;
  for (std::size_t i = 0; i < name.size(); i++) {
    const char c = name[i];
    if (c >= 'A' && c <= 'Z') { if (i) words += '-'; words += static_cast<char>(c - 'A' + 'a'); }
    else words += c;
  }
  return words;
}

}  // namespace

const std::vector<Entry>& allTests() { return entries(); }

Registration::Registration(const char* group, const char* functionName, std::function<void()> run) { add(group, wordsOf(functionName), std::move(run)); }

void add(const std::string& group, const std::string& name, std::function<void()> run) { entries().push_back({group, name, std::move(run)}); }

void renderSong(const std::string& name) {
  const std::string songs = WAX_TEST_SONGS;
  const wax::Project project = wax::loadProject(songs + "/" + name + ".wax");
  const wax::RenderResult render = wax::renderSong(project);
  std::ifstream file(songs + "/" + name + ".ref.json");
  const wax::Json reference = wax::Json::parse(file);
  check(render.warnings.empty(), name + ": the song uses nothing that is not ported");
  check(render.audio.sampleRate == reference.value("sampleRate", 0), name + ": the render has the reference's sample rate");
  check(render.audio.frames() == reference.value("frames", std::size_t{0}), name + ": the render is as long as the reference, to the sample");
  check(std::abs(render.peak - reference.value("peak", 0.0)) < 1e-4, name + ": the render peaks where the reference does");
  std::printf("ok: %s.wax renders (%zu frames, peak %.6f)\n", name.c_str(), render.audio.frames(), render.peak);
}

}  // namespace tests

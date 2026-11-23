// Tests: import.
#include "tests/support.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "app/it_import.h"
#include "app/xm_import.h"
#include "core/node_registry.h"
#include "core/params.h"
#include "core/random.h"
#include "engine/offline.h"
#include "edit/cell_editor.h"
#include "app/machines.h"
#include "project/samples.h"
#include "edit/document.h"
#include "edit/presets.h"
#include "seq/loop.h"
#include "seq/curve_edit.h"
#include "edit/spline_ops.h"
#include "edit/sync.h"
#include "nodes/bezier.h"
#include "nodes/dx7core.h"
#include "nodes/genexpr_check.h"
#include "app/units.h"
#include "engine/engine.h"
#include "engine/takes.h"
#include "io/wav.h"
#include "project/project.h"
#include "project/sample_edit.h"
#include "project/backups.h"
#include "project/onsets.h"
#include "project/spectrogram.h"
#include "project/save.h"
#include "edit/sequence_ops.h"

using tests::check;
using tests::throws;

// A FastTracker 2 module opened in the native app plays the same as the web app's import of it (the .wax beside it).
WAX_TEST("import", testXmImport) {
  const std::string songs = WAX_TEST_SONGS;
  for (const std::string name : {"xmchan", "xmchanamiga"}) {
    const wax::Project imported = wax::app::projectFromXmFile(songs + "/" + name + ".xm");
    const wax::Project web = wax::loadProject(songs + "/" + name + ".wax");
    check(imported.lanes.size() == web.lanes.size() && imported.lanes.size() == 5, name + ".xm: a lane for each of the four channels, and the master");
    const wax::RenderResult a = wax::renderSong(imported), b = wax::renderSong(web);
    check(a.warnings.empty(), name + ".xm: the import uses nothing that is not ported");
    check(a.audio.left == b.audio.left && a.audio.right == b.audio.right, name + ".xm: the import renders the same samples as the web app's");
    std::printf("ok: %s.xm opens and renders as the web app's import does\n", name.c_str());
  }
  bool refused = false;
  try { wax::app::parseXm(std::vector<std::uint8_t>(100, 0)); } catch (const std::exception&) { refused = true; }
  check(refused, "a file that is not an XM is refused");
}

// An Impulse Tracker module opened in the native app plays the same as the web app's import of it (the .wax beside it).
WAX_TEST("import", testItImport) {
  const std::string songs = WAX_TEST_SONGS;
  for (const std::string name : {"itchan", "itchanamiga"}) {
    const wax::Project imported = wax::app::projectFromItFile(songs + "/" + name + ".it");
    const wax::Project web = wax::loadProject(songs + "/" + name + ".wax");
    check(imported.lanes.size() == web.lanes.size() && imported.lanes.size() == 5, name + ".it: a lane for each of the four channels, and the master");
    const wax::RenderResult a = wax::renderSong(imported), b = wax::renderSong(web);
    check(a.warnings.empty(), name + ".it: the import uses nothing that is not ported");
    check(a.audio.left == b.audio.left && a.audio.right == b.audio.right, name + ".it: the import renders the same samples as the web app's");
    std::printf("ok: %s.it opens and renders as the web app's import does\n", name.c_str());
  }
  // IT 2.14 / 2.15 decompression: a 9-bit value, the escape to width 4, then 4-bit values (+3, -2)
  std::vector<bool> bits;
  const auto put = [&](unsigned v, int n) { for (int i = 0; i < n; i++) bits.push_back((v >> i) & 1); };
  put(5, 9); put(0x103, 9); put(3, 4); put(0xE, 4);
  std::vector<std::uint8_t> packed(2, 0);
  for (std::size_t i = 0; i < bits.size(); i += 8) { std::uint8_t byte = 0; for (std::size_t k = 0; k < 8 && i + k < bits.size(); k++) byte = static_cast<std::uint8_t>(byte | (bits[i + k] << k)); packed.push_back(byte); }
  packed[0] = static_cast<std::uint8_t>((packed.size() - 2) & 255); packed[1] = static_cast<std::uint8_t>((packed.size() - 2) >> 8);
  check(wax::app::decompressIt(packed, 0, 3, false, false) == std::vector<int>({5, 8, 6}), "IT 2.14: one running sum");
  check(wax::app::decompressIt(packed, 0, 3, false, true) == std::vector<int>({5, 13, 19}), "IT 2.15: a second running sum");
  bool refused = false;
  try { wax::app::parseIt(std::vector<std::uint8_t>(300, 0)); } catch (const std::exception&) { refused = true; }
  check(refused, "a file that is not an IT module is refused");
}

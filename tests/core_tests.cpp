// Tests: core.
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

WAX_TEST("core", testParams) {
  wax::ParamRegistry params;
  const int cutoff = params.allocate("bass.filter.cutoff", 800, 20, 20000);
  check(cutoff == 0 && params.get("bass.filter.cutoff") == 800, "a new parameter starts at its default");
  params.set("bass.filter.cutoff", 50000);
  check(params.get("bass.filter.cutoff") == 20000, "a value is clamped to the parameter's range");
  params.set("bass.filter.cutoff", std::nanf(""));
  check(params.get("bass.filter.cutoff") == 20000, "a value that is not a number is ignored");
  check(params.allocate("bass.filter.cutoff", 1, 0, 1) == cutoff && params.get("bass.filter.cutoff") == 20000, "allocating a known address keeps its slot and value");
  check(params.allocate("bass.filter.q", 1, 0, 10) == 1, "slots are handed out in order");
  check(throws([&] { params.set("nobody.home", 1); }), "an unknown address is refused");
  std::printf("ok: parameter block\n");
}

WAX_TEST("core", testRandom) {
  // The first numbers webtracker's tools/render-ref.mjs draws from the same seed.
  wax::Xorshift32 dice;
  const double expected[] = {0.31659353361465037, 0.8757069851271808, 0.4833001629449427};
  for (const double e : expected) check(dice.next() == e, "the dice match the JS reference");
  std::printf("ok: dice\n");
}

WAX_TEST("core", testProject) {
  const std::string path = std::string(WAX_TEST_SONGS) + "/concrete.wax";
  const wax::Project project = wax::loadProject(path);
  check(project.song.bpm == 134 && project.song.length == 64 && project.song.ticksPerBeat == 4, "the song's tempo and length are read");
  check(project.instruments.size() == 7 && project.instruments.front().id == "master" && project.instruments[1].id == "kick" && project.instruments.back().id == "grid", "the instruments keep the order of the file, after the master");
  const wax::Instrument* kick = project.findInstrument("kick");
  check(kick && kick->graph.nodes.size() == 5 && kick->graph.nodes[0].type == "env" && kick->graph.nodes[0].params.value("decay", 0.0) == 70, "a graph's nodes and parameters are read");
  check(kick && kick->graph.connections.size() == 4 && kick->graph.connections[0].from == "pitchEnv.out" && kick->graph.connections[0].to == "osc.fm", "a graph's connections are read");
  check(project.findInstrument("hat")->noteColumns == 0 && kick->noteColumns == 1, "note columns default to one");
  check(project.lanes.size() == 2 && project.lanes[0].id == "master" && !project.lanes[1].patterns.empty() && !project.lanes[1].patterns[0].rows.empty() && project.lanes[1].placements[1].start == 4, "lanes, patterns and placements are read");
  const std::vector<std::string> expected = {"bus-in", "clap", "combperc", "env", "gain", "hat808", "osc", "out", "tempo"};
  check(project.nodeTypes() == expected, "the song's node types are listed");

  check(throws([] { wax::parseProject("not json"); }), "text that is not JSON is refused");
  check(throws([] { wax::parseProject(R"({"format":"webtracker","version":2})"); }), "a newer file version is refused");
  check(throws([] { wax::parseProject(R"({"format":"webtracker","version":1,"song":{},"instruments":{},"lanes":[{"id":"a","instrument":"gone"}]})"); }), "a lane without its instrument is refused");
  std::printf("ok: project files\n");
}

WAX_TEST("core", testGain) {
  const wax::NodeType* gain = wax::builtinNodes().find("gain");
  check(gain && gain->inputs.size() == 2 && gain->params.size() == 1 && gain->params[0].name == "gain", "the gain node is registered with its ports and parameter");
  if (!gain) return;

  constexpr int n = 64;
  std::vector<float> in(n, 0.5f), cv(n, 0.5f), out(n, 0.0f);
  const float* inputs[] = {in.data(), cv.data()};
  float* outputs[] = {out.data()};
  float params[] = {2.0f};
  const int slots[] = {0};

  wax::NodeContext context;
  context.connected = {true, false};
  auto plain = gain->create(context);
  plain->process(inputs, outputs, n, params, slots);
  check(out[0] == 1.0f && out[n - 1] == 1.0f, "without cv the output is in * gain");

  context.connected = {true, true};
  auto vca = gain->create(context);
  vca->process(inputs, outputs, n, params, slots);
  check(out[n - 1] == 0.5f, "with cv connected the output is in * gain * cv");

  params[0] = 4.0f;
  plain->process(inputs, outputs, n, params, slots);
  check(out[0] > 1.0f && out[0] < 1.1f && out[n - 1] == 2.0f, "a change of gain ramps across the block");
  std::printf("ok: gain node\n");
}

WAX_TEST("core", testWav) {
  wax::StereoAudio audio;
  audio.sampleRate = 44100;
  for (int i = 0; i < 1000; i++) {
    audio.left.push_back(static_cast<float>(std::sin(i * 0.05)));
    audio.right.push_back(static_cast<float>(std::cos(i * 0.05)) * 1.5f);   // over full scale: float files do not clip
  }
  const std::string path = (std::filesystem::temp_directory_path() / "wax-core-test.wav").string();
  wax::writeWav(path, audio);
  const wax::StereoAudio back = wax::readWav(path);
  check(back.sampleRate == 44100 && back.left == audio.left && back.right == audio.right, "a float WAV reads back exactly as written");

  wax::StereoAudio nudged = audio;
  nudged.left[500] += 0.001f;
  const wax::AudioDifference same = wax::compareAudio(audio, back), off = wax::compareAudio(audio, nudged);
  check(same.peakDb < -200 && same.lengthGap == 0, "equal renders compare as equal");
  check(std::abs(off.peakDb + 60) < 0.1 && off.worstFrame == 500, "a difference of 0.001 is found, at -60 dB");
  std::filesystem::remove(path);
  std::printf("ok: sound files\n");
}

WAX_TEST("core", testUnits) {
  using wax::app::displayValue;
  using wax::app::parseValue;
  const wax::ParamSpec cutoff("cutoff", 20, 20000, 1200), tune("tune", -48, 48, 0), mix("mix", 0, 1, 0.5);
  check(displayValue("filter", cutoff, 1200) == "1.2 kHz", "frequencies over 1000 show in kHz");
  check(displayValue("lone", tune, 3) == "+3 st", "tuning shows a sign and st");
  check(displayValue("delay", mix, 0.25) == "25%", "a mix shows as a percentage");
  double v = 0;
  check(parseValue("filter", cutoff, "2.5k", v) && v == 2500, "2.5k types as 2500 Hz");
  check(parseValue("delay", mix, "40%", v) && v == 0.4, "40% types as 0.4");
  std::printf("ok: units\n");
}

WAX_TEST("core", testLevels) {
  const wax::Project song = wax::loadProject(std::string(WAX_TEST_SONGS) + "/jawharp.wax");
  wax::Engine engine(song, 44100);
  check(engine.takeLevel("lone") == 0, "an instrument that has not sounded has no level");
  engine.sequencer().play(500);
  std::vector<float> l(wax::kEngineBlock), r(wax::kEngineBlock);
  float loudest = 0;
  for (int b = 0; b < 400; b++) {
    engine.sequencer().process(l.data(), r.data(), wax::kEngineBlock);
    loudest = std::max(loudest, engine.takeLevel("lone"));
  }
  check(loudest > 0.001f, "a playing instrument reports a level");
  check(engine.takeLevel("lone") == 0 && engine.takeLevel("nosuch") == 0, "asking resets it, and an unknown instrument has none");
  std::printf("ok: levels\n");
}

WAX_TEST("core", testGenExprCheck) {
  const wax::GenExprReport good = wax::checkGenExpr("Param amount(0.5, min=0, max=1);\nout1 = in1 * amount;\n");
  check(good.ok && good.params.size() == 1 && good.params[0].name == "amount" && good.params[0].max == 1 && good.params[0].def == 0.5, "a good program compiles and lists its knob");
  const wax::GenExprReport bad = wax::checkGenExpr("out1 = in1;\nout2 = nosuch * 2;\n");
  check(!bad.ok && bad.errors.size() == 1 && bad.errors[0].line == 2 && bad.errors[0].col >= 8, "a mistake is reported on its line");
  const wax::GenExprReport open = wax::checkGenExpr("out1 = (in1 + 2;\n");
  check(!open.ok && open.errors[0].line == 1 && open.errors[0].message.find("expected )") == 0, "a missing bracket is named");
  const wax::GenExprReport range = wax::checkGenExpr("Param p(5, min=0, max=1);\nout1 = in1;\n");
  check(!range.ok && range.errors[0].line == 1, "a Param outside its range is a mistake");
  check(wax::checkGenExpr(wax::genexprDefaultCode()).ok, "the default code compiles");
  std::printf("ok: genexpr check\n");
}

// The tests: one plain program with no framework. Each check prints a line when it fails; the exit code is the number of
// failures, so `ctest` (and the GitHub build) goes red when anything is wrong.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/node_registry.h"
#include "core/params.h"
#include "core/random.h"
#include "engine/offline.h"
#include "io/wav.h"
#include "project/project.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  if (ok) return;
  failures++;
  std::printf("FAILED: %s\n", what.c_str());
}

bool throws(const auto& action) {
  try { action(); } catch (const std::exception&) { return true; }
  return false;
}

void testParams() {
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

void testRandom() {
  // The first numbers webtracker's tools/render-ref.mjs draws from the same seed.
  wax::Xorshift32 dice;
  const double expected[] = {0.31659353361465037, 0.8757069851271808, 0.4833001629449427};
  for (const double e : expected) check(dice.next() == e, "the dice match the JS reference");
  std::printf("ok: dice\n");
}

void testProject() {
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

void testGain() {
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

void testWav() {
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

// Whole songs: rendered and checked against what webtracker's reference render of them measured
// (tests/songs/<name>.ref.json, written by `node tools/render-ref.mjs`). The full sample-by-sample comparison is done
// with wax-compare against the reference WAV, which is too big to keep in the repo.
//   concrete: the first song (a meta machine playing five drum machines)
//   nodes: the Node Sequencer, with its dice, playing polyphonic synths
void testRenderSong(const std::string& name) {
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

// The same song on one thread and on four: not one sample may differ.
void testThreads(const std::string& name) {
  const std::string songs = WAX_TEST_SONGS;
  const wax::Project project = wax::loadProject(songs + "/" + name + ".wax");
  wax::RenderOptions options;
  const wax::RenderResult single = wax::renderSong(project, options);
  options.threads = 4;
  const wax::RenderResult several = wax::renderSong(project, options);
  const bool same = single.audio.left == several.audio.left && single.audio.right == several.audio.right;
  check(same, name + ": four threads render the same samples as one");
  std::printf("ok: %s.wax is the same on 1 and 4 threads\n", name.c_str());
}

}  // namespace

int main() {
  testParams();
  testRandom();
  testProject();
  testGain();
  testWav();
  testRenderSong("concrete");
  testRenderSong("nodes");
  testRenderSong("mosh");
  testRenderSong("suite");
  testRenderSong("curves");
  testRenderSong("effects");
  testRenderSong("effects2");
  testRenderSong("oberheim");
  testRenderSong("fourpole");
  testRenderSong("sampler-repeats");
  testRenderSong("sampler-grains");
  testRenderSong("bugsong");
  testRenderSong("cloves");
  testRenderSong("islands");
  testRenderSong("eqladdergate");
  testRenderSong("saturator");
  testRenderSong("bongo");
  testRenderSong("membrane");
  testRenderSong("cardinal-eco");
  testRenderSong("exciter-sweep");
  testRenderSong("pulsar");
  testRenderSong("pulsetrain");
  testRenderSong("ensemble");
  testRenderSong("reflect");
  testRenderSong("mbsat");
  testRenderSong("split");
  testRenderSong("headpan");
  testRenderSong("pan");
  testRenderSong("follower");
  testRenderSong("range");
  testRenderSong("rangeknob");
  testRenderSong("envfx");
  testRenderSong("notetrig");
  testRenderSong("ctrl");
  testRenderSong("ctrllog");
  testRenderSong("notepan");
  testRenderSong("notepancluster");
  testRenderSong("notepanrandom");
  testRenderSong("hpss");
  testRenderSong("hpssperc");
  testRenderSong("bodyblur");
  testRenderSong("bodyblurbig");
  testRenderSong("mbcomp");
  testRenderSong("specdyn");
  testRenderSong("specdynbig");
  testRenderSong("cardinal");
  testRenderSong("drumsynth");
  testRenderSong("waveshaper");
  testRenderSong("combringtransient");
  testRenderSong("xsynth");
  testRenderSong("xsynthbig");
  testRenderSong("rotary");
  testRenderSong("rotarystop");
  testRenderSong("jawharp");
  testRenderSong("jawharpsoft");
  testRenderSong("khufu");
  testRenderSong("khufuone");
  testRenderSong("granular");
  testRenderSong("granularshort");
  testRenderSong("groove");
  testRenderSong("groovesim");
  testRenderSong("groovewarp");
  testRenderSong("snarl");
  testRenderSong("snarlglide");
  testRenderSong("snarlmod");
  testRenderSong("dvii");
  testRenderSong("dviilfo");
  testRenderSong("dviisolo");
  testRenderSong("dviigrand");
  testRenderSong("specsynth");
  testRenderSong("specsynthroot");
  testRenderSong("modchan");
  testRenderSong("modchan2");
  testRenderSong("modchannearest");
  testRenderSong("compass");
  testRenderSong("compassonce");
  testRenderSong("waveseq");
  testRenderSong("waveseqonce");
  testRenderSong("vecmix");
  testRenderSong("vecmixonce");
  for (const char* name : {"concrete", "nodes", "mosh", "suite", "effects2", "bugsong", "cloves"}) testThreads(name);
  if (failures) std::printf("%d check%s failed\n", failures, failures == 1 ? "" : "s");
  else std::printf("all tests passed\n");
  return failures;
}

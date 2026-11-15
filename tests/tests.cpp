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
#include "edit/cell_editor.h"
#include "app/machines.h"
#include "edit/document.h"
#include "seq/loop.h"
#include "edit/spline_ops.h"
#include "edit/sync.h"
#include "nodes/bezier.h"
#include "nodes/dx7core.h"
#include "app/units.h"
#include "engine/engine.h"
#include "engine/takes.h"
#include "io/wav.h"
#include "project/project.h"
#include "project/save.h"

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


void testTap() {
  const wax::NodeType* tap = wax::builtinNodes().find("tap");
  check(tap && tap->params.size() >= 7 && tap->params[6].name == "copy", "the tap node is registered with its seven parameters");
  if (!tap) return;
  wax::Clock clock;
  clock.playing = true;
  std::vector<wax::TakeEvent> events;
  wax::NodeContext context;
  context.sampleRate = 48000;
  context.clock = &clock;
  context.emit = [&](const wax::TakeMessage& m) { events.push_back({"inst", "tap1", m}); };
  auto node = tap->create(context);

  constexpr int n = 64;
  std::vector<float> inL(n), inR(n), outL(n), outR(n);
  const float* inputs[] = {inL.data(), inR.data()};
  float* outputs[] = {outL.data(), outR.data()};
  // trig len smp ch fade fadein copy, in slots 0..6
  float params[] = {0, 2, 3, 2, 0, 1, 0};
  const int slots[] = {0, 1, 2, 3, 4, 5, 6};
  auto block = [&](int base) {
    for (int i = 0; i < n; i++) { inL[i] = 0.25f + 0.001f * static_cast<float>(base + i); inR[i] = -0.5f + 0.002f * static_cast<float>(base + i); }
    node->process(inputs, outputs, n, params, slots);
  };
  block(0);
  check(events.empty() && outL == inL && outR == inR, "tap passes the audio through and records nothing until its trig cell is written");
  params[0] = 1;
  clock.stamps[0] = 1;   // the pattern wrote trig = 1
  clock.tickId = 5;
  block(64);
  check(events.size() == 1 && events[0].message.phase == wax::TakeMessage::Phase::Start && events[0].message.smp == 3 && events[0].message.ticks == 2, "a trig write starts a take in the slot asked for");
  block(128);
  clock.tickId = 7;   // two ticks have passed: the take ends before this block is recorded
  block(192);
  check(events.size() == 3 && events[2].message.phase == wax::TakeMessage::Phase::End, "a take of len ticks ends on the tick boundary");
  check(outL == inL && outR == inR, "the audio still passes through while recording");

  wax::TakeAssembler assembler;
  std::optional<wax::FinishedTake> done;
  for (const auto& e : events) { auto r = assembler.feed(e); if (r) done = r; }
  check(done && done->smp == 3 && done->left.size() == 2 * n && done->right.size() == 2 * n, "the assembler joins the chunks of a take");
  if (done) {
    check(wax::tapPoolId(done->smp) == "tap03", "pool slots are named tap01 to tap16");
    // fadein 1 ms at 48 kHz is 48 frames of raised cosine, 0 at the first frame and half way up at the middle
    check(done->left[0] == 0.0f && std::fabs(done->left[24] - 0.5f * (0.25f + 0.001f * 88.0f)) < 1e-6f, "the start of a take is faded in");
    check(std::fabs(done->left[100] - (0.25f + 0.001f * 164.0f)) < 1e-6f, "the middle of a take is untouched (fade out is 0)");
  }
  std::printf("ok: tap node\n");
}


void testDocument() {
  wax::Project project;
  wax::Lane lane;
  lane.id = "L1";
  wax::Pattern pattern;
  pattern.id = "p01";
  pattern.rows = {"C-4 40 ---", "--- -- ---", "E-4 40 ---", "--- -- ---"};
  lane.patterns.push_back(pattern);
  project.lanes.push_back(lane);
  wax::Document doc(project);
  const auto rows = [&] { return doc.project().lanes[0].patterns[0].rows; };
  const std::vector<std::string> original = rows();

  check(wax::withCell("C-4  40 ---", 1, "7F") == "C-4  7F ---", "withCell keeps the spacing and the other cells");
  bool threw = false;
  try { wax::withCell("C-4 40", 5, "x"); } catch (const std::out_of_range&) { threw = true; }
  check(threw, "withCell refuses a cell the row does not have");

  doc.apply(std::make_unique<wax::SetRow>("L1", "p01", 1, "G-4 40 ---"));
  check(rows()[1] == "G-4 40 ---" && doc.canUndo() && doc.modified(), "an edit changes the row and can be undone");
  doc.undo();
  check(rows() == original && !doc.canUndo() && doc.canRedo(), "undo puts the pattern back exactly");
  doc.redo();
  check(rows()[1] == "G-4 40 ---", "redo applies it again");
  doc.undo();

  // typing one cell in three steps is a single undo
  doc.apply(std::make_unique<wax::SetRow>("L1", "p01", 0, "G-- 40 ---"));
  doc.apply(std::make_unique<wax::SetRow>("L1", "p01", 0, "G-5 40 ---"), true);
  doc.apply(std::make_unique<wax::SetRow>("L1", "p01", 0, "G-5 7F ---"), true);
  check(rows()[0] == "G-5 7F ---", "the steps of an entry land");
  doc.undo();
  check(rows() == original && !doc.canUndo(), "one undo takes back a whole entry");
  doc.redo();
  check(rows()[0] == "G-5 7F ---", "and one redo brings it all back");
  doc.undo();

  doc.apply(std::make_unique<wax::InsertRow>("L1", "p01", 1, "--- -- ---"));
  check(rows().size() == 4 && rows()[2] == "--- -- ---" && rows()[3] == "E-4 40 ---", "insert pushes rows down and keeps the length");
  doc.undo();
  check(rows() == original, "undoing an insert brings back the row that fell off");
  doc.apply(std::make_unique<wax::DeleteRow>("L1", "p01", 0, "--- -- ---"));
  check(rows()[0] == "--- -- ---" && rows()[1] == "E-4 40 ---" && rows()[3] == "--- -- ---", "delete pulls rows up and keeps the length");
  doc.undo();
  check(rows() == original, "undoing a delete puts the row back");

  doc.apply(std::make_unique<wax::SetRow>("L1", "p01", 2, "A-4 40 ---"));
  doc.undo();
  doc.apply(std::make_unique<wax::SetRow>("L1", "p01", 3, "B-4 40 ---"));
  check(!doc.canRedo(), "a new edit ends the redo history");
  const auto lanes = doc.takeChangedLanes();
  check(lanes.size() == 1 && lanes.count("L1") && doc.takeChangedLanes().empty(), "the changed lanes are reported once");
  doc.markSaved();
  check(!doc.modified(), "saving clears the modified flag");
  doc.undo();
  check(doc.modified(), "and an undo after saving sets it again");
  std::printf("ok: document and undo\n");
}


// An edit sent to a playing engine sounds the same as the edited song played from the start.
void testLiveEdit() {
  const std::string songs = WAX_TEST_SONGS;
  const wax::Project original = wax::loadProject(songs + "/jawharp.wax");
  const std::string lane = "lone";
  const std::string pattern = original.lanes[1].patterns[0].id;   // [0] is the master lane
  check(original.lanes[1].id == lane, "the live edit test finds the lane it expects");

  wax::Document doc(original);
  const std::string edited = "E-4 .. C-5 .. G-4 .. ... .. ... .. ... .. .. .. .. .. ......";
  wax::Document reference(original);
  reference.apply(std::make_unique<wax::SetRow>(lane, pattern, 0, edited));

  constexpr int kBlocks = 600, kBefore = 100;
  const auto play = [&](wax::Engine& engine, wax::Document* during, bool edit) {
    std::vector<float> out;
    engine.sequencer().play(500);
    std::vector<float> l(wax::kEngineBlock), r(wax::kEngineBlock);
    for (int b = 0; b < kBlocks; b++) {
      if (edit && b == kBefore) {   // the window thread edits while the song plays (tick 500 is before the first placement at 512)
        during->apply(std::make_unique<wax::SetRow>(lane, pattern, 0, edited));
        const auto problems = wax::syncLanes(*during, engine);
        check(problems.empty(), "the edited lane is sent without a problem");
      }
      engine.applySentLanes();
      engine.sequencer().process(l.data(), r.data(), wax::kEngineBlock);
      out.insert(out.end(), l.begin(), l.end());
      out.insert(out.end(), r.begin(), r.end());
    }
    return out;
  };
  wax::Engine live(original, 44100);
  wax::Engine fresh(reference.project(), 44100);
  const std::vector<float> a = play(live, &doc, true);
  const std::vector<float> b = play(fresh, nullptr, false);
  check(a == b, "a lane edited while the song plays sounds exactly like the edited song from the start");
  double peak = 0;
  for (float v : a) peak = std::max(peak, static_cast<double>(std::fabs(v)));
  check(peak > 0.01, "the live edit test hears something");
  // and the same with the edit undone: back to the original
  doc.undo();
  check(wax::syncLanes(doc, live).empty(), "an undo is sent like any other edit");
  live.applySentLanes();
  live.collectRetired();
  std::printf("ok: live edits\n");
}


void testBlockEdit() {
  wax::Project project;
  project.song.length = 8;
  wax::Lane lane;
  lane.id = "L1";
  wax::Pattern pattern;
  pattern.id = "p01";
  pattern.rows = {"C-4 40 .... ......", "D-4 41 .... ......", "E-4 42 .... ......", "... .. .... ......"};
  lane.patterns.push_back(pattern);
  lane.placements.push_back({"p01", 2, 4});
  project.lanes.push_back(lane);
  std::vector<wax::Column> columns(4);
  columns[0].kind = wax::ColumnKind::Note;  columns[0].token = wax::Token::Note;
  columns[1].kind = wax::ColumnKind::Vel;   columns[1].token = wax::Token::Hex2;
  columns[2].kind = wax::ColumnKind::Param; columns[2].token = wax::Token::Hex4;
  columns[3].kind = wax::ColumnKind::Cmd;   columns[3].token = wax::Token::Cmd;
  wax::Document doc(project);
  wax::CellEditor ed(doc);
  ed.setLane("L1", columns);
  const auto row = [&](int i) { return doc.project().lanes[0].patterns[0].rows[static_cast<std::size_t>(i)]; };

  ed.moveTo(2, 0);
  ed.extend(1, 1);   // ticks 2..3, cells 0..1
  check(ed.selection().active && ed.selection().tick1 == 3 && ed.selection().cell1 == 1, "shift-moving selects a block");
  const std::string text = ed.copy();
  check(text == "C-4\t40\nD-4\t41\n", "copy gives rows and cells as text");
  ed.moveTo(4, 0);
  check(ed.paste(), "paste works");
  check(row(2) == "C-4 40 .... ......" && row(3) == "D-4 41 .... ......" || row(2) == "C-4 40 .... ......", "the pasted rows are there");
  check(row(2).substr(0, 6) == "C-4 40" && row(3).substr(0, 6) == "D-4 41", "the block lands at the cursor");
  doc.undo();
  check(row(2) == "E-4 42 .... ......" , "undo takes back the whole paste in one step");
  ed.moveTo(2, 0);
  ed.extend(1, 1);
  check(ed.cut(), "cut works");
  check(row(0) == "... .. .... ......" && row(1) == "... .. .... ......", "cut blanks the cells");
  doc.undo();
  check(row(0) == "C-4 40 .... ......", "undo puts a cut back");
  std::printf("ok: block edits\n");
}

void testCellEditor() {
  wax::Project project;
  project.song.length = 8;
  wax::Lane lane;
  lane.id = "L1";
  wax::Pattern pattern;
  pattern.id = "p01";
  pattern.rows = {"... .. .... ......", "... .. .... ......", "... .. .... ......", "... .. .... ......"};
  lane.patterns.push_back(pattern);
  lane.placements.push_back({"p01", 2, 4});   // the pattern plays on ticks 2 to 5
  project.lanes.push_back(lane);
  std::vector<wax::Column> columns(4);
  columns[0].kind = wax::ColumnKind::Note;  columns[0].token = wax::Token::Note;
  columns[1].kind = wax::ColumnKind::Vel;   columns[1].token = wax::Token::Hex2;
  columns[2].kind = wax::ColumnKind::Param; columns[2].token = wax::Token::Hex4;
  columns[3].kind = wax::ColumnKind::Cmd;   columns[3].token = wax::Token::Cmd;

  wax::Document doc(project);
  wax::CellEditor ed(doc);
  ed.setLane("L1", columns);
  const auto row = [&](int i) { return doc.project().lanes[0].patterns[0].rows[static_cast<std::size_t>(i)]; };

  ed.moveTo(0, 0);
  check(ed.onPattern() == false, "no pattern plays on tick 0");
  ed.moveTo(2, 0);
  check(ed.onPattern() && ed.onNoteCell(), "the cursor finds the pattern and the kind of cell");
  check(ed.typeNote(61) && row(0) == "C#4 .. .... ......" && ed.tick() == 3, "a note is written and the cursor steps down");
  ed.move(-1, 1);
  check(ed.onHexCell() && ed.tick() == 2 && ed.cell() == 1, "the cursor moves to the next cell");
  check(ed.typeHexDigit(4) && row(0) == "C#4 40 .... ......" && ed.tick() == 2, "the first digit of a hex cell fills it with zeros");
  check(ed.typeHexDigit(7) && row(0) == "C#4 47 .... ......" && ed.tick() == 3, "the second completes it and the cursor steps down");
  doc.undo();
  check(row(0) == "C#4 .. .... ......", "a two-digit entry is one undo");
  doc.undo();
  check(row(0) == "... .. .... ......" && !doc.canUndo(), "and the note is another");

  ed.moveTo(3, 2);
  for (int d : {1, 2, 15, 0}) ed.typeHexDigit(d);
  check(row(1) == "... .. 12F0 ......" && ed.tick() == 4, "four digits fill a four-digit cell");
  ed.moveTo(3, 0);
  check(ed.typeNoteOff() && row(1) == "=== .. 12F0 ......", "note off writes the off mark");
  ed.moveTo(4, 0);
  check(ed.typeNoteCut() && row(2).substr(0, 3) == "^^^", "note cut writes its mark");
  ed.moveTo(3, 1);
  check(!ed.typeNoteCut() && !ed.typeNote(60), "notes are not typed into a hex cell");
  ed.moveTo(3, 2);
  check(ed.clearCell(false) && row(1) == "=== .. .... ......", "clearing puts the blank back");

  ed.moveTo(2, 0);
  ed.setStep(0);
  ed.typeNote(60);
  ed.moveTo(3, 0);
  const std::string below = row(1);
  check(ed.insertRow() && row(1) == "... .. .... ......" && row(2) == below, "insert pushes the rows down");
  check(ed.deleteRow() && row(1) == below, "delete pulls them back up");
  ed.moveTo(6, 0);
  check(!ed.onPattern(), "the ticks after the pattern are not a pattern");
  ed.move(100, 100);
  check(ed.tick() == 7 && ed.cell() == 3, "the cursor stays inside the song and the lane");
  std::printf("ok: cell editor\n");
}


void testNewPattern() {
  wax::Project project;
  project.song.length = 40;
  wax::Lane lane;
  lane.id = "L1";
  project.lanes.push_back(lane);
  std::vector<wax::Column> columns(2);
  columns[0].kind = wax::ColumnKind::Note; columns[0].token = wax::Token::Note;
  columns[1].kind = wax::ColumnKind::Vel;  columns[1].token = wax::Token::Hex2;
  wax::Document doc(project);
  wax::CellEditor ed(doc);
  ed.setLane("L1", columns);
  const auto& l = [&]() -> const wax::Lane& { return doc.project().lanes[0]; };

  ed.moveTo(21, 0);
  check(ed.typeNote(60) && l().patterns.size() == 1 && l().placements.size() == 1, "typing where no pattern plays makes one");
  check(l().placements[0].start == 16 && l().placements[0].length == 16 && l().patterns[0].rows.size() == 16, "it is 16 rows, at the multiple of 16 below the cursor");
  check(l().patterns[0].rows[5] == "C-4 .." && l().patterns[0].name == "pat00" && l().patterns[0].index == 0, "the note is in the right row of the new pattern");
  check(ed.tick() == 22, "and the cursor stepped on");
  ed.moveTo(22, 1);
  ed.typeHexDigit(4); ed.typeHexDigit(8);
  doc.undo();
  check(l().patterns[0].rows[6] == "... .." && l().patterns.size() == 1, "later typing is its own undo step");
  doc.undo();
  check(l().patterns.empty() && l().placements.empty() && !doc.canUndo(), "one undo takes the new pattern and its first note away");
  doc.redo();
  check(l().patterns.size() == 1 && l().patterns[0].rows[5] == "C-4 ..", "redo brings both back");

  ed.moveTo(36, 0);
  check(ed.typeNote(62) && l().placements.size() == 2 && l().placements[1].start == 32 && l().placements[1].length == 8, "a pattern at the end of the song is cut to fit it");
  check(l().patterns[1].index == 1 && l().patterns[1].name == "pat01", "the next pattern takes the next index");
  ed.moveTo(33, 0);
  check(ed.onPattern() && !ed.typeNote(0) == false, "typing inside a pattern writes into it");
  std::printf("ok: new patterns\n");
}


void testSave() {
  const std::string songs = WAX_TEST_SONGS;
  // an untouched song saves as the same song: what is read back is what was read
  for (const std::string name : {"jawharp", "dvii", "modchan", "convolver"}) {
    const wax::Project first = wax::loadProject(songs + "/" + name + ".wax");
    const std::string text = wax::serializeProject(first);
    const wax::Project second = wax::parseProject(text);
    check(second.file == first.file, name + ": saving and reading again gives the same file contents");
    check(wax::serializeProject(second) == text, name + ": saving twice gives the same text");
  }
  // an edit is in the saved file, and nothing else changed
  wax::Document doc(wax::loadProject(songs + "/jawharp.wax"));
  const std::string pattern = doc.project().lanes[1].patterns[0].id;
  doc.apply(std::make_unique<wax::SetRow>("lone", pattern, 0, "E-4 .. C-5 .. G-4 .. ... .. ... .. ... .. .. .. .. .. ......"));
  const wax::Project reread = wax::parseProject(wax::serializeProject(doc.project()));
  check(reread.lanes[1].patterns[0].rows[0].rfind("E-4", 0) == 0, "an edited row is in the saved file");
  wax::Json before = doc.project().file, after = reread.file;
  before["lanes"][1]["patterns"][pattern]["rows"][0] = after["lanes"][1]["patterns"][pattern]["rows"][0];
  check(before == after, "and the rest of the file is as it was");
  // a new pattern is saved too
  wax::Document fresh(wax::loadProject(songs + "/jawharp.wax"));
  wax::Pattern np; np.id = "9-0"; np.name = "pat99"; np.index = 99; np.rows = {"... .. ... .. ... .. ... .. ... .. ... .. .. .. .. .. ......"};
  fresh.apply(std::make_unique<wax::AddPattern>("lone", np, wax::Placement{"9-0", 3, 1}));
  const wax::Project again = wax::parseProject(wax::serializeProject(fresh.project()));
  check(again.lanes[1].patterns.back().id == "9-0" && again.lanes[1].placements.back().start == 3, "a new pattern and its placement are saved");
  // writing to disk
  const std::string path = (std::filesystem::temp_directory_path() / "wax-save-test.wax").string();
  wax::saveProject(fresh.project(), path);
  check(wax::loadProject(path).lanes[1].patterns.back().id == "9-0", "a saved file loads");
  std::filesystem::remove(path);
  std::printf("ok: saving\n");
}

void testSpline() {
  using namespace wax::bezier;
  Spline s = shapeOf(Shape::Hump);
  const auto before = walk(s);
  const double scale[2] = {1, 1};
  const Nearest n = nearestOnSpline(s, 0.2, 0.7, scale);
  check(n.segment == 0 && n.d < 0.5, "a point is found on the curve");
  const int at = insertOn(s, n);
  check(at == 1 && s.size() == 4, "an anchor is put on the curve");
  const auto after = walk(s);
  double worst = 0;   // the shape is unchanged: every table value stays where it was
  const Table a = envelopeTable(shapeOf(Shape::Hump)), b = envelopeTable(s);
  for (std::size_t k = 0; k < a.size(); k++) worst = std::max(worst, static_cast<double>(std::fabs(a[k] - b[k])));
  check(worst < 0.01, "inserting on a curve leaves its shape as it was");
  (void)before; (void)after;
  check(removeAnchor(s, 1) && s.size() == 3 && !removeAnchor(s, 0) && !removeAnchor(s, 2), "inner anchors go, the ends stay");
  setKind(s, 1, "sharp");
  check(kindOf(s[1]) == "sharp" && s[1].hasLeft && s[1].left[0] == 0 && s[1].right[1] == 0, "a sharp anchor has no handles");
  setKind(s, 1, "smooth");
  check(kindOf(s[1]).empty() && !s[1].hasLeft, "and smooth again works them out");
  setHandle(s, 1, Side::Right, s[1].x + 0.1, s[1].y + 0.2, false);
  check(std::fabs(s[1].left[0] * s[1].right[1] - s[1].left[1] * s[1].right[0]) < 1e-9, "a smooth anchor's handles stay in line");
  setKind(s, 1, "equal");
  setHandle(s, 1, Side::Left, s[1].x - 0.15, s[1].y, false);
  check(std::fabs(s[1].right[0] - 0.15) < 1e-9 && std::fabs(s[1].right[1]) < 1e-9, "an equal anchor mirrors its handle");
  setKnot(s, 1);
  check(sustainKnot(s) && sustainKnot(s)->index == 1, "the sustain knot is set");
  setKnot(s, 1);
  check(!sustainKnot(s), "and taken off by asking again");
  moveAnchor(s, 1, 5, 0.3);
  check(s[1].x < 1 && s[1].x > 0.9, "an anchor stays before the next one");
  Spline r = s;
  reverse(r); reverse(r);
  const Table t1 = envelopeTable(s), t2 = envelopeTable(r);
  double diff = 0;
  for (std::size_t k = 0; k < t1.size(); k++) diff = std::max(diff, static_cast<double>(std::fabs(t1[k] - t2[k])));
  check(diff < 1e-6, "reversing twice gives the drawing back");
  const wax::Json stored = toJson(s);
  const Spline again = cleanSpline(&stored, Shape::Fall);
  const Table t3 = envelopeTable(again);
  diff = 0;
  for (std::size_t k = 0; k < t1.size(); k++) diff = std::max(diff, static_cast<double>(std::fabs(t1[k] - t3[k])));
  check(diff < 1e-3 && again.size() == s.size() && kindOf(again[1]) == kindOf(s[1]), "a drawing written to the file reads back the same");
  std::printf("ok: spline editing\n");
}

void testDrawingEdit() {
  using namespace wax::bezier;
  const wax::Project original = wax::loadProject(std::string(WAX_TEST_SONGS) + "/khufu.wax");
  Spline drawn = shapeOf(Shape::Hump);
  setKnot(drawn, 1);
  addAnchor(drawn, 0.2, 0.3);
  const wax::Json points = toJson(drawn);

  wax::Document doc(original), reference(original);
  reference.apply(std::make_unique<wax::SetDrawing>("lone", "synth", "amp", points));
  constexpr int kBlocks = 700, kBefore = 100;
  const auto play = [&](wax::Engine& engine, wax::Document* during) {
    std::vector<float> out, l(wax::kEngineBlock), r(wax::kEngineBlock);
    engine.sequencer().play(500);
    for (int b = 0; b < kBlocks; b++) {
      if (during && b == kBefore) {
        during->apply(std::make_unique<wax::SetDrawing>("lone", "synth", "amp", points));
        wax::syncData(*during, engine);
      }
      engine.applySentLanes();
      engine.sequencer().process(l.data(), r.data(), wax::kEngineBlock);
      out.insert(out.end(), l.begin(), l.end());
      out.insert(out.end(), r.begin(), r.end());
    }
    return out;
  };
  wax::Engine live(original, 44100), fresh(reference.project(), 44100), untouched(original, 44100);
  const std::vector<float> a = play(live, &doc), b = play(fresh, nullptr), c = play(untouched, nullptr);
  check(a == b, "a drawing edited while the song plays sounds exactly like the edited song from the start");
  check(a != c, "and the edit is heard");

  // the edit is one undo step when dragged, and it is saved
  wax::Document drag(original);
  drag.apply(std::make_unique<wax::SetDrawing>("lone", "synth", "amp", points));
  drag.apply(std::make_unique<wax::SetDrawing>("lone", "synth", "amp", wax::Json::array()), true);
  drag.apply(std::make_unique<wax::SetDrawing>("lone", "synth", "amp", points), true);
  check(drag.takeChangedData().count(std::string("lone\x1fsynth")) == 1, "the node is reported changed");
  const wax::Project reread = wax::parseProject(wax::serializeProject(drag.project()));
  check(reread.instruments[1].graph.nodes[0].data["env"]["amp"].size() == points.size(), "the drawing is in the saved file");
  drag.undo();
  check(drag.project().instruments[1].graph.nodes[0].data == original.instruments[1].graph.nodes[0].data && !drag.canUndo(), "one undo takes the whole drag back");
  std::printf("ok: drawing edits\n");
}

void testBankEdit() {
  const wax::Project original = wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax");
  std::vector<int> bank = wax::dx7::defaultBank();
  wax::dx7::Voice v = wax::dx7::unpackVoice(bank, 3 * 128);
  v.alg = (v.alg + 5) % 32;
  v.ops[1].ol = 40;
  v.fb = 6;
  wax::dx7::packVoice(v, bank, 3 * 128);

  wax::Document doc(original), reference(original);
  reference.apply(std::make_unique<wax::SetBank>("lone", "synth", bank));
  const auto play = [&](wax::Engine& engine, wax::Document* during) {
    std::vector<float> out, l(wax::kEngineBlock), r(wax::kEngineBlock);
    engine.sequencer().play(500);
    for (int b = 0; b < 700; b++) {
      if (during && b == 100) {
        during->apply(std::make_unique<wax::SetBank>("lone", "synth", bank));
        wax::syncData(*during, engine);
      }
      engine.applySentLanes();
      engine.sequencer().process(l.data(), r.data(), wax::kEngineBlock);
      out.insert(out.end(), l.begin(), l.end());
    }
    return out;
  };
  wax::Engine live(original, 44100), fresh(reference.project(), 44100), untouched(original, 44100);
  const auto a = play(live, &doc), b = play(fresh, nullptr), c = play(untouched, nullptr);
  check(a == b, "a DX bank edited while the song plays sounds like the edited song from the start");
  check(a != c, "and the edit is heard");
  const wax::Project reread = wax::parseProject(wax::serializeProject(doc.project()));
  check(reread.instruments[1].graph.nodes[0].data["bank"].size() == 4096, "the bank is in the saved file");
  doc.undo();
  check(doc.project().instruments[1].graph.nodes[0].data.is_null() && !doc.canUndo(), "undo takes it back");
  std::printf("ok: bank edits\n");
}

void testRewire() {
  const wax::Project original = wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax");
  wax::Document doc(original), reference(original);
  const auto rewire = [](wax::Document& d) {
    d.apply(std::make_unique<wax::DeleteNode>("lone", "prismacanyon1"));
    d.apply(std::make_unique<wax::Connect>("lone", "poly.out", "out.in"));
  };
  rewire(doc);
  rewire(reference);
  const auto play = [&](wax::Engine& engine, wax::Document* during, int at) {
    std::vector<float> out, l(wax::kEngineBlock), r(wax::kEngineBlock);
    engine.sequencer().play(500);
    for (int b = 0; b < 700; b++) {
      if (during && b == at) check(wax::syncGraphs(*during, engine).empty(), "the new wiring compiles");
      engine.applySentLanes();
      engine.sequencer().process(l.data(), r.data(), wax::kEngineBlock);
      out.insert(out.end(), l.begin(), l.end());
    }
    return out;
  };
  wax::Engine live(original, 44100), fresh(reference.project(), 44100), untouched(original, 44100), midplay(original, 44100);
  wax::Document doc2(original);
  rewire(doc2);
  const auto a = play(live, &doc, 0), b = play(fresh, nullptr, 0), c = play(untouched, nullptr, 0), m = play(midplay, &doc2, 150);
  check(a == b, "rewiring a node graph live gives the sound of the rewired song");
  check(a != c, "and the rewiring is heard");
  check(m != c, "rewiring in the middle of a song is heard too");
  doc.undo();
  doc.undo();
  check(doc.project().instruments[1].graph.nodes.size() == original.instruments[1].graph.nodes.size() &&
            doc.project().instruments[1].graph.connections.size() == original.instruments[1].graph.connections.size() && !doc.canUndo(),
        "undo puts the node and its wires back");
  std::printf("ok: rewiring\n");
}

void testUnits() {
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

void testAddNode() {
  const wax::Project original = wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax");
  wax::Document doc(original), reference(original);
  const auto build = [](wax::Document& d) {
    d.apply(std::make_unique<wax::AddNode>("lone", "distortion", "distortion", 100, 100));
    d.apply(std::make_unique<wax::Disconnect>("lone", "poly.out", "prismacanyon1.in"));
    d.apply(std::make_unique<wax::Connect>("lone", "poly.out", "distortion.in"));
    d.apply(std::make_unique<wax::Connect>("lone", "distortion.out", "prismacanyon1.in"));
  };
  build(doc);
  build(reference);
  wax::Engine live(original, 44100), fresh(reference.project(), 44100), untouched(original, 44100);
  const auto play = [&](wax::Engine& engine) {
    std::vector<float> out, l(wax::kEngineBlock), r(wax::kEngineBlock);
    engine.sequencer().play(500);
    for (int b = 0; b < 500; b++) {
      engine.applySentLanes();
      engine.sequencer().process(l.data(), r.data(), wax::kEngineBlock);
      out.insert(out.end(), l.begin(), l.end());
    }
    return out;
  };
  check(wax::syncGraphs(doc, live).empty(), "a graph with a new node compiles live");
  const auto a = play(live), b = play(fresh), c = play(untouched);
  check(a == b, "a node added live sounds like the song with that node from the start");
  check(a != c, "and is heard");
  doc.undo(); doc.undo(); doc.undo(); doc.undo();
  check(doc.project().instruments[1].graph.nodes.size() == original.instruments[1].graph.nodes.size(), "undo takes the node out");
  std::printf("ok: add node\n");
}

void testSongSettings() {
  const wax::Project original = wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax");
  wax::Document doc(original);
  doc.apply(std::make_unique<wax::SetParam>("master", "m", "bpm", 150));
  doc.apply(std::make_unique<wax::SetParam>("master", "m", "swing", 60));
  doc.apply(std::make_unique<wax::SetParam>("master", "m", "tpb", 6));
  doc.apply(std::make_unique<wax::SetBeatsPerBar>(3));
  const wax::Project reread = wax::parseProject(wax::serializeProject(doc.project()));
  check(reread.song.bpm == 150 && reread.song.swing == 0.6 && reread.song.ticksPerBeat == 6 && reread.song.beatsPerBar == 3, "the header's tempo settings are saved with the song");
  std::printf("ok: song settings\n");
}

void testLoop() {
  const wax::Project original = wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax");
  wax::Document doc(original);
  std::string message;
  doc.apply(std::make_unique<wax::SetLoop>(wax::loopWithPoint(doc.project().song, "start", 4, message), message));
  doc.apply(std::make_unique<wax::SetLoop>(wax::loopWithPoint(doc.project().song, "end", 8, message), message));
  const wax::LoopRange range = wax::loopOf(doc.project().song);
  check(range.on && range.start == 4 && range.end == 8, "Ctrl+B and Ctrl+E set the loop");
  wax::Engine engine(doc.project(), 44100);
  engine.sequencer().play(0);
  std::vector<float> l(wax::kEngineBlock), r(wax::kEngineBlock);
  int lowest = 1 << 30, highest = 0;
  bool passedLoop = false;
  for (int b = 0; b < 3000; b++) {
    engine.sequencer().process(l.data(), r.data(), wax::kEngineBlock);
    const int t = engine.sequencer().tick();
    if (t >= 4) passedLoop = true;
    if (passedLoop) { lowest = std::min(lowest, t); highest = std::max(highest, t); }
  }
  check(passedLoop && lowest >= 4 && highest <= 8, "a song with a loop plays round inside it");
  const wax::Project reread = wax::parseProject(wax::serializeProject(doc.project()));
  const wax::LoopRange saved = wax::loopOf(reread.song);
  check(saved.on && saved.start == 4 && saved.end == 8, "the loop is saved with the song");
  doc.apply(std::make_unique<wax::SetLoop>(wax::loopToggled(doc.project().song), "Loop off"));
  check(!wax::loopOf(doc.project().song).on && wax::loopOf(doc.project().song).start == 4, "Ctrl+L switches the loop off and keeps its points");
  doc.undo(); doc.undo(); doc.undo();
  check(!doc.project().song.raw.contains("loop") && !doc.canUndo(), "undo takes the loop away again");
  std::printf("ok: song loop\n");
}

void testPlacePattern() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax"));
  const wax::Lane* lane = nullptr;
  for (const wax::Lane& l : doc.project().lanes) if (!l.patterns.empty() && l.id != "master") { lane = &l; break; }
  check(lane != nullptr, "the song has a lane with a pattern");
  const std::string id = lane->id, pattern = lane->patterns.front().id;
  const std::size_t before = lane->placements.size();
  const int tick = doc.project().song.length - 4;
  doc.apply(std::make_unique<wax::PlacePattern>(id, pattern, tick, 4));
  const wax::Lane& after = doc.project().lanes[static_cast<std::size_t>(lane - doc.project().lanes.data())];
  check(after.placements.size() >= before && after.placements.back().start == tick && after.placements.back().pattern == pattern, "a placed pattern is the last placement");
  doc.undo();
  check(doc.project().lanes[static_cast<std::size_t>(lane - doc.project().lanes.data())].placements.size() == before, "undo takes the placement away and brings back what it replaced");
  std::printf("ok: place pattern\n");
}

void testAddInstrument() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax"));
  wax::Engine engine(doc.project(), 44100);
  const std::size_t instruments = doc.project().instruments.size(), lanes = doc.project().lanes.size();
  const wax::Json kick = wax::app::machineJson("kick");
  check(!kick.is_null(), "the machine table knows the kick");
  doc.apply(std::make_unique<wax::AddInstrument>("kick", kick, "kick"));
  check(doc.project().instruments.size() == instruments + 1 && doc.project().lanes.size() == lanes + 1, "an added machine brings its lane");
  check(engine.sendGraph(doc.project(), "kick").empty(), "the new machine compiles while the song plays");
  check(engine.sendLane(doc.project(), "kick"), "its lane is sent");
  engine.applySentLanes();
  check(!engine.laneColumns("kick").empty(), "the grid knows the new lane's columns");
  engine.sequencer().play(0);
  std::vector<float> l(wax::kEngineBlock), r(wax::kEngineBlock);
  for (int b = 0; b < 50; b++) engine.sequencer().process(l.data(), r.data(), wax::kEngineBlock);
  const wax::Project reread = wax::parseProject(wax::serializeProject(doc.project()));
  check(reread.instruments.size() == instruments + 1 && reread.lanes.size() == lanes + 1, "the added machine and its lane are saved");
  doc.undo();
  check(doc.project().instruments.size() == instruments && doc.project().lanes.size() == lanes, "undo takes the machine and its lane away");
  check(engine.sendLane(doc.project(), "kick"), "an undone lane is replaced by an empty one");
  engine.applySentLanes();
  for (int b = 0; b < 50; b++) engine.sequencer().process(l.data(), r.data(), wax::kEngineBlock);
  std::printf("ok: add instrument\n");
}

void testMoveNode() {
  const wax::Project original = wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax");
  wax::Document doc(original);
  doc.apply(std::make_unique<wax::MoveNode>("lone", "synth", 50, 60));
  doc.apply(std::make_unique<wax::MoveNode>("lone", "synth", 70, 80), true);
  const wax::Project reread = wax::parseProject(wax::serializeProject(doc.project()));
  const wax::GraphNode& n = reread.instruments[1].graph.nodes[0];
  check(n.placed && n.x == 70 && n.y == 80, "a moved node keeps its place in the saved file");
  doc.undo();
  const wax::GraphNode& back = doc.project().instruments[1].graph.nodes[0];
  check(back.x == original.instruments[1].graph.nodes[0].x && !doc.canUndo(), "a drag is one undo step");
  std::printf("ok: node moves\n");
}

void testLevels() {
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

void testSetParam() {
  const std::string songs = WAX_TEST_SONGS;
  wax::Document doc(wax::loadProject(songs + "/jawharp.wax"));
  const wax::Instrument& inst = doc.project().instruments[1];
  const std::string iid = inst.id;
  const wax::GraphNode* target = nullptr;
  for (const auto& n : inst.graph.nodes) if (n.params.is_object() && !n.params.empty()) { target = &n; break; }
  check(target != nullptr, "the song has a node with parameters");
  const std::string nid = target->id, pname = target->params.begin().key();
  const wax::Json original = target->params.begin().value();
  auto value = [&]() { for (const auto& n : doc.project().instruments[1].graph.nodes) if (n.id == nid) return n.params[pname]; return wax::Json(); };
  doc.apply(std::make_unique<wax::SetParam>(iid, nid, pname, 0.25));
  for (double v : {0.3, 0.4, 0.5}) doc.apply(std::make_unique<wax::SetParam>(iid, nid, pname, v), true);
  check(value() == 0.5, "the knob's last value is the one held");
  check(doc.takeChangedParams().count(iid + "." + nid + "." + pname) == 1, "the change is reported by address");
  doc.undo();
  check(value() == original && !doc.canUndo(), "one undo takes the whole drag back");
  doc.redo();
  check(value() == 0.5, "redo puts it back");
  const wax::Project reread = wax::parseProject(wax::serializeProject(doc.project()));
  bool saved = false;
  for (const auto& n : reread.instruments[1].graph.nodes) if (n.id == nid) saved = n.params[pname] == 0.5;
  check(saved, "the edited value is in the saved file");
  doc.apply(std::make_unique<wax::SetSilenced>("lone", true));
  check(doc.project().lanes[1].silenced && doc.takeChangedLanes().count("lone") == 1, "silencing a lane marks it changed");
  check(wax::parseProject(wax::serializeProject(doc.project())).lanes[1].silenced, "and the silence is saved");
  doc.undo();
  check(!doc.project().lanes[1].silenced, "undo lets it play again");
  std::printf("ok: parameter edits\n");
}

int main() {
  testBlockEdit();
  testSongSettings();
  testLoop();
  testPlacePattern();
  testAddInstrument();
  testAddNode();
  testUnits();
  testRewire();
  testMoveNode();
  testBankEdit();
  testDrawingEdit();
  testSpline();
  testLevels();
  testSetParam();
  testSave();
  testNewPattern();
  testCellEditor();
  testLiveEdit();
  testDocument();
  testTap();
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
  testRenderSong("convolver");
  testRenderSong("convolverlong");
  testRenderSong("convolverwide");
  testRenderSong("amp");
  testRenderSong("ampclean");
  testRenderSong("route");
  testRenderSong("add");
  testRenderSong("genexpr");
  testRenderSong("genexprfm");
  testRenderSong("genexprpluck");
  testRenderSong("genexprecho");
  testRenderSong("genexprcrush");
  testRenderSong("genexprstress");
  testRenderSong("genexprstressvoice");
  testRenderSong("genexprdefault");
  testRenderSong("genexprbad");
  testRenderSong("tap");
  for (const char* name : {"concrete", "nodes", "mosh", "suite", "effects2", "bugsong", "cloves"}) testThreads(name);
  if (failures) std::printf("%d check%s failed\n", failures, failures == 1 ? "" : "s");
  else std::printf("all tests passed\n");
  return failures;
}

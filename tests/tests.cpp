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
#include "project/samples.h"
#include "edit/document.h"
#include "edit/presets.h"
#include "seq/loop.h"
#include "seq/curve_edit.h"
#include "edit/spline_ops.h"
#include "edit/sync.h"
#include "nodes/bezier.h"
#include "nodes/dx7core.h"
#include "app/units.h"
#include "engine/engine.h"
#include "engine/takes.h"
#include "io/wav.h"
#include "project/project.h"
#include "project/sample_edit.h"
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

// A parameter's live value becomes the cell that gives it back: linear, integer and logarithmic columns.
void testLiveCellText() {
  wax::Column lin; lin.kind = wax::ColumnKind::Param; lin.token = wax::Token::Hex2; lin.min = 0; lin.max = 1; lin.denom = 255;
  check(wax::CellEditor::liveCellText(lin, 0.0) == "00" && wax::CellEditor::liveCellText(lin, 1.0) == "FF" && wax::CellEditor::liveCellText(lin, 0.5) == "80", "a linear value is its share of the range in hex");
  check(wax::CellEditor::liveCellText(lin, 7.0) == "FF" && wax::CellEditor::liveCellText(lin, -3.0) == "00", "a value outside the range is held at the ends");
  wax::Column whole; whole.kind = wax::ColumnKind::Param; whole.token = wax::Token::Hex1; whole.isInt = true; whole.min = 2; whole.max = 12; whole.denom = 10;
  check(wax::CellEditor::liveCellText(whole, 5.0) == "3", "an integer column holds the value less its minimum");
  wax::Column logc; logc.kind = wax::ColumnKind::Param; logc.token = wax::Token::Hex2; logc.log = true; logc.min = 20; logc.max = 20000; logc.denom = 255;
  check(wax::CellEditor::liveCellText(logc, 20.0) == "00" && wax::CellEditor::liveCellText(logc, 20000.0) == "FF" && wax::CellEditor::liveCellText(logc, 112.468) == "40", "a logarithmic column is in equal steps per octave");
  std::printf("ok: live cell text\n");
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

// A sample loaded while the song plays reaches a sampler added at the same time: a note on it sounds.
void testLiveSampler() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax"));
  wax::Engine engine(doc.project(), 44100);
  std::vector<float> tone;
  for (int i = 0; i < 20000; i++) tone.push_back(0.8f * std::sin(i * 0.1f));
  doc.apply(std::make_unique<wax::AddSample>("probe", wax::encodeSample(44100, tone, {})));
  doc.apply(std::make_unique<wax::AddInstrument>("sampler", wax::app::machineJson("sampler", {"probe"}), "sampler"));
  check(engine.sendGraph(doc.project(), "sampler").empty(), "the sampler compiles while the song plays");
  check(engine.sendLane(doc.project(), "sampler"), "its lane is sent");
  engine.applySentLanes();
  std::vector<float> l(wax::kEngineBlock), r(wax::kEngineBlock);
  const auto heard = [&](int blocks) {   // the song is not playing: only a note played now can be heard
    double sum = 0;
    for (int b = 0; b < blocks; b++) {
      engine.sequencer().process(l.data(), r.data(), wax::kEngineBlock);
      for (int i = 0; i < wax::kEngineBlock; i++) sum += std::abs(l[i]) + std::abs(r[i]);
    }
    return sum;
  };
  check(heard(10) == 0, "nothing sounds before a note");
  wax::NoteEvent on;
  on.type = wax::NoteType::On;
  on.note = 60;
  on.vel = 1;
  engine.runtime().note("sampler", on);
  check(heard(60) > 1.0, "a note on the new sampler sounds the sample that was loaded with it");
  // editing the sample while it plays: the nodes that play it are given the new version, and undo gives them the old one back
  check(wax::syncSamples(doc, engine).empty(), "the loaded sample is sent");
  heard(3000);   // let the note die away
  wax::EditableSample probe = wax::readSample(doc.project().samples["probe"]);
  wax::silenceSample(probe, std::nullopt);
  doc.apply(std::make_unique<wax::SetSample>("probe", wax::writeSample(probe, doc.project().samples["probe"]), "Silence"));
  check(wax::syncSamples(doc, engine).empty(), "an edited sample is sent");
  engine.applySentLanes();   // (the audio thread takes what was sent at the start of a block)
  engine.runtime().note("sampler", on);
  check(heard(60) < 0.01, "after silencing the sample the sampler plays silence");
  doc.undo();
  check(wax::syncSamples(doc, engine).empty(), "undo sends the old sample");
  engine.applySentLanes();
  heard(3000);
  engine.runtime().note("sampler", on);
  check(heard(60) > 1.0, "undo brings the sound back");
  check(wax::syncSamples(doc, engine).empty(), "nothing changed, nothing sent");
  std::printf("ok: live sampler\n");
}

void testLaneEdits() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax"));
  const auto ids = [&] { std::string out; for (const wax::Lane& l : doc.project().lanes) out += l.id + ","; return out; };
  const std::string before = ids();
  std::string first;
  for (const wax::Lane& l : doc.project().lanes) if (l.id != "master") { first = l.id; break; }
  doc.apply(std::make_unique<wax::RenameLane>(first, "renamed"));
  check(ids().find("renamed") != std::string::npos && ids().find(first + ",") == std::string::npos, "a lane can be renamed");
  const wax::Project reread = wax::parseProject(wax::serializeProject(doc.project()));
  bool found = false;
  for (const wax::Lane& l : reread.lanes) found |= l.id == "renamed";
  check(found, "the new name is saved");
  doc.undo();
  check(ids() == before, "undo gives the lane its name back");
  doc.apply(std::make_unique<wax::DuplicateLane>(first, "copy"));
  check(doc.project().lanes.size() > 0 && ids().find("copy") != std::string::npos, "a lane can be duplicated");
  doc.undo();
  const std::size_t count = doc.project().lanes.size();
  doc.apply(std::make_unique<wax::RemoveLane>(first, false));
  check(doc.project().lanes.size() == count - 1, "a lane can be removed");
  doc.undo();
  check(ids() == before, "undo puts the lane back in its place");
  std::printf("ok: lane edits\n");
}

void testPatternRenameClone() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax"));
  const wax::Lane* lane = nullptr;
  for (const wax::Lane& l : doc.project().lanes) if (!l.patterns.empty()) { lane = &l; break; }
  check(lane != nullptr, "a lane with patterns");
  const std::string laneId = lane->id, pat = lane->patterns.front().id, oldName = lane->patterns.front().name;
  const std::size_t count = lane->patterns.size();
  doc.apply(std::make_unique<wax::RenamePattern>(laneId, pat, "newname"));
  check(doc.project().lanes[0].id.size() > 0, "project still there");
  const auto nameOf = [&] { for (const wax::Lane& l : doc.project().lanes) if (l.id == laneId) for (const wax::Pattern& p : l.patterns) if (p.id == pat) return p.name; return std::string(); };
  check(nameOf() == "newname", "pattern renamed");
  doc.undo();
  check(nameOf() == oldName, "rename undone");
  doc.apply(std::make_unique<wax::ClonePattern>(laneId, pat, "the-copy"));
  const auto sizeOf = [&] { for (const wax::Lane& l : doc.project().lanes) if (l.id == laneId) return l.patterns.size(); return std::size_t(0); };
  check(sizeOf() == count + 1, "pattern cloned");
  doc.undo();
  check(sizeOf() == count, "clone undone");
  std::printf("ok: pattern rename and clone\n");
}

void testInsertSilence() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax"));
  const int length = doc.project().song.length;
  std::vector<int> starts;
  for (const wax::Lane& l : doc.project().lanes) for (const wax::Placement& p : l.placements) starts.push_back(p.start);
  doc.apply(std::make_unique<wax::InsertSilence>(32, 64));
  check(doc.project().song.length == length + 64, "the song grows");
  std::size_t i = 0; bool moved = true;
  for (const wax::Lane& l : doc.project().lanes) for (const wax::Placement& p : l.placements) { const int was = starts[i++]; moved &= p.start == (was >= 32 ? was + 64 : was); }
  check(moved, "placements from the row on move later");
  const wax::Project saved = wax::parseProject(wax::serializeProject(doc.project()));
  check(saved.song.length == length + 64, "the new length is saved");
  doc.undo();
  i = 0; bool back = doc.project().song.length == length;
  for (const wax::Lane& l : doc.project().lanes) for (const wax::Placement& p : l.placements) back &= p.start == starts[i++];
  check(back, "undo puts everything back");
  {   // cut: a pattern across the row becomes two, undo brings the one back
    wax::Document cutting(wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax"));
    const wax::Project before = cutting.project();
    int crossing = -1, laneAt = -1, cutTick = 0;
    for (std::size_t i = 0; i < before.lanes.size() && crossing < 0; i++)
      for (std::size_t k = 0; k < before.lanes[i].placements.size(); k++) {
        const wax::Placement& p = before.lanes[i].placements[k];
        if (p.length >= 32) { crossing = static_cast<int>(k); laneAt = static_cast<int>(i); cutTick = p.start + 16; break; }
      }
    if (crossing >= 0) {
      cutting.apply(std::make_unique<wax::InsertSilence>(cutTick, 16, true));
      check(cutting.project().lanes[static_cast<std::size_t>(laneAt)].placements.size() == before.lanes[static_cast<std::size_t>(laneAt)].placements.size() + 1, "a pattern across the row is cut in two");
      cutting.undo();
      check(cutting.project().lanes[static_cast<std::size_t>(laneAt)].placements.size() == before.lanes[static_cast<std::size_t>(laneAt)].placements.size() && cutting.project().song.length == before.song.length, "cut undone");
    }
  }
  std::printf("ok: insert silence\n");
}

void testPatternColour() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax"));
  const wax::Lane& lane = doc.project().lanes[1];
  const std::string laneId = lane.id, pat = lane.patterns.front().id;
  doc.apply(std::make_unique<wax::SetPatternColor>(laneId, pat, wax::Json(5)));
  const wax::Project reread = wax::parseProject(wax::serializeProject(doc.project()));
  check(reread.lanes[1].patterns.front().color == wax::Json(5), "a pattern colour is saved");
  doc.undo();
  check(doc.project().lanes[1].patterns.front().color == lane.patterns.front().color, "colour undone");
  std::printf("ok: pattern colour\n");
}

void testSongLength() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax"));
  const int length = doc.project().song.length;
  std::size_t placements = 0;
  for (const wax::Lane& l : doc.project().lanes) placements += l.placements.size();
  doc.apply(std::make_unique<wax::SetSongLength>(64));
  check(doc.project().song.length == 64, "the song is shortened");
  bool inside = true;
  for (const wax::Lane& l : doc.project().lanes) for (const wax::Placement& p : l.placements) inside &= p.start < 64 && p.start + p.length <= 64;
  check(inside, "placements stay inside the new length");
  doc.undo();
  std::size_t back = 0;
  for (const wax::Lane& l : doc.project().lanes) back += l.placements.size();
  check(doc.project().song.length == length && back == placements, "undo brings the placements back");
  doc.apply(std::make_unique<wax::SetSongInfo>("hello\ngreetings"));
  check(wax::parseProject(wax::serializeProject(doc.project())).song.raw.value("info", std::string()) == "hello\ngreetings", "the song info is saved");
  doc.undo();
  check(!doc.project().song.raw.contains("info"), "song info undone");
  doc.apply(std::make_unique<wax::SetSongName>("my song"));
  check(wax::parseProject(wax::serializeProject(doc.project())).song.name == "my song", "the name is saved");
  std::printf("ok: song length and name\n");
}

void testWireEdits() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax"));
  const wax::Instrument* inst = nullptr;
  for (const wax::Instrument& i : doc.project().instruments) if (i.graph.connections.size() >= 2 && i.id != "master") { inst = &i; break; }
  check(inst != nullptr, "an instrument with wires");
  const std::string id = inst->id;
  const auto wires = [&] { for (const wax::Instrument& i : doc.project().instruments) if (i.id == id) return i.graph.connections; return std::vector<wax::Connection>(); };
  const std::vector<wax::Connection> before = wires();
  // a wire back from the end of a chain to its start would make a loop
  const wax::Connection last = before.back();
  bool refused = false;
  try { doc.apply(std::make_unique<wax::Connect>(id, last.to.substr(0, last.to.find('.')) + ".out", before.front().from.substr(0, before.front().from.find('.')) + ".in")); }
  catch (const std::exception&) { refused = true; }
  check(wires().size() == before.size(), "no wire was added by a refused connect");
  (void)refused;
  // moving a wire's end and undoing it
  const wax::Connection first = before.front();
  doc.apply(std::make_unique<wax::MoveWire>(id, first.from, first.to, first.from, first.to));
  check(wires().size() == before.size(), "moving a wire onto itself changes nothing");
  doc.undo();
  check(wires().size() == before.size() && wires().front().from == first.from, "undo puts the wire back in its place");
  std::printf("ok: wire edits\n");
}

void testInstrumentSetup() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax"));
  std::string id;
  for (const wax::Instrument& i : doc.project().instruments) if (i.id != "master") { id = i.id; break; }
  const auto voices = [&](const wax::Project& p) { for (const wax::Instrument& i : p.instruments) if (i.id == id) return i.graph.voices; return 0; };
  const int before = voices(doc.project());
  doc.apply(std::make_unique<wax::SetInstrumentSetup>(id, 4, true));
  const wax::Project reread = wax::parseProject(wax::serializeProject(doc.project()));
  check(voices(reread) == 4, "the voices are saved");
  bool never = false;
  for (const wax::Instrument& i : reread.instruments) if (i.id == id) never = i.graph.neverSleeps;
  check(never, "never sleeping is saved");
  doc.undo();
  check(voices(doc.project()) == before, "voices undone");
  wax::Document many(wax::loadProject(std::string(WAX_TEST_SONGS) + "/cloves.wax"));
  const std::string first = many.project().instruments[1].id;
  many.apply(std::make_unique<wax::MoveInstrument>(first, 1));
  check(many.project().instruments[2].id == first, "an instrument moves later");
  many.undo();
  check(many.project().instruments[1].id == first, "move undone");
  std::printf("ok: instrument setup\n");
}

void testInstrumentManage() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/cloves.wax"));
  const std::string id = doc.project().instruments[1].id;
  const std::size_t count = doc.project().instruments.size();
  doc.apply(std::make_unique<wax::DuplicateInstrument>(id, id + "_copy"));
  check(doc.project().instruments.size() == count + 1 && doc.project().instruments[2].id == id + "_copy", "a copy follows its machine");
  const wax::Project reread = wax::parseProject(wax::serializeProject(doc.project()));
  check(reread.instruments.size() == count + 1, "the copy is saved");
  doc.apply(std::make_unique<wax::RenameInstrument>(id + "_copy", "fresh"));
  check(doc.project().instruments[2].id == "fresh", "renamed");
  bool refused = false;
  try { doc.apply(std::make_unique<wax::RenameInstrument>("fresh", id)); } catch (const std::exception&) { refused = true; }
  check(refused, "a taken name is refused");
  doc.apply(std::make_unique<wax::RemoveInstrument>("fresh"));
  check(doc.project().instruments.size() == count, "removed");
  doc.undo();
  check(doc.project().instruments.size() == count + 1, "remove undone");
  doc.undo(); doc.undo();
  check(doc.project().instruments.size() == count, "all undone");
  std::string played;
  for (const wax::Lane& lane : doc.project().lanes) if (lane.instrument == id) played = lane.id;
  if (!played.empty()) {
    refused = false;
    try { doc.apply(std::make_unique<wax::RemoveInstrument>(id)); } catch (const std::exception&) { refused = true; }
    check(refused, "a played machine stays");
  }
  doc.apply(std::make_unique<wax::SetNoteColumns>(id, 5));
  check(doc.project().instruments[1].noteColumns == 5, "tracks set");
  check(wax::parseProject(wax::serializeProject(doc.project())).instruments[1].noteColumns == 5, "tracks saved");
  doc.undo();
  {   // a lane for a machine, and a clone of a lane with its machine
    const std::size_t lanes = doc.project().lanes.size();
    doc.apply(std::make_unique<wax::AddLane>(id, "extra"));
    check(doc.project().lanes.size() == lanes + 1 && doc.project().lanes.back().instrument == id, "an empty lane for a machine");
    doc.undo();
    std::string source;
    for (const wax::Lane& lane : doc.project().lanes) if (lane.instrument == id) source = lane.id;
    if (!source.empty()) {
      doc.apply(std::make_unique<wax::CloneLaneWithMachine>(source, id + "_c", source + "_c", false, false));
      check(doc.project().instruments.size() == count + 1, "the machine is copied");
      bool empty = false;
      for (const wax::Lane& lane : doc.project().lanes) if (lane.id == source + "_c") empty = lane.placements.empty() && lane.instrument == id + "_c";
      check(empty, "the clone starts empty on its own machine");
      doc.undo();
      check(doc.project().instruments.size() == count && doc.project().lanes.size() == lanes, "clone undone");
    }
  }
  std::printf("ok: instrument manage\n");
}

// The editing side of lane curves: anchors, snapping, pins following their pattern, sections with gaps.
void testCurveEdit() {
  using wax::CurvePin;
  wax::Lane lane;
  lane.id = "l";
  for (const char* id : {"a", "b"}) { wax::Pattern p; p.id = id; p.rows.assign(16, ""); lane.patterns.push_back(p); }
  lane.placements = {{"a", 0, 16}, {"b", 32, 16}};
  wax::LaneCurve curve = wax::newLaneCurve("filter.cutoff", 4, 0.25);
  check(curve.pts.size() == 1 && curve.kinds.empty(), "a new curve has one anchor");
  check(wax::addPoint(curve, 20, 0.75) == 1 && curve.kinds.size() == 1 && curve.kinds[0] == 'c', "a second anchor makes a curved segment");
  check(wax::addPoint(curve, 12, 0.5) == 1 && curve.kinds.size() == 2, "an anchor between splits the segment");
  check(wax::addPoint(curve, 12, 0.6) == 1 && curve.pts.size() == 3 && curve.pts[1].v == 0.6, "an anchor on an anchor takes its value");
  check(std::isnan(wax::curveValueAt(curve, 2)) && std::isnan(wax::curveValueAt(curve, 21)), "no value before the first anchor or after the last");
  check(wax::curveValueAt(curve, 4) == 0.25 && wax::curveValueAt(curve, 20) == 0.75, "the anchors' own values");
  curve.kinds[0] = 'l';
  check(std::abs(wax::curveValueAt(curve, 8) - 0.425) < 1e-9, "a straight segment is linear");
  check(wax::toggleGap(curve, 1) && std::isnan(wax::curveValueAt(curve, 16)), "a gap has no value");
  check(wax::toggleGap(curve, 1) && !std::isnan(wax::curveValueAt(curve, 16)), "and joins again");
  // snapping: pattern edges first (a start beats an end at the same tick), then rows
  const wax::CurveSnap edge = wax::snapTick(lane, 30.0, true, 4, 6, 64);
  check(edge.t == 32 && edge.pin == CurvePin::Start, "near an edge it lands on the edge, pinned");
  const wax::CurveSnap tied = wax::snapTick(lane, 16.2, true, 4, 6, 64);
  check(tied.t == 16 && tied.pin == CurvePin::End, "the end of a pattern with nothing starting there");
  const wax::CurveSnap row = wax::snapTick(lane, 25.0, true, 4, 3, 64);
  check(row.t == 24 && row.pin == CurvePin::None, "away from edges it lands on a row");
  // moving keeps order and lets go of a pin
  wax::movePoint(curve, 1, 100, 2.0);
  check(curve.pts[1].t == 19 && curve.pts[1].v == 1.0, "an anchor stops one tick short of the next and the value is held to 0..1");
  check(wax::removePoint(curve, 1) && curve.pts.size() == 2 && curve.kinds.size() == 1, "removing an anchor drops a kind");
  // pins follow the pattern edge they sat on
  wax::LaneCurve pinned = wax::newLaneCurve("x.y", 32, 0.5);
  pinned.pts[0].pin = CurvePin::Start;
  wax::addPoint(pinned, 48, 0.5, CurvePin::End);
  const std::vector<wax::CurveSpan> before = wax::curveSpans(lane);
  lane.placements[1].start = 64;
  const std::vector<wax::CurveSpan> after = wax::curveSpans(lane);
  check(wax::followPins(pinned, before, after) == 0 && pinned.pts[0].t == 64 && pinned.pts[1].t == 80, "a pinned section moves with its pattern");
  lane.placements.pop_back();
  check(wax::followPins(pinned, after, wax::curveSpans(lane)) == 2 && pinned.pts[0].pin == CurvePin::None, "pins let go when the pattern is gone");
  // sections, extend and skip
  lane.placements = {{"a", 0, 16}, {"b", 32, 16}, {"a", 48, 16}};
  wax::LaneCurve sec = wax::newLaneCurve("x.y", 0, 0.5);
  check(wax::addSection(sec, 0, 16, 0.5) == false, "a section will not go over an anchor");
  sec.pts.clear(); sec.kinds.clear();
  check(wax::addSection(sec, 0, 16, 0.5) && sec.pts.size() == 2 && sec.kinds[0] == 'c', "a section: two pinned anchors joined");
  check(wax::addSection(sec, 32, 48, 0.8) && sec.kinds[1] == 'g' && wax::curveSections(sec).size() == 2, "a second section is apart, across a gap");
  check(wax::removeSection(sec, 0, 16) && sec.pts.size() == 2, "removing a section keeps the other");
  wax::LaneCurve grow = wax::newLaneCurve("x.y", 0, 0.5);
  grow.pts[0].pin = CurvePin::Start;
  const int ext = wax::extendCurve(lane, grow, 0, 64);
  check(ext == 1 && grow.pts[1].t == 16 && grow.pts[1].pin == CurvePin::End, "extend: an anchor at the end of the next pattern");
  const int skip = wax::skipCurve(lane, grow, 1, 64);
  check(skip == 3 && grow.kinds[1] == 'g' && grow.pts[2].t == 48 && grow.pts[3].t == 64, "skip: a gap over a pattern, a new section over the next");
  // reading and writing keep what editing does not touch
  const wax::Json written = wax::writeLaneCurve(grow);
  check(written["target"] == "x.y" && written["pts"].size() == 4 && wax::readLaneCurve(written).pts.size() == 4, "a curve round-trips through its json");
  std::printf("ok: curve editing\n");
}

// Curves in a document: an edit is undone as one step, and pinned anchors follow their pattern when an edit moves it.
void testCurveDocument() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/curves.wax"));
  const wax::Lane* bass = nullptr;
  for (const wax::Lane& l : doc.project().lanes) if (l.id == "bass") bass = &l;
  check(bass != nullptr, "the song has a bass lane");
  wax::LaneCurve curve = wax::readLaneCurve(bass->raw["curves"][0]);
  curve.pts.clear(); curve.kinds.clear();
  wax::addSection(curve, 48, 64, 0.5);   // over the second pattern, pinned at both ends
  wax::Json list = wax::Json::array();
  list.push_back(wax::writeLaneCurve(curve));
  doc.apply(std::make_unique<wax::SetCurves>("bass", list, "Edit curve"));
  doc.takeChangedLanes();
  const auto firstTick = [&]() { for (const wax::Lane& l : doc.project().lanes) if (l.id == "bass") return l.raw["curves"][0]["pts"][0]["t"].get<double>(); return -1.0; };
  check(firstTick() == 48, "the anchor is where it was put");
  doc.apply(std::make_unique<wax::InsertSilence>(40, 16));
  check(firstTick() == 64, "a pinned anchor follows its pattern when silence is inserted before it");
  check(doc.takeChangedLanes().count("bass") == 1, "the lane is rebuilt");
  doc.undo();
  check(firstTick() == 48, "one undo puts the pattern and the anchor back");
  doc.redo();
  check(firstTick() == 64, "and redo moves both again");
  doc.undo();
  // dragging folds into one undo step
  wax::Json a = wax::Json::array(), b = wax::Json::array();
  a.push_back(wax::writeLaneCurve(wax::newLaneCurve("sq.cutoff", 10, 0.1)));
  b.push_back(wax::writeLaneCurve(wax::newLaneCurve("sq.cutoff", 12, 0.2)));
  doc.apply(std::make_unique<wax::SetCurves>("bass", a, "Move anchor"));
  doc.apply(std::make_unique<wax::SetCurves>("bass", b, "Move anchor"), true);
  doc.undo();
  check(firstTick() == 48, "a drag is one undo step");
  std::printf("ok: curves in a document\n");
}

// Samples loaded into a project: named as the web app names them, kept as 16-bit PCM in the file, given to the machines that play them.
void testSampleEdit() {
  using namespace wax;
  const auto ramp = [](int n) { EditableSample s; s.rate = 8000; for (int i = 0; i < n; i++) s.left.push_back(static_cast<float>(i) / 100.0f); return s; };
  const auto throws = [](auto fn) { try { fn(); } catch (const std::runtime_error&) { return true; } return false; };

  EditableSample s = ramp(20);
  s.props["loopStart"] = 4; s.props["loopEnd"] = 12; s.props["loopOn"] = true; s.props["slices"] = Json::array({2, 8, 15});
  cropSample(s, SampleRange{3, 14});
  check(s.frames() == 11 && std::abs(s.left[0] - 0.03f) < 1e-6 && s.number("loopStart") == 1 && s.number("loopEnd") == 9, "crop keeps the range and moves the loop");
  check(s.props["slices"] == Json::array({5}), "crop keeps and moves the slice markers inside");

  s = ramp(20); s.props["loopStart"] = 4; s.props["loopEnd"] = 12;
  cropSample(s, SampleRange{5, 14});
  check(!s.props.contains("loopEnd"), "a loop that reaches outside the crop is dropped");
  s = ramp(20); s.props["loopStart"] = 4; s.props["loopEnd"] = 12;
  cropSample(s, std::nullopt);
  check(s.frames() == 8 && s.number("loopStart") == 0 && s.number("loopEnd") == 8, "crop with no selection keeps the loop region");
  check(throws([&] { EditableSample t = ramp(5); cropSample(t, std::nullopt); }), "crop with nothing to keep says so");

  s = ramp(20); s.props["loopStart"] = 10; s.props["loopEnd"] = 18;
  cutSample(s, SampleRange{2, 6});
  check(s.frames() == 16 && std::abs(s.left[2] - 0.06f) < 1e-6 && s.number("loopStart") == 6 && s.number("loopEnd") == 14, "cut closes the gap and moves the loop");
  check(throws([&] { EditableSample t = ramp(5); cutSample(t, SampleRange{0, 5}); }), "cut cannot remove everything");

  s = ramp(10);
  reverseSample(s, SampleRange{2, 6});
  check(std::abs(s.left[2] - 0.05f) < 1e-6 && std::abs(s.left[5] - 0.02f) < 1e-6 && std::abs(s.left[6] - 0.06f) < 1e-6, "reverse flips only the range");
  silenceSample(s, SampleRange{0, 3});
  check(s.left[0] == 0 && s.left[2] == 0 && s.left[3] != 0, "silence zeroes the range");

  s = ramp(5); s.right = {0, 0, 0.8f, 0, 0};
  normaliseSample(s, std::nullopt);
  check(std::abs(s.right[2] - 1.0f) < 1e-6 && std::abs(s.left[4] - 0.04f / 0.8f) < 1e-6, "normalise scales both channels by the loudest frame");
  check(throws([&] { EditableSample t; t.left = {0, 0, 0}; normaliseSample(t, std::nullopt); }), "a silent range cannot be normalised");

  s = EditableSample(); s.left.assign(4, 1.0f);
  fadeInSample(s, std::nullopt);
  check(s.left[0] == 0 && std::abs(s.left[2] - 0.5f) < 1e-6, "fade in");
  s.left.assign(4, 1.0f); fadeOutSample(s, std::nullopt);
  check(s.left[0] == 1 && std::abs(s.left[3] - 0.25f) < 1e-6, "fade out");
  s.left = {1.5f, 2.5f, 3.5f}; removeDcSample(s, std::nullopt);
  check(std::abs(s.left[0] + 1) < 1e-6 && std::abs(s.left[1]) < 1e-6 && std::abs(s.left[2] - 1) < 1e-6, "remove DC");
  gainSample(s, std::nullopt, 2);
  check(std::abs(s.left[2] - 2) < 1e-6, "gain");

  s = ramp(10); s.props["loopStart"] = 6; s.props["loopEnd"] = 9; s.props["slices"] = Json::array({1, 7});
  SampleClip clip; clip.left = {9, 9};
  pasteClip(s, 4, clip, PasteMode::Insert);
  check(s.frames() == 12 && s.left[4] == 9 && std::abs(s.left[6] - 0.04f) < 1e-6 && s.number("loopStart") == 8 && s.number("loopEnd") == 11 && s.props["slices"] == Json::array({1, 9}), "paste inserts and moves what follows");
  s = ramp(10);
  pasteClip(s, 9, clip, PasteMode::Overwrite);
  check(s.frames() == 11 && s.left[9] == 9 && s.left[10] == 9 && std::abs(s.left[8] - 0.08f) < 1e-6, "paste overwrite grows past the end");
  s = ramp(4); s.right = {0, 0, 0, 0};
  SampleClip mono; mono.left = {1, 1};
  pasteClip(s, 1, mono, PasteMode::Overwrite);
  check(s.right[1] == 1 && s.right[2] == 1, "a mono clip goes to both sides of a stereo sample");
  s = ramp(4); SampleClip st; st.left = {1}; st.right = {0};
  pasteClip(s, 0, st, PasteMode::Overwrite);
  check(std::abs(s.left[0] - 0.5f) < 1e-6, "a stereo clip is mixed down into a mono sample");
  check(throws([&] { EditableSample t = ramp(3); pasteClip(t, 0, SampleClip(), PasteMode::Insert); }), "an empty clip cannot be pasted");

  s = ramp(20); s.props["loopStart"] = 4; s.props["loopEnd"] = 12; s.props["loopXfade"] = 2;
  const float before = s.left[11];
  bakeCrossfades(s);
  check(!s.props.contains("loopXfade") && s.left[11] != before && s.left[10] == 0.1f, "baking writes the crossfade in and clears it");
  check(throws([&] { bakeCrossfades(s); }), "nothing left to bake");

  s = ramp(6); s.props["volume"] = 0.5; s.props["pan"] = -0.25;
  const EditableSample back = readSample(writeSample(s));
  check(back.frames() == 6 && back.rate == 8000 && back.number("volume") == 0.5 && back.number("pan") == -0.25 && std::abs(back.left[5] - 0.05f) < 1e-4, "a sample survives writing and reading");
  s = ramp(10);
  setLoopPoints(s, 2, 8, false);
  check(s.number("loopStart") == 2 && s.number("loopEnd") == 8 && !loopActive(s, false), "a new loop starts switched off");
  s.props["loopOn"] = true;
  check(loopActive(s, false) && !loopActive(s, true), "a loop plays when it has points and is on");
  check(throws([&] { setLoopPoints(s, 4, 5, false); }) && throws([&] { setLoopPoints(s, 0, 11, false); }), "loops are two frames or more and inside the sample");
  setLoopPoints(s, 0, 0, false);
  check(!s.props.contains("loopEnd") && !s.props.contains("loopOn"), "0 and 0 take the loop away");
  setSliceMarkers(s, {7, 3, 7});
  check(sliceMarkers(s) == std::vector<int>({3, 7}) && throws([&] { setSliceMarkers(s, {10}); }), "slice markers sorted, unique, inside");
  setSampleLevel(s, "volume", 0.5); setSampleLevel(s, "pan", 0);
  check(s.number("volume") == 0.5 && !s.props.contains("pan") && throws([&] { setSampleLevel(s, "pan", 2); }), "levels: defaults are not stored");
  Json original = Json::object(); original["taken"] = true; original["volume"] = 0.3;
  const Json written = writeSample(ramp(4), original);
  check(written.value("taken", false) && !written.contains("volume"), "writing keeps the other keys and drops settings the sample no longer has");
  s = ramp(10);
  check(throws([&] { applySampleTool(s, "nonsense", std::nullopt); }) && throws([&] { applySampleTool(s, "cut", std::nullopt); }), "unknown tools and a cut with no selection are refused");
}

void testSampleLoading() {
  check(wax::sampleIdFromName("My Kick (01).wav") == "My_Kick_01_" && wax::sampleIdFromName("808.wav") == "_808" && wax::sampleIdFromName("...") == "_" && wax::sampleIdFromName(".wav") == "sample" && wax::sampleIdFromName("") == "sample", "a file name becomes an id");
  check(wax::sampleIdFromName("a_very_long_sample_name_that_goes_on_and_on.wav").size() == 24, "an id is at most 24 characters");
  wax::Json taken = wax::Json::object();
  taken["kick"] = wax::Json::object();
  taken["kick2"] = wax::Json::object();
  check(wax::uniqueSampleId(taken, "kick") == "kick3" && wax::uniqueSampleId(taken, "snare") == "snare", "a taken id counts up");
  check(wax::isPoolSample("tap01") && wax::isPoolSample("tap16") && !wax::isPoolSample("tap17") && !wax::isPoolSample("tap00") && !wax::isPoolSample("tape"), "the tap pool's slots");
  std::vector<float> left, right;
  for (int i = 0; i < 1000; i++) { left.push_back(std::sin(i * 0.05f) * 0.8f); right.push_back(std::cos(i * 0.03f) * 0.5f); }
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/cloves.wax"));
  doc.apply(std::make_unique<wax::AddSample>("probe", wax::encodeSample(48000, left, right)));
  const wax::Project reread = wax::parseProject(wax::serializeProject(doc.project()));
  const auto decoded = wax::decodeSamples(reread.samples);
  check(decoded.count("probe") == 1, "a loaded sample is saved with the song");
  const auto& probe = *decoded.at("probe");
  check(probe.rate == 48000 && probe.data.size() == 1000 && probe.right.size() == 1000, "its rate and both channels come back");
  double worst = 0;
  for (int i = 0; i < 1000; i++) worst = std::max({worst, std::abs(static_cast<double>(probe.data[i]) - left[i]), std::abs(static_cast<double>(probe.right[i]) - right[i])});
  check(worst < 1.0 / 32767 + 1e-6, "16-bit PCM keeps it to within one step");
  wax::Json mono = wax::encodeSample(44100, left, {});
  check(!mono.contains("right"), "a mono sample has one channel");
  doc.undo();
  check(!doc.project().samples.contains("probe"), "undo takes the sample out again");
  const wax::Json sampler = wax::app::machineJson("sampler", {"probe", "other"});
  check(sampler["samples"]["sampler"] == "probe", "a sampler is given the first sample");
  const wax::Json tracker = wax::app::machineJson("modchan", {"probe", "other"});
  check(tracker["samples"]["ch"].size() == 2, "a MOD channel is given them all, as its slots");
  std::printf("ok: sample loading\n");
}

void testPresets() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/cloves.wax"));
  const wax::Instrument& first = doc.project().instruments[1];
  const std::string id = first.id;
  const auto none = [](const std::string&) { return std::optional<double>(); };
  const wax::Json values = wax::capturePreset(first, none);
  check(values.is_object() && !values.empty(), "a preset holds the parameters");
  int a = -1, b = -1;
  doc.apply(std::make_unique<wax::SetPresets>(id, wax::presetsWithAdded(first, "bright", values, a), "Save preset"));
  doc.apply(std::make_unique<wax::SetPresets>(id, wax::presetsWithAdded(doc.project().instruments[1], "", values, b), "Save preset"));
  check(a >= 0 && b == a + 1, "the lowest free number each time");
  const wax::Project reread = wax::parseProject(wax::serializeProject(doc.project()));
  check(reread.instruments[1].raw.value("presets", wax::Json::array()).size() >= 2, "presets are saved");
  // a recalled preset sets the parameters, and undo puts them back
  std::string firstKey = values.begin().key();
  const std::size_t dot = firstKey.find('.');
  doc.apply(std::make_unique<wax::SetParam>(id, firstKey.substr(0, dot), firstKey.substr(dot + 1), 0.123));
  doc.apply(std::make_unique<wax::RecallPreset>(id, a), false);
  bool restored = false;
  for (const wax::GraphNode& n : doc.project().instruments[1].graph.nodes)
    if (n.id == firstKey.substr(0, dot)) restored = n.params.value(firstKey.substr(dot + 1), -1.0) == values.begin().value().get<double>();
  check(restored, "recall sets the values");
  doc.undo();
  doc.apply(std::make_unique<wax::SetPresets>(id, wax::presetsWithout(doc.project().instruments[1], a), "Remove preset"));
  check(doc.project().instruments[1].raw["presets"].size() == 1, "a preset is removed, the other keeps its number");
  doc.undo();
  check(doc.project().instruments[1].raw["presets"].size() == 2, "remove undone");
  // a preset can carry drawings: recalling puts them on their node, undo takes them off
  const std::string drawnNode = firstKey.substr(0, dot);
  wax::Json drawn = wax::Json::object();
  drawn[drawnNode] = wax::Json{{"wave", wax::Json::array({1, 2, 3})}};
  int c = -1;
  doc.apply(std::make_unique<wax::SetPresets>(id, wax::presetsWithAdded(doc.project().instruments[1], "drawn", values, c, drawn), "Save preset"));
  doc.apply(std::make_unique<wax::RecallPreset>(id, c), false);
  const auto dataOf = [&]() { for (const wax::GraphNode& n : doc.project().instruments[1].graph.nodes) if (n.id == drawnNode) return n.data; return wax::Json(); };
  check(dataOf().is_object() && dataOf().value("wave", wax::Json::array()).size() == 3, "recall sets the drawings");
  check(wax::capturePresetData(doc.project().instruments[1]).contains(drawnNode), "the drawings are captured again");
  doc.undo();
  check(!dataOf().is_object() || !dataOf().contains("wave"), "undo takes the drawings off again");
  std::printf("ok: presets\n");
}

void testGraphSaved() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/cloves.wax"));
  const wax::Instrument& first = doc.project().instruments[1];
  const std::string id = first.id;
  const wax::Connection wire = first.graph.connections.front();
  const std::size_t wires = first.graph.connections.size(), nodes = first.graph.nodes.size();
  doc.apply(std::make_unique<wax::Disconnect>(id, wire.from, wire.to));
  doc.apply(std::make_unique<wax::AddNode>(id, "gain", "extra_gain", 40, 40));
  const wax::Project reread = wax::parseProject(wax::serializeProject(doc.project()));
  check(reread.instruments[1].graph.connections.size() == wires - 1, "a cut wire is saved");
  check(reread.instruments[1].graph.nodes.size() == nodes + 1, "an added node is saved");
  doc.apply(std::make_unique<wax::DeleteNode>(id, "extra_gain"));
  check(wax::parseProject(wax::serializeProject(doc.project())).instruments[1].graph.nodes.size() == nodes, "a deleted node leaves the file");
  std::printf("ok: graph saved\n");
}

void testExposePort() {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/cloves.wax"));
  const wax::Instrument& first = doc.project().instruments[1];
  const std::string id = first.id;
  const std::size_t shown = first.exposed.size();
  const wax::Exposed one = first.exposed.front();
  doc.apply(std::make_unique<wax::SetExposed>(id, one.node, one.param, false));
  check(doc.project().instruments[1].exposed.size() == shown - 1, "a parameter is hidden");
  check(wax::parseProject(wax::serializeProject(doc.project())).instruments[1].exposed.size() == shown - 1, "the hiding is saved");
  doc.undo();
  doc.apply(std::make_unique<wax::SetExposedBits>(id, one.node, one.param, 16));
  check(wax::parseProject(wax::serializeProject(doc.project())).instruments[1].exposed.front().bits == 16, "the width is saved");
  doc.undo();
  doc.apply(std::make_unique<wax::SetPort>(id, one.node, one.param, true));
  check(!wax::parseProject(wax::serializeProject(doc.project())).instruments[1].graph.nodes.front().ports.empty() || doc.project().instruments[1].graph.nodes.size() > 1, "a port is added");
  doc.undo();
  std::printf("ok: expose and port\n");
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

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--samples") {   // just the sample tests (the whole run takes a long while)
    testSampleLoading();
    testSampleEdit();
    testLiveSampler();
    if (failures) std::printf("%d check%s failed\n", failures, failures == 1 ? "" : "s");
    else std::printf("sample tests passed\n");
    return failures;
  }
  testBlockEdit();
  testSongSettings();
  testLiveCellText();
  testLoop();
  testPlacePattern();
  testAddInstrument();
  testLiveSampler();
  testLaneEdits();
  testPatternRenameClone();
  testInsertSilence();
  testPatternColour();
  testSongLength();
  testWireEdits();
  testInstrumentSetup();
  testInstrumentManage();
  testCurveEdit();
  testCurveDocument();
  testSampleLoading();
  testSampleEdit();
  testPresets();
  testGraphSaved();
  testExposePort();
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

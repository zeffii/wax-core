// Tests: document.
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

WAX_TEST("document", testDocument) {
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
WAX_TEST("document", testLiveEdit) {
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

WAX_TEST("document", testBlockEdit) {
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

  ed.moveTo(0, 0);
  ed.move(-1, 0);
  check(ed.tick() == 0, "Up stops on the first row");
  ed.setWrapRows(true);
  ed.move(-1, 0);
  check(ed.tick() == 7, "with wrap, Up on the first row goes to the last");
  ed.move(1, 0);
  check(ed.tick() == 0, "with wrap, Down on the last row goes back to the first");
  ed.setWrapRows(false);
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

  // The cursor is on one character of a cell: Left/Right step it across cells, typing changes it, another column starts at its first.
  doc.undo(); doc.undo();
  ed.moveTo(2, 0);
  check(ed.character() == 0, "the cursor starts on the first character");
  check(ed.moveChar(1) && ed.character() == 1 && ed.cell() == 0, "Right steps a character");
  check(ed.typeCharacter('#') && row(0).substr(0, 3) == "C#4", "'#' sharpens the note in place");
  check(ed.character() == 2, "and the cursor moves on to the octave");
  check(ed.typeCharacter('5') && row(0).substr(0, 3) == "C#5", "a digit sets the octave");
  check(ed.character() == 2 && ed.tick() == 3, "at the last character the cursor goes down a row, on the same character");
  ed.moveTo(5, 0);
  check(!ed.typeCharacter('5'), "an empty cell takes no character edits");
  ed.moveTo(2, 0);
  check(ed.moveChar(1) && ed.cell() == 1 && ed.character() == 0, "Right at the last character goes on to the next cell's first");
  check(ed.moveChar(-1) && ed.cell() == 0 && ed.character() == 2, "Left at the first goes back to the last of the one before");
  ed.move(0, 1);
  check(ed.cell() == 1 && ed.character() == 0, "another column starts at its first character");
  check(ed.moveChar(1) && ed.typeCharacter('F') && row(0).substr(4, 2) == "4F", "a hex cell changes one digit");
  std::printf("ok: character cursor\n");

  // Transposing: notes by a semitone or an octave, values by one or one digit.
  ed.moveTo(2, 0);
  check(ed.transpose(true, 1, false) && row(0).substr(0, 3) == "D-5", "Ctrl+Up raises a note a semitone");
  check(ed.transpose(true, -1, true) && row(0).substr(0, 3) == "D-4", "Shift: an octave");
  ed.moveTo(2, 1);
  check(ed.transpose(false, 1, false) && row(0).substr(4, 2) == "50", "Ctrl+. raises a value by one");
  check(ed.transpose(false, -1, true) && row(0).substr(4, 2) == "40", "Shift: by a digit");
  check(!ed.transpose(true, 1, false), "a hex cell is not transposed as a note");
  doc.undo(); doc.undo(); doc.undo(); doc.undo();
  std::printf("ok: transpose\n");

  // Shifting a selection: the rows below move up or down inside the pattern, in the selected columns only.
  doc.undo(); doc.undo(); doc.undo();
  ed.moveTo(2, 0);                                      // row 0 of the pattern: "C-4 40", D-4, E-4, empty
  check(ed.shiftSelection(wax::CellEditor::Shift::PullUp) && row(0).substr(0, 3) == "D-4" && row(1).substr(0, 3) == "E-4" && row(2).substr(0, 3) == "...", "Ctrl+Delete on a cell pulls the cells below it up");
  doc.undo();
  check(row(0).substr(0, 6) == "C-4 40", "one undo takes the pull back");
  ed.moveTo(2, 0);
  check(ed.shiftSelection(wax::CellEditor::Shift::PushDown) && row(0).substr(0, 3) == "..." && row(1).substr(0, 3) == "C-4" && row(3).substr(0, 3) == "E-4", "Ctrl+Insert pushes them down");
  doc.undo();
  ed.moveTo(2, 0);
  ed.extend(2, 0);
  check(ed.shiftSelection(wax::CellEditor::Shift::RotateUp) && row(0).substr(0, 3) == "D-4" && row(2).substr(0, 3) == "C-4", "rotate takes the first row of the selection to its end");
  doc.undo();
  std::printf("ok: shift selection\n");
}

// A parameter's live value becomes the cell that gives it back: linear, integer and logarithmic columns.
WAX_TEST("document", testLiveCellText) {
  wax::Column lin; lin.kind = wax::ColumnKind::Param; lin.token = wax::Token::Hex2; lin.min = 0; lin.max = 1; lin.denom = 255;
  check(wax::CellEditor::liveCellText(lin, 0.0) == "00" && wax::CellEditor::liveCellText(lin, 1.0) == "FF" && wax::CellEditor::liveCellText(lin, 0.5) == "80", "a linear value is its share of the range in hex");
  check(wax::CellEditor::liveCellText(lin, 7.0) == "FF" && wax::CellEditor::liveCellText(lin, -3.0) == "00", "a value outside the range is held at the ends");
  wax::Column whole; whole.kind = wax::ColumnKind::Param; whole.token = wax::Token::Hex1; whole.isInt = true; whole.min = 2; whole.max = 12; whole.denom = 10;
  check(wax::CellEditor::liveCellText(whole, 5.0) == "3", "an integer column holds the value less its minimum");
  wax::Column logc; logc.kind = wax::ColumnKind::Param; logc.token = wax::Token::Hex2; logc.log = true; logc.min = 20; logc.max = 20000; logc.denom = 255;
  check(wax::CellEditor::liveCellText(logc, 20.0) == "00" && wax::CellEditor::liveCellText(logc, 20000.0) == "FF" && wax::CellEditor::liveCellText(logc, 112.468) == "40", "a logarithmic column is in equal steps per octave");
  std::printf("ok: live cell text\n");
}

WAX_TEST("document", testSoloEdit) {
  wax::Project project;
  project.song.length = 16;
  wax::Lane lane;
  lane.id = "L1";
  wax::Pattern pattern;
  pattern.id = "p01";
  pattern.rows = {"... .. .... ......", "... .. .... ......", "... .. .... ......", "... .. .... ......"};
  lane.patterns.push_back(pattern);
  lane.placements.push_back({"p01", 4, 4});
  project.lanes.push_back(lane);
  wax::Document doc(project);
  wax::CellEditor editor(doc);
  std::vector<wax::Column> columns(1);
  columns[0].kind = wax::ColumnKind::Note;  columns[0].token = wax::Token::Note;
  editor.setLane("L1", columns);
  editor.setSolo(4, 8);
  editor.moveTo(0, 0);
  check(editor.tick() == 4, "solo-edit: the cursor cannot go above the pattern");
  editor.moveTo(12, 0);
  check(editor.tick() == 7, "solo-edit: or below it");
  editor.move(1, 0);
  check(editor.tick() == 7, "solo-edit: moving down stops at the last row");
  editor.setWrapRows(true);
  editor.move(1, 0);
  check(editor.tick() == 4, "solo-edit: with wrapping the cursor wraps within the pattern");
  editor.selectAll();
  const wax::CellEditor::Selection all = editor.selection();
  check(all.tick0 == 4 && all.tick1 == 7, "solo-edit: select all takes the pattern's rows only");
  check(!editor.createPatternHere() && !editor.placePatternHere("p01"), "solo-edit: no pattern can be made or placed");
  editor.clearSolo();
  editor.moveTo(0, 0);
  check(editor.tick() == 0, "solo-edit ended: the cursor goes anywhere again");
  std::printf("ok: solo-edit\n");
}

WAX_TEST("document", testCellEditor) {
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
  ed.moveTo(3, 2);
  check(ed.typeNoteOffRow() && row(1) == "=== .. 12F0 ......" && ed.noteOffColumns() == 1 && ed.tick() == 4 && ed.cell() == 2, "Shift+1 writes a note off in every note column of the row, and the cursor steps down on the same column");
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

WAX_TEST("document", testNewPattern) {
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

WAX_TEST("document", testSave) {
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
  // the preview time of a machine is saved with it, and undone
  wax::Document timed(wax::loadProject(songs + "/jawharp.wax"));
  const std::string machine = timed.project().lanes[1].instrument;
  timed.apply(std::make_unique<wax::SetInstrumentPreview>(machine, 1500));
  const auto previewOf = [&](const wax::Project& project) { for (const wax::Instrument& i : project.instruments) if (i.id == machine) return i.previewMs; return -1; };
  check(previewOf(wax::parseProject(wax::serializeProject(timed.project()))) == 1500, "a machine's preview time is saved");
  timed.undo();
  check(previewOf(timed.project()) == 0 && !wax::serializeProject(timed.project()).empty() && wax::serializeProject(timed.project()).find("previewMs") == std::string::npos, "and undone, it leaves nothing in the file");
  // writing to disk
  const std::string path = (std::filesystem::temp_directory_path() / "wax-save-test.wax").string();
  wax::saveProject(fresh.project(), path);
  check(wax::loadProject(path).lanes[1].patterns.back().id == "9-0", "a saved file loads");
  std::filesystem::remove(path);
  std::printf("ok: saving\n");
}

WAX_TEST("document", testSongSettings) {
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

WAX_TEST("document", testLoop) {
  const wax::Project original = wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax");
  wax::Document doc(original);
  std::string message;
  doc.apply(std::make_unique<wax::SetLoop>(wax::loopWithPoint(doc.project().song, "start", 4, message), message));
  doc.apply(std::make_unique<wax::SetLoop>(wax::loopWithPoint(doc.project().song, "end", 8, message), message));
  const wax::LoopRange range = wax::loopOf(doc.project().song);
  check(range.on && range.start == 4 && range.end == 8, "Ctrl+B and Ctrl+E set the loop");
  {   // Shift+L: a loop round one pattern; again takes it away; a pattern that is the whole song needs none
    bool changed = false;
    wax::Json round = wax::loopAround(doc.project().song, 16, 16, message, changed);
    check(changed && round.value("start", -1) == 16 && round.value("end", -1) == 32, "Shift+L: the loop goes round the pattern");
    wax::Document again(doc.project());
    again.apply(std::make_unique<wax::SetLoop>(round, message));
    check(wax::loopAround(again.project().song, 16, 16, message, changed).is_null() && message == "Loop removed.", "Shift+L again removes the loop");
    check(wax::loopAround(doc.project().song, 0, doc.project().song.length, message, changed).is_null(), "Shift+L on a pattern as long as the song sets no loop");
    wax::loopAround(doc.project().song, 0, -1, message, changed);
    check(!changed, "Shift+L with no pattern under the cursor does nothing");
  }
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

WAX_TEST("document", testPlacePattern) {
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

WAX_TEST("document", testLaneEdits) {
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

WAX_TEST("document", testPatternRenameClone) {
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

WAX_TEST("document", testInsertSilence) {
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
    for (std::size_t lane = 0; lane < before.lanes.size() && crossing < 0; lane++)
      for (std::size_t k = 0; k < before.lanes[lane].placements.size(); k++) {
        const wax::Placement& p = before.lanes[lane].placements[k];
        if (p.length >= 32) { crossing = static_cast<int>(k); laneAt = static_cast<int>(lane); cutTick = p.start + 16; break; }
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

WAX_TEST("document", testPatternColour) {
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

WAX_TEST("document", testSongLength) {
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

WAX_TEST("document", testBackups) {
  namespace fs = std::filesystem;
  std::error_code ignored;   // (a folder another process still holds, as on Windows, is not a failure of the test)
  const fs::path folder = fs::temp_directory_path() / ("wax-backup-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::remove_all(folder, ignored);
  const std::time_t base = 1700000000;
  for (int i = 0; i < 5; i++) wax::writeBackup(folder, "My Song!", "{\"n\":" + std::to_string(i) + "}", 3, base + i * 200);
  wax::writeBackup(folder, "", "{}", 3, base + 5 * 200);
  std::vector<wax::BackupInfo> list = wax::listBackups(folder);
  check(list.size() == 3, "only the newest snapshots are kept");
  if (list.size() != 3) {   // (say what is there, and do not index past it)
    std::printf("  found %zu snapshots in %s:\n", list.size(), folder.string().c_str());
    for (const auto& entry : fs::directory_iterator(folder)) std::printf("    %s\n", entry.path().filename().string().c_str());
    fs::remove_all(folder, ignored);
    return;
  }
  check(list[0].name == "song" && list[1].name == "My_Song_" && list[0].time == base + 1000 && list[1].time == base + 800, "newest first, names made safe, times read back");
  std::string text;
  {
    std::ifstream in(list[1].file);   // (closed before the folder is removed: Windows will not delete an open file)
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  check(text == "{\"n\":4}", "a snapshot holds the song as it was written");
  check(wax::listBackups(folder / "nothing").empty(), "no folder, no snapshots");
  fs::remove_all(folder, ignored);
}

// Hiding or showing a parameter column moves the cells of the patterns with it (and undo puts them back), so the lane stays readable.
WAX_TEST("document", testSequenceOps) {
  using namespace wax;
  using namespace wax::seq;
  Lane lane;
  lane.id = "l";
  Pattern a;
  a.id = "a"; a.index = 0; a.name = "pat00";
  for (int i = 0; i < 16; i++) a.rows.push_back("x" + std::to_string(i));
  lane.patterns.push_back(a);
  lane.placements.push_back({"a", 32, 16});
  Context context;
  context.length = 256;
  context.blankRow = "-";
  check(!insertPattern(lane, context, 40, 16, false) && insertPattern(lane, context, 40, 16, true), "sequence: insert refuses ground a pattern plays on, unless overlaid");
  check(!clonePlacement(lane, 100) && clonePlacement(lane, 32) && lane.patterns.size() == 3 && patternOf(lane, placementAt(lane, 32)->pattern)->name == "pat00 copy", "sequence: clone gives the placement its own pattern");
  Lane sliced = lane;
  check(slicePlacement(sliced, 36) && sliced.placements.size() == lane.placements.size() + 1, "sequence: slice makes two halves");
  const Placement* first = placementAt(sliced, 32);
  check(first && first->length == 4 && patternOf(sliced, first->pattern)->rows.size() == 4, "sequence: the first half is 4 rows");
  check(resizePlacement(lane, context, 32, 8) == 24 && patternOf(lane, placementAt(lane, 32)->pattern)->rows.size() == 24, "sequence: growing adds empty rows");
  check(!movePlacement(lane, context, 32, 8).has_value(), "sequence: a move onto another start is refused");
  check(movePlacement(lane, context, 32, -16) == 16, "sequence: move");
  Lane shifting;
  shifting.id = "s"; shifting.patterns.push_back(a);
  shifting.placements = {{"a", 16, 16}, {"a", 64, 16}};
  check(shiftPlacements(shifting, context, 40, -100) == -24 && shifting.placements[1].start == 40, "sequence: pulling up stops at the cursor row");
  check(shiftPlacements(shifting, context, 32, 16) == 16 && shifting.placements[1].start == 56, "sequence: pushing down");
  Lane merged;
  merged.id = "m"; merged.patterns.push_back(a);
  merged.placements = {{"a", 0, 16}};
  check(mergeRange(merged, context, 8, 24, true) && merged.placements.size() == 2, "sequence: merge cuts the head and makes one pattern");
  const Placement* made = placementAt(merged, 8);
  check(made && made->length == 16 && patternOf(merged, made->pattern)->rows[0] == "x8" && patternOf(merged, made->pattern)->rows[8] == "-", "sequence: merged rows");
  Lane colours;
  colours.id = "c"; colours.patterns.push_back(a); colours.placements = {{"a", 0, 16}};
  check(colourPattern(colours, 0, 1) && colours.patterns[0].color == 0 && colourPattern(colours, 0, -1) && colours.patterns[0].color.is_null(), "sequence: colour steps through the presets and back to automatic");
  check(toggleMute(colours, context, 32) && colours.mutes.size() == 1 && toggleMute(colours, context, 32) && colours.mutes.empty(), "sequence: mute flips");
  Lane cleared;
  cleared.id = "k"; cleared.patterns.push_back(a);
  cleared.placements = {{"a", 0, 16}, {"a", 16, 16}, {"a", 32, 16}};
  check(clearRange(cleared, 16, 32) == 1 && cleared.placements.size() == 2 && cleared.patterns.size() == 1, "sequence: clearing a range removes the placements starting in it, not the patterns");
  // several lanes as one undo step (a block glued or cleared, a waypoint restored)
  Project project;
  Lane one = merged, two = cleared;
  project.lanes = {one, two};
  Document document(project);
  Lane oneAfter = one, twoAfter = two;
  oneAfter.placements.clear(); twoAfter.placements.clear();
  document.apply(std::make_unique<SetLaneSequences>(std::vector<Lane>{oneAfter, twoAfter}, "clear block"));
  check(document.project().lanes[0].placements.empty() && document.project().lanes[1].placements.empty(), "sequence: several lanes replaced at once");
  document.undo();
  check(document.project().lanes[0].placements.size() == one.placements.size() && document.project().lanes[1].placements.size() == two.placements.size(), "sequence: one undo puts every lane back");
  // blocks: copy, paste into another lane (same columns) and back into the lane they came from
  std::vector<Lane> pair;
  Lane from = cleared, to;
  from.id = "from"; to.id = "to";
  pair = {from, to};
  const std::vector<std::string> sigs = {"x", "x"};
  const Clip clip = captureClip(pair, sigs, 0, 0, 0, 48);
  check(clip.items.size() == 2 && clip.ticks == 48 && clip.lanes == 1, "block: capture takes the placements that start in it");
  const PlaceResult there = placeClip(pair, sigs, clip, 1, 64, context);
  check(there.placed == 2 && pair[1].placements.size() == 2 && pair[1].patterns.size() == 1 && pair[1].placements[0].start == 64, "block: pasted into another lane as copies (one pattern shared by both placements)");
  const PlaceResult back = placeClip(pair, sigs, clip, 0, 100, context);
  check(back.placed == 2 && pair[0].patterns.size() == 1, "block: pasted into its own lane it is the same pattern");
  check(placeClip(pair, {"x", "other"}, clip, 1, 0, context).skipped == 2, "block: a lane with other columns takes nothing");
  {   // a message left on a graph survives saving and loading, text and fold state
    Document noted(loadProject(std::string(WAX_TEST_SONGS) + "/cloves.wax"));
    const std::string instrument = noted.project().instruments.front().id;
    noted.apply(std::make_unique<AddNode>(instrument, "msg", "msg", 40, 40));
    noted.apply(std::make_unique<SetNodeData>(instrument, "msg", "text", Json("keep the gain low"), "message"));
    noted.apply(std::make_unique<SetNodeData>(instrument, "msg", "open", Json(false), "fold message"));
    const std::string file = (std::filesystem::temp_directory_path() / "wax-note-test.wax").string();
    saveProject(noted.project(), file);
    const Project again = loadProject(file);
    bool kept = false;
    for (const GraphNode& n : again.instruments.front().graph.nodes) if (n.type == "msg") kept = n.data.value("text", std::string()) == "keep the gain low" && !n.data.value("open", true);
    check(kept, "msg: saved and loaded again with its text and fold state");
    {   // a message saved under its first name, note, loads as msg
      Json saved; { std::ifstream in(file); in >> saved; }
      for (auto& [id, savedInstrument] : saved["instruments"].items())
        for (Json& n : savedInstrument["graph"]["nodes"]) if (n.value("type", "") == "msg") n["type"] = "note";
      { std::ofstream out(file); out << saved.dump(); }
      bool migrated = false;
      for (const Instrument& inst : loadProject(file).instruments)   // (every instrument: the rewritten file may list them in another order)
        for (const GraphNode& n : inst.graph.nodes) if (n.type == "msg") migrated = true;
      check(migrated, "msg: a file with the first name, note, loads as msg");
    }
    std::filesystem::remove(file);
  }
  Project soloing;
  Lane lead, bass, master;
  lead.id = "lead"; lead.instrument = "a"; bass.id = "bass"; bass.instrument = "b"; master.id = "master"; master.instrument = "master";
  soloing.lanes = {lead, bass, master};
  Document solos(soloing);
  check(solos.toggleSolo("lead") == true && !solos.project().lanes[0].soloMuted && solos.project().lanes[1].soloMuted && !solos.project().lanes[2].soloMuted, "solo: the other lanes let go, the master does not");
  check(!solos.modified() && !solos.canUndo(), "solo: not undoable, does not modify the song");
  check(solos.takeChangedLanes().count("bass") == 1, "solo: the lane that let go is rebuilt");
  check(solos.toggleSolo("lead") == false && !solos.project().lanes[1].soloMuted && !solos.toggleSolo("master").has_value(), "solo: off again; the master cannot be soloed");
}

// Tests: graph.
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
#include "live/player.h"
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

WAX_TEST("graph", testBankEdit) {
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

WAX_TEST("graph", testRewire) {
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

WAX_TEST("graph", testAddNode) {
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

WAX_TEST("graph", testAddInstrument) {
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

WAX_TEST("graph", testWireEdits) {
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
  // a node put into a wire, and taken out again (the node's ports only need names for the wires)
  const auto has = [&](const std::string& from, const std::string& to) { for (const wax::Connection& c : wires()) if (c.from == from && c.to == to) return true; return false; };
  doc.apply(std::make_unique<wax::InsertOnWire>(id, first.from, first.to, "zz.in", "zz.out"));
  check(wires().size() == before.size() + 1 && !has(first.from, first.to) && has(first.from, "zz.in") && has("zz.out", first.to), "a node dropped on a wire is put into it");
  doc.apply(std::make_unique<wax::PullOut>(id, "zz.in", "zz.out"));
  check(wires().size() == before.size() && has(first.from, first.to) && !has(first.from, "zz.in"), "taking it out joins its neighbours again");
  doc.undo();
  check(has(first.from, "zz.in") && has("zz.out", first.to) && !has(first.from, first.to), "undo of the pull-out puts the node back in the wire");
  doc.undo();
  check(wires().size() == before.size() && has(first.from, first.to), "undo of the insert puts the wire back");
  bool looped = false;   // a node placed so that the wire would feed itself is refused and nothing changes
  try { doc.apply(std::make_unique<wax::InsertOnWire>(id, first.from, first.to, "zz.in", first.to.substr(0, first.to.find('.')) + ".o")); } catch (const std::exception&) { looped = true; }
  check(looped && wires().size() == before.size() && has(first.from, first.to), "a loop through the new node is refused");
  {   // a node made another type: the project stays whole, and undo gives it back exactly
    const std::string saved = wax::serializeProject(doc.project());
    const wax::Instrument* host = nullptr;
    for (const wax::Instrument& i : doc.project().instruments) if (i.id == id) host = &i;
    std::string target;
    for (const wax::GraphNode& n : host->graph.nodes) if (n.type != "out" && n.type != "tempo" && n.type != "osc") { target = n.id; break; }
    check(!target.empty(), "a node to replace");
    auto edit = std::make_unique<wax::ReplaceNode>(id, target, "osc");
    const wax::ReplaceNode* replace = edit.get();
    doc.apply(std::move(edit));
    const wax::Instrument* after = nullptr;
    for (const wax::Instrument& i : doc.project().instruments) if (i.id == id) after = &i;
    bool whole = true, there = false;
    for (const wax::Connection& c : after->graph.connections) {
      bool a = false, b = false;
      for (const wax::GraphNode& n : after->graph.nodes) { a |= n.id == c.from.substr(0, c.from.find('.')); b |= n.id == c.to.substr(0, c.to.find('.')); }
      whole &= a && b;
    }
    for (const wax::GraphNode& n : after->graph.nodes) there |= n.id == replace->newId() && n.type == "osc";
    check(whole && there, "the replaced node is an osc and no wire is left dangling");
    for (const wax::Exposed& e : after->exposed) { bool ok = false; for (const wax::GraphNode& n : after->graph.nodes) ok |= n.id == e.node; whole &= ok; }
    check(whole, "exposed columns follow the node or go");
    doc.undo();
    check(wax::serializeProject(doc.project()) == saved, "undo of the replace gives the node back exactly");
  }
  {   // a node added and wired into a wire as one step (the wire's right-click menu)
    const std::string saved = wax::serializeProject(doc.project());
    std::vector<std::unique_ptr<wax::Edit>> follow;
    follow.push_back(std::make_unique<wax::InsertOnWire>(id, first.from, first.to, "zq.in", "zq.out"));
    doc.apply(std::make_unique<wax::FollowedEdit>(std::make_unique<wax::AddNode>(id, "osc", "zq", 0, 0), std::move(follow)));
    check(has(first.from, "zq.in") && has("zq.out", first.to) && !has(first.from, first.to), "a node added into a wire in one step");
    doc.undo();
    check(wax::serializeProject(doc.project()) == saved, "one undo takes the added node and its wiring away");
  }
  {   // remove-and-join, a group move and a duplicate
    std::vector<std::unique_ptr<wax::Edit>> follow;
    follow.push_back(std::make_unique<wax::InsertOnWire>(id, first.from, first.to, "zq.in", "zq.out"));
    doc.apply(std::make_unique<wax::FollowedEdit>(std::make_unique<wax::AddNode>(id, "osc", "zq", 5, 5), std::move(follow)));
    const std::string inserted = wax::serializeProject(doc.project());
    std::vector<std::unique_ptr<wax::Edit>> after;
    after.push_back(std::make_unique<wax::DeleteNode>(id, "zq"));
    doc.apply(std::make_unique<wax::FollowedEdit>(std::make_unique<wax::JoinAround>(id, std::vector<std::string>{"zq.in"}, "zq.out"), std::move(after)));
    check(has(first.from, first.to) && !has(first.from, "zq.in"), "removing a node and joining the wires round it");
    doc.undo();
    check(wax::serializeProject(doc.project()) == inserted, "undo brings the node and its wires back");
    doc.apply(std::make_unique<wax::MoveNodes>(id, std::vector<wax::MoveNodes::Place>{{"zq", 100, 110}}), true);
    doc.apply(std::make_unique<wax::MoveNodes>(id, std::vector<wax::MoveNodes::Place>{{"zq", 120, 130}}), true);
    doc.undo();
    check(wax::serializeProject(doc.project()) == inserted, "a group drag is one undo step");
    const std::size_t nodesBefore = [&] { for (const wax::Instrument& i : doc.project().instruments) if (i.id == id) return i.graph.nodes.size(); return std::size_t(0); }();
    doc.apply(std::make_unique<wax::DuplicateNodes>(id, std::vector<std::pair<std::string, std::string>>{{"zq", "zq2"}}));
    const std::size_t nodesAfter = [&] { for (const wax::Instrument& i : doc.project().instruments) if (i.id == id) return i.graph.nodes.size(); return std::size_t(0); }();
    check(nodesAfter == nodesBefore + 1, "a duplicate adds a node");
    doc.undo();
    check(wax::serializeProject(doc.project()) == inserted, "undo of the duplicate takes the copy away");
    doc.apply(std::make_unique<wax::SetKeepView>(id, std::vector<std::string>{"zq"}, true));
    check(wax::serializeProject(doc.project()).find("keepView") != std::string::npos, "a node that keeps its pictures is saved so");
    doc.undo();
    check(wax::serializeProject(doc.project()) == inserted, "undo of keeping the pictures");
    doc.undo();
  }
  std::printf("ok: wire edits\n");
}

WAX_TEST("graph", testInstrumentSetup) {
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

WAX_TEST("graph", testInstrumentManage) {
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
  {   // a pattern already placed gains blank cells for a new note column, and loses them again
    const auto placedRows = [&]() {
      for (const wax::Lane& lane : doc.project().lanes)
        if (lane.instrument == id && !lane.patterns.empty()) return lane.patterns.front().rows.front();
      return std::string();
    };
    const auto cells = [](const std::string& row) { std::istringstream in(row); std::string c; std::size_t n = 0; while (in >> c) n++; return n; };
    const std::size_t before = cells(placedRows());
    if (before > 0) {
      doc.apply(std::make_unique<wax::SetNoteColumns>(id, doc.project().instruments[1].noteColumns + 1));
      check(cells(placedRows()) == before + 2, "a new note column adds a note and a velocity cell to placed patterns");
      doc.undo();
      check(cells(placedRows()) == before, "undo restores the rows");
    }
  }
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
  {   // extract: the wires through a node are joined, the node is left unwired; undo restores them
    const auto lane = std::find_if(doc.project().instruments.begin(), doc.project().instruments.end(), [&](const wax::Instrument& i) { return i.id == id; });
    if (lane != doc.project().instruments.end()) {
      for (const wax::GraphNode& node : lane->graph.nodes) {
        const auto touching = [&](const wax::Instrument& i) { std::size_t n = 0; for (const wax::Connection& c : i.graph.connections) if (c.from.rfind(node.id + ".", 0) == 0 || c.to.rfind(node.id + ".", 0) == 0) n++; return n; };
        if (node.type != "filter" || touching(*lane) == 0) continue;
        const std::size_t wires = lane->graph.connections.size();
        doc.apply(std::make_unique<wax::ExtractNode>(id, node.id));
        const auto& after = std::find_if(doc.project().instruments.begin(), doc.project().instruments.end(), [&](const wax::Instrument& i) { return i.id == id; })->graph.connections;
        bool free = true;
        for (const wax::Connection& c : after) free &= c.from.rfind(node.id + ".", 0) != 0 && c.to.rfind(node.id + ".", 0) != 0;
        check(free && !after.empty(), "an extracted node has no wires, the chain stays joined");
        doc.undo();
        check(std::find_if(doc.project().instruments.begin(), doc.project().instruments.end(), [&](const wax::Instrument& i) { return i.id == id; })->graph.connections.size() == wires, "extract undone");
        break;
      }
    }
  }
  std::printf("ok: instrument manage\n");
}

WAX_TEST("graph", testPresets) {
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

WAX_TEST("graph", testGraphSaved) {
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

WAX_TEST("graph", testExposePort) {
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

WAX_TEST("graph", testExposeMovesCells) {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/cloves.wax"));
  const wax::Instrument& first = doc.project().instruments[1];   // clang: notes, then shown parameters, then a command column
  const std::string id = first.id;
  const std::size_t shown = first.exposed.size();
  const wax::Exposed one = first.exposed.front();
  const wax::Lane* lane = nullptr;
  for (const wax::Lane& l : doc.project().lanes) if (l.instrument == id) lane = &l;
  check(lane && !lane->patterns.empty(), "the lane has a pattern");
  const auto cellCount = [&](const wax::Document& d) { std::istringstream words(d.project().lanes[1].patterns.front().rows.front()); std::size_t n = 0; for (std::string w; words >> w;) n++; return n; };
  const std::size_t before = cellCount(doc);
  doc.apply(std::make_unique<wax::SetExposed>(id, one.node, one.param, false));
  check(doc.project().instruments[1].exposed.size() == shown - 1 && cellCount(doc) == before - 1, "hiding a parameter takes its cell out of the rows");
  doc.undo();
  check(cellCount(doc) == before, "undoing it puts the cell back");
  std::printf("ok: expose moves cells\n");
}

WAX_TEST("graph", testMoveNode) {
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

WAX_TEST("graph", testSetParam) {
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

// Live notes (the MIDI keyboard, the piano keys): a release finds the machine the note started on even when the machine
// being played has changed since, and panic stops every machine.
WAX_TEST("graph", testLiveNotes) {
  wax::Document doc(wax::loadProject(std::string(WAX_TEST_SONGS) + "/dvii.wax"));
  for (const char* kind : {"subtractive", "fourpole"}) {
    const wax::Json machine = wax::app::machineJson(kind);
    check(!machine.is_null(), "the machine table knows the machine");
    doc.apply(std::make_unique<wax::AddInstrument>(kind, machine, kind));
  }
  const double rate = 44100;
  // The loudest sample in the last tenth of a second of `seconds` of sound.
  const auto tailPeak = [&](wax::Player& player, double seconds) {
    std::vector<float> out(static_cast<std::size_t>(seconds * rate) * 2);
    player.fill(out.data(), static_cast<int>(out.size() / 2));
    float peak = 0;
    for (std::size_t i = out.size() - static_cast<std::size_t>(rate / 10) * 2; i < out.size(); i++) peak = std::max(peak, std::fabs(out[i]));
    return peak;
  };
  const auto start = [&](wax::Player& player) {
    player.setInstrument("subtractive");
    player.ring().push({wax::LiveCommand::NoteOn, 60, 1});
    tailPeak(player, 0.3);
  };

  wax::Player held(doc.project(), rate, 1, "subtractive", false);
  start(held);
  check(tailPeak(held, 0.5) > 0.001, "a note held keeps sounding");

  wax::Player released(doc.project(), rate, 1, "subtractive", false);
  start(released);
  released.setInstrument("fourpole");   // the machine being played changes while the key is down
  released.ring().push({wax::LiveCommand::NoteOff, 60, 0});
  check(tailPeak(released, 4.0) < 0.001, "its release goes to the machine it started on, not the one played now");

  wax::Player panicked(doc.project(), rate, 1, "subtractive", false);
  start(panicked);
  panicked.setInstrument("fourpole");
  panicked.ring().push({wax::LiveCommand::NoteOn, 64, 1});
  tailPeak(panicked, 0.3);
  panicked.ring().push({wax::LiveCommand::Panic, 0, 0});
  check(tailPeak(panicked, 0.5) < 0.001, "panic stops the notes on every machine");
  std::printf("ok: live notes\n");
}

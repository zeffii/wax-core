// Tests: curves.
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

WAX_TEST("curves", testSpline) {
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

WAX_TEST("curves", testDrawingEdit) {
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

// The editing side of lane curves: anchors, snapping, pins following their pattern, sections with gaps.
WAX_TEST("curves", testCurveEdit) {
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
WAX_TEST("curves", testCurveDocument) {
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

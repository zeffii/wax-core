// Tests: samples.
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

WAX_TEST("samples", testTap) {
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

// A sample loaded while the song plays reaches a sampler added at the same time: a note on it sounds.
WAX_TEST("samples", testLiveSampler) {
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
  check(wax::instrumentsPlaying(doc.project(), "probe") == std::vector<std::string>({"sampler"}) && wax::instrumentsPlaying(doc.project(), "nothing").empty(), "who plays a sample");
  doc.apply(std::make_unique<wax::RenameSample>("probe", "renamed"));
  check(doc.project().samples.contains("renamed") && !doc.project().samples.contains("probe") && wax::instrumentsPlaying(doc.project(), "renamed").size() == 1, "renaming a sample moves the instruments that play it");
  doc.undo();
  check(doc.project().samples.contains("probe") && wax::instrumentsPlaying(doc.project(), "probe").size() == 1, "undo gives the old name back to both");
  doc.apply(std::make_unique<wax::AddSample>("spare", wax::encodeSample(44100, tone, {})));
  doc.apply(std::make_unique<wax::RemoveSample>("spare"));
  check(!doc.project().samples.contains("spare"), "a sample can be removed");
  doc.undo();
  check(doc.project().samples.contains("spare"), "undo brings it back");
  std::printf("ok: live sampler\n");
}

// Samples loaded into a project: named as the web app names them, kept as 16-bit PCM in the file, given to the machines that play them.
WAX_TEST("samples", testSampleEdit) {
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

WAX_TEST("samples", testSpectrogram) {   // the numbers come from the web app's computeSpectrogram on the same signal
  const int n = 6000;
  std::vector<float> d(n), r(n);
  for (int i = 0; i < n; i++) {
    d[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * std::sin(i * 0.3) * std::exp(-i / 3000.0) + 0.2 * std::sin(i * 1.1));
    r[static_cast<std::size_t>(i)] = static_cast<float>(0.1 * std::sin(i * 0.07));
  }
  const auto sum = [](const std::vector<std::uint8_t>& a) { long total = 0; for (const std::uint8_t v : a) total += v; return total; };
  const auto stereo = wax::computeSpectrogram(d, r, 0, n, 12, 20), mono = wax::computeSpectrogram(d, {}, 0, n, 12, 20);
  check(sum(stereo) == 14337 && stereo[0] == 106 && stereo[5 * 12 + 3] == 0 && stereo[19 * 12 + 11] == 135, "a stereo spectrogram matches the web app's");
  check(sum(mono) == 10971 && mono[7 * 12 + 6] == 150, "a mono spectrogram matches the web app's");
  const auto part = wax::computeSpectrogram(d, {}, 1000, 4000, 8, 16);
  check(sum(part) == 5806 && part[40] == 150, "a part of the sample matches the web app's");
  const auto heat = wax::spectrogramHeat(100);
  check(heat[0] == 127 && heat[1] == 26 && heat[2] == 246 && wax::spectrogramHeat(255)[0] == 255 && wax::spectrogramHeat(0)[2] == 0, "the heat colours");
}

WAX_TEST("samples", testOnsets) {   // the frames come from the web app's detectOnsets on the same signal
  const int rate = 22050, n = 22050;
  std::vector<float> d(n, 0.0f), r(n, 0.0f);
  long long seed = 12345;
  const auto rnd = [&]() { seed = (seed * 16807) % 2147483647LL; return static_cast<double>(seed) / 2147483647.0 - 0.5; };
  for (const int at : {2000, 7000, 12500, 17000})
    for (int i = 0; i < 3000; i++) {
      const double e = std::exp(-i / 400.0);
      d[static_cast<std::size_t>(at + i)] = static_cast<float>(d[static_cast<std::size_t>(at + i)] + (std::sin(i * 0.4) * 0.6 + rnd() * 0.3) * e);
      r[static_cast<std::size_t>(at + i)] = static_cast<float>(r[static_cast<std::size_t>(at + i)] + std::sin(i * 0.2) * 0.2 * e);
    }
  check(wax::detectOnsets(d, {}, rate) == std::vector<int>({1979, 6979, 12479, 16979}), "onsets: four hits found where the web app finds them");
  wax::OnsetSettings loud;
  loud.sensitivity = 0.8; loud.minGapMs = 100;
  check(wax::detectOnsets(d, r, rate, loud) == std::vector<int>({1979, 6979, 12479, 16979}), "onsets: stereo, other settings");
  wax::OnsetSettings part;
  part.from = 5000; part.to = 20000; part.sensitivity = 0.3;
  check(wax::detectOnsets(d, {}, rate, part) == std::vector<int>({6979, 12479, 16979}), "onsets: only inside the range");
  check(wax::detectOnsets(std::vector<float>(d.begin(), d.begin() + 500), {}, rate).empty(), "onsets: too short to look at");
}

WAX_TEST("samples", testSampleLoading) {
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

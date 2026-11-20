#include "app/xm_import.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>

#include "app/machines.h"
#include "app/module_util.h"
#include "core/js_math.h"
#include "project/samples.h"

namespace wax::app {
namespace {

constexpr int kKeyOff = 97;
constexpr int kEffectG = 0x10, kEffectH = 0x11;   // the global volume and its slide

// An envelope from its 12 points (x, y words), the count and the sustain / loop points; false when it is off.
bool readEnvelope(const Reader& r, std::size_t at, int count, int sus, int ls, int le, int type, XmEnvelope& out) {
  if (!(type & 1) || count < 1) return false;
  const int n = std::min(12, count);
  for (int i = 0; i < n; i++) {
    out.pts.push_back(r.u16(at + static_cast<std::size_t>(i) * 4));
    out.pts.push_back(std::min(64, r.u16(at + static_cast<std::size_t>(i) * 4 + 2)));
  }
  const auto inside = [&](int k) { return k < n ? k : -1; };
  out.sus = (type & 2) ? inside(sus) : -1;
  out.ls = (type & 4) ? inside(ls) : -1;
  out.le = (type & 4) ? inside(le) : -1;
  return true;
}

// Delta-coded sample data as floats in -1..1.
std::vector<float> decodeSample(const Reader& r, std::size_t at, std::size_t bytes, bool sixteen) {
  std::vector<float> out;
  if (sixteen) {
    const std::size_t n = bytes >> 1;
    out.resize(n);
    int old = 0;
    for (std::size_t i = 0; i < n; i++) {
      old = (old + r.at(at)[i * 2] + (r.at(at)[i * 2 + 1] << 8)) & 0xffff;
      out[i] = static_cast<float>((old > 32767 ? old - 65536 : old) / 32768.0);
    }
    return out;
  }
  out.resize(bytes);
  int old = 0;
  for (std::size_t i = 0; i < bytes; i++) {
    old = (old + r.at(at)[i]) & 0xff;
    out[i] = static_cast<float>((old > 127 ? old - 256 : old) / 128.0);
  }
  return out;
}

// The rate a sample is played at on C-4: its relative note and finetune folded in (8363 Hz is untransposed).
int sampleRateOf(const XmSample& s) {
  const double rate = jsRound(8363 * js::pow(2, (s.relNote + s.finetune / 128.0) / 12));
  return static_cast<int>(std::max(1000.0, std::min(384000.0, rate)));
}

Json envelopeJson(const XmEnvelope& e) {
  return {{"pts", e.pts}, {"sus", e.sus}, {"ls", e.ls}, {"le", e.le}};
}

// What the cell says in the pattern text of an xmchan lane: note, instrument minus one, volume column, effect word.
std::string cellText(const XmCell& cell) {
  std::string note = "...";
  if (cell.note == kKeyOff) note = "===";
  else if (cell.note >= 1 && cell.note <= 96) note = noteToken(cell.note - 1);
  const std::string ins = cell.ins >= 1 && cell.ins <= 128 ? hex(cell.ins - 1, 2) : "..";
  const std::string vol = cell.vol >= 0x10 ? hex(cell.vol, 2) : "..";
  const int effect = cell.effect <= 0x23 ? cell.effect : 0;
  return note + " " + ins + " " + vol + " 01" + hex((effect << 8) | cell.param, 4);
}

}  // namespace

XmModule parseXm(const std::vector<std::uint8_t>& bytes) {
  const Reader r(bytes, "XM file");
  if (r.length() < 80 || r.text(0, 17) != "Extended Module:") throw std::runtime_error("Not an XM file.");
  if (r.u16(58) < 0x0104) throw std::runtime_error("This XM file is from an old FastTracker (before 2.04); its layout is not supported.");
  const std::size_t headerSize = r.u32(60);
  const int songLength = std::max(1, std::min(256, r.u16(64)));
  XmModule xm;
  xm.restart = r.u16(66);
  xm.channels = r.u16(68);
  const int patternCount = r.u16(70), instrumentCount = r.u16(72), flags = r.u16(74);
  if (xm.channels < 1 || xm.channels > 64) throw std::runtime_error("That XM file has no channels.");
  for (int i = 0; i < songLength && 80 + static_cast<std::size_t>(i) < r.length(); i++) xm.orders.push_back(r.u8(80 + static_cast<std::size_t>(i)));
  std::size_t at = 60 + headerSize;

  for (int p = 0; p < patternCount; p++) {
    const std::size_t len = r.u32(at);
    const int rowsCount = r.u16(at + 5), packed = r.u16(at + 7);
    at += len;
    std::vector<std::vector<XmCell>> rows(static_cast<std::size_t>(rowsCount ? rowsCount : 64), std::vector<XmCell>(static_cast<std::size_t>(xm.channels)));
    std::size_t q = at;
    const std::size_t end = at + static_cast<std::size_t>(packed);
    for (std::size_t row = 0; packed && row < rows.size(); row++) {
      for (std::size_t c = 0; c < static_cast<std::size_t>(xm.channels) && q < end; c++) {
        XmCell& cell = rows[row][c];
        const int first = r.u8(q++);
        if (first & 0x80) {
          if (first & 1) cell.note = r.u8(q++);
          if (first & 2) cell.ins = r.u8(q++);
          if (first & 4) cell.vol = r.u8(q++);
          if (first & 8) cell.effect = r.u8(q++);
          if (first & 16) cell.param = r.u8(q++);
        } else {
          cell.note = first; cell.ins = r.u8(q++); cell.vol = r.u8(q++); cell.effect = r.u8(q++); cell.param = r.u8(q++);
        }
      }
    }
    at = end;
    xm.patterns.push_back(std::move(rows));
  }

  for (int i = 0; i < instrumentCount && at + 29 <= r.length(); i++) {
    const std::size_t size = r.u32(at);
    const int count = r.u16(at + 27);
    XmInstrument inst;
    inst.name = r.text(at + 4, at + 26);
    if (count == 0) { xm.instruments.push_back(std::move(inst)); at += size; continue; }
    const std::size_t sampleHeader = r.u32(at + 29);
    for (std::size_t k = 0; k < 96; k++) inst.map[k] = r.u8(at + 33 + k);
    inst.hasVol = readEnvelope(r, at + 129, r.u8(at + 225), r.u8(at + 227), r.u8(at + 228), r.u8(at + 229), r.u8(at + 233), inst.vol);
    inst.hasPan = readEnvelope(r, at + 177, r.u8(at + 226), r.u8(at + 230), r.u8(at + 231), r.u8(at + 232), r.u8(at + 234), inst.pan);
    for (std::size_t k = 0; k < 4; k++) inst.vib[k] = r.u8(at + 235 + k);
    inst.fade = r.u16(at + 239);
    at += size;
    struct Head { std::size_t bytes = 0; XmSample s; int loopStart = 0, loopLength = 0; bool sixteen = false; };
    std::vector<Head> heads;
    for (int k = 0; k < count; k++, at += sampleHeader) {
      Head h;
      const int type = r.u8(at + 14);
      h.sixteen = (type & 16) != 0;
      const std::uint32_t unit = h.sixteen ? 2 : 1;
      h.bytes = r.u32(at);
      h.loopStart = static_cast<int>(r.u32(at + 4) / unit);
      h.loopLength = static_cast<int>(r.u32(at + 8) / unit);
      h.s.volume = std::min(64, r.u8(at + 12));
      h.s.finetune = r.s8(at + 13);
      h.s.loopType = type & 3;
      h.s.pan = r.u8(at + 15);
      h.s.relNote = r.s8(at + 16);
      h.s.name = r.text(at + 18, at + 40);
      heads.push_back(std::move(h));
    }
    for (Head& h : heads) {
      const std::size_t room = at < r.length() ? r.length() - at : 0;
      const std::size_t have = std::min(h.bytes, room);
      h.s.data = decodeSample(r, at, have, h.sixteen);
      at += h.bytes;
      const bool loops = h.s.loopType != 0 && h.loopLength > 1 && h.loopStart + h.loopLength <= static_cast<int>(h.s.data.size());
      if (!loops) h.s.loopType = 0;
      h.s.loopStart = loops ? h.loopStart : 0;
      h.s.loopEnd = loops ? h.loopStart + h.loopLength : 0;
      inst.samples.push_back(std::move(h.s));
    }
    xm.instruments.push_back(std::move(inst));
  }
  xm.title = r.text(17, 37);
  xm.linear = (flags & 1) != 0;
  xm.speed = r.u16(76) ? r.u16(76) : 6;
  xm.bpm = r.u16(78) ? r.u16(78) : 125;
  return xm;
}

std::vector<XmStep> playOut(const XmModule& xm, int maxRows) {
  std::vector<XmStep> steps;
  int speed = xm.speed, gv = 64, memH = 0;
  std::size_t pos = 0;
  int row = 0;
  std::set<std::pair<std::size_t, int>> seen;
  std::vector<int> loopRow(static_cast<std::size_t>(xm.channels), 0), loopCount(static_cast<std::size_t>(xm.channels), 0);
  while (pos < xm.orders.size() && static_cast<int>(steps.size()) < maxRows) {
    const std::size_t pattern = static_cast<std::size_t>(xm.orders[pos]);
    if (pattern >= xm.patterns.size()) { pos++; row = 0; continue; }   // an order past the patterns plays nothing
    const auto& rows = xm.patterns[pattern];
    if (row >= static_cast<int>(rows.size())) { pos++; row = 0; continue; }
    const bool looping = std::any_of(loopCount.begin(), loopCount.end(), [](int c) { return c > 0; });
    if (seen.count({pos, row}) && !looping) break;
    seen.insert({pos, row});
    std::optional<int> jump, brk, loop;
    int delay = 0, tempoHere = 0, gslide = 0;
    bool stop = false;
    for (std::size_t c = 0; c < rows[static_cast<std::size_t>(row)].size(); c++) {
      const XmCell& cell = rows[static_cast<std::size_t>(row)][c];
      const int e = cell.effect, p = cell.param, x = p & 15;
      if (e == 0xF) { if (p == 0) stop = true; else if (p < 32) speed = p; else tempoHere = p; }
      else if (e == 0xB) jump = p;
      else if (e == 0xD) brk = (p >> 4) * 10 + x;
      else if (e == 0xE && (p >> 4) == 6) {
        if (x == 0) loopRow[c] = row;
        else {
          if (loopCount[c] == 0) loopCount[c] = x; else loopCount[c]--;
          if (loopCount[c] > 0) loop = loopRow[c];
        }
      } else if (e == 0xE && (p >> 4) == 0xE) delay = x;
      else if (e == kEffectG) gv = std::min(64, p);
      else if (e == kEffectH) { if (p) memH = p; gslide = (memH >> 4) ? memH >> 4 : -(memH & 15); }
    }
    XmStep step;
    step.pattern = static_cast<int>(pattern); step.row = row; step.speed = speed; step.delay = delay; step.gv = gv; step.gslide = gslide; step.tempo = tempoHere;
    steps.push_back(step);
    const int later = speed * (1 + delay) - 1;
    gv = std::max(0, std::min(64, gv + gslide * later));
    if (stop) break;
    if (!jump && !brk && loop) { row = *loop; continue; }
    if (jump || brk) {
      pos = jump ? static_cast<std::size_t>(*jump) : pos + 1;
      const bool known = pos < xm.orders.size() && static_cast<std::size_t>(xm.orders[pos]) < xm.patterns.size();
      row = brk && known && *brk < static_cast<int>(xm.patterns[static_cast<std::size_t>(xm.orders[pos])].size()) ? *brk : 0;
    } else if (++row >= static_cast<int>(rows.size())) { pos++; row = 0; }
  }
  return steps;
}

Json xmToProjectFile(const XmModule& xm, const std::string& name) {
  const std::vector<XmStep> steps = playOut(xm);
  if (steps.empty()) throw std::runtime_error("That module has no rows to play.");

  // the samples: every sample of every instrument gets a slot, and each instrument's keymap points at its slots
  Json samples = Json::object(), ins = Json::array();
  std::vector<std::string> slots;
  for (std::size_t i = 0; i < xm.instruments.size(); i++) {
    const XmInstrument& inst = xm.instruments[i];
    const int first = static_cast<int>(slots.size());
    for (std::size_t k = 0; k < inst.samples.size(); k++) {
      const XmSample& s = inst.samples[k];
      const std::string label = hex(static_cast<int>(i) + 1, 2) + "_" + std::to_string(k) + "_" + (!s.name.empty() ? s.name : !inst.name.empty() ? inst.name : "sample");
      const std::string id = uniqueSampleId(samples, sampleIdFromName(label));
      Json entry = encodeSample(sampleRateOf(s), s.data.empty() ? std::vector<float>(2, 0.0f) : s.data, {});
      entry["volume"] = s.volume / 64.0;
      if (s.pan != 128) entry["pan"] = std::max(-1.0, std::min(1.0, (s.pan - 128) / 128.0));
      if (s.loopType) {
        entry["loopStart"] = s.loopStart;
        entry["loopEnd"] = s.loopEnd;
        entry["loopOn"] = true;
        if (s.loopType == 2) entry["pingpong"] = true;
      }
      samples[id] = entry;
      slots.push_back(id);
    }
    if (inst.samples.empty()) { ins.push_back(nullptr); continue; }
    std::vector<int> map;
    for (const int k : inst.map) map.push_back(k < static_cast<int>(inst.samples.size()) ? first + k : -1);
    Json def = Json::object();
    if (std::all_of(map.begin(), map.end(), [&](int m) { return m == map[0]; })) def["map"] = map[0]; else def["map"] = map;
    def["fade"] = inst.fade;
    if (inst.vib[0] || inst.vib[1] || inst.vib[2] || inst.vib[3]) def["vib"] = std::vector<int>(inst.vib, inst.vib + 4);
    if (inst.hasVol) def["vol"] = envelopeJson(inst.vol);
    if (inst.hasPan) def["pan"] = envelopeJson(inst.pan);
    ins.push_back(def);
  }
  if (slots.empty()) throw std::runtime_error("That XM file has no samples.");

  // Rows that go together become one pattern: consecutive rows of one XM pattern at one speed. A row with a pattern delay stands alone.
  struct Run { int pattern, start, count, speed, delay; };
  std::vector<Run> runs;
  for (const XmStep& s : steps) {
    if (!runs.empty()) {
      Run& last = runs.back();
      if (!s.delay && !last.delay && last.pattern == s.pattern && last.speed == s.speed && last.start + last.count == s.row) { last.count++; continue; }
    }
    runs.push_back({s.pattern, s.row, 1, s.speed, s.delay});
  }
  int total = 0;
  for (const XmStep& s : steps) total += s.speed * (1 + s.delay);
  const auto puidOf = [&](const Run& r) {
    std::string id = "p" + hex(r.pattern, 2);
    if (r.start || r.count != static_cast<int>(xm.patterns[static_cast<std::size_t>(r.pattern)].size())) id += "-r" + std::to_string(r.start) + "-" + std::to_string(r.start + r.count - 1);
    if (r.speed != 6) id += "-s" + std::to_string(r.speed);
    if (r.delay) id += "-d" + std::to_string(r.delay);
    return id;
  };

  Json instruments = Json::object(), lanes = Json::array();
  std::vector<Json> laneList;
  for (int c = 0; c < xm.channels; c++) {
    Json def = machineJson("xmchan", slots);
    for (Json& node : def["graph"]["nodes"]) if (node.value("id", std::string()) == "ch") node["data"] = {{"linear", xm.linear}, {"ins", ins}};
    const std::string id = "ch" + std::to_string(c + 1);
    instruments[id] = def;
    laneList.push_back({{"id", id}, {"instrument", id}, {"patterns", Json::object()}, {"placements", Json::array()}});
  }
  int tick = 0;
  for (const Run& run : runs) {
    const std::string puid = puidOf(run);
    const int rowTicks = run.speed * (1 + run.delay);
    for (int c = 0; c < xm.channels; c++) {
      Json& lane = laneList[static_cast<std::size_t>(c)];
      if (!lane["patterns"].contains(puid)) {
        Json rows = Json::array();
        for (int r = run.start; r < run.start + run.count; r++) {
          rows.push_back(cellText(xm.patterns[static_cast<std::size_t>(run.pattern)][static_cast<std::size_t>(r)][static_cast<std::size_t>(c)]));
          for (int t = 1; t < rowTicks; t++) rows.push_back("... .. .. ......");
        }
        const int index = static_cast<int>(lane["patterns"].size());
        lane["patterns"][puid] = {{"name", puid}, {"index", index}, {"rows", rows}};
      }
      lane["placements"].push_back({{"puid", puid}, {"start", tick}, {"length", run.count * rowTicks}});
    }
    tick += run.count * rowTicks;
  }

  // The master lane: tempo changes in the BPM column on the tick the row starts, and the global volume in the Vol column on every tick it changes.
  Json master = {{"id", "master"}, {"instrument", "master"}, {"patterns", Json::object()}, {"placements", Json::array()}};
  std::vector<std::string> bpmCells(static_cast<std::size_t>(total), "...."), volCells(static_cast<std::size_t>(total), "..");
  int at = 0, gvNow = 64;
  bool any = false;
  for (const XmStep& s : steps) {
    if (s.tempo) { bpmCells[static_cast<std::size_t>(at)] = hex(s.tempo, 4); any = true; }
    const int ticks = s.speed * (1 + s.delay);
    for (int t = 0; t < ticks; t++) {
      const int gv = std::max(0, std::min(64, s.gv + s.gslide * t));
      if (gv != gvNow) { volCells[static_cast<std::size_t>(at + t)] = hex(jsRoundInt(gv / 64.0 * 255), 2); gvNow = gv; any = true; }
    }
    at += ticks;
  }
  if (any) {
    Json rows = Json::array();
    for (int t = 0; t < total; t++) rows.push_back(bpmCells[static_cast<std::size_t>(t)] + " .. .. " + volCells[static_cast<std::size_t>(t)]);
    master["patterns"]["song"] = {{"name", "song"}, {"index", 0}, {"rows", rows}};
    master["placements"].push_back({{"puid", "song"}, {"start", 0}, {"length", total}});
  }
  lanes.push_back(master);
  for (const Json& lane : laneList) lanes.push_back(lane);

  const std::string title = cleanName(xm.title);
  Json file = Json::object();
  file["format"] = "webtracker";
  file["version"] = 1;
  file["song"] = {{"name", !title.empty() ? title : name}, {"bpm", xm.bpm}, {"ticksPerBeat", kXmTicksPerBeat}, {"length", std::max(16, (total + 15) / 16 * 16)}};
  file["instruments"] = instruments;
  file["samples"] = samples;
  file["lanes"] = lanes;
  return file;
}

Project projectFromXmFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("The file could not be read.");
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string name = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
  if (name.size() > 3 && (name.substr(name.size() - 3) == ".xm" || name.substr(name.size() - 3) == ".XM")) name.resize(name.size() - 3);
  return parseProject(xmToProjectFile(parseXm(bytes), name).dump());
}

}  // namespace wax::app

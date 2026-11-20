#include "app/it_import.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>

#include "app/machines.h"
#include "app/module_util.h"
#include "project/samples.h"

namespace wax::app {
namespace {

constexpr int kNoteCut = 254, kNoteOff = 255, kTopNote = 115;   // G-9, the highest note a Wax cell holds
// The effect numbers the arrangement needs (A is 1 ... Z is 26).
constexpr int kA = 1, kB = 2, kC = 3, kS = 19, kT = 20, kV = 22, kW = 23;

struct SampleHead {
  std::uint32_t pointer = 0, length = 0;
  bool sixteen = false, stereo = false, compressed = false, isSigned = false, delta = false;
};

// One channel's frames as floats in -1..1, from `at` on; `next` is where the following data starts.
std::vector<float> readSampleChannel(const Reader& r, const SampleHead& h, std::size_t at, std::size_t& next) {
  std::vector<float> out(h.length, 0.0f);
  const double scale = h.sixteen ? 32768.0 : 128.0;
  if (h.compressed) {
    const std::vector<int> values = decompressIt(r.bytes(), at, h.length, h.sixteen, h.delta);
    // the end of the blocks is found again by reading their lengths
    std::size_t q = at, done = 0;
    const std::size_t blockMax = h.sixteen ? 0x4000 : 0x8000;
    while (done < h.length && q + 2 <= r.length()) {
      q = std::min(r.length(), q + 2 + static_cast<std::size_t>(r.at(q)[0] | (r.at(q)[1] << 8)));
      done += std::min(blockMax, static_cast<std::size_t>(h.length) - done);
    }
    next = q;
    for (std::size_t i = 0; i < h.length; i++) {
      const int v = h.isSigned ? values[i] : values[i] ^ (h.sixteen ? -32768 : -128);
      out[i] = static_cast<float>(v / scale);
    }
    return out;
  }
  const std::size_t unit = h.sixteen ? 2 : 1;
  const std::size_t room = at < r.length() ? (r.length() - at) / unit : 0;
  const std::size_t have = std::min<std::size_t>(h.length, room);
  std::int64_t sum = 0;
  for (std::size_t i = 0; i < have; i++) {
    int v = h.sixteen ? r.at(at)[i * 2] | (r.at(at)[i * 2 + 1] << 8) : r.at(at)[i];
    if (h.sixteen && v > 32767) v -= 65536; else if (!h.sixteen && v > 127) v -= 256;
    if (h.delta) { sum += v; v = h.sixteen ? static_cast<std::int16_t>(sum) : static_cast<std::int8_t>(sum); }
    else if (!h.isSigned) v = h.sixteen ? v ^ -32768 : v ^ -128;
    out[i] = static_cast<float>(v / scale);
  }
  next = at + static_cast<std::size_t>(h.length) * unit;
  return out;
}

// An envelope from its 82 bytes: flags, node count, loop and sustain points, 25 (value, tick) nodes. Panning and pitch are stored
// -32..32 and moved up to 0..64.
ItEnvelope readEnvelope(const Reader& r, std::size_t at, bool centred, bool& filter) {
  ItEnvelope env;
  const int flags = r.u8(at), count = std::min(25, r.u8(at + 1));
  filter = (flags & 0x80) != 0;
  if (!(flags & 1) || count < 1) return env;
  env.on = true;
  for (int i = 0; i < count; i++) {
    const int y = (centred ? r.s8(at + 6 + static_cast<std::size_t>(i) * 3) + 32 : r.s8(at + 6 + static_cast<std::size_t>(i) * 3));
    env.pts.push_back(r.u16(at + 7 + static_cast<std::size_t>(i) * 3));
    env.pts.push_back(std::max(0, std::min(64, y)));
  }
  const auto inside = [&](int k) { return k < count ? k : -1; };
  env.ls = (flags & 2) ? inside(r.u8(at + 2)) : -1;
  env.le = (flags & 2) ? inside(r.u8(at + 3)) : -1;
  env.ss = (flags & 4) ? inside(r.u8(at + 4)) : -1;
  env.se = (flags & 4) ? inside(r.u8(at + 5)) : -1;
  return env;
}

Json envelopeJson(const ItEnvelope& e) {
  if (!e.on) return nullptr;
  return {{"pts", e.pts}, {"ls", e.ls}, {"le", e.le}, {"ss", e.ss}, {"se", e.se}};
}

// What the cell says in the pattern text of an itchan lane: note, instrument minus one, volume column byte, effect word. The Cmd cell
// is on every row: it is what starts the row on the machine.
std::string cellText(const ItCell& cell) {
  std::string note = "...";
  if (cell.note == kNoteOff) note = "===";
  else if (cell.note == kNoteCut) note = "^^^";
  else if (cell.note >= 0 && cell.note <= kTopNote) note = noteToken(cell.note);
  const std::string ins = cell.ins >= 1 ? hex(cell.ins - 1, 2) : "..";
  const std::string vol = cell.vol >= 0 ? hex(cell.vol, 2) : "..";
  const int word = cell.effect >= 1 && cell.effect <= 26 ? (cell.effect << 8) | cell.param : 0;
  return note + " " + ins + " " + vol + " 01" + hex(word, 4);
}

bool isEmptyCell(const ItCell& c) { return c.note < 0 && !c.ins && c.vol < 0 && !c.effect; }

}  // namespace

std::vector<int> decompressIt(const std::vector<std::uint8_t>& b, std::size_t at, std::size_t count, bool sixteen, bool second) {
  std::vector<int> out(count, 0);
  const std::size_t blockMax = sixteen ? 0x4000 : 0x8000;
  const int start = sixteen ? 17 : 9, bits = sixteen ? 16 : 8;
  std::size_t done = 0;
  while (done < count && at + 2 <= b.size()) {
    const std::size_t size = static_cast<std::size_t>(b[at] | (b[at + 1] << 8));
    at += 2;
    const std::size_t end = std::min(b.size(), at + size);
    unsigned bitBuffer = 0;
    int bitCount = 0;
    std::size_t q = at;
    const auto read = [&](int n) {
      std::uint32_t value = 0;
      for (int got = 0; got < n;) {
        if (bitCount == 0) { bitBuffer = q < end ? b[q++] : 0u; bitCount = 8; }
        const int take = std::min(n - got, bitCount);
        value |= (bitBuffer & ((1u << take) - 1)) << got;
        bitBuffer >>= take; bitCount -= take; got += take;
      }
      return value;
    };
    const std::size_t length = std::min(blockMax, count - done);
    int width = start;
    std::int64_t d1 = 0, d2 = 0;
    std::size_t n = 0;
    while (n < length) {
      if (width > start) break;   // a corrupt block
      std::uint32_t value = read(width);
      if (width < 7) {
        if (value == (1u << (width - 1))) { const int v = static_cast<int>(read(sixteen ? 4 : 3)) + 1; width = v < width ? v : v + 1; continue; }
      } else if (width < start) {
        const std::uint32_t border = ((sixteen ? 0xffffu : 0xffu) >> (start - width)) - (sixteen ? 8 : 4), span = sixteen ? 16 : 8;
        if (value > border && value <= border + span) { value -= border; width = static_cast<int>(value) < width ? static_cast<int>(value) : static_cast<int>(value) + 1; continue; }
      } else if (value & (1u << (start - 1))) { width = static_cast<int>((value + 1) & 0xff); continue; }
      std::int32_t v;
      if (width < bits) { const int shift = 32 - width; v = static_cast<std::int32_t>(value << shift) >> shift; }
      else v = sixteen ? static_cast<std::int16_t>(value) : static_cast<std::int8_t>(value);
      d1 += v; d2 += d1;
      const std::int64_t sum = second ? d2 : d1;
      out[done + n++] = sixteen ? static_cast<std::int16_t>(sum) : static_cast<std::int8_t>(sum);   // wraps to 8 or 16 bits
    }
    done += length;
    at = end;
  }
  return out;
}

ItModule parseIt(const std::vector<std::uint8_t>& bytes) {
  const Reader r(bytes, "IT file");
  if (r.length() < 192 || r.text(0, 4) != "IMPM") throw std::runtime_error("Not an IT file.");
  const int orderCount = r.u16(32), instrumentCount = r.u16(34), sampleCount = r.u16(36), patternCount = r.u16(38);
  if (r.u16(42) < 0x200) throw std::runtime_error("This IT file is from before Impulse Tracker 2.00 and is not supported.");
  ItModule it;
  it.flags = r.u16(44);
  it.useInstruments = (it.flags & 4) != 0;
  for (int i = 0; i < orderCount; i++) { const int o = r.u8(192 + static_cast<std::size_t>(i)); if (o < 254) it.orders.push_back(o); }
  const std::size_t listAt = 192 + static_cast<std::size_t>(orderCount);
  const auto offsetAt = [&](int index) { return static_cast<std::size_t>(r.u32(listAt + static_cast<std::size_t>(index) * 4)); };

  for (int p = 0; p < patternCount; p++) {
    const std::size_t offset = offsetAt(instrumentCount + sampleCount + p);
    if (!offset) { it.patterns.emplace_back(64, std::vector<ItCell>(64)); continue; }
    const std::size_t length = static_cast<std::size_t>(r.u16(offset)), rowCount = static_cast<std::size_t>(std::max(1, std::min(256, r.u16(offset + 2))));
    std::vector<std::vector<ItCell>> rows(rowCount, std::vector<ItCell>(64));
    struct Memory { int mask = 0; ItCell cell; };
    std::vector<Memory> last(64);
    std::size_t q = offset + 8, row = 0;
    const std::size_t end = std::min(r.length(), q + length);
    while (row < rowCount && q < end) {
      const int tag = r.u8(q++);
      if (tag == 0) { row++; continue; }
      const std::size_t c = static_cast<std::size_t>((tag - 1) & 63);
      Memory& memory = last[c];
      if (tag & 128) memory.mask = r.u8(q++);
      const int mask = memory.mask;
      ItCell& cell = rows[row][c];
      if (mask & 1) memory.cell.note = r.u8(q++);
      if (mask & 2) memory.cell.ins = r.u8(q++);
      if (mask & 4) memory.cell.vol = r.u8(q++);
      if (mask & 8) { memory.cell.effect = r.u8(q++); memory.cell.param = r.u8(q++); }
      if (mask & 0x11) cell.note = memory.cell.note;
      if (mask & 0x22) cell.ins = memory.cell.ins;
      if (mask & 0x44) cell.vol = memory.cell.vol;
      if (mask & 0x88) { cell.effect = memory.cell.effect; cell.param = memory.cell.param; }
    }
    it.patterns.push_back(std::move(rows));
  }

  for (int i = 0; i < sampleCount; i++) {
    const std::size_t at = offsetAt(instrumentCount + i);
    ItSample s;
    if (at && r.text(at, at + 4) == "IMPS") {
      s.valid = true;
      const int flag = r.u8(at + 18), cvt = r.u8(at + 46);
      s.name = r.text(at + 20, at + 46);
      s.globalVolume = std::min(64, r.u8(at + 17)); s.volume = std::min(64, r.u8(at + 19)); s.pan = r.u8(at + 47);
      s.length = r.u32(at + 48); s.loopStart = r.u32(at + 52); s.loopEnd = r.u32(at + 56); s.rate = r.u32(at + 60);
      s.susStart = r.u32(at + 64); s.susEnd = r.u32(at + 68);
      for (std::size_t k = 0; k < 4; k++) s.vib[k] = r.u8(at + 76 + k);
      s.loop = (flag & 16) != 0; s.sus = (flag & 32) != 0; s.pingpong = (flag & 64) != 0; s.susPingpong = (flag & 128) != 0;
      SampleHead head;
      head.pointer = r.u32(at + 72); head.length = s.length;
      head.sixteen = (flag & 2) != 0; head.stereo = (flag & 4) != 0; head.compressed = (flag & 8) != 0;
      head.isSigned = (cvt & 1) != 0; head.delta = (cvt & 4) != 0;
      if ((flag & 1) && s.length) {
        std::size_t next = head.pointer;
        s.data = readSampleChannel(r, head, head.pointer, next);
        if (head.stereo) s.right = readSampleChannel(r, head, next, next);
      }
    }
    it.hasStereo = it.hasStereo || !s.right.empty();
    it.samples.push_back(std::move(s));
  }

  for (int i = 0; i < instrumentCount; i++) {
    const std::size_t at = offsetAt(i);
    ItInstrument inst;
    if (at && r.text(at, at + 4) == "IMPI") {
      inst.valid = true;
      for (int n = 0; n < 120; n++) { inst.notes.push_back(r.u8(at + 64 + static_cast<std::size_t>(n) * 2)); inst.map.push_back(r.u8(at + 65 + static_cast<std::size_t>(n) * 2) - 1); }
      bool volFilter = false, panFilter = false, pitFilter = false;
      inst.vol = readEnvelope(r, at + 304, false, volFilter);
      inst.pan2 = readEnvelope(r, at + 386, true, panFilter);
      inst.pit = readEnvelope(r, at + 468, true, pitFilter);
      const int dfp = r.u8(at + 25), ifc = r.u8(at + 58), ifr = r.u8(at + 59);
      inst.nna = std::min(3, r.u8(at + 17)); inst.dct = std::min(3, r.u8(at + 18)); inst.dca = std::min(2, r.u8(at + 19));
      inst.fade = r.u16(at + 20); inst.pps = r.s8(at + 22); inst.ppc = std::min(119, r.u8(at + 23)); inst.gbv = std::min(128, r.u8(at + 24));
      inst.pan = (dfp & 128) ? -1 : std::min(64, dfp);
      inst.ifc = (ifc & 128) ? ifc & 127 : -1;
      inst.ifr = (ifr & 128) ? ifr & 127 : -1;
      inst.filter = pitFilter && inst.pit.on;
    }
    it.instruments.push_back(std::move(inst));
  }

  it.title = r.text(4, 30);
  it.speed = r.u8(50) ? r.u8(50) : 6;
  it.bpm = std::max(32, r.u8(51) ? r.u8(51) : 125);
  it.gv = std::min(128, r.u8(48));
  for (std::size_t c = 0; c < 64; c++) { it.chanPan.push_back(r.u8(64 + c)); it.chanVol.push_back(r.u8(128 + c)); }
  return it;
}

std::vector<ItStep> playOut(const ItModule& it, int maxRows) {
  std::vector<ItStep> steps;
  int speed = it.speed, tempo = it.bpm, gv = it.gv, memT = 0, memW = 0;
  std::size_t pos = 0;
  int row = 0;
  std::set<std::pair<std::size_t, int>> seen;
  std::vector<int> loopRow(64, 0), loopCount(64, 0), memS(64, 0);
  const auto clampTempo = [](int t) { return std::max(32, std::min(255, t)); };
  while (pos < it.orders.size() && static_cast<int>(steps.size()) < maxRows) {
    const std::size_t pattern = static_cast<std::size_t>(it.orders[pos]);
    if (pattern >= it.patterns.size() || row >= static_cast<int>(it.patterns[pattern].size())) { pos++; row = 0; continue; }
    const auto& rows = it.patterns[pattern];
    const bool looping = std::any_of(loopCount.begin(), loopCount.end(), [](int c) { return c > 0; });
    if (seen.count({pos, row}) && !looping) break;
    seen.insert({pos, row});
    std::optional<int> jump, brk, loop;
    int delay = 0, tickDelay = 0, tslide = 0, gslide = 0;
    for (std::size_t c = 0; c < rows[static_cast<std::size_t>(row)].size(); c++) {
      const ItCell& cell = rows[static_cast<std::size_t>(row)][c];
      const int e = cell.effect, p = cell.param;
      if (e == kA) { if (p) speed = p; }
      else if (e == kT) {
        if (p) memT = p;
        if (memT >= 0x20) { if (p) tempo = clampTempo(p); } else tslide = (memT >> 4) == 1 ? memT & 15 : -(memT & 15);
      } else if (e == kB) jump = p;
      else if (e == kC) brk = p;
      else if (e == kV) gv = std::min(128, p);
      else if (e == kW) {
        if (p) memW = p;
        const int x = memW >> 4, y = memW & 15;
        if (y == 15 && x) gv = std::min(128, gv + x);
        else if (x == 15 && y) gv = std::max(0, gv - y);
        else if (y == 0) gslide = x; else if (x == 0) gslide = -y;
      } else if (e == kS) {
        if (p) memS[c] = p;
        const int sub = memS[c] >> 4, x = memS[c] & 15;
        if (sub == 0xB) {
          if (x == 0) loopRow[c] = row;
          else {
            if (loopCount[c] == 0) loopCount[c] = x; else loopCount[c]--;
            if (loopCount[c] > 0) loop = loopRow[c];
          }
        } else if (sub == 0xE) { if (!delay) delay = x; }
        else if (sub == 6) tickDelay += x;
      }
    }
    ItStep step;
    step.pattern = static_cast<int>(pattern); step.row = row; step.speed = speed; step.ticks = speed * (1 + delay) + tickDelay;
    step.tempo = tempo; step.tslide = tslide; step.gv = gv; step.gslide = gslide;
    steps.push_back(step);
    tempo = clampTempo(tempo + tslide * (step.ticks - 1));
    gv = std::max(0, std::min(128, gv + gslide * (step.ticks - 1)));
    if (!jump && !brk && loop) { row = *loop; continue; }
    if (jump || brk) {
      pos = jump ? static_cast<std::size_t>(*jump) : pos + 1;
      const bool known = pos < it.orders.size() && static_cast<std::size_t>(it.orders[pos]) < it.patterns.size();
      row = brk && known && *brk < static_cast<int>(it.patterns[static_cast<std::size_t>(it.orders[pos])].size()) ? *brk : 0;
    } else if (++row >= static_cast<int>(rows.size())) { pos++; row = 0; }
  }
  return steps;
}

Json itToProjectFile(const ItModule& it, const std::string& name) {
  const std::vector<ItStep> steps = playOut(it);
  if (steps.empty()) throw std::runtime_error("That module has no rows to play.");

  Json samples = Json::object();
  std::vector<std::string> slots;
  for (std::size_t i = 0; i < it.samples.size(); i++) {
    const ItSample& s = it.samples[i];
    const std::string id = uniqueSampleId(samples, sampleIdFromName(hex(static_cast<int>(i) + 1, 2) + "_" + (s.valid && !s.name.empty() ? s.name : "sample")));
    slots.push_back(id);
    if (!s.valid) { samples[id] = encodeSample(8363, std::vector<float>(2, 0.0f), {}); continue; }
    const double size = static_cast<double>(s.data.size());
    const auto fits = [&](double a, double b) { return b - a >= 2 && b <= size && a >= 0; };
    const double rate = std::max(1000.0, std::min(384000.0, s.rate ? static_cast<double>(s.rate) : 8363.0));
    Json entry = encodeSample(rate, s.data.empty() ? std::vector<float>(2, 0.0f) : s.data, s.right);
    entry["volume"] = s.volume / 64.0;
    if (s.globalVolume != 64) entry["globalVolume"] = s.globalVolume / 64.0;
    if (s.pan & 128) { entry["pan"] = std::max(-1.0, std::min(1.0, ((s.pan & 127) - 32) / 32.0)); entry["panOn"] = true; }
    if (s.loop && fits(s.loopStart, s.loopEnd)) {
      entry["loopStart"] = s.loopStart; entry["loopEnd"] = s.loopEnd; entry["loopOn"] = true;
      if (s.pingpong) entry["pingpong"] = true;
    }
    if (s.sus && fits(s.susStart, s.susEnd)) {
      entry["susStart"] = s.susStart; entry["susEnd"] = s.susEnd; entry["susOn"] = true;
      if (s.susPingpong) entry["susPingpong"] = true;
    }
    samples[id] = entry;
  }
  if (slots.empty()) throw std::runtime_error("That IT file has no samples.");
  Json smp = Json::array();
  for (const ItSample& s : it.samples) {
    if (s.valid && s.vib[1] && s.vib[2]) {
      Json extras = Json::object();
      extras["vib"] = std::vector<int>{s.vib[0], s.vib[1], s.vib[2], s.vib[3] & 3};
      smp.push_back(extras);
    } else smp.push_back(nullptr);
  }
  Json ins = Json::array();
  if (it.useInstruments) {
    for (const ItInstrument& i : it.instruments) {
      if (!i.valid) { ins.push_back(nullptr); continue; }
      Json def = {{"nna", i.nna}, {"dct", i.dct}, {"dca", i.dca}, {"fade", i.fade}, {"pps", i.pps}, {"ppc", i.ppc}, {"gbv", i.gbv}, {"pan", i.pan},
                  {"ifc", i.ifc}, {"ifr", i.ifr}, {"map", i.map}, {"notes", i.notes}, {"vol", envelopeJson(i.vol)}, {"pan2", envelopeJson(i.pan2)},
                  {"pit", envelopeJson(i.pit)}, {"filter", i.filter}};
      ins.push_back(def);
    }
  }

  std::vector<int> channelsUsed;
  for (int c = 0; c < 64; c++) {
    const bool sounds = std::any_of(steps.begin(), steps.end(), [&](const ItStep& s) {
      return !isEmptyCell(it.patterns[static_cast<std::size_t>(s.pattern)][static_cast<std::size_t>(s.row)][static_cast<std::size_t>(c)]);
    });
    if (sounds) channelsUsed.push_back(c);
  }
  if (channelsUsed.empty()) throw std::runtime_error("That module has no notes.");

  // Rows that go together become one pattern: consecutive rows of one IT pattern at one speed. A row with extra ticks stands alone.
  struct Run { int pattern, start, count, speed, ticks; };
  const auto extra = [](int speed, int ticks) { return ticks != speed; };
  std::vector<Run> runs;
  for (const ItStep& s : steps) {
    if (!runs.empty()) {
      Run& last = runs.back();
      if (!extra(s.speed, s.ticks) && !extra(last.speed, last.ticks) && last.pattern == s.pattern && last.speed == s.speed && last.start + last.count == s.row) { last.count++; continue; }
    }
    runs.push_back({s.pattern, s.row, 1, s.speed, s.ticks});
  }
  int total = 0;
  for (const ItStep& s : steps) total += s.ticks;
  const auto puidOf = [&](const Run& r) {
    std::string id = "p" + hex(r.pattern, 2);
    if (r.start || r.count != static_cast<int>(it.patterns[static_cast<std::size_t>(r.pattern)].size())) id += "-r" + std::to_string(r.start) + "-" + std::to_string(r.start + r.count - 1);
    if (r.speed != 6) id += "-s" + std::to_string(r.speed);
    if (extra(r.speed, r.ticks)) id += "-t" + std::to_string(r.ticks);
    return id;
  };

  Json instruments = Json::object(), lanes = Json::array();
  std::vector<Json> laneList;
  for (const int c : channelsUsed) {
    Json def = machineJson("itchan", slots);
    const int pan = it.chanPan[static_cast<std::size_t>(c)];
    Json data = {{"linear", (it.flags & 8) != 0}, {"oldEffects", (it.flags & 16) != 0}, {"compatG", (it.flags & 32) != 0},
                 {"chanVol", std::min(64, it.chanVol[static_cast<std::size_t>(c)])}, {"chanPan", pan < 65 ? pan : 32}};
    if (it.hasStereo) data["it3"] = true;
    if (it.useInstruments) data["ins"] = ins;
    data["smp"] = smp;
    for (Json& node : def["graph"]["nodes"]) if (node.value("id", std::string()) == "ch") node["data"] = data;
    const std::string id = "ch" + std::to_string(c + 1);
    instruments[id] = def;
    laneList.push_back({{"id", id}, {"instrument", id}, {"patterns", Json::object()}, {"placements", Json::array()}});
  }
  int tick = 0;
  for (const Run& run : runs) {
    const std::string puid = puidOf(run);
    for (std::size_t lane = 0; lane < channelsUsed.size(); lane++) {
      Json& target = laneList[lane];
      if (!target["patterns"].contains(puid)) {
        Json rows = Json::array();
        for (int r = run.start; r < run.start + run.count; r++) {
          rows.push_back(cellText(it.patterns[static_cast<std::size_t>(run.pattern)][static_cast<std::size_t>(r)][static_cast<std::size_t>(channelsUsed[lane])]));
          for (int t = 1; t < run.ticks; t++) rows.push_back("... .. .. ......");
        }
        const int index = static_cast<int>(target["patterns"].size());
        target["patterns"][puid] = {{"name", puid}, {"index", index}, {"rows", rows}};
      }
      target["placements"].push_back({{"puid", puid}, {"start", tick}, {"length", run.count * run.ticks}});
    }
    tick += run.count * run.ticks;
  }

  // The master lane: the tempo in the BPM column on the tick it changes, the global volume in the Vol column.
  Json master = {{"id", "master"}, {"instrument", "master"}, {"patterns", Json::object()}, {"placements", Json::array()}};
  std::vector<std::string> bpmCells(static_cast<std::size_t>(total), "...."), volCells(static_cast<std::size_t>(total), "..");
  int at = 0, gvNow = 128, tempoNow = it.bpm;
  bool any = false;
  for (const ItStep& s : steps) {
    for (int t = 0; t < s.ticks; t++) {
      const int gv = std::max(0, std::min(128, s.gv + s.gslide * t)), tempo = std::max(32, std::min(255, s.tempo + s.tslide * t));
      if (gv != gvNow) { volCells[static_cast<std::size_t>(at + t)] = hex(static_cast<int>(std::floor(gv / 128.0 * 255 + 0.5)), 2); gvNow = gv; any = true; }
      if (tempo != tempoNow) { bpmCells[static_cast<std::size_t>(at + t)] = hex(tempo, 4); tempoNow = tempo; any = true; }
    }
    at += s.ticks;
  }
  if (any) {
    Json rows = Json::array();
    for (int t = 0; t < total; t++) rows.push_back(bpmCells[static_cast<std::size_t>(t)] + " .. .. " + volCells[static_cast<std::size_t>(t)]);
    master["patterns"]["song"] = {{"name", "song"}, {"index", 0}, {"rows", rows}};
    master["placements"].push_back({{"puid", "song"}, {"start", 0}, {"length", total}});
  }
  lanes.push_back(master);
  for (const Json& lane : laneList) lanes.push_back(lane);

  const std::string title = cleanName(it.title);
  Json file = Json::object();
  file["format"] = "webtracker";
  file["version"] = 1;
  file["song"] = {{"name", !title.empty() ? title : name}, {"bpm", it.bpm}, {"ticksPerBeat", kItTicksPerBeat}, {"length", std::max(16, (total + 15) / 16 * 16)}};
  file["instruments"] = instruments;
  file["samples"] = samples;
  file["lanes"] = lanes;
  return file;
}

Project projectFromItFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("The file could not be read.");
  const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string name = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
  if (name.size() > 3 && (name.substr(name.size() - 3) == ".it" || name.substr(name.size() - 3) == ".IT")) name.resize(name.size() - 3);
  return parseProject(itToProjectFile(parseIt(bytes), name).dump());
}

}  // namespace wax::app

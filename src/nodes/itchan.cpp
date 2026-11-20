// One Impulse Tracker channel: an IT playroutine for IT files and IT-style patterns. Port of webtracker
// src/audio/nodes/itchan.js, which describes the pattern cells, the effects and the data the node keeps.
//
// What makes it IT: New Note Actions (the old voice goes to the background and is cut, carried on, released or faded; the
// duplicate check can silence background voices), a resonant low-pass filter on every voice, instruments with volume / panning /
// pitch (or filter) envelopes, sample sustain loops and ping-pong loops, auto-vibrato. With `it3` in the node's data it also
// plays stereo samples and crossfaded loops, which IT itself does not have.
//
// Timing works as for the XM channel: the song's tick is the IT tick, a row starts when the Cmd column writes `fx` (command 01),
// and the note, the Ins cell and the Vol cell of that tick come with it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kIns = 0, kVcol, kFx, kLevel, kInterp };

const int kSine[32] = {0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253, 255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24};
constexpr int kMidiC0 = 12;            // a note's MIDI number for C-0
constexpr int kC5 = 60;                // the note a sample plays at its own rate (IT note numbers count from C-0)
constexpr std::size_t kMaxBack = 31;   // background voices
constexpr double kMaxPeriod = 32000;
constexpr double kFadeFull = 65536;    // the fadeout volume of a new note; every tick of a fade takes 64 times the instrument's fadeout
constexpr double kPi = 3.141592653589793;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Pitch is kept as a "period": linear (64 to a semitone, higher notes have smaller periods) or Amiga (the classic table, times four).
double periodOf(double note, bool linear) { return linear ? 7680 - (note - 12) * 64 : 1712 * js::pow(2, (60 - note) / 12); }
double rateOf(double period, bool linear) { return linear ? 8363 * js::pow(2, (4608 - period) / 768) : 8363 * 1712 / period; }

int waveOf(int kind, int pos) {   // -64..64 for a position 0..63 of the cycle: sine, ramp down, square (random is left to the caller)
  const int w = kind & 3, p = pos & 63;
  if (w == 0) return ((p & 32) ? -1 : 1) * jsRoundInt(kSine[p & 31] / 4.0);
  if (w == 1) return 64 - p * 2;
  if (w == 2) return p < 32 ? 64 : -64;
  return 0;
}

// points: ticks and values, flat (x0, y0, x1, y1, ...); ls, le, ss and se are point numbers or -1.
struct Envelope {
  std::vector<double> pts;
  int ls = -1, le = -1, ss = -1, se = -1;
  double at(int point) const {   // the tick of a point, NaN when there is no such point (so a comparison with it is false)
    const long index = static_cast<long>(point) * 2;
    return index >= 0 && index < static_cast<long>(pts.size()) ? pts[static_cast<std::size_t>(index)] : kNaN;
  }
};

double envValue(const Envelope& env, double pos) {
  const std::vector<double>& p = env.pts;
  const std::size_t last = p.size() - 2;
  if (pos <= p[0]) return p[1];
  for (std::size_t i = 0; i < last; i += 2) {
    const double x0 = p[i], x1 = p[i + 2];
    if (pos < x1) return x1 > x0 ? p[i + 1] + (p[i + 3] - p[i + 1]) * (pos - x0) / (x1 - x0) : p[i + 3];
  }
  return p[last + 1];
}

// The tick after `pos`: the sustain loop holds while the key is down, the loop turns at its end.
double envNext(const Envelope& env, double pos, bool keyOn) {
  const double next = pos + 1;
  if (keyOn && env.ss >= 0 && next > env.at(env.se)) return env.at(env.ss);
  if (env.ls >= 0 && next > env.at(env.le)) return env.at(env.ls);
  return next;
}

struct Instrument {
  int nna = 0, dct = 0, dca = 0, fade = 0, pps = 0;
  double ppc = kC5, gbv = 128, pan = -1, ifc = -1, ifr = -1;
  std::vector<int> map;                 // the sample slot of each note from C-0 (-1: none)
  std::optional<std::vector<int>> notes;  // the note each note plays (optional remap)
  std::optional<Envelope> vol, pan2, pit;
  bool filter = false;
};

struct Extra {   // a sample's own extras from the node's data
  bool vib = false;
  int speed = 0, depth = 0, rate = 0, type = 0;
};

struct Bank {
  std::shared_ptr<const SampleMessage> msg;
  double volume = 1, globalVolume = 1, pan = 0, tune = 0;
  bool panSet = false, pp = false, spp = false;
  bool loop = false, sus = false;
  double ls = 0, le = 0, ss = 0, se = 0, xf = 0, sxf = 0;
};

// One sounding voice: the host voice of the channel, or one that plays on in the background.
struct Voice {
  bool active = false;
  int slot = -1;
  double pos = 0;
  int dir = 1, note = kC5, ins = -1;
  double period = 0, outPeriod = 0, target = 0;
  int vol = 64, outVol = 64, pan = 32;
  double gain = 0;
  bool keyOn = true, fading = false;
  double fadeVol = kFadeFull, volEnvPos = 0, panEnvPos = 0, pitEnvPos = 0;
  int autoPos = 0, autoAmp = 0;
  int cut = 127, res = 0;
  double envVol = 64, envPan = 32, envMod = 0, pitchOff = 0;
  int autoDelta = 0;
  double fg = 1, fb0 = 0, fb1 = 0;
  bool filterOn = false;
  double y[4] = {0, 0, 0, 0};
};

// What a row's first tick is told: the note, key off or cut that came with it, the instrument and the volume column byte.
struct Row {
  int note = -1;
  bool off = false, cutNote = false, insWritten = false;
  int insValue = 0, v = -1;
};

double numberAt(const Json& j, const char* key, double fallback) {
  return j.is_object() && j.contains(key) && j[key].is_number() ? j[key].get<double>() : fallback;
}

// `x | 0`: a number as a 32-bit integer (0 when it is not one).
int intAt(const Json& j, const char* key) { return static_cast<int>(numberAt(j, key, 0)); }

std::optional<Envelope> envelopeOf(const Json& j, const char* key) {
  if (!j.contains(key) || !j[key].is_object()) return std::nullopt;
  const Json& e = j[key];
  if (!e.contains("pts") || !e["pts"].is_array() || e["pts"].size() < 2) return std::nullopt;
  Envelope out;
  for (const auto& v : e["pts"]) out.pts.push_back(v.is_number() ? v.get<double>() : 0.0);
  out.ls = static_cast<int>(numberAt(e, "ls", -1));
  out.le = static_cast<int>(numberAt(e, "le", -1));
  out.ss = static_cast<int>(numberAt(e, "ss", -1));
  out.se = static_cast<int>(numberAt(e, "se", -1));
  return out;
}

class ItChan final : public Node {
public:
  explicit ItChan(const NodeContext& context) : sampleRate_(context.sampleRate), clock_(context.clock) {
    if (context.data) readData(*context.data);
    host_.pan = chanPan_;   // a note takes the channel's panning unless its sample or instrument has one of its own
    host_.period = host_.outPeriod = host_.target = periodOf(kC5, linear_);
  }

  void data(const Json& data) override { readData(data); }

  void sample(const std::shared_ptr<const SampleMessage>& message) override {
    const std::size_t slot = static_cast<std::size_t>(std::max(0, message->slot));
    if (bank_.size() <= slot) bank_.resize(slot + 1);
    auto S = std::make_unique<Bank>();
    S->msg = message;
    S->volume = message->volume;
    S->globalVolume = message->globalVolume;
    S->pan = std::max(-1.0, std::min(1.0, message->pan));
    S->panSet = message->panOn;
    S->pp = message->pingpong;
    S->spp = message->susPingpong;
    S->tune = 12 * js::log2(std::max(1.0, message->rate) / 8363);   // semitones from IT's C-5 (8363 Hz): the sample's own speed
    const double size = static_cast<double>(message->data.size());
    const double start = message->loopStart, end = message->loopEnd;
    if (message->loopOn && end - start >= 2 && end <= size && start >= 0) {
      S->loop = true; S->ls = start; S->le = end;
      S->xf = std::max(0.0, std::min({std::floor(message->loopXfade), start, end - start}));
    }
    const double ss = message->susStart, se = message->susEnd;
    if (message->susOn && se - ss >= 2 && se <= size && ss >= 0) {
      S->sus = true; S->ss = ss; S->se = se;
      S->sxf = std::max(0.0, std::min({std::floor(message->susXfade), ss, se - ss}));
    }
    bank_[slot] = std::move(S);
  }

  void note(const NoteEvent& event) override {
    const bool playing = clock_ && clock_->playing;
    if (event.type == NoteType::On) { pendingNote_ = event.note - kMidiC0; pendingOff_ = false; pendingCut_ = false; }
    else if (event.type == NoteType::Off && !event.implied) { pendingOff_ = true; if (!playing) keyOffVoice(host_); }
    else if (event.type == NoteType::Cut) { pendingCut_ = true; if (!playing) silence(host_); }
    else if (event.type == NoteType::AllOff) {
      silence(host_);
      for (Voice& b : back_) silence(b);
      back_.clear();
      pendingNote_ = -1; pendingOff_ = false; pendingCut_ = false; delayedRow_.reset();
      cmd_ = par_ = 0; vcol_ = -1; lastFx_ = -1;
    }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    const bool playing = clock_ && clock_->playing;
    const int id = clock_ ? clock_->tickId : 0;
    if (playing && id != lastTick_) { lastTick_ = id; tick(params, slots); }
    else if (!playing) {
      lastTick_ = id;
      if (pendingNote_ >= 0) {   // not playing: a note from the keyboard sounds at once, with the Ins column's instrument
        curIns_ = std::max(0, jsRoundInt(params[slots[kIns]]));
        cmd_ = par_ = 0; vcol_ = -1;
        if (newNote(pendingNote_, curIns_, 0)) takeDefaults(host_, curIns_);
        host_.outPeriod = host_.period; host_.outVol = host_.vol; pendingNote_ = -1; pendingOff_ = false;
        idleSamples_ = 0; allTick();
      }
      idleSamples_ += n;   // the envelopes keep time on their own, so an instrument can be heard as it will play
      const double tickSamples = clock_ && clock_->tickSamples != 0 ? clock_->tickSamples : sampleRate_ / 50;
      while (idleSamples_ >= tickSamples) { idleSamples_ -= tickSamples; allTick(); }
    }
    std::fill(outL, outL + n, 0.0f);
    std::fill(outR, outR + n, 0.0f);
    const double level = params[slots[kLevel]];
    const bool interpolate = params[slots[kInterp]] >= 0.5;
    mixVoice(host_, true, n, outL, outR, level, interpolate);
    for (Voice& b : back_) mixVoice(b, false, n, outL, outR, level, interpolate);
  }

private:
  void readData(const Json& d) {
    it3_ = d.is_object() && d.contains("it3") && d["it3"].is_boolean() && d["it3"].get<bool>();
    linear_ = !(d.is_object() && d.contains("linear") && d["linear"].is_boolean() && !d["linear"].get<bool>());
    oldEffects_ = d.is_object() && d.contains("oldEffects") && d["oldEffects"].is_boolean() && d["oldEffects"].get<bool>();
    compatG_ = !(d.is_object() && d.contains("compatG") && d["compatG"].is_boolean() && !d["compatG"].get<bool>());
    chanVol_ = static_cast<int>(numberAt(d, "chanVol", 64));
    chanPan_ = static_cast<int>(numberAt(d, "chanPan", 32));
    instruments_.clear();
    extras_.clear();
    hasInstruments_ = d.is_object() && d.contains("ins") && d["ins"].is_array() && !d["ins"].empty();
    if (hasInstruments_) {
      for (const auto& i : d["ins"]) {
        if (!i.is_object()) { instruments_.push_back(std::nullopt); continue; }
        Instrument I;
        I.nna = intAt(i, "nna"); I.dct = intAt(i, "dct"); I.dca = intAt(i, "dca"); I.fade = intAt(i, "fade"); I.pps = intAt(i, "pps");
        I.ppc = numberAt(i, "ppc", kC5); I.gbv = numberAt(i, "gbv", 128);
        I.pan = numberAt(i, "pan", -1); I.ifc = numberAt(i, "ifc", -1); I.ifr = numberAt(i, "ifr", -1);
        if (i.contains("map") && i["map"].is_array()) {
          for (const auto& m : i["map"]) I.map.push_back(m.is_number() ? static_cast<int>(m.get<double>()) : -1);
        }
        if (i.contains("notes") && i["notes"].is_array()) {
          std::vector<int> notes;
          for (const auto& m : i["notes"]) notes.push_back(m.is_number() ? static_cast<int>(m.get<double>()) : 0);
          I.notes = std::move(notes);
        }
        I.vol = envelopeOf(i, "vol"); I.pan2 = envelopeOf(i, "pan2"); I.pit = envelopeOf(i, "pit");
        I.filter = i.contains("filter") && i["filter"].is_boolean() && i["filter"].get<bool>();
        instruments_.push_back(std::move(I));
      }
    }
    if (d.is_object() && d.contains("smp") && d["smp"].is_array()) {
      for (const auto& s : d["smp"]) {
        Extra x;
        if (s.is_object() && s.contains("vib") && s["vib"].is_array() && s["vib"].size() >= 3) {
          const Json& v = s["vib"];
          auto at = [&](std::size_t k) { return k < v.size() && v[k].is_number() ? static_cast<int>(v[k].get<double>()) : 0; };
          x.speed = at(0); x.depth = at(1); x.rate = at(2); x.type = at(3);
          x.vib = x.depth != 0 && x.rate != 0;
        }
        extras_.push_back(x);
      }
    }
  }

  static int clampVol(double v) { return std::max(0, std::min(64, jsRoundInt(v))); }
  static int clampPan(double v) { return std::max(0, std::min(64, jsRoundInt(v))); }
  static double clampPeriod(double v) { return std::max(1.0, std::min(kMaxPeriod, v)); }

  const Bank* bankAt(int i) const { return i >= 0 && i < static_cast<int>(bank_.size()) ? bank_[static_cast<std::size_t>(i)].get() : nullptr; }
  const Instrument* instrumentAt(int i) const {
    return i >= 0 && i < static_cast<int>(instruments_.size()) && instruments_[static_cast<std::size_t>(i)] ? &*instruments_[static_cast<std::size_t>(i)] : nullptr;
  }
  const Instrument* instrumentOf(const Voice& v) const { return hasInstruments_ ? instrumentAt(v.ins) : nullptr; }
  const Extra* extra(int slot) const { return slot >= 0 && slot < static_cast<int>(extras_.size()) ? &extras_[static_cast<std::size_t>(slot)] : nullptr; }
  double tuneOf(int slot) const { const Bank* S = bankAt(slot); return S ? S->tune : 0; }

  // The sample slot for an instrument number and a note, and the note it plays.
  void slotFor(int insNo, int n, int& slot, int& played) const {
    played = n;
    if (!hasInstruments_) { slot = insNo; return; }
    const Instrument* I = instrumentAt(insNo);
    if (!I) { slot = -1; return; }
    slot = n >= 0 && n < static_cast<int>(I->map.size()) ? I->map[static_cast<std::size_t>(n)] : -1;
    if (I->notes && n >= 0 && n < static_cast<int>(I->notes->size())) played = (*I->notes)[static_cast<std::size_t>(n)];
  }

  static void silence(Voice& v) { v.active = false; }
  void keyOffVoice(Voice& v) {
    v.keyOn = false; v.fading = true;
    if (!instrumentOf(v)) v.vol = 0;   // without an instrument a key off is a cut
  }
  static void resetVoiceEnvelopes(Voice& v) {
    v.keyOn = true; v.fading = false; v.fadeVol = kFadeFull; v.volEnvPos = v.panEnvPos = v.pitEnvPos = 0; v.autoPos = 0; v.autoAmp = 0;
  }

  // The filter coefficients of a voice for this tick (IT's resonant two-pole low-pass, in OpenMPT's form).
  void setupFilter(Voice& v) const {
    const int c = std::max(0, std::min(127, v.cut)), r = std::max(0, std::min(127, v.res));
    const double computed = std::trunc(c * (v.envMod + 256) / 256);
    if (r == 0 && computed >= 254) { v.filterOn = false; return; }
    double freq = 110 * js::pow(2, 0.25 + c * (v.envMod + 256) / (24 * 512));
    freq = std::max(120.0, std::min(20000.0, freq));
    freq = std::min(freq, sampleRate_ / 2);
    const double dmp = js::pow(10, -r * (24.0 / 128 / 20));
    const double fc = freq * 2 * kPi, rr = sampleRate_ / fc;
    const double d = dmp * rr + dmp - 1, e = rr * rr;
    v.fg = 1 / (1 + d + e); v.fb0 = (d + e + e) * v.fg; v.fb1 = -e * v.fg;
    v.filterOn = true;
  }

  // A note arrives on the channel: the duplicate check, the note action of the voice that was playing, and the new host voice.
  bool newNote(int n, int insNo, double offset) {
    int slot = -1, played = n;
    slotFor(insNo, n, slot, played);
    const Instrument* I = hasInstruments_ ? instrumentAt(insNo) : nullptr;
    const Bank* S = bankAt(slot);
    if (slot < 0 || !S) return false;
    if (I && I->dct) {
      auto same = [&](const Voice& v) {
        if (!v.active) return false;
        if (I->dct == 1) return v.note == played && v.ins == insNo;
        if (I->dct == 2) return v.slot == slot;
        return v.ins == insNo;
      };
      auto check = [&](Voice& v) {
        if (!same(v)) return;
        if (I->dca == 0) silence(v); else if (I->dca == 1) keyOffVoice(v); else v.fading = true;
      };
      check(host_);
      for (Voice& b : back_) check(b);
    }
    if (host_.active) {
      const Instrument* old = instrumentOf(host_);
      const int action = nnaOverride_ >= 0 ? nnaOverride_ : old ? old->nna : 0;
      if (action != 0) {
        Voice bg = host_;
        if (action == 2) keyOffVoice(bg); else if (action == 3) bg.fading = true;
        back_.push_back(bg);
        if (back_.size() > kMaxBack) back_.erase(back_.begin());
      }
    }
    Voice v;
    v.slot = slot; v.ins = insNo; v.note = played; v.active = true;
    v.vol = host_.vol; v.pan = host_.pan; v.cut = cut_; v.res = res_;
    v.period = v.outPeriod = v.target = clampPeriod(periodOf(played + tuneOf(slot), linear_));
    v.pos = offset;
    v.active = v.pos < static_cast<double>(S->msg->data.size());
    host_ = v;
    return true;
  }

  // An instrument number on the row: volume, panning and filter of the instrument and sample.
  void takeDefaults(Voice& v, int insNo) {
    const Bank* S = bankAt(v.slot);
    if (!S) return;
    const Instrument* I = hasInstruments_ ? instrumentAt(insNo) : nullptr;
    v.vol = clampVol(S->volume * 64);
    if (I && I->pan >= 0) v.pan = static_cast<int>(I->pan);
    else if (S->panSet) v.pan = clampPan(S->pan * 32 + 32);
    if (I) {
      if (I->ifc >= 0) cut_ = static_cast<int>(I->ifc);
      if (I->ifr >= 0) res_ = static_cast<int>(I->ifr);
      v.cut = cut_; v.res = res_;
    }
  }

  // The row's first tick: the note, the instrument, the volume column and the effect's first move.
  void startRow(const Row& row) {
    const bool porta = cmd_ == 7 || cmd_ == 12 || (row.v >= 0xC1 && row.v <= 0xCA);
    if (row.insWritten) curIns_ = row.insValue;
    if (row.cutNote) silence(host_);
    else if (row.off) keyOffVoice(host_);
    else if (row.note >= 0) {
      if (porta && host_.active) {
        int slot = -1, played = row.note;
        slotFor(curIns_, row.note, slot, played);
        host_.target = clampPeriod(periodOf(played + tuneOf(host_.slot), linear_));
        if (row.insWritten) { takeDefaults(host_, curIns_); resetVoiceEnvelopes(host_); }
      } else {
        if (cmd_ == 15 && par_) memO_ = par_;
        const double offset = cmd_ == 15 ? (memO_ + memHigh_ * 256) * 256 : 0;
        if (newNote(row.note, curIns_, offset)) {
          if (row.insWritten) takeDefaults(host_, curIns_);
          else if (hasInstruments_) {
            const Instrument* I = instrumentAt(curIns_);
            if (I && I->ifc >= 0) { cut_ = static_cast<int>(I->ifc); host_.cut = cut_; }
            if (I && I->ifr >= 0) { res_ = static_cast<int>(I->ifr); host_.res = res_; }
          }
          if ((vibWave_ & 4) == 0) vibPos_ = 0;
          if ((tremWave_ & 4) == 0) tremPos_ = 0;
          if ((panWave_ & 4) == 0) panPos_ = 0;
          tremorCount_ = 0; tremorOn_ = true; retrigCount_ = 0;
        } else host_.active = false;
      }
    } else if (row.insWritten) { takeDefaults(host_, curIns_); resetVoiceEnvelopes(host_); }
    volumeColumnFirstTick(row.v);
  }

  void volumeColumnFirstTick(int vc) {
    if (vc < 0) return;
    static const int portaSpeeds[10] = {0, 1, 4, 8, 16, 32, 64, 96, 128, 255};
    if (vc <= 64) host_.vol = vc;
    else if (vc <= 74) { const int x = (vc - 65) ? vc - 65 : memVol_; if (vc - 65) memVol_ = vc - 65; host_.vol = clampVol(host_.vol + x); }
    else if (vc <= 84) { const int x = (vc - 75) ? vc - 75 : memVol_; if (vc - 75) memVol_ = vc - 75; host_.vol = clampVol(host_.vol - x); }
    else if (vc <= 94) { if (vc - 85) memVol_ = vc - 85; }
    else if (vc <= 104) { if (vc - 95) memVol_ = vc - 95; }
    else if (vc <= 114) { if (vc - 105) memEF_ = vc - 105; }
    else if (vc <= 124) { if (vc - 115) memEF_ = vc - 115; }
    else if (vc >= 128 && vc <= 192) host_.pan = std::min(64, vc - 128);
    else if (vc >= 193 && vc <= 202) { const int speed = portaSpeeds[vc - 193]; if (speed) { if (compatG_) memEF_ = speed; else memG_ = speed; } }
    else if (vc >= 203 && vc <= 212) { if (vc - 203) vibDepth_ = vc - 203; }
  }

  void volumeColumnLaterTick(int vc) {
    if (vc < 0) return;
    if (vc >= 85 && vc <= 94) host_.vol = clampVol(host_.vol + memVol_);
    else if (vc >= 95 && vc <= 104) host_.vol = clampVol(host_.vol - memVol_);
    else if (vc >= 105 && vc <= 114) { host_.period = clampPeriod(host_.period + memEF_ * 4); host_.outPeriod = host_.period; }
    else if (vc >= 115 && vc <= 124) { host_.period = clampPeriod(host_.period - memEF_ * 4); host_.outPeriod = host_.period; }
    else if (vc >= 193 && vc <= 202) slideToTarget();
    else if (vc >= 203 && vc <= 212) vibrato(false);
  }

  void slideToTarget() {
    const double speed = (compatG_ ? memEF_ : memG_) * 4;
    if (host_.period > host_.target) host_.period = std::max(host_.target, host_.period - speed);
    else if (host_.period < host_.target) host_.period = std::min(host_.target, host_.period + speed);
    host_.outPeriod = host_.period;
  }

  void vibrato(bool fine) {
    const int delta = (waveOf(vibWave_, vibPos_) * vibDepth_) >> (fine ? 7 : oldEffects_ ? 4 : 5);
    host_.outPeriod = host_.period + delta;
    vibPos_ = (vibPos_ + vibSpeed_) & 63;
  }

  void volumeSlide(int param, bool first) {
    const int x = param >> 4, y = param & 15;
    if (first) {
      if (y == 15 && x > 0) host_.vol = clampVol(host_.vol + x);        // DxF: fine up
      else if (x == 15 && y > 0) host_.vol = clampVol(host_.vol - y);   // DFy: fine down
      return;
    }
    if (y == 15 && x > 0) return;
    if (x == 15 && y > 0) return;
    if (y == 0) host_.vol = clampVol(host_.vol + x); else if (x == 0) host_.vol = clampVol(host_.vol - y);
  }

  void arpeggio(int t) {
    const int off = t % 3 == 0 ? 0 : t % 3 == 1 ? memJ_ >> 4 : memJ_ & 15;
    host_.outPeriod = host_.period - off * 64;
  }

  void effectFirstTick() {
    const int x = par_ & 15, hi = par_ >> 4;
    switch (cmd_) {
      case 4: if (par_) memD_ = par_; volumeSlide(memD_, true); break;   // D
      case 5: case 6: {                                                   // E, F
        if (par_) memEF_ = par_;
        const int dir = cmd_ == 5 ? 1 : -1;
        if (memEF_ >= 0xF0) host_.period = clampPeriod(host_.period + dir * (memEF_ & 15) * 4);
        else if (memEF_ >= 0xE0) host_.period = clampPeriod(host_.period + dir * (memEF_ & 15));
        host_.outPeriod = host_.period;
        break;
      }
      case 7: if (par_) { if (compatG_) memEF_ = par_; else memG_ = par_; } break;   // G
      case 8: if (hi) vibSpeed_ = hi; if (x) vibDepth_ = x; if (oldEffects_) vibrato(false); break;   // H
      case 9: if (par_) memI_ = par_; break;                                // I
      case 10: if (par_) memJ_ = par_; break;                               // J
      case 11: if (par_) memD_ = par_; volumeSlide(memD_, true); break;     // K
      case 12: if (par_) memD_ = par_; volumeSlide(memD_, true); break;     // L
      case 13: chanVol_ = std::min(64, par_); break;                        // M
      case 14: {                                                            // N
        if (par_) memN_ = par_;
        const int a = memN_ >> 4, b = memN_ & 15;
        if (b == 15 && a > 0) chanVol_ = std::min(64, chanVol_ + a); else if (a == 15 && b > 0) chanVol_ = std::max(0, chanVol_ - b);
        break;
      }
      case 16: {                                                            // P
        if (par_) memP_ = par_;
        const int a = memP_ >> 4, b = memP_ & 15;
        if (b == 15 && a > 0) host_.pan = clampPan(host_.pan - a); else if (a == 15 && b > 0) host_.pan = clampPan(host_.pan + b);
        break;
      }
      case 17: if (par_) memQ_ = par_; break;                               // Q
      case 18: if (hi) tremSpeed_ = hi; if (x) tremDepth_ = x; break;       // R
      case 19: sFirstTick(); break;                                         // S
      case 21: if (hi) vibSpeed_ = hi; if (x) vibDepth_ = x; break;         // U
      case 24: host_.pan = std::min(64, (par_ * 64 + 128) >> 8); break;     // X
      case 25: if (hi) panSpeed_ = hi; if (x) panDepth_ = x; break;         // Y
      case 26: zFilter(); break;                                            // Z
      default: break;
    }
  }

  void sFirstTick() {
    if (par_) memS_ = par_;
    const int sub = memS_ >> 4, x = memS_ & 15;
    switch (sub) {
      case 3: vibWave_ = x; break;
      case 4: tremWave_ = x; break;
      case 5: panWave_ = x; break;
      case 7:   // NNA and envelope switches
        if (x == 0) { for (Voice& b : back_) silence(b); }
        else if (x == 1) { for (Voice& b : back_) keyOffVoice(b); }
        else if (x == 2) { for (Voice& b : back_) b.fading = true; }
        else if (x >= 3 && x <= 6) nnaOverride_ = x - 3;
        else if (x >= 7 && x <= 12) envOverride_[static_cast<std::size_t>((x - 7) >> 1)] = (x & 1) == 1;
        break;
      case 8: host_.pan = std::min(64, x * 4 + (x >> 2)); break;
      case 10: memHigh_ = x; break;
      case 12: if (x == 0) silence(host_); else noteCutAt_ = x; break;
      default: break;
    }
  }

  void zFilter() {   // the default macros: Z00-Z7F set the cutoff, Z80-Z8F the resonance
    if (par_ <= 0x7f) { cut_ = par_; host_.cut = cut_; }
    else if (par_ <= 0x8f) { res_ = (par_ - 0x80) * 8; host_.res = res_; }
  }

  static double retriggerVolume(int nibble, double v) {
    switch (nibble) {
      case 1: return v - 1;
      case 2: return v - 2;
      case 3: return v - 4;
      case 4: return v - 8;
      case 5: return v - 16;
      case 6: return v * 2 / 3;
      case 7: return v / 2;
      case 9: return v + 1;
      case 10: return v + 2;
      case 11: return v + 4;
      case 12: return v + 8;
      case 13: return v + 16;
      case 14: return v * 3 / 2;
      case 15: return v * 2;
      default: return v;
    }
  }

  void effectLaterTick(int t) {
    switch (cmd_) {
      case 4: volumeSlide(memD_, false); break;
      case 5: if (memEF_ < 0xE0) { host_.period = clampPeriod(host_.period + memEF_ * 4); host_.outPeriod = host_.period; } break;
      case 6: if (memEF_ < 0xE0) { host_.period = clampPeriod(host_.period - memEF_ * 4); host_.outPeriod = host_.period; } break;
      case 7: slideToTarget(); break;
      case 8: vibrato(false); break;
      case 9: {
        const int on = (memI_ >> 4) + 1, off = (memI_ & 15) + 1;
        tremorOn_ = tremorCount_ % (on + off) < on;
        tremorCount_++;
        break;
      }
      case 10: arpeggio(t); break;
      case 11: vibrato(false); volumeSlide(memD_, false); break;
      case 12: slideToTarget(); volumeSlide(memD_, false); break;
      case 14: {
        const int a = memN_ >> 4, b = memN_ & 15;
        if (b == 0) chanVol_ = std::min(64, chanVol_ + a); else if (a == 0) chanVol_ = std::max(0, chanVol_ - b);
        break;
      }
      case 16: {
        const int a = memP_ >> 4, b = memP_ & 15;
        if (b == 0) host_.pan = clampPan(host_.pan - a); else if (a == 0) host_.pan = clampPan(host_.pan + b);
        break;
      }
      case 17: {
        const int rate = memQ_ & 15;
        if (rate && ++retrigCount_ >= rate) {
          retrigCount_ = 0;
          host_.pos = 0; host_.dir = 1;
          if (bankAt(host_.slot)) host_.active = true;
          host_.vol = clampVol(retriggerVolume(memQ_ >> 4, host_.vol));
        }
        break;
      }
      case 18: {
        const int delta = (waveOf(tremWave_, tremPos_) * tremDepth_) >> 5;
        host_.outVol = clampVol(host_.vol + delta);
        tremPos_ = (tremPos_ + tremSpeed_) & 63;
        break;
      }
      case 19: {
        const int sub = memS_ >> 4, sx = memS_ & 15;
        if (sub == 12 && t == noteCutAt_) { silence(host_); noteCutAt_ = -1; }
        else if (sub == 13 && delayedRow_ && t == sx) { const Row row = *delayedRow_; delayedRow_.reset(); startRow(row); }
        break;
      }
      case 21: vibrato(true); break;
      case 25: panbrelloOut_ = (waveOf(panWave_, panPos_) * panDepth_) >> 6; panPos_ = (panPos_ + panSpeed_) & 63; break;
      default: break;
    }
  }

  std::uint32_t random() {
    dice_ ^= dice_ << 13; dice_ ^= dice_ >> 17; dice_ ^= dice_ << 5;
    return dice_;
  }

  // Every tick, playing or not: each voice's envelopes, fadeout, auto-vibrato and filter move on.
  void voiceTick(Voice& v, bool isHost) {
    const Instrument* I = instrumentOf(v);
    const Extra* X = extra(v.slot);
    v.envVol = 64; v.envPan = 32; v.envMod = 0; v.pitchOff = 0; v.autoDelta = 0;
    if (I) {
      if (I->vol && envOverride_[0]) { v.envVol = envValue(*I->vol, v.volEnvPos); v.volEnvPos = envNext(*I->vol, v.volEnvPos, v.keyOn); }
      if (I->pan2 && envOverride_[1]) { v.envPan = envValue(*I->pan2, v.panEnvPos); v.panEnvPos = envNext(*I->pan2, v.panEnvPos, v.keyOn); }
      if (I->pit && envOverride_[2]) {
        const double e = envValue(*I->pit, v.pitEnvPos);
        v.pitEnvPos = envNext(*I->pit, v.pitEnvPos, v.keyOn);
        if (I->filter) v.envMod = (e - 32) * 8; else v.pitchOff = (e - 32) * 32;
      }
      if (v.fading) {
        if (I->fade) v.fadeVol = std::max(0.0, v.fadeVol - I->fade * 64);
        else if (!I->vol || !envOverride_[0]) v.fadeVol = 0;
      }
    } else if (v.fading) v.fadeVol = 0;
    if (X && X->vib) {
      v.autoAmp = std::min(X->depth << 8, v.autoAmp + (X->rate << 2));
      const int w = X->type == 3 ? static_cast<int>((static_cast<std::int32_t>(random()) >> 8) & 127) - 64 : waveOf(X->type, v.autoPos);
      v.autoDelta = (w * v.autoAmp) >> 14;
      v.autoPos = (v.autoPos + X->speed) & 63;
    }
    if (isHost) { v.cut = cut_; v.res = res_; }
    setupFilter(v);
    if (v.fading && v.fadeVol == 0) v.active = false;
  }

  void allTick() {
    voiceTick(host_, true);
    for (Voice& b : back_) voiceTick(b, false);
    back_.erase(std::remove_if(back_.begin(), back_.end(), [](const Voice& b) { return !b.active; }), back_.end());
  }

  // One song tick of the channel.
  void tick(const float* params, const int* slots) {
    const std::int64_t stamp = clock_ ? clock_->stamps[static_cast<std::size_t>(slots[kFx])] : 0;
    if (stamp != lastFx_) {
      lastFx_ = stamp;
      const int word = jsRoundInt(params[slots[kFx]]);
      cmd_ = (word >> 8) & 0x3f; par_ = word & 255; tickInRow_ = 0; noteCutAt_ = -1; panbrelloOut_ = 0;
      if (cmd_ != 9) tremorOn_ = true;
      const std::int64_t insStamp = clock_ ? clock_->stamps[static_cast<std::size_t>(slots[kIns])] : 0;
      const std::int64_t vStamp = clock_ ? clock_->stamps[static_cast<std::size_t>(slots[kVcol])] : 0;
      const bool insWritten = insStamp != lastIns_ && insStamp != 0;
      const bool vWritten = vStamp != lastVcol_ && vStamp != 0;
      lastIns_ = insStamp; lastVcol_ = vStamp;
      vcol_ = vWritten ? jsRoundInt(params[slots[kVcol]]) : -1;
      Row row;
      row.note = pendingNote_; row.off = pendingOff_; row.cutNote = pendingCut_; row.insWritten = insWritten;
      row.insValue = std::max(0, jsRoundInt(params[slots[kIns]])); row.v = vcol_;
      pendingNote_ = -1; pendingOff_ = false; pendingCut_ = false; delayedRow_.reset();
      const int sParam = par_ ? par_ : memS_;
      if (cmd_ == 19 && (sParam >> 4) == 13 && (sParam & 15) > 0) { if (par_) memS_ = par_; delayedRow_ = row; }   // SDx: the row starts later
      else startRow(row);
      effectFirstTick();
      host_.outPeriod = host_.period; host_.outVol = host_.vol;
    } else {
      tickInRow_++;
      host_.outPeriod = host_.period; host_.outVol = host_.vol; panbrelloOut_ = cmd_ == 25 ? panbrelloOut_ : 0;
      volumeColumnLaterTick(vcol_);
      effectLaterTick(tickInRow_);
      if (cmd_ != 18) host_.outVol = host_.vol;
    }
    allTick();
  }

  // One input frame of a sample channel at position q: interpolated, with the loop's wrap-around.
  struct Reader {
    const std::vector<float>& a;
    bool interpolate, pingpong, looping;
    double ls, le, span, top, len;
    double operator()(double q) const {
      const double k = std::floor(q);
      const double s0 = k >= 0 && k < static_cast<double>(a.size()) ? static_cast<double>(a[static_cast<std::size_t>(k)]) : 0.0;
      if (!interpolate) return s0;
      double k2 = k + 1;
      if (pingpong) k2 = std::min(top, k2);
      else if (looping && k2 >= le) k2 = ls + std::fmod(k2 - ls, span);
      const double next = k2 >= 0 && k2 < len && k2 < static_cast<double>(a.size()) ? static_cast<double>(a[static_cast<std::size_t>(k2)]) : 0.0;
      return s0 + (next - s0) * (q - k);
    }
  };

  // The sound of one voice into the block.
  void mixVoice(Voice& v, bool isHost, int n, float* outL, float* outR, double level, bool interpolate) {
    const Bank* S = bankAt(v.slot);
    if (!v.active || !S) { v.gain = 0; return; }
    const std::vector<float>& sample = S->msg->data;
    const double len = static_cast<double>(sample.size());
    const Instrument* I = instrumentOf(v);
    const double pitch = clampPeriod(v.outPeriod - v.pitchOff + (v.autoDelta ? (linear_ ? -v.autoDelta : v.autoDelta) : 0));
    const double inc = rateOf(pitch, linear_) / sampleRate_;
    double pan = v.pan + (I ? (v.note - I->ppc) * I->pps / 8 : 0);
    pan += panbrelloOut_ * (isHost ? 1 : 0);
    const int panned = clampPan(pan + (v.envPan - 32) * (32 - std::abs(pan - 32)) / 32);
    const double gl = std::min(1.0, (64 - panned) / 32.0), gr = std::min(1.0, panned / 32.0);
    const int outVol = isHost ? v.outVol : v.vol;
    const double goal = (isHost && !tremorOn_) ? 0
        : outVol / 64.0 * chanVol_ / 64 * S->globalVolume * (I ? I->gbv / 128 : 1.0) * v.envVol / 64 * v.fadeVol / kFadeFull * level;
    const bool useSus = v.keyOn && S->sus, mainLoop = S->loop;
    const double ls = useSus ? S->ss : S->ls, le = useSus ? S->se : S->le;
    const bool looping = useSus || mainLoop;
    const bool pingpong = looping && (useSus ? S->spp : S->pp);
    const double span = le - ls, top = le - 1;
    if (!pingpong) v.dir = 1;
    const bool stereo = it3_ && !S->msg->right.empty();
    const std::vector<float>& rightData = S->msg->right;
    const double xf = it3_ && looping && !pingpong ? (useSus ? S->sxf : S->xf) : 0;
    const Reader readLeft{sample, interpolate, pingpong, looping, ls, le, span, top, len};
    const Reader readRight{rightData, interpolate, pingpong, looping, ls, le, span, top, len};
    double p = v.pos;
    const bool filterOn = v.filterOn;
    const double fg = v.fg, fb0 = v.fb0, fb1 = v.fb1;
    double* y = v.y;
    for (int i = 0; i < n; i++) {
      if (pingpong) {
        if (v.dir > 0 && p >= top) {
          p = top - std::fmod(p - top, 2 * (top - ls)); v.dir = -1;
          if (p < ls) { p = 2 * ls - p; v.dir = 1; }
        } else if (v.dir < 0 && p <= ls) {
          p = ls + std::fmod(ls - p, 2 * (top - ls)); v.dir = 1;
          if (p > top) { p = 2 * top - p; v.dir = -1; }
        }
      } else if (looping) {
        if (p >= le) p = ls + std::fmod(p - ls, span);
      } else if (p >= len) { v.active = false; break; }
      double sl = readLeft(p), sr = stereo ? readRight(p) : sl;
      if (xf > 0 && p >= le - xf) {   // inside the crossfade the loop's tail blends into the audio before its start
        const double t = (p - (le - xf)) / xf, wa = js::cos(t * kPi / 2), wb = js::sin(t * kPi / 2);
        sl = sl * wa + readLeft(p - span) * wb;
        if (stereo) sr = sr * wa + readRight(p - span) * wb;
      }
      v.gain += (goal - v.gain) * 0.02;
      double l = sl * v.gain * gl, r = sr * v.gain * gr;
      if (filterOn) {
        const double fl = l * fg + y[0] * fb0 + y[1] * fb1; y[1] = y[0]; y[0] = fl; l = fl;
        const double fr = r * fg + y[2] * fb0 + y[3] * fb1; y[3] = y[2]; y[2] = fr; r = fr;
      }
      outL[i] = static_cast<float>(static_cast<double>(outL[i]) + l);
      outR[i] = static_cast<float>(static_cast<double>(outR[i]) + r);
      p += inc * v.dir;
    }
    v.pos = p;
  }

  double sampleRate_;
  const Clock* clock_;
  bool it3_ = false, linear_ = true, oldEffects_ = false, compatG_ = true, hasInstruments_ = false;
  std::vector<std::optional<Instrument>> instruments_;
  std::vector<Extra> extras_;
  std::vector<std::unique_ptr<Bank>> bank_;
  std::uint32_t dice_ = 0x2545f491;

  // channel state
  int chanVol_ = 64, chanPan_ = 32, cut_ = 127, res_ = 0, curIns_ = 0;
  Voice host_;
  std::vector<Voice> back_;
  // effect memories
  int memD_ = 0, memEF_ = 0, memG_ = 0, memI_ = 0, memJ_ = 0, memN_ = 0, memO_ = 0, memP_ = 0, memQ_ = 0, memS_ = 0, memHigh_ = 0, memVol_ = 0;
  int vibWave_ = 0, tremWave_ = 0, panWave_ = 0, vibPos_ = 0, vibSpeed_ = 0, vibDepth_ = 0, tremPos_ = 0, tremSpeed_ = 0, tremDepth_ = 0;
  int panPos_ = 0, panSpeed_ = 0, panDepth_ = 0;
  int tremorCount_ = 0, retrigCount_ = 0, nnaOverride_ = -1, panbrelloOut_ = 0;
  bool tremorOn_ = true;
  bool envOverride_[3] = {true, true, true};   // volume, panning and pitch envelopes on (S77 to S7C)
  // the row
  int cmd_ = 0, par_ = 0, vcol_ = 0, tickInRow_ = 0, noteCutAt_ = -1;
  std::optional<Row> delayedRow_;
  // what came in since the last tick, and what has been seen
  int pendingNote_ = -1;
  bool pendingOff_ = false, pendingCut_ = false;
  int lastTick_ = 0;
  std::int64_t lastFx_ = -1, lastIns_ = -1, lastVcol_ = -1;
  double idleSamples_ = 0;
};

}  // namespace

void registerItChan(NodeRegistry& registry) {
  NodeType type;
  type.type = "itchan";
  type.inputs = {};
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.shared = true;
  type.params = {{"ins", 0, 255, 0, true}, {"vcol", 0, 255, 0, true}, {"fx", 0, 0x1aff, 0, true}, {"level", 0, 1, 0.45}, {"interp", 0, 1, 1, true}};
  type.create = [](const NodeContext& context) { return std::make_unique<ItChan>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

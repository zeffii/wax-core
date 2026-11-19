// One FastTracker 2 channel: an XM playroutine for XM files and XM-style patterns. It plays instruments, not bare samples: an
// instrument maps each note to one of its samples (the keymap), and has a volume and a panning envelope (points, a sustain
// point, a loop), a fadeout after the key is let go, and an automatic vibrato. Port of webtracker src/audio/nodes/xmchan.js;
// the pattern cells and the effects are described there.
//
// Timing works as for the MOD channel: the song's tick is the XM tick, a row starts when the Cmd column writes `fx`
// (command 01), and the note, the Ins cell and the Vol cell of that tick come with it.
#include <algorithm>
#include <cmath>
#include <cstdint>
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
constexpr int kC4 = 48;                // C-4 counted in semitones from C-0: the note a sample plays at its own rate
constexpr int kMidiC0 = 12;            // a note's MIDI number for C-0
constexpr double kMaxPeriod = 32000;
constexpr double kFadeFull = 32768;    // the fadeout volume at the start of a note

// The period of a note (semitones from C-0, a fraction allowed) and the playback rate a period stands for.
double periodOf(double note, bool linear) { return linear ? 7680 - note * 64 : 1712 * js::pow(2, (kC4 - note) / 12); }
double rateOf(double period, bool linear) { return linear ? 8363 * js::pow(2, (4608 - period) / 768) : 8363 * 1712 / period; }

int waveOf(int kind, int pos) {   // 0..255 for a position 0..63 in the cycle (the half is in bit 5)
  const int w = kind & 3, p = pos & 31;
  if (w == 0) return kSine[p];
  if (w == 1) return (pos & 32) ? 255 - p * 8 : p * 8;   // ramp
  return 255;                                              // square
}

int autoWave(int type, int pos) {   // -64..64 for a position 0..255: sine, square, ramp down, ramp up
  if (type == 1) return pos < 128 ? 64 : -64;
  if (type == 2) return 64 - (pos >> 1);
  if (type == 3) return (pos >> 1) - 64;
  return jsRoundInt(js::sin(pos / 128.0 * 3.141592653589793) * 64);
}

// Multi retrigger: what each volume nibble does to the volume.
double retriggerVolume(int nibble, double v) {
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

// points: ticks and values, flat (x0, y0, x1, y1, ...); sus, ls and le are point numbers or -1.
struct Envelope {
  std::vector<double> pts;
  double sus = -1, ls = -1, le = -1;
};

struct Instrument {
  std::vector<int> map;      // the sample slot of each note from C-0 (-1: none); one entry means every note
  bool mapIsList = false;
  int fade = 0;
  double vib[4] = {0, 0, 0, 0};   // type, sweep, depth, rate
  std::optional<Envelope> vol, pan;
};

// An envelope's value at a tick, and the tick after it (held on the sustain point while the key is down, and taken back to
// the loop start at the loop end).
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

double envNext(const Envelope& env, double pos, bool keyOn) {
  const std::vector<double>& p = env.pts;
  if (keyOn && env.sus >= 0 && pos == p[static_cast<std::size_t>(env.sus) * 2]) return pos;
  double next = pos + 1;
  if (env.ls >= 0 && env.le >= 0 && next >= p[static_cast<std::size_t>(env.le) * 2]) next = p[static_cast<std::size_t>(env.ls) * 2];
  return next;
}

double numberAt(const Json& j, const char* key, double fallback) {
  return j.contains(key) && j[key].is_number() ? j[key].get<double>() : fallback;
}

std::optional<Envelope> envelopeOf(const Json& j, const char* key) {
  if (!j.contains(key) || !j[key].is_object()) return std::nullopt;
  const Json& e = j[key];
  if (!e.contains("pts") || !e["pts"].is_array() || e["pts"].size() < 2) return std::nullopt;
  Envelope out;
  for (const auto& v : e["pts"]) out.pts.push_back(v.is_number() ? v.get<double>() : 0.0);
  out.sus = numberAt(e, "sus", -1);
  out.ls = numberAt(e, "ls", -1);
  out.le = numberAt(e, "le", -1);
  return out;
}

struct Bank {
  std::shared_ptr<const SampleMessage> msg;
  double volume = 1, pan = 0;
  double tune = 0;   // semitones from C-4 at 8363 Hz
  bool loop = false, pingpong = false;
  double ls = 0, le = 0;
};

// What a row's first tick is told: the note (or key off) that came with it, the instrument and the volume column byte.
struct Row {
  int note = -1;
  bool off = false, insWritten = false;
  int insValue = 0, v = 0;
};

class XmChan final : public Node {
public:
  explicit XmChan(const NodeContext& context) : sampleRate_(context.sampleRate), clock_(context.clock) {
    period_ = outPeriod_ = target_ = periodOf(kC4, true);
    if (context.data) readData(*context.data);
  }

  void data(const Json& data) override { readData(data); }

  void sample(const std::shared_ptr<const SampleMessage>& message) override {
    const std::size_t slot = static_cast<std::size_t>(std::max(0, message->slot));
    if (bank_.size() <= slot) bank_.resize(slot + 1);
    auto S = std::make_unique<Bank>();
    S->msg = message;
    S->volume = message->volume;
    S->pan = std::max(-1.0, std::min(1.0, message->pan));
    S->tune = 12 * js::log2(std::max(1.0, message->rate) / 8363);
    S->pingpong = message->pingpong;
    const double start = message->loopStart, end = message->loopEnd;
    if (message->loopOn && end - start >= 2 && end <= static_cast<double>(message->data.size()) && start >= 0) { S->loop = true; S->ls = start; S->le = end; }
    bank_[slot] = std::move(S);
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { pendingNote_ = event.note - kMidiC0; pendingOff_ = false; }
    else if (event.type == NoteType::Off && !event.implied) { pendingOff_ = true; if (!(clock_ && clock_->playing)) keyOff(); }
    else if (event.type == NoteType::AllOff || event.type == NoteType::Cut) { silence(); cmd_ = par_ = vcol_ = 0; lastFx_ = -1; }
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
        ins_ = std::max(0, jsRoundInt(params[slots[kIns]]));
        cmd_ = par_ = vcol_ = 0;
        if (trigger(pendingNote_, 0)) takeSampleDefaults();
        outPeriod_ = period_; outVol_ = vol_; pendingNote_ = -1; pendingOff_ = false;
        idleSamples_ = 0; instrumentTick();
      }
      idleSamples_ += n;   // the envelopes keep time on their own, so an instrument can be heard as it will play
      const double tickSamples = clock_ && clock_->tickSamples != 0 ? clock_->tickSamples : sampleRate_ / 50;
      while (idleSamples_ >= tickSamples) { idleSamples_ -= tickSamples; instrumentTick(); }
    }
    std::fill(outL, outL + n, 0.0f);
    std::fill(outR, outR + n, 0.0f);
    const Bank* S = active_ ? bankAt(cur_) : nullptr;
    if (!S) { gain_ = 0; return; }
    const std::vector<float>& sample = S->msg->data;
    const double len = static_cast<double>(sample.size());
    const double inc = rateOf(clampPeriod(outPeriod_ + autoDelta_), linear_) / sampleRate_;
    const double p0 = clampPan(pan_ + (envPan_ - 32) * (128 - std::abs(pan_ - 128)) / 32);
    const double gl = std::min(1.0, (255 - p0) / 127.5), gr = std::min(1.0, p0 / 127.5);
    const double level = params[slots[kLevel]];
    const double goal = tremorOn_ ? outVol_ / 64.0 * envVol_ / 64 * fadeVol_ / kFadeFull * level : 0;
    const bool interpolate = params[slots[kInterp]] >= 0.5;
    const bool loop = S->loop;
    const double ls = S->ls, le = S->le, span = le - ls;
    const bool pingpong = loop && S->pingpong;
    const double top = le - 1;   // a ping-pong loop turns on its last frame and on its first
    double p = pos_;
    for (int i = 0; i < n; i++) {
      if (pingpong) {
        if (dir_ > 0 && p >= top) {
          p = top - std::fmod(p - top, 2 * (top - ls)); dir_ = -1;
          if (p < ls) { p = 2 * ls - p; dir_ = 1; }
        } else if (dir_ < 0 && p <= ls) {
          p = ls + std::fmod(ls - p, 2 * (top - ls)); dir_ = 1;
          if (p > top) { p = 2 * top - p; dir_ = -1; }
        }
      } else if (loop) {
        if (p >= le) p = ls + std::fmod(p - ls, span);
      } else if (p >= len) { active_ = false; break; }
      const int k = static_cast<int>(std::floor(p));
      double v = sample[static_cast<std::size_t>(k)];
      if (interpolate) {
        double k2 = k + 1;
        if (pingpong) k2 = std::min(top, k2);
        else if (loop && k2 >= le) k2 = ls + std::fmod(k2 - ls, span);
        const double next = k2 < len ? static_cast<double>(sample[static_cast<std::size_t>(k2)]) : 0.0;
        v += (next - v) * (p - k);
      }
      gain_ += (goal - gain_) * 0.02;
      outL[i] = static_cast<float>(v * gain_ * gl);
      outR[i] = static_cast<float>(v * gain_ * gr);
      p += inc * dir_;
    }
    pos_ = p;
  }

private:
  void readData(const Json& d) {
    linear_ = !(d.is_object() && d.contains("linear") && d["linear"].is_boolean() && !d["linear"].get<bool>());
    instruments_.clear();
    if (!d.is_object() || !d.contains("ins") || !d["ins"].is_array()) return;
    for (const auto& i : d["ins"]) {
      if (!i.is_object()) { instruments_.push_back(std::nullopt); continue; }
      Instrument I;
      if (i.contains("map") && i["map"].is_array()) {
        I.mapIsList = true;
        for (const auto& m : i["map"]) I.map.push_back(m.is_number() ? static_cast<int>(m.get<double>()) : -1);
      } else if (i.contains("map") && i["map"].is_number()) I.map.push_back(static_cast<int>(i["map"].get<double>()));
      I.fade = static_cast<int>(numberAt(i, "fade", 0));
      if (i.contains("vib") && i["vib"].is_array()) {
        for (std::size_t j = 0; j < 4 && j < i["vib"].size(); j++) I.vib[j] = i["vib"][j].is_number() ? i["vib"][j].get<double>() : 0;
      }
      I.vol = envelopeOf(i, "vol");
      I.pan = envelopeOf(i, "pan");
      instruments_.push_back(std::move(I));
    }
  }

  const Bank* bankAt(int i) const { return i >= 0 && i < static_cast<int>(bank_.size()) ? bank_[static_cast<std::size_t>(i)].get() : nullptr; }
  const Instrument* instrument() const {
    return ins_ >= 0 && ins_ < static_cast<int>(instruments_.size()) && instruments_[static_cast<std::size_t>(ins_)] ? &*instruments_[static_cast<std::size_t>(ins_)] : nullptr;
  }
  static int clampVol(double v) { return std::max(0, std::min(64, jsRoundInt(v))); }
  static int clampPan(double v) { return std::max(0, std::min(255, jsRoundInt(v))); }
  static double clampPeriod(double v) { return std::max(1.0, std::min(kMaxPeriod, v)); }

  // The sample slot instrument i plays for note n (semitones from C-0), or -1.
  int slotFor(int i, int n) const {
    if (i < 0 || i >= static_cast<int>(instruments_.size()) || !instruments_[static_cast<std::size_t>(i)]) return instruments_.empty() ? i : -1;   // no data: instrument n is slot n
    const Instrument& I = *instruments_[static_cast<std::size_t>(i)];
    if (I.map.empty()) return -1;
    if (!I.mapIsList) return I.map[0];
    const std::size_t at = static_cast<std::size_t>(std::max(0, std::min(95, n)));
    return at < I.map.size() ? I.map[at] : -1;
  }
  double sampleNote(double n) const {
    const Bank* S = bankAt(cur_);
    return n + (S ? S->tune : 0);   // the note with the sample's own tuning added
  }

  void silence() { active_ = false; pendingNote_ = -1; pendingOff_ = false; delayedRow_.reset(); }
  void resetEnvelopes() {
    keyOn_ = true; fadeVol_ = kFadeFull; volEnvPos_ = panEnvPos_ = 0; autoPos_ = 0; autoAmp_ = 0;
    if ((vibWave_ & 4) == 0) vibPos_ = 0;
    if ((tremWave_ & 4) == 0) tremPos_ = 0;
    retrigCount_ = 0; tremorCount_ = 0; tremorOn_ = true;
  }
  void takeSampleDefaults() {   // an instrument number on the row: the volume and panning of its sample
    const Bank* S = bankAt(cur_);
    if (!S) return;
    vol_ = clampVol(S->volume * 64);
    pan_ = clampPan(S->pan * 128 + 128);
  }
  void keyOff() {
    keyOn_ = false;
    const Instrument* I = instrument();
    if (!(I && I->vol)) vol_ = 0;   // without a volume envelope a key off is a cut
  }
  bool trigger(int n, double offset) {   // plays note n (semitones from C-0) on the instrument
    const int slot = slotFor(ins_, n);
    if (slot < 0 || !bankAt(slot)) { active_ = false; return false; }
    cur_ = slot;
    note_ = n;
    period_ = outPeriod_ = target_ = clampPeriod(periodOf(sampleNote(n), linear_));
    pos_ = offset; dir_ = 1;
    active_ = pos_ < static_cast<double>(bankAt(cur_)->msg->data.size());
    resetEnvelopes();
    return true;
  }

  // The row's first tick: the note, the instrument, the volume column and the effect's first move.
  void startRow(const Row& row) {
    const int n = row.note, v = row.v;
    const bool porta = cmd_ == 3 || cmd_ == 5 || (v >> 4) == 0xF;
    if (row.insWritten) ins_ = row.insValue;
    if (row.off) keyOff();
    else if (n >= 0) {
      if (porta && active_) {
        target_ = clampPeriod(periodOf(sampleNote(n), linear_));
        if (row.insWritten) { takeSampleDefaults(); resetEnvelopes(); }
      } else {
        if (cmd_ == 9 && par_) memOffset_ = par_;
        trigger(n, cmd_ == 9 ? memOffset_ * 256 : 0);
        if (row.insWritten) takeSampleDefaults();
      }
    } else if (row.insWritten) { takeSampleDefaults(); resetEnvelopes(); }
    volumeColumnFirstTick(v);
  }

  void volumeColumnFirstTick(int v) {
    const int hi = v >> 4, x = v & 15;
    if (v >= 0x10 && v <= 0x50) vol_ = v - 0x10;
    else if (hi == 0x8) vol_ = clampVol(vol_ - x);
    else if (hi == 0x9) vol_ = clampVol(vol_ + x);
    else if (hi == 0xA) vibSpeed_ = x;
    else if (hi == 0xB) { if (x) vibDepth_ = x; }
    else if (hi == 0xC) pan_ = x << 4;
    else if (hi == 0xF && x) portaSpeed_ = x << 6;
  }
  void volumeColumnLaterTick(int v) {
    const int hi = v >> 4, x = v & 15;
    if (hi == 0x6) vol_ = clampVol(vol_ - x);
    else if (hi == 0x7) vol_ = clampVol(vol_ + x);
    else if (hi == 0xB) vibrato();
    else if (hi == 0xD) pan_ = clampPan(pan_ - x);
    else if (hi == 0xE) pan_ = clampPan(pan_ + x);
    else if (hi == 0xF) slideToTarget();
  }

  void slideToTarget() {
    if (period_ > target_) period_ = std::max(target_, period_ - portaSpeed_);
    else if (period_ < target_) period_ = std::min(target_, period_ + portaSpeed_);
    outPeriod_ = period_;
  }
  void vibrato() {
    const int delta = (waveOf(vibWave_, vibPos_) * vibDepth_) >> 5;
    outPeriod_ = (vibPos_ & 32) ? period_ - delta : period_ + delta;
    vibPos_ = (vibPos_ + vibSpeed_) & 63;
  }
  void volumeSlide() { vol_ = clampVol(vol_ + ((memVol_ >> 4) ? memVol_ >> 4 : -(memVol_ & 15))); }
  void arpeggio(int t) {
    const int off = t % 3 == 0 ? 0 : t % 3 == 1 ? par_ >> 4 : par_ & 15;
    outPeriod_ = linear_ ? period_ - off * 64 : period_ * js::pow(2, -off / 12.0);
  }

  void effectFirstTick() {
    const int x = par_ & 15, sub = par_ >> 4;
    switch (cmd_) {
      case 1: if (par_) mem1_ = par_; break;
      case 2: if (par_) mem2_ = par_; break;
      case 3: if (par_) portaSpeed_ = par_ << 2; break;
      case 4: if (sub) vibSpeed_ = sub; if (x) vibDepth_ = x; break;
      case 5: case 6: case 0xA: if (par_) memVol_ = par_; break;
      case 7: if (sub) tremSpeed_ = sub; if (x) tremDepth_ = x; break;
      case 8: pan_ = par_; break;
      case 0xC: vol_ = clampVol(par_); break;
      case 0xE:
        if (sub == 1) { if (x) memE1_ = x; period_ = clampPeriod(period_ - memE1_ * 4); }
        else if (sub == 2) { if (x) memE2_ = x; period_ = clampPeriod(period_ + memE2_ * 4); }
        else if (sub == 4) vibWave_ = x;
        else if (sub == 7) tremWave_ = x;
        else if (sub == 0xA) { if (x) memFineUp_ = x; vol_ = clampVol(vol_ + memFineUp_); }
        else if (sub == 0xB) { if (x) memFineDown_ = x; vol_ = clampVol(vol_ - memFineDown_); }
        else if (sub == 0xC && x == 0) vol_ = 0;
        break;
      case 0x14: if (par_ == 0) keyOff(); else keyOffAt_ = par_; break;     // K
      case 0x15: volEnvPos_ = panEnvPos_ = par_; break;                      // L
      case 0x19: if (par_) memPan_ = par_; break;                            // P
      case 0x1B: if (sub) retrigVol_ = sub; if (x) retrigRate_ = x; break;   // R
      case 0x1D: if (par_) tremorMem_ = par_; break;                         // T
      case 0x21:                                                             // X
        if (sub == 1) { if (x) memX1_ = x; period_ = clampPeriod(period_ - memX1_); }
        else if (sub == 2) { if (x) memX2_ = x; period_ = clampPeriod(period_ + memX2_); }
        break;
      default: break;
    }
  }

  void effectLaterTick(int t) {
    const int x = par_ & 15, sub = par_ >> 4;
    switch (cmd_) {
      case 0: if (par_) arpeggio(t); break;
      case 1: period_ = clampPeriod(period_ - mem1_ * 4); outPeriod_ = period_; break;
      case 2: period_ = clampPeriod(period_ + mem2_ * 4); outPeriod_ = period_; break;
      case 3: slideToTarget(); break;
      case 4: vibrato(); break;
      case 5: slideToTarget(); volumeSlide(); break;
      case 6: vibrato(); volumeSlide(); break;
      case 7: {
        const int delta = (waveOf(tremWave_, tremPos_) * tremDepth_) >> 6;
        outVol_ = clampVol((tremPos_ & 32) ? vol_ - delta : vol_ + delta);
        tremPos_ = (tremPos_ + tremSpeed_) & 63;
        break;
      }
      case 0xA: volumeSlide(); break;
      case 0xE:
        if (sub == 9 && x && t % x == 0) { pos_ = 0; dir_ = 1; active_ = bankAt(cur_) != nullptr; }
        else if (sub == 0xC && t == x) vol_ = 0;
        else if (sub == 0xD && delayedRow_ && t == x) { const Row row = *delayedRow_; delayedRow_.reset(); startRow(row); }
        break;
      case 0x14: if (t == keyOffAt_) keyOff(); break;
      case 0x19: pan_ = clampPan((memPan_ >> 4) ? pan_ + (memPan_ >> 4) : pan_ - (memPan_ & 15)); break;
      case 0x1B:
        if (retrigRate_ && ++retrigCount_ >= retrigRate_) {
          retrigCount_ = 0; pos_ = 0; dir_ = 1; active_ = bankAt(cur_) != nullptr;
          vol_ = clampVol(retriggerVolume(retrigVol_, vol_));
        }
        break;
      case 0x1D: {
        const int on = (tremorMem_ >> 4) + 1, off = (tremorMem_ & 15) + 1;
        tremorOn_ = tremorCount_ % (on + off) < on;
        tremorCount_++;
        break;
      }
      default: break;
    }
  }

  // Every tick, playing or not: the envelopes, the fadeout and the automatic vibrato move on.
  void instrumentTick() {
    const Instrument* I = instrument();
    envVol_ = 64; envPan_ = 32; autoDelta_ = 0;
    if (!I) return;
    if (I->vol) { envVol_ = envValue(*I->vol, volEnvPos_); volEnvPos_ = envNext(*I->vol, volEnvPos_, keyOn_); }
    if (I->pan) { envPan_ = envValue(*I->pan, panEnvPos_); panEnvPos_ = envNext(*I->pan, panEnvPos_, keyOn_); }
    if (!keyOn_ && I->vol) fadeVol_ = std::max(0.0, fadeVol_ - I->fade);
    const int type = static_cast<int>(I->vib[0]);
    const double sweep = I->vib[1];
    const int depth = static_cast<int>(I->vib[2]), rate = static_cast<int>(I->vib[3]);
    if (depth && rate) {
      const double full = static_cast<double>(depth << 8);
      autoAmp_ = sweep != 0 ? std::min(full, autoAmp_ + full / sweep) : full;
      autoDelta_ = static_cast<int>(std::trunc(autoWave(type, autoPos_) * autoAmp_)) >> 14;
      autoPos_ = (autoPos_ + rate) & 255;
    }
  }

  // One song tick of the channel.
  void tick(const float* params, const int* slots) {
    const std::int64_t stamp = clock_ ? clock_->stamps[static_cast<std::size_t>(slots[kFx])] : 0;
    if (stamp != lastFx_) {
      lastFx_ = stamp;
      const int word = jsRoundInt(params[slots[kFx]]);
      cmd_ = (word >> 8) & 0x3f; par_ = word & 255; tickInRow_ = 0; keyOffAt_ = -1;
      const std::int64_t insStamp = clock_ ? clock_->stamps[static_cast<std::size_t>(slots[kIns])] : 0;
      const std::int64_t vStamp = clock_ ? clock_->stamps[static_cast<std::size_t>(slots[kVcol])] : 0;
      const bool insWritten = insStamp != lastIns_ && insStamp != 0;
      const bool vWritten = vStamp != lastVcol_ && vStamp != 0;
      lastIns_ = insStamp; lastVcol_ = vStamp;
      vcol_ = vWritten ? jsRoundInt(params[slots[kVcol]]) : 0;
      Row row;
      row.note = pendingNote_; row.off = pendingOff_; row.insWritten = insWritten;
      row.insValue = std::max(0, jsRoundInt(params[slots[kIns]])); row.v = vcol_;
      pendingNote_ = -1; pendingOff_ = false; delayedRow_.reset();
      if (cmd_ == 0xE && (par_ >> 4) == 0xD && (par_ & 15) > 0) delayedRow_ = row;   // ED: the row starts later
      else startRow(row);
      effectFirstTick();
      outPeriod_ = period_; outVol_ = vol_;
    } else {
      tickInRow_++;
      outPeriod_ = period_; outVol_ = vol_;
      volumeColumnLaterTick(vcol_);
      effectLaterTick(tickInRow_);
      if (cmd_ != 7) outVol_ = vol_;
    }
    instrumentTick();
  }

  double sampleRate_;
  const Clock* clock_;
  bool linear_ = true;
  std::vector<std::optional<Instrument>> instruments_;
  std::vector<std::unique_ptr<Bank>> bank_;

  // what is playing
  bool active_ = false;
  double pos_ = 0;
  int dir_ = 1, cur_ = 0, ins_ = 0, note_ = kC4;
  double period_ = 0, outPeriod_ = 0;
  int vol_ = 64, outVol_ = 64, pan_ = 128;
  double gain_ = 0;
  bool keyOn_ = true;
  double fadeVol_ = kFadeFull, volEnvPos_ = 0, panEnvPos_ = 0;
  int autoPos_ = 0;
  double autoAmp_ = 0;
  // the row
  int cmd_ = 0, par_ = 0, vcol_ = 0, tickInRow_ = 0, keyOffAt_ = -1;
  std::optional<Row> delayedRow_;
  // effect memories (FT2 keeps one for each)
  int mem1_ = 0, mem2_ = 0, memE1_ = 0, memE2_ = 0, memX1_ = 0, memX2_ = 0, memVol_ = 0, memFineUp_ = 0, memFineDown_ = 0, memPan_ = 0, memOffset_ = 0;
  double target_ = 0;
  int portaSpeed_ = 0, vibSpeed_ = 0, vibDepth_ = 0, vibPos_ = 0, vibWave_ = 0;
  int tremSpeed_ = 0, tremDepth_ = 0, tremPos_ = 0, tremWave_ = 0, retrigVol_ = 0, retrigRate_ = 0, retrigCount_ = 0, tremorMem_ = 0, tremorCount_ = 0;
  bool tremorOn_ = true;
  // what the instrument's envelopes make of it this tick
  double envVol_ = 64, envPan_ = 32;
  int autoDelta_ = 0;
  // what came in since the last tick, and what has been seen
  int pendingNote_ = -1;
  bool pendingOff_ = false;
  int lastTick_ = 0;
  std::int64_t lastFx_ = -1, lastIns_ = -1, lastVcol_ = -1;
  double idleSamples_ = 0;
};

}  // namespace

void registerXmChan(NodeRegistry& registry) {
  NodeType type;
  type.type = "xmchan";
  type.inputs = {};
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.shared = true;
  type.params = {{"ins", 0, 255, 0, true}, {"vcol", 0, 255, 0, true}, {"fx", 0, 0x23ff, 0, true}, {"level", 0, 1, 0.45}, {"interp", 0, 1, 1, true}};
  type.create = [](const NodeContext& context) { return std::make_unique<XmChan>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

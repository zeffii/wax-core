// One Amiga tracker channel: a ProTracker playroutine for MOD files and MOD-style patterns (arpeggio, slides, tone portamento,
// vibrato, tremolo, sample offset, volume slides, the E-effects). Port of webtracker src/audio/nodes/modchan.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kSmp = 0, kFx, kLevel, kPan, kInterp };
const int kPeriods[36] = {856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453, 428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226, 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113};
constexpr int kFirstNote = 24;
const int kSine[32] = {0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253, 255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24};
constexpr double kPaula = 3579545;
constexpr int kMinPeriod = 113, kMaxPeriod = 856;

int periodOf(int note) {
  const int i = note - kFirstNote;
  return i >= 0 && i < 36 ? kPeriods[i] : std::max(20, std::min(3000, jsRoundInt(856 * js::pow(2, -i / 12.0))));
}

int waveOf(int kind, int pos) {
  const int w = kind & 3, p = pos & 31;
  if (w == 0) return kSine[p];
  if (w == 1) return (pos & 32) ? 255 - p * 8 : p * 8;
  return 255;
}

struct Bank {
  std::shared_ptr<const SampleMessage> msg;
  double rate = 8363, volume = 1;
  bool loop = false;
  double ls = 0, le = 0;
};

class ModChan final : public Node {
public:
  explicit ModChan(const NodeContext& context) : sampleRate_(context.sampleRate), clock_(context.clock) {}

  void sample(const std::shared_ptr<const SampleMessage>& message) override {
    const std::size_t slot = static_cast<std::size_t>(std::max(0, message->slot));
    if (bank_.size() <= slot) bank_.resize(slot + 1);
    auto S = std::make_unique<Bank>();
    S->msg = message;
    S->rate = message->rate;
    S->volume = message->volume;
    const double start = message->loopStart, end = message->loopEnd;
    if (message->loopOn && end - start >= 2 && end <= static_cast<double>(message->data.size()) && start >= 0) { S->loop = true; S->ls = start; S->le = end; }
    bank_[slot] = std::move(S);
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) pendingNote_ = event.note;
    else if (event.type == NoteType::AllOff || event.type == NoteType::Cut) {
      silence(); cmd_ = par_ = 0; hasPan_ = false; lastFx_ = -1;
    }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    const bool playing = clock_ && clock_->playing;
    const int id = clock_ ? clock_->tickId : 0;
    if (playing && id != lastTick_) { lastTick_ = id; tick(params, slots); }
    else if (pendingNote_ >= 0 && !playing) {
      cur_ = std::max(0, jsRoundInt(params[slots[kSmp]]));
      cmd_ = par_ = 0;
      const Bank* S0 = bankAt(cur_);
      if (S0) vol_ = outVol_ = clampVol(jsRoundInt(S0->volume * 64));
      trigger(pendingNote_);
      pendingNote_ = -1;
    }
    if (!playing) lastTick_ = id;
    std::fill(outL, outL + n, 0.0f);
    std::fill(outR, outR + n, 0.0f);
    const Bank* S = active_ ? bankAt(cur_) : nullptr;
    if (!S) { gain_ = 0; return; }
    const std::vector<float>& sample = S->msg->data;
    const double len = static_cast<double>(sample.size());
    const double inc = kPaula / std::max(20, outPeriod_) * (S->rate / 8363) * ftMul_ / sampleRate_;
    const double pan = std::max(-1.0, std::min(1.0, hasPan_ ? panFx_ : static_cast<double>(params[slots[kPan]])));
    const double level = params[slots[kLevel]], targetGain = outVol_ / 64.0 * level;
    const double gl = pan > 0 ? 1 - pan : 1, gr = pan < 0 ? 1 + pan : 1;
    const bool linear = params[slots[kInterp]] >= 0.5;
    const bool loop = S->loop;
    const double ls = S->ls, le = S->le, span = le - ls;
    double p = pos_;
    for (int i = 0; i < n; i++) {
      if (loop) { if (p >= le) p = ls + std::fmod(p - ls, span); }
      else if (p >= len) { active_ = false; break; }
      const int k = static_cast<int>(std::floor(p));
      double v = sample[static_cast<std::size_t>(k)];
      if (linear) {
        double k2 = k + 1;
        if (loop && k2 >= le) k2 = ls + std::fmod(k2 - ls, span);
        const double next = k2 < len ? static_cast<double>(sample[static_cast<std::size_t>(k2)]) : 0.0;
        v += (next - v) * (p - k);
      }
      gain_ += (targetGain - gain_) * 0.02;
      outL[i] = static_cast<float>(v * gain_ * gl);
      outR[i] = static_cast<float>(v * gain_ * gr);
      p += inc;
    }
    pos_ = p;
  }

private:
  const Bank* bankAt(int i) const { return i >= 0 && i < static_cast<int>(bank_.size()) ? bank_[static_cast<std::size_t>(i)].get() : nullptr; }
  static int clampVol(int v) { return std::max(0, std::min(64, v)); }
  void silence() { active_ = false; pendingNote_ = delayed_ = -1; }

  void trigger(int note) {
    const Bank* S = bankAt(cur_);
    noteIdx_ = note - kFirstNote;
    period_ = outPeriod_ = periodOf(note);
    pos_ = cmd_ == 9 ? offsetMem_ * 256 : 0;
    if ((vibWave_ & 4) == 0) vibPos_ = 0;
    if ((tremWave_ & 4) == 0) tremPos_ = 0;
    active_ = S && pos_ < static_cast<double>(S->msg->data.size());
  }
  void slide() {
    if (period_ > target_) period_ = std::max(target_, period_ - portaSpeed_);
    else if (period_ < target_) period_ = std::min(target_, period_ + portaSpeed_);
    outPeriod_ = period_;
  }
  void vibrato() {
    const int delta = (waveOf(vibWave_, vibPos_) * vibDepth_) >> 7;
    outPeriod_ = (vibPos_ & 32) ? period_ - delta : period_ + delta;
    vibPos_ = (vibPos_ + vibSpeed_) & 63;
  }
  void volSlide() { vol_ = clampVol(vol_ + ((par_ >> 4) ? par_ >> 4 : -(par_ & 15))); outVol_ = vol_; }

  void tick(const float* params, const int* slots) {
    const std::int64_t stamp = clock_ ? clock_->stamps[static_cast<std::size_t>(slots[kFx])] : 0;
    const std::int64_t smpStamp = clock_ ? clock_->stamps[static_cast<std::size_t>(slots[kSmp])] : 0;
    const bool newRow = stamp != lastFx_;
    if (newRow) {
      lastFx_ = stamp;
      const int word = jsRoundInt(params[slots[kFx]]);
      cmd_ = (word >> 8) & 15; par_ = word & 255; tickInRow_ = 0;
      const bool smpWritten = smpStamp != lastSmp_ && smpStamp != 0;
      lastSmp_ = smpStamp;
      if (smpWritten) { cur_ = std::max(0, jsRoundInt(params[slots[kSmp]])); ftMul_ = 1; }
      if (cmd_ == 9 && par_) offsetMem_ = par_;
      const int note = pendingNote_;
      pendingNote_ = -1; delayed_ = -1;
      const Bank* S = bankAt(cur_);
      if (smpWritten && S) vol_ = clampVol(jsRoundInt(S->volume * 64));
      if (cmd_ == 3 || cmd_ == 5) {
        if (cmd_ == 3 && par_) portaSpeed_ = par_;
        if (note >= 0) target_ = periodOf(note);
      } else if (note >= 0) {
        if (cmd_ == 0xE && (par_ >> 4) == 0xD && (par_ & 15) > 0) delayed_ = note;
        else trigger(note);
      }
      if (cmd_ == 4 || cmd_ == 6) {
        if ((par_ >> 4) && cmd_ == 4) vibSpeed_ = par_ >> 4;
        if ((par_ & 15) && cmd_ == 4) vibDepth_ = par_ & 15;
      }
      if (cmd_ == 7) { if (par_ >> 4) tremSpeed_ = par_ >> 4; if (par_ & 15) tremDepth_ = par_ & 15; }
      const int x = par_ & 15;
      if (cmd_ == 0xC) vol_ = clampVol(par_);
      else if (cmd_ == 8) { hasPan_ = true; panFx_ = (par_ - 128) / 128.0; }
      else if (cmd_ == 0xE) {
        const int sub = par_ >> 4;
        if (sub == 1) period_ = std::max(kMinPeriod, period_ - x);
        else if (sub == 2) period_ = std::min(kMaxPeriod, period_ + x);
        else if (sub == 4) vibWave_ = x;
        else if (sub == 5) ftMul_ = js::pow(2, (x > 7 ? x - 16 : x) / 96.0);
        else if (sub == 7) tremWave_ = x;
        else if (sub == 8) { hasPan_ = true; panFx_ = (x * 17 - 128) / 128.0; }
        else if (sub == 0xA) vol_ = clampVol(vol_ + x);
        else if (sub == 0xB) vol_ = clampVol(vol_ - x);
        else if (sub == 0xC && x == 0) vol_ = 0;
      }
      outPeriod_ = period_; outVol_ = vol_;
      return;
    }
    tickInRow_++;
    const int t = tickInRow_, x = par_ & 15;
    if (cmd_ != 0 && cmd_ != 4 && cmd_ != 6) outPeriod_ = period_;
    if (cmd_ != 7) outVol_ = vol_;
    switch (cmd_) {
      case 0:
        if (par_) {
          const int off = t % 3 == 0 ? 0 : t % 3 == 1 ? par_ >> 4 : x, i = noteIdx_ + off;
          outPeriod_ = i >= 0 && i < 36 ? kPeriods[i] : period_;
        } else outPeriod_ = period_;
        break;
      case 1: period_ = std::max(kMinPeriod, period_ - par_); outPeriod_ = period_; break;
      case 2: period_ = std::min(kMaxPeriod, period_ + par_); outPeriod_ = period_; break;
      case 3: slide(); break;
      case 4: vibrato(); break;
      case 5: slide(); volSlide(); break;
      case 6: vibrato(); volSlide(); break;
      case 7: {
        const int delta = (waveOf(tremWave_, tremPos_) * tremDepth_) >> 6;
        outVol_ = clampVol((tremPos_ & 32) ? vol_ - delta : vol_ + delta);
        tremPos_ = (tremPos_ + tremSpeed_) & 63;
        break;
      }
      case 0xA: volSlide(); break;
      case 0xE: {
        const int sub = par_ >> 4;
        if (sub == 9 && x && t % x == 0) { pos_ = 0; active_ = bankAt(cur_) != nullptr; }
        else if (sub == 0xC && t == x) { vol_ = 0; outVol_ = 0; }
        else if (sub == 0xD && delayed_ >= 0 && t == x) { trigger(delayed_); delayed_ = -1; }
        break;
      }
      default: break;
    }
  }

  double sampleRate_;
  const Clock* clock_;
  std::vector<std::unique_ptr<Bank>> bank_;
  bool active_ = false;
  double pos_ = 0;
  int period_ = 428, outPeriod_ = 428, vol_ = 64, outVol_ = 64;
  double gain_ = 0;
  int cur_ = 0, noteIdx_ = 0, target_ = 428, portaSpeed_ = 0, offsetMem_ = 0;
  double ftMul_ = 1;
  int vibSpeed_ = 0, vibDepth_ = 0, vibPos_ = 0, vibWave_ = 0, tremSpeed_ = 0, tremDepth_ = 0, tremPos_ = 0, tremWave_ = 0;
  int cmd_ = 0, par_ = 0, tickInRow_ = 0;
  bool hasPan_ = false;
  double panFx_ = 0;
  int pendingNote_ = -1, delayed_ = -1, lastTick_ = 0;
  std::int64_t lastFx_ = -1, lastSmp_ = -1;
};

}  // namespace

void registerModChan(NodeRegistry& registry) {
  NodeType type;
  type.type = "modchan";
  type.inputs = {};
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.shared = true;
  type.params = {{"smp", 0, 255, 0, true}, {"fx", 0, 4095, 0, true}, {"level", 0, 1, 0.45}, {"pan", -1, 1, 0}, {"interp", 0, 1, 1, true}};
  type.create = [](const NodeContext& context) { return std::make_unique<ModChan>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

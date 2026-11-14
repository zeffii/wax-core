// Voice-aware sample player with a new-note action (NNA). A new note never interrupts a sample that is playing to its end,
// so percussion can ring over the next hit. A looping sample cannot end by itself, so a new note fades the looping voices
// out over nnaFade ms; note off releases looping voices over `release` ms and leaves one-shots to finish; all-off releases
// everything; note cut stops everything at once. A sample has a normal loop and a sustain loop, each with its own
// crossfade. Notes can repeat (retrig), with grains, jitters, stretch, jumps, volume and pitch slides, a filter per sound
// and a vibrato. The long list of what each parameter does is in webtracker src/audio/nodes/sampler.js, which this follows
// line by line (the settings are read as a note starts, and the pattern writes them just before).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

constexpr int kVoices = 16;
enum Param {
  kLevel = 0, kRoot = 1, kRelease = 2, kNnaFade = 3, kOffset = 4, kReverse = 5, kRetrig = 6, kRepeats = 7, kShift = 8, kRamp = 9,
  kSample = 10, kSlice = 11, kGrain = 12, kEdge = 13, kAccel = 14, kStep = 15, kSlide = 16, kFlip = 17, kProb = 18, kJOff = 19,
  kJPitch = 20, kJPan = 21, kJTime = 22, kJLevel = 23, kVs = 24, kSubdiv = 34, kAbs = 35, kPort = 36, kPortTo = 40, kRMin = 41,
  kRMax = 42, kRPow = 43, kCutAfter = 44, kRelAfter = 45, kTScale = 46, kStretch = 47, kJump = 50, kFType = 58, kFCut = 59,
  kFRes = 60, kCsD = 61, kCsU = 62, kCStep = 63, kJCut = 64, kJTsub = 65, kVibS = 66, kVibD = 67, kVibW = 68, kLfoS = 69,
  kLfoD = 70, kLfoW = 71, kVibPh = 72, kLfoPh = 73, kVibNow = 74, kLfoNow = 75, kNnaAll = 76
};
constexpr int kVsKinds = 10, kPortKinds = 4;
constexpr double kFloor = 1e-6;   // a slide to nothing stops at -120 dB
constexpr double kInf = std::numeric_limits<double>::infinity();
enum Mode { kPlaying = 0, kReleasing = 1, kFading = 2, kCutting = 3 };

struct Loop {
  bool on = false;
  double start = 0, end = 0, xf = 0;
};

struct Bank {
  std::shared_ptr<const SampleMessage> msg;
  double ratio = 1;
  double volume = 1, globalVolume = 1, pan = 0;
  Loop norm, sus;
  const std::vector<float>& sample() const { return msg->data; }
  const std::vector<float>* right() const { return msg->right.empty() ? nullptr : &msg->right; }
};

// A loop in frames; it counts only when it fits inside the sample. The crossfade needs that many frames before the loop
// start and cannot be longer than the loop.
void setLoop(const Bank& S, Loop& loop, bool on, double start, double end, double xfade) {
  const bool ok = end - start >= 2 && end <= static_cast<double>(S.sample().size()) && start >= 0;
  loop.on = ok && on;
  loop.start = start;
  loop.end = end;
  loop.xf = ok ? std::max(0.0, std::min(std::min(std::floor(xfade), start), end - start)) : 0;
}

// The sample at frame q, interpolated; when wrapAt > 0 the frame after wrapAt - 1 is `to`. (An index outside the sample
// reads as NaN, as it does in JS.)
double tapAt(const std::vector<float>& a, double q, double wrapAt, double to) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const long k = static_cast<long>(q);
  const double f = q - static_cast<double>(k);
  const long k1 = wrapAt > 0 && static_cast<double>(k + 1) >= wrapAt ? static_cast<long>(to) : std::min<long>(k + 1, static_cast<long>(a.size()) - 1);
  const auto at = [&](long i) { return i >= 0 && i < static_cast<long>(a.size()) ? static_cast<double>(a[static_cast<std::size_t>(i)]) : nan; };
  return at(k) + (at(k1) - at(k)) * f;
}

double waveOf(int w, double ph) {
  return w == 1 ? 4 * std::abs(ph - 0.5) - 1 : w == 2 ? (ph < 0.5 ? 1 : -1) : w == 3 ? 2 * ph - 1 : js::sin(ph * 2 * kPi);
}

struct Slide {
  bool on = false;
  int kind = 0, value = 0;
};

struct Stretch {
  bool on = false;
  double L = 0;
  bool keep = false;
};

// What a note's settings were when it started (they stay as they were for its repeats).
struct Cfg {
  double shift, ramp, accel, step, slide, grain, edge, jOffset, jPitch, jPan, jTime, jLevel;
  bool flip;
  double prob;
  Slide vs, ps, cs;
  int fType;
  double fHz, fQ, cStep, jCut, jTsub, vibPh, lfoPh, rMin, rMax, rPow, tScale;
  Stretch stretch;
};

struct St {
  double cutAdd = 0;
  int si = 0, note = 0;
  double dir = 1;
  bool fromSlice = false;
  double base = 0, gain = 0, pitch = 0;
  Cfg cfg;
  double cutT = 0, relT = 0;
};

struct Chain {
  bool alive = true;
  int v = 0;
  double vAge = 0;
  int note = 0;
  St st;
  double base = 0;
  int k = 0;
  double interval = 0, nom = 0, spawnIn = 0, left = 0;
};

class Sampler final : public Node {
public:
  explicit Sampler(const NodeContext& context)
      : sr_(context.sampleRate), stereo_(context.stereo), clock_(context.clock), attack_(1 / (0.002 * context.sampleRate)), minGap_(0.001 * context.sampleRate) {
    for (int i = 0; i < kVoices; i++) {
      dir_[i] = 1; cutAt_[i] = kInf; pitchMul_[i] = 1; slideMul_[i] = 1; grainLeft_[i] = kInf; atkStep_[i] = 1; fadeStep_[i] = 1;
      volMul_[i] = 1; cutLeft_[i] = kInf; relLeft_[i] = kInf; portMul_[i] = 1; cutIn_[i] = kInf; from_[i] = -1;
    }
    lastNow_[0] = lastNow_[1] = std::numeric_limits<double>::quiet_NaN();
  }

  void sample(const std::shared_ptr<const SampleMessage>& message) override {
    const std::size_t slot = static_cast<std::size_t>(std::max(0, message->slot));
    if (bank_.size() <= slot) bank_.resize(slot + 1);
    auto S = std::make_unique<Bank>();
    S->msg = message;
    S->ratio = message->rate / sr_;
    S->volume = message->volume;
    S->globalVolume = message->globalVolume;
    S->pan = std::max(-1.0, std::min(1.0, message->pan));
    setLoop(*S, S->norm, message->loopOn, message->loopStart, message->loopEnd, message->loopXfade);
    setLoop(*S, S->sus, message->susOn, message->susStart, message->susEnd, message->susXfade);
    bank_[slot] = std::move(S);
  }

  void note(const NoteEvent& msg) override {
    if (msg.type == NoteType::On) {
      for (Chain& c : chains_) c.alive = false;   // a new note ends the repeats of the one before
      int v = -1;
      for (int k = 0; k < kVoices; k++) if (!active_[k]) { v = k; break; }
      if (v < 0) {   // all voices busy: take the oldest
        v = 0;
        for (int k = 1; k < kVoices; k++) if (age_[k] < age_[v]) v = k;
      }
      active_[v] = 1; mode_[v] = kPlaying; note_[v] = msg.note; inSus_[v] = 0; loopy_[v] = 0;
      pos_[v] = 0; gain_[v] = static_cast<float>(msg.vel); env_[v] = 0; age_[v] = ++stamp_;
      fresh_[v] = 1; dir_[v] = 1; skip_[v] = 0; cutAt_[v] = kInf; grainLeft_[v] = kInf;
      from_[v] = msg.from; choke_[v] = 0; cutIn_[v] = kInf;
    } else if (msg.type == NoteType::Off) {
      for (Chain& c : chains_) if (c.note == msg.note) c.alive = false;
      if (!msg.implied) {   // an implied release (a new note in the same column) is left to nnaFade
        for (int v = 0; v < kVoices; v++) {
          if (!active_[v] || !loopy_[v] || mode_[v] != kPlaying || note_[v] != msg.note) continue;
          const Bank& S = *bank_[static_cast<std::size_t>(sm_[v])];
          const bool wasSus = inSus_[v];
          inSus_[v] = 0;
          if (S.norm.on || !wasSus) mode_[v] = kReleasing;   // with only a sustain loop the tail plays out
        }
      }
    } else if (msg.type == NoteType::AllOff) {
      for (Chain& c : chains_) c.alive = false;
      for (int v = 0; v < kVoices; v++) if (active_[v] && mode_[v] == kPlaying) { mode_[v] = kReleasing; inSus_[v] = 0; }
    } else {
      for (int v = 0; v < kVoices; v++) active_[v] = 0;
      chains_.clear();
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override;

private:
  double rnd() {
    seed_ = seed_ * 1664525u + 1013904223u;
    return seed_ / 4294967296.0;
  }
  double jit() { return rnd() * 2 - 1; }

  double lim(const Cfg& cfg, double t) const { return std::max(std::max(minGap_, cfg.rMin), cfg.rMax > 0 ? std::min(t, cfg.rMax) : t); }

  double sr_;
  bool stereo_;
  const Clock* clock_;
  double attack_;   // also the 2 ms ramp used for a cut
  double minGap_;
  std::vector<std::unique_ptr<Bank>> bank_;
  double stamp_ = 0;
  std::uint8_t fromSlice_[kVoices] = {};
  int sm_[kVoices] = {};
  std::uint8_t loopy_[kVoices] = {}, inSus_[kVoices] = {}, active_[kVoices] = {}, mode_[kVoices] = {};
  int note_[kVoices] = {};
  double pos_[kVoices] = {}, age_[kVoices] = {};
  float gain_[kVoices] = {}, env_[kVoices] = {};
  std::uint8_t fresh_[kVoices] = {};
  double dir_[kVoices];
  int skip_[kVoices] = {};
  double cutAt_[kVoices], pitchMul_[kVoices], slideMul_[kVoices], vpan_[kVoices] = {};
  double grainLeft_[kVoices], atkStep_[kVoices], fadeStep_[kVoices];
  double volMul_[kVoices];
  std::uint8_t vsKind_[kVoices] = {};
  double vsLeft_[kVoices] = {}, vsStep_[kVoices] = {};
  double lastVs_[kVsKinds] = {};
  double cutLeft_[kVoices], relLeft_[kVoices];
  double portLeft_[kVoices] = {}, portMul_[kVoices];
  double cutIn_[kVoices];
  int from_[kVoices];
  std::uint8_t choke_[kVoices] = {};
  double lastPs_[kPortKinds] = {};
  std::uint8_t fType_[kVoices] = {};
  double fHz_[kVoices] = {}, fQ_[kVoices] = {}, fOff_[kVoices] = {};
  double fz_[kVoices * 4] = {}, fk_[kVoices * 5] = {};   // states z1 z2 (left) y1 y2 (right); coefficients b0 b1 b2 a1 a2
  double csLeft_[kVoices] = {}, csStep_[kVoices] = {};
  double vibPh_[kVoices] = {}, lfoPh_[kVoices] = {};
  double lastJ_[8] = {}, lastCs_[2] = {}, lastNow_[2];
  double gVib_ = 0, gLfo_ = 0;
  std::vector<Chain> chains_;
  std::uint32_t seed_ = 0x2545f491u;
};

void Sampler::process(const float* const* /*inputs*/, float* const* outputs, int n, const float* params, const int* slots) {
  float* outL = outputs[0];
  float* outR = stereo_ ? outputs[1] : nullptr;
  std::fill(outL, outL + n, 0.0f);
  if (outR) std::fill(outR, outR + n, 0.0f);
  if (bank_.empty()) return;
  const auto P = [&](int i) { return static_cast<double>(params[slots[i]]); };
  const double sr = sr_;
  const double root = P(kRoot);
  const double relStep = 1 / (std::max(1.0, P(kRelease)) * 0.001 * sr);
  const double fadeMs = P(kNnaFade);
  const double fadeStepNna = fadeMs <= 0 ? attack_ : 1 / (fadeMs * 0.001 * sr);
  // Voices that have just started read where to start and whether to repeat.
  const double tickSamples = clock_ ? clock_->tickSamples : 0;
  const auto clamp01 = [](double x) { return std::max(0.0, std::min(1.0, x)); };
  const double subdivide = std::max(1.0, std::floor(P(kSubdiv)));

  // A volume slide on voice w: kind index, value XXYY.
  const auto startSlide = [&](int w, int kind, int value) {
    const int t = (value >> 8) & 255, a = value & 255;
    const double cur = std::max(kFloor, volMul_[w]);
    const double len = std::max(1.0, jsRound(kind >= 6 ? t * 0.01 * sr : t / subdivide * tickSamples));   // the Ms ones: 10 * t milliseconds
    vsLeft_[w] = len;
    if (kind == 0) { vsKind_[w] = 1; vsStep_[w] = (a / 64.0 - volMul_[w]) / len; return; }
    const int k = kind >= 6 ? kind - 4 : kind;
    const bool down = k == 2 || k == 4, fine = k >= 4;   // the Ms kinds are the same slides with another unit of time
    const double target = k == 1 ? std::max(kFloor, a / 64.0) : cur * js::pow(10, (down ? -1 : 1) * (fine ? a / 256.0 : static_cast<double>(a)) / 20);
    vsKind_[w] = 2;
    vsStep_[w] = js::pow(std::max(kFloor, target) / cur, 1 / len);
  };
  // A pitch slide on voice w: XX semitones, or XX/256 for the fine ones, over YY subticks.
  const auto startPort = [&](int w, int kind, int value) {
    const int a = (value >> 8) & 255, t = value & 255;
    const double semis = kind == 0 ? -a : kind == 1 ? a : kind == 2 ? -a / 256.0 : a / 256.0;
    portLeft_[w] = std::max(1.0, jsRound(t / subdivide * tickSamples));
    portMul_[w] = js::pow(2, semis / 12 / portLeft_[w]);
  };
  const auto startCs = [&](int w, int kind, int value) {
    const int a = (value >> 8) & 255, t = value & 255;
    const double len = std::max(1.0, jsRound(t / subdivide * tickSamples));
    csLeft_[w] = len;
    csStep_[w] = (kind == 0 ? -a : a) / len;
  };
  // Starts a sound on voice w from a chain's state: where in the sample, which way, how loud, how high; the jitters apply here.
  const auto begin = [&](int w, St& st, int at) {
    const Bank& S = *bank_[static_cast<std::size_t>(st.si)];
    const Cfg& cfg = st.cfg;
    const double last = static_cast<double>(S.sample().size()) - 1;
    double start;
    if (st.fromSlice) start = st.base;
    else { const double j = jit(); start = clamp01(st.base + cfg.jOffset * j); }
    sm_[w] = st.si; note_[w] = st.note; dir_[w] = st.dir; fromSlice_[w] = st.fromSlice ? 1 : 0;
    inSus_[w] = S.sus.on ? 1 : 0; loopy_[w] = S.norm.on || S.sus.on ? 1 : 0;
    pos_[w] = dir_[w] > 0 || fromSlice_[w] ? start * last : last - start * last;
    const double r1 = rnd();
    gain_[w] = static_cast<float>(st.gain * (1 - cfg.jLevel * r1));
    const double j2 = jit();
    pitchMul_[w] = js::pow(2, (st.pitch + cfg.jPitch * j2) / 12);
    volMul_[w] = 1; vsLeft_[w] = 0; vsKind_[w] = 0;
    if (cfg.vs.on) startSlide(w, cfg.vs.kind, cfg.vs.value);
    slideMul_[w] = cfg.slide != 0 && tickSamples > 0 ? js::pow(2, cfg.slide / 12 / tickSamples) : 1;
    const double j3 = jit();
    vpan_[w] = cfg.jPan * j3;
    cutLeft_[w] = st.cutT > 0 ? st.cutT : kInf; relLeft_[w] = st.relT > 0 ? st.relT : kInf;
    fType_[w] = static_cast<std::uint8_t>(cfg.fType); fHz_[w] = cfg.fHz; fQ_[w] = cfg.fQ;
    const double j4 = jit();
    fOff_[w] = st.cutAdd + cfg.jCut * j4; csLeft_[w] = 0; csStep_[w] = 0;
    for (int k = 0; k < 4; k++) fz_[w * 4 + k] = 0;
    for (int k = 0; k < 5; k++) fk_[w * 5 + k] = 0;
    if (cfg.cs.on) startCs(w, cfg.cs.kind, cfg.cs.value);
    vibPh_[w] = cfg.vibPh >= 0 ? cfg.vibPh : gVib_; lfoPh_[w] = cfg.lfoPh >= 0 ? cfg.lfoPh : gLfo_;
    portLeft_[w] = 0; portMul_[w] = 1;
    if (cfg.ps.on) startPort(w, cfg.ps.kind, cfg.ps.value);
    skip_[w] = at;
    if (cfg.grain >= 1) {
      const double fade = std::max(0.002 * sr, cfg.edge * cfg.grain / 2);
      atkStep_[w] = fadeStep_[w] = 1 / std::min(fade, cfg.grain / 2 + 1);
      grainLeft_[w] = std::max(0.0, cfg.grain - fade);
    } else { atkStep_[w] = attack_; fadeStep_[w] = attack_; grainLeft_[w] = kInf; }
  };

  for (int v = 0; v < kVoices; v++) {
    if (!active_[v] || !fresh_[v]) continue;
    fresh_[v] = 0;
    const double portTo = P(kPortTo);
    if (portTo > 0 && from_[v] >= 0 && from_[v] != note_[v]) {   // a glide: the sound still playing in this column bends to the new note, with no restart
      int w = -1;
      for (int k = 0; k < kVoices; k++) if (k != v && active_[k] && mode_[k] == kPlaying && note_[k] == from_[v] && (w < 0 || age_[k] > age_[w])) w = k;
      if (w >= 0) {
        pitchMul_[w] *= js::pow(2, (from_[v] - note_[v]) / 12.0);   // sounds as before at the new note's key, then slides to it
        note_[w] = note_[v]; gain_[w] = gain_[v];
        portLeft_[w] = std::max(1.0, jsRound(portTo / 16 / subdivide * tickSamples));
        portMul_[w] = js::pow(2, (note_[v] - from_[v]) / 12.0 / portLeft_[w]);
        active_[v] = 0;
        continue;
      }
    }
    for (int k = 0; k < kVoices; k++) {   // a one-shot is cut by the next note of its own column, not by other columns' notes
      if (k != v && active_[k] && mode_[k] == kPlaying && !fresh_[k] && age_[k] < age_[v] && (loopy_[k] || choke_[v] || (P(kNnaAll) >= 0.5 && from_[v] >= 0 && note_[k] == from_[v]))) mode_[k] = kFading;
    }
    const double siRaw = std::floor(P(kSample));
    const int si = std::max(0, std::min(255, std::isnan(siRaw) ? 0 : static_cast<int>(siRaw)));
    if (static_cast<std::size_t>(si) >= bank_.size() || !bank_[static_cast<std::size_t>(si)]) { active_[v] = 0; continue; }   // no sample in that slot: the note is silent
    const Bank& S = *bank_[static_cast<std::size_t>(si)];
    const double last = static_cast<double>(S.sample().size()) - 1;
    const std::vector<double>& slices = S.msg->slices;
    const double sliceIndex = std::floor(P(kSlice));
    const bool hasMarker = sliceIndex >= 0 && sliceIndex < static_cast<double>(slices.size());
    const double marker = hasMarker ? slices[static_cast<std::size_t>(sliceIndex)] : 0;
    const double block = std::floor(P(kAbs));
    const double frame = block < 65535 ? block * 65536 + jsRound(std::max(0.0, std::min(1.0, P(kOffset))) * 65535) : -1;   // absolute: Off is the low part of the frame number
    const double off = hasMarker ? std::min(1.0, marker / std::max(1.0, last)) : frame >= 0 ? std::min(1.0, frame / std::max(1.0, last)) : std::max(0.0, std::min(1.0, P(kOffset)));
    St st;
    Cfg& cfg = st.cfg;
    cfg.shift = P(kShift); cfg.ramp = P(kRamp); cfg.accel = P(kAccel); cfg.step = P(kStep); cfg.flip = P(kFlip) >= 1; cfg.prob = P(kProb); cfg.slide = P(kSlide);
    cfg.grain = P(kGrain) * tickSamples; cfg.edge = P(kEdge); cfg.jOffset = P(kJOff); cfg.jPitch = P(kJPitch); cfg.jPan = P(kJPan); cfg.jTime = P(kJTime); cfg.jLevel = P(kJLevel);
    cfg.vs = Slide{};
    for (int k = 0; k < kVsKinds; k++) { const int x = static_cast<int>(std::floor(P(kVs + k))); lastVs_[k] = x; if (!cfg.vs.on && x > 0) cfg.vs = {true, k, x}; }
    cfg.ps = Slide{};
    for (int k = 0; k < kPortKinds; k++) { const int x = static_cast<int>(std::floor(P(kPort + k))); lastPs_[k] = x; if (!cfg.ps.on && x > 0) cfg.ps = {true, k, x}; }
    cfg.cs = Slide{};
    for (int k = 0; k < 2; k++) { const int x = static_cast<int>(std::floor(P(kCsD + k))); lastCs_[k] = x; if (!cfg.cs.on && x > 0) cfg.cs = {true, k, x}; }
    cfg.fType = static_cast<int>(std::floor(P(kFType)));
    cfg.fHz = 20 * js::pow(1000, P(kFCut) / 65535); cfg.fQ = 0.707 * js::pow(28, P(kFRes) / 65535);
    cfg.cStep = P(kCStep); cfg.jCut = P(kJCut); cfg.jTsub = P(kJTsub);
    cfg.vibPh = P(kVibPh) < 65535 ? P(kVibPh) / 65536 : -1; cfg.lfoPh = P(kLfoPh) < 65535 ? P(kLfoPh) / 65536 : -1;
    for (int k = 0; k < 8; k++) lastJ_[k] = std::floor(P(kJump + k));
    const double sub = tickSamples / subdivide;   // samples in a subtick
    cfg.rMin = P(kRMin) * sub; cfg.rMax = P(kRMax) * sub; cfg.rPow = P(kRPow); cfg.tScale = P(kTScale);
    // Stretch: the whole sample over a length, each repeat starting that much further along (negative: backwards).
    const double sSub = std::floor(P(kStretch)) - 32768, sTk = std::floor(P(kStretch + 1)) - 32768, sKeep = std::floor(P(kStretch + 2)) - 32768;
    cfg.stretch = sKeep != 0 ? Stretch{true, sKeep * tickSamples, true} : sTk != 0 ? Stretch{true, sTk * tickSamples, false} : sSub != 0 ? Stretch{true, sSub * sub, false} : Stretch{};
    st.cutAdd = 0; st.si = si; st.note = note_[v]; st.dir = P(kReverse) >= 1 ? -1 : 1; st.fromSlice = hasMarker; st.base = off; st.gain = gain_[v]; st.pitch = 0;
    st.cutT = std::min(cutIn_[v], P(kCutAfter) > 0 ? P(kCutAfter) / 16 * tickSamples : kInf);
    st.relT = P(kRelAfter) / 16 * tickSamples;
    begin(v, st, 0);
    const double every = P(kRetrig) * tickSamples;
    if (every >= 1) {
      const double reps = std::floor(P(kRepeats));
      const double first = lim(cfg, every);
      Chain c;
      c.alive = true; c.v = v; c.vAge = age_[v]; c.note = note_[v]; c.st = st; c.base = every; c.k = 0; c.interval = first; c.nom = first;
      const double r = rnd();
      c.spawnIn = first + cfg.jTime * first * r;
      c.left = reps > 0 ? reps : kInf;
      chains_.push_back(c);
    }
  }

  // A slide value written while sounds play slides them (the sounds that are playing and not yet fading out).
  for (int k = 0; k < kVsKinds; k++) {
    const double x = std::floor(P(kVs + k));
    if (std::isnan(x)) continue;
    if (x != lastVs_[k] && x > 0) for (int v = 0; v < kVoices; v++) if (active_[v] && mode_[v] == kPlaying) startSlide(v, k, static_cast<int>(x));
    lastVs_[k] = x;
  }
  for (int k = 0; k < kPortKinds; k++) {
    const double x = std::floor(P(kPort + k));
    if (std::isnan(x)) continue;
    if (x != lastPs_[k] && x > 0) for (int v = 0; v < kVoices; v++) if (active_[v] && mode_[v] == kPlaying) startPort(v, k, static_cast<int>(x));
    lastPs_[k] = x;
  }
  for (int k = 0; k < 2; k++) {
    const double x = std::floor(P(kCsD + k));
    if (std::isnan(x)) continue;
    if (x != lastCs_[k] && x > 0) for (int v = 0; v < kVoices; v++) if (active_[v] && mode_[v] == kPlaying) startCs(v, k, static_cast<int>(x));
    lastCs_[k] = x;
  }
  // A jump written while sounds play moves them along their wave: subticks or ticks, by the sample's own time or the playing speed.
  for (int k = 0; k < 8; k++) {
    const double x = std::floor(P(kJump + k));
    if (std::isnan(x)) continue;
    if (x != lastJ_[k] && x > 0) {
      for (int v = 0; v < kVoices; v++) {
        if (!active_[v] || mode_[v] != kPlaying || static_cast<std::size_t>(sm_[v]) >= bank_.size() || !bank_[static_cast<std::size_t>(sm_[v])]) continue;
        const Bank& S = *bank_[static_cast<std::size_t>(sm_[v])];
        const double unit = k < 4 ? tickSamples / subdivide : tickSamples;
        const double speed = (k & 2) ? js::pow(2, (note_[v] - root) / 12) * S.ratio * pitchMul_[v] : S.ratio;
        const double sign = (k & 1) ? -1 : 1;
        pos_[v] = std::max(0.0, std::min(static_cast<double>(S.sample().size()) - 1, pos_[v] + dir_[v] * sign * x * unit * speed));
      }
    }
    lastJ_[k] = x;
  }
  // The phases of the vibrato and cutoff LFO; a value written to the sync parameters sets them now.
  const double vibIn = P(kVibNow), lfoIn = P(kLfoNow);
  if (!std::isnan(vibIn) && !(vibIn == lastNow_[0])) {
    if (!std::isnan(lastNow_[0])) { gVib_ = vibIn / 65536; for (int v = 0; v < kVoices; v++) vibPh_[v] = gVib_; }
    lastNow_[0] = vibIn;
  }
  if (!std::isnan(lfoIn) && !(lfoIn == lastNow_[1])) {
    if (!std::isnan(lastNow_[1])) { gLfo_ = lfoIn / 65536; for (int v = 0; v < kVoices; v++) lfoPh_[v] = gLfo_; }
    lastNow_[1] = lfoIn;
  }
  const double subS = tickSamples / subdivide;
  const double vibInc = P(kVibS) > 0 && subS > 0 ? 1 / (P(kVibS) / 16 * subS) : 0, vibDepth = P(kVibD) / 256;
  const int vibWave = static_cast<int>(std::floor(P(kVibW)));
  const double lfoInc = P(kLfoS) > 0 && subS > 0 ? 1 / (P(kLfoS) / 16 * subS) : 0, lfoDepth = P(kLfoD) / 256;
  const int lfoWave = static_cast<int>(std::floor(P(kLfoW)));
  gVib_ = jsMod(gVib_ + vibInc * n, 1); gLfo_ = jsMod(gLfo_ + lfoInc * n, 1);

  // Repeats that fall in this block: the new sound starts part way through it and, with no grain length, the one before is cut there.
  chains_.erase(std::remove_if(chains_.begin(), chains_.end(), [](const Chain& c) { return !c.alive; }), chains_.end());
  for (Chain& c : chains_) {
    St& st = c.st;
    const Cfg& cfg = st.cfg;
    while (c.spawnIn < n) {
      const double nextBase = st.base + (cfg.stretch.on ? (cfg.stretch.keep ? c.interval : c.base) / cfg.stretch.L : cfg.shift);
      if (c.left < 1 || nextBase < 0 || nextBase > 1) { c.alive = false; break; }   // the chain is finished
      st.base = nextBase; st.gain *= cfg.ramp; st.pitch += cfg.step; st.cutAdd += cfg.cStep; if (cfg.flip) st.dir = -st.dir;
      st.cutT *= cfg.tScale; st.relT *= cfg.tScale;
      c.left--;
      const int at = static_cast<int>(std::max(0.0, std::ceil(c.spawnIn)));
      if (rnd() < cfg.prob) {
        int w = -1;
        for (int k = 0; k < kVoices; k++) if (!active_[k]) { w = k; break; }
        if (w < 0) {   // all voices busy: take the oldest, not the one being repeated
          w = c.v == 0 ? 1 : 0;
          for (int k = 0; k < kVoices; k++) if (k != c.v && age_[k] < age_[w]) w = k;
        }
        if (cfg.grain < 1 && active_[c.v] && age_[c.v] == c.vAge) cutAt_[c.v] = at;
        active_[w] = 1; mode_[w] = kPlaying; inSus_[w] = 0; fresh_[w] = 0; env_[w] = 0; age_[w] = ++stamp_; cutAt_[w] = kInf;
        begin(w, st, at);
        c.v = w; c.vAge = age_[w];
      }
      c.nom += c.interval;
      c.k++;
      c.interval = lim(cfg, c.base * js::pow(cfg.accel, c.k) * js::pow(c.k + 1, cfg.rPow));
      const double ra = rnd();
      const double rb = rnd();
      c.spawnIn = c.nom + cfg.jTime * c.interval * ra + cfg.jTsub * (tickSamples / subdivide) * rb;
    }
    if (c.alive) { c.nom -= n; c.spawnIn -= n; }
  }

  for (int v = 0; v < kVoices; v++) {
    if (!active_[v]) continue;
    const Bank& S = *bank_[static_cast<std::size_t>(sm_[v])];
    const std::vector<float>& sample = S.sample();
    const std::vector<float>* right = S.right();
    const double last = static_cast<double>(sample.size()) - 1;
    const double level = P(kLevel) * S.globalVolume * S.volume;
    const double pn = std::max(-1.0, std::min(1.0, S.pan + vpan_[v]));
    const double panL = std::min(1.0, 1 - pn), panR = std::min(1.0, 1 + pn);
    const double baseRate = js::pow(2, (note_[v] - root) / 12) * S.ratio, slideBy = slideMul_[v];
    double cl = cutLeft_[v], rl = relLeft_[v], pl = portLeft_[v];
    const double pmul = portMul_[v];
    double pm = pitchMul_[v], gl = grainLeft_[v], cs = attack_, vm = volMul_[v], vl = vsLeft_[v];
    const int vk = vsKind_[v];
    const double vstep = vsStep_[v];
    const double atk = atkStep_[v];
    int m = mode_[v];
    const int ft = fType_[v];
    const double fq = fQ_[v], hz0 = fHz_[v];
    double fo = fOff_[v], csl = csLeft_[v], vph = vibPh_[v], lph = lfoPh_[v];
    double z1 = fz_[v * 4], z2 = fz_[v * 4 + 1], y1 = fz_[v * 4 + 2], y2 = fz_[v * 4 + 3];
    double b0 = fk_[v * 5], b1 = fk_[v * 5 + 1], b2 = fk_[v * 5 + 2], a1 = fk_[v * 5 + 3], a2 = fk_[v * 5 + 4];
    const bool back = dir_[v] < 0;
    const Loop* loop = back ? nullptr : inSus_[v] ? &S.sus : S.norm.on ? &S.norm : nullptr;
    const double ls = loop ? loop->start : 0, le = loop ? loop->end : 0, xf = loop ? loop->xf : 0;
    const double span = le - ls;
    double p = pos_[v], e = env_[v];
    for (int i = skip_[v]; i < n; i++) {
      if (i >= cutAt_[v]) { m = mode_[v] = kCutting; cs = attack_; }
      else if (gl <= 0 && m != kCutting) { m = mode_[v] = kCutting; cs = fadeStep_[v]; }
      else if (cl <= 0 && m != kCutting) { m = mode_[v] = kCutting; cs = attack_; }
      else if (rl <= 0 && m == kPlaying) { m = mode_[v] = kReleasing; inSus_[v] = 0; }
      gl--; cl--; rl--;
      double vibS = 0, lfoS = 0;
      if (vibInc > 0) { vibS = vibDepth * waveOf(vibWave, vph); vph += vibInc; if (vph >= 1) vph -= 1; }
      if (lfoInc > 0) { lfoS = lfoDepth * waveOf(lfoWave, lph); lph += lfoInc; if (lph >= 1) lph -= 1; }
      const double rate = baseRate * pm * (vibS != 0 ? js::pow(2, vibS / 12) : 1);
      if (loop) {
        if (p >= le) p = ls + jsMod(p - ls, span);
      } else if (back ? p <= 0 : p >= last) {
        active_[v] = 0;
        break;
      }
      if (m == kPlaying) {
        if (e < 1) e = std::min(1.0, e + atk);
      } else {
        e -= m == kReleasing ? relStep : m == kFading ? fadeStepNna : cs;
        if (e <= 0) { active_[v] = 0; break; }
      }
      if (vl > 0) { vm = vk == 1 ? std::max(0.0, vm + vstep) : vm * vstep; vl--; }
      if (csl > 0) { fo += csStep_[v]; csl--; }
      const double amp = e * static_cast<double>(gain_[v]) * level * vm;
      // inside the crossfade the loop's tail blends into the audio before its start
      const double t = xf > 0 && p >= le - xf ? (p - (le - xf)) / xf : -1;
      const double wa = t < 0 ? 1 : js::cos(t * kPi / 2), wb = t < 0 ? 0 : js::sin(t * kPi / 2);
      double l = tapAt(sample, p, le, ls), r = 0;
      if (t >= 0) l = l * wa + tapAt(sample, p - span, 0, 0) * wb;
      if (right) {
        r = tapAt(*right, p, le, ls);
        if (t >= 0) r = r * wa + tapAt(*right, p - span, 0, 0) * wb;
      }
      if (ft > 0) {   // the voice filter: coefficients are renewed every 16 samples, since the cutoff moves
        if (((i + 1) & 15) == 0 || b0 == 0) {
          const double f = std::min(0.45 * sr, std::max(20.0, hz0 * js::pow(2, (fo + lfoS) / 12))), w0 = 2 * kPi * f / sr;
          const double cw = js::cos(w0), sw = js::sin(w0), al = sw / (2 * fq);
          double nb0, nb1, nb2;
          if (ft == 1) { nb0 = (1 - cw) / 2; nb1 = 1 - cw; nb2 = nb0; }
          else if (ft == 2) { nb0 = (1 + cw) / 2; nb1 = -(1 + cw); nb2 = nb0; }
          else if (ft == 3) { nb0 = sw / 2; nb1 = 0; nb2 = -nb0; }
          else if (ft == 4) { nb0 = al; nb1 = 0; nb2 = -al; }
          else { nb0 = 1; nb1 = -2 * cw; nb2 = 1; }
          const double a0 = 1 + al;
          b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = -2 * cw / a0; a2 = (1 - al) / a0;
        }
        const double yl = b0 * l + z1; z1 = b1 * l - a1 * yl + z2; z2 = b2 * l - a2 * yl; l = yl;
        if (right) { const double yr = b0 * r + y1; y1 = b1 * r - a1 * yr + y2; y2 = b2 * r - a2 * yr; r = yr; }
      }
      if (!right) {
        if (outR) {
          outL[i] = static_cast<float>(static_cast<double>(outL[i]) + l * amp * panL);
          outR[i] = static_cast<float>(static_cast<double>(outR[i]) + l * amp * panR);
        } else {
          outL[i] = static_cast<float>(static_cast<double>(outL[i]) + l * amp);
        }
      } else if (outR) {
        outL[i] = static_cast<float>(static_cast<double>(outL[i]) + l * amp * panL);
        outR[i] = static_cast<float>(static_cast<double>(outR[i]) + r * amp * panR);
      } else {
        outL[i] = static_cast<float>(static_cast<double>(outL[i]) + (l + r) * 0.5 * amp);
      }
      p += back ? -rate : rate;
      pm *= slideBy;
      if (pl > 0) { pm *= pmul; pl--; }
    }
    fOff_[v] = fo; csLeft_[v] = csl; vibPh_[v] = vph; lfoPh_[v] = lph;
    fz_[v * 4] = z1; fz_[v * 4 + 1] = z2; fz_[v * 4 + 2] = y1; fz_[v * 4 + 3] = y2;
    fk_[v * 5] = b0; fk_[v * 5 + 1] = b1; fk_[v * 5 + 2] = b2; fk_[v * 5 + 3] = a1; fk_[v * 5 + 4] = a2;
    pos_[v] = p; env_[v] = static_cast<float>(e); skip_[v] = 0; pitchMul_[v] = pm; grainLeft_[v] = gl; cutLeft_[v] = cl; relLeft_[v] = rl;
    portLeft_[v] = pl; volMul_[v] = vm; vsLeft_[v] = vl;
  }
  for (int v = 0; v < kVoices; v++) cutAt_[v] = kInf;
}

}  // namespace

void registerSampler(NodeRegistry& registry) {
  NodeType type;
  type.type = "sampler";
  type.inputs = {};
  type.outputs = {"out"};
  type.stereoOptional = {"out"};   // stereo when the sample has two channels
  type.params = {
    {"level", 0, 1, 0.8},
    {"root", 0, 127, 60},
    {"release", 5, 2000, 150},
    {"nnaFade", 0, 2000, 20},
    {"offset", 0, 1, 0},                            // where the note starts, as a fraction of the sample
    {"reverse", 0, 1, 0, true},
    {"retrig", 0, 65535.0 / 4096, 0},               // ticks between repeats; hex 1000 is one tick
    {"repeats", 0, 255, 0, true},                   // 0: until the note ends
    {"shift", -1, 65535.0 / 32768 - 1, 0},          // per repeat; hex 8000 is zero
    {"ramp", 0, 65535.0 / 49152, 1},                // per repeat; hex C000 is one
    {"sample", 0, 255, 0, true},                    // which sample of the instrument's list plays
    {"slice", 0, 65535, 65535, true},               // start at this slice marker of the sample
    {"grain", 0, 65535.0 / 4096, 0},                // how long each sound lasts, in ticks; 0: the sample's own length
    {"edge", 0, 1, 0.5},                            // how much of a grain is fade
    {"accel", 0, 65535.0 / 32768, 1},               // each repeat's interval is multiplied by this
    {"step", -16, 65535.0 / 2048 - 16, 0},          // semitones each repeat is higher
    {"slide", -16, 65535.0 / 2048 - 16, 0},         // pitch bend while a sound plays, semitones per tick
    {"flip", 0, 1, 0, true},                        // 1: every repeat plays the other way round
    {"prob", 0, 1, 1},                              // chance that a repeat sounds
    {"jOffset", 0, 1, 0},
    {"jPitch", 0, 12, 0},
    {"jPan", 0, 1, 0},
    {"jTime", 0, 1, 0},
    {"jLevel", 0, 1, 0},
    {"volLin", 0, 65535, 0, true},                  // volume slides: XX the time, YY the amount
    {"volLog", 0, 65535, 0, true},
    {"volDown", 0, 65535, 0, true},
    {"volUp", 0, 65535, 0, true},
    {"volFineDown", 0, 65535, 0, true},
    {"volFineUp", 0, 65535, 0, true},
    {"volDownMs", 0, 65535, 0, true},
    {"volUpMs", 0, 65535, 0, true},
    {"volFineDownMs", 0, 65535, 0, true},
    {"volFineUpMs", 0, 65535, 0, true},
    {"subdivide", 1, 255, 6, true},                 // subticks in a tick
    {"blockCount", 0, 65535, 65535, true},          // absolute offset: the 65536-frame block Off counts from
    {"portDown", 0, 65535, 0, true},
    {"portUp", 0, 65535, 0, true},
    {"portFineDown", 0, 65535, 0, true},
    {"portFineUp", 0, 65535, 0, true},
    {"portTo", 0, 65535.0 / 16, 0},
    {"retrigMin", 0, 65535.0 / 256, 0},
    {"retrigMax", 0, 65535, 0, true},
    {"retrigPow", -2, 65535.0 / 16384 - 2, 0},
    {"cutAfter", 0, 65535.0 / 16, 0},
    {"releaseAfter", 0, 65535.0 / 16, 0},
    {"cutScale", 0, 65535.0 / 32768, 1},
    {"stretchSub", 0, 65535, 32768, true},
    {"stretchTicks", 0, 65535, 32768, true},
    {"stretchKeep", 0, 65535, 32768, true},
    {"jumpAhead", 0, 65535, 0, true},
    {"jumpBack", 0, 65535, 0, true},
    {"jumpAheadRel", 0, 65535, 0, true},
    {"jumpBackRel", 0, 65535, 0, true},
    {"jumpTicksAhead", 0, 65535, 0, true},
    {"jumpTicksBack", 0, 65535, 0, true},
    {"jumpTicksAheadRel", 0, 65535, 0, true},
    {"jumpTicksBackRel", 0, 65535, 0, true},
    {"filterType", 0, 5, 0, true},                  // 0 off, 1 low pass, 2 high pass, 3 band pass (constant skirt), 4 band pass (constant peak), 5 notch
    {"filterCutoff", 0, 65535, 65535, true},
    {"filterRes", 0, 65535, 0, true},
    {"cutDown", 0, 65535, 0, true},
    {"cutUp", 0, 65535, 0, true},
    {"cutStep", -16, 65535.0 / 2048 - 16, 0},
    {"jCutoff", 0, 65535.0 / 256, 0},
    {"jTimeSub", 0, 65535.0 / 256, 0},
    {"vibSpeed", 0, 65535.0 / 16, 0},
    {"vibDepth", 0, 65535.0 / 256, 0},
    {"vibWave", 0, 3, 0, true},
    {"lfoSpeed", 0, 65535.0 / 16, 0},
    {"lfoDepth", 0, 65535.0 / 256, 0},
    {"lfoWave", 0, 3, 0, true},
    {"vibPhase", 0, 65535, 65535, true},
    {"lfoPhase", 0, 65535, 65535, true},
    {"vibSync", 0, 65535, 0, true},
    {"lfoSync", 0, 65535, 0, true},
    {"nnaAll", 0, 1, 1, true},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Sampler>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

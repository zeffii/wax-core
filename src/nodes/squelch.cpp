// Squelch: an acid bassline machine. One voice, a band-limited saw or square, a resonant four-pole ladder low-pass
// whose feedback saturates (so it squeals and, turned right up, rings by itself), and the three things that make the sound:
//
//  - the filter envelope: a note restarts it and the cutoff rises `envmod` (up to five octaves) and falls back over `decay`;
//  - slide: a note written with the Sld flag glides to its pitch from the note before, over `slide` ms, and, because the
//    gate stays up, does not restart either envelope (legato). A slide with no note sounding is just a note;
//  - accent: a note written with the Acc flag (1 to F) is louder and punchier, a transient on top of its level, and opens
//    the filter further. Its filter envelope is always short, and a run of accents charges a sweep that builds on itself.
//
// Acc and Sld are for one note only: they count when the pattern writes them on the row of the note, and a blank cell is
// off. (A parameter column normally keeps its value; here the node looks at whether the row wrote it.) A note off lets
// the amplitude go over `rel` ms; a new note while another sounds is not a release. Note cut silences it at once.
// Port of webtracker src/audio/nodes/squelch.js.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kWave = 0, kTune, kCutoff, kRes, kEnvmod, kDecay, kAccent, kSlide, kDrive, kRel, kVel, kLevel, kAcc, kSld };
enum class Stage { Attack, Held, Release };

class Squelch final : public Node {
public:
  explicit Squelch(const NodeContext& context) : sr_(context.sampleRate), clock_(context.clock) {
    attackStep_ = 1 / (0.003 * sr_);
    heldCoef_ = js::exp(-1 / (0.02 * sr_));
    envSmooth_ = 1 - js::exp(-1 / (0.002 * sr_));
    accDecay_ = js::exp(-1 / (0.25 * sr_));
    capDecay_ = js::exp(-1 / (0.6 * sr_));
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { pending_ = true; pendNote_ = event.note; pendVel_ = clamp(event.vel, 0, 1); }
    else if (event.type == NoteType::Off) { if (!event.implied && gate_) { gate_ = false; stage_ = Stage::Release; } }
    else if (event.type == NoteType::AllOff) { if (gate_) { gate_ = false; stage_ = Stage::Release; } }
    else {   // a cut
      gate_ = false; active_ = false; pending_ = false;
      vca_ = 0; env_ = envS_ = 0; accEnv_ = accCap_ = 0;
      clearFilter();
    }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    const auto g = [&](Param p) { return static_cast<double>(params[slots[p]]); };
    // Did this row write the flags? A blank cell is not a write, so the flag counts for this block's note only.
    // (the very first block counts too: the song may start on a flagged row)
    const std::int64_t sStamp = clock_ ? clock_->stamps[static_cast<std::size_t>(slots[kSld])] : 0;
    const std::int64_t aStamp = clock_ ? clock_->stamps[static_cast<std::size_t>(slots[kAcc])] : 0;
    const bool slideFlag = sStamp != lastS_ && (lastS_ != -1 || sStamp > 0) && g(kSld) >= 1;
    const bool accFlag = aStamp != lastA_ && (lastA_ != -1 || aStamp > 0) && g(kAcc) >= 1;
    lastS_ = sStamp;
    lastA_ = aStamp;
    if (pending_) {
      const bool sliding = slideFlag && gate_;
      velocity_ = pendVel_;
      target_ = pendNote_;
      if (!sliding) {
        pitch_ = target_;
        env_ = 1;
        stage_ = Stage::Attack;
        accNote_ = accFlag;
        if (accFlag) { accNow_ = 1 + (g(kAcc) - 1) / 14; accEnv_ = accNow_; accCap_ += (1 - accCap_) * 0.45; }
      }
      gate_ = true;
      active_ = true;
      pending_ = false;
    }
    if (!active_) {
      std::fill(outLeft, outLeft + n, 0.0f);
      std::fill(outRight, outRight + n, 0.0f);
      return;
    }
    const double cutoff = g(kCutoff), res = g(kRes), envmod = g(kEnvmod) * 5, accent = g(kAccent), drive = g(kDrive), level = g(kLevel) * 0.9;
    const double decayMs = accNote_ ? std::min(g(kDecay), 200.0) : g(kDecay);
    const double envCoef = js::exp(-6.9078 / (sr_ * decayMs * 0.001));
    const double slideCoef = 1 - js::exp(-3 / (sr_ * g(kSlide) * 0.001));
    const double relCoef = js::exp(-6.9078 / (sr_ * g(kRel) * 0.001));
    const double tune = g(kTune), vel = g(kVel);
    const bool square = g(kWave) >= 0.5;
    const double velGain = 1 - vel + vel * velocity_, driveGain = 1 + drive * 7, driveNorm = std::sqrt(driveGain);
    double inc = 0, G = 0, k = 0, G4 = 0;
    double* s = s_;
    for (int i = 0; i < n; i++) {
      if ((i & 7) == 0) {   // pitch, cutoff and the filter's coefficients are worked out every 8 samples
        inc = 440 * js::pow(2.0, (pitch_ + tune - 69) / 12) / sr_;
        const double accSum = std::min(2.0, accEnv_ + 0.6 * accCap_);
        const double octs = envmod * envS_ * (1 + accent * accSum) + accent * 0.6 * accSum;
        const double fc = std::min(0.45 * sr_, std::max(20.0, cutoff * js::pow(2.0, octs)));
        const double gg = js::tan(kPi * fc / (2 * sr_));   // the filter runs twice per sample
        G = gg / (1 + gg);
        G4 = G * G * G * G;
        k = 4.2 * std::min(1.0, res + accent * 0.12 * accSum);
      }
      // the oscillator, band-limited
      phase_ += inc;
      if (phase_ >= 1) phase_ -= 1;
      double x;
      if (square) x = (phase_ < 0.5 ? 1 : -1) + polyBlep(phase_, inc) - polyBlep(jsMod(phase_ + 0.5, 1), inc);
      else x = 2 * phase_ - 1 - polyBlep(phase_, inc);
      const double xin = x * 1.6;
      // the ladder, twice per sample
      double y = 0;
      for (int sub = 0; sub < 2; sub++) {
        const double rest = (1 - G) * (G * G * G * s[0] + G * G * s[1] + G * s[2] + s[3]);
        const double y4 = (G4 * xin + rest) / (1 + k * G4);
        const double u = softClip(xin - k * y4);
        const double v1 = (u - s[0]) * G, y1 = v1 + s[0]; s[0] = y1 + v1;
        const double v2 = (y1 - s[1]) * G, y2 = v2 + s[1]; s[1] = y2 + v2;
        const double v3 = (y2 - s[2]) * G, y3 = v3 + s[2]; s[2] = y3 + v3;
        const double v4 = (y3 - s[3]) * G, y4n = v4 + s[3]; s[3] = y4n + v4;
        y += y4n * 0.5;
      }
      // amplitude: a quick attack, a small settle, held until the note off; an accent adds a transient on top
      if (stage_ == Stage::Attack) { vca_ += attackStep_; if (vca_ >= 1) { vca_ = 1; stage_ = Stage::Held; } }
      else if (stage_ == Stage::Held) vca_ = 0.85 + (vca_ - 0.85) * heldCoef_;
      else vca_ *= relCoef;
      double o = y * vca_ * (1 + accent * accEnv_ * 1.3) * velGain;
      o = drive > 0 ? softClip(o * driveGain) / driveNorm * 1.4 : o;
      const double r = o - dcx_ + 0.995 * dc_;
      dcx_ = o;
      dc_ = r;
      outLeft[i] = outRight[i] = static_cast<float>(r * level);
      env_ *= envCoef;
      envS_ += (env_ - envS_) * envSmooth_;
      accEnv_ *= accDecay_;
      accCap_ *= capDecay_;
      pitch_ += (target_ - pitch_) * slideCoef;
      if (std::abs(target_ - pitch_) < 1e-3) pitch_ = target_;
    }
    if (!std::isfinite(dc_ + dcx_ + s[0] + s[3])) clearFilter();
    if (stage_ == Stage::Release && vca_ < 1e-5) { active_ = false; vca_ = 0; clearFilter(); }
  }

private:
  void clearFilter() {
    s_[0] = s_[1] = s_[2] = s_[3] = 0;
    dc_ = dcx_ = 0;
  }

  double sr_;
  const Clock* clock_;
  double attackStep_, heldCoef_, envSmooth_, accDecay_, capDecay_;
  double phase_ = 0, pitch_ = 36, target_ = 36;
  bool gate_ = false, active_ = false;
  bool pending_ = false;        // a note is waiting for the next block
  int pendNote_ = 0;
  double pendVel_ = 0;
  Stage stage_ = Stage::Release;
  double vca_ = 0, velocity_ = 1, env_ = 0, envS_ = 0;
  bool accNote_ = false;
  double accEnv_ = 0, accCap_ = 0, accNow_ = 0;
  std::int64_t lastS_ = -1, lastA_ = -1;   // how often the Sld and Acc columns had been written at the last block
  double s_[4] = {0, 0, 0, 0};             // the ladder's four stages
  double dc_ = 0, dcx_ = 0;
};

}  // namespace

void registerSquelch(NodeRegistry& registry) {
  NodeType type;
  type.type = "squelch";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.shared = true;   // one voice for the instrument
  type.params = {
    {"wave", 0, 1, 0, true},        // 0 saw, 1 square
    {"tune", -48, 48, 0},           // semitones
    {"cutoff", 30, 12000, 700},     // Hz, before the envelope
    {"res", 0, 1, 0.65},
    {"envmod", 0, 1, 0.55},         // how far the envelope opens the filter, up to 5 octaves
    {"decay", 30, 3000, 350},       // ms for the filter envelope to fall 60 dB
    {"accent", 0, 1, 0.6},          // how much an accent adds
    {"slide", 1, 500, 60},          // ms
    {"drive", 0, 1, 0.25},
    {"rel", 1, 500, 12},            // ms, amplitude release after a note off
    {"vel", 0, 1, 0.3},             // how much velocity changes the level
    {"level", 0, 1, 0.7},
    {"acc", 0, 15, 0, true},        // the Acc column: 1 an accent, up to F a stronger one
    {"sld", 0, 1, 0, true},         // the Sld column: 1 slides into the note
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Squelch>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

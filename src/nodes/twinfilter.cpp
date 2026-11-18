// twin: two multimode filters that run in series, in parallel or anywhere between, with the second optionally locked to the first at a ratio
// (or both following the pitch of the input), drive and tilt in front, noise and feedback, and a follower and an LFO on each cutoff.
// Each filter is a zero-delay (TPT) state-variable filter with a soft-clipped band state, with its own low, band and high levels.
// Port of webtracker src/audio/nodes/twin.js; the panel's picture is in the app (twinresponse.js there, twin_response here).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/bezier.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param {
  kDrive = 0, kTilt, kTiltFreq, kTrigger, kCut1, kRes1, kLow1, kBand1, kHigh1, kCut2, kRes2, kLow2, kBand2, kHigh2,
  kBlend, kLock, kRatio, kDetune, kTrack, kTranspose, kLfoRate, kLfoShape, kLfo1, kLfo2,
  kFAttack, kFRelease, kFSens, kFol1, kFol2, kNoise, kFeedback, kFbSource, kMix, kLevel,
  kTime1, kTime2, kDepth1, kDepth2, kPick1, kPick2
};

enum Phase { kIdle, kRun, kHold, kRelease, kFade };
constexpr double kFadeMin = 0.01, kFadeMax = 0.06;   // seconds

// One drawn envelope played in blocks: the same behaviour as env2's, started and let go by the twin's trigger instead of by notes.
class DrawnEnvelope {
public:
  DrawnEnvelope(double sampleRate, const Json* data, const char* slot) : sampleRate_(sampleRate) { set(bezier::drawingOf(data, slot)); }

  void set(const bezier::Spline& points) {
    table_ = bezier::envelopeTable(points);
    knot_ = bezier::sustainKnot(points);
  }
  void start() { fresh_ = true; pendingOff_ = false; }
  void release() { pendingOff_ = true; }

  // The value over the next m samples (the drawing is len samples long), then moves on by m.
  double step(int m, double len) {
    if (fresh_) {
      fresh_ = false;
      offset_ = std::fabs(last_) > 1e-3 ? last_ - bezier::readEnvelope(table_, 0) : 0;
      phase_ = kRun; pos_ = 0;
    }
    if (phase_ == kIdle) return 0;
    if (pendingOff_) {
      pendingOff_ = false;
      if (knot_ && phase_ == kRun) {   // ended before the knot: a straight fade from here to the last anchor's level
        fadeLen_ = fadeLeft_ = jsRound(std::min(kFadeMax, std::max(kFadeMin, (1 - knot_->x) * len / sampleRate_)) * sampleRate_);
        fadeFrom_ = shape_;
        phase_ = kFade;
      } else if (knot_ && phase_ == kHold) { pos_ = knot_->x * len; phase_ = kRelease; }
    }
    double x = pos_ / len;
    if (phase_ == kRun && knot_ && x >= knot_->x) { phase_ = kHold; pos_ = knot_->x * len; x = knot_->x; }
    if (phase_ == kFade) {
      const double end = table_[bezier::kEnvPoints];
      shape_ = end + (fadeFrom_ - end) * (fadeLeft_ / fadeLen_);
      fadeLeft_ = std::max(0.0, fadeLeft_ - m);
    } else {
      shape_ = bezier::readEnvelope(table_, x);
    }
    last_ = shape_;
    offset_ *= js::exp(-m / (0.002 * sampleRate_));
    if (std::fabs(offset_) < 1e-9) offset_ = 0;
    if (phase_ == kRun || phase_ == kRelease) pos_ += m;
    return last_ + offset_;
  }

private:
  double sampleRate_;
  bezier::Table table_;
  std::optional<bezier::Knot> knot_;
  int phase_ = kIdle;
  double pos_ = 0, shape_ = 0, last_ = 0, offset_ = 0, fadeFrom_ = 0, fadeLeft_ = 0, fadeLen_ = 1;
  bool fresh_ = false, pendingOff_ = false;
};

// smooth tanh-like, exact at +-3
double clip(double x) { return x > 3 ? 1 : x < -3 ? -1 : x * (27 + x * x) / (27 + 9 * x * x); }

class TwinFilter final : public Node {
public:
  explicit TwinFilter(const NodeContext& context)
      : sr_(context.sampleRate),
        cs_(1 - js::exp(-4 / (context.sampleRate * 0.004))),
        envA_(context.sampleRate, context.data, "a"), envB_(context.sampleRate, context.data, "b"),
        mod1On_(context.connected.size() > 1 && context.connected[1]),
        mod2On_(context.connected.size() > 2 && context.connected[2]),
        pLowCoef_(1 - js::exp(-2 * kPi * 1500 / context.sampleRate)),
        minPeriod_(context.sampleRate / 2000),
        maxPeriod_(context.sampleRate / 40) {}

  void data(const Json& data) override {
    envA_.set(bezier::drawingOf(&data, "a"));
    envB_.set(bezier::drawingOf(&data, "b"));
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* input = inputs[0];
    const float* mod1 = inputs[1];
    const float* mod2 = inputs[2];
    float* out = outputs[0];
    const double sr = sr_;
    auto P = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double drive = P(kDrive), pre = 1 + drive * 9, post = 1 / (1 + drive * 2);
    const bool driven = drive > 0.001;
    const double tilt = P(kTilt), tiltCoef = 1 - js::exp(-2 * kPi * std::min(P(kTiltFreq), sr * 0.4) / sr);
    const bool tilted = tilt != 0;
    const double gLow = js::pow(10, -tilt / 40), gHigh = js::pow(10, tilt / 40);
    const double trigger = P(kTrigger), sens = P(kFSens);
    const double cut1 = P(kCut1), cut2 = P(kCut2);
    const double res1 = std::min(std::max(P(kRes1), 0.0), 1.0), res2 = std::min(std::max(P(kRes2), 0.0), 1.0);
    const double l1 = P(kLow1), b1 = P(kBand1), h1 = P(kHigh1), l2 = P(kLow2), b2 = P(kBand2), h2 = P(kHigh2);
    const double blend = std::min(std::max(P(kBlend), 0.0), 1.0);
    const bool lock = jsRoundInt(P(kLock)) == 1;
    const double ratio = P(kRatio), detune = js::pow(2, P(kDetune) / 1200);
    const bool track = jsRoundInt(P(kTrack)) == 1;
    const double transpose = js::pow(2, jsRoundInt(P(kTranspose)) / 12.0);
    const double lfoStep = P(kLfoRate) * 4 / sr;
    const int lfoShape = jsRoundInt(P(kLfoShape));
    const double lfo1 = P(kLfo1) * 2, lfo2 = P(kLfo2) * 2;
    const double attack = 1 - js::exp(-1 / (sr * P(kFAttack) / 1000)), release = 1 - js::exp(-1 / (sr * P(kFRelease) / 1000));
    attackRate_ = attack; releaseRate_ = release;
    const double fol1 = P(kFol1) * 3, fol2 = P(kFol2) * 3;
    const double noiseAmt = P(kNoise) * 0.5, feedback = P(kFeedback) * 0.9;
    const bool fbFinal = jsRoundInt(P(kFbSource)) == 1;
    const double mix = P(kMix), level = P(kLevel);
    const double k1 = 2 - 2.04 * res1, kb1 = std::max(k1, 0.02), k2 = 2 - 2.04 * res2, kb2 = std::max(k2, 0.02);
    const double kick1 = res1 > 0.97 ? 1e-5 : 0, kick2 = res2 > 0.97 ? 1e-5 : 0;
    const double wetLevel = 1 / (1 + blend);
    const double lenA = std::max(1.0, P(kTime1) * 0.001 * sr), lenB = std::max(1.0, P(kTime2) * 0.001 * sr);
    const double depthA = P(kDepth1), depthB = P(kDepth2);
    const bool pickA = jsRoundInt(P(kPick1)) == 1, pickB = jsRoundInt(P(kPick2)) == 1;
    for (int i = 0; i < n; i += 4) {
      const int m = std::min(4, n - i);
      double lfo;
      const double ph = lfoPhase_;
      if (lfoShape == 0) lfo = js::sin(2 * kPi * ph);
      else if (lfoShape == 1) lfo = ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;
      else if (lfoShape == 2) lfo = 1 - 2 * ph;
      else lfo = ph < 0.5 ? 1 : -1;
      lfoPhase_ += lfoStep;
      if (lfoPhase_ >= 1) lfoPhase_ -= std::floor(lfoPhase_);
      const double db = 20 * js::log10(env_ + 1e-9);
      if (armed_) { if (db >= trigger) { armed_ = false; envA_.start(); envB_.start(); } }
      else if (db < trigger - 6) { armed_ = true; envA_.release(); envB_.release(); }
      const double shapeA = envA_.step(m, lenA), shapeB = envB_.step(m, lenB);
      const double level01 = std::min(std::max((db - trigger) / 30 * (0.5 + sens), 0.0), 1.5);
      double tracked = 0;
      if (track && pitchSeen_) tracked = std::min(std::max(pitch_ * transpose, 20.0), 18000.0);
      const double base1 = tracked > 0 && lock ? tracked / ratio : cut1;
      const double o1 = std::max(-8.0, std::min(8.0, (mod1On_ ? static_cast<double>(mod1[i]) : 0.0) + lfo * lfo1 + level01 * fol1 + (pickA ? shapeB : shapeA) * depthA));
      const double o2 = std::max(-8.0, std::min(8.0, (mod2On_ ? static_cast<double>(mod2[i]) : 0.0) + lfo * lfo2 + level01 * fol2 + (pickB ? shapeB : shapeA) * depthB));
      const double t1 = base1 * js::pow(2, o1);
      const double t2 = lock ? t1 * ratio * detune * js::pow(2, o2) : (tracked > 0 ? tracked : cut2) * detune * js::pow(2, o2);
      if (fc1_ < 0) { fc1_ = t1; fc2_ = t2; }
      fc1_ += (t1 - fc1_) * cs_;
      fc2_ += (t2 - fc2_) * cs_;
      const double f1 = std::min(std::max(fc1_, 5.0), sr * 0.45), f2 = std::min(std::max(fc2_, 5.0), sr * 0.45);
      const double g1 = js::tan(kPi * f1 / sr), a11 = 1 / (1 + g1 * (g1 + k1)), a12 = g1 * a11, a13 = g1 * a12;
      const double g2 = js::tan(kPi * f2 / sr), a21 = 1 / (1 + g2 * (g2 + k2)), a22 = g2 * a21, a23 = g2 * a22;
      for (int j = i; j < i + m; j++) {
        const double dry = input[j];
        listen(dry);
        rng_ = rng_ * 1664525u + 1013904223u;
        noiseRng_ = noiseRng_ * 69069u + 1u;
        double x = dry;
        if (noiseAmt > 0) x += (noiseRng_ / 4294967296.0 - 0.5) * 2 * noiseAmt;
        if (feedback > 0) x += feedback * clip(fb_);
        if (driven) x = clip(pre * x) * post;
        if (tilted) { tiltLow_ += (x - tiltLow_) * tiltCoef; x = tiltLow_ * gLow + (x - tiltLow_) * gHigh; }
        if (kick1 != 0 && l1 + b1 + h1 != 0) x += (rng_ / 4294967296.0 - 0.5) * kick1;
        step(0, x, a11, a12, a13);
        const double y1 = l1 * v2_ + b1 * kb1 * v1_ + h1 * (x - k1 * v1_ - v2_);
        double x2 = x + (y1 - x) * (1 - blend);
        if (kick2 != 0) x2 += (rng_ / 4294967296.0 - 0.5) * kick2;
        step(2, x2, a21, a22, a23);
        const double y2 = l2 * v2_ + b2 * kb2 * v1_ + h2 * (x2 - k2 * v1_ - v2_);
        const double y = (blend * y1 + y2) * wetLevel;
        fb_ = fbFinal ? y : y1;
        out[j] = static_cast<float>((dry + (y - dry) * mix) * level);
      }
    }
    for (double& s : st_) if (std::fabs(s) < 1e-20) s = 0;
    if (std::fabs(tiltLow_) < 1e-20) tiltLow_ = 0;
    if (std::fabs(fb_) < 1e-20) fb_ = 0;
    if (std::fabs(env_) < 1e-20) env_ = 0;
  }

private:
  // The follower and the pitch follower, both listening to the dry input: positive-going zero crossings of the low-passed input, accepted
  // when two in a row agree.
  void listen(double dry) {
    const double a = std::fabs(dry);
    env_ += (a - env_) * (a > env_ ? attackRate_ : releaseRate_);
    pLow_ += (dry - pLow_) * pLowCoef_;
    peak_ = std::max(std::fabs(pLow_), peak_ * 0.9995);
    const double th = peak_ * 0.25;
    sinceCross_++;
    if (!positive_ && pLow_ > th && peak_ > 0.002) {
      positive_ = true;
      if (sinceCross_ >= minPeriod_ && sinceCross_ <= maxPeriod_) {
        const double p = sr_ / sinceCross_;
        if (pitch_ > 0 && std::fabs(js::log2(p / pitch_)) < 0.3) { pitch_ += (p - pitch_) * 0.35; candidate_ = 0; }
        else if (candidate_ > 0 && std::fabs(js::log2(p / candidate_)) < 0.15) { pitch_ = p; candidate_ = 0; pitchSeen_ = true; }
        else candidate_ = p;
        if (pitch_ > 0) pitchSeen_ = true;
      }
      sinceCross_ = 0;
    } else if (positive_ && pLow_ < -th) positive_ = false;
    if (sinceCross_ > sr_) sinceCross_ = sr_;
  }

  // One TPT state-variable step on the state at offset o; leaves band in v1_ and low in v2_.
  void step(int o, double x, double a1, double a2, double a3) {
    const double v3 = x - st_[o + 1];
    double v1 = a1 * st_[o] + a2 * v3;
    v1 = 2 * clip(0.5 * v1);
    const double v2 = st_[o + 1] + a2 * st_[o] + a3 * v3;
    st_[o] = 2 * v1 - st_[o];
    st_[o + 1] = 2 * v2 - st_[o + 1];
    v1_ = v1; v2_ = v2;
  }

  double sr_, cs_;
  DrawnEnvelope envA_, envB_;
  bool armed_ = true;
  bool mod1On_, mod2On_;
  double pLowCoef_, minPeriod_, maxPeriod_;
  double attackRate_ = 0, releaseRate_ = 0;
  double st_[4] = {};
  double fc1_ = -1, fc2_ = -1;
  std::uint32_t rng_ = 22222, noiseRng_ = 7777;
  double lfoPhase_ = 0, env_ = 0, tiltLow_ = 0, fb_ = 0;
  double pLow_ = 0, peak_ = 0, sinceCross_ = 0, pitch_ = 0, candidate_ = 0;
  bool positive_ = false, pitchSeen_ = false;
  double v1_ = 0, v2_ = 0;
};

}  // namespace

void registerTwinFilter(NodeRegistry& registry) {
  NodeType type;
  type.type = "twin";
  type.inputs = {"in", "mod1", "mod2"};   // mod1 and mod2 are in octaves, applied sample by sample to filter 1 and filter 2
  type.outputs = {"out"};
  type.dual = true;
  type.isEffect = true;
  type.kinds = {{"mod1", PortKind::Control}, {"mod2", PortKind::Control}};
  type.params = {
    {"drive", 0, 1, 0},
    {"tilt", -12, 12, 0},
    {"tiltfreq", 200, 8000, 2000},
    {"trigger", -60, 0, -30},
    {"cutoff1", 20, 18000, 1500},
    {"res1", 0, 1, 0.25},
    {"low1", -1, 1, 1},
    {"band1", -1, 1, 0},
    {"high1", -1, 1, 0},
    {"cutoff2", 20, 18000, 1500},
    {"res2", 0, 1, 0.25},
    {"low2", -1, 1, 1},
    {"band2", -1, 1, 0},
    {"high2", -1, 1, 0},
    {"blend", 0, 1, 0},
    {"lock", 0, 1, 0, true},
    {"ratio", 0.25, 4, 1.5},
    {"detune", -100, 100, 0},
    {"track", 0, 1, 0, true},
    {"transpose", -24, 24, 0, true},
    {"lforate", 0.02, 40, 0.4},
    {"lfoshape", 0, 3, 0, true},
    {"lfo1", -1, 1, 0},
    {"lfo2", -1, 1, 0},
    {"fattack", 1, 200, 5},
    {"frelease", 10, 2000, 120},
    {"fsens", 0, 1, 0.5},
    {"fol1", -1, 1, 0},
    {"fol2", -1, 1, 0},
    {"noise", 0, 1, 0},
    {"feedback", 0, 1, 0},
    {"fbsource", 0, 1, 0, true},
    {"mix", 0, 1, 1},
    {"level", 0, 2, 1},
    {"time1", 1, 20000, 400},
    {"time2", 1, 20000, 400},
    {"depth1", -4, 4, 0},
    {"depth2", -4, 4, 0},
    {"pick1", 0, 1, 0, true},
    {"pick2", 0, 1, 1, true},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<TwinFilter>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

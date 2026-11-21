// Pogue: a grubby multi-voice chorus. Several voices read the sound back from a short delay line, each at a delay that
// wanders between `mindelay` and `maxdelay`; the voices are averaged, filtered, saturated and bit-reduced, and that wet sound is
// fed back into the line. The voices move on a slow sine each, drawn towards random targets by `change`, picked anew on a timer,
// on a transient or pitch step in the sound (`onset`), or on a pattern of steps on the song's grid (`pattern`).
// Port of webtracker src/audio/nodes/pogue.js (without the panel's reports).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param {
  kPregain = 0, kVoices, kRate, kResolution, kChange, kMindelay, kMaxdelay, kStereo, kInterp, kFeedback, kXfeedback, kSaturation,
  kDegrade, kDither, kErrormix, kLpcut, kLpq, kHpcut, kHpq, kInertia, kPostgain, kMix, kOnset, kPattern, kPatlen, kPatrate, kMoves, kLookahead
};

const double kRates[] = {0.25, 1.0 / 3, 0.5, 2.0 / 3, 1, 1.5, 2, 3, 4, 6, 8, 16};
constexpr int kRateCount = 12;
constexpr int kMaxVoices = 8;
constexpr double kMaxMs = 60, kLookMs = 10, kHoldoffMs = 30;
constexpr int kMovePhase = 1;
constexpr int kReportEvery = 1600;   // the picture's reports: about every 35 ms, while it keeps asking
constexpr int kWide = 1, kSpread = 2, kNearest = 0, kLinear = 1;

struct Svf { double g, k, a1, a2, a3; };
Svf svfCoefs(double cutoff, double q, double sampleRate) {
  const double g = js::tan(kPi * std::min(cutoff, sampleRate * 0.45) / sampleRate), k = 1 / q;
  const double a1 = 1 / (1 + g * (g + k));
  return {g, k, a1, g * a1, g * g * a1};
}
double svf(double* state, const Svf& c, double x, bool high) {
  const double v3 = x - state[1];
  const double v1 = c.a1 * state[0] + c.a2 * v3, v2 = state[1] + c.a2 * state[0] + c.a3 * v3;
  state[0] = 2 * v1 - state[0];
  state[1] = 2 * v2 - state[1];
  return high ? x - c.k * v1 - v2 : v2;
}

struct Voice { double phase, speed, target, want = -1, delay = -1; };

class Pogue final : public Node {
public:
  explicit Pogue(const NodeContext& context)
      : sampleRate_(context.sampleRate), clock_(context.clock), probe_(context.probe),
        size_(static_cast<int>(std::ceil(sampleRate_ * kMaxMs / 1000)) + 4),
        bufL_(static_cast<std::size_t>(size_), 0.0f), bufR_(static_cast<std::size_t>(size_), 0.0f),
        look_(static_cast<int>(jsRound(kLookMs * sampleRate_ / 1000))),
        lookL_(static_cast<std::size_t>(look_), 0.0f), lookR_(static_cast<std::size_t>(look_), 0.0f) {
    for (auto& set : voices_)
      for (Voice& v : set) {
        v.phase = random();
        const double s = random();
        v.speed = 1.0 / 3 + (2.0 / 3) * s;
        v.target = random();
      }
    fastCoef_ = 1 - js::exp(-1 / (0.0008 * sampleRate_));
    slowCoef_ = 1 - js::exp(-1 / (0.06 * sampleRate_));
    holdoff_ = static_cast<int>(jsRound(kHoldoffMs * sampleRate_ / 1000));
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inL = inputs[0];
    const float* inR = inputs[1];
    float* outL = outputs[0];
    float* outR = outputs[1];
    auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double pre = js::pow(10, p(kPregain) / 20), post = js::pow(10, p(kPostgain) / 20);
    const int count = std::max(1, std::min(kMaxVoices, jsRoundInt(p(kVoices))));
    const double rate = p(kRate), change = p(kChange);
    const double lo = std::min(p(kMindelay), p(kMaxdelay)), hi = std::max(p(kMindelay), p(kMaxdelay));
    const int stereo = jsRoundInt(p(kStereo)), interp = jsRoundInt(p(kInterp));
    const double feedback = p(kFeedback), xfeedback = p(kXfeedback), saturation = p(kSaturation);
    const double bits = 16 - 14 * p(kDegrade), levels = p(kDegrade) > 0 ? js::pow(2, bits - 1) : 0;
    const double dither = p(kDither), errormix = p(kErrormix), mix = p(kMix);
    const Svf hp = svfCoefs(p(kHpcut), p(kHpq), sampleRate_), lp = svfCoefs(p(kLpcut), p(kLpq), sampleRate_);
    const double onset = p(kOnset);
    const long steps = static_cast<long>(jsRound(p(kPattern)));
    const int len = jsRoundInt(p(kPatlen)), moves = jsRoundInt(p(kMoves));
    const bool timer = onset <= 0 && steps <= 0;
    const bool aligned = jsRoundInt(p(kLookahead)) == 1 && onset > 0;
    if (steps > 0) stepsInBlock(n, kRates[std::max(0, std::min(kRateCount - 1, jsRoundInt(p(kPatrate))))], steps, len);
    else { fires_.clear(); patternSeen_ = false; }
    std::size_t nextFire = 0;
    const bool reporting = probe_ && probe_->watched();
    const int hop = std::max(1, jsRoundInt(sampleRate_ / p(kResolution)));
    const double glide = p(kInertia) > 0 ? 1 - js::exp(-1000 / (sampleRate_ * p(kInertia))) : 1;
    for (int v = 0; v < count; v++) {
      const double pan = count > 1 ? static_cast<double>(v) / (count - 1) : 0.5;
      pans_[static_cast<std::size_t>(v * 2)] = static_cast<float>(js::cos(pan * kPi / 2) * kSqrt2);
      pans_[static_cast<std::size_t>(v * 2 + 1)] = static_cast<float>(js::sin(pan * kPi / 2) * kSqrt2);
    }

    for (int i = 0; i < n; i++) {
      if (untilMove_ <= 0) { moveVoices(count, rate, change, lo, hi, static_cast<double>(hop) / sampleRate_, timer); untilMove_ = hop; }
      untilMove_--;
      const double xl = inL[i], xr = inR[i];
      if (onset > 0 && detect(0.5 * (xl + xr), onset)) fire(count, moves, change, lo, hi);
      else if (onset <= 0 && reporting) detect(0.5 * (xl + xr), 0.5);   // (while the picture is watched the detector runs even at 0, to show what it would do)
      while (nextFire < fires_.size() && fires_[nextFire] <= i) { fire(count, moves, change, lo, hi); repFired_ |= 4; nextFire++; }

      double sumL = 0, sumR = 0;
      for (int v = 0; v < count; v++) {
        Voice& left = voices_[0][static_cast<std::size_t>(v)];
        Voice& right = stereo == kWide ? voices_[1][static_cast<std::size_t>(v)] : left;
        left.delay += (left.want - left.delay) * glide;
        if (&right != &left) right.delay += (right.want - right.delay) * glide;
        if (stereo == kSpread) {
          const double a = read(bufL_, left.delay, interp), b = read(bufR_, left.delay, interp);
          const double both = 0.5 * (a + b);
          sumL += both * pans_[static_cast<std::size_t>(v * 2)];
          sumR += both * pans_[static_cast<std::size_t>(v * 2 + 1)];
        } else {
          sumL += read(bufL_, left.delay, interp);
          sumR += read(bufR_, right.delay, interp);
        }
      }
      double l = sumL / count, r = sumR / count;
      l = svf(filters_[2], lp, svf(filters_[0], hp, l, true), false);
      r = svf(filters_[3], lp, svf(filters_[1], hp, r, true), false);
      l = degrade(saturate(l, saturation), levels, dither, errormix, 0);
      r = degrade(saturate(r, saturation), levels, dither, errormix, 1);
      const double wetL = std::abs(l) < 1e-20 ? 0 : l;
      const double wetR = std::abs(r) < 1e-20 ? 0 : r;

      double heardL = xl, heardR = xr;
      if (look_ > 0) {
        const std::size_t at = static_cast<std::size_t>(lookAt_);
        const double oldL = lookL_[at], oldR = lookR_[at];
        lookL_[at] = static_cast<float>(heardL);
        lookR_[at] = static_cast<float>(heardR);
        if (++lookAt_ == look_) lookAt_ = 0;
        if (aligned) { heardL = oldL; heardR = oldR; }
      }
      const double dryL = heardL * pre, dryR = heardR * pre;
      bufL_[static_cast<std::size_t>(write_)] = static_cast<float>(dryL + feedback * wetL + xfeedback * wetR);
      bufR_[static_cast<std::size_t>(write_)] = static_cast<float>(dryR + feedback * wetR + xfeedback * wetL);
      if (++write_ == size_) write_ = 0;

      outL[i] = static_cast<float>((dryL + wetL * mix) * post);
      outR[i] = static_cast<float>((dryR + wetR * mix) * post);
    }
    if (reporting) {
      sinceReport_ += n;
      if (sinceReport_ >= kReportEvery) { sinceReport_ = 0; report(count); }
    }
  }

private:
  // What the picture is told (see app/pogue_picture.cpp): the report's number, the voices, the loudest level, the highest fast/slow ratio and
  // which triggers fired (1 hit, 2 pitch step, 4 pattern) since the last report, then each voice's delay in ms (left side, right side).
  void report(int count) {
    std::vector<float> values = {static_cast<float>(++reportNumber_), static_cast<float>(count), static_cast<float>(repLevel_), static_cast<float>(repRatio_), static_cast<float>(repFired_)};
    const double ms = 1000 / sampleRate_;
    for (const auto& set : voices_)
      for (int v = 0; v < count; v++) values.push_back(static_cast<float>(set[static_cast<std::size_t>(v)].delay * ms));
    probe_->publish(values, {});
    repLevel_ = 0; repRatio_ = 1; repFired_ = 0;
  }

  double random() {
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return seed_ / 4294967296.0;
  }

  double read(const std::vector<float>& buf, double delaySamples, int interp) const {
    double pos = write_ - delaySamples;
    if (pos < 0) pos += size_;
    const int k = static_cast<int>(pos);
    const double f = pos - k;
    auto at = [&](int i) { return static_cast<double>(buf[static_cast<std::size_t>((k + i + size_) % size_)]); };
    if (interp == kNearest) return f < 0.5 ? at(0) : at(1);
    if (interp == kLinear) return at(0) + (at(1) - at(0)) * f;
    const double y0 = at(-1), y1 = at(0), y2 = at(1), y3 = at(2);
    const double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
  }

  static double saturate(double x, double amount) {
    if (amount <= 0) return x;
    const double drive = 1 + 9 * amount;
    return js::tanh(x * drive) / js::tanh(drive);
  }

  double degrade(double x, double levels, double dither, double errormix, int side) {
    if (levels <= 0) return x;
    double& err = side == 0 ? errL_ : errR_;
    const double shaped = x - dither * err;
    const double rounded = jsRound(shaped * levels) / levels;
    err = rounded - shaped;
    const double error = x - rounded;
    return rounded + (error - rounded) * errormix;
  }

  void aim(Voice& voice, double change, double lo, double hi) const {
    const double sine = 0.5 + 0.5 * js::sin(2 * kPi * voice.phase);
    const double where = sine + (voice.target - sine) * change;
    voice.want = (lo + (hi - lo) * where) * sampleRate_ / 1000;
    if (voice.delay < 0) voice.delay = voice.want;
  }
  void moveVoices(int count, double rate, double change, double lo, double hi, double stepSeconds, bool timer) {
    for (auto& set : voices_)
      for (int v = 0; v < count; v++) {
        Voice& voice = set[static_cast<std::size_t>(v)];
        const double hz = rate * voice.speed;
        voice.phase += hz * stepSeconds;
        if (voice.phase >= 1) voice.phase -= std::floor(voice.phase);
        if (timer && random() < change * hz * stepSeconds * 2) voice.target = random();
        aim(voice, change, lo, hi);
      }
  }
  void fire(int count, int moves, double change, double lo, double hi) {
    for (auto& set : voices_)
      for (int v = 0; v < count; v++) {
        Voice& voice = set[static_cast<std::size_t>(v)];
        if (moves != kMovePhase) voice.target = random();
        if (moves >= kMovePhase) voice.phase = random();
        aim(voice, change, lo, hi);
      }
  }

  // Onset: a fast and a slow follower of the level; a hit is the fast one jumping well above the slow one. Pitch: the time
  // between rising zero crossings of a low-passed copy; two new periods that agree and differ from the three before are a step.
  int detect(double x, double sensitivity) {
    constexpr double lpCoef = 0.25, kFloor = 0.004;
    const double a = std::abs(x);
    det_.fast += (a - det_.fast) * fastCoef_;
    det_.slow += (a - det_.slow) * slowCoef_;
    const double before = det_.lp;
    det_.lp += (x - det_.lp) * lpCoef;
    det_.since++;
    if (det_.hold > 0) det_.hold--;
    int hit = 0;
    const double ratio = det_.fast > kFloor ? det_.fast / (det_.slow + 1e-9) : 1;
    if (ratio > repRatio_) repRatio_ = ratio;
    if (det_.fast > repLevel_) repLevel_ = det_.fast;
    if (det_.hold == 0 && ratio > 1.2 + 8 * js::pow(1 - sensitivity, 2)) hit = 1;
    if (before <= 0 && det_.lp > 0 && det_.fast > kFloor) {
      const double period = det_.since;
      det_.since = 0;
      if (period > sampleRate_ / 6000 && period < sampleRate_ / 25) {
        for (int i = 4; i > 0; i--) det_.periods[static_cast<std::size_t>(i)] = det_.periods[static_cast<std::size_t>(i - 1)];
        det_.periods[0] = period;
        det_.count++;
        if (det_.count >= 5 && det_.hold == 0 && hit == 0) {
          const double a0 = det_.periods[0], a1 = det_.periods[1], b0 = det_.periods[2], b1 = det_.periods[3], b2 = det_.periods[4];
          const double semis = 0.6 + 5 * (1 - sensitivity);
          const double step = 12 * std::abs(js::log2(((a0 + a1) / 2) / ((b0 + b1 + b2) / 3))), agree = 12 * std::abs(js::log2(a0 / a1));
          if (step > semis && agree < semis / 2) hit = 2;
        }
      } else det_.count = 0;
    } else if (det_.since > sampleRate_ / 20) det_.count = 0;
    if (hit) { det_.hold = holdoff_; det_.count = 0; repFired_ |= hit; }
    return hit;
  }

  // Pattern: where the song is, in steps. While the song plays the steps sit on its grid; otherwise the node counts its own.
  void stepsInBlock(int n, double patRate, long bitsOf, int len) {
    fires_.clear();
    const double stepSamples = (clock_ ? clock_->tickSamples : sampleRate_ * 0.125) * patRate;
    double start = cursor_;
    if (clock_ && clock_->playing) {
      const double at = clock_->songPos / patRate;
      if (!patternSeen_ || std::abs(at - cursor_) > 0.5) start = at;
    }
    patternSeen_ = true;
    const double end = start + n / stepSamples;
    for (double m = std::ceil(start); m < end; m++) {
      const long mm = static_cast<long>(m);
      if ((bitsOf >> (len - 1 - ((mm % len) + len) % len)) & 1) fires_.push_back(static_cast<int>(std::max(0.0, std::min(static_cast<double>(n - 1), jsRound((m - start) * stepSamples)))));
    }
    cursor_ = end;
  }

  struct Detector { double fast = 0, slow = 0, lp = 0, since = 0; int hold = 0; std::array<double, 5> periods{}; int count = 0; };

  double sampleRate_;
  const Clock* clock_;
  std::shared_ptr<SpectrumProbe> probe_;
  int size_;
  std::vector<float> bufL_, bufR_;
  int write_ = 0;
  std::uint32_t seed_ = 0x2545f491u;
  std::array<std::array<Voice, kMaxVoices>, 2> voices_;
  std::array<float, kMaxVoices * 2> pans_{};
  double filters_[4][2] = {};   // hpL, hpR, lpL, lpR
  double errL_ = 0, errR_ = 0;
  int untilMove_ = 0;
  double fastCoef_ = 0, slowCoef_ = 0;
  int holdoff_ = 0;
  Detector det_;
  double repLevel_ = 0, repRatio_ = 1;
  int repFired_ = 0, sinceReport_ = 0;
  long reportNumber_ = 0;
  double cursor_ = 0;
  bool patternSeen_ = false;
  std::vector<int> fires_;
  int look_;
  std::vector<float> lookL_, lookR_;
  int lookAt_ = 0;
};

}  // namespace

void registerPogue(NodeRegistry& registry) {
  NodeType type;
  type.type = "pogue";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {
    {"pregain", -24, 24, 0},
    {"voices", 1, kMaxVoices, 3, true},
    {"rate", 0.02, 10, 0.8},
    {"resolution", 10, 2000, 1000},
    {"change", 0, 1, 0.3},
    {"mindelay", 0.5, 50, 6},
    {"maxdelay", 0.5, 50, 14},
    {"stereo", 0, 2, 1, true},
    {"interp", 0, 2, 1, true},
    {"feedback", -0.95, 0.95, 0},
    {"xfeedback", -0.95, 0.95, 0},
    {"saturation", 0, 1, 0},
    {"degrade", 0, 1, 0},
    {"dither", 0, 1, 0},
    {"errormix", 0, 1, 0},
    {"lpcut", 200, 20000, 12000},
    {"lpq", 0.5, 8, 0.7},
    {"hpcut", 10, 2000, 60},
    {"hpq", 0.5, 8, 0.7},
    {"inertia", 0, 500, 20},
    {"postgain", -24, 24, 0},
    {"mix", 0, 1, 0.5},
    {"onset", 0, 1, 0},
    {"pattern", 0, 65535, 0, true},
    {"patlen", 1, 16, 8, true},
    {"patrate", 0, kRateCount - 1, 4, true},
    {"moves", 0, 2, 0, true},
    {"lookahead", 0, 1, 0, true},
    {"span", 2, 6, 6},   // seconds of history the voices picture shows (a view setting: the sound ignores it)
  };
  type.latency = [](const std::map<std::string, double>& settings, double sampleRate) {
    return jsRound(settings.at("lookahead")) == 1 && settings.at("onset") > 0 ? jsRound(kLookMs * sampleRate / 1000) : 0.0;
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Pogue>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

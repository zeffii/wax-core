// Mosh: what a video codec does when a keyframe is lost, done to a spectrogram. A codec stores one whole picture now
// and then (a keyframe) and between them only how the picture moves; drop the keyframe and the movement of the new
// scene is laid over the old picture, which smears, slides and blooms. Here the picture is the spectrum of the sound at
// `in`, and the movement comes from the sound at `mod`.
//
// The picture is held per frequency as a loudness, a phase and a frequency (so a held partial keeps sounding as the
// tone it was). At a keyframe, every `keyframe` ms, it is replaced by the live sound. Between keyframes each frame (a
// quarter of the frame length) the spectrum of `mod` is compared with the one before it, in two ways, as a codec would:
//   - brightness: how much louder or quieter each frequency became is added to the picture (`motion`);
//   - motion: around every frequency, the best sideways shift of the modulator's spectrum from one frame to the next
//     is searched for (block matching, like the macroblocks of a video, but overlapping). The picture is slid by that
//     much (`slide`), partials and all. A voice gliding up drags the pad up with it.
// `hold` says how much of the input's own movement is thrown away between keyframes (1: the picture is frozen, only
// the modulator moves it), `heal` lets the loudness drift slowly back toward the live sound, and `bloom` says how far
// new energy may grow in frequencies that were empty at the keyframe (ghost partials). `smooth` is how much the
// modulator's spectrum is blurred before it is measured.
// Stereo, each side on its own. Latency is one frame: 1024, 2048 or 4096 samples (`size`).
// Port of webtracker src/audio/nodes/mosh.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/stft.h"

namespace wax {
namespace {

enum Param { kMotion = 0, kSlide, kKeyframe, kHold, kHeal, kBloom, kSmooth, kMix, kSize };

constexpr double kWindow = 0.25;   // the window each bin matches motion over reaches this fraction of its own number up and down (at least 8 bins)
constexpr int kReach = 6;          // the furthest a block can be found to have moved in one frame, in bins
constexpr double kStep = 18;       // the most any bin's loudness can change in one frame, in dB
constexpr double kFloor = -120;    // dB below full scale: silence
constexpr double kQuiet = -90;     // the modulator below this does not move anything
constexpr double kPi = 3.141592653589793;
constexpr double kTau = 2 * kPi;
constexpr int kBins = kStftMaxSize / 2 + 1;

double wrap(double x) { return x - kTau * jsRound(x / kTau); }   // an angle brought into -pi..pi

double toAmplitude(double db, double peak) { return db <= kFloor ? 0 : peak * js::pow(10.0, db / 20); }

// One side of the stereo pair: its analysers, its output, and the picture.
struct Side {
  Analyser carrier, mod;
  Resynth out;
  std::vector<double> cur;         // the picture's loudness per bin, in dB
  std::vector<double> key;         // the loudness at the last keyframe (moves with the picture)
  std::vector<double> fq;          // the frequency of what is in each bin, in bins (a tone between bins 9 and 10 reads 9.4)
  std::vector<double> ph;          // its phase
  std::vector<double> prevPhase;   // the live sound's phases one frame ago, to measure frequencies from
  std::vector<double> prevA, prevM;
  std::vector<double> carry;       // sideways motion found but not yet a whole bin (moves with the picture)
  int since = 0;
  double peak = kFloor;
  bool primed = false, started = false;

  explicit Side(int n)
      : carrier(n), mod(n), out(n), cur(kBins, kFloor), key(kBins, 0.0), fq(kBins, 0.0), ph(kBins, 0.0), prevPhase(kBins, 0.0), prevA(kBins, 0.0), prevM(kBins, 0.0), carry(kBins, 0.0) {}
};

// The settings of one block, as the frames use them.
struct Settings {
  double motion, slide, hold, bloom;
  int every;      // frames between keyframes, 0: only the first
  double heal;    // the pull toward the live sound each frame
  int reach;      // how many bins either side the modulator's levels are blurred over
};

class Mosh final : public Node {
public:
  explicit Mosh(const NodeContext& context)
      : sampleRate_(context.sampleRate),
        re_(kStftMaxSize), im_(kStftMaxSize), reM_(kStftMaxSize), imM_(kStftMaxSize),
        logA_(kBins), logM_(kBins), phaseA_(kBins), sum_(kBins + 1), blur_(kBins),
        tmpCur_(kBins), tmpKey_(kBins), tmpFq_(kBins), tmpPh_(kBins), tmpCarry_(kBins),
        shiftAt_(kBins), found_(kBins), costSum_(2 * kReach + 1, std::vector<double>(kBins + 1)), liveSum_(kBins + 1) {}

  void process(const float* const* inputs, float* const* outputs, int count, const float* params, const int* slots) override {
    const int size = stftSizeOf(params[slots[kSize]]);
    if (size != n_) start(size);
    const double hop = n_ / 4.0, heal = params[slots[kHeal]];
    Settings p;
    p.motion = params[slots[kMotion]];
    p.slide = params[slots[kSlide]];
    p.hold = params[slots[kHold]];
    p.bloom = params[slots[kBloom]];
    p.every = jsRoundInt(params[slots[kKeyframe]] * sampleRate_ / 1000 / hop);
    // the pull toward the live sound each frame: a time constant of 2 s at a heal near 0, 0.1 s at 1 (nothing at 0)
    p.heal = heal > 0 ? 1 - js::exp(-hop / (sampleRate_ * 2 * js::pow(0.05, heal))) : 0;
    p.reach = jsRoundInt(params[slots[kSmooth]] * 6);
    const double mix = params[slots[kMix]];
    for (int c = 0; c < 2; c++) {
      Side& s = *sides_[c];
      const float* x = inputs[c];
      const float* m = inputs[2 + c];
      float* y = outputs[c];
      for (int i = 0; i < count; i++) {
        s.mod.push(m[i]);
        if (s.carrier.push(x[i])) { s.carrier.fill(re_.data(), im_.data()); frame(s, p); }
        const double dry = s.carrier.delayed(), wet = s.out.take(s.carrier.t());
        y[i] = static_cast<float>(dry + (wet - dry) * mix);
      }
    }
  }

private:
  void start(int size) {
    n_ = size;
    sides_[0] = std::make_unique<Side>(size);
    sides_[1] = std::make_unique<Side>(size);
  }

  // dB below full scale of each bin of a spectrum in r and i (and the phases, when asked for)
  static void levels(const std::vector<double>& r, const std::vector<double>& i, std::vector<double>& out, int half, double peak, std::vector<double>* phases) {
    for (int k = 0; k <= half; k++) {
      const std::size_t at = static_cast<std::size_t>(k);
      out[at] = std::max(kFloor, 20 * js::log10(jsHypot(r[at], i[at]) / peak + 1e-9));
      if (phases) (*phases)[at] = js::atan2(i[at], r[at]);
    }
  }

  // A keyframe: the picture becomes the live sound, with the frequency of each bin read from how its phase turned.
  void keyframe(Side& s, int half) {
    for (int k = 0; k <= half; k++) {
      const std::size_t at = static_cast<std::size_t>(k);
      const double turned = wrap(phaseA_[at] - s.prevPhase[at] - kPi / 2 * k);   // beyond what a tone exactly at the bin's centre would do
      s.cur[at] = logA_[at];
      s.key[at] = logA_[at];
      s.fq[at] = k + clamp(turned / (kPi / 2), -2, 2);
      s.ph[at] = phaseA_[at];
    }
    double peak = kFloor;
    for (int k = 0; k <= half; k++) if (logA_[static_cast<std::size_t>(k)] > peak) peak = logA_[static_cast<std::size_t>(k)];
    s.peak = peak;
    s.since = 0;
    s.started = true;
    std::fill(s.carry.begin(), s.carry.end(), 0.0);
  }

  // The sideways motion of the modulator since the last frame around every bin, in bins. Each bin looks at a window of
  // its neighbours (wider higher up, as pitch is) and finds the shift that best matches the last frame of the
  // modulator to this one: block matching, with the blocks overlapping so that a tone can cross from one to the next.
  // The motion is found to a fraction and handed on in whole bins as it adds up; shiftAt_ gets the whole bins.
  void findMotion(Side& s, int half, double slide) {
    for (int d = -kReach; d <= kReach; d++) {   // the cost of every window at every shift, from running sums
      std::vector<double>& sums = costSum_[static_cast<std::size_t>(d + kReach)];
      sums[0] = 0;
      for (int k = 0; k <= half; k++) {
        const int j = k - d;
        const double before = j < 0 || j > half ? kFloor : s.prevM[static_cast<std::size_t>(j)];
        sums[static_cast<std::size_t>(k + 1)] = sums[static_cast<std::size_t>(k)] + std::abs(logM_[static_cast<std::size_t>(k)] - before);
      }
    }
    liveSum_[0] = 0;
    for (int k = 0; k <= half; k++) liveSum_[static_cast<std::size_t>(k + 1)] = liveSum_[static_cast<std::size_t>(k)] + (logM_[static_cast<std::size_t>(k)] > kQuiet ? 1 : 0);
    double cost[2 * kReach + 1];
    for (int k = 0; k <= half; k++) {
      const int w = std::max(8, jsRoundInt(k * kWindow)), lo = std::max(0, k - w), hi = std::min(half, k + w);
      found_[static_cast<std::size_t>(k)] = 0;
      if (liveSum_[static_cast<std::size_t>(hi + 1)] - liveSum_[static_cast<std::size_t>(lo)] < 3) continue;
      int best = 0;
      double bestCost = std::numeric_limits<double>::infinity();
      for (int d = -kReach; d <= kReach; d++) {
        const std::vector<double>& sums = costSum_[static_cast<std::size_t>(d + kReach)];
        const double c = sums[static_cast<std::size_t>(hi + 1)] - sums[static_cast<std::size_t>(lo)];
        cost[d + kReach] = c;
        if (c < bestCost) { bestCost = c; best = d; }
      }
      if (best != 0 && bestCost >= 0.85 * cost[kReach]) continue;   // only a clear win moves anything (a codec prefers no motion)
      double part = 0;
      if (best > -kReach && best < kReach) {   // between the samples
        const double l = cost[best + kReach - 1], h = cost[best + kReach + 1], m = cost[best + kReach], rise = std::max(l, h) - m;
        if (rise > 1e-9) part = clamp(0.5 * (l - h) / rise, -0.5, 0.5);
      }
      found_[static_cast<std::size_t>(k)] = best + part;
    }
    // neighbours move together: the median of five takes out the odd one
    for (int k = 0; k <= half; k++) {
      const std::size_t at = static_cast<std::size_t>(k);
      double v = found_[at];
      if (k >= 2 && k <= half - 2) {
        double five[5] = {found_[at - 2], found_[at - 1], found_[at], found_[at + 1], found_[at + 2]};
        std::sort(five, five + 5);
        v = five[2];
      }
      s.carry[at] += v * slide;
      const double whole = std::trunc(s.carry[at]);
      s.carry[at] -= whole;
      shiftAt_[at] = static_cast<std::int16_t>(whole);
    }
  }

  // Slides the picture by shiftAt_: what was at j is now at j + shift, as the same tone moved up by that many bins.
  void slidePicture(Side& s, int half) {
    for (int k = 0; k <= half; k++) {
      const std::size_t at = static_cast<std::size_t>(k);
      const int j = std::min(half, std::max(0, k - shiftAt_[at]));
      const std::size_t from = static_cast<std::size_t>(j);
      tmpCur_[at] = s.cur[from];
      tmpKey_[at] = s.key[from];
      tmpFq_[at] = s.fq[from] + (k - j);
      tmpPh_[at] = s.ph[from];
      tmpCarry_[at] = s.carry[from];
    }
    const std::size_t count = static_cast<std::size_t>(half + 1);
    std::copy_n(tmpCur_.begin(), count, s.cur.begin());
    std::copy_n(tmpKey_.begin(), count, s.key.begin());
    std::copy_n(tmpFq_.begin(), count, s.fq.begin());
    std::copy_n(tmpPh_.begin(), count, s.ph.begin());
    std::copy_n(tmpCarry_.begin(), count, s.carry.begin());
  }

  // One frame of one side: the carrier's spectrum is in re_ and im_.
  void frame(Side& s, const Settings& p) {
    const int half = n_ >> 1;
    const double peak = stftPeakOf(n_);
    levels(re_, im_, logA_, half, peak, &phaseA_);
    s.mod.fill(reM_.data(), imM_.data());
    levels(reM_, imM_, logM_, half, peak, nullptr);
    // blur the modulator's levels a little, so that single noisy bins do not move things
    if (p.reach > 0) {
      sum_[0] = 0;
      for (int k = 0; k <= half; k++) sum_[static_cast<std::size_t>(k + 1)] = sum_[static_cast<std::size_t>(k)] + logM_[static_cast<std::size_t>(k)];
      for (int k = 0; k <= half; k++) {
        const int lo = std::max(0, k - p.reach), hi = std::min(half, k + p.reach);
        blur_[static_cast<std::size_t>(k)] = (sum_[static_cast<std::size_t>(hi + 1)] - sum_[static_cast<std::size_t>(lo)]) / (hi - lo + 1);
      }
      std::copy_n(blur_.begin(), static_cast<std::size_t>(half + 1), logM_.begin());
    }
    s.since++;
    if (!s.primed) {
      s.primed = true;   // the first frame only gives the phases to measure the next one against
    } else if (!s.started || (p.every > 0 && s.since >= p.every)) {
      keyframe(s, half);
    } else {
      // motion first (the picture moves), then brightness
      std::fill_n(shiftAt_.begin(), static_cast<std::size_t>(half + 1), std::int16_t{0});
      if (p.slide > 0) { findMotion(s, half, p.slide); slidePicture(s, half); }
      const double refresh = 1 - p.hold, ceilingEmpty = s.peak - 60 * (1 - p.bloom);
      for (int k = 0; k <= half; k++) {
        const std::size_t at = static_cast<std::size_t>(k);
        double d = 0;
        if (logM_[at] > kQuiet || s.prevM[at] > kQuiet) d += p.motion * clamp(logM_[at] - s.prevM[at], -kStep, kStep);
        d += refresh * (logA_[at] - s.prevA[at]);
        const double v = s.cur[at] + d + p.heal * (logA_[at] - s.cur[at]);
        const double ceiling = std::max(s.key[at] + 12, ceilingEmpty);
        s.cur[at] = std::min(ceiling, std::max(kFloor, v));
        s.ph[at] = wrap(s.ph[at] + kPi / 2 * s.fq[at]);   // the held tone goes on turning
      }
    }
    for (int k = 0; k <= half; k++) {
      const std::size_t at = static_cast<std::size_t>(k);
      s.prevM[at] = logM_[at];
      s.prevA[at] = logA_[at];
      s.prevPhase[at] = phaseA_[at];
    }
    // the picture back into a spectrum
    for (int k = 0; k <= half; k++) {
      const std::size_t at = static_cast<std::size_t>(k);
      const double a = toAmplitude(s.cur[at], peak);
      re_[at] = a * js::cos(s.ph[at]);
      im_[at] = a * js::sin(s.ph[at]);
    }
    mirrorSpectrum(re_.data(), im_.data(), n_);
    s.out.add(re_.data(), im_.data(), s.carrier.t());
  }

  double sampleRate_;
  int n_ = 0;   // the frame length in use (0: not started)
  std::unique_ptr<Side> sides_[2];
  // working space, shared by both sides (they take turns)
  std::vector<double> re_, im_, reM_, imM_;
  std::vector<double> logA_, logM_, phaseA_, sum_, blur_;
  std::vector<double> tmpCur_, tmpKey_, tmpFq_, tmpPh_, tmpCarry_;
  std::vector<std::int16_t> shiftAt_;
  std::vector<double> found_;
  std::vector<std::vector<double>> costSum_;
  std::vector<double> liveSum_;
};

}  // namespace

void registerMosh(NodeRegistry& registry) {
  NodeType type;
  type.type = "mosh";
  type.inputs = {"in", "mod"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"mod", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.latency = [](const std::map<std::string, double>& settings, double) { return static_cast<double>(stftSizeOf(settings.at("size")) - 1); };   // one frame
  type.params = {
    {"motion", 0, 2, 1},
    {"slide", 0, 1, 0.6},
    {"keyframe", 0, 8000, 2000},    // ms
    {"hold", 0, 1, 0.85},
    {"heal", 0, 1, 0.1},
    {"bloom", 0, 1, 0.4},
    {"smooth", 0, 1, 0.3},
    {"mix", 0, 1, 1},
    {"size", 0, 2, 1, true},        // frame length: 1024, 2048, 4096 samples
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Mosh>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Impulse-response reverb: partitioned FFT convolution in three stages (blocks of 128, 1024 and 4096 samples; the last is spread out
// over the blocks in between). The response is rebuilt a step per block in the background, as in webtracker
// src/audio/nodes/convolver.js (the web app's generator becomes a list of tasks run one per block, so the new response is
// installed at the same block).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/fft.h"
#include "nodes/rooms.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kSample = 0, kSize, kDecay, kPredelay, kLowcut, kHighcut, kWidth, kLevel, kMix, kRoom };
constexpr int kB1 = 128, kB2 = 1024, kB3 = 4096;
const int kMaxP[3] = {7, 7, std::numeric_limits<int>::max()};
const int kStart[3] = {0, 896, 8064};
constexpr int kMaxSeconds = 8;

using FloatVec = std::vector<float>;
using FloatPtr = std::shared_ptr<FloatVec>;

struct Spectra {
  int count = 0;
  FloatPtr hRe[2], hIm[2], fRe[2], fIm[2];
};

// One stage: overlap-save partitioned convolution with block B; slow stages spread their output over the next block.
struct Stage {
  Stage(int b, bool s) : B(b), N(2 * b), bins(b + 1), slow(s), plan(&fftPlan(2 * b)), re(2 * b), im(2 * b) {
    for (int c = 0; c < 2; c++) {
      win[c].assign(static_cast<std::size_t>(N), 0.0);
      playing[c].assign(static_cast<std::size_t>(B), 0.0f);
      done[c].assign(static_cast<std::size_t>(B), 0.0f);
      yRe[c].assign(static_cast<std::size_t>(B) + 1, 0.0);
      yIm[c].assign(static_cast<std::size_t>(B) + 1, 0.0);
    }
  }

  void install(const Spectra& s) {
    P = s.count;
    for (int c = 0; c < 2; c++) { hRe[c] = s.hRe[c]; hIm[c] = s.hIm[c]; fRe[c] = s.fRe[c]; fIm[c] = s.fIm[c]; }
    head = 0; jobActive = false;
    for (int c = 0; c < 2; c++) {
      std::fill(win[c].begin(), win[c].end(), 0.0);
      std::fill(playing[c].begin(), playing[c].end(), 0.0f);
      std::fill(done[c].begin(), done[c].end(), 0.0f);
    }
    fill = 0;
  }

  void mac(int c, int k0, int k1) {
    double* yr = yRe[c].data();
    double* yi = yIm[c].data();
    const float* fr = fRe[c]->data();
    const float* fi = fIm[c]->data();
    const float* hr = hRe[c]->data();
    const float* hi = hIm[c]->data();
    for (int k = k0; k < k1; k++) {
      const std::size_t f = static_cast<std::size_t>(((head - k + P) % P) * bins), h = static_cast<std::size_t>(k * bins);
      for (int b = 0; b < bins; b++) {
        const double xr = fr[f + static_cast<std::size_t>(b)], xi = fi[f + static_cast<std::size_t>(b)];
        const double hre = hr[h + static_cast<std::size_t>(b)], him = hi[h + static_cast<std::size_t>(b)];
        yr[b] += xr * hre - xi * him;
        yi[b] += xr * him + xi * hre;
      }
    }
  }

  void finish(int c, float* into) {
    for (int b = 0; b <= B; b++) { re[static_cast<std::size_t>(b)] = yRe[c][static_cast<std::size_t>(b)]; im[static_cast<std::size_t>(b)] = yIm[c][static_cast<std::size_t>(b)]; }
    for (int b = B + 1; b < N; b++) { re[static_cast<std::size_t>(b)] = yRe[c][static_cast<std::size_t>(N - b)]; im[static_cast<std::size_t>(b)] = -yIm[c][static_cast<std::size_t>(N - b)]; }
    plan->run(re.data(), im.data(), true);
    for (int i = 0; i < B; i++) into[i] = static_cast<float>(re[static_cast<std::size_t>(B + i)]);
  }

  void boundary() {
    if (P == 0) {
      fill = 0;
      for (int c = 0; c < 2; c++) std::copy(win[c].begin() + B, win[c].begin() + 2 * B, win[c].begin());
      return;
    }
    head = (head + 1) % P;
    for (int c = 0; c < 2; c++) {
      std::copy(win[c].begin(), win[c].end(), re.begin());
      std::fill(im.begin(), im.end(), 0.0);
      plan->run(re.data(), im.data());
      const std::size_t at = static_cast<std::size_t>(head * bins);
      float* fr = fRe[c]->data();
      float* fi = fIm[c]->data();
      for (int b = 0; b < bins; b++) { fr[at + static_cast<std::size_t>(b)] = static_cast<float>(re[static_cast<std::size_t>(b)]); fi[at + static_cast<std::size_t>(b)] = static_cast<float>(im[static_cast<std::size_t>(b)]); }
      std::copy(win[c].begin() + B, win[c].begin() + 2 * B, win[c].begin());
    }
    if (slow) {
      for (int c = 0; c < 2; c++) playing[c] = done[c];
      for (int c = 0; c < 2; c++) { std::fill(yRe[c].begin(), yRe[c].end(), 0.0); std::fill(yIm[c].begin(), yIm[c].end(), 0.0); }
      jobActive = true; jobK = 0; jobFinished = false;
    } else {
      for (int c = 0; c < 2; c++) {
        std::fill(yRe[c].begin(), yRe[c].end(), 0.0); std::fill(yIm[c].begin(), yIm[c].end(), 0.0);
        mac(c, 0, P);
        finish(c, playing[c].data());
      }
    }
    fill = 0;
  }

  void advance() {
    if (!jobActive || jobFinished) return;
    const int target = fill >= B * 0.7 ? P : static_cast<int>(std::floor(static_cast<double>(P) * fill / (B * 0.7)));
    if (target > jobK) { for (int c = 0; c < 2; c++) mac(c, jobK, target); jobK = target; }
    if (jobK >= P) { for (int c = 0; c < 2; c++) finish(c, done[c].data()); jobFinished = true; }
  }

  void run(const float* inL, const float* inR, float* accL, float* accR, int n) {
    int pos = 0;
    while (pos < n) {
      const int take = std::min(n - pos, B - fill), f = fill;
      std::vector<double>& wL = win[0];
      std::vector<double>& wR = win[1];
      for (int i = 0; i < take; i++) {
        wL[static_cast<std::size_t>(B + f + i)] = inL[pos + i];
        wR[static_cast<std::size_t>(B + f + i)] = inR[pos + i];
        accL[pos + i] = static_cast<float>(static_cast<double>(accL[pos + i]) + playing[0][static_cast<std::size_t>(f + i)]);
        accR[pos + i] = static_cast<float>(static_cast<double>(accR[pos + i]) + playing[1][static_cast<std::size_t>(f + i)]);
      }
      fill += take; pos += take;
      if (fill == B) boundary();
    }
    if (slow) advance();
  }

  int B, N, bins;
  bool slow;
  const FftPlan* plan;
  std::vector<double> re, im;
  std::vector<double> win[2];
  FloatVec playing[2], done[2];
  std::vector<double> yRe[2], yIm[2];
  int fill = 0, head = 0, P = 0;
  bool jobActive = false, jobFinished = false;
  int jobK = 0;
  FloatPtr hRe[2], hIm[2], fRe[2], fIm[2];
};

struct Bank {
  std::shared_ptr<const SampleMessage> msg;
  double ratio = 1, gain = 1;
};

// The state of one rebuild of the response, worked through a task at a time.
struct BuildState {
  std::shared_ptr<const SampleMessage> ir;
  double ratio = 1, irGain = 1, size = 1, decay = 10, sampleRate = 44100;
  bool stereo = false;
  int len = 1;
  double step = 1, fade = 0, energy = 0, gain = 0;
  FloatVec hs[2];
  Spectra out[3];
  bool allocated[3] = {false, false, false};
  std::vector<std::function<void()>> tasks;
  std::size_t next = 0;
};

class Convolver final : public Node {
public:
  explicit Convolver(const NodeContext& context)
      : sampleRate_(context.sampleRate), wl_(context.maxBlock, 0.0f), wr_(context.maxBlock, 0.0f),
        maxPre_(static_cast<int>(std::ceil(context.sampleRate * 0.5)) + 1), maxLen_(static_cast<int>(std::floor(kMaxSeconds * context.sampleRate))) {
    stages_.reserve(3);
    stages_.emplace_back(kB1, false);
    stages_.emplace_back(kB2, false);
    stages_.emplace_back(kB3, true);
    pre_[0].assign(static_cast<std::size_t>(maxPre_), 0.0f);
    pre_[1].assign(static_cast<std::size_t>(maxPre_), 0.0f);
  }

  void sample(const std::shared_ptr<const SampleMessage>& message) override {
    const std::size_t slot = static_cast<std::size_t>(std::max(0, message->slot));
    if (bank_.size() <= slot) bank_.resize(slot + 1);
    auto S = std::make_unique<Bank>();
    S->msg = message;
    S->ratio = message->rate / sampleRate_;
    S->gain = message->volume * message->globalVolume;
    bank_[slot] = std::move(S);
    irVersion_++;
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inL = inputs[0];
    const float* inR = inputs[1];
    float* outL = outputs[0];
    float* outR = outputs[1];
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    // a built-in room stands in for the sample: it takes a slot number after the 256 the project can have
    const int room = std::max(0, std::min(kRoomCount, static_cast<int>(std::floor(g(kRoom)))));
    const int slot = room > 0 ? 255 + room : std::max(0, std::min(255, static_cast<int>(std::floor(g(kSample)))));
    const std::int64_t sizeKey = std::llround(g(kSize) * 1000), decayKey = std::llround(g(kDecay) * 100);
    const Sig sig{slot, sizeKey, decayKey, irVersion_};
    if (!haveWant_ || !(sig == want_)) {
      want_ = sig; haveWant_ = true; stable_ = 0;
      if (job_ && !(jobSig_ == sig)) { job_.reset(); haveBuilt_ = false; }
    } else if (stable_ < 1000) stable_++;
    const Bank* ir = room > 0 ? roomBank(room) : slot < static_cast<int>(bank_.size()) ? bank_[static_cast<std::size_t>(slot)].get() : nullptr;
    if (!job_ && stable_ >= 12 && !(haveBuilt_ && built_ == want_) && ir) {
      job_ = startBuild(*ir, g(kSize), g(kDecay));
      jobSig_ = want_;
      built_ = want_; haveBuilt_ = true;
    }
    if (job_) {
      if (job_->next < job_->tasks.size()) job_->tasks[job_->next++]();
      else { for (int s = 0; s < 3; s++) stages_[static_cast<std::size_t>(s)].install(job_->out[s]); job_.reset(); }
    }
    if (static_cast<int>(wl_.size()) < n) { wl_.assign(static_cast<std::size_t>(n), 0.0f); wr_.assign(static_cast<std::size_t>(n), 0.0f); }
    std::fill(wl_.begin(), wl_.begin() + n, 0.0f);
    std::fill(wr_.begin(), wr_.begin() + n, 0.0f);
    for (Stage& st : stages_) st.run(inL, inR, wl_.data(), wr_.data(), n);
    const double mix = g(kMix), level = js::pow(10, g(kLevel) / 20), width = g(kWidth);
    const int pd = std::min(maxPre_ - 1, jsRoundInt(g(kPredelay) * 0.001 * sampleRate_));
    const double sr = sampleRate_;
    const double aLow = 1 - js::exp(-2 * kPi * g(kLowcut) / sr), aHigh = 1 - js::exp(-2 * kPi * std::min(g(kHighcut), sr * 0.45) / sr);
    const bool useLow = g(kLowcut) > 25, useHigh = g(kHighcut) < 19000;
    for (int i = 0; i < n; i++) {
      pre_[0][static_cast<std::size_t>(pw_)] = wl_[static_cast<std::size_t>(i)];
      pre_[1][static_cast<std::size_t>(pw_)] = wr_[static_cast<std::size_t>(i)];
      int r = pw_ - pd;
      if (r < 0) r += maxPre_;
      double l = pre_[0][static_cast<std::size_t>(r)], rr = pre_[1][static_cast<std::size_t>(r)];
      if (++pw_ == maxPre_) pw_ = 0;
      if (width != 1) { const double m = (l + rr) * 0.5, s = (l - rr) * 0.5 * width; l = m + s; rr = m - s; }
      for (int c = 0; c < 2; c++) {
        double v = c ? rr : l;
        if (useLow) {
          lp_[c * 2] += aLow * (v - lp_[c * 2]); v -= lp_[c * 2];
          lp_[c * 2 + 1] += aLow * (v - lp_[c * 2 + 1]); v -= lp_[c * 2 + 1];
        }
        if (useHigh) {
          hc_[c * 2] += aHigh * (v - hc_[c * 2]); v = hc_[c * 2];
          hc_[c * 2 + 1] += aHigh * (v - hc_[c * 2 + 1]); v = hc_[c * 2 + 1];
        }
        const double dry = c ? inR[i] : inL[i];
        (c ? outR : outL)[i] = static_cast<float>(dry * (1 - mix) + v * level * mix);
      }
    }
    if (!std::isfinite(lp_[0] + lp_[1] + hc_[0] + hc_[1] + static_cast<double>(outL[0]) + static_cast<double>(outR[0]))) {
      for (int k = 0; k < 4; k++) { lp_[k] = 0; hc_[k] = 0; }
    }
  }

private:
  // The response of a built-in room, made the first time it is asked for.
  const Bank* roomBank(int room) {
    std::unique_ptr<Bank>& kept = rooms_[static_cast<std::size_t>(room - 1)];
    if (!kept) {
      kept = std::make_unique<Bank>();
      kept->msg = makeRoom(room, sampleRate_);
      kept->ratio = 1;   // (made at the engine's rate)
      kept->gain = 1;
    }
    return kept.get();
  }

  struct Sig {
    int slot = 0;
    std::int64_t size = 0, decay = 0;
    int version = 0;
    bool operator==(const Sig& o) const { return slot == o.slot && size == o.size && decay == o.decay && version == o.version; }
  };

  // Plans the rebuild: one task for each place the web app's generator yields.
  std::unique_ptr<BuildState> startBuild(const Bank& bank, double size, double decay) {
    auto st = std::make_unique<BuildState>();
    BuildState* s = st.get();
    s->ir = bank.msg;
    s->ratio = bank.ratio; s->irGain = bank.gain; s->size = size; s->decay = decay; s->sampleRate = sampleRate_;
    s->stereo = !bank.msg->right.empty();
    const double srcLen = static_cast<double>(bank.msg->data.size());
    s->step = s->ratio / size;
    s->len = std::max(1, std::min(maxLen_, static_cast<int>(std::floor((srcLen - 1) / s->step))));
    const int len = s->len;
    s->hs[0].assign(static_cast<std::size_t>(len), 0.0f);
    if (s->stereo) s->hs[1].assign(static_cast<std::size_t>(len), 0.0f);
    s->fade = decay < 10 ? 6.9078 / (decay * sampleRate_) : 0;
    const int channels = s->stereo ? 2 : 1;
    // shaping the response: stretched, faded
    for (int c = 0; c < channels; c++) {
      for (int i0 = 0; i0 < len; i0 += 8192) {
        s->tasks.push_back([s, c, i0, len] {
          const std::vector<float>& src = c ? s->ir->right : s->ir->data;
          FloatVec& dst = s->hs[c];
          const int end = std::min(len, i0 + 8192);
          for (int i = i0; i < end; i++) {
            const double p = i * s->step;
            const int j = static_cast<int>(p);
            const double f = p - j;
            const double next = static_cast<std::size_t>(j) + 1 < src.size() ? static_cast<double>(src[static_cast<std::size_t>(j) + 1]) : 0.0;
            const double v = (src[static_cast<std::size_t>(j)] * (1 - f) + next * f) * (s->fade != 0 ? js::exp(-s->fade * i) : 1);
            dst[static_cast<std::size_t>(i)] = static_cast<float>(v);
            s->energy += v * v;
          }
        });
      }
    }
    // normalising
    bool first = true;
    for (int c = 0; c < channels; c++) {
      for (int i0 = 0; i0 < len; i0 += 16384) {
        const bool prelude = first;
        first = false;
        s->tasks.push_back([s, c, i0, len, prelude] {
          if (prelude) s->gain = s->energy > 1e-12 ? s->irGain / std::sqrt(s->energy / (s->stereo ? 2 : 1)) : 0;
          FloatVec& h = s->hs[c];
          const int end = std::min(len, i0 + 16384);
          for (int i = i0; i < end; i++) h[static_cast<std::size_t>(i)] = static_cast<float>(h[static_cast<std::size_t>(i)] * s->gain);
        });
      }
    }
    // each stage's partitions
    for (int stage = 0; stage < 3; stage++) {
      const int B = stage == 0 ? kB1 : stage == 1 ? kB2 : kB3, bins = B + 1;
      const int count = len > kStart[stage] ? std::min(kMaxP[stage], static_cast<int>(std::ceil(static_cast<double>(len - kStart[stage]) / B))) : 0;
      auto allocate = [s, stage, count, bins] {
        if (s->allocated[stage]) return;
        s->allocated[stage] = true;
        Spectra& o = s->out[stage];
        o.count = count;
        const std::size_t spectrumLength = static_cast<std::size_t>(count) * static_cast<std::size_t>(bins);
        o.hRe[0] = std::make_shared<FloatVec>(spectrumLength, 0.0f);
        o.hIm[0] = std::make_shared<FloatVec>(spectrumLength, 0.0f);
        if (s->stereo) { o.hRe[1] = std::make_shared<FloatVec>(spectrumLength, 0.0f); o.hIm[1] = std::make_shared<FloatVec>(spectrumLength, 0.0f); }
      };
      for (int c = 0; c < channels; c++) {
        for (int k = 0; k < count; k++) {
          s->tasks.push_back([s, stage, c, k, B, bins, len, allocate] {
            allocate();
            const FloatVec& seg = s->hs[s->stereo ? c : 0];
            std::vector<double> re(static_cast<std::size_t>(2 * B), 0.0), im(static_cast<std::size_t>(2 * B), 0.0);
            const int a = kStart[stage] + k * B;
            for (int i = 0; i < B && a + i < len; i++) re[static_cast<std::size_t>(i)] = seg[static_cast<std::size_t>(a + i)];
            fftPlan(2 * B).run(re.data(), im.data());
            Spectra& o = s->out[stage];
            for (int b = 0; b < bins; b++) {
              (*o.hRe[c])[static_cast<std::size_t>(k * bins + b)] = static_cast<float>(re[static_cast<std::size_t>(b)]);
              (*o.hIm[c])[static_cast<std::size_t>(k * bins + b)] = static_cast<float>(im[static_cast<std::size_t>(b)]);
            }
          });
        }
      }
      s->tasks.push_back([s, stage, count, bins, allocate] {
        allocate();
        Spectra& o = s->out[stage];
        if (!s->stereo) { o.hRe[1] = o.hRe[0]; o.hIm[1] = o.hIm[0]; }
        const std::size_t spectrumLength = static_cast<std::size_t>(std::max(1, count)) * static_cast<std::size_t>(bins);
        for (int c = 0; c < 2; c++) { o.fRe[c] = std::make_shared<FloatVec>(spectrumLength, 0.0f); o.fIm[c] = std::make_shared<FloatVec>(spectrumLength, 0.0f); }
      });
    }
    return st;
  }

  double sampleRate_;
  std::vector<std::unique_ptr<Bank>> bank_;
  std::unique_ptr<Bank> rooms_[kRoomCount];
  std::vector<Stage> stages_;
  int irVersion_ = 0;
  Sig want_, built_, jobSig_;
  bool haveWant_ = false, haveBuilt_ = false;
  int stable_ = 0;
  std::unique_ptr<BuildState> job_;
  FloatVec wl_, wr_;
  int maxPre_, maxLen_;
  FloatVec pre_[2];
  int pw_ = 0;
  double lp_[4] = {0, 0, 0, 0}, hc_[4] = {0, 0, 0, 0};
};

}  // namespace

void registerConvolver(NodeRegistry& registry) {
  NodeType type;
  type.type = "convolver";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {
    {"sample", 0, 255, 0, true}, {"size", 0.25, 2, 1}, {"decay", 0.05, 10, 10}, {"predelay", 0, 500, 0}, {"lowcut", 20, 2000, 20}, {"highcut", 1000, 20000, 20000},
    {"width", 0, 1.5, 1}, {"level", -24, 12, 0}, {"mix", 0, 1, 0.3},
    {"room", 0, kRoomCount, 0, true},   // 0: the sample is the response; 1 studio, 2 chamber, 3 hall, 4 church
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Convolver>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

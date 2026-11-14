// Granular synth: a cloud of short grains read from one of the instrument's samples. Every grain starts at `position` plus a random
// `spray`, lasts `size` ms, is windowed and placed in the stereo field; `density` grains start every second; `scan` moves the
// position along the sample. Monophonic per node, last note wins. Port of webtracker src/audio/nodes/granular.js (without the
// scope's 'cloud' reports).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/js_math.h"
#include "nodes/adsr.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kSample = 0, kPosition, kSpray, kScan, kSize, kDensity, kShape, kTune, kPitchRand, kKeytrack, kRoot, kReverse, kSpread, kVel, kLevel, kAttack, kDecay, kSustain, kRelease };
constexpr int kMaxGrains = 96;
constexpr int kTs = 512;   // the fade in and out come from a table: a quarter-cycle cosine

struct Bank {
  std::shared_ptr<const SampleMessage> msg;
  double ratio = 1, gain = 1;
};

class Granular final : public Node {
public:
  explicit Granular(const NodeContext& context) : sampleRate_(context.sampleRate), env_(context.sampleRate), mixL_(context.maxBlock, 0.0), mixR_(context.maxBlock, 0.0) {
    fade_.resize(kTs + 2);
    for (int j = 0; j < kTs + 2; j++) fade_[j] = 0.5 - 0.5 * js::cos(kPi * std::min(j, kTs) / kTs);
  }

  void sample(const std::shared_ptr<const SampleMessage>& message) override {
    const std::size_t slot = static_cast<std::size_t>(std::max(0, message->slot));
    if (bank_.size() <= slot) bank_.resize(slot + 1);
    auto S = std::make_unique<Bank>();
    S->msg = message;
    S->ratio = message->rate / sampleRate_;
    S->gain = message->volume * message->globalVolume;
    bank_[slot] = std::move(S);
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      if (env_.idle()) clock_ = 1;
      scanned_ = 0;
      note_ = event.note; velocity_ = event.vel;
      env_.on();
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) env_.off();
    else { env_.cut(); std::fill(live_, live_ + kMaxGrains, 0); nAct_ = 0; }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (env_.idle() || bank_.empty()) {
      std::fill(outL, outL + n, 0.0f);
      std::fill(outR, outR + n, 0.0f);
      std::fill(live_, live_ + kMaxGrains, 0);
      nAct_ = 0;
      return;
    }
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double sr = sampleRate_;
    env_.set(g(kAttack), g(kDecay), g(kSustain), g(kRelease));
    const double every = sr / std::max(1.0, g(kDensity));
    const double overlap = std::max(1.0, g(kDensity) * g(kSize) * 0.001);
    const double level = g(kLevel) * (1 - g(kVel) + g(kVel) * velocity_) / std::sqrt(overlap);
    const double scanStep = g(kScan) / sr;
    if (static_cast<int>(mixL_.size()) < n) { mixL_.assign(n, 0.0); mixR_.assign(n, 0.0); }
    const double scanned0 = scanned_;
    for (int i = 0; i < n; i++) {
      clock_ += 1;
      if (clock_ >= every) {
        clock_ -= every;
        scanned_ = scanned0 + scanStep * (i + 1);
        const int k = spawn(g);
        if (k >= 0) off_[k] = i;
      }
    }
    scanned_ = scanned0 + scanStep * n;
    std::fill(mixL_.begin(), mixL_.begin() + n, 0.0);
    std::fill(mixR_.begin(), mixR_.begin() + n, 0.0);
    for (int q = 0; q < nAct_; q++) {
      const int k = act_[q];
      int i = off_[k];
      double p = pos_[k], age = age_[k];
      bool dead = false;
      off_[k] = 0;
      const double step = step_[k], inv = inv_[k], fade = fade_k_[k], last = last_[k], gain0 = gain_[k];
      const SampleMessage& m = *grainMsg_[k];
      const float* a = m.data.data();
      const float* right = m.right.empty() ? nullptr : m.right.data();
      const double pl = static_cast<double>(gl_[k]) * (right ? 1.4142 : 1), pr = static_cast<double>(gr_[k]) * (right ? 1.4142 : 1);
      for (; i < n; i++) {
        const double u = age * inv;
        if (u >= 1 || p < 0 || p >= last) { dead = true; break; }
        const int j = static_cast<int>(p);
        const double f = p - j;
        const double x = (u < 0.5 ? u : 1 - u) * fade;
        double w = gain0;
        if (x < kTs) { const int mi = static_cast<int>(x); w *= fade_[mi] + (fade_[mi + 1] - fade_[mi]) * (x - mi); }
        const double aj = a[j], aj1 = a[j + 1];
        mixL_[i] += (aj + (aj1 - aj) * f) * w * pl;
        const double rj = right ? right[j] : 0, rj1 = right ? right[j + 1] : 0;
        mixR_[i] += (right ? rj + (rj1 - rj) * f : aj + (aj1 - aj) * f) * w * pr;
        p += step;
        age += 1;
      }
      pos_[k] = p;
      age_[k] = age;
      if (dead) {
        live_[k] = 0;
        const int moved = --nAct_;
        act_[q] = act_[moved];
        q--;
      }
    }
    for (int i = 0; i < n; i++) {
      const double e = env_.next() * level;
      outL[i] = static_cast<float>(mixL_[i] * e);
      outR[i] = static_cast<float>(mixR_[i] * e);
    }
    scanned_ -= std::floor(scanned_);
  }

private:
  double rand() {
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    return seed_ / 4294967296.0;
  }

  template <typename G>
  int spawn(G& g) {
    const int pick = std::max(0, std::min(255, static_cast<int>(std::floor(g(kSample)))));
    if (pick >= static_cast<int>(bank_.size()) || !bank_[pick]) return -1;
    const Bank& S = *bank_[pick];
    int k = -1;
    for (int j = 0; j < kMaxGrains; j++) if (live_[j] == 0) { k = j; break; }
    if (k < 0) { k = 0; for (int j = 1; j < kMaxGrains; j++) if (age_[j] > age_[k]) k = j; }
    const double last = static_cast<double>(S.msg->data.size()) - 1;
    const double r1 = rand();
    double at = g(kPosition) + scanned_ + (r1 * 2 - 1) * g(kSpray);
    at -= std::floor(at);
    const double r2 = rand();
    const double semis = (g(kKeytrack) >= 1 ? note_ - g(kRoot) : 0) + g(kTune) + (r2 * 2 - 1) * g(kPitchRand) / 100;
    const double r3 = rand();
    const bool back = r3 < g(kReverse);
    const double len = std::max(8.0, g(kSize) * 0.001 * sampleRate_);
    const double step = js::pow(2, semis / 12) * S.ratio * (back ? -1 : 1);
    pos_[k] = std::max(0.0, std::min(last, at * last));
    const double taper = std::max(0.02, g(kShape));
    step_[k] = step; age_[k] = 0; len_[k] = len; inv_[k] = 1 / len; fade_k_[k] = 2 * kTs / taper; last_[k] = last;
    grainMsg_[k] = S.msg; gain_[k] = S.gain;
    const double r4 = rand();
    const double pan = (r4 * 2 - 1) * g(kSpread);
    const double a = (pan + 1) * kPi / 4;
    gl_[k] = static_cast<float>(js::cos(a));
    gr_[k] = static_cast<float>(js::sin(a));
    if (!live_[k]) act_[nAct_++] = k;
    live_[k] = 1;
    return k;
  }

  double sampleRate_;
  Adsr env_;
  std::vector<std::unique_ptr<Bank>> bank_;
  std::vector<double> fade_;
  std::uint8_t live_[kMaxGrains] = {};
  double pos_[kMaxGrains] = {}, step_[kMaxGrains] = {}, age_[kMaxGrains] = {}, len_[kMaxGrains] = {}, inv_[kMaxGrains] = {}, fade_k_[kMaxGrains] = {}, last_[kMaxGrains] = {}, gain_[kMaxGrains] = {};
  float gl_[kMaxGrains] = {}, gr_[kMaxGrains] = {};
  std::shared_ptr<const SampleMessage> grainMsg_[kMaxGrains];
  int act_[kMaxGrains] = {};
  int nAct_ = 0;
  int off_[kMaxGrains] = {};
  std::vector<double> mixL_, mixR_;
  std::uint32_t seed_ = 0x2545f491u;
  double clock_ = 1, scanned_ = 0, velocity_ = 1;
  int note_ = 60;
};

}  // namespace

void registerGranular(NodeRegistry& registry) {
  NodeType type;
  type.type = "granular";
  type.inputs = {};
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.params = {
    {"sample", 0, 255, 0, true}, {"position", 0, 1, 0.2}, {"spray", 0, 1, 0.02}, {"scan", -1, 1, 0}, {"size", 5, 1000, 80}, {"density", 1, 300, 30},
    {"shape", 0.02, 1, 1}, {"tune", -48, 48, 0}, {"pitchRand", 0, 1200, 0}, {"keytrack", 0, 1, 1, true}, {"root", 0, 127, 60}, {"reverse", 0, 1, 0},
    {"spread", 0, 1, 0.5}, {"vel", 0, 1, 0.5}, {"level", 0, 1, 0.7}, {"attack", 0.1, 5000, 20}, {"decay", 1, 5000, 300}, {"sustain", 0, 1, 1}, {"release", 1, 5000, 300},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Granular>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

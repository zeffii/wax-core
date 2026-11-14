// Drum synth: one drum hit built from three parts, each with its own ADSR, plus a transient. Body: a sine whose pitch falls from
// `startPitch` to `endPitch` along the pitch envelope. Noise: five colours, a low-pass, its own envelope. Click: a short burst at
// the start, synthesized (a damped sine) or the first `clickLen` ms of a sample from the node's list. Monophonic; a hit restarts the
// sound and the old output carries on into the new hit and dies away in about a millisecond. Port of webtracker
// src/audio/nodes/drumsynth.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "core/js_math.h"
#include "nodes/adsr.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param {
  kStartPitch = 0, kEndPitch, kPitchMul, kKeytrack, kRoot, kPatk, kPdec, kPsus, kPrel, kBody, kAttack, kDecay, kSustain, kRelease,
  kDrive, kNoise, kNoiseType, kNoiseTone, kNatk, kNdec, kNsus, kNrel, kClick, kClickMode, kClickLen, kClickTone, kClickSample,
  kClickTail, kVel, kLevel
};

struct ClickSample { std::vector<float> data; double ratio = 1; };

class DrumSynth final : public Node {
public:
  explicit DrumSynth(const NodeContext& context) : sampleRate_(context.sampleRate), amp_(context.sampleRate), pit_(context.sampleRate), noi_(context.sampleRate) {}

  void sample(const std::shared_ptr<const SampleMessage>& message) override {
    ClickSample s;
    s.data.resize(message->data.size());
    for (std::size_t i = 0; i < s.data.size(); i++) {
      const double d = message->data[i];
      s.data[i] = static_cast<float>(message->right.empty() ? d : (d + message->right[i]) * 0.5);
    }
    s.ratio = message->rate / sampleRate_;
    bank_[message->slot] = std::move(s);
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { note_ = event.note; velocity_ = event.vel; hit_ = true; amp_.on(); pit_.on(); noi_.on(); }
    else if (event.type == NoteType::Off || event.type == NoteType::AllOff) { amp_.off(); pit_.off(); noi_.off(); }
    else { amp_.cut(); pit_.cut(); noi_.cut(); kOn_ = false; hit_ = false; last_ = carry_ = 0; }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (amp_.idle() && noi_.idle() && !kOn_ && !hit_) {
      std::fill(outL, outL + n, 0.0f);
      std::fill(outR, outR + n, 0.0f);
      last_ = carry_ = 0;
      return;
    }
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double sr = sampleRate_;
    amp_.set(g(kAttack), g(kDecay), g(kSustain), g(kRelease));
    pit_.set(g(kPatk), g(kPdec), g(kPsus), g(kPrel));
    noi_.set(g(kNatk), g(kNdec), g(kNsus), g(kNrel));
    const double ms = sr * 0.001, keyed = g(kKeytrack) >= 1 ? js::pow(2, (note_ - g(kRoot)) / 12) : 1, mul = g(kPitchMul) * keyed;
    const double f0 = g(kEndPitch), f1 = g(kStartPitch);
    const double bodyLevel = g(kBody), noiseAmt = g(kNoise);
    const int type = jsRoundInt(g(kNoiseType));
    const double clickAmt = g(kClick);
    const double nf = 1 - js::exp(-2 * kPi * std::min(g(kNoiseTone), sr * 0.45) / sr);
    const double v = 1 - g(kVel) + g(kVel) * velocity_, level = g(kLevel) * v, d = 1 + g(kDrive) * 4, dn = 1 / js::tanh(d);
    if (hit_) {
      hit_ = false; phase_ = 0; k_ = 0; carry_ = last_;
      kLen_ = std::max(1.0, jsRound(g(kClickLen) * ms));
      kSample_ = nullptr; kNorm_ = 1; kOn_ = clickAmt > 0;
      if (g(kClickMode) >= 1) {
        const auto found = bank_.find(jsRoundInt(g(kClickSample)));
        kSample_ = found == bank_.end() ? nullptr : &found->second;
        if (!kSample_) kOn_ = false;
        else {
          const std::vector<float>& s = kSample_->data;
          kLen_ = std::min(kLen_, std::floor(static_cast<double>(s.size()) / kSample_->ratio));
          double pk = 0;
          for (double i = 0; i < kLen_; i++) {
            const double at = std::min(static_cast<double>(s.size()) - 1, std::floor(i * kSample_->ratio));
            pk = std::max(pk, std::fabs(static_cast<double>(s[static_cast<std::size_t>(at)])));
          }
          kNorm_ = pk > 1e-6 ? 1 / pk : 1;
          if (kLen_ < 1) kOn_ = false;
        }
      }
      kTail_ = std::min(kLen_, g(kClickTail) * ms);
    }
    const double kRamp = std::max(2.0, jsRound(0.00015 * sr)), cd = js::exp(-1 / (0.0015 * sr));
    const double tone = 2 * kPi * g(kClickTone) / sr, kEnd = kSample_ ? kLen_ : kLen_ * 1.5;
    for (int i = 0; i < n; i++) {
      const double pe = pit_.next(), f = (f0 + (f1 - f0) * pe) * mul;
      phase_ += f / sr;
      if (phase_ >= 1) phase_ -= std::floor(phase_);
      const double body = js::sin(6.283185307179586 * phase_) * bodyLevel * amp_.next();
      const double w = white();
      double nz;
      if (type == 1) { b0_ = 0.99765 * b0_ + w * 0.099046; b1_ = 0.963 * b1_ + w * 0.2965164; b2_ = 0.57 * b2_ + w * 1.0526913; nz = (b0_ + b1_ + b2_ + w * 0.1848) * 0.3; }
      else if (type == 2) { brown_ = (brown_ + 0.02 * w) / 1.02; nz = brown_ * 3.5; }
      else if (type == 3) nz = (w - w1_) * 0.7;
      else if (type == 4) nz = (w - 2 * w1_ + w2_) * 0.4;
      else nz = w;
      w2_ = w1_; w1_ = w;
      lp_ += nf * (nz - lp_);
      const double noise = lp_ * noiseAmt * noi_.next();
      double click = 0;
      if (kOn_) {
        if (kSample_) {
          const std::vector<float>& s = kSample_->data;
          const double pos = k_ * kSample_->ratio, a = std::floor(pos), fr = pos - a;
          const std::size_t ia = static_cast<std::size_t>(a);
          const double sa = s[ia], sb = ia + 1 < s.size() ? s[ia + 1] : sa;
          double c = (sa + (sb - sa) * fr) * kNorm_;
          if (kTail_ > 0 && k_ > kLen_ - kTail_) c *= 0.5 + 0.5 * js::cos(kPi * (k_ - (kLen_ - kTail_)) / kTail_);
          click = c;
        } else click = (js::sin(tone * k_) * 0.8 + w * 0.3) * js::exp(-6.9078 * k_ / kLen_);
        click *= clickAmt * 0.9 * std::min(1.0, (k_ + 1) / kRamp);
        if (++k_ >= kEnd) kOn_ = false;
      }
      const double fresh = js::tanh((body + noise * 0.7 + click) * d) * dn * level;
      const double y = fresh + carry_;
      carry_ *= cd; last_ = fresh;
      outL[i] = static_cast<float>(y);
      outR[i] = static_cast<float>(y);
    }
    if (std::fabs(carry_) < 1e-6) carry_ = 0;
    if (!std::isfinite(lp_ + brown_ + b0_ + b1_ + b2_)) lp_ = brown_ = b0_ = b1_ = b2_ = 0;
  }

private:
  double white() {
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    return static_cast<double>(seed_) / 2147483648.0 - 1;
  }

  double sampleRate_;
  Adsr amp_, pit_, noi_;
  std::map<int, ClickSample> bank_;
  int note_ = 60;
  double velocity_ = 1;
  bool hit_ = false;
  double phase_ = 0;
  std::uint32_t seed_ = 0x9e3779b9u;
  bool kOn_ = false;
  double k_ = 0, kLen_ = 0, kNorm_ = 1, kTail_ = 0;
  const ClickSample* kSample_ = nullptr;
  double b0_ = 0, b1_ = 0, b2_ = 0, brown_ = 0, w1_ = 0, w2_ = 0, lp_ = 0;
  double last_ = 0, carry_ = 0;
};

}  // namespace

void registerDrumSynth(NodeRegistry& registry) {
  NodeType type;
  type.type = "drumsynth";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.params = {
    {"startPitch", 20, 4000, 220}, {"endPitch", 20, 2000, 52}, {"pitchMul", 0.25, 4, 1}, {"keytrack", 0, 1, 1, true},
    {"root", 0, 127, 60, true}, {"patk", 0.1, 500, 0.1}, {"pdec", 1, 2000, 55}, {"psus", 0, 1, 0}, {"prel", 1, 2000, 40},
    {"body", 0, 1, 0.9}, {"attack", 0.1, 500, 0.5}, {"decay", 1, 3000, 280}, {"sustain", 0, 1, 0}, {"release", 1, 3000, 60},
    {"drive", 0, 1, 0.15}, {"noise", 0, 1, 0.15}, {"noiseType", 0, 4, 0, true}, {"noiseTone", 200, 16000, 9000},
    {"natk", 0.1, 500, 0.1}, {"ndec", 1, 3000, 90}, {"nsus", 0, 1, 0}, {"nrel", 1, 3000, 30}, {"click", 0, 1, 0.4},
    {"clickMode", 0, 1, 0, true}, {"clickLen", 0.3, 120, 6}, {"clickTone", 400, 12000, 3500}, {"clickSample", 0, 255, 0, true},
    {"clickTail", 0, 60, 4}, {"vel", 0, 1, 0.6}, {"level", 0, 1, 0.7},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<DrumSynth>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

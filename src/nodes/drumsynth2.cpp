// Drum synth 2: a drum hit from three parts whose envelopes are drawn as Bezier splines: a body (a sine whose pitch and
// volume follow drawings), noise (six colours and two more), and a short click (a damped sine, or the start of a sample),
// then a low-shelf boost and a soft limiter. Monophonic; the voices node makes it polyphonic.
// Port of webtracker src/audio/nodes/drumsynth2.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/bezier.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kStartPitch = 0, kEndPitch, kPitchMul, kKeytrack, kRoot, kPrate, kBody, kArate, kHarm, kDrive, kNoise, kNoiseType,
             kNoiseTone, kNoiseHigh, kNrate, kClick, kClickMode, kClickLen, kClickTone, kClickSample, kClickTail, kBoost,
             kBoostFreq, kVel, kLevel };

constexpr double kReferenceMs[3] = {250, 700, 400};   // pitch, amp, noise

double softLimit(double y) {
  const double a = std::fabs(y);
  return a <= 0.8 ? y : sign(y) * (0.8 + 0.2 * js::tanh((a - 0.8) / 0.2));
}

struct ClickSample { std::vector<float> data; double ratio = 1; };

class DrumSynth2 final : public Node {
public:
  explicit DrumSynth2(const NodeContext& context) : sampleRate_(context.sampleRate) {
    loadDrawings(context.data);
  }

  void data(const Json& data) override { loadDrawings(&data); }

private:
  void loadDrawings(const Json* data) {
    static const double pitchR[] = {0.015, -0.7}, pitchL[] = {-0.6, 0};
    static const double ampR[] = {0.06, 0}, ampL[] = {-0.55, 0};
    static const double noiseR[] = {0.02, -0.55}, noiseL[] = {-0.4, 0};
    const bezier::Spline defaults[3] = {
      {bezier::makeAnchor(0, 1, nullptr, pitchR), bezier::makeAnchor(1, 0, pitchL, nullptr)},
      {bezier::makeAnchor(0, 1, nullptr, ampR), bezier::makeAnchor(1, 0, ampL, nullptr)},
      {bezier::makeAnchor(0, 1, nullptr, noiseR), bezier::makeAnchor(1, 0, noiseL, nullptr)}};
    static const char* const keys[3] = {"pitch", "amp", "noise"};
    for (int k = 0; k < 3; k++) tables_[k] = bezier::envelopeTable(bezier::drawingOf(data, keys[k], defaults[k]));
  }

public:
  // The click's sample, mixed down to one channel.
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
    if (event.type == NoteType::On) { note_ = event.note; velocity_ = event.vel; hit_ = true; held_ = 0; fade_ = 1; off_ = false; playing_ = true; }
    else if (event.type == NoteType::Off || event.type == NoteType::AllOff) off_ = true;
    else if (event.type == NoteType::Cut) { kOn_ = false; hit_ = false; playing_ = false; last_ = carry_ = 0; z1_ = z2_ = 0; }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (!playing_ && !kOn_ && !hit_) {
      std::fill(outL, outL + n, 0.0f);
      std::fill(outR, outR + n, 0.0f);
      last_ = carry_ = 0;
      return;
    }
    const auto g = [&](int p) -> double { return params[slots[p]]; };
    const double sr = sampleRate_;
    const double ms = sr * 0.001, keyed = g(kKeytrack) >= 1 ? js::pow(2, (note_ - g(kRoot)) / 12) : 1, mul = g(kPitchMul) * keyed;
    const double lo = g(kEndPitch), ratio = std::max(1.0, g(kStartPitch)) / std::max(1.0, lo);
    const double harm = g(kHarm), bodyLevel = g(kBody), noiseAmt = g(kNoise);
    const int type = jsRoundInt(g(kNoiseType));
    const double clickAmt = g(kClick);
    const double nf = 1 - js::exp(-2 * kPi * std::min(g(kNoiseTone), sr * 0.45) / sr);
    const double hf = 1 - js::exp(-2 * kPi * std::min(g(kNoiseHigh), sr * 0.45) / sr);
    const int stepEvery = std::max(1, jsRoundInt(sr / (2 * std::max(50.0, g(kNoiseTone)))));
    const double v = 1 - g(kVel) + g(kVel) * velocity_, level = g(kLevel) * v, d = 1 + g(kDrive) * 4, dn = 1 / js::tanh(d);
    const double pStep = g(kPrate) / (kReferenceMs[0] * ms), aStep = g(kArate) / (kReferenceMs[1] * ms), nStep = g(kNrate) / (kReferenceMs[2] * ms);
    const double boost = g(kBoost);
    double bb0 = 1, bb1 = 0, bb2 = 0, ba1 = 0, ba2 = 0;
    if (boost > 0.05) {   // a low shelf (RBJ, shelf slope 1)
      const double A = js::pow(10, boost / 40), w0 = 2 * kPi * std::min(g(kBoostFreq), sr * 0.2) / sr, cs = js::cos(w0), sn = js::sin(w0);
      const double al = sn / 2 * kSqrt2, tw = 2 * std::sqrt(A) * al, a0 = (A + 1) + (A - 1) * cs + tw;
      bb0 = A * ((A + 1) - (A - 1) * cs + tw) / a0; bb1 = 2 * A * ((A - 1) - (A + 1) * cs) / a0; bb2 = A * ((A + 1) - (A - 1) * cs - tw) / a0;
      ba1 = -2 * ((A - 1) + (A + 1) * cs) / a0; ba2 = ((A + 1) + (A - 1) * cs - tw) / a0;
    }
    if (hit_) {   // a new hit: the click starts, from its sample when that is what it is
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
          kNorm_ = pk > 1e-6 ? 1 / pk : 1;   // the click is as loud as its amount says, whatever the sample's own level
          if (kLen_ < 1) kOn_ = false;
        }
      }
      kTail_ = std::min(kLen_, g(kClickTail) * ms);
    }
    const double kRamp = std::max(2.0, jsRound(0.00015 * sr)), cd = js::exp(-1 / (0.0015 * sr));
    const double tone = 2 * kPi * g(kClickTone) / sr, kEnd = kSample_ ? kLen_ : kLen_ * 1.5;
    const double fadeStep = 1 / (0.04 * sr);
    const bezier::Table& tp = tables_[0];
    const bezier::Table& ta = tables_[1];
    const bezier::Table& tn = tables_[2];
    for (int i = 0; i < n; i++) {
      const double pe = bezier::readEnvelope(tp, held_ * pStep), f = lo * js::pow(ratio, pe) * mul;
      phase_ += f / sr;
      if (phase_ >= 1) phase_ -= std::floor(phase_);
      const double ae = bezier::readEnvelope(ta, held_ * aStep), th = 6.283185307179586 * phase_;
      const double body = js::sin(th + harm * 2 * ae * js::sin(th)) * bodyLevel * ae;
      const double w = white();
      double nz;
      if (type == 1) { b0_ = 0.99765 * b0_ + w * 0.099046; b1_ = 0.963 * b1_ + w * 0.2965164; b2_ = 0.57 * b2_ + w * 1.0526913; nz = (b0_ + b1_ + b2_ + w * 0.1848) * 0.3; }
      else if (type == 2) { brown_ = (brown_ + 0.02 * w) / 1.02; nz = brown_ * 3.5; }
      else if (type == 3) nz = (w - w1_) * 0.7;
      else if (type == 4) nz = (w - 2 * w1_ + w2_) * 0.4;
      else if (type == 5) { const double r = white(); nz = r > 0.96 ? (r > 0.98 ? 1.6 : -1.6) : 0; }   // velvet
      else if (type == 6) { const double r = white(); const bool burst = std::fabs(r) > 0.985; nz = burst ? white() * 2.2 : 0; }   // crackle
      else if (type == 7) { if (--stepLeft_ <= 0) { stepHold_ = w; stepLeft_ = stepEvery; } nz = stepHold_; }   // stepped
      else nz = w;
      w2_ = w1_; w1_ = w;
      lp_ += nf * (nz - lp_);
      hpLow_ += hf * (lp_ - hpLow_);
      const double noise = (lp_ - hpLow_) * noiseAmt * bezier::readEnvelope(tn, held_ * nStep);
      double click = 0;
      if (kOn_) {
        if (kSample_) {
          const std::vector<float>& s = kSample_->data;
          const double pos = k_ * kSample_->ratio, a = std::floor(pos), fr = pos - a;
          const std::size_t ia = static_cast<std::size_t>(a);
          const double sa = s[ia], sb = ia + 1 < s.size() ? s[ia + 1] : sa;
          double c = (sa + (sb - sa) * fr) * kNorm_;
          if (kTail_ > 0 && k_ > kLen_ - kTail_) c *= 0.5 + 0.5 * js::cos(kPi * (k_ - (kLen_ - kTail_)) / kTail_);   // a raised-cosine fade
          click = c;
        } else {
          click = (js::sin(tone * k_) * 0.8 + w * 0.3) * js::exp(-6.9078 * k_ / kLen_);
        }
        click *= clickAmt * 0.9 * std::min(1.0, (k_ + 1) / kRamp);
        if (++k_ >= kEnd) kOn_ = false;
      }
      held_++;
      if (off_) fade_ = std::max(0.0, fade_ - fadeStep);
      const double fresh = js::tanh((body + noise * 0.7 + click) * d) * dn * level * fade_;
      const double y = fresh + carry_;
      carry_ *= cd; last_ = fresh;
      double out = y;
      if (boost > 0.05) { const double o = bb0 * y + z1_; z1_ = bb1 * y - ba1 * o + z2_; z2_ = bb2 * y - ba2 * o; out = softLimit(o); }
      outL[i] = static_cast<float>(out);
      outR[i] = static_cast<float>(out);
    }
    if (std::fabs(carry_) < 1e-6) carry_ = 0;
    if (!std::isfinite(lp_ + hpLow_ + brown_ + b0_ + b1_ + b2_ + z1_ + z2_)) lp_ = hpLow_ = brown_ = b0_ = b1_ = b2_ = z1_ = z2_ = 0;
    // finished: the drawings are over, the volume holds nothing, and the click and its carry are gone
    const double doneAt = std::max({1 / aStep, noiseAmt > 0 ? 1 / nStep : 0.0, kEnd + 4 * ms});
    if (held_ > doneAt && ta[bezier::kEnvPoints] < 1e-3 && (noiseAmt == 0 || tn[bezier::kEnvPoints] < 1e-3) && !kOn_ && carry_ == 0 && !hit_) playing_ = false;
    if (off_ && fade_ <= 0) playing_ = false;
  }

private:
  double white() {
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    return static_cast<double>(seed_) / 2147483648.0 - 1;
  }

  double sampleRate_;
  bezier::Table tables_[3];
  std::map<int, ClickSample> bank_;
  int note_ = 60;
  double velocity_ = 1;
  bool hit_ = false;
  double phase_ = 0;
  std::uint32_t seed_ = 0x9e3779b9u;
  double held_ = 0, fade_ = 1;
  bool off_ = false;
  bool kOn_ = false;
  double k_ = 0, kLen_ = 0, kNorm_ = 1, kTail_ = 0;
  const ClickSample* kSample_ = nullptr;
  double b0_ = 0, b1_ = 0, b2_ = 0, brown_ = 0, w1_ = 0, w2_ = 0, lp_ = 0, hpLow_ = 0, stepHold_ = 0;
  int stepLeft_ = 0;
  double last_ = 0, carry_ = 0, z1_ = 0, z2_ = 0;
  bool playing_ = false;
};

}  // namespace

void registerDrumSynth2(NodeRegistry& registry) {
  NodeType type;
  type.type = "drumsynth2";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.params = {
    {"startPitch", 20, 4000, 160},    // Hz at the top of the pitch drawing, at the root note
    {"endPitch", 20, 2000, 48},       // Hz at the bottom of it
    {"pitchMul", 0.25, 4, 1},
    {"keytrack", 0, 1, 1, true},
    {"root", 0, 127, 60, true},
    {"prate", 0.1, 10, 1.5},          // how fast the pitch drawing plays
    {"body", 0, 1, 0.9},
    {"arate", 0.1, 10, 1.6},          // the volume drawing
    {"harm", 0, 1, 0.35},             // harmonics of the body, strongest at the start of the volume drawing
    {"drive", 0, 1, 0.3},
    {"noise", 0, 1, 0.15},
    {"noiseType", 0, 7, 0, true},
    {"noiseTone", 200, 16000, 9000},  // Hz, low-pass on the noise (for stepped, how often it steps)
    {"noiseHigh", 20, 8000, 20},      // Hz, high-pass on the noise
    {"nrate", 0.1, 10, 1},            // the noise drawing
    {"click", 0, 1, 0.4},
    {"clickMode", 0, 1, 0, true},
    {"clickLen", 0.3, 120, 6},
    {"clickTone", 400, 12000, 3500},
    {"clickSample", 0, 255, 0, true},
    {"clickTail", 0, 60, 4},
    {"boost", 0, 18, 6},              // dB of low shelf at the end
    {"boostFreq", 25, 200, 55},       // Hz
    {"vel", 0, 1, 0.6},
    {"level", 0, 1, 0.7},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<DrumSynth2>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

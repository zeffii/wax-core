// Lone: a one-node lead synth in the manner of the classic monophonic analogue leads, with a Braids wavetable added.
// A saw, a pulse, the wavetable, a sub oscillator and noise (each with its own level), hard sync, a wave folder, the 24 dB/oct
// ladder low-pass of the subtractive synth, an LFO with a fade-in, glide, slide and legato. Monophonic, last note wins.
// Port of webtracker src/audio/nodes/lone.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "core/js_math.h"
#include "nodes/adsr.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"
#include "nodes/wave_table_data.h"

namespace wax {
namespace {

struct Spec { const char* name; double min, max, def; bool isInt; };
const Spec kSpec[] = {
  {"saw", 0, 1, 0.8, false}, {"pulse", 0, 1, 0, false}, {"pw", 0.05, 0.95, 0.5, false}, {"pwlfo", 0, 0.45, 0, false},
  {"pwenv", -0.45, 0.45, 0, false}, {"wt", 0, 1, 0, false}, {"position", 0, 1, 0.2, false}, {"pamt", -1, 1, 0, false},
  {"plfo", 0, 1, 0, false}, {"patk", 0.1, 5000, 5, false}, {"pdec", 1, 5000, 600, false}, {"psus", 0, 1, 0, false},
  {"prel", 1, 5000, 300, false}, {"sub", 0, 1, 0.4, false}, {"suboct", 1, 2, 1, true}, {"subwave", 0, 1, 0, true},
  {"noise", 0, 1, 0, false}, {"sync", 0, 1, 0, true}, {"syncratio", 1, 8, 2, false}, {"fold", 0, 1, 0, false},
  {"cutoff", 20, 20000, 900, false}, {"res", 0, 1, 0.3, false}, {"drive", 0, 1, 0.2, false}, {"fenv", -8, 8, 3, false},
  {"keytrack", 0, 1, 0.5, false}, {"fmod", 0, 4, 0, false}, {"fatk", 0.1, 5000, 1, false}, {"fdec", 1, 5000, 250, false},
  {"fsus", 0, 1, 0.2, false}, {"frel", 1, 5000, 150, false}, {"atk", 0.1, 5000, 2, false}, {"dec", 1, 5000, 300, false},
  {"sus", 0, 1, 0.8, false}, {"rel", 1, 5000, 150, false}, {"lfoRate", 0.05, 40, 5, false}, {"lfoShape", 0, 3, 0, true},
  {"lfoDelay", 0, 5000, 0, false}, {"vib", 0, 200, 0, false}, {"glide", 0, 2000, 0, false}, {"slide", 0, 15.9375, 0, false},
  {"legato", 0, 1, 0, true}, {"tune", -24, 24, 0, false}, {"fine", -100, 100, 0, false}, {"vel", 0, 1, 0.5, false},
  {"level", 0, 1, 0.5, false}, {"smooth", 0, 500, 10, false},
};
enum P {
  kSaw, kPulse, kPw, kPwlfo, kPwenv, kWt, kPosition, kPamt, kPlfo, kPatk, kPdec, kPsus, kPrel, kSub, kSuboct, kSubwave, kNoise, kSync,
  kSyncratio, kFold, kCutoff, kRes, kDrive, kFenv, kKeytrack, kFmod, kFatk, kFdec, kFsus, kFrel, kAtk, kDec, kSus, kRel, kLfoRate,
  kLfoShape, kLfoDelay, kVib, kGlide, kSlide, kLegato, kTune, kFine, kVel, kLevel, kSmooth
};

constexpr int kSubBlock = 8;
constexpr int kFrame = kTableFrame;

double blep(double t, double dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1; }
  if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
  return 0;
}
double sawAt(double p, double dt) { return 2 * p - 1 - blep(p, dt); }
double pulseAt(double p, double dt, double pw) {
  double q = p + 1 - pw;
  if (q >= 1) q -= 1;
  return (p < pw ? 1 : -1) + blep(p, dt) - blep(q, dt);
}
double lfoAt(int shape, double p, double held) {
  if (shape == 1) return p < 0.5 ? 1 : -1;
  if (shape == 2) return 1 - 2 * p;
  if (shape == 3) return held;
  return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
}
double fold(double x, double amount) {
  if (amount <= 0) return x;
  return js::sin(x * (1 + amount * 5) * (kPi / 2)) / (1 + amount * 0.3);
}
double flush(double v) { return std::abs(v) < 1e-18 ? 0 : v; }

class Lone final : public Node {
public:
  explicit Lone(const NodeContext& context)
      : sampleRate_(context.sampleRate), clock_(context.clock), pitchOn_(context.connected[0]), cutOn_(context.connected[1]),
        posOn_(context.connected[2]), table_(&bundledWaveFrames()), count_(static_cast<int>(table_->size()) / kFrame),
        amp_(context.sampleRate), fil_(context.sampleRate), posEnv_(context.sampleRate) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      const bool tied = legatoOn_ && amp_.held();
      ampFresh_ = amp_.idle();
      if (amp_.idle()) {
        dcIn_ = dcOut_ = 0; shown_ = -1; master_ = slave_ = subPhase_ = 0; lfoPhase_ = 0; since_ = 0;
        for (double& v : s_) v = 0;
        pos_ = event.note;
      }
      note_ = event.note; velocity_ = event.vel;
      latch_ = true; from_ = event.from;
      if (!tied) { amp_.on(); fil_.on(); posEnv_.on(); since_ = 0; }
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) {
      amp_.off(); fil_.off(); posEnv_.off();
    } else {
      amp_.cut(); fil_.cut(); posEnv_.cut();
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (amp_.idle()) {
      std::fill(outL, outL + n, 0.0f);
      std::fill(outR, outR + n, 0.0f);
      return;
    }
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    legatoOn_ = g(kLegato) >= 0.5;
    const float* pitchIn = inputs[0];
    const float* cutIn = inputs[1];
    const float* posIn = inputs[2];
    amp_.set(g(kAtk), g(kDec), g(kSus), g(kRel));
    fil_.set(g(kFatk), g(kFdec), g(kFsus), g(kFrel));
    posEnv_.set(g(kPatk), g(kPdec), g(kPsus), g(kPrel));
    const double glideMs = g(kGlide);
    if (latch_) {
      latch_ = false;
      const double ticks = g(kSlide), tickSamples = clock_ ? clock_->tickSamples : sampleRate_ * 0.125;
      if (ticks > 0) {
        if (from_ >= 0 && pos_ != from_ && ampFresh_) pos_ = from_;
        rate_ = std::abs(note_ - pos_) / (ticks * tickSamples);
      } else rate_ = 0;
      ampFresh_ = false;
    }
    if (rate_ > 0) {
      const double step = rate_ * n;
      const double d = note_ - pos_;
      pos_ = std::abs(d) <= step ? static_cast<double>(note_) : pos_ + (d > 0 ? 1.0 : d < 0 ? -1.0 : 0.0) * step;
      if (pos_ == note_) rate_ = 0;
    } else if (glideMs > 0.5) pos_ += (note_ - pos_) * (1 - js::exp(-static_cast<double>(n) / (sampleRate_ * glideMs * 0.001 / 3)));
    else pos_ = note_;

    const double sawL = g(kSaw), pulseL = g(kPulse), wtL = g(kWt), subL = g(kSub), noiseL = g(kNoise);
    const double pw0 = g(kPw), pwLfo = g(kPwlfo), pwEnv = g(kPwenv);
    const double position = g(kPosition), pAmt = g(kPamt), pLfo = g(kPlfo);
    const double subDiv = g(kSuboct) >= 1.5 ? 4 : 2;
    const bool subSine = g(kSubwave) >= 0.5;
    const bool syncOn = g(kSync) >= 0.5;
    const double ratio = syncOn ? g(kSyncratio) : 1;
    const double foldAmt = g(kFold);
    const double lfoRate = g(kLfoRate) / sampleRate_;
    const int shape = jsRoundInt(g(kLfoShape));
    const double delay = g(kLfoDelay) * sampleRate_ * 0.001;
    const double vib = g(kVib) / 1200, fmod = g(kFmod);
    const double cutoff = g(kCutoff), res = g(kRes), k = res * 4, drive = 1 + g(kDrive) * 4, comp = 1 + res * 0.5;
    const double fenv = g(kFenv), keyOct = (g(kKeytrack) * (pos_ - 60)) / 12;
    const double baseHz = 440 * js::pow(2, (pos_ + g(kTune) + g(kFine) / 100 - 69) / 12);
    const double level = g(kLevel) * (1 - g(kVel) + g(kVel) * velocity_) * 2.2;
    const double dcR = 1 - (kTwoPi * 10) / sampleRate_, nyq = sampleRate_ * 0.45;
    const double slew = g(kSmooth) > 0 ? 1 - js::exp(-1 / (sampleRate_ * g(kSmooth) * 0.001)) : 1;
    if (shown_ < 0) shown_ = position;
    double G = 0, G2 = 0, G3 = 0, G4 = 0, norm = 1;
    const float* table = table_->data();
    for (int i = 0; i < n; i++) {
      const double a = amp_.next(), f = fil_.next(), pe = posEnv_.next();
      lfoPhase_ += lfoRate;
      if (lfoPhase_ >= 1) { lfoPhase_ -= 1; seed_ = seed_ * 1664525u + 1013904223u; held_ = seed_ / 2147483648.0 - 1; }
      since_++;
      const double fade = delay > 0 ? std::min(1.0, since_ / delay) : 1;
      const double lfo = lfoAt(shape, lfoPhase_, held_) * fade;
      if ((i & (kSubBlock - 1)) == 0) {
        const double oct = fenv * f + keyOct + fmod * lfo + (cutOn_ ? static_cast<double>(cutIn[i]) : 0.0);
        const double hz = std::min(nyq, std::max(10.0, cutoff * js::pow(2, oct)));
        const double gg = js::tan((kPi * hz) / sampleRate_);
        G = gg / (1 + gg); G2 = G * G; G3 = G2 * G; G4 = G2 * G2;
        norm = 1 / (1 + k * G4);
      }
      const double fm = js::pow(2, (vib > 0 ? vib * lfo : 0) + (pitchOn_ ? static_cast<double>(pitchIn[i]) : 0.0));
      const double inc = std::min(0.2, (baseHz * fm) / sampleRate_);
      const double sinc = std::min(0.45, inc * ratio);
      shown_ += (position - shown_) * slew;
      double v = 0;
      if (sawL > 0 || pulseL > 0 || wtL > 0) {
        if (sawL > 0) v += sawL * sawAt(slave_, sinc);
        if (pulseL > 0) {
          const double pw = std::max(0.05, std::min(0.95, pw0 + pwLfo * lfo + pwEnv * f));
          v += pulseL * 0.8 * pulseAt(slave_, sinc, pw);
        }
        if (wtL > 0) {
          double p = shown_ + pAmt * pe + pLfo * 0.5 * lfo + (posOn_ ? static_cast<double>(posIn[i]) : 0.0);
          p = p < 0 ? 0 : p > 1 ? 1 : p;
          const double fp = p * (count_ - 1), kf = std::min(static_cast<double>(count_ - 2), std::floor(fp)), ff = fp - kf;
          const double x = slave_ * kFrame, jf = std::floor(x), t = x - jf;
          const int j = static_cast<int>(jf), j1 = j + 1 >= kFrame ? 0 : j + 1;
          const std::size_t b0 = static_cast<std::size_t>(kf) * kFrame, b1 = b0 + kFrame;
          const double wa0 = table[b0 + static_cast<std::size_t>(j)], wa1 = table[b0 + static_cast<std::size_t>(j1)];
          const double wb0 = table[b1 + static_cast<std::size_t>(j)], wb1 = table[b1 + static_cast<std::size_t>(j1)];
          const double wa = wa0 + (wa1 - wa0) * t;
          const double wb = wb0 + (wb1 - wb0) * t;
          v += wtL * (wa + (wb - wa) * ff);
        }
      }
      if (subL > 0) {
        const double sinc2 = inc / subDiv;
        v += subL * (subSine ? js::sin(kTwoPi * subPhase_) : pulseAt(subPhase_, sinc2, 0.5) * 0.8);
      }
      subPhase_ += inc / subDiv;
      if (subPhase_ >= 1) subPhase_ -= 1;
      if (noiseL > 0) { seed_ = seed_ * 1664525u + 1013904223u; v += noiseL * (seed_ / 2147483648.0 - 1); }
      master_ += inc;
      if (master_ >= 1) {
        master_ -= 1;
        slave_ = syncOn ? std::fmod(master_ * ratio, 1.0) : slave_ + sinc;
        if (!syncOn && slave_ >= 1) slave_ -= 1;
      } else {
        slave_ += sinc;
        if (slave_ >= 1) slave_ -= 1;
      }
      v = fold(v * 0.6, foldAmt);
      dcOut_ = v - dcIn_ + dcR * dcOut_; dcIn_ = v; v = dcOut_;
      const double hh = 1 - G;
      const double S1 = hh * s_[0], S2 = hh * s_[1], S3 = hh * s_[2], S4 = hh * s_[3];
      const double sigma = G3 * S1 + G2 * S2 + G * S3 + S4;
      const double z = (v * comp * drive - k * sigma) * norm, z2 = z * z;
      const double x1 = z > 3 ? 1 : z < -3 ? -1 : z * (27 + z2) / (27 + 9 * z2);
      const double y1 = G * x1 + S1, y2 = G * y1 + S2, y3 = G * y2 + S3, y4 = G * y3 + S4;
      s_[0] = flush(2 * y1 - s_[0]); s_[1] = flush(2 * y2 - s_[1]); s_[2] = flush(2 * y3 - s_[2]); s_[3] = flush(2 * y4 - s_[3]);
      const double out = (y4 * a * level) / drive;
      outL[i] = static_cast<float>(out);
      outR[i] = static_cast<float>(out);
    }
  }

private:
  double sampleRate_;
  const Clock* clock_;
  bool pitchOn_, cutOn_, posOn_;
  const std::vector<float>* table_;
  int count_;
  Adsr amp_, fil_, posEnv_;
  double master_ = 0, slave_ = 0, subPhase_ = 0, lfoPhase_ = 0, held_ = 0, since_ = 0;
  double dcIn_ = 0, dcOut_ = 0, shown_ = -1, pos_ = 60, velocity_ = 1;
  int note_ = 60;
  std::uint32_t seed_ = 4242;
  bool legatoOn_ = false, latch_ = false, ampFresh_ = false;
  int from_ = -1;
  double rate_ = 0;
  double s_[4] = {0, 0, 0, 0};
};

}  // namespace

void registerLone(NodeRegistry& registry) {
  NodeType type;
  type.type = "lone";
  type.inputs = {"pitch", "cut", "pos"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"cut", PortKind::Control}, {"pos", PortKind::Control}, {"out", PortKind::Stereo}};
  for (const Spec& s : kSpec) type.params.push_back({s.name, s.min, s.max, s.def, s.isInt});
  type.create = [](const NodeContext& context) { return std::make_unique<Lone>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

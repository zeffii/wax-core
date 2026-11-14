// Cardinal: a monophonic virtual-analogue lead voice. Two oscillators (triangle, saw, pulse, sine or noise; sync, fm, ring, key
// tracking), unison of up to five detuned and panned copies, a filter (ladder with saturation, or a 12 dB state-variable type),
// an amp envelope and a mod envelope, two LFOs with a destination each, glide and slide. Port of webtracker
// src/audio/nodes/cardinal.js (without the node's picture).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "core/js_math.h"
#include "nodes/adsr.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

struct Spec { const char* name; double min, max, def; bool isInt; };
const Spec kSpec[] = {
  {"o1wave", 0, 4, 1, true}, {"o1pw", 0.05, 0.95, 0.5, false}, {"o2wave", 0, 4, 1, true}, {"o2pw", 0.05, 0.95, 0.5, false},
  {"o2semi", -24, 24, 0, false}, {"o2fine", -100, 100, 6, false}, {"o2track", 0, 1, 1, true}, {"o2sync", 0, 1, 0, true},
  {"fm", 0, 1, 0, false}, {"ring", 0, 1, 0, false}, {"mix", 0, 1, 0.5, false}, {"noise", 0, 1, 0, false},
  {"unison", 1, 5, 1, true}, {"detune", 0, 50, 12, false}, {"width", 0, 1, 0.7, false},
  {"ftype", 0, 4, 0, true}, {"cutoff", 20, 20000, 1500, false}, {"res", 0, 1, 0.25, false}, {"dist", 0, 1, 0.15, false},
  {"fkey", 0, 1, 0.5, false},
  {"matk", 0.1, 5000, 1, false}, {"mdec", 1, 5000, 400, false}, {"msus", 0, 1, 0, false}, {"mrel", 1, 5000, 200, false},
  {"e2cut", -8, 8, 3, false}, {"e2pitch", -24, 24, 0, false}, {"e2fm", 0, 1, 0, false}, {"e2pw", -0.45, 0.45, 0, false},
  {"atk", 0.1, 5000, 2, false}, {"dec", 1, 5000, 300, false}, {"sus", 0, 1, 0.8, false}, {"rel", 1, 5000, 150, false},
  {"vel", 0, 1, 0.5, false},
  {"l1wave", 0, 4, 0, true}, {"l1rate", 0.02, 50, 5, false}, {"l1amt", 0, 1, 0, false}, {"l1dest", 0, 5, 0, true},
  {"l2wave", 0, 4, 0, true}, {"l2rate", 0.02, 50, 0.5, false}, {"l2amt", 0, 1, 0, false}, {"l2dest", 0, 5, 4, true},
  {"glide", 0, 2000, 0, false}, {"slide", 0, 15.9375, 0, false}, {"level", 0, 1, 0.4, false}, {"quality", 0, 1, 0, true},
};
enum P {
  kO1wave, kO1pw, kO2wave, kO2pw, kO2semi, kO2fine, kO2track, kO2sync, kFm, kRing, kMix, kNoise, kUnison, kDetune, kWidth,
  kFtype, kCutoff, kRes, kDist, kFkey, kMatk, kMdec, kMsus, kMrel, kE2cut, kE2pitch, kE2fm, kE2pw, kAtk, kDec, kSus, kRel, kVel,
  kL1wave, kL1rate, kL1amt, kL1dest, kL2wave, kL2rate, kL2amt, kL2dest, kGlide, kSlide, kLevel, kQuality
};

constexpr int kMaxV = 5, kControlBlock = 8;
constexpr double kLfoPitch = 12, kLfoFilter = 4, kLfoPw = 0.45;

double blep(double t, double dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1; }
  if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
  return 0;
}
double wrap01(double p) { return p - std::floor(p); }

double oscillator(int wave, double p, double dt, double pw, double noise) {
  switch (wave) {
    case 0: return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;
    case 1: return 2 * p - 1 - blep(p, dt);
    case 2: { const double q = wrap01(p + 1 - pw); return (p < pw ? 1 : -1) + blep(p, dt) - blep(q, dt); }
    case 3: return js::sin(kTwoPi * p);
    default: return noise;
  }
}
double lfoShape(int wave, double p, double held) {
  switch (wave) {
    case 0: return js::sin(kTwoPi * p);
    case 1: return p < 0.5 ? 4 * p - 1 : 3 - 4 * p;
    case 2: return 2 * p - 1;
    case 3: return p < 0.5 ? 1 : -1;
    default: return held;
  }
}
double flush(double v) { return std::abs(v) < 1e-18 ? 0 : v; }

class Cardinal final : public Node {
public:
  explicit Cardinal(const NodeContext& context)
      : sampleRate_(context.sampleRate), clock_(context.clock),
        pitchOn_(context.connected.size() > 0 && context.connected[0]), cutOn_(context.connected.size() > 1 && context.connected[1]),
        amp_(context.sampleRate), mod_(context.sampleRate) {
    for (int i = 0; i < kMaxV; i++) startPhase_[i] = std::fmod(i * 0.61803398875, 1.0);
    restartPhases();
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      ampFresh_ = amp_.idle();
      if (amp_.idle()) { restartPhases(); for (double& v : ladder_) v = 0; for (double& v : svf_) v = 0; pos_ = event.note; }
      note_ = event.note; velocity_ = event.vel;
      latch_ = true; from_ = event.from;
      amp_.on(); mod_.on();
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) { amp_.off(); mod_.off(); }
    else { amp_.cut(); mod_.cut(); }
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
    const double sr = sampleRate_;
    const float* pitchIn = inputs[0];
    const float* cutIn = inputs[1];
    amp_.set(g(kAtk), g(kDec), g(kSus), g(kRel));
    mod_.set(g(kMatk), g(kMdec), g(kMsus), g(kMrel));

    if (latch_) {
      latch_ = false;
      const double ticks = g(kSlide), tickSamples = clock_ ? clock_->tickSamples : sr * 0.125;
      if (ticks > 0) {
        if (from_ >= 0 && pos_ != from_ && ampFresh_) pos_ = from_;
        slideRate_ = std::abs(note_ - pos_) / (ticks * tickSamples);
      } else slideRate_ = 0;
      ampFresh_ = false;
    }
    if (slideRate_ > 0) {
      const double step = slideRate_ * n, d = note_ - pos_;
      pos_ = std::abs(d) <= step ? static_cast<double>(note_) : pos_ + (d > 0 ? 1.0 : d < 0 ? -1.0 : 0.0) * step;
      if (pos_ == note_) slideRate_ = 0;
    } else if (g(kGlide) > 0.5) pos_ += (note_ - pos_) * (1 - js::exp(-static_cast<double>(n) / (sr * g(kGlide) * 0.001 / 3)));
    else pos_ = note_;

    const int w1 = jsRoundInt(g(kO1wave)), w2 = jsRoundInt(g(kO2wave));
    const double pw1 = g(kO1pw), pw2 = g(kO2pw);
    const bool sync = g(kO2sync) >= 0.5, tracks = g(kO2track) >= 0.5;
    const double fmAmount = g(kFm), ring = g(kRing), mix = g(kMix), noiseLevel = g(kNoise);
    const int voices = std::max(1, std::min(kMaxV, jsRoundInt(g(kUnison))));
    const double detune = g(kDetune), width = g(kWidth);
    const double baseHz = 440 * js::pow(2, (pos_ - 69) / 12), osc2Hz = tracks ? baseHz : 440 * js::pow(2, (60 - 69) / 12);
    const double osc2Semi = g(kO2semi) + g(kO2fine) / 100;
    const double voiceGain = 1 / std::sqrt(static_cast<double>(voices));
    for (int u = 0; u < voices; u++) {
      const double x = voices == 1 ? 0 : (2.0 * u) / (voices - 1) - 1;
      inc1_[u] = (baseHz * js::pow(2, (x * detune) / 1200)) / sr;
      inc2_[u] = (osc2Hz * js::pow(2, (osc2Semi * 100 + x * detune) / 1200)) / sr;
      const double a = ((x * width + 1) * kPi) / 4;
      panL_[u] = js::cos(a) * kSqrt2 * voiceGain;
      panR_[u] = js::sin(a) * kSqrt2 * voiceGain;
    }
    const int type = jsRoundInt(g(kFtype));
    const double cutoff = g(kCutoff), res = g(kRes);
    const double distGain = 1 + g(kDist) * 4, ladderK = res * 4, svfK = 2 - 1.95 * res;
    const double e2cut = g(kE2cut), e2pitch = g(kE2pitch), e2fm = g(kE2fm), e2pw = g(kE2pw);
    const double keyOct = (g(kFkey) * (pos_ - 60)) / 12;
    const double level = g(kLevel) * (1 - g(kVel) + g(kVel) * velocity_) * 1.5;
    const int lfoWave[2] = {jsRoundInt(g(kL1wave)), jsRoundInt(g(kL2wave))};
    const double lfoInc[2] = {g(kL1rate) / sr, g(kL2rate) / sr};
    const double lfoAmt[2] = {g(kL1amt), g(kL2amt)};
    const int lfoDest[2] = {jsRoundInt(g(kL1dest)), jsRoundInt(g(kL2dest))};
    const double nyquist = sr * 0.45;
    double G = 0, G2 = 0, G3 = 0, G4 = 0, ladderNorm = 1;
    double svfA1 = 0, svfA2 = 0, svfA3 = 0;
    const bool eco = g(kQuality) >= 0.5;
    double pitchMul = 1, osc2Mul = 1, fmDepth = 0, width1 = pw1, width2 = pw2;

    for (int i = 0; i < n; i++) {
      const double ampLevel = amp_.next(), modLevel = mod_.next();

      double pitchBoth = 0, pitch2 = 0, fmAdd = 0, pwAdd = 0, cutOct = 0, tremolo = 1;
      for (int k = 0; k < 2; k++) {
        lfoPhase_[k] += lfoInc[k];
        if (lfoPhase_[k] >= 1) { lfoPhase_[k] -= 1; lfoHeld_[k] = random(); }
        if (lfoAmt[k] == 0) continue;
        const double v = lfoShape(lfoWave[k], lfoPhase_[k], lfoHeld_[k]) * lfoAmt[k];
        switch (lfoDest[k]) {
          case 0: pitchBoth += v * kLfoPitch; break;
          case 1: pitch2 += v * kLfoPitch; break;
          case 2: fmAdd += v; break;
          case 3: pwAdd += v * kLfoPw; break;
          case 4: cutOct += v * kLfoFilter; break;
          default: tremolo *= 1 - lfoAmt[k] * (0.5 - 0.5 * lfoShape(lfoWave[k], lfoPhase_[k], lfoHeld_[k]));
        }
      }

      if ((i & (kControlBlock - 1)) == 0) {
        const double oct = e2cut * modLevel + keyOct + cutOct + (cutOn_ ? static_cast<double>(cutIn[i]) : 0.0);
        const double hz = std::min(nyquist, std::max(10.0, cutoff * js::pow(2, oct)));
        const double gg = js::tan((kPi * hz) / sr);
        if (type == 0) { G = gg / (1 + gg); G2 = G * G; G3 = G2 * G; G4 = G2 * G2; ladderNorm = 1 / (1 + ladderK * G4); }
        else { svfA1 = 1 / (1 + gg * (gg + svfK)); svfA2 = gg * svfA1; svfA3 = gg * svfA2; }
      }

      if (!eco || (i & (kControlBlock - 1)) == 0) {
        pitchMul = js::pow(2, (pitchBoth / 12) + (pitchOn_ ? static_cast<double>(pitchIn[i]) : 0.0));
        osc2Mul = pitchMul * js::pow(2, (pitch2 + e2pitch * modLevel) / 12);
        fmDepth = std::min(1.0, std::max(0.0, fmAmount + e2fm * modLevel + fmAdd)) * 0.5;
        width1 = std::min(0.95, std::max(0.05, pw1 + pwAdd + e2pw * modLevel));
        width2 = std::min(0.95, std::max(0.05, pw2 + pwAdd + e2pw * modLevel));
      }
      double l = 0, r = 0;
      for (int u = 0; u < voices; u++) {
        const double d1 = inc1_[u] * pitchMul, d2 = inc2_[u] * osc2Mul;
        const double n1 = w1 == 4 ? random() : 0;
        const double o1 = oscillator(w1, phase1_[u], d1, width1, n1);
        const double n2 = w2 == 4 ? random() : 0;
        const double o2 = oscillator(w2, wrap01(phase2_[u] + fmDepth * o1), d2, width2, n2);
        double v = o1 * (1 - mix) + o2 * mix;
        if (ring > 0) v = v * (1 - ring) + ring * o1 * o2 * 1.5;
        l += v * panL_[u]; r += v * panR_[u];
        double p1 = phase1_[u] + d1, p2 = phase2_[u] + d2;
        if (p1 >= 1) { p1 -= 1; if (sync) p2 = p1 * (d2 / d1); }
        if (p2 >= 1) p2 -= 1;
        phase1_[u] = p1; phase2_[u] = p2;
      }
      if (noiseLevel > 0) { const double w = noiseLevel * random(); l += w; r += w; }

      for (int ch = 0; ch < 2; ch++) {
        const double x = (ch ? r : l) * distGain;
        double y;
        if (type == 0) {
          const int o = ch * 4;
          const double h = 1 - G;
          const double S1 = h * ladder_[o], S2 = h * ladder_[o + 1], S3 = h * ladder_[o + 2], S4 = h * ladder_[o + 3];
          const double sigma = G3 * S1 + G2 * S2 + G * S3 + S4;
          const double z = (x * (1 + res * 0.5) - ladderK * sigma) * ladderNorm, z2 = z * z;
          const double s0 = z > 3 ? 1 : z < -3 ? -1 : z * (27 + z2) / (27 + 9 * z2);
          const double y1 = G * s0 + S1, y2 = G * y1 + S2, y3 = G * y2 + S3, y4 = G * y3 + S4;
          ladder_[o] = flush(2 * y1 - ladder_[o]); ladder_[o + 1] = flush(2 * y2 - ladder_[o + 1]);
          ladder_[o + 2] = flush(2 * y3 - ladder_[o + 2]); ladder_[o + 3] = flush(2 * y4 - ladder_[o + 3]);
          y = y4;
        } else {
          const int o = ch * 2;
          const double v3 = x - svf_[o + 1];
          const double v1 = svfA1 * svf_[o] + svfA2 * v3, v2 = svf_[o + 1] + svfA2 * svf_[o] + svfA3 * v3;
          svf_[o] = flush(2 * v1 - svf_[o]); svf_[o + 1] = flush(2 * v2 - svf_[o + 1]);
          const double high = x - svfK * v1 - v2;
          y = type == 1 ? v2 : type == 2 ? v1 * svfK : type == 3 ? high : v2 + high;
        }
        if (ch) r = y; else l = y;
      }
      const double gain = (ampLevel * level * tremolo) / distGain;
      outL[i] = static_cast<float>(l * gain);
      outR[i] = static_cast<float>(r * gain);
    }
  }

private:
  double random() {
    seed_ = seed_ * 1664525u + 1013904223u;
    return seed_ / 2147483648.0 - 1;
  }
  void restartPhases() { for (int i = 0; i < kMaxV; i++) { phase1_[i] = startPhase_[i]; phase2_[i] = startPhase_[i]; } }

  double sampleRate_;
  const Clock* clock_;
  bool pitchOn_, cutOn_;
  Adsr amp_, mod_;
  double phase1_[kMaxV] = {}, phase2_[kMaxV] = {}, inc1_[kMaxV] = {}, inc2_[kMaxV] = {}, panL_[kMaxV] = {}, panR_[kMaxV] = {}, startPhase_[kMaxV] = {};
  double ladder_[8] = {}, svf_[4] = {};
  double lfoPhase_[2] = {0, 0}, lfoHeld_[2] = {0, 0};
  int note_ = 60;
  double pos_ = 60, velocity_ = 1;
  std::uint32_t seed_ = 12345;
  bool latch_ = false, ampFresh_ = false;
  int from_ = -1;
  double slideRate_ = 0;
};

}  // namespace

void registerCardinal(NodeRegistry& registry) {
  NodeType type;
  type.type = "cardinal";
  type.inputs = {"pitch", "cut"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"cut", PortKind::Control}, {"out", PortKind::Stereo}};
  for (const Spec& s : kSpec) type.params.push_back({s.name, s.min, s.max, s.def, s.isInt});
  type.create = [](const NodeContext& context) { return std::make_unique<Cardinal>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

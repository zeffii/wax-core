// DVIIloader: a six-operator FM synth in the manner of the Yamaha DX7 (32 algorithms, rate/level envelopes, keyboard scaling,
// velocity sensitivity, feedback, pitch envelope, LFO). Monophonic, last note wins. Port of webtracker src/audio/nodes/dvii.js
// (a bank dropped in from the panel comes through the node's `data.bank`; the random LFO wave is not repeatable).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/dx7core.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

constexpr int kOps = 6, kChunk = 16;
enum Param { kVoice = 0, kTune, kVel, kLevel, kLfo, kSpeed, kGritty, kMute, kSolo };

const int kVelocity[64] = {0, 70, 86, 97, 106, 114, 121, 126, 132, 138, 142, 148, 152, 156, 160, 163, 166, 170, 173, 174, 178, 181, 184, 186, 189, 190, 194, 196, 198, 200, 202, 205, 206, 209, 211, 214, 216, 218, 220, 222, 224, 225, 227, 229, 230, 232, 233, 235, 237, 238, 240, 241, 242, 243, 244, 246, 246, 248, 249, 250, 251, 252, 253, 254};
const int kPitchLow[20] = {-128, -116, -104, -95, -85, -76, -68, -61, -56, -52, -49, -46, -43, -41, -39, -37, -35, -33, -32, -31};
const int kPmsTab[8] = {0, 10, 20, 33, 55, 92, 153, 255};
const double kAmsTab[4] = {0, 0.259, 0.427, 1};

double pitchOct(int pl) {
  const int v = pl < 20 ? kPitchLow[pl] : pl <= 50 ? pl - 50 : -(100 - pl < 20 ? kPitchLow[100 - pl] : 100 - pl - 50);
  return v / 32.0;
}

int velocityScaling(int vel, int sens) {
  const int value = kVelocity[std::min(127, std::max(0, vel)) >> 1] - 239;
  return ((sens * value + 7) >> 3) << 4;
}

class Dvii final : public Node {
public:
  explicit Dvii(const NodeContext& context) : sampleRate_(context.sampleRate), pitchOn_(!context.connected.empty() && context.connected[0]), rateScale_(44100 / context.sampleRate) {
    std::vector<int> bank;
    if (context.data && context.data->is_object() && context.data->contains("bank") && (*context.data)["bank"].is_array()) {
      for (const auto& b : (*context.data)["bank"]) bank.push_back(b.is_number() ? static_cast<int>(b.get<double>()) : 0);
    }
    voices_ = dx7::unpackBank(bank);
    v_ = &voices_[0];
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      note_ = event.note;
      velocity_ = std::min(127, std::max(1, jsRoundInt(event.vel * 127)));
      if (cur_ < 0) cur_ = 0;
      prep();
      if (idle_) { std::fill(egL_, egL_ + kOps, 0.0); pL_ = 0; }
      if (v_->oks) std::fill(phase_, phase_ + kOps, 0.0);
      std::fill(egStage_, egStage_ + kOps, 0); pStage_ = 0; hist0_ = hist1_ = 0;
      if (v_->lks || idle_) { lfoPhase_ = 0.25; lfoAge_ = 0; }
      sh_ = rnd() * 2 - 1;
      held_ = true; idle_ = false;
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) {
      if (held_) { held_ = false; std::fill(egStage_, egStage_ + kOps, 4); pStage_ = 4; }
    } else {
      held_ = false; idle_ = true; std::fill(egL_, egL_ + kOps, 0.0); std::fill(out_, out_ + kOps, 0.0);
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (idle_) {
      std::fill(outL, outL + n, 0.0f);
      std::fill(outR, outR + n, 0.0f);
      return;
    }
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double sr = sampleRate_;
    const int want = std::min(dx7::kVoices - 1, std::max(0, jsRoundInt(g(kVoice))));
    velParam_ = g(kVel);
    if (want != cur_) { cur_ = want; prep(); }
    const dx7::Voice& v = *v_;
    const dx7::Algorithm& alg = dx7::algorithms()[static_cast<std::size_t>(v.alg)];
    const double tune = js::pow(2, g(kTune) / 12), speed = g(kSpeed), lfoScale = g(kLfo);
    const bool gritty = g(kGritty) >= 0.5;
    const double pitchIn = pitchOn_ ? js::pow(2, static_cast<double>(inputs[0][0])) : 1;
    const double mix = (g(kLevel) * 0.7) / std::sqrt(static_cast<double>(alg.carriers.size()));
    const int muteBits = jsRoundInt(g(kMute)), soloBits = jsRoundInt(g(kSolo));
    bool isCarrier[kOps] = {};
    for (int c : alg.carriers) isCarrier[c] = true;
    const double fbScale = v.fb ? js::pow(2, v.fb - 8) : 0;
    const int fbFrom = alg.fbFrom, fbTo = alg.fbTo;
    const double lfoHz = 0.062 * js::pow(50 / 0.062, v.lfs / 99.0);
    const double lfoDelay = js::pow(v.lfd / 99.0, 2) * 5;
    const double pitchDepth = (v.lpmd / 99.0) * (kPmsTab[v.pms] / 255.0) * lfoScale;
    const double ampDepth = (v.lamd / 99.0) * lfoScale * 64;
    int wi = 0;
    for (int c0 = 0; c0 < n; c0 += kChunk) {
      const int m = std::min(kChunk, n - c0);
      bool silent = !held_;
      for (int k = 0; k < kOps; k++) {
        const bool attackCurve = egStage_[k] < 3;
        advance(egL_[k], egStage_[k], &egTarget_[k * 4], &egRate_[k * 4], m, speed, attackCurve);
      }
      advance(pL_, pStage_, pTarget_, pRate_, m, speed, false);
      lfoAge_ += static_cast<double>(m) / sr;
      lfoPhase_ += (lfoHz * m) / sr;
      if (lfoPhase_ >= 1) { lfoPhase_ -= 1; sh_ = rnd() * 2 - 1; }
      const double lp = lfoPhase_;
      const double wave = v.lfw == 0 ? (lp < 0.5 ? 4 * lp - 1 : 3 - 4 * lp) : v.lfw == 1 ? 1 - 2 * lp : v.lfw == 2 ? 2 * lp - 1 : v.lfw == 3 ? (lp < 0.5 ? 1 : -1) : v.lfw == 4 ? js::sin(2 * kPi * lp) : sh_;
      const double fade = lfoDelay > 0 ? std::min(1.0, std::max(0.0, (lfoAge_ - lfoDelay * 0.4) / (lfoDelay * 0.6))) : 1;
      const double pitchMul = js::pow(2, pL_ + wave * fade * pitchDepth) * tune * pitchIn;
      for (int k = 0; k < kOps; k++) {
        const double fx = fixed_[k] ? js::pow(2, wave * fade * pitchDepth) : 1;
        inc_[k] = (baseHz_[k] * (fixed_[k] ? 1 : pitchMul) * fx) / sr;
        if (inc_[k] >= 0.5) inc_[k] = 0;
        const double am = ampDepth * kAmsTab[v.ops[k].ams] * ((wave * fade + 1) * 0.5);
        const double steps = std::min(127.0, std::max(0.0, egL_[k] + olBase_[k] - 127 - am));
        const bool audible = !((muteBits >> k) & 1) && (soloBits == 0 || ((soloBits >> k) & 1));
        gainNext_[k] = steps < 1 || !audible ? 0 : js::pow(2, (steps - 127) / 8);
        if (isCarrier[k] && (held_ || steps >= 2)) silent = false;
      }
      for (int i = 0; i < m; i++) {
        const double t = static_cast<double>(i + 1) / m;
        double sum = 0;
        for (int k = kOps - 1; k >= 0; k--) {
          double mod = 0;
          for (const auto& e : alg.mods) if (e[0] == k) mod += out_[e[1]];
          if (k == fbTo) mod += fbScale * 0.5 * (hist0_ + hist1_);
          double p = phase_[k] + mod;
          p -= std::floor(p);
          double o = js::sin(6.283185307179586 * p) * (gain_[k] + (gainNext_[k] - gain_[k]) * t);
          if (gritty) o = jsRound(o * 1024) / 1024;
          out_[k] = o;
          if (k == fbFrom) { hist1_ = hist0_; hist0_ = o; }
          if (isCarrier[k]) sum += o;
          double ph = phase_[k] + inc_[k];
          if (ph >= 1) ph -= 1;
          phase_[k] = ph;
        }
        outL[wi] = outR[wi] = static_cast<float>(sum * mix);
        wi++;
      }
      for (int k = 0; k < kOps; k++) gain_[k] = gainNext_[k];
      if (silent && !held_) {
        idle_ = true;
        std::fill(out_, out_ + kOps, 0.0);
        hist0_ = hist1_ = 0;
        std::fill(outL + wi, outL + n, 0.0f);
        std::fill(outR + wi, outR + n, 0.0f);
        return;
      }
    }
  }

private:
  double rnd() { seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5; return seed_ / 4294967296.0; }

  void prep() {
    v_ = &voices_[static_cast<std::size_t>(cur_)];
    const dx7::Voice& v = *v_;
    const int n = note_ + v.trnsp - 24;
    const double hz = 440 * js::pow(2, (n - 69) / 12.0);
    for (int k = 0; k < kOps; k++) {
      const dx7::Op& p = v.ops[k];
      fixed_[k] = static_cast<std::uint8_t>(p.mode);
      const double det = js::pow(2, ((p.det - 7) * 2) / 1200.0);
      baseHz_[k] = p.mode ? js::pow(10, (p.fc & 3) + p.ff / 100.0) * det : hz * (p.fc == 0 ? 0.5 : p.fc) * (1 + p.ff / 100.0) * det;
      double ol = std::min(127, std::max(0, dx7::scaleOL(p.ol) + dx7::levelScaling(n, p)));
      ol = std::max(0.0, ol + (velocityScaling(velocity_, p.kvs) / 32.0) * velParam_);
      olBase_[k] = ol;
      const int x = std::min(31, std::max(0, (n / 3) - 7)), delta = (p.rs * x) >> 3;
      for (int s = 0; s < 4; s++) {
        egTarget_[k * 4 + s] = dx7::scaleOL(p.l[s]);
        const int q = std::min(63, ((p.r[s] * 41) >> 6) + delta);
        egRate_[k * 4 + s] = ((4 + (q & 3)) * js::pow(2, q >> 2) / 524288) * rateScale_;
      }
    }
    for (int s = 0; s < 4; s++) {
      pTarget_[s] = pitchOct(v.pl[s]);
      pRate_[s] = 0.04 * js::pow(2, v.pr[s] / 9.0) / sampleRate_;
    }
  }

  // advances one envelope by m samples: stages 0..2 move to L1..L3, 3 holds, 4 releases to L4
  static void advance(double& L, std::uint8_t& stageRef, const double* target, const double* rate, int m, double speed, bool attackCurve) {
    double l = L;
    int stage = stageRef;
    for (int guard = 0; guard < 4 && stage < 5 && m > 0; guard++) {
      const int idx = stage == 4 ? 3 : std::min(stage, 2);
      if (stage == 3) break;
      const double t = target[idx], r = rate[idx] * speed;
      if (l < t) {
        if (attackCurve && l < 53.6) l = std::min(53.6, t);
        l += r * (attackCurve ? std::max(0.05, (136 - l) / 8) : 1) * m;
        if (l >= t) { l = t; if (stage == 4) { stage = 5; break; } stage++; continue; }
      } else if (l > t) {
        l -= r * m;
        if (l <= t) { l = t; if (stage == 4) { stage = 5; break; } stage++; continue; }
      } else { if (stage == 4) { stage = 5; break; } stage++; continue; }
      break;
    }
    L = l;
    stageRef = static_cast<std::uint8_t>(stage);
  }

  double sampleRate_;
  bool pitchOn_;
  double rateScale_;
  std::vector<dx7::Voice> voices_;
  const dx7::Voice* v_ = nullptr;
  int cur_ = -1;
  double phase_[kOps] = {}, inc_[kOps] = {}, out_[kOps] = {}, gain_[kOps] = {}, gainNext_[kOps] = {}, baseHz_[kOps] = {};
  std::uint8_t fixed_[kOps] = {};
  double egL_[kOps] = {};
  std::uint8_t egStage_[kOps] = {};
  double egRate_[kOps * 4] = {}, egTarget_[kOps * 4] = {}, olBase_[kOps] = {};
  double pL_ = 0;
  std::uint8_t pStage_ = 4;
  double pTarget_[4] = {}, pRate_[4] = {};
  int note_ = 60, velocity_ = 100;
  bool held_ = false, idle_ = true;
  double lfoPhase_ = 0, lfoAge_ = 0, hist0_ = 0, hist1_ = 0, sh_ = 0, velParam_ = 1;
  std::uint32_t seed_ = 0x2545f491u;
};

}  // namespace

void registerDvii(NodeRegistry& registry) {
  NodeType type;
  type.type = "dvii";
  type.inputs = {"pitch"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"voice", 0, 31, 0, true}, {"tune", -48, 48, 0}, {"vel", 0, 1, 1}, {"level", 0, 1, 0.5}, {"lfo", 0, 2, 1},
    {"speed", 0.25, 4, 1}, {"gritty", 0, 1, 0, true}, {"mute", 0, 63, 0, true}, {"solo", 0, 63, 0, true},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Dvii>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

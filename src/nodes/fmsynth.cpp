// Four-operator FM synth. Each operator is a phase-modulation oscillator reading a single-cycle waveform (the node's
// `data.waves`; a sine until it carries its own) with its own ratio, level and ADSR envelope. The algorithm says who
// modulates whom; the operators that feed the output are the carriers. Operator 4 can modulate itself (feedback).
// Modulation is in cycles of the receiving operator: `index` scales every modulator. Monophonic, last note wins, with
// glide. Port of webtracker src/audio/nodes/fmsynth.js.
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "core/js_math.h"
#include "nodes/adsr.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"
#include "nodes/waves.h"

namespace wax {
namespace {

constexpr int kOperators = 4;

// Each algorithm: mods are {receiver, sender} pairs (the sender has the higher number, so operators are worked out
// 4, 3, 2, 1), carriers are the operators you hear. Numbers are 0 based here.
struct Algorithm {
  std::vector<std::array<int, 2>> mods;
  std::vector<int> carriers;
};

const std::vector<Algorithm>& algorithms() {
  static const std::vector<Algorithm> list = {
    {{{2, 3}, {1, 2}, {0, 1}}, {0}},          // 4>3>2>1
    {{{2, 3}, {0, 2}, {0, 1}}, {0}},          // (4>3)+2>1
    {{{0, 1}, {0, 2}, {0, 3}}, {0}},          // 4+3+2>1
    {{{0, 1}, {2, 3}}, {0, 2}},               // 2>1  4>3
    {{{0, 3}, {1, 3}, {2, 3}}, {0, 1, 2}},    // 4>1,2,3
    {{{1, 2}, {0, 1}}, {0, 3}},               // 3>2>1  4
    {{{0, 1}}, {0, 2, 3}},                    // 2>1  3  4
    {{}, {0, 1, 2, 3}},                       // all carriers
  };
  return list;
}

// slots of the common parameters, then seven per operator
enum Param { kAlgo = 0, kIndex, kFb, kTune, kGlide, kVel, kLevel, kOpBase };
constexpr int kPerOp = 7;   // ratio, fine, level, atk, dec, sus, rel
enum OpParam { kRatio = 0, kFine, kOpLevel, kAtk, kDec, kSus, kRel };

class FmSynth final : public Node {
public:
  explicit FmSynth(const NodeContext& context)
      : sampleRate_(context.sampleRate), pitchOn_(!context.connected.empty() && context.connected[0]),
        env_{Adsr(context.sampleRate), Adsr(context.sampleRate), Adsr(context.sampleRate), Adsr(context.sampleRate)} {
    const Json* waves = context.data && context.data->contains("waves") && (*context.data)["waves"].is_array() ? &(*context.data)["waves"] : nullptr;
    for (int i = 0; i < kOperators; i++) {
      const Json* one = waves && static_cast<std::size_t>(i) < waves->size() ? &(*waves)[static_cast<std::size_t>(i)] : nullptr;
      tables_[static_cast<std::size_t>(i)] = normalizeWave(one);
    }
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      if (idle()) { for (double& p : phase_) p = 0; fbPrev_ = fbPrev2_ = 0; pos_ = event.note; }
      note_ = event.note; velocity_ = event.vel;
      for (Adsr& e : env_) e.on();
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) {
      for (Adsr& e : env_) e.off();
    } else if (event.type == NoteType::Cut) {
      for (Adsr& e : env_) e.cut();
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (idle()) { std::fill(outL, outL + n, 0.0f); std::fill(outR, outR + n, 0.0f); return; }
    auto g = [&](int i) -> double { return params[slots[i]]; };
    const auto& list = algorithms();
    const Algorithm& alg = list[static_cast<std::size_t>(std::min(static_cast<int>(list.size()) - 1, std::max(0, jsRoundInt(g(kAlgo)))))];
    const double glideMs = g(kGlide);
    if (glideMs > 0.5) pos_ += (note_ - pos_) * (1 - js::exp(-n / (sampleRate_ * glideMs * 0.001 / 3)));
    else pos_ = note_;
    const double f0 = 440 * js::pow(2, (pos_ + g(kTune) - 69) / 12) * (pitchOn_ ? js::pow(2, inputs[0][0]) : 1);
    const double index = g(kIndex), fb = g(kFb) * 0.5;
    const double vm = 1 - g(kVel) + g(kVel) * velocity_;
    std::fill(std::begin(carrier_), std::end(carrier_), 0.0);
    std::fill(std::begin(modm_), std::end(modm_), 0.0);
    for (int c : alg.carriers) carrier_[c] = 1;
    for (const auto& m : alg.mods) modm_[m[0] * kOperators + m[1]] = 1;
    for (int k = 0; k < kOperators; k++) {
      const int base = kOpBase + k * kPerOp;
      env_[static_cast<std::size_t>(k)].set(g(base + kAtk), g(base + kDec), g(base + kSus), g(base + kRel));
      inc_[k] = (f0 * g(base + kRatio) * js::pow(2, g(base + kFine) / 1200)) / sampleRate_;
      lvl_[k] = g(base + kOpLevel) * vm;
      if (inc_[k] >= 0.5) inc_[k] = 0;   // above Nyquist: silent instead of aliasing noise
    }
    const double mix = (g(kLevel) * 0.5) / std::sqrt(static_cast<double>(alg.carriers.size()));
    const int last = kOperators - 1;
    for (int i = 0; i < n; i++) {
      double sum = 0;
      for (int k = last; k >= 0; k--) {
        double m = 0;
        for (int s = k + 1; s < kOperators; s++) m += modm_[k * kOperators + s] * out_[s];
        double p = phase_[k] + m * index + (k == last ? fb * (fbPrev_ + fbPrev2_) : 0);
        p -= std::floor(p);
        const double x = p * kWavePoints;
        const int j = static_cast<int>(x);
        const double f = x - j;
        const WaveTable& t = tables_[static_cast<std::size_t>(k)];
        const double w = static_cast<double>(t[static_cast<std::size_t>(j)]) +
                         (static_cast<double>(t[static_cast<std::size_t>((j + 1) & (kWavePoints - 1))]) - t[static_cast<std::size_t>(j)]) * f;
        const double o = w * env_[static_cast<std::size_t>(k)].next() * lvl_[k];
        out_[k] = o;
        if (k == last) { fbPrev2_ = fbPrev_; fbPrev_ = o; }
        sum += o * carrier_[k];
        double ph = phase_[k] + inc_[k];
        if (ph >= 1) ph -= 1;
        phase_[k] = ph;
      }
      outL[i] = outR[i] = static_cast<float>(sum * mix);
    }
  }

private:
  bool idle() const {
    for (const Adsr& e : env_) if (!e.idle()) return false;
    return true;
  }

  double sampleRate_;
  bool pitchOn_;
  std::array<WaveTable, kOperators> tables_;
  Adsr env_[kOperators];
  double phase_[kOperators] = {}, inc_[kOperators] = {}, lvl_[kOperators] = {};
  double out_[kOperators] = {}, carrier_[kOperators] = {}, modm_[kOperators * kOperators] = {};
  int note_ = 60;
  double pos_ = 60, velocity_ = 1, fbPrev_ = 0, fbPrev2_ = 0;
};

}  // namespace

void registerFmSynth(NodeRegistry& registry) {
  NodeType type;
  type.type = "fmsynth";
  type.inputs = {"pitch"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {
    {"algo", 0, 7, 0, true},
    {"index", 0, 8, 2},         // modulation depth, in cycles
    {"fb", 0, 1, 0},            // operator 4 modulating itself
    {"tune", -48, 48, 0},
    {"glide", 0, 2000, 0},
    {"vel", 0, 1, 0.5},
    {"level", 0, 1, 0.5},
  };
  const double ratios[4] = {1, 1, 2, 1}, levels[4] = {1, 0.7, 0.5, 0.4};
  for (int k = 1; k <= kOperators; k++) {
    const std::string s = std::to_string(k);
    type.params.push_back({"ratio" + s, 0.25, 16, ratios[k - 1]});
    type.params.push_back({"fine" + s, -100, 100, 0});      // cents
    type.params.push_back({"level" + s, 0, 1, levels[k - 1]});
    type.params.push_back({"atk" + s, 0.1, 5000, 2});       // ms
    type.params.push_back({"dec" + s, 1, 5000, 300});
    type.params.push_back({"sus" + s, 0, 1, k == 1 ? 0.8 : 0.4});
    type.params.push_back({"rel" + s, 1, 5000, 200});
  }
  type.create = [](const NodeContext& context) { return std::make_unique<FmSynth>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

// Hi-hat 2: closed (note 0) and open (note 1) hits in one voice. Six inharmonic square waves plus noise, a band-pass and
// a high-pass, a drawn volume shape per hat, a choke, then a flanger and a bit crusher. One hat for the whole instrument.
// Port of webtracker src/audio/nodes/hat2.js.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/bezier.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kTune = 0, kSpread, kNoise, kBody, kTone, kHuman, kLevel, kCrate, kOrate, kClvl, kOlvl, kChoke, kFlange, kFrate,
             kFdepth, kFfeed, kCrush, kBits, kDown };

constexpr double kBank[6] = {205.3, 304.4, 369.6, 522.7, 540, 800};
constexpr double kOffset[6] = {-0.07, 0.11, -0.04, 0.09, -0.12, 0.06};
constexpr double kChokeMs = 8;
constexpr double kFlangeBaseMs = 1.2, kFlangeSweepMs = 4;
constexpr double kReferenceMs[2] = {140, 700};   // closed, open

struct Hit { double age = -1, vel = 0; };

class Hat2 final : public Node {
public:
  explicit Hat2(const NodeContext& context) : sampleRate_(context.sampleRate) {
    loadDrawings(context.data);
    const std::size_t length = static_cast<std::size_t>(std::ceil(sampleRate_ * 0.02));
    line_[0].assign(length, 0.0f);
    line_[1].assign(length, 0.0f);
  }

  void data(const Json& data) override { loadDrawings(&data); }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      const double v = std::max(0.0, std::min(1.0, event.vel));
      if (event.note == 1) pendOpen_ = v; else pendClosed_ = v;
    } else if (event.type == NoteType::Cut || event.type == NoteType::AllOff) {
      hat_[0].age = hat_[1].age = -1; chokeLeft_ = 0; pendClosed_ = pendOpen_ = 0;
    }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    const auto get = [&](int p) -> double { return params[slots[p]]; };
    const double human = get(kHuman);
    const bool choke = get(kChoke) >= 0.5;
    const double level = get(kLevel) * 0.45, mixNoise = get(kNoise), spread = get(kSpread);
    const double clvl = get(kClvl), olvl = get(kOlvl);
    const double lens[2] = {kReferenceMs[0] * 0.001 * sampleRate_ / get(kCrate), kReferenceMs[1] * 0.001 * sampleRate_ / get(kOrate)};
    const double levels[2] = {clvl, olvl};
    const double flange = get(kFlange), fstep = get(kFrate) / sampleRate_, fdepth = get(kFdepth), ffeed = get(kFfeed);
    const double crush = get(kCrush);
    const int bits = jsRoundInt(get(kBits));
    const int down = std::max(1, jsRoundInt(get(kDown)));
    const double quant = js::pow(2, bits - 1);

    // new hits: the open hat starts first, so a closed hit on the same row cuts it
    if (pendOpen_ > 0) { vary(human); hat_[1].age = 0; hat_[1].vel = pendOpen_; chokeLeft_ = 0; pendOpen_ = 0; }
    if (pendClosed_ > 0) {
      if (pendOpen_ == 0 && hat_[1].age < 0) vary(human);
      hat_[0].age = 0; hat_[0].vel = pendClosed_; pendClosed_ = 0;
      if (choke && hat_[1].age >= 0) chokeLeft_ = kChokeMs * 0.001 * sampleRate_;
    }

    const double centre = std::min(sampleRate_ * 0.45, get(kBody) * toneJitter_);
    const double g = js::tan(kPi * centre / sampleRate_), kq = 1 / 0.8;
    const double a1 = 1 / (1 + g * (g + kq)), a2 = g * a1, a3 = g * a2;
    const double rh = js::exp(-2 * kPi * std::min(get(kTone) * toneJitter_, sampleRate_ * 0.45) / sampleRate_);
    const double ratio = js::pow(2, get(kTune) / 12) * pitchJitter_;
    double dts[6];
    for (int k = 0; k < 6; k++) dts[k] = std::min(0.45, kBank[k] * ratio * (1 + spread * kOffset[k]) / sampleRate_);
    const int lineLen = static_cast<int>(line_[0].size());

    for (int i = 0; i < n; i++) {
      // the two drawings
      double amp = 0;
      for (int kind = 0; kind < 2; kind++) {
        Hit& h = hat_[kind];
        if (h.age < 0) continue;
        const double len = lens[kind];
        const double pos = h.age / len;
        double env = bezier::readEnvelope(tables_[kind], std::min(1.0, pos));
        if (kind == 1 && chokeLeft_ > 0) {
          env *= chokeLeft_ / (kChokeMs * 0.001 * sampleRate_);
          chokeLeft_--;
          if (chokeLeft_ <= 0) { h.age = -1; continue; }
        }
        amp += env * h.vel * levels[kind];
        h.age++;
        if (pos >= 1 && std::fabs(env) < 1e-4) h.age = -1;
      }
      double metal = 0;
      for (int k = 0; k < 6; k++) {
        const double dt = dts[k];
        double p = phase_[k] + dt;
        if (p >= 1) p -= 1;
        phase_[k] = p;
        metal += (p < 0.5 ? 1 : -1) + polyBlep(p, dt) - polyBlep(std::fmod(p + 0.5, 1), dt);
      }
      noiseState_ ^= noiseState_ << 13; noiseState_ ^= noiseState_ >> 17; noiseState_ ^= noiseState_ << 5;
      const double white = static_cast<double>(noiseState_) / 2147483648.0 - 1;
      const double x = (metal * 0.25 * (1 - mixNoise) + white * 1.2 * mixNoise) * amp * levelJitter_;
      const double v3 = x - ic2_, v1 = a1 * ic1_ + a2 * v3, v2 = ic2_ + a2 * ic1_ + a3 * v3;
      ic1_ = 2 * v1 - ic1_; ic2_ = 2 * v2 - ic2_;
      const double y1 = v1 - hp1x_ + rh * hp1_; hp1x_ = v1; hp1_ = y1;
      const double y2 = y1 - hp2x_ + rh * hp2_; hp2x_ = y1; hp2_ = y2;
      const double dry = y2 * level;

      // flanger: a short delay on each side, swept by a wave a quarter turn apart
      double sL = dry, sR = dry;
      if (flange > 0) {
        lfo_ += fstep;
        if (lfo_ >= 1) lfo_ -= 1;
        double outs[2] = {0, 0};
        for (int c = 0; c < 2; c++) {
          const double w = 0.5 + 0.5 * js::sin(2 * kPi * (lfo_ + c * 0.25));
          const double delay = (kFlangeBaseMs + kFlangeSweepMs * fdepth * w) * 0.001 * sampleRate_;
          double rp = write_ - delay;
          if (rp < 0) rp += lineLen;
          const int ia = static_cast<int>(std::floor(rp));
          const double fr = rp - ia;
          const int ib = ia + 1 >= lineLen ? 0 : ia + 1;
          outs[c] = line_[c][static_cast<std::size_t>(ia)] * (1 - fr) + line_[c][static_cast<std::size_t>(ib)] * fr;
          line_[c][static_cast<std::size_t>(write_)] = static_cast<float>(dry + outs[c] * ffeed);
        }
        write_ = write_ + 1 >= lineLen ? 0 : write_ + 1;
        sL = dry + outs[0] * flange; sR = dry + outs[1] * flange;
      }
      // bit crusher: hold every `down` samples, round to the depth
      if (crush > 0) {
        if (holdCount_ <= 0) { heldL_ = jsRound(sL * quant) / quant; heldR_ = jsRound(sR * quant) / quant; holdCount_ = down; }
        holdCount_--;
        sL += (heldL_ - sL) * crush; sR += (heldR_ - sR) * crush;
      }
      outL[i] = static_cast<float>(sL);
      outR[i] = static_cast<float>(sR);
    }
    if (!std::isfinite(ic1_ + ic2_ + hp1_ + hp2_ + heldL_ + heldR_)) {
      ic1_ = ic2_ = hp1_ = hp2_ = hp1x_ = hp2x_ = heldL_ = heldR_ = 0;
      std::fill(line_[0].begin(), line_[0].end(), 0.0f);
      std::fill(line_[1].begin(), line_[1].end(), 0.0f);
    }
  }

private:
  void vary(double human) {
    pitchJitter_ = 1 + (rand() * 2 - 1) * 0.04 * human;
    toneJitter_ = 1 + (rand() * 2 - 1) * 0.25 * human;
    levelJitter_ = 1 - rand() * 0.35 * human;
    if (human > 0) for (int k = 0; k < 6; k++) phase_[k] = rand();
  }
  double rand() {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return static_cast<double>(rng_) / 4294967296.0;
  }

  double sampleRate_;
  void loadDrawings(const Json* data) {
    static const double closedR[] = {0.02, -0.3}, closedL[] = {-0.45, 0};
    static const double openR[] = {0.04, -0.15}, openL[] = {-0.5, 0};
    const bezier::Spline defaults[2] = {
      {bezier::makeAnchor(0, 1, nullptr, closedR), bezier::makeAnchor(1, 0, closedL, nullptr)},
      {bezier::makeAnchor(0, 1, nullptr, openR), bezier::makeAnchor(1, 0, openL, nullptr)}};
    static const char* const keys[2] = {"closed", "open"};
    for (int k = 0; k < 2; k++) tables_[k] = bezier::envelopeTable(bezier::drawingOf(data, keys[k], defaults[k]));
  }

  bezier::Table tables_[2];
  double phase_[6] = {0, 0, 0, 0, 0, 0};
  std::uint32_t rng_ = 0x2545f491u, noiseState_ = 0x1234567u;
  Hit hat_[2];
  double chokeLeft_ = 0, pendClosed_ = 0, pendOpen_ = 0;
  double pitchJitter_ = 1, toneJitter_ = 1, levelJitter_ = 1;
  double ic1_ = 0, ic2_ = 0, hp1_ = 0, hp1x_ = 0, hp2_ = 0, hp2x_ = 0;
  std::vector<float> line_[2];
  int write_ = 0;
  double lfo_ = 0, heldL_ = 0, heldR_ = 0;
  int holdCount_ = 0;
};

}  // namespace

void registerHat2(NodeRegistry& registry) {
  NodeType type;
  type.type = "hat2";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.shared = true;   // one hat for the instrument, never copied for each voice
  type.params = {
    {"tune", -12, 12, 0},          // semitones, moves the whole metal bank
    {"spread", 0, 1, 0.35},
    {"noise", 0, 1, 0.45},
    {"body", 2000, 14000, 9000},   // Hz, centre of the band-pass
    {"tone", 2000, 12000, 6000},   // Hz, high-pass
    {"human", 0, 1, 0.4},
    {"level", 0, 1, 0.6},
    {"crate", 0.1, 10, 1},
    {"orate", 0.1, 10, 1},
    {"clvl", 0, 1, 1},
    {"olvl", 0, 1, 1},
    {"choke", 0, 1, 1, true},
    {"flange", 0, 1, 0},
    {"frate", 0.05, 6, 0.35},
    {"fdepth", 0, 1, 0.6},
    {"ffeed", -0.9, 0.9, 0.5},
    {"crush", 0, 1, 0},
    {"bits", 2, 16, 8, true},
    {"down", 1, 32, 4, true},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Hat2>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

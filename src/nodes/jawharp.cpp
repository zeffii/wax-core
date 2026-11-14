// Jaw harp: a plucked steel tongue whose swings let narrow puffs of air into a mouth. The mouth is three resonators at the formants
// of a vowel (u o a e i from 0 to 1), swept by `vowel`, `mouth` and `track`. Monophonic; a new note plucks again. Port of webtracker
// src/audio/nodes/jawharp.js.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kVowel = 0, kMouth, kTrack, kFocus, kDrive, kTwang, kBreath, kBody, kDecay, kTune, kVel, kLevel, kRelease };
constexpr double kVowels[5][3] = {{300, 870, 2240}, {400, 800, 2600}, {700, 1200, 2600}, {500, 1850, 2600}, {300, 2300, 3000}};
constexpr double kWeights[3] = {1, 0.7, 0.35};

class JawHarp final : public Node {
public:
  explicit JawHarp(const NodeContext& context)
      : sampleRate_(context.sampleRate), pitchOn_(!context.connected.empty() && context.connected[0]), dcR_(1 - (2 * kPi * 30) / context.sampleRate) {}

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) { note_ = event.note; velocity_ = event.vel; pending_ = true; held_ = true; active_ = true; }
    else if (event.type == NoteType::Off || event.type == NoteType::AllOff) held_ = false;
    else { active_ = false; held_ = false; amp_ = 0; clearResonators(); }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    if (!active_) {
      std::fill(outputs[0], outputs[0] + n, 0.0f);
      std::fill(outputs[1], outputs[1] + n, 0.0f);
      return;
    }
    auto g = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double sr = sampleRate_;
    aim(g(kVowel), g(kMouth), g(kTrack));
    if (pending_) {
      pending_ = false;
      phase_ = 0; amp_ = 1; age_ = 0; slap_ = 1; hp_ = 0; hpIn_ = 0;
      gain_ = 1 - g(kVel) + g(kVel) * velocity_;
      for (int k = 0; k < 3; k++) fs_[k] = target_[k];
      clearResonators();
    } else for (int k = 0; k < 3; k++) fs_[k] += (target_[k] - fs_[k]) * 0.35;
    const double base = 440 * js::pow(2, (note_ + g(kTune) - 69) / 12 + (pitchOn_ ? static_cast<double>(inputs[0][0]) : 0.0));
    const double twang = g(kTwang), power = 1 + 7 * g(kDrive), breath = g(kBreath) * 0.3, body = g(kBody);
    const double k = 1 / g(kFocus), perSample = js::pow(10, -3 / (g(kDecay) * sr)), relSample = js::pow(10, -3 / (g(kRelease) * 0.001 * sr));
    double a1[3], a2[3], a3[3];
    for (int r = 0; r < 3; r++) {
      const double x = js::tan((kPi * fs_[r]) / sr);
      a1[r] = 1 / (1 + x * (x + k));
      a2[r] = x * a1[r];
      a3[r] = x * a2[r];
    }
    const double out = g(kLevel) * gain_ * 6;
    for (int i = 0; i < n; i++) {
      const double dip = 1 + twang * 0.05 * js::exp(-age_ / (0.05 * sr));
      phase_ += (base * dip) / sr;
      phase_ -= std::floor(phase_);
      amp_ *= held_ ? perSample : perSample * relSample;
      age_++;
      const double swing = js::sin(2 * kPi * phase_), puff = js::pow(std::max(0.0, swing), power);
      slap_ *= 0.995;
      const double noise = rand();
      const double air = amp_ * (puff + noise * (breath + twang * slap_ * 0.8));
      hp_ = air - hpIn_ + dcR_ * hp_;
      hpIn_ = air;
      double y = 0;
      for (int r = 0; r < 3; r++) {
        const double v3 = hp_ - s2_[r], v1 = a1[r] * s1_[r] + a2[r] * v3, v2 = s2_[r] + a2[r] * s1_[r] + a3[r] * v3;
        s1_[r] = 2 * v1 - s1_[r];
        s2_[r] = 2 * v2 - s2_[r];
        y += kWeights[r] * k * v1;
      }
      y = (y + body * hp_) * out;
      outputs[0][i] = outputs[1][i] = static_cast<float>(y);
    }
    if (amp_ < 1e-5) { active_ = false; amp_ = 0; clearResonators(); }
  }

private:
  void clearResonators() { for (int r = 0; r < 3; r++) s1_[r] = s2_[r] = 0; }
  double rand() {
    seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
    return seed_ / 4294967296.0 * 2 - 1;
  }
  void aim(double vowel, double mouth, double track) {
    const double x = std::min(0.9999, std::max(0.0, vowel)) * 4, f = x - std::floor(x);
    const int i = static_cast<int>(std::floor(x));
    for (int k = 0; k < 3; k++) target_[k] = kVowels[i][k] + (kVowels[i + 1][k] - kVowels[i][k]) * f;
    const double shift = js::pow(2, (mouth + track * (note_ - 60)) / 12);
    for (int k = 0; k < 3; k++) target_[k] = std::min(sampleRate_ * 0.45, target_[k] * shift);
  }

  double sampleRate_;
  bool pitchOn_;
  double dcR_;
  int note_ = 60;
  double velocity_ = 1;
  bool held_ = false, active_ = false, pending_ = false;
  double phase_ = 0, amp_ = 0, age_ = 0, gain_ = 1, hp_ = 0, hpIn_ = 0, slap_ = 0;
  double fs_[3] = {0, 0, 0}, target_[3] = {0, 0, 0}, s1_[3] = {0, 0, 0}, s2_[3] = {0, 0, 0};
  std::uint32_t seed_ = 0x9e3779b9u;
};

}  // namespace

void registerJawHarp(NodeRegistry& registry) {
  NodeType type;
  type.type = "jawharp";
  type.inputs = {"pitch"};
  type.outputs = {"out"};
  type.kinds = {{"pitch", PortKind::Control}, {"out", PortKind::Stereo}};
  type.params = {{"vowel", 0, 1, 0.4}, {"mouth", -12, 12, 0}, {"track", 0, 1, 0}, {"focus", 2, 40, 12}, {"drive", 0, 1, 0.5}, {"twang", 0, 1, 0.4},
                 {"breath", 0, 1, 0.1}, {"body", 0, 1, 0.15}, {"decay", 0.1, 8, 1.5}, {"tune", -48, 48, 0}, {"vel", 0, 1, 0.5}, {"level", 0, 1, 0.5}, {"release", 5, 3000, 120}};
  type.create = [](const NodeContext& context) { return std::make_unique<JawHarp>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

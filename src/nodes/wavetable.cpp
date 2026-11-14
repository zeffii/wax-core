// Wavetable synth. `source` 0: a stack of single-cycle frames (the bundled set until the node carries its own table in
// `data.table`), `position` moves through it, blending the two frames it falls between (`snap` jumps instead).
// `source` 1: two waves of 512 points (`data.pair`, a sine and a saw by default); `mix` blends A to B. A second envelope
// times `pamt` moves the position (or the mix) over a note, and an LFO wobbles it. Amplitude has its own ADSR.
// Monophonic, last note wins, with glide. Port of webtracker src/audio/nodes/wavetable.js and wtutil.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/js_math.h"
#include "nodes/adsr.h"
#include "nodes/all_nodes.h"
#include "nodes/wave_table_data.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param {
  kSource = 0, kPosition, kMix, kSnap, kTune, kFine, kGlide, kVel, kLevel, kAttack, kDecay, kSustain, kRelease,
  kPamt, kPatk, kPdec, kPsus, kPrel, kLfoRate, kLfoDepth, kSmooth
};

constexpr int kFrame = 512;
using Frames = std::vector<float>;

// base64 of little-endian 16-bit numbers -> frames (-1 to 1)
Frames decodeFrames(const std::string& text) {
  static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<std::uint8_t> bytes;
  std::uint32_t acc = 0;
  int bits = 0;
  std::size_t clean = 0;
  for (const char ch : text) if (alphabet.find(ch) != std::string::npos) clean++;
  const std::size_t total = clean * 3 / 4;
  for (const char ch : text) {
    const std::size_t v = alphabet.find(ch);
    if (v == std::string::npos) continue;
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (bytes.size() < total) bytes.push_back(static_cast<std::uint8_t>((acc >> bits) & 255));
    }
  }
  Frames out(bytes.size() / 2);
  for (std::size_t i = 0; i < out.size(); i++) {
    const std::int16_t v = static_cast<std::int16_t>(static_cast<std::uint16_t>((bytes[2 * i + 1] << 8) | bytes[2 * i]));
    out[i] = static_cast<float>(v / 32767.0);
  }
  return out;
}

// the jump between the cycle's end and its start removed with a ramp, the mean taken out and the peak set to `peak`
void settle(float* w, double peak) {
  const double jump = static_cast<double>(w[kFrame - 1]) - w[0];
  double mean = 0;
  for (int i = 0; i < kFrame; i++) {
    w[i] = static_cast<float>(static_cast<double>(w[i]) - jump * (static_cast<double>(i) / kFrame));
    mean += w[i];
  }
  mean /= kFrame;
  double top = 1e-9;
  for (int i = 0; i < kFrame; i++) {
    w[i] = static_cast<float>(static_cast<double>(w[i]) - mean);
    top = std::max(top, static_cast<double>(std::abs(w[i])));
  }
  for (int i = 0; i < kFrame; i++) w[i] = static_cast<float>((static_cast<double>(w[i]) / top) * peak);
}

// One cycle resampled to kFrame points (cubic, going up) and settled.
void cycleCubic(const std::vector<float>& src, double peak, float* out) {
  const int n = static_cast<int>(src.size());
  auto at = [&](int i) -> double { return src[static_cast<std::size_t>(((i % n) + n) % n)]; };
  for (int i = 0; i < kFrame; i++) {
    const double x = (static_cast<double>(i) / kFrame) * n, kf = std::floor(x), f = x - kf;
    const int k = static_cast<int>(kf);
    const double a = at(k - 1), b = at(k), c = at(k + 1), d = at(k + 2);
    out[i] = static_cast<float>(b + 0.5 * f * (c - a + f * (2 * a - 5 * b + 4 * c - d + f * (3 * (b - c) + d - a))));
  }
  settle(out, peak);
}

const Frames& bundledFrames() {   // 256 frames, shared by every node
  static const Frames frames = [] {
    Frames flat(static_cast<std::size_t>(kTableWaves) * kFrame);
    for (int k = 0; k < kTableWaves; k++) {
      std::vector<float> raw(kTablePoints);
      for (int i = 0; i < kTablePoints; i++) raw[static_cast<std::size_t>(i)] = static_cast<float>((static_cast<int>(kTableBytes[k * kTablePoints + i]) - 128) / 128.0);
      cycleCubic(raw, 0.95, flat.data() + static_cast<std::size_t>(k) * kFrame);
    }
    return flat;
  }();
  return frames;
}

Frames defaultPair() {   // a sine and a saw
  Frames flat(2 * kFrame);
  for (int i = 0; i < kFrame; i++) {
    flat[static_cast<std::size_t>(i)] = static_cast<float>(0.9 * js::sin((2 * kPi * i) / kFrame));
    flat[static_cast<std::size_t>(kFrame + i)] = static_cast<float>(0.9 * (1 - (2.0 * i) / kFrame));
  }
  return flat;
}

bool loadFrames(const Json* data, const char* key, Frames& out) {
  if (!data || !data->contains(key) || !(*data)[key].is_string()) return false;
  const std::string text = (*data)[key].get<std::string>();
  if (text.empty()) return false;
  out = decodeFrames(text);
  return true;
}

class Wavetable final : public Node {
public:
  explicit Wavetable(const NodeContext& context) : sampleRate_(context.sampleRate), ampEnv_(context.sampleRate), posEnv_(context.sampleRate) {
    if (!loadFrames(context.data, "table", table_)) table_ = bundledFrames();
    if (!loadFrames(context.data, "pair", pair_)) pair_ = defaultPair();
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      if (ampEnv_.idle()) { phase_ = 0; lfo_ = 0; pos_ = event.note; shown_ = -1; }
      note_ = event.note; velocity_ = event.vel;
      ampEnv_.on(); posEnv_.on();
    } else if (event.type == NoteType::Off || event.type == NoteType::AllOff) {
      ampEnv_.off(); posEnv_.off();
    } else if (event.type == NoteType::Cut) {
      ampEnv_.cut(); posEnv_.cut();
    }
  }

  void process(const float* const*, float* const* outputs, int n, const float* params, const int* slots) override {
    float* outL = outputs[0];
    float* outR = outputs[1];
    if (ampEnv_.idle()) { std::fill(outL, outL + n, 0.0f); std::fill(outR, outR + n, 0.0f); return; }
    auto g = [&](int i) -> double { return params[slots[i]]; };
    ampEnv_.set(g(kAttack), g(kDecay), g(kSustain), g(kRelease));
    posEnv_.set(g(kPatk), g(kPdec), g(kPsus), g(kPrel));
    const double glideMs = g(kGlide);
    if (glideMs > 0.5) pos_ += (note_ - pos_) * (1 - js::exp(-n / (sampleRate_ * glideMs * 0.001 / 3)));
    else pos_ = note_;
    const double inc = (440 * js::pow(2, (pos_ + g(kTune) + g(kFine) / 100 - 69) / 12)) / sampleRate_;
    const double level = g(kLevel) * (1 - g(kVel) + g(kVel) * velocity_);
    const bool pairMode = g(kSource) >= 0.5, snap = g(kSnap) >= 0.5;
    const Frames& frames = pairMode ? pair_ : table_;
    const int count = static_cast<int>(frames.size()) / kFrame;
    const double base = pairMode ? g(kMix) : g(kPosition), amt = g(kPamt), depth = g(kLfoDepth), lfoInc = (2 * kPi * g(kLfoRate)) / sampleRate_;
    const double slew = g(kSmooth) > 0 ? 1 - js::exp(-1 / (sampleRate_ * g(kSmooth) * 0.001)) : 1;
    if (shown_ < 0 || pairMode != lastPair_) shown_ = base;
    lastPair_ = pairMode;
    for (int i = 0; i < n; i++) {
      shown_ += (base - shown_) * slew;
      // the frame position for this sample: the base value moved by the second envelope and the LFO
      double p = shown_ + amt * posEnv_.next() + depth * 0.5 * js::sin(lfo_);
      lfo_ += lfoInc;
      p = p < 0 ? 0 : p > 1 ? 1 : p;
      double fp = p * (count - 1);
      if (snap) fp = jsRound(fp);
      const int k = static_cast<int>(std::min(static_cast<double>(count - 2 < 0 ? 0 : count - 2), std::floor(fp)));
      const double f = count > 1 ? fp - k : 0;
      const double x = phase_ * kFrame, jf = std::floor(x), t = x - jf;
      const int j = static_cast<int>(jf), j1 = j + 1 >= kFrame ? 0 : j + 1;
      const std::size_t row = static_cast<std::size_t>(k) * kFrame;
      const double a = static_cast<double>(frames[row + static_cast<std::size_t>(j)]) +
                       (static_cast<double>(frames[row + static_cast<std::size_t>(j1)]) - frames[row + static_cast<std::size_t>(j)]) * t;
      double w = a;
      if (f > 0) {
        const std::size_t row2 = static_cast<std::size_t>(k + 1) * kFrame;
        const double b = static_cast<double>(frames[row2 + static_cast<std::size_t>(j)]) +
                         (static_cast<double>(frames[row2 + static_cast<std::size_t>(j1)]) - frames[row2 + static_cast<std::size_t>(j)]) * t;
        w = a + (b - a) * f;
      }
      outL[i] = outR[i] = static_cast<float>(w * ampEnv_.next() * level);
      phase_ += inc;
      if (phase_ >= 1) phase_ -= 1;
    }
  }

private:
  double sampleRate_;
  Adsr ampEnv_, posEnv_;
  Frames table_, pair_;
  double phase_ = 0, pos_ = 60, velocity_ = 1, lfo_ = 0, shown_ = -1;
  int note_ = 60;
  bool lastPair_ = false;
};

}  // namespace

void registerWavetable(NodeRegistry& registry) {
  NodeType type;
  type.type = "wavetable";
  type.outputs = {"out"};
  type.kinds = {{"out", PortKind::Stereo}};
  type.params = {
    {"source", 0, 1, 0, true},
    {"position", 0, 1, 0.2},
    {"mix", 0, 1, 0},
    {"snap", 0, 1, 0, true},
    {"tune", -48, 48, 0},
    {"fine", -100, 100, 0},        // cents
    {"glide", 0, 2000, 0},
    {"vel", 0, 1, 0.5},
    {"level", 0, 1, 0.6},
    {"attack", 0.1, 5000, 3},
    {"decay", 1, 5000, 300},
    {"sustain", 0, 1, 0.8},
    {"release", 1, 5000, 250},
    {"pamt", -1, 1, 0},
    {"patk", 0.1, 5000, 5},
    {"pdec", 1, 5000, 600},
    {"psus", 0, 1, 0},
    {"prel", 1, 5000, 300},
    {"lfoRate", 0.05, 20, 2},      // Hz
    {"lfoDepth", 0, 1, 0},
    {"smooth", 0, 500, 10},        // ms
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Wavetable>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

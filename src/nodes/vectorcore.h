// Vector synthesis and wave sequencing: what the compass synth and its building blocks (waveseq, vecmix, vecpath) share: the
// diamond of four sources, the path the mix travels, the wave bank with its band-limited copies, and the lane player.
// Port of webtracker src/audio/nodes/vectorcore.js and wavebank.js.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "core/js_math.h"
#include "core/node.h"
#include "nodes/fft.h"
#include "nodes/oversample.h"
#include "nodes/wave_table_data.h"

namespace wax::vec {

constexpr int kMaxPoints = 8, kMaxSteps = 16, kMaxLen = 64;
enum Mode { Once = 0, Loop = 1, Bounce = 2 };
constexpr int kMinFadeSamples = 48;

inline double clamp(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }
inline double number(const Json& j, const char* key, double fallback) {
  if (!j.is_object() || !j.contains(key)) return fallback;
  const Json& v = j[key];
  if (!v.is_number()) return fallback;
  const double d = v.get<double>();
  return std::isfinite(d) ? d : fallback;
}

// ---------------------------------------------------------------------------------------------------------------- wave bank

constexpr int kFrame = 512;
constexpr int kHarmonics[7] = {256, 128, 64, 32, 16, 8, 4};
constexpr double kTop = 0.45;

inline int cleanWave(double index) { return std::max(0, std::min(255, jsRoundInt(std::isfinite(index) ? index : 0))); }

using WaveCopies = std::vector<std::vector<float>>;

inline const WaveCopies& waveCopies(int index) {
  static std::map<int, std::unique_ptr<WaveCopies>> made;
  const int wave = cleanWave(index);
  auto found = made.find(wave);
  if (found != made.end()) return *found->second;
  const FftPlan& plan = fftPlan(kFrame);
  const std::vector<float>& frames = bundledWaveFrames();
  std::vector<double> re(kFrame), im(kFrame, 0.0);
  for (int i = 0; i < kFrame; i++) re[static_cast<std::size_t>(i)] = frames[static_cast<std::size_t>(wave) * kFrame + static_cast<std::size_t>(i)];
  plan.run(re.data(), im.data());
  auto copies = std::make_unique<WaveCopies>();
  for (int keep : kHarmonics) {
    std::vector<double> cutRe = re, cutIm = im;
    for (int bin = keep; bin <= kFrame - keep; bin++) { cutRe[static_cast<std::size_t>(bin)] = 0; cutIm[static_cast<std::size_t>(bin)] = 0; }
    cutRe[0] = 0; cutIm[0] = 0;
    plan.run(cutRe.data(), cutIm.data(), true);
    std::vector<float> copy(kFrame + 1);
    for (int i = 0; i < kFrame; i++) copy[static_cast<std::size_t>(i)] = static_cast<float>(cutRe[static_cast<std::size_t>(i)]);
    copy[kFrame] = copy[0];
    copies->push_back(std::move(copy));
  }
  return *made.emplace(wave, std::move(copies)).first->second;
}

inline int levelFor(double inc) {
  const double allowed = kTop / std::max(1e-9, inc);
  int level = 0;
  while (level < 6 && kHarmonics[level] > allowed) level++;
  return level;
}

inline double readWave(const std::vector<float>& copy, double phase) {
  const double x = phase * kFrame;
  const int k = static_cast<int>(x);
  const double a = copy[static_cast<std::size_t>(k)], b = copy[static_cast<std::size_t>(k) + 1];
  return a + (b - a) * (x - k);
}

// ---------------------------------------------------------------------------------------------------------------- diamond

inline void insideDiamond(double x, double y, double& ox, double& oy) {
  const double reach = std::fabs(x) + std::fabs(y);
  if (reach > 1) { ox = x / reach; oy = y / reach; } else { ox = x; oy = y; }
}

// The shares of A, B, C and D at (x, y), written into `out` (4 numbers, kept as floats like the web app's Float32Array).
inline void diamondWeights(double x, double y, float* out) {
  double px, py;
  insideDiamond(x, y, px, py);
  const double p = (px + py + 1) / 2, q = (py - px + 1) / 2;
  out[0] = static_cast<float>((1 - p) * q);
  out[1] = static_cast<float>(p * q);
  out[2] = static_cast<float>(p * (1 - q));
  out[3] = static_cast<float>((1 - p) * (1 - q));
}

// ---------------------------------------------------------------------------------------------------------------- path

struct PathPoint { double x, y, d; };
using Path = std::vector<PathPoint>;

inline Path defaultPath() { return {{-1, 0, 1}, {0, 1, 1}, {1, 0, 1}, {0, -1, 1}}; }

inline Path cleanPath(const Json* raw) {
  if (!raw || !raw->is_array()) return defaultPath();
  Path points;
  for (const Json& p : *raw) {
    if (!p.is_object()) continue;
    if (static_cast<int>(points.size()) >= kMaxPoints) break;
    double x, y;
    insideDiamond(clamp(number(p, "x", 0), -1, 1), clamp(number(p, "y", 0), -1, 1), x, y);
    points.push_back({x, y, clamp(number(p, "d", 1), 0.01, 100)});
  }
  return points.empty() ? defaultPath() : points;
}

inline void pathAt(const Path& path, int mode, double time, double* out) {
  const int count = static_cast<int>(path.size());
  if (count == 1 || time <= 0) { out[0] = path[0].x; out[1] = path[0].y; return; }
  const bool closed = mode == Loop;
  double total = 0;
  for (int i = closed ? 0 : 1; i < count; i++) total += path[static_cast<std::size_t>(i)].d;
  double along;
  if (mode == Once) along = std::min(1.0, time);
  else if (mode == Loop) along = time - std::floor(time);
  else { const double turn = time - 2 * std::floor(time / 2); along = turn <= 1 ? turn : 2 - turn; }
  double left = along * total;
  const int stretches = closed ? count : count - 1;
  for (int s = 0; s < stretches; s++) {
    const PathPoint& from = path[static_cast<std::size_t>(s)];
    const PathPoint& to = path[static_cast<std::size_t>((s + 1) % count)];
    const double length = to.d;
    if (left <= length || s == stretches - 1) {
      const double t = clamp(left / length, 0, 1);
      out[0] = from.x + (to.x - from.x) * t; out[1] = from.y + (to.y - from.y) * t;
      return;
    }
    left -= length;
  }
}

// ---------------------------------------------------------------------------------------------------------------- lanes

struct Step { int w = 0; int len = 4; double fade = 0.5; int semi = 0; double lvl = 1; };
using Lane = std::vector<Step>;

inline Lane defaultLane(int lane) {
  static const int waves[4] = {176, 172, 173, 179};
  Step s;
  s.w = lane >= 0 && lane < 4 ? waves[lane] : 0;
  return {s};
}

inline Step cleanStep(const Json& raw) {
  Step s;
  s.w = cleanWave(number(raw, "w", 0));
  s.len = static_cast<int>(clamp(jsRound(number(raw, "len", 4)), 1, kMaxLen));
  s.fade = clamp(number(raw, "fade", 0.5), 0, 1);
  s.semi = static_cast<int>(clamp(jsRound(number(raw, "semi", 0)), -24, 24));
  s.lvl = clamp(number(raw, "lvl", 1), 0, 1);
  return s;
}

inline Lane cleanLane(const Json* raw, int lane) {
  if (!raw || !raw->is_array() || raw->empty()) return defaultLane(lane);
  Lane steps;
  for (const Json& s : *raw) {
    if (static_cast<int>(steps.size()) >= kMaxSteps) break;
    steps.push_back(cleanStep(s));
  }
  return steps;
}

// `count` lanes from whatever was stored.
inline std::vector<Lane> cleanLanes(const Json* raw, int count) {
  std::vector<Lane> lanes;
  for (int i = 0; i < count; i++) {
    const Json* one = raw && raw->is_array() && i < static_cast<int>(raw->size()) ? &(*raw)[static_cast<std::size_t>(i)] : nullptr;
    lanes.push_back(cleanLane(one, i));
  }
  return lanes;
}

struct WaveVoice {
  const WaveCopies* copies = &waveCopies(0);
  const std::vector<float>* copy = &(*copies)[0];
  double phase = 0, ratio = 1, level = 1, inc = 0;
  void load(const Step& step) { copies = &waveCopies(step.w); ratio = js::pow(2, step.semi / 12.0); level = step.lvl; }
  void tune(double i) { inc = std::min(0.45, i * ratio); copy = &(*copies)[static_cast<std::size_t>(levelFor(inc))]; }
  double next() {
    const double value = readWave(*copy, phase) * level;
    phase += inc;
    if (phase >= 1) phase -= 1;
    return value;
  }
};

class LanePlayer {
public:
  explicit LanePlayer(int lane = 0) {
    steps_ = defaultLane(lane);
    start();
  }

  void setSteps(const Lane& steps) {
    steps_ = steps;
    if (index_ >= static_cast<int>(steps_.size())) index_ = static_cast<int>(steps_.size()) - 1;
    now_.load(steps_[static_cast<std::size_t>(index_)]);
  }

  void start(bool keepPhase = false) {
    index_ = 0; direction_ = 1; ticks_ = 0; fadeLeft_ = 0; fadeLength_ = 1; inc_ = 0;
    if (!keepPhase) now_.phase = 0;
    now_.load(steps_[0]);
  }

  void tune(double inc) { inc_ = inc; now_.tune(inc); if (fadeLeft_ > 0) before_.tune(inc); }

  double next(int mode, double ticksPerSample) {
    ticks_ += ticksPerSample;
    if (ticks_ >= steps_[static_cast<std::size_t>(index_)].len) advance(mode, ticksPerSample);
    double value = now_.next();
    if (fadeLeft_ > 0) {
      const double old = static_cast<double>(fadeLeft_) / fadeLength_;
      value = value * (1 - old) + before_.next() * old;
      fadeLeft_--;
    }
    return value;
  }

private:
  int following(int mode) {
    const int last = static_cast<int>(steps_.size()) - 1;
    if (last == 0) return -1;
    if (mode == Loop) return index_ == last ? 0 : index_ + 1;
    if (mode == Bounce) {
      if (index_ + direction_ > last || index_ + direction_ < 0) direction_ = -direction_;
      return index_ + direction_;
    }
    return index_ == last ? -1 : index_ + 1;
  }

  void advance(int mode, double ticksPerSample) {
    const int next = following(mode);
    if (next < 0) { ticks_ = steps_[static_cast<std::size_t>(index_)].len; return; }
    ticks_ -= steps_[static_cast<std::size_t>(index_)].len;
    std::swap(before_, now_);
    index_ = next;
    const Step& step = steps_[static_cast<std::size_t>(next)];
    now_.load(step);
    now_.phase = before_.phase;
    now_.tune(inc_);
    before_.tune(inc_);
    const double stepSamples = step.len / std::max(1e-9, ticksPerSample);
    fadeLength_ = fadeLeft_ = std::max(kMinFadeSamples, jsRoundInt(step.fade * stepSamples));
  }

  WaveVoice now_, before_;
  Lane steps_;
  int index_ = 0, direction_ = 1, fadeLeft_ = 0, fadeLength_ = 1;
  double ticks_ = 0, inc_ = 0;
};

}  // namespace wax::vec

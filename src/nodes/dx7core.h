// DX7 voice data: the 32 algorithms, the packed 128-byte voice format of a 32-voice bank, a small built-in bank, and the
// level maths shared by the engine. Port of webtracker src/audio/nodes/dx7.js (the parts the engine uses).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "core/js_math.h"

namespace wax::dx7 {

constexpr int kVoices = 32;

struct Algorithm {
  std::vector<int> carriers;
  std::vector<std::array<int, 2>> mods;   // {receiver, sender}
  int fbFrom = 0, fbTo = 0;
};

inline Algorithm makeAlgorithm(std::vector<int> carriers, std::vector<std::array<int, 2>> edges, int fb, int fbTo = 0) {
  Algorithm a;
  for (int c : carriers) a.carriers.push_back(c - 1);
  for (const auto& e : edges) a.mods.push_back({e[1] - 1, e[0] - 1});
  a.fbFrom = fb - 1;
  a.fbTo = (fbTo ? fbTo : fb) - 1;
  return a;
}

inline const std::vector<Algorithm>& algorithms() {
  static const std::vector<Algorithm> list = {
    makeAlgorithm({3, 1}, {{6, 5}, {5, 4}, {4, 3}, {2, 1}}, 6),
    makeAlgorithm({3, 1}, {{6, 5}, {5, 4}, {4, 3}, {2, 1}}, 2),
    makeAlgorithm({4, 1}, {{6, 5}, {5, 4}, {3, 2}, {2, 1}}, 6),
    makeAlgorithm({4, 1}, {{6, 5}, {5, 4}, {3, 2}, {2, 1}}, 4, 6),
    makeAlgorithm({5, 3, 1}, {{6, 5}, {4, 3}, {2, 1}}, 6),
    makeAlgorithm({5, 3, 1}, {{6, 5}, {4, 3}, {2, 1}}, 5, 6),
    makeAlgorithm({3, 1}, {{6, 5}, {5, 3}, {4, 3}, {2, 1}}, 6),
    makeAlgorithm({3, 1}, {{6, 5}, {5, 3}, {4, 3}, {2, 1}}, 4),
    makeAlgorithm({3, 1}, {{6, 5}, {5, 3}, {4, 3}, {2, 1}}, 2),
    makeAlgorithm({4, 1}, {{6, 4}, {5, 4}, {3, 2}, {2, 1}}, 3),
    makeAlgorithm({4, 1}, {{6, 4}, {5, 4}, {3, 2}, {2, 1}}, 6),
    makeAlgorithm({3, 1}, {{6, 3}, {5, 3}, {4, 3}, {2, 1}}, 2),
    makeAlgorithm({3, 1}, {{6, 3}, {5, 3}, {4, 3}, {2, 1}}, 6),
    makeAlgorithm({3, 1}, {{6, 4}, {5, 4}, {4, 3}, {2, 1}}, 6),
    makeAlgorithm({3, 1}, {{6, 4}, {5, 4}, {4, 3}, {2, 1}}, 2),
    makeAlgorithm({1}, {{6, 5}, {5, 1}, {4, 3}, {3, 1}, {2, 1}}, 6),
    makeAlgorithm({1}, {{6, 5}, {5, 1}, {4, 3}, {3, 1}, {2, 1}}, 2),
    makeAlgorithm({1}, {{6, 5}, {5, 4}, {4, 1}, {3, 1}, {2, 1}}, 3),
    makeAlgorithm({5, 4, 1}, {{6, 5}, {6, 4}, {3, 2}, {2, 1}}, 6),
    makeAlgorithm({4, 2, 1}, {{6, 4}, {5, 4}, {3, 2}, {3, 1}}, 3),
    makeAlgorithm({5, 4, 2, 1}, {{6, 5}, {6, 4}, {3, 2}, {3, 1}}, 3),
    makeAlgorithm({5, 4, 3, 1}, {{6, 5}, {6, 4}, {6, 3}, {2, 1}}, 6),
    makeAlgorithm({5, 4, 2, 1}, {{6, 5}, {6, 4}, {3, 2}}, 6),
    makeAlgorithm({5, 4, 3, 2, 1}, {{6, 5}, {6, 4}, {6, 3}}, 6),
    makeAlgorithm({5, 4, 3, 2, 1}, {{6, 5}, {6, 4}}, 6),
    makeAlgorithm({4, 2, 1}, {{6, 4}, {5, 4}, {3, 2}}, 6),
    makeAlgorithm({4, 2, 1}, {{6, 4}, {5, 4}, {3, 2}}, 3),
    makeAlgorithm({6, 3, 1}, {{5, 4}, {4, 3}, {2, 1}}, 5),
    makeAlgorithm({5, 3, 2, 1}, {{6, 5}, {4, 3}}, 6),
    makeAlgorithm({6, 3, 2, 1}, {{5, 4}, {4, 3}}, 5),
    makeAlgorithm({5, 4, 3, 2, 1}, {{6, 5}}, 6),
    makeAlgorithm({6, 5, 4, 3, 2, 1}, {}, 6),
  };
  return list;
}

struct Op {
  int r[4] = {99, 99, 99, 99}, l[4] = {99, 99, 99, 0};
  int bp = 39, ld = 0, rd = 0, lc = 0, rc = 0, rs = 0, det = 7, ams = 0, kvs = 0, ol = 0, mode = 0, fc = 1, ff = 0;
};

struct Voice {
  Op ops[6];
  int pr[4] = {99, 99, 99, 99}, pl[4] = {50, 50, 50, 50};
  int alg = 0, fb = 0, oks = 1, lfs = 35, lfd = 0, lpmd = 0, lamd = 0, lks = 1, lfw = 0, pms = 3, trnsp = 24;
};

inline int clampInt(int v, int lo, int hi) { return std::min(hi, std::max(lo, v)); }

inline Voice unpackVoice(const std::vector<int>& b, std::size_t at) {
  Voice v;
  for (int i = 0; i < 6; i++) {
    const std::size_t o = at + static_cast<std::size_t>(5 - i) * 17;
    Op& p = v.ops[i];
    for (int k = 0; k < 4; k++) { p.r[k] = b[o + k] & 127; p.l[k] = b[o + 4 + k] & 127; }
    p.bp = b[o + 8] & 127; p.ld = b[o + 9] & 127; p.rd = b[o + 10] & 127; p.lc = b[o + 11] & 3; p.rc = (b[o + 11] >> 2) & 3;
    p.rs = b[o + 12] & 7; p.det = (b[o + 12] >> 3) & 15; p.ams = b[o + 13] & 3; p.kvs = (b[o + 13] >> 2) & 7;
    p.ol = b[o + 14] & 127; p.mode = b[o + 15] & 1; p.fc = (b[o + 15] >> 1) & 31; p.ff = b[o + 16] & 127;
  }
  const std::size_t g = at + 102;
  for (int k = 0; k < 4; k++) { v.pr[k] = b[g + k] & 127; v.pl[k] = b[g + 4 + k] & 127; }
  v.alg = b[g + 8] & 31; v.fb = b[g + 9] & 7; v.oks = (b[g + 9] >> 3) & 1; v.lfs = b[g + 10] & 127; v.lfd = b[g + 11] & 127;
  v.lpmd = b[g + 12] & 127; v.lamd = b[g + 13] & 127; v.lks = b[g + 14] & 1; v.lfw = (b[g + 14] >> 1) & 7; v.pms = (b[g + 14] >> 4) & 7;
  v.trnsp = std::min(48, b[g + 15] & 127);
  return v;
}

inline void packVoice(const Voice& v, std::vector<int>& out, std::size_t at) {
  for (int i = 0; i < 6; i++) {
    const Op& p = v.ops[i];
    const std::size_t o = at + static_cast<std::size_t>(5 - i) * 17;
    for (int k = 0; k < 4; k++) { out[o + k] = clampInt(p.r[k], 0, 99); out[o + 4 + k] = clampInt(p.l[k], 0, 99); }
    out[o + 8] = clampInt(p.bp, 0, 99); out[o + 9] = clampInt(p.ld, 0, 99); out[o + 10] = clampInt(p.rd, 0, 99);
    out[o + 11] = clampInt(p.lc, 0, 3) | (clampInt(p.rc, 0, 3) << 2);
    out[o + 12] = clampInt(p.rs, 0, 7) | (clampInt(p.det, 0, 14) << 3);
    out[o + 13] = clampInt(p.ams, 0, 3) | (clampInt(p.kvs, 0, 7) << 2);
    out[o + 14] = clampInt(p.ol, 0, 99);
    out[o + 15] = clampInt(p.mode, 0, 1) | (clampInt(p.fc, 0, 31) << 1);
    out[o + 16] = clampInt(p.ff, 0, 99);
  }
  const std::size_t g = at + 102;
  for (int k = 0; k < 4; k++) { out[g + k] = clampInt(v.pr[k], 0, 99); out[g + 4 + k] = clampInt(v.pl[k], 0, 99); }
  out[g + 8] = clampInt(v.alg, 0, 31); out[g + 9] = clampInt(v.fb, 0, 7) | (clampInt(v.oks, 0, 1) << 3);
  out[g + 10] = clampInt(v.lfs, 0, 99); out[g + 11] = clampInt(v.lfd, 0, 99); out[g + 12] = clampInt(v.lpmd, 0, 99); out[g + 13] = clampInt(v.lamd, 0, 99);
  out[g + 14] = clampInt(v.lks, 0, 1) | (clampInt(v.lfw, 0, 5) << 1) | (clampInt(v.pms, 0, 7) << 4);
  out[g + 15] = clampInt(v.trnsp, 0, 48);
  for (int i = 0; i < 10; i++) out[g + 16 + static_cast<std::size_t>(i)] = 32;   // (names are not used by the engine)
}

// ---- the built-in bank ----
struct OpSpec {
  double ratio;
  int ol;
  std::array<int, 4> r = {99, 99, 99, 99}, l = {99, 99, 99, 0};
  int kvs = 0, rs = 0, det = 7, ams = 0;
};

inline Op ratioOp(const OpSpec& s) {
  Op p;
  const int fc = static_cast<int>(std::floor(s.ratio));
  const int ff = fc ? jsRoundInt((s.ratio / fc - 1) * 100) : 0;
  if (s.ratio < 1) p.fc = 0; else { p.fc = fc; p.ff = ff; }
  p.ol = s.ol;
  for (int k = 0; k < 4; k++) { p.r[k] = s.r[k]; p.l[k] = s.l[k]; }
  p.kvs = s.kvs; p.rs = s.rs; p.det = s.det; p.ams = s.ams;
  return p;
}

inline Voice makeVoice(int alg, int fb, std::array<OpSpec, 6> ops, int lfs = 35, int lamd = 0) {
  Voice v;
  v.alg = alg; v.fb = fb; v.lfs = lfs; v.lamd = lamd;
  for (int i = 0; i < 6; i++) v.ops[i] = ratioOp(ops[static_cast<std::size_t>(i)]);
  return v;
}

inline Voice initVoice() {
  Voice v;
  v.ops[0].ol = 99;
  return v;
}

inline std::vector<Voice> handmade() {
  std::vector<Voice> h;
  h.push_back(initVoice());
  h.push_back(makeVoice(4, 5, {{
    {1, 99, {96, 25, 25, 67}, {99, 75, 0, 0}, 2}, {1, 58, {95, 50, 35, 78}, {99, 75, 0, 0}, 3, 2}, {1, 99, {95, 29, 20, 50}, {99, 95, 0, 0}, 2, 0, 8},
    {14, 79, {95, 50, 35, 78}, {99, 75, 0, 0}, 4}, {1, 90, {95, 29, 20, 50}, {99, 95, 0, 0}, 2, 0, 6}, {1, 56, {95, 33, 20, 50}, {99, 75, 0, 0}, 3}}}));
  h.push_back(makeVoice(0, 5, {{
    {1, 99, {99, 60, 40, 70}, {99, 90, 80, 0}}, {1, 78, {99, 70, 40, 70}, {99, 55, 0, 0}}, {0.5, 88, {99, 60, 40, 70}, {99, 90, 80, 0}},
    {1, 66, {99, 75, 40, 70}, {99, 45, 0, 0}}, {1, 0}, {1, 58, {99, 80, 40, 70}, {99, 30, 0, 0}}}}));
  h.push_back(makeVoice(4, 3, {{
    {1, 99, {99, 30, 25, 40}, {99, 85, 0, 0}}, {3.5, 72, {99, 45, 30, 45}, {99, 60, 0, 0}}, {2, 90, {99, 33, 25, 40}, {99, 80, 0, 0}},
    {5.19, 70, {99, 50, 30, 45}, {99, 55, 0, 0}}, {4, 85, {99, 36, 25, 40}, {99, 75, 0, 0}}, {7.1, 64, {99, 55, 30, 45}, {99, 50, 0, 0}}}}));
  h.push_back(makeVoice(31, 0, {{
    {0.5, 90, {90, 99, 99, 80}, {99, 99, 99, 0}}, {1, 99, {90, 99, 99, 80}, {99, 99, 99, 0}}, {2, 88, {90, 99, 99, 80}, {99, 99, 99, 0}},
    {3, 70, {90, 99, 99, 80}, {99, 99, 99, 0}}, {4, 72, {90, 99, 99, 80}, {99, 99, 99, 0}}, {6, 55, {90, 99, 99, 80}, {99, 99, 99, 0}}}}));
  h.push_back(makeVoice(4, 3, {{
    {1, 99, {99, 22, 16, 55}, {99, 82, 0, 0}, 2, 3}, {1, 74, {99, 38, 26, 60}, {99, 55, 0, 0}, 4, 2}, {2, 84, {99, 26, 18, 55}, {99, 72, 0, 0}, 2, 3, 8},
    {14, 60, {99, 75, 45, 70}, {99, 12, 0, 0}, 5}, {1, 88, {99, 20, 15, 55}, {99, 80, 0, 0}, 2, 3, 6}, {3, 66, {99, 45, 30, 60}, {99, 40, 0, 0}, 4}}}));
  h.push_back(makeVoice(4, 4, {{
    {1, 99, {99, 24, 16, 55}, {99, 78, 0, 0}, 2, 3}, {1, 85, {99, 40, 28, 60}, {99, 60, 0, 0}, 5}, {1, 92, {99, 26, 18, 55}, {99, 75, 0, 0}, 0, 3, 8},
    {7, 72, {99, 55, 35, 65}, {99, 35, 0, 0}, 5}, {1, 90, {99, 22, 15, 55}, {99, 78, 0, 0}, 0, 3, 6}, {14, 66, {99, 80, 50, 70}, {99, 10, 0, 0}, 5}}}));
  h.push_back(makeVoice(4, 1, {{
    {1, 99, {96, 20, 14, 55}, {99, 80, 0, 0}, 0, 2}, {1, 52, {96, 35, 25, 60}, {99, 40, 0, 0}, 3}, {1, 92, {96, 22, 16, 55}, {99, 75, 0, 0}, 0, 0, 8},
    {2, 50, {96, 40, 28, 60}, {99, 30, 0, 0}, 3}, {0.5, 70, {96, 18, 14, 55}, {99, 70, 0, 0}}, {1, 40, {96, 40, 28, 60}, {99, 25, 0, 0}}}}));
  h.push_back(makeVoice(4, 5, {{
    {1, 99, {99, 26, 18, 55}, {99, 78, 0, 0}, 0, 3, 12}, {1, 80, {99, 45, 30, 60}, {99, 50, 0, 0}, 4}, {1, 99, {99, 26, 18, 55}, {99, 78, 0, 0}, 0, 3, 2},
    {3, 70, {99, 50, 35, 65}, {99, 30, 0, 0}, 4}, {1, 90, {99, 28, 20, 55}, {99, 70, 0, 0}, 0, 0, 7}, {1, 76, {99, 50, 35, 60}, {99, 40, 0, 0}}}}));
  h.push_back(makeVoice(4, 4, {{
    {1, 99, {99, 40, 99, 60}, {99, 92, 90, 0}, 1}, {1, 68, {99, 55, 99, 60}, {99, 60, 55, 0}, 3}, {1, 96, {99, 40, 99, 60}, {99, 92, 90, 0}, 0, 0, 11},
    {2, 62, {99, 60, 99, 60}, {99, 50, 42, 0}, 0, 0, 3}, {2, 80, {99, 50, 30, 55}, {99, 60, 0, 0}}, {3.5, 78, {99, 70, 50, 60}, {99, 25, 0, 0}}}}));
  h.push_back(makeVoice(4, 3, {{
    {1, 99, {99, 45, 30, 60}, {99, 50, 0, 0}, 0, 3}, {1, 82, {99, 70, 50, 70}, {99, 15, 0, 0}, 4}, {2, 85, {99, 40, 28, 60}, {99, 55, 0, 0}, 0, 0, 8},
    {5, 70, {99, 75, 55, 70}, {99, 10, 0, 0}}, {4, 90, {99, 30, 20, 55}, {99, 72, 0, 0}, 0, 0, 7, 1}, {2.76, 66, {99, 60, 40, 60}, {99, 30, 0, 0}}}}, 33, 28));
  return h;
}

inline const std::vector<int>& defaultBank() {
  static const std::vector<int> bank = [] {
    std::vector<int> b(static_cast<std::size_t>(kVoices) * 128, 0);
    const std::vector<Voice> h = handmade();
    for (int i = 0; i < kVoices; i++) packVoice(static_cast<std::size_t>(i) < h.size() ? h[static_cast<std::size_t>(i)] : initVoice(), b, static_cast<std::size_t>(i) * 128);
    return b;
  }();
  return bank;
}

inline std::vector<Voice> unpackBank(const std::vector<int>& bank) {
  const std::vector<int>& bytes = bank.size() >= static_cast<std::size_t>(kVoices) * 128 ? bank : defaultBank();
  std::vector<Voice> v;
  for (int i = 0; i < kVoices; i++) v.push_back(unpackVoice(bytes, static_cast<std::size_t>(i) * 128));
  return v;
}

// ---- shared maths ----
inline int scaleOL(int v) {
  static const int low[20] = {0, 5, 9, 13, 17, 20, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 42, 43, 45, 46};
  return v < 20 ? low[v] : v + 28;
}
inline int curveScale(int group, int depth, int curve) {
  static const int expScale[33] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 16, 19, 23, 27, 33, 39, 47, 56, 66, 80, 94, 110, 126, 142, 158, 174, 190, 206, 222, 238, 250};
  int scale = curve == 0 || curve == 3 ? (group * depth * 329) >> 12 : (expScale[std::min(group, 32)] * depth * 329) >> 15;
  if (curve < 2) scale = -scale;
  return scale;
}
inline int levelScaling(int note, const Op& p) {
  const int offset = note - p.bp - 17;
  return offset >= 0 ? curveScale(offset / 3, p.rd, p.rc) : curveScale((-offset) / 3, p.ld, p.lc);
}

}  // namespace wax::dx7

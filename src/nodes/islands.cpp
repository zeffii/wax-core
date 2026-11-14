// Spectral islands: the piano roll's "add islands", as an effect. The sound is cut into long overlapping frames, and in each the
// peak inside every loud blob of the spectrum is found; a thin ridge of sound around each peak is kept and the rest is turned
// down. Pitched sound comes out cleaner and thinner. The frames are long, so this adds a lot of delay (`size`); the dry sound is
// delayed to match, so `mix` stays in time. Stereo, each side on its own.
// Port of webtracker src/audio/nodes/islands.js and src/audio/islandcore.js (the mask that decides each bin's gain).
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/stft.h"

namespace wax {
namespace {

constexpr double kRangeDb = 80, kBlob = 0.4, kMid = 0.55, kMainlobe = 1.5, kRefFall = 12;
double grow(double semis) { return js::pow(2, semis / 12) - 1; }

int islandSizeOf(double value) {
  constexpr int sizes[4] = {2048, 4096, 8192, 16384};
  return sizes[std::min(3, std::max(0, jsRoundInt(value)))];
}

struct Params { double contrast, islands, depth, smooth; };

class IslandMask {
public:
  explicit IslandMask(int n)
      : half_(n >> 1), db_(static_cast<std::size_t>(half_) + 2, 0.0), bright_(db_.size(), 0.0), col_(db_.size(), 0.0),
        prefix_(db_.size() + 1, 0.0), ridge_(db_.size(), 0.0f), gain_(db_.size(), 0.0), cands_(db_.size(), 0) {}

  double measure(const double* re, const double* im, double peak) {
    double top = -std::numeric_limits<double>::infinity();
    for (int k = 0; k <= half_; k++) {
      const double level = 20 * js::log10(jsHypot(re[k], im[k]) / peak + 1e-9);
      db_[static_cast<std::size_t>(k)] = level;
      if (level > top) top = level;
    }
    return top;
  }

  void decide(double ref, const Params& p) {
    const double smoothW = grow(0.5), reachW = grow(0.75), sigmaW = grow(0.32);
    const double k = 2 + 22 * p.contrast * p.contrast;
    auto f = [&](double v) { return 1 / (1 + js::exp(-k * (v - kMid))); };
    const double lo = f(0), span = f(1) - lo;
    const double floorDb = ref - kRangeDb;
    std::vector<double> cut(static_cast<std::size_t>(half_) + 1, 0.0);
    for (int i = 0; i <= half_; i++) {
      const std::size_t u = static_cast<std::size_t>(i);
      const double b = std::min(1.0, std::max(0.0, (db_[u] - floorDb) / kRangeDb));
      double b2 = b;
      if (p.contrast > 0) { b2 = (f(b) - lo) / span; cut[u] = std::min(0.0, (b2 - b) * kRangeDb); }
      bright_[u] = b2;
    }
    std::fill(ridge_.begin(), ridge_.begin() + half_ + 1, 0.0f);
    if (p.islands > 0) {
      prefix_[0] = 0;
      for (int i = 0; i <= half_; i++) prefix_[static_cast<std::size_t>(i) + 1] = prefix_[static_cast<std::size_t>(i)] + bright_[static_cast<std::size_t>(i)];
      for (int i = 0; i <= half_; i++) {
        const int w = static_cast<int>(std::max(1.0, jsRound(i * smoothW))), a = std::max(0, i - w), b = std::min(half_, i + w);
        col_[static_cast<std::size_t>(i)] = (prefix_[static_cast<std::size_t>(b) + 1] - prefix_[static_cast<std::size_t>(a)]) / (b - a + 1);
      }
      int count = 0;
      for (int i = 1; i < half_; i++) {
        const std::size_t u = static_cast<std::size_t>(i);
        if (col_[u] >= kBlob && col_[u] >= col_[u - 1] && col_[u] > col_[u + 1]) cands_[static_cast<std::size_t>(count++)] = i;
      }
      for (int c = 0; c < count; c++) {
        const int i = cands_[static_cast<std::size_t>(c)];
        const double ci = col_[static_cast<std::size_t>(i)];
        const int reach = static_cast<int>(std::max(2.0, jsRound(i * reachW)));
        bool top = true;
        for (int j = c - 1; j >= 0 && i - cands_[static_cast<std::size_t>(j)] <= reach; j--)
          if (col_[static_cast<std::size_t>(cands_[static_cast<std::size_t>(j)])] >= ci) { top = false; break; }
        for (int j = c + 1; top && j < count && cands_[static_cast<std::size_t>(j)] - i <= reach; j++)
          if (col_[static_cast<std::size_t>(cands_[static_cast<std::size_t>(j)])] > ci) top = false;
        if (!top) continue;
        const double sigma = std::max(kMainlobe, i * sigmaW);
        const int span2 = static_cast<int>(std::ceil(sigma * 4.5));
        for (int j = std::max(0, i - span2); j <= std::min(half_, i + span2); j++) {
          const double dj = ((j - i) / sigma);
          const double g = js::exp(-0.5 * js::pow(dj, 2));
          if (g > static_cast<double>(ridge_[static_cast<std::size_t>(j)])) ridge_[static_cast<std::size_t>(j)] = static_cast<float>(g);
        }
      }
    }
    for (int i = 0; i <= half_; i++) {
      const std::size_t u = static_cast<std::size_t>(i);
      const double target = cut[u] - p.islands * p.depth * (1 - static_cast<double>(ridge_[u]));
      gain_[u] = primed_ ? p.smooth * gain_[u] + (1 - p.smooth) * target : target;
    }
    primed_ = true;
  }

  void apply(double* re, double* im) const {
    for (int k = 0; k <= half_; k++) {
      const double g = js::pow(10, gain_[static_cast<std::size_t>(k)] / 20);
      re[k] *= g;
      im[k] *= g;
    }
  }

private:
  int half_;
  std::vector<double> db_, bright_, col_, prefix_;
  std::vector<float> ridge_;
  std::vector<double> gain_;
  std::vector<int> cands_;
  bool primed_ = false;
};

enum Param { kContrast = 0, kIslands, kDepth, kSmooth, kMix, kSize };

struct Channel {
  explicit Channel(int n) : a(n), r(n), mask(n) {}
  Analyser a;
  Resynth r;
  IslandMask mask;
  double ref = -std::numeric_limits<double>::infinity();
};

class Islands final : public Node {
public:
  explicit Islands(const NodeContext& context) : sampleRate_(context.sampleRate), re_(16384), im_(16384) {}

  void process(const float* const* inputs, float* const* outputs, int count, const float* params, const int* slots) override {
    const int size = islandSizeOf(params[slots[kSize]]);
    if (size != n_) {
      n_ = size;
      channels_[0] = std::make_unique<Channel>(n_);
      channels_[1] = std::make_unique<Channel>(n_);
    }
    const double mix = params[slots[kMix]];
    const Params p{params[slots[kContrast]], params[slots[kIslands]], params[slots[kDepth]], params[slots[kSmooth]]};
    for (int c = 0; c < 2; c++) {
      Channel& s = *channels_[static_cast<std::size_t>(c)];
      const float* x = inputs[c];
      float* y = outputs[c];
      for (int i = 0; i < count; i++) {
        if (s.a.push(x[i])) {
          s.a.fill(re_.data(), im_.data());
          const double top = s.mask.measure(re_.data(), im_.data(), stftPeakOf(n_));
          s.ref = std::max(top, s.ref - kRefFall * (n_ / 4.0) / sampleRate_);
          s.mask.decide(s.ref, p);
          s.mask.apply(re_.data(), im_.data());
          mirrorSpectrum(re_.data(), im_.data(), n_);
          s.r.add(re_.data(), im_.data(), s.a.t());
        }
        const double dry = s.a.delayed(), wet = s.r.take(s.a.t());
        y[i] = static_cast<float>(dry + (wet - dry) * mix);
      }
    }
  }

private:
  double sampleRate_;
  int n_ = 0;
  std::array<std::unique_ptr<Channel>, 2> channels_;
  std::vector<double> re_, im_;
};

}  // namespace

void registerIslands(NodeRegistry& registry) {
  NodeType type;
  type.type = "islands";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {
    {"contrast", 0, 1, 0.5},
    {"islands", 0, 1, 0.7},
    {"depth", 6, 80, 40},
    {"smooth", 0, 0.95, 0.5},
    {"mix", 0, 1, 1},
    {"size", 0, 3, 1, true},
  };
  type.latency = [](const std::map<std::string, double>& settings, double) { return static_cast<double>(islandSizeOf(settings.at("size")) - 1); };
  type.create = [](const NodeContext& context) { return std::make_unique<Islands>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

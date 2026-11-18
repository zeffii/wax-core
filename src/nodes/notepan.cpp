// Note pan: each note gets its own place in the stereo field, chosen from the notes being played, and can swing or circle round
// the listener. It sits inside the voices: every sounding note is its own copy of this node, and the copies share what is held and
// what came before (NodeContext::shared). Port of webtracker src/audio/nodes/notepan.js (without the panel's orbit reports).
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"

namespace wax {
namespace {

enum Param { kStyle = 0, kRange, kCentre, kGlide, kRate, kAmp, kShape, kSpread, kBehind };
constexpr int kStyles = 6;   // keyboard, fan, alternate, random, cluster, mirror

double fract(double v) { return v - std::floor(v); }
double hashNote(double n) { return fract(js::sin(n * 127.1 + 311.7) * 43758.5453); }

// Where a note sits before any rotation: -1 (hard left) to 1 (hard right). `held` are the notes held now, in the order played.
double panPlace(double style, int note, int seq, const std::vector<int>& held) {
  auto clampX = [](double v) { return std::max(-1.0, std::min(1.0, v)); };
  const auto found = std::find(held.begin(), held.end(), note);
  const int at = found == held.end() ? -1 : static_cast<int>(found - held.begin());
  switch (std::max(0, std::min(kStyles - 1, jsRoundInt(style)))) {
    case 1: return held.size() < 2 || at < 0 ? 0 : -1 + 2.0 * at / (static_cast<double>(held.size()) - 1);
    case 2: return seq % 2 == 0 ? -1 : 1;
    case 3: return hashNote(note) * 2 - 1;
    case 4: {
      if (held.empty()) return 0;
      double sum = 0;
      for (const int m : held) sum += m;
      return clampX((note - sum / static_cast<double>(held.size())) / 12);
    }
    case 5: return (seq % 2 ? 1 : -1) * std::max(0.15, std::min(1.0, std::abs(note - 60.0) / 30));
    default: return clampX((note - 60.0) / 30);
  }
}

// The swing or turn added to a place: phase is in cycles (unwrapped), amp 0 to 1. Radians.
double panRotation(double shape, double phase, double amp) {
  const double f = fract(phase);
  switch (jsRoundInt(shape)) {
    case 1: return amp * kPi * (f < 0.5 ? 4 * f - 1 : 3 - 4 * f);
    case 2: return 2 * kPi * f;
    default: return amp * kPi * js::sin(2 * kPi * phase);
  }
}

struct Shared {
  std::map<int, int> held;   // note -> the order it was played in
  int seq = 0;
  double phase = 0;
  // What the picture is told: one entry for each copy that has been heard from lately (a copy that stops reporting ages out).
  struct Heard { int note = -1; double angle = 0; bool held = false; int age = 0; };
  std::map<int, Heard> heard;
  int nextId = 0;
  double sinceReport = 0;
  long reports = 0;
};

class NotePan final : public Node {
public:
  explicit NotePan(const NodeContext& context) : sampleRate_(context.sampleRate), probe_(context.probe) {
    if (context.shared) {
      if (!*context.shared) *context.shared = std::make_shared<Shared>();
      shared_ = std::static_pointer_cast<Shared>(*context.shared);
    } else shared_ = std::make_shared<Shared>();
    id_ = shared_->nextId++;
  }

  void note(const NoteEvent& event) override {
    Shared& s = *shared_;
    if (event.type == NoteType::On) {
      mine_ = event.note; seq_ = s.seq++; held_ = true; started_ = false;
      s.held.erase(mine_);
      s.held[mine_] = seq_;
      phase_ = s.phase;
    } else if (event.type == NoteType::Off) {
      s.held.erase(event.note);
      if (event.note == mine_) held_ = false;
    } else {
      s.held.clear();
      held_ = false;
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    Shared& s = *shared_;
    auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const double style = p(kStyle), range = p(kRange), centre = p(kCentre), rate = p(kRate), amp = p(kAmp), shape = p(kShape), spread = p(kSpread), behind = p(kBehind);
    const double sr = sampleRate_;
    if (held_ || !started_) {
      std::vector<std::pair<int, int>> entries(s.held.begin(), s.held.end());
      std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
      std::vector<int> list;
      for (const auto& e : entries) list.push_back(e.first);
      x_ = mine_ < 0 ? 0 : panPlace(style, mine_, seq_, list);
    }
    const double target = std::max(-1.0, std::min(1.0, x_ * range + centre)) * kPi / 2;
    if (!started_) { phi0_ = target; started_ = true; }
    const double slew = 1 - js::exp(-1 / (sr * std::max(1.0, p(kGlide)) / 1000));
    const double inc = rate / sr, off = spread * fract(seq_ * 0.618034);
    const double lpLo = 1 - js::exp(-2 * kPi * 4000 / sr);
    for (int i = 0; i < n; i++) {
      phi0_ += (target - phi0_) * slew;
      const double angle = phi0_ + panRotation(shape, phase_ + off, amp);
      angle_ = angle;
      const double pan = js::sin(angle), back = std::max(0.0, -js::cos(angle));
      const double q = (pan + 1) * kPi / 4, g = (1 - 0.5 * back * behind) * kSqrt2;
      const double c = 1 - (1 - lpLo) * back * behind;
      lpL_ += (static_cast<double>(inputs[0][i]) - lpL_) * c;
      lpR_ += (static_cast<double>(inputs[1][i]) - lpR_) * c;
      outputs[0][i] = static_cast<float>(lpL_ * js::cos(q) * g);
      outputs[1][i] = static_cast<float>(lpR_ * js::sin(q) * g);
      phase_ += inc;
    }
    s.phase = phase_;
    if (probe_ && probe_->watched()) report(s, n, angle_);
  }

private:
  // The picture is told where each sounding copy is (see app/space_pictures.cpp): [report number, count, then id, note, angle, held for each].
  void report(Shared& s, int n, double angle) {
    s.heard[id_] = {mine_, angle, held_, 0};
    s.sinceReport += n;
    if (s.sinceReport < sampleRate_ * 0.06) return;
    s.sinceReport = 0;
    std::vector<float> values = {static_cast<float>(++s.reports), 0};
    for (auto it = s.heard.begin(); it != s.heard.end();) {
      if (++it->second.age > 7) { it = s.heard.erase(it); continue; }   // about 0.4 s without a word
      values.insert(values.end(), {static_cast<float>(it->first), static_cast<float>(it->second.note), static_cast<float>(it->second.angle), it->second.held ? 1.0f : 0.0f});
      ++it;
    }
    values[1] = static_cast<float>((values.size() - 2) / 4);
    probe_->publish(values, {});
  }

  double sampleRate_;
  std::shared_ptr<SpectrumProbe> probe_;
  std::shared_ptr<Shared> shared_;
  int id_ = 0;
  double angle_ = 0;
  int mine_ = -1, seq_ = 0;
  bool held_ = false, started_ = false;
  double x_ = 0, phi0_ = 0, phase_ = 0, lpL_ = 0, lpR_ = 0;
};

}  // namespace

void registerNotePan(NodeRegistry& registry) {
  NodeType type;
  type.type = "notepan";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {{"style", 0, kStyles - 1, 0, true}, {"range", 0, 1, 0.7}, {"centre", -1, 1, 0}, {"glide", 0, 1000, 60}, {"rate", -4, 4, 0},
                 {"amp", 0, 1, 0.25}, {"shape", 0, 2, 0, true}, {"spread", 0, 1, 0}, {"behind", 0, 1, 0.4}};
  type.create = [](const NodeContext& context) { return std::make_unique<NotePan>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

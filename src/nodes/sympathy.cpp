// Sympathetic strings: up to 12 tuned resonators (each a short feedback delay with a low-pass and a DC blocker in its
// loop) that ring when the input has a partial of their pitch. The chord picks the pitches, or with `follow` the strings
// are tuned to the notes the instrument plays. The dry sound stays; `mix` is the level of the strings.
// Port of webtracker src/audio/nodes/sympathy.js.
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <set>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"
#include "nodes/spacecore.h"

namespace wax {
namespace {

using space::kMaxStrings;

enum Param { kRoot = 0, kChord, kStrings, kRing, kDamp, kDetune, kWidth, kMix, kFollow, kPartials };

constexpr std::size_t kBag = 12;   // the notes remembered
constexpr double kLowest = 30;
constexpr double kReach = 36;      // semitones from the played note at which a string is as far out as `width` allows
constexpr double kDcHz = 2;
constexpr double kSqrt1_2 = 0.7071067811865476;

class Sympathy final : public Node {
public:
  explicit Sympathy(const NodeContext& context)
      : sampleRate_(context.sampleRate), probe_(context.probe), size_(static_cast<int>(std::ceil(context.sampleRate / kLowest)) + 4),
        dcR_(1 - 2 * kPi * kDcHz / context.sampleRate) {
    for (auto& b : bufs_) b.assign(static_cast<std::size_t>(size_), 0.0f);
    pitch_.fill(-1);
    for (int i = 0; i < kMaxStrings; i++) { pan_[static_cast<std::size_t>(i)] = {kSqrt1_2, kSqrt1_2}; panTo_[static_cast<std::size_t>(i)] = {kSqrt1_2, kSqrt1_2}; }
  }

  void note(const NoteEvent& event) override {
    if (event.type == NoteType::On) {
      const auto at = std::find(bag_.begin(), bag_.end(), event.note);
      if (at != bag_.end()) bag_.erase(at);
      bag_.insert(bag_.begin(), event.note);
      if (bag_.size() > kBag) bag_.pop_back();
      dirty_ = true;
    } else if (event.type == NoteType::Cut) {
      bag_.clear();
      dirty_ = true;
    }
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* inL = inputs[0];
    const float* inR = inputs[1];
    float* outL = outputs[0];
    float* outR = outputs[1];
    const auto v = [&](int p) -> double { return params[slots[p]]; };
    const bool follow = v(kFollow) >= 0.5;
    const std::array<double, 9> key = {v(kRoot), v(kChord), v(kStrings), v(kRing), v(kDamp), v(kDetune), v(kWidth), follow ? 1.0 : 0.0, v(kPartials)};
    if (!haveKey_ || key != key_ || (follow && dirty_)) {
      key_ = key; haveKey_ = true; dirty_ = false;
      const std::vector<int> list = follow ? space::wantedStrings(bag_, v(kPartials), v(kStrings)) : space::stringsOf(v(kRoot), v(kChord), v(kStrings));
      assign(list, v(kDetune), v(kRing), v(kWidth), follow && !bag_.empty() ? bag_.front() : v(kRoot));
    }
    for (std::size_t s = 0; s < kMaxStrings; s++)
      if (pitch_[s] >= 0) {   // glide, so a new note does not make the ringing strings jump
        pan_[s][0] += (panTo_[s][0] - pan_[s][0]) * 0.1;
        pan_[s][1] += (panTo_[s][1] - pan_[s][1]) * 0.1;
      }
    const double mix = v(kMix) / std::sqrt(std::max(1, active_)) * 2;
    const double c = 1 - js::exp(-2 * kPi * v(kDamp) / sampleRate_), feed = 0.15;
    const bool reporting = probe_ && probe_->watched();
    for (int i = 0; i < n; i++) {
      const double x = (static_cast<double>(inL[i]) + inR[i]) * 0.5 * feed;
      double l = 0, r = 0;
      for (std::size_t s = 0; s < kMaxStrings; s++) {
        if (pitch_[s] < 0) continue;
        std::vector<float>& b = bufs_[s];
        double rp = pos_[s] - len_[s];
        if (rp < 0) rp += size_;
        const int j = static_cast<int>(rp);
        const double f = rp - j;
        const double a = b[static_cast<std::size_t>(j)];
        const double y = a + (b[static_cast<std::size_t>(j + 1 == size_ ? 0 : j + 1)] - a) * f;
        lp_[s] += (y - lp_[s]) * c;
        // The DC blocker is inside the loop, so a constant part in the input does not pile up as an offset in the string.
        const double u = lp_[s] * fb_[s] + x, d = u - hx_[s] + dcR_ * hy_[s];
        hx_[s] = u;
        hy_[s] = std::fabs(d) < 1e-20 ? 0 : d;
        const double w = d;
        b[static_cast<std::size_t>(pos_[s])] = std::fabs(w) < 1e-20 ? 0.0f : static_cast<float>(w);
        if (++pos_[s] == size_) pos_[s] = 0;
        l += y * pan_[s][0];
        r += y * pan_[s][1];
        if (reporting) energy_[s] = std::max(energy_[s] * 0.9995, std::fabs(y));
      }
      outL[i] = static_cast<float>(inL[i] + l * mix);
      outR[i] = static_cast<float>(inR[i] + r * mix);
    }
    if (reporting) {   // the picture: for each string its pitch, how much it rings and what it sends to each side, about every 80 ms
      sent_ += n;
      if (sent_ >= sampleRate_ * 0.08) { sent_ = 0; report(); }
    }
  }

private:
  // [report number, strings, then the pitches, the energies, the left gains and the right gains of the strings that are in use]
  void report() {
    std::vector<float> notes, energy, left, right;
    for (std::size_t s = 0; s < kMaxStrings; s++)
      if (pitch_[s] >= 0) { notes.push_back(pitch_[s]); energy.push_back(static_cast<float>(energy_[s])); left.push_back(static_cast<float>(pan_[s][0])); right.push_back(static_cast<float>(pan_[s][1])); }
    std::vector<float> values = {static_cast<float>(++reports_), static_cast<float>(notes.size())};
    for (const auto* part : {&notes, &energy, &left, &right}) values.insert(values.end(), part->begin(), part->end());
    probe_->publish(values, {});
  }

  // string i gets its pitch (a little off true, more for the ones further out) and its loop gain from the ring time
  void tune(std::size_t i, double detuneParam, double ring) {
    const double detune = detuneParam * ((static_cast<int>(i) >> 1) + 1) / 3 * (i % 2 ? 1 : -1);
    const double hz = std::max(kLowest, space::midiHz(pitch_[i]) * js::pow(2, detune / 1200));
    len_[i] = std::min(static_cast<double>(size_ - 3), sampleRate_ / hz + sampleRate_ * js::atan(kDcHz / hz) / (2 * kPi * hz));
    fb_[i] = js::pow(0.001, len_[i] / (ring / 1000 * sampleRate_));
  }

  // Each string sits in the stereo field by how far its pitch is from the note that was played: the one on the note itself
  // is in the middle, the ones further away move out, taking turns left and right.
  void place(double reference, double width, const std::set<std::size_t>& jump) {
    struct Used { std::size_t i; double d; };
    std::vector<Used> used;
    for (std::size_t i = 0; i < kMaxStrings; i++) if (pitch_[i] >= 0) used.push_back({i, std::fabs(pitch_[i] - reference)});
    std::sort(used.begin(), used.end(), [](const Used& a, const Used& b) { return a.d != b.d ? a.d < b.d : a.i < b.i; });
    for (std::size_t rank = 0; rank < used.size(); rank++) {
      const std::size_t i = used[rank].i;
      const double a = std::min(1.0, used[rank].d / kReach) * width * (rank % 2 ? 1 : -1), q = (a + 1) * kPi / 4;
      panTo_[i][0] = js::cos(q);
      panTo_[i][1] = js::sin(q);
      if (jump.count(i)) { pan_[i][0] = panTo_[i][0]; pan_[i][1] = panTo_[i][1]; }   // a new string starts where it belongs
    }
  }

  // string the notes in `list`: a string already on a wanted note keeps ringing; the others are freed and given to the new notes
  void assign(const std::vector<int>& list, double detune, double ring, double width, double reference) {
    std::vector<int> want(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(list.size(), kMaxStrings)));
    std::set<std::size_t> fresh;
    for (std::size_t i = 0; i < kMaxStrings; i++)
      if (pitch_[i] >= 0 && std::find(want.begin(), want.end(), pitch_[i]) == want.end()) pitch_[i] = -1;
    for (const int note : want) {
      if (std::find(pitch_.begin(), pitch_.end(), note) != pitch_.end()) continue;
      const auto free = std::find(pitch_.begin(), pitch_.end(), -1);
      if (free == pitch_.end()) break;
      const std::size_t i = static_cast<std::size_t>(free - pitch_.begin());
      fresh.insert(i);
      pitch_[i] = static_cast<short>(note);
      std::fill(bufs_[i].begin(), bufs_[i].end(), 0.0f);
      lp_[i] = 0; hx_[i] = 0; hy_[i] = 0;
    }
    active_ = 0;
    for (std::size_t i = 0; i < kMaxStrings; i++) if (pitch_[i] >= 0) { tune(i, detune, ring); active_++; }
    place(reference, width, fresh);
  }

  double sampleRate_;
  std::shared_ptr<SpectrumProbe> probe_;
  int size_;
  double dcR_;
  std::array<double, kMaxStrings> energy_{};
  double sent_ = 0;
  long reports_ = 0;
  std::array<std::vector<float>, kMaxStrings> bufs_;
  std::array<int, kMaxStrings> pos_{};
  std::array<double, kMaxStrings> lp_{}, len_{}, fb_{}, hx_{}, hy_{};
  std::array<short, kMaxStrings> pitch_;   // the note each string is tuned to, -1 for a string not in use
  std::array<std::array<double, 2>, kMaxStrings> pan_, panTo_;
  std::vector<int> bag_;   // the notes played, newest first
  std::array<double, 9> key_{};
  bool haveKey_ = false, dirty_ = true;
  int active_ = 0;
};

}  // namespace

void registerSympathy(NodeRegistry& registry) {
  NodeType type;
  type.type = "sympathy";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {
    {"root", 24, 84, 48, true},       // MIDI note
    {"chord", 0, 7, 1, true},
    {"strings", 1, kMaxStrings, 8, true},
    {"ringtime", 100, 8000, 2500},    // ms to fall 60 dB
    {"damp", 1000, 16000, 6000},      // Hz
    {"detune", 0, 30, 4},             // cents
    {"width", 0, 1, 0.25},
    {"mix", 0, 1.5, 0.5},
    {"follow", 0, 1, 0, true},        // 1: tune the strings to the notes being played
    {"partials", 1, 6, 3, true},      // follow: how many harmonics of each note get a string
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Sympathy>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

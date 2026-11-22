// Sympathetic strings: up to 12 tuned resonators (each a short feedback delay with a low-pass and a DC blocker in its
// loop) that ring when the input has a partial of their pitch. The chord picks the pitches, or with `follow` the strings
// are tuned to the notes the instrument plays. The dry sound stays; `mix` is the level of the strings.
// Port of webtracker src/audio/nodes/sympathy.js.
//
// Layout: the strings' state is kept per quantity (all the low-pass states together, all the feedback gains together, ...) and a block is
// run string by string in three plain loops, so the arithmetic loop is one the compiler can do several strings at a time. A string that
// is not in use is all zeros (state, buffer, gains), so running it adds nothing and the loops need no test for it.
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
    store_.assign(static_cast<std::size_t>(size_) * kMaxStrings, 0.0f);
    pitch_.fill(-1);
    for (int i = 0; i < kMaxStrings; i++) { pan_[static_cast<std::size_t>(i)] = {0, 0}; panTo_[static_cast<std::size_t>(i)] = {0, 0}; }
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
    const bool reporting = probe_ && probe_->watched();
    if (dead_ && !reporting && allZero(inL, n) && allZero(inR, n)) {   // nothing rings and nothing comes in: the strings would add exactly nothing
      std::copy(inL, inL + n, outL);
      std::copy(inR, inR + n, outR);
      return;
    }
    const double mix = v(kMix) / std::sqrt(std::max(1, active_)) * 2;
    const double c = 1 - js::exp(-2 * kPi * v(kDamp) / sampleRate_), feed = 0.15;
    const std::size_t top = top_;
    for (int i = 0; i < n; i++) {
      const double x = (static_cast<double>(inL[i]) + inR[i]) * 0.5 * feed;
      dead_ = false;
      // 1. read each string's delay line, a little between two neighbouring samples
      for (std::size_t s = 0; s < top; s++) {
        int j = pos_ - back_[s];
        if (j < 0) j += size_;
        const float* line = &store_[s * static_cast<std::size_t>(size_)];
        const double a = line[j];
        y_[s] = a + (line[j + 1 == size_ ? 0 : j + 1] - a) * frac_[s];
      }
      // 2. the loop: low-pass, then the DC blocker (inside the loop, so a constant part in the input does not pile up as an offset in the string)
      for (std::size_t s = 0; s < top; s++) {
        lp_[s] += (y_[s] - lp_[s]) * c;
        const double u = lp_[s] * fb_[s] + x * fed_[s], d = u - hx_[s] + dcR_ * hy_[s];
        hx_[s] = u;
        hy_[s] = std::fabs(d) < 1e-20 ? 0 : d;
        w_[s] = d;
      }
      // 3. write the new sample into each line, and add the strings up (in string order, as always)
      double l = 0, r = 0;
      bool sounding = x != 0;
      for (std::size_t s = 0; s < top; s++) {
        const float written = std::fabs(w_[s]) < 1e-20 ? 0.0f : static_cast<float>(w_[s]);
        store_[s * static_cast<std::size_t>(size_) + static_cast<std::size_t>(pos_)] = written;
        sounding |= written != 0;
        l += y_[s] * pan_[s][0];
        r += y_[s] * pan_[s][1];
        if (reporting) energy_[s] = std::max(energy_[s] * 0.9995, std::fabs(y_[s]));
      }
      if (++pos_ == size_) pos_ = 0;
      quiet_ = sounding ? 0 : quiet_ + 1;
      outL[i] = static_cast<float>(inL[i] + l * mix);
      outR[i] = static_cast<float>(inR[i] + r * mix);
    }
    if (quiet_ > size_) settle();
    if (reporting) {   // the picture: for each string its pitch, how much it rings and what it sends to each side, about every 80 ms
      sent_ += n;
      if (sent_ >= sampleRate_ * 0.08) { sent_ = 0; report(); }
    }
  }

private:
  static bool allZero(const float* samples, int n) {
    for (int i = 0; i < n; i++) if (samples[i] != 0) return false;
    return true;
  }

  // Every line has been written with zeros since the last sound: nothing is left in the strings but the dust in their filters, which is let go.
  void settle() {
    for (std::size_t i = 0; i < kMaxStrings; i++) { lp_[i] = 0; hx_[i] = 0; hy_[i] = 0; energy_[i] = 0; }
    dead_ = true;
  }

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
    // the line is read `len` samples back: a whole number of samples and the fraction between that one and the one after it
    const double whole = std::floor(len_[i]);
    back_[i] = static_cast<int>(whole) + 1;
    frac_[i] = 1 - (len_[i] - whole);
    fed_[i] = 1;
  }

  // a string that is let go becomes silent at once, as if it were not there
  void silence(std::size_t i) {
    pitch_[i] = -1;
    std::fill_n(store_.begin() + static_cast<std::ptrdiff_t>(i * static_cast<std::size_t>(size_)), size_, 0.0f);
    lp_[i] = 0; hx_[i] = 0; hy_[i] = 0; w_[i] = 0; y_[i] = 0; fb_[i] = 0; fed_[i] = 0; frac_[i] = 0; back_[i] = 1;
    pan_[i] = {0, 0}; panTo_[i] = {0, 0};
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
      if (pitch_[i] >= 0 && std::find(want.begin(), want.end(), pitch_[i]) == want.end()) silence(i);
    for (const int note : want) {
      if (std::find(pitch_.begin(), pitch_.end(), note) != pitch_.end()) continue;
      const auto free = std::find(pitch_.begin(), pitch_.end(), -1);
      if (free == pitch_.end()) break;
      const std::size_t i = static_cast<std::size_t>(free - pitch_.begin());
      fresh.insert(i);
      silence(i);   // (start from nothing)
      pitch_[i] = static_cast<short>(note);
    }
    active_ = 0;
    top_ = 0;
    for (std::size_t i = 0; i < kMaxStrings; i++) if (pitch_[i] >= 0) { tune(i, detune, ring); active_++; top_ = i + 1; }
    place(reference, width, fresh);
  }

  double sampleRate_;
  std::shared_ptr<SpectrumProbe> probe_;
  int size_;
  double dcR_;
  std::array<double, kMaxStrings> energy_{};
  double sent_ = 0;
  long reports_ = 0;
  std::vector<float> store_;   // the delay lines one after another, `size_` samples each
  int pos_ = 0;                // where the next sample is written, the same in every line
  std::size_t top_ = 0;        // the strings from this one up are all unused
  std::array<int, kMaxStrings> back_{};
  std::array<double, kMaxStrings> lp_{}, len_{}, fb_{}, hx_{}, hy_{}, frac_{}, fed_{}, y_{}, w_{};
  std::array<short, kMaxStrings> pitch_;   // the note each string is tuned to, -1 for a string not in use
  std::array<std::array<double, 2>, kMaxStrings> pan_, panTo_;
  std::vector<int> bag_;   // the notes played, newest first
  std::array<double, 9> key_{};
  bool haveKey_ = false, dirty_ = true;
  int active_ = 0;
  int quiet_ = 0;       // samples in a row in which nothing came in and nothing was written into the lines
  bool dead_ = false;   // the strings are empty (settled): a block with no input passes straight through
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

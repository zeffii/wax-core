// Arpeggiator: sits in an instrument's graph (no wires) and changes what that instrument does with the notes it is
// given. Held keys become a pool, and the arp plays them one at a time, a step at a time, in the order the mode asks
// for. It takes the notes before the voices do (see GraphRuntime::note).
//
// Time. A step lasts `rate` ticks, so the arp follows the song's tempo. While the song plays, steps sit on the song's
// grid and the arp never drifts; while it is stopped it runs on its own clock at the same tempo. Events are made once
// per audio block, so a step can land a few ms off its ideal place. The step mask (`data.gate`, 16 values) says how loud
// each step is, and no bar is a rest. Port of webtracker src/audio/nodes/arp.js.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

enum Param {
  kMode = 0, kRate, kGate, kSwing, kOct, kTranspose, kSteps, kHold, kRetrig, kOffset,
  kRepeats, kSeed, kVelType, kLevel, kHuman, kCyc1, kCyc2, kThru
};

const char* const kModes[] = {
  "up", "down", "up/down", "down/up", "up/down+", "down/up+", "converge", "diverge", "converge/diverge",
  "as played", "random", "random other", "random once", "chord", "repeat x2", "repeat x4",
  "pinky up", "pinky up/down", "thumb up", "thumb up/down", "interval cycle",
};
constexpr int kModeCount = 21;

// A step is this many ticks long: 1 is a sixteenth with the usual 4 ticks to the beat.
const double kRates[] = {0.25, 1.0 / 3, 0.5, 2.0 / 3, 1, 1.5, 2, 3, 4, 6, 8, 16};
constexpr int kRateCount = 12;

using Notes = std::vector<int>;

// Small seeded generator, so the same seed always gives the same random arp.
class Rng {
public:
  explicit Rng(int seed = 1) : a_(static_cast<std::uint32_t>(static_cast<std::uint64_t>(seed) * 2654435761ull + 1)) {}
  double operator()() {
    a_ += 0x6d2b79f5u;
    std::uint32_t t = a_;
    t = (t ^ (t >> 15)) * (t | 1);
    t ^= t + (t ^ (t >> 7)) * (t | 61);
    return (t ^ (t >> 14)) / 4294967296.0;
  }
private:
  std::uint32_t a_;
};

Notes without(const Notes& list) { return list.size() > 2 ? Notes(list.begin() + 1, list.end() - 1) : Notes(); }   // without the two ends

Notes reversed(Notes list) { std::reverse(list.begin(), list.end()); return list; }

Notes cat(Notes a, const Notes& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

Notes converge(const Notes& up) {
  Notes out;
  const int n = static_cast<int>(up.size());
  for (int i = 0, j = n - 1; i <= j; i++, j--) { out.push_back(up[static_cast<std::size_t>(i)]); if (j != i) out.push_back(up[static_cast<std::size_t>(j)]); }
  return out;
}

// The note order for one pass of a (non-random) mode. `pool` is sorted low to high, or in played order for 'as played'.
Notes orderOf(int mode, const Notes& pool, int x, int y) {
  const int n = static_cast<int>(pool.size());
  if (!n) return {};
  const Notes& up = pool;
  const Notes down = reversed(pool);
  const std::string name = mode >= 0 && mode < kModeCount ? kModes[mode] : "";
  if (name == "up" || name == "as played" || name == "random once" || name == "chord") return up;
  if (name == "down") return down;
  if (name == "up/down") return cat(up, without(down));
  if (name == "down/up") return cat(down, without(up));
  if (name == "up/down+") return cat(up, down);
  if (name == "down/up+") return cat(down, up);
  if (name == "converge") return converge(up);
  if (name == "diverge") return reversed(converge(up));
  if (name == "converge/diverge") { const Notes c = converge(up); return cat(c, without(reversed(c))); }
  if (name == "repeat x2") { Notes o; for (int v : up) { o.push_back(v); o.push_back(v); } return o; }
  if (name == "repeat x4") { Notes o; for (int v : up) for (int i = 0; i < 4; i++) o.push_back(v); return o; }
  if (name == "pinky up") {
    if (n < 2) return up;
    Notes o;
    for (int i = 0; i < n - 1; i++) { o.push_back(up[static_cast<std::size_t>(i)]); o.push_back(up[static_cast<std::size_t>(n - 1)]); }
    return o;
  }
  if (name == "pinky up/down") {
    if (n < 2) return up;
    const int top = up[static_cast<std::size_t>(n - 1)];
    Notes o;
    for (int i = 0; i < n - 1; i++) { o.push_back(up[static_cast<std::size_t>(i)]); o.push_back(top); }
    for (int i = n - 2; i >= 1; i--) { o.push_back(up[static_cast<std::size_t>(i)]); o.push_back(top); }
    return o;
  }
  if (name == "thumb up") {
    if (n < 2) return up;
    Notes o;
    for (int i = 1; i < n; i++) { o.push_back(up[0]); o.push_back(up[static_cast<std::size_t>(i)]); }
    return o;
  }
  if (name == "thumb up/down") {
    if (n < 2) return up;
    const int low = up[0];
    Notes o;
    for (int i = 1; i < n; i++) { o.push_back(low); o.push_back(up[static_cast<std::size_t>(i)]); }
    for (int i = n - 2; i >= 1; i--) { o.push_back(low); o.push_back(up[static_cast<std::size_t>(i)]); }
    return o;
  }
  if (name == "interval cycle") { Notes o; for (int v : up) { o.push_back(v); o.push_back(v + x); o.push_back(v + y); } return o; }
  return up;
}

struct Key { int note; double vel; };
struct Sounding { int note; double off; };   // off: in samples from the start of the arp's clock

class Arp final : public Node {
public:
  explicit Arp(const NodeContext& context) : sampleRate_(context.sampleRate), clock_(context.clock), send_(context.deliver) {
    for (double& m : mask_) m = 1;
    if (context.data && context.data->contains("gate") && (*context.data)["gate"].is_array() && (*context.data)["gate"].size() == 16) {
      int i = 0;
      for (const auto& v : (*context.data)["gate"]) mask_[i++] = v.is_number() ? v.get<double>() : 1;
    }
  }

  // Called by the runtime for every note event on the instrument.
  void noteInput(const NoteEvent& event, const float* params, const int* slots) override {
    const bool hold = params && slots ? params[slots[kHold]] >= 0.5f : false;
    const bool thru = params && slots ? params[slots[kThru]] >= 0.5f : false;
    const int retrig = params && slots ? jsRoundInt(params[slots[kRetrig]]) : 1;
    if (event.type == NoteType::On) {
      if (latched_ && keys_.empty()) { pool_.clear(); latched_ = false; }   // a new chord replaces a latched one
      for (std::size_t i = 0; i < keys_.size(); i++) if (keys_[i].note == event.note) { keys_.erase(keys_.begin() + static_cast<long>(i)); break; }
      keys_.push_back({event.note, event.vel});
      lastVel_ = event.vel;
      pool_ = poolOf();
      if (retrig == 1) restart_ = true;
      if (thru) send(event);
    } else if (event.type == NoteType::Off) {
      for (std::size_t i = 0; i < keys_.size(); i++) if (keys_[i].note == event.note) { keys_.erase(keys_.begin() + static_cast<long>(i)); break; }
      if (hold) { latched_ = true; if (!keys_.empty()) pool_ = poolOf(); } else pool_ = poolOf();
      if (thru) send(event);
      if (!hold && keys_.empty()) stopAll();
    } else if (event.type == NoteType::AllOff) {
      keys_.clear(); pool_.clear(); latched_ = false; stopAll();
      if (thru) send(event);
    } else if (event.type == NoteType::Cut) {
      keys_.clear(); pool_.clear(); latched_ = false; sounding_.clear();
      send(event);
    }
  }

  void process(const float* const*, float* const*, int n, const float* params, const int* slots) override {
    auto p = [&](int i) -> double { return params[slots[i]]; };
    // turning hold off with no keys down ends the arp
    if (latched_ && p(kHold) < 0.5 && keys_.empty()) { latched_ = false; pool_.clear(); stopAll(); }
    const int seed = jsRoundInt(p(kSeed));
    if (seed != lastSeed_) reseed(seed);
    // note offs that fall due by the start of this block
    if (!sounding_.empty()) {
      std::vector<Sounding> keep;
      for (const Sounding& s : sounding_) {
        if (s.off <= clockSamples_) sendOff(s.note); else keep.push_back(s);
      }
      sounding_ = keep;
    }
    const double tickSamples = clock_ ? clock_->tickSamples : sampleRate_ * 0.125;
    const double rate = kRates[std::max(0, std::min(kRateCount - 1, jsRoundInt(p(kRate))))];
    const double stepSamples = tickSamples * rate;
    // where this block starts, in steps: on the song's grid while it plays (and back on it if the song jumped), else on the arp's own clock
    double start = cursor_;
    if (clock_ && clock_->playing) {
      const double at = clock_->songPos / rate;
      if (!started_ || std::abs(at - cursor_) > 0.5) start = at;
    }
    started_ = true;
    const double end = start + n / stepSamples;
    const double swing = (std::max(50.0, std::min(75.0, p(kSwing))) - 50) / 50;   // 0 to 0.5 of a step
    for (long m = std::max(0L, static_cast<long>(std::floor(start)) - 1); m <= static_cast<long>(std::ceil(end)) + 1; m++) {
      const double t = m + (m % 2 == 1 ? swing : 0);
      if (t >= start && t < end) fire(m, p);
    }
    cursor_ = end;
    clockSamples_ += n;
  }

private:
  Notes poolOf() const { Notes out; for (const Key& k : keys_) out.push_back(k.note); return out; }

  void send(const NoteEvent& event) { if (send_) send_(event); }
  void sendOff(int note) { NoteEvent e; e.type = NoteType::Off; e.note = note; send(e); }
  void stopAll() { for (const Sounding& s : sounding_) sendOff(s.note); sounding_.clear(); }
  void reseed(int seed) { rand_ = Rng(seed); lastSeed_ = seed; bag_.clear(); once_.clear(); onceMade_ = false; }

  void shuffle(Notes& list) {
    for (int i = static_cast<int>(list.size()) - 1; i > 0; i--) {
      const int j = static_cast<int>(std::floor(rand_() * (i + 1)));
      std::swap(list[static_cast<std::size_t>(i)], list[static_cast<std::size_t>(j)]);
    }
  }

  // One step: choose the note and start it.
  template <typename P>
  void fire(long m, P& p) {
    if (pool_.empty()) { idx_ = 0; passes_ = 0; restart_ = false; return; }
    const int steps = std::max(1, std::min(16, jsRoundInt(p(kSteps))));
    const int retrig = jsRoundInt(p(kRetrig));
    if (retrig >= 2) {
      const int every[3] = {4, 8, 16};
      if (m % every[retrig - 2] == 0) restart_ = true;
    }
    if (restart_) { idx_ = jsRoundInt(p(kOffset)); passes_ = 0; restart_ = false; bag_.clear(); once_.clear(); onceMade_ = false; reseed(jsRoundInt(p(kSeed))); }
    const int mode = jsRoundInt(p(kMode));
    const std::string name = mode >= 0 && mode < kModeCount ? kModes[mode] : "";
    const int octaves = std::max(1, std::min(4, jsRoundInt(p(kOct)))), shift = jsRoundInt(p(kTranspose));
    // the pool, over the octaves
    Notes base = pool_;
    if (name != "as played") std::sort(base.begin(), base.end());
    Notes wide;
    for (int o = 0; o < octaves; o++) for (int v : base) wide.push_back(v + 12 * o + shift);
    const int repeats = jsRoundInt(p(kRepeats));
    const double level = mask_[m % steps];
    Notes chosen;
    int length = 0;
    if (name == "random" || name == "random other") {
      length = 1;
      if (name == "random") {
        chosen = {wide[static_cast<std::size_t>(std::floor(rand_() * wide.size()))]};
      } else {
        if (bagAt_ >= static_cast<int>(bag_.size()) || bag_.size() != wide.size()) { bag_ = wide; shuffle(bag_); bagAt_ = 0; }
        chosen = {bag_[static_cast<std::size_t>(bagAt_++)]};
      }
    } else if (name == "chord") {
      chosen = wide; length = 1;
    } else {
      Notes order;
      if (name == "random once") {
        std::string sig;
        for (int v : wide) sig += std::to_string(v) + ",";
        if (!onceMade_ || sig != lastSig_) { once_ = wide; shuffle(once_); onceMade_ = true; lastSig_ = sig; }
        order = once_;
      } else {
        order = orderOf(mode, wide, jsRoundInt(p(kCyc1)), jsRoundInt(p(kCyc2)));
      }
      length = static_cast<int>(order.size());
      if (length) chosen = {order[static_cast<std::size_t>(idx_ % length)]};
      if (repeats > 0 && (length == 0 || idx_ / length >= repeats)) chosen.clear();   // the passes are used up
    }
    if (name == "random" || name == "random other" || name == "chord") {
      if (repeats > 0 && idx_ >= repeats * std::max(1, static_cast<int>(wide.size()))) chosen.clear();
    }
    idx_++;
    if (level <= 0 || chosen.empty()) return;   // a rest
    const bool fixed = jsRoundInt(p(kVelType)) == 1;
    const double gate = p(kGate), master = p(kLevel);
    const double stepSamples = (clock_ ? clock_->tickSamples : sampleRate_ * 0.125) * kRates[std::max(0, std::min(kRateCount - 1, jsRoundInt(p(kRate))))];
    for (int note : chosen) {
      double vel = fixed ? master : lastVel_ * master;
      vel *= level;
      const double human = p(kHuman);
      if (human > 0) vel *= 1 - human * 0.5 * rand_();
      const int n = std::max(0, std::min(127, note));
      // a note still sounding from the last step on the same key is ended first, so it can be heard again
      for (std::size_t i = 0; i < sounding_.size(); i++) {
        if (sounding_[i].note == n) { sendOff(n); sounding_.erase(sounding_.begin() + static_cast<long>(i)); break; }
      }
      NoteEvent on;
      on.type = NoteType::On; on.note = n; on.vel = std::max(0.01, std::min(1.0, vel));
      send(on);
      sounding_.push_back({n, clockSamples_ + std::max(64.0, stepSamples * gate)});
    }
  }

  double sampleRate_;
  const Clock* clock_;
  std::function<void(const NoteEvent&)> send_;
  double mask_[16];
  std::vector<Key> keys_;      // physically held keys
  Notes pool_;                 // what the arp plays from: the keys, or the latched ones
  bool latched_ = false;       // hold: the pool outlives the keys until a new key is pressed
  double cursor_ = 0;          // position in steps
  bool started_ = false;
  double lastVel_ = 1;         // the velocity the keys were pressed with
  int idx_ = 0;                // position in the pattern
  int passes_ = 0;
  bool restart_ = false;       // the pattern starts again at the next step
  std::vector<Sounding> sounding_;
  Notes bag_, once_;
  int bagAt_ = 0;
  bool onceMade_ = false;
  Rng rand_{1};
  int lastSeed_ = 1;
  double clockSamples_ = 0;    // counts samples, to time the note offs
  std::string lastSig_;        // the pool and mode the random-once pattern was made for
};

}  // namespace

void registerArp(NodeRegistry& registry) {
  NodeType type;
  type.type = "arp";
  type.shared = true;       // one for the instrument, never copied for each voice
  type.noteFilter = true;   // the runtime gives it the instrument's notes first, and plays what it makes
  type.params = {
    {"mode", 0, kModeCount - 1, 0, true},
    {"rate", 0, kRateCount - 1, 4, true},     // index into the step lengths, in ticks
    {"gate", 0.05, 2, 0.6},                   // note length, as a fraction of the step
    {"swing", 50, 75, 50},                    // percent, like the song's swing
    {"octaves", 1, 4, 1, true},
    {"transpose", -24, 24, 0, true},          // semitones
    {"steps", 1, 16, 16, true},               // length of the step mask
    {"hold", 0, 1, 0, true},
    {"retrigger", 0, 4, 1, true},
    {"offset", 0, 15, 0, true},
    {"repeats", 0, 16, 0, true},
    {"seed", 0, 255, 1, true},
    {"veltype", 0, 1, 0, true},
    {"level", 0, 1, 1},
    {"humanize", 0, 1, 0},
    {"cycle1", 0, 24, 4, true},
    {"cycle2", 0, 24, 7, true},
    {"thru", 0, 1, 0, true},
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Arp>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

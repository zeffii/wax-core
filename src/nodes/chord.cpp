// Chord: sits in an instrument's graph (no wires) and turns every note it is given into a chord. The chords are typed
// as names ("F# 13sus", "Dm7/C") into 256 slots, 00 to FF, and the Chd column of the pattern says which slot a note
// plays. A slot left empty plays the note as it is. It takes the notes before the voices do (see GraphRuntime::note),
// so the pattern, the keyboard and MIDI all play chords.
//
// The chord is absolute: C-4 in the pattern and F#-2 play the same notes, as typed, with the root in octave `octave`
// (Oct column). With `follow` on, the chord moves with the note instead: C4 plays it as typed, D4 a tone higher.
// `strum` spreads the notes out in time, a few ms apart, from the lowest or from the highest.
// Port of webtracker src/audio/nodes/chord.js. (Chord names typed while the song plays arrive as a node message: not ported yet.)
#include <algorithm>
#include <map>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "seq/chordname.h"

namespace wax {
namespace {

enum Param { kChord = 0, kOctave, kInversion, kNotes, kStrum, kDirection, kFollow, kLevel };

constexpr int kSlots = 256;

class Chord final : public Node {
public:
  explicit Chord(const NodeContext& context) : sampleRate_(context.sampleRate), send_(context.deliver) {
    if (context.data && context.data->contains("chords") && (*context.data)["chords"].is_object()) load((*context.data)["chords"]);
  }

  void noteInput(const NoteEvent& event, const float* params, const int* slots) override {
    const auto p = [&](Param i) { return static_cast<double>(params[slots[i]]); };
    if (event.type == NoteType::On) {
      stop(event.note);
      const auto chord = parsed_.find(std::max(0, std::min(kSlots - 1, jsRoundInt(p(kChord)))));
      std::vector<int> list;
      if (chord == parsed_.end()) {
        list = {event.note};
      } else {
        list = chordNotes(chord->second, jsRoundInt(p(kOctave)), jsRoundInt(p(kInversion)), jsRoundInt(p(kNotes)));
        if (p(kFollow) >= 0.5) {
          const int shift = event.note - 60;
          std::vector<int> moved;
          for (const int n : list) if (n + shift >= 0 && n + shift <= 127) moved.push_back(n + shift);
          list = moved;
        }
      }
      if (p(kDirection) >= 0.5) std::reverse(list.begin(), list.end());
      const long gap = static_cast<long>(jsRound(p(kStrum) * sampleRate_ / 1000));
      const double vel = std::max(0.01, std::min(1.0, event.vel * p(kLevel)));
      std::vector<int> started;
      for (std::size_t k = 0; k < list.size(); k++) {
        const int n = list[k];
        if (std::find(started.begin(), started.end(), n) != started.end()) continue;
        started.push_back(n);
        if (k == 0 || gap == 0) play(n, vel);
        else pending_.push_back({n, vel, clockSamples_ + static_cast<long>(k) * gap, event.note});
      }
      held_[event.note] = started;
    } else if (event.type == NoteType::Off) {
      stop(event.note);
    } else {   // all off, or a cut: passed on as it is
      pending_.clear();
      held_.clear();
      if (send_) send_(event);
    }
  }

  void process(const float* const*, float* const*, int n, const float*, const int*) override {
    if (!pending_.empty()) {   // the strummed notes whose time has come
      std::vector<Strummed> due, later;
      for (const Strummed& q : pending_) (q.at < clockSamples_ + n ? due : later).push_back(q);
      if (!due.empty()) {
        pending_ = later;
        for (const Strummed& q : due) play(q.note, q.vel);
      }
    }
    clockSamples_ += n;
  }

private:
  struct Strummed {
    int note;
    double vel;
    long at;      // in samples from the start of the stream
    int owner;    // the note that was played
  };

  // slot ("00" to "FF") -> chord name; only the slots with a name that can be read are kept
  void load(const Json& slots) {
    parsed_.clear();
    for (const auto& [key, name] : slots.items()) {
      if (!name.is_string()) continue;
      int slot = -1;
      try { slot = std::stoi(key, nullptr, 16); } catch (const std::exception&) { continue; }
      if (slot < 0 || slot >= kSlots) continue;
      if (const auto chord = parseChord(name.get<std::string>())) parsed_[slot] = *chord;
    }
  }

  void play(int note, double vel) {
    if (!send_) return;
    NoteEvent on;
    on.note = note;
    on.vel = vel;
    send_(on);
  }

  // Ends what an incoming note started, sounding or still waiting to be strummed.
  void stop(int owner) {
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Strummed& q) { return q.owner == owner; }), pending_.end());
    const auto found = held_.find(owner);
    if (found == held_.end()) return;
    const std::vector<int> notes = found->second;
    held_.erase(found);
    for (const int n : notes) {
      NoteEvent off;
      off.type = NoteType::Off;
      off.note = n;
      if (send_) send_(off);
    }
  }

  double sampleRate_;
  std::function<void(const NoteEvent&)> send_;
  std::map<int, ParsedChord> parsed_;
  std::map<int, std::vector<int>> held_;   // incoming note -> the notes started for it
  std::vector<Strummed> pending_;          // strummed notes not yet started
  long clockSamples_ = 0;
};

}  // namespace

void registerChord(NodeRegistry& registry) {
  NodeType type;
  type.type = "chord";
  type.shared = true;       // one for the instrument, never copied for each voice
  type.noteFilter = true;   // the runtime gives it the instrument's notes first, and plays what it makes
  type.params = {
    {"chord", 0, 255, 0, true},       // which slot plays (the Chd column)
    {"octave", 0, 15, 4, true},       // the octave of the chord's root (the Oct column); C4 is middle C
    {"inversion", 0, 6, 0, true},     // the lowest notes move up an octave, this many of them
    {"notes", 1, 8, 8, true},         // at most this many notes (the fifth goes first, then the highest)
    {"strum", 0, 120, 0},             // ms between one note and the next
    {"direction", 0, 1, 0, true},     // 0 from the lowest note, 1 from the highest
    {"follow", 0, 1, 0, true},        // 1: the chord moves with the note played (C4 plays it as typed)
    {"level", 0, 1, 1},               // scales the velocity
  };
  type.create = [](const NodeContext& context) { return std::make_unique<Chord>(context); };
  registry.add(std::move(type));
}

}  // namespace wax

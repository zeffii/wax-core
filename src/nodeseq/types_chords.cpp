// The Node Sequencer's chord nodes: Pchord, Chord, Phrase, Voicing.
// Ports of the same types in webtracker src/nodeseq/nodes.js.
#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodeseq/seq_node.h"
#include "nodeseq/seq_parse.h"

namespace wax::nodeseq {
namespace {

// Sends the notes of a chord out of a port: all together, or a strum (`gap` subticks apart), up, down, or each time
// the other way. A note with no volume is left out. Returns the new state of "alternate".
bool rollNotes(const Cx& cx, const std::string& out, const std::vector<int>& notes, const std::vector<double>& vels, int gap, int direction, bool flip, double len) {
  std::vector<std::pair<int, double>> order;
  for (std::size_t i = 0; i < notes.size(); i++) order.push_back({notes[i], i < vels.size() ? vels[i] : 0});
  if (direction == 2) flip = !flip;
  if (direction == 1 || (direction == 2 && flip)) std::reverse(order.begin(), order.end());
  int k = 0;
  for (const auto& [note, vel] : order) {
    if (!(vel > 0)) continue;
    const int at = k++ * std::max(0, gap);
    if (at > 0) cx.after(at, [cx, out, note = note, vel = vel, len] { cx.emit(out, note, vel, len); });
    else cx.emit(out, note, vel, len);
  }
  return flip;
}

// Pchord: steps through a list of chords, one a line; sends every note of the chord, optionally strummed
class PchordNode final : public SeqNode {
public:
  explicit PchordNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double, double, double) override {
    if (port == "reset") { idx_ = 0; return; }
    if (!read_) { list_ = parseChordList(textOf(cx_.data(), "text")); read_ = true; }
    if (list_.empty()) return;
    const std::optional<ParsedChord>& chord = list_[idx_];
    idx_ = (idx_ + 1) % list_.size();
    if (!chord) return;
    std::vector<int> notes = chordNotes(*chord, jsRoundInt(cx_.p("octave")), jsRoundInt(cx_.p("inversion")), jsRoundInt(cx_.p("notes")));
    if (notes.empty()) return;
    cx_.emit("root", notes[0], -1);
    const int d = jsRoundInt(cx_.p("direction"));
    if (d == 2) flip_ = !flip_;
    if (d == 1 || (d == 2 && flip_)) std::reverse(notes.begin(), notes.end());
    const int gap = jsRoundInt(cx_.p("strum"));
    const Cx cx = cx_;
    for (std::size_t k = 0; k < notes.size(); k++) {
      const int n = notes[k];
      if (gap > 0 && k > 0) cx_.after(static_cast<double>(k) * gap, [cx, n] { cx.emit("note", n, -1); });
      else cx_.emit("note", n, -1);
    }
  }
  void reset() override { idx_ = 0; flip_ = false; }

private:
  Cx cx_;
  bool read_ = false;
  std::vector<std::optional<ParsedChord>> list_;
  std::size_t idx_ = 0;
  bool flip_ = false;
};

// Chord: one chord, written as a name (A min7) or as notes (A2 C3 E3 G3, each can have a volume: A2@100 C3@60); every
// note has its own volume. A Phrase node can play it.
class ChordNode final : public SeqNode {
public:
  explicit ChordNode(Cx cx) : cx_(cx) {}

  std::optional<Pulled> get(const std::string& out) override { return out == "chord" ? sound(-1) : std::nullopt; }

  void event(const std::string&, double, double vel, double) override {
    const std::optional<Pulled> s = sound(vel);
    if (!s) return;
    cx_.emit("root", s->notes[0], s->vels[0] > 0 ? s->vels[0] : 0.01);
    flip_ = rollNotes(cx_, "notes", s->notes, s->vels, jsRoundInt(cx_.p("strum")), jsRoundInt(cx_.p("direction")), flip_, cx_.p("length"));
  }
  void reset() override { flip_ = false; }

private:
  // the notes and their volumes for one playing of the chord; the controls wired in win over the knobs
  std::optional<Pulled> sound(double vel) {
    const ChordText chord = parseChordText(textOf(cx_.data(), "text"), jsRoundInt(cx_.p("octave")), jsRoundInt(cx_.p("inversion")), jsRoundInt(cx_.p("notes")));
    if (chord.notes.empty()) return std::nullopt;
    const std::optional<Pulled> wired = cx_.pull("weights");
    const bool useWired = wired && wired->isWeights;
    std::vector<double> base;
    for (std::size_t i = 0; i < chord.notes.size(); i++) {
      if (useWired) base.push_back(i < wired->weights.size() ? wired->weights[i] : 100);
      else if (chord.inlineVolumes[i]) base.push_back(*chord.inlineVolumes[i]);
      else base.push_back(ownWeight(i));
    }
    const double lv = cx_.read("level"), va = cx_.read("variation"), ti = cx_.read("tilt");
    const double level = (std::isnan(lv) ? cx_.p("level") : lv) * (vel > 0 ? vel : 1);
    const Cx cx = cx_;
    Pulled s;
    s.notes = chord.notes;
    s.vels = voice(chord.notes.size(), base, std::isnan(ti) ? cx_.p("tilt") : ti * 2 - 1, std::isnan(va) ? cx_.p("variation") : va, level, [cx] { return cx.rand(); });
    return s;
  }

  // the volume kept in the node for a place in the chord (percent), 100 when there is none
  double ownWeight(std::size_t i) const {
    const Json& data = cx_.data();
    if (!data.is_object() || !data.contains("weights") || !data["weights"].is_array()) return 100;
    const Json& weights = data["weights"];
    return i < weights.size() && weights[i].is_number() ? weights[i].get<double>() : 100;
  }

  Cx cx_;
  bool flip_ = false;
};

// Phrase: plays one of the chords wired into it: the number arriving at index picks which (0 is the first, it wraps
// round). A number arriving at length sets how many ticks the chords last from then on (0: the note node decides).
class PhraseNode final : public SeqNode {
public:
  explicit PhraseNode(Cx cx) : cx_(cx) {}
  void event(const std::string& port, double v, double vel, double) override {
    if (port == "length") { duration_ = std::isfinite(v) && v > 0 ? v : 0; return; }
    std::vector<std::string> ports;
    for (int i = 1; i <= 16; i++) if (cx_.has("c" + std::to_string(i))) ports.push_back("c" + std::to_string(i));
    if (ports.empty() || !std::isfinite(v)) return;
    const long count = static_cast<long>(ports.size());
    const long k = ((static_cast<long>(jsRound(v)) % count) + count) % count;
    const std::optional<Pulled> got = cx_.pull(ports[static_cast<std::size_t>(k)]);
    if (!got || got->notes.empty()) return;
    const double m = cx_.p("level") * (vel > 0 ? vel : 1);
    std::vector<double> vels;
    for (const double x : got->vels) vels.push_back(std::min(1.0, x * m));
    cx_.emit("root", got->notes[0], vels[0] > 0 ? vels[0] : 0.01);
    flip_ = rollNotes(cx_, "notes", got->notes, vels, jsRoundInt(cx_.p("strum")), jsRoundInt(cx_.p("direction")), flip_, duration_ > 0 ? duration_ : cx_.p("length"));
  }
  void reset() override { flip_ = false; duration_ = 0; }

private:
  Cx cx_;
  bool flip_ = false;
  double duration_ = 0;
};

// Voicing: a row of volumes for the notes of a chord, lowest note first. Wire it into the weights port of any number
// of Chord nodes.
class VoicingNode final : public SeqNode {
public:
  explicit VoicingNode(Cx cx) : cx_(cx) {}
  std::optional<Pulled> get(const std::string& out) override {
    if (out != "weights") return std::nullopt;
    const Json& data = cx_.data();
    const bool has = data.is_object() && data.contains("weights") && data["weights"].is_array();
    Pulled p;
    p.isWeights = true;
    for (std::size_t i = 0; i < static_cast<std::size_t>(std::max(0, jsRoundInt(cx_.p("notes")))); i++) {
      p.weights.push_back(has && i < data["weights"].size() && data["weights"][i].is_number() ? data["weights"][i].get<double>() : 100);
    }
    return p;
  }

private:
  Cx cx_;
};

}  // namespace

void addChordTypes(TypeTable& types) {
  types["Pchord"] = makeType<PchordNode>({ev("next"), ev("reset")}, {ev("note"), ev("root")},
                                         {{"octave", 4}, {"inversion", 0}, {"notes", 6}, {"strum", 0}, {"direction", 0}});
  types["Chord"] = makeType<ChordNode>({ev("play"), ct("level"), ct("variation"), ct("tilt"), {"weights", PortKind::Weights}},
                                       {ev("notes"), ev("root"), {"chord", PortKind::Chord}},
                                       {{"octave", 4}, {"inversion", 0}, {"notes", 6}, {"level", 1}, {"variation", 0}, {"tilt", 0}, {"strum", 0}, {"direction", 0}, {"length", 0}});
  types["Phrase"] = makeType<PhraseNode>({ev("index"), ev("length")}, {ev("notes"), ev("root")}, {{"level", 1}, {"strum", 0}, {"direction", 0}, {"length", 0}});
  types["Phrase"].dynInPrefix = "c";
  types["Phrase"].dynInKind = PortKind::Chord;
  types["Voicing"] = makeType<VoicingNode>({}, {{"weights", PortKind::Weights}}, {{"notes", 4}});
}

}  // namespace wax::nodeseq
